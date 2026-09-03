/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * velocity_lsq.c — see velocity_lsq.h for the design citation and the
 * integer-only rationale (app_vl53l1x.c:263-273, FPU/CONTROL.FPCA).
 */
#include "velocity_lsq.h"

/*
 * Ordinary least-squares slope of y over t, for n (t, y) pairs, computed
 * as the exact rational number num/den (not divided here, so callers can
 * fold in a final unit-conversion scale and round exactly once instead of
 * rounding twice).
 *
 * t[] is normalised in-place-conceptually (not mutated -- computed off a
 * local copy) by subtracting its minimum before summing. This is NOT
 * optional precision polish: t is a raw millisecond uptime counter that
 * grows without bound (tk_get_otm().lo), so on a long-running board
 * n*sum(t^2) and (sum t)^2 are both huge and nearly equal -- computing
 * their difference directly loses essentially all significant digits
 * (catastrophic cancellation). Subtracting the window's own minimum first
 * keeps every t value in the tens-to-hundreds-of-ms range for the whole
 * life of the board, where this problem cannot occur.
 *
 * den = n*sum(t^2) - (sum t)^2 is mathematically n times the variance of
 * t, so it is always >= 0, and is exactly 0 only when every t in the
 * window is identical (a degenerate window, or n < 2).
 */
static void vlsq_core(const int32_t *y, const uint32_t *t, int n,
                       int64_t *num_out, int64_t *den_out)
{
	uint32_t t_min;
	int64_t sum_t = 0, sum_y = 0, sum_ty = 0, sum_tt = 0;
	int64_t tn;
	int i;

	*num_out = 0;
	*den_out = 0;
	if (n < 2)
		return;

	t_min = t[0];
	for (i = 1; i < n; i++)
		if (t[i] < t_min)
			t_min = t[i];

	for (i = 0; i < n; i++) {
		tn = (int64_t)(t[i] - t_min);	/* UW wrap-safe subtraction first */
		sum_t  += tn;
		sum_y  += (int64_t)y[i];
		sum_ty += tn * (int64_t)y[i];
		sum_tt += tn * tn;
	}

	*num_out = (int64_t)n * sum_ty - sum_t * sum_y;
	*den_out = (int64_t)n * sum_tt - sum_t * sum_t;	/* always >= 0 */
}

/*
 * Round num/den to the nearest integer. den must be >= 0 (vlsq_core's
 * contract); num may be either sign. Ties round away from zero.
 */
static int32_t vlsq_rdiv(int64_t num, int64_t den)
{
	if (den <= 0)
		return 0;
	if (num >= 0)
		return (int32_t)((num + den / 2) / den);
	return (int32_t)(-((-num + den / 2) / den));
}

int32_t vlsq_velocity_cm_s(const int32_t *d_mm, const uint32_t *t_ms, int n)
{
	int64_t num, den;

	if (n > VLSQ_WINDOW)
		n = VLSQ_WINDOW;
	vlsq_core(d_mm, t_ms, n, &num, &den);
	if (den == 0)
		return 0;

	/* slope = num/den is d(distance)/d(time) in mm/ms, positive when
	 * distance is INCREASING with time (retreating). The design's sign
	 * convention is the opposite (positive = closing), so negate; and
	 * mm/ms -> cm/s is *100 (1000 ms->s, /10 mm->cm). Both folded into
	 * one rounded division so there is only one rounding step. */
	return vlsq_rdiv(num * -100, den);
}

int32_t vlsq_accel_cm_s2(const int32_t *v_cm_s, const uint32_t *t_ms, int n)
{
	int64_t num, den;

	if (n > VLSQ_WINDOW)
		n = VLSQ_WINDOW;
	vlsq_core(v_cm_s, t_ms, n, &num, &den);
	if (den == 0)
		return 0;

	/* slope = num/den is d(v)/d(time) in (cm/s)/ms, and positive already
	 * means closing speed increasing -- no sign flip needed here, unlike
	 * velocity above. (cm/s)/ms -> cm/s^2 is *1000 (ms -> s). */
	return vlsq_rdiv(num * 1000, den);
}
