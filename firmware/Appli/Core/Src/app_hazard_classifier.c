/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_hazard_classifier.c — see app_hazard_classifier.h for the design
 * rationale (integer-only, no FPU touch) and the validation note.
 *
 * Quantization scheme: standard TFLite int8 post-training quantization for
 * FullyConnected -- per layer,
 *
 *     acc[out] = bias_i32[out] + sum_k (in_i8[k] - in_zp) * w_i8[out][k]
 *     q[out]   = clamp( round(acc[out] * in_scale*w_scale/out_scale) + out_zp,
 *                        -128, 127 )
 *
 * (weight zero-point is 0 for every layer in this model -- confirmed from
 * the .tflite's tensor metadata, symmetric int8 weight quantization).
 * ReLU is fused into layers 1 and 2 with no extra clamp code needed: both
 * layers' output zero-point is -128, which is already the int8 minimum, so
 * "clamp real value >= 0" and "clamp int8 >= -128" coincide.
 *
 * The per-layer multiplier (in_scale*w_scale/out_scale) is precomputed
 * OFFLINE (Python, double precision) as a single 40-bit fixed-point integer
 * HAZ_FM{1,2,3} = round(multiplier * 2^40); at runtime, requantize() does
 * one int64 multiply + round + shift, no float. This single-step form was
 * chosen (over the textbook two-step gemmlowp Q31-multiplier-plus-shift
 * scheme) because it's what was actually cross-validated bit-exact against
 * the real interpreter on all 3721 held-out rows -- see app_hazard_
 * classifier.h's VALIDATION note. int64 headroom: worst-case |acc| here is
 * ~1.0e6 and HAZ_FM{1,2,3} are ~4e10-6e10, so acc*FM tops out around 6e16,
 * comfortably inside int64's 9.2e18 range.
 */
#include "app_hazard_classifier.h"
#include "app_hazard_classifier_weights.h"
#include <stddef.h>

/* --- Quantization parameters, tflite_learn_1115874_3.tflite (v4) ---
 * Regenerate all six of these together with app_hazard_classifier_weights.h
 * if the model is retrained; see that file's header comment. */
/* HAZ_IN_ZP/HAZ_IN_NUM/HAZ_IN_DEN and the HAZ_LOGIT_THRESH_Q* constants
 * moved to app_hazard_classifier.h (2026-09-18) so app_hazard_classifier_
 * npu.c can share them -- values unchanged. */
#define HAZ_ACT1_ZP      (-128)
#define HAZ_ACT2_ZP      (-128)
#define HAZ_LOGIT_ZP     (33)

#define HAZ_SHIFT_BITS   40
#define HAZ_FM1          39818978025LL  /* in_scale*w1_scale/act1_scale  * 2^40 */
#define HAZ_FM2          9271462265LL   /* act1_scale*w2_scale/act2_scale* 2^40 */
#define HAZ_FM3          58912131854LL  /* act2_scale*w3_scale/logit_scale*2^40 */

/* HAZ_IN_NUM/HAZ_IN_DEN (input quantization: xq = round(x/in_scale)+in_zp,
 * in_scale = 7.843137264251709 = 2000/255, computed as an exact integer
 * ratio instead of a stored float) and the HAZ_LOGIT_THRESH_Q* decision
 * threshold (sigmoid is monotonic, so prob >= p <=> logit_q >=
 * HAZ_LOGIT_THRESH_Q; p=0.20 gives recall 81.3%/precision 64.1%/FPR 13.6%,
 * the locked operating point -- missed hazard is the dangerous failure
 * mode here, false alarm is not) both now live in app_hazard_classifier.h
 * so app_hazard_classifier_npu.c can share them without drift. */

static int8_t clamp_i8(int32_t v)
{
	if (v > 127)  return 127;
	if (v < -128) return -128;
	return (int8_t)v;
}

/* Single-step fixed-point requantize: round(acc * fm / 2^HAZ_SHIFT_BITS),
 * round-half-away-from-zero, int64 throughout. */
static int32_t requantize(int64_t acc, int64_t fm)
{
	int64_t num = acc * fm;
	int64_t half = (int64_t)1 << (HAZ_SHIFT_BITS - 1);
	int64_t result;

	if (num >= 0)
		result = (num + half) >> HAZ_SHIFT_BITS;
	else
		result = -(((-num) + half) >> HAZ_SHIFT_BITS);
	return (int32_t)result;
}

static void fc_layer(const int8_t *in, int32_t in_zp,
                      const int8_t *w, const int32_t *bias,
                      int32_t out_dim, int32_t in_dim,
                      int64_t fm, int32_t out_zp,
                      int8_t *out)
{
	int32_t o, k;

	for (o = 0; o < out_dim; o++) {
		int64_t acc = bias[o];
		const int8_t *wrow = w + (size_t)o * (size_t)in_dim;

		for (k = 0; k < in_dim; k++)
			acc += (int64_t)(in[k] - in_zp) * (int64_t)wrow[k];

		out[o] = clamp_i8(requantize(acc, fm) + out_zp);
	}
}

uint8_t hazard_classify(const int32_t norm_feat[HAZ_FEAT_COUNT])
{
	int8_t xq[HAZ_FEAT_COUNT];
	int8_t act1[32];
	int8_t act2[16];
	int8_t logit_q[1];
	int32_t i;

	for (i = 0; i < HAZ_FEAT_COUNT; i++) {
		int32_t x = norm_feat[i];
		int64_t num = (int64_t)x * HAZ_IN_NUM;
		int32_t q;

		if (num >= 0)
			q = (int32_t)((num + HAZ_IN_DEN / 2) / HAZ_IN_DEN);
		else
			q = -(int32_t)((-num + HAZ_IN_DEN / 2) / HAZ_IN_DEN);
		q += HAZ_IN_ZP;
		xq[i] = clamp_i8(q);
	}

	fc_layer(xq, HAZ_IN_ZP, HAZ_W1, HAZ_B1, 32, 17, HAZ_FM1, HAZ_ACT1_ZP, act1);
	fc_layer(act1, HAZ_ACT1_ZP, HAZ_W2, HAZ_B2, 16, 32, HAZ_FM2, HAZ_ACT2_ZP, act2);
	fc_layer(act2, HAZ_ACT2_ZP, HAZ_W3, HAZ_B3, 1, 16, HAZ_FM3, HAZ_LOGIT_ZP, logit_q);

	return (logit_q[0] >= HAZ_LOGIT_THRESH_Q) ? 1u : 0u;
}
