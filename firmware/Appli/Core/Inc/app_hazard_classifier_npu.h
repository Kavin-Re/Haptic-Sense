/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_hazard_classifier_npu.h -- NPU (ST Neural-ART) path for the SAME
 * hazard-classifier model app_hazard_classifier.c runs on the CPU, this
 * time run on the STM32N6's NPU via ST Edge AI Core / atonn-generated
 * code (Model/STM32N6570-DK/network.c). Same model, same 17 pre-normalized
 * features, same input convention as app_hazard_classifier.h -- read that
 * file first, this one only covers what's different about the NPU path.
 *
 * PROVENANCE: network.c/.h/_ecblobs.h were generated 2026-09-18 by
 * ST Edge AI Developer Cloud (ST Edge AI Core v4.0.1, atonn v1.1.3-275)
 * from the exact same haptic_sense_hazard_v4.tflite app_hazard_classifier.c
 * was validated bit-exact against. That generated code was then compiled,
 * flashed to a REAL STM32N6570-DK over ST's cloud board farm, and run
 * on-target via `stedgeai validate --mode target`: profiler reported
 * HW: 85.6%% / SW: 0.0%% (zero operators fell back to CPU -- the whole
 * model maps to the NPU as a single epoch-controller blob) and the
 * cross-accuracy report against the reference TFLite output was bit-exact
 * (rmse=mae=l2r=0, cos=1.0). Full detail:
 * claude/BLOCK6_TO_SUBMISSION_STATUS_20260916.md, section 2 item 4.
 *
 * The Lib/AI_Runtime/Npu/{ll_aton,Devices/STM32N6XX} runtime files in this
 * repo were replaced wholesale with the matching v1.1.3-275 versions from
 * that same generate step -- network.c itself #errors at compile time if
 * the linked ll_aton_version.h doesn't match exactly (see network.c's own
 * version check), so this isn't optional; the two must be regenerated and
 * updated together if the model is ever retrained.
 *
 * WHAT THIS MODULE DOES: calls the low-level LL_ATON_RT_* API this
 * project's own utils.c:Run_Inference() already uses (the pattern the
 * project's STM32CubeIDE/X-CUBE-AI scaffolding was originally set up
 * with) -- NOT the newer higher-level stai_network.c/.h wrapper, which
 * would need additional ll_aton_stai_internal.c-based plumbing this
 * project doesn't otherwise use. network.c's own
 * LL_ATON_Input/Output_Buffers_Info_network() report the model's I/O as
 * NOT user-allocated: both the 17-byte input and the 1-byte output live
 * at a fixed NPU-RAM address (0x342e0000, npuRAM5) that network.c's own
 * generated epoch-controller blob owns outright -- input and output
 * happen to alias the SAME address (the epoch overwrites the input bytes
 * with the output byte once it's done), which is expected, not a bug.
 * This module gets that address via LL_Buffer_addr_start() rather than
 * hardcoding it, so a model regenerate that moves the buffer doesn't
 * silently break this code.
 *
 * CACHE MAINTENANCE: this project does not define USE_DCACHE (see
 * Appli/Core/Src/main.c), so the Cortex-M55 D-cache is never enabled and
 * no cache clean/invalidate is needed around the plain stores/loads this
 * module does to that NPU-RAM address -- see the note in the .c file if
 * that ever changes.
 *
 * STATUS -- READ BEFORE FLIPPING THIS ON: the cloud run above validated
 * network.c's generated code IN ISOLATION on real hardware. This specific
 * integration (this file, wired into inference_task_fct) has NOT been
 * hardware-tested -- unlike app_hazard_classifier.c's CPU path, which is
 * both bit-exact validated AND has been the thing actually running in
 * every build so far. Build with HAZARD_CLASSIFIER_USE_NPU defined to
 * make inference_task call this path instead; leave it undefined
 * (default) to keep shipping the validated CPU path. Before trusting the
 * NPU path for real: flash a build with it enabled, feed known inputs
 * (e.g. via a debug print of norm_feat[] and the returned hazard bit),
 * and confirm hazard_classify_npu() agrees with hazard_classify() on the
 * same inputs before relying on it for the submission.
 */
#ifndef APP_HAZARD_CLASSIFIER_NPU_H
#define APP_HAZARD_CLASSIFIER_NPU_H

#include <stdint.h>
#include "app_hazard_classifier.h"   /* HAZ_FEAT_COUNT + shared quant/threshold constants */

/* Call once at startup (app_tasks_run(), before tasks start), not from an
 * ISR. Matches app.c's nn_thread_fct one-time LL_ATON_RT_RuntimeInit()/
 * LL_ATON_RT_Init_Network() pattern (that file is excluded from this
 * build, but is the project's own precedent for this exact call pair). */
void hazard_npu_init(void);

/* Same contract as hazard_classify(): norm_feat is the SAME pre-normalized
 * (norm1000()) 17-feature array in the same order. Returns 1 if the NPU
 * model's hazard probability is >= the locked decision threshold, 0
 * otherwise. Must not be called before hazard_npu_init(). */
/* out_raw_logit (optional, may be NULL): if non-NULL, set to the raw
 * int8 output byte read from NPU-RAM BEFORE the returned 0/1 threshold
 * decision is applied -- lets a caller (HAZARD_NPU_DIAG) log the actual
 * logit, not just the thresholded bit. Added 2026-09-18 bring-up. */
/* 2026-09-18 round 2: the round-1 fix (read out_buf[0] BEFORE
 * LL_ATON_RT_Reset_Network() instead of after, via Run_Inference())
 * did NOT change anything -- a second hardware capture still showed
 * raw=127 (INT8_MAX) on 329/329 frames, completely flat regardless of
 * scene. That rules out the reset-ordering theory. This diag struct
 * captures three snapshots of the SAME byte address (in_buf/out_buf
 * are documented to alias) plus the two buffer addresses themselves,
 * so the caller can tell apart: (a) the address never changes at all
 * across a whole run (dead/wrong address), (b) our own quantization
 * write doesn't stick (aliasing claim false), or (c) RunEpochBlock
 * genuinely overwrites it with a real, but always-saturated, value
 * (points at the model/quantization instead of the plumbing). */
typedef struct {
	int8_t raw_pre;         /* out_buf[0] on entry, before this call writes anything */
	int8_t raw_post_quant;  /* out_buf[0] (== in_buf[0] if truly aliased) right after
	                         * quantizing feature 0, before RunEpochBlock */
	int8_t raw_post_epoch;  /* out_buf[0] right after the epoch loop finishes, before
	                         * LL_ATON_RT_Reset_Network() -- same as round 1's raw_logit */
	uint32_t in_addr;       /* LL_Buffer_addr_start(&in_info[0]) */
	uint32_t out_addr;      /* LL_Buffer_addr_start(&out_info[0]) */
} hazard_npu_diag_t;

/* diag (optional, may be NULL): filled in with the snapshots above --
 * see hazard_npu_diag_t. HAZARD_NPU_DIAG bring-up only. */
uint8_t hazard_classify_npu(const int32_t norm_feat[HAZ_FEAT_COUNT], hazard_npu_diag_t *diag);

#endif /* APP_HAZARD_CLASSIFIER_NPU_H */
