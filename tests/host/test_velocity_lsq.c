/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for firmware/Appli/Core/Src/velocity_lsq.c -- the
 * integer least-squares slope estimator behind the closing-velocity and
 * closing-acceleration features. The firmware source is compiled as-is.
 *
 * Conventions under test (velocity_lsq.h): index 0 is the NEWEST sample,
 * positive velocity means the target is CLOSING (distance decreasing).
 */
#include <stdint.h>

#include "minunit.h"
#include "velocity_lsq.h"

/* Fill a newest-first window: d(t) = d_newest + step_mm * age, one sample
 * every period_ms, newest sample stamped t_newest. */
static void fill(int32_t *d, uint32_t *t, int n, int32_t d_newest,
		 int32_t step_mm_per_frame_older, uint32_t t_newest,
		 uint32_t period_ms)
{
	int i;

	for (i = 0; i < n; i++) {
		d[i] = d_newest + step_mm_per_frame_older * i;
		t[i] = t_newest - period_ms * (uint32_t)i;
	}
}

static void test_too_few_samples_returns_zero(void)
{
	int32_t d[VLSQ_WINDOW] = { 500, 600, 700, 800, 900 };
	uint32_t t[VLSQ_WINDOW] = { 80, 60, 40, 20, 0 };

	CHECK_EQ(vlsq_velocity_cm_s(d, t, 0), 0);
	CHECK_EQ(vlsq_velocity_cm_s(d, t, 1), 0);
	CHECK_EQ(vlsq_velocity_cm_s(d, t, -3), 0);	/* negative n is not read */
	CHECK_EQ(vlsq_accel_cm_s2(d, t, 0), 0);
	CHECK_EQ(vlsq_accel_cm_s2(d, t, 1), 0);
}

static void test_degenerate_timestamps_do_not_divide_by_zero(void)
{
	int32_t d[VLSQ_WINDOW] = { 100, 200, 300, 400, 500 };
	uint32_t t[VLSQ_WINDOW] = { 1234, 1234, 1234, 1234, 1234 };

	CHECK_EQ(vlsq_velocity_cm_s(d, t, VLSQ_WINDOW), 0);
	CHECK_EQ(vlsq_accel_cm_s2(d, t, VLSQ_WINDOW), 0);
	CHECK_EQ(vlsq_velocity_cm_s(d, t, 2), 0);
}

static void test_closing_is_positive_retreating_is_negative(void)
{
	int32_t d[VLSQ_WINDOW];
	uint32_t t[VLSQ_WINDOW];

	/* 10 mm closer every 20 ms = 0.5 mm/ms = 50 cm/s closing. This is
	 * the firmware's own synthetic scenario (SYN_STEP_MM in app_tasks.c). */
	fill(d, t, VLSQ_WINDOW, 500, +10, 1000, 20);
	CHECK_EQ(vlsq_velocity_cm_s(d, t, VLSQ_WINDOW), 50);

	fill(d, t, VLSQ_WINDOW, 500, -10, 1000, 20);
	CHECK_EQ(vlsq_velocity_cm_s(d, t, VLSQ_WINDOW), -50);

	/* Stationary target. */
	fill(d, t, VLSQ_WINDOW, 500, 0, 1000, 20);
	CHECK_EQ(vlsq_velocity_cm_s(d, t, VLSQ_WINDOW), 0);
}

static void test_two_point_window(void)
{
	int32_t d[2] = { 480, 500 };	/* newest first: 20 mm closer ... */
	uint32_t t[2] = { 1040, 1000 };	/* ... in 40 ms = 50 cm/s        */

	CHECK_EQ(vlsq_velocity_cm_s(d, t, 2), 50);
}

static void test_uses_real_timestamps_not_assumed_period(void)
{
	int32_t d[VLSQ_WINDOW];
	uint32_t t[VLSQ_WINDOW];

	/* Same 10 mm/frame, but frames 37 ms apart (the measured full-
	 * pipeline period): 10/37 mm/ms = 27.03 cm/s -> 27. */
	fill(d, t, VLSQ_WINDOW, 500, +10, 5000, 37);
	CHECK_EQ(vlsq_velocity_cm_s(d, t, VLSQ_WINDOW), 27);

	/* Irregular spacing on an exact line d = 2000 - t/2 (0.5 mm/ms). */
	t[0] = 400; t[1] = 370; t[2] = 300; t[3] = 280; t[4] = 200;
	d[0] = 1800; d[1] = 1815; d[2] = 1850; d[3] = 1860; d[4] = 1900;
	CHECK_EQ(vlsq_velocity_cm_s(d, t, VLSQ_WINDOW), 50);
}

static void test_rounding_is_to_nearest_ties_away_from_zero(void)
{
	/* slope -1 mm / 200 ms = -0.005 mm/ms -> +0.5 cm/s closing -> 1 */
	int32_t d_close[2] = { 999, 1000 };
	int32_t d_away[2]  = { 1001, 1000 };
	uint32_t t[2] = { 200, 0 };
	/* -1 mm / 250 ms -> 0.4 cm/s -> 0 */
	uint32_t t_slow[2] = { 250, 0 };

	CHECK_EQ(vlsq_velocity_cm_s(d_close, t, 2), 1);
	CHECK_EQ(vlsq_velocity_cm_s(d_away, t, 2), -1);
	CHECK_EQ(vlsq_velocity_cm_s(d_close, t_slow, 2), 0);
	CHECK_EQ(vlsq_velocity_cm_s(d_away, t_slow, 2), 0);
}

static void test_noise_is_averaged_not_amplified(void)
{
	/* The design note in velocity_lsq.h: a +-3 mm frame error turns into
	 * +-15 cm/s with a two-point difference at 20 ms. Over the 5-sample
	 * window a single +3 mm outlier on the newest sample of a stationary
	 * target must stay well under the 20 cm/s hazard threshold. */
	int32_t d[VLSQ_WINDOW] = { 503, 500, 500, 500, 500 };
	uint32_t t[VLSQ_WINDOW] = { 1080, 1060, 1040, 1020, 1000 };
	int32_t v = vlsq_velocity_cm_s(d, t, VLSQ_WINDOW);

	CHECK_EQ(v, -3);	/* slope = 3*40*5 / (5*4000) mm/ms = 0.03 */
	CHECK(v > -20 && v < 20);
}

static void test_n_is_clamped_to_window(void)
{
	/* Entries beyond VLSQ_WINDOW must never be read: make them absurd. */
	int32_t d[VLSQ_WINDOW + 2];
	uint32_t t[VLSQ_WINDOW + 2];

	fill(d, t, VLSQ_WINDOW, 500, +10, 1000, 20);
	d[VLSQ_WINDOW] = 1000000; d[VLSQ_WINDOW + 1] = -1000000;
	t[VLSQ_WINDOW] = 0;       t[VLSQ_WINDOW + 1] = 7;
	CHECK_EQ(vlsq_velocity_cm_s(d, t, VLSQ_WINDOW + 2), 50);
}

static void test_large_uptime_does_not_lose_precision(void)
{
	int32_t d[VLSQ_WINDOW];
	uint32_t t[VLSQ_WINDOW];

	/* 49 days of uptime: without the subtract-the-minimum normalisation
	 * n*sum(t^2) - (sum t)^2 would cancel catastrophically. */
	fill(d, t, VLSQ_WINDOW, 500, +10, 4233600000u, 20);
	CHECK_EQ(vlsq_velocity_cm_s(d, t, VLSQ_WINDOW), 50);

	/* Whole window after the 2^32 ms wrap: also fine. */
	fill(d, t, VLSQ_WINDOW, 500, +10, 200u, 20);
	CHECK_EQ(vlsq_velocity_cm_s(d, t, VLSQ_WINDOW), 50);

	/*
	 * NOT TESTED, KNOWN LIMITATION: a window that STRADDLES the 2^32 ms
	 * (49.7 day) wrap. vlsq_core() picks t_min with a plain `<`, so the
	 * wrapped (small) newest timestamp becomes the minimum and the older
	 * samples normalise to ~4.29e9 ms, whose square overflows int64
	 * (undefined behaviour). Affects at most VLSQ_WINDOW frames once per
	 * 49.7 days of continuous uptime. Recorded in the review notes rather
	 * than exercised here, because executing UB is not a test.
	 */
}

static void test_extreme_but_physical_inputs_stay_in_range(void)
{
	/* Full-scale VL53L1X jump (0 <-> 4000 mm) between two frames 1 ms
	 * apart is the steepest slope the pipeline can present. */
	int32_t d[2] = { 0, 4000 };
	uint32_t t[2] = { 1, 0 };

	CHECK_EQ(vlsq_velocity_cm_s(d, t, 2), 400000);
	d[0] = 4000; d[1] = 0;
	CHECK_EQ(vlsq_velocity_cm_s(d, t, 2), -400000);
}

static void test_acceleration(void)
{
	int32_t v[VLSQ_WINDOW];
	uint32_t t[VLSQ_WINDOW];
	int i;

	/* Closing speed rising 2 cm/s per 20 ms frame = 100 cm/s^2. */
	for (i = 0; i < VLSQ_WINDOW; i++) {
		v[i] = 60 - 2 * i;
		t[i] = 1000u - 20u * (uint32_t)i;
	}
	CHECK_EQ(vlsq_accel_cm_s2(v, t, VLSQ_WINDOW), 100);

	/* Decelerating approach: no sign flip, unlike velocity. */
	for (i = 0; i < VLSQ_WINDOW; i++)
		v[i] = 60 + 2 * i;
	CHECK_EQ(vlsq_accel_cm_s2(v, t, VLSQ_WINDOW), -100);

	/* Constant speed. */
	for (i = 0; i < VLSQ_WINDOW; i++)
		v[i] = 50;
	CHECK_EQ(vlsq_accel_cm_s2(v, t, VLSQ_WINDOW), 0);
}

static void test_inputs_are_not_modified(void)
{
	int32_t d[VLSQ_WINDOW], d0[VLSQ_WINDOW];
	uint32_t t[VLSQ_WINDOW], t0[VLSQ_WINDOW];
	int i;

	fill(d, t, VLSQ_WINDOW, 700, +7, 123456u, 23);
	for (i = 0; i < VLSQ_WINDOW; i++) {
		d0[i] = d[i];
		t0[i] = t[i];
	}
	(void)vlsq_velocity_cm_s(d, t, VLSQ_WINDOW);
	(void)vlsq_accel_cm_s2(d, t, VLSQ_WINDOW);
	for (i = 0; i < VLSQ_WINDOW; i++) {
		CHECK_EQ(d[i], d0[i]);
		CHECK_EQ(t[i], t0[i]);
	}
}

int main(void)
{
	RUN(test_too_few_samples_returns_zero);
	RUN(test_degenerate_timestamps_do_not_divide_by_zero);
	RUN(test_closing_is_positive_retreating_is_negative);
	RUN(test_two_point_window);
	RUN(test_uses_real_timestamps_not_assumed_period);
	RUN(test_rounding_is_to_nearest_ties_away_from_zero);
	RUN(test_noise_is_averaged_not_amplified);
	RUN(test_n_is_clamped_to_window);
	RUN(test_large_uptime_does_not_lose_precision);
	RUN(test_extreme_but_physical_inputs_stay_in_range);
	RUN(test_acceleration);
	RUN(test_inputs_are_not_modified);
	return MU_REPORT();
}
