/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_hazard_classifier_npu.c -- see app_hazard_classifier_npu.h for the
 * design rationale, provenance, and the "not hardware-tested yet" status
 * note. hazard_classify_npu() runs its own inline copy of the epoch loop
 * (LL_ATON_RT_RunEpochBlock()/LL_ATON_OSAL_WFE(), matching the project's
 * own utils.c:Run_Inference() pattern) instead of calling Run_Inference()
 * itself -- see the 2026-09-18 comment at the call site below for why:
 * this model's output buffer aliases its input buffer, and the order of
 * "read the output" vs. "reset the network" matters.
 */
#include "app_hazard_classifier_npu.h"

#include <stdint.h>

#include "ll_aton_rt_user_api.h"
#include "utils.h"     /* Run_Inference() */
#include "network.h"   /* LL_ATON_NETWORK_IN_1_SIZE_BYTES / OUT_1_SIZE_BYTES */

/* Wires up NN_Instance_network / a matching NN_Interface_network to the
 * `_network`-suffixed functions network.c defines (LL_ATON_EpochBlockItems_
 * network, LL_ATON_Input_Buffers_Info_network, etc. -- "network" is this
 * model's --network-name, see network.c's header comment). Both symbols
 * are file-local (declared `static` by this macro); nothing outside this
 * file touches them directly. */
LL_ATON_DECLARE_NAMED_NN_INSTANCE_AND_INTERFACE(network);

/* Catches a retrained/regenerated model whose I/O shape no longer matches
 * what this file assumes, at compile time rather than as a silent
 * out-of-bounds write/read on the NPU-RAM buffer. */
_Static_assert(LL_ATON_NETWORK_IN_1_SIZE_BYTES == HAZ_FEAT_COUNT,
               "network.c input size no longer matches HAZ_FEAT_COUNT -- "
               "model and feature count have drifted apart");
_Static_assert(LL_ATON_NETWORK_OUT_1_SIZE_BYTES == 1,
               "network.c output size is no longer the single logit byte "
               "this code assumes");

static int s_npu_ready;

void hazard_npu_init(void)
{
	LL_ATON_RT_RuntimeInit();
	LL_ATON_RT_Init_Network(&NN_Instance_network);
	s_npu_ready = 1;
}

uint8_t hazard_classify_npu(const int32_t norm_feat[HAZ_FEAT_COUNT], hazard_npu_diag_t *diag)
{
	const LL_Buffer_InfoTypeDef *in_info  = LL_ATON_Input_Buffers_Info(&NN_Instance_network);
	const LL_Buffer_InfoTypeDef *out_info = LL_ATON_Output_Buffers_Info(&NN_Instance_network);
	/* Per network.c's LL_ATON_Input_Buffers_Info_network()/LL_ATON_Output_
	 * Buffers_Info_network(): both buffers are compiler-placed (not user-
	 * allocated) in the same 32-byte npuRAM5 slot (0x342e0000) the epoch-
	 * controller blob owns -- input and output ALIAS the same address
	 * (the epoch overwrites the input bytes with the 1-byte output once
	 * it's done). That's expected for this model, not a bug; in_buf and
	 * out_buf below will compare equal. */
	int8_t *in_buf  = (int8_t *)LL_Buffer_addr_start(&in_info[0]);
	int8_t *out_buf = (int8_t *)LL_Buffer_addr_start(&out_info[0]);
	int32_t i;

	if (diag) {
		diag->in_addr  = (uint32_t)(uintptr_t)in_buf;
		diag->out_addr = (uint32_t)(uintptr_t)out_buf;
		diag->raw_pre  = out_buf[0];	/* snapshot BEFORE this call touches anything */
	}

	if (!s_npu_ready) {
		if (diag) {
			diag->raw_post_quant = 0;
			diag->raw_post_epoch = 0;
		}
		return 0u;	/* hazard_npu_init() wasn't called -- don't touch NPU-owned memory */
	}

	/* Quantize input: bit-for-bit the SAME formula as app_hazard_
	 * classifier.c's hazard_classify() (HAZ_IN_NUM/HAZ_IN_DEN/HAZ_IN_ZP,
	 * exact integer ratio, round-half-away-from-zero, now shared via
	 * app_hazard_classifier.h) -- must stay identical, or the NPU and CPU
	 * paths would silently diverge on the same reading. */
	for (i = 0; i < HAZ_FEAT_COUNT; i++) {
		int32_t x = norm_feat[i];
		int64_t num = (int64_t)x * HAZ_IN_NUM;
		int32_t q;

		if (num >= 0)
			q = (int32_t)((num + HAZ_IN_DEN / 2) / HAZ_IN_DEN);
		else
			q = -(int32_t)((-num + HAZ_IN_DEN / 2) / HAZ_IN_DEN);
		q += HAZ_IN_ZP;
		if (q > 127)  q = 127;
		if (q < -128) q = -128;
		in_buf[i] = (int8_t)q;
	}

	if (diag)
		diag->raw_post_quant = out_buf[0];	/* == in_buf[0] if the aliasing claim holds */

	/* This project does not define USE_DCACHE (Appli/Core/Src/main.c),
	 * so the Cortex-M55 D-cache is never enabled and the plain stores
	 * above / load below need no cache maintenance. If that ever
	 * changes: clean in_buf[0..HAZ_FEAT_COUNT) here before Run_Inference()
	 * and invalidate out_buf[0..1) after it, mirroring app.c's (excluded,
	 * but original-author) CACHE_OP(SCB_...DCache_by_Addr(...)) calls
	 * around its own NN buffer accesses. */
	/* Deliberately NOT calling the shared utils.c:Run_Inference() here.
	 * That helper's LAST step is LL_ATON_RT_Reset_Network() -- and this
	 * model's output buffer ALIASES the input buffer address (see this
	 * file's header). 2026-09-18 HAZARD_NPU_DIAG hardware bring-up showed
	 * the NPU decision pinned at hazard=1 on ~100% of frames regardless
	 * of scene (477/477 samples, see
	 * claude/BLOCK6_TO_SUBMISSION_STATUS_20260916.md) -- the prime suspect
	 * is that Reset_Network() re-arms/clears that shared NPU-RAM slot
	 * before the caller ever reads out_buf[0], so the value being read was
	 * a post-reset artifact, not the real inference result. Fix: run the
	 * identical epoch loop inline, read the output byte FIRST, reset
	 * SECOND. diag (if non-NULL) lets the caller log the actual byte (not
	 * just the thresholded bit) to confirm this before/after a fix.
	 *
	 * ROUND 2 (still 2026-09-18): this fix alone did NOT help -- a second
	 * capture showed raw=127 (INT8_MAX) on 329/329 frames, still flat
	 * regardless of scene. diag now also captures raw_pre (before this
	 * call touches anything) and raw_post_quant (right after writing
	 * in_buf[0], before RunEpochBlock) so the three snapshots together
	 * can show whether the address ever changes at all. */
	{
		LL_ATON_RT_RetValues_t ll_aton_rt_ret;
		int8_t raw_logit;

		do {
			ll_aton_rt_ret = LL_ATON_RT_RunEpochBlock(&NN_Instance_network);
			if (ll_aton_rt_ret == LL_ATON_RT_WFE)
				LL_ATON_OSAL_WFE();
		} while (ll_aton_rt_ret != LL_ATON_RT_DONE);

		raw_logit = out_buf[0];	/* read BEFORE reset -- see comment above */
		if (diag)
			diag->raw_post_epoch = raw_logit;

		LL_ATON_RT_Reset_Network(&NN_Instance_network);

		return (raw_logit >= HAZ_PROB_THRESH_Q) ? 1u : 0u;	/* PROB, not LOGIT -- see app_hazard_classifier.h */
	}
}
