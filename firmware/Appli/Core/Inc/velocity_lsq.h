/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * velocity_lsq.h — least-squares slope estimator for closing velocity and
 * acceleration (Haptic-Sense, Block 2/4).
 *
 * DESIGN, docs/PHASE5_DESIGN_sensor_bringup_i2c.md section "Feature
 * computation" (verbatim):
 *   "Naive v = (d(t-1) - d(t))/20 ms amplifies mm-level ToF noise: +-3 mm
 *    frame noise -> +-15 cm/s velocity noise, on the same order as the
 *    20 cm/s hazard threshold. Unacceptable. Design: least-squares slope
 *    over the last 5 samples (100 ms window) for v; a = same estimator
 *    over the v history. Sign convention: positive v = closing. Implement
 *    once in a tiny shared C file compiled both into firmware and into
 *    the training-data generator script -- one source of truth."
 *
 * That naive form is what actually shipped in app_tasks.c (found by the
 * 2026-09-03 full-project audit, docs/audits/PROJECT_AUDIT_20260903.md
 * section 1.3) -- this file is the design catching up to itself.
 *
 * INTEGER ONLY, DELIBERATELY. See app_vl53l1x.c:263-273: touching the FPU
 * in a TK_PRI 3 task latches CONTROL.FPCA, and every later preemption of
 * that task then stacks S16-S31 -- which is exactly what the Block 9
 * PH6-3 timing campaign measures. Integer arithmetic avoids the question
 * entirely, the same reason app_vl53l1x.c's IMP calculation is integer.
 *
 * SELF-CONTAINED ON PURPOSE (only <stdint.h>): this file has no tkernel or
 * HAL dependency so it can be compiled unmodified into a host-side
 * training-data generator script, per the design doc's "one source of
 * truth" instruction -- the firmware and the offline label/feature
 * pipeline must compute IDENTICAL numbers from the same samples, or the
 * deployed model sees a distribution it never trained on (Red Zone #6).
 */
#ifndef VELOCITY_LSQ_H
#define VELOCITY_LSQ_H

#include <stdint.h>

/* Design doc: "last 5 samples (100 ms window)" at the locked 50 Hz frame
 * rate. Callers may pass fewer than this many valid samples (e.g. during
 * the first ~100 ms after boot); see vlsq_velocity_cm_s below. */
#define VLSQ_WINDOW	5

/*
 * Closing velocity from a distance history, least-squares slope over up to
 * VLSQ_WINDOW (time, distance) samples.
 *
 * d_mm[0..n-1]: distance samples in mm, d_mm[0] = NEWEST (matches this
 *               project's feature_buf[0] = d(t) convention).
 * t_ms[0..n-1]: the wall-clock timestamp (tk_get_otm().lo, or any
 *               monotonic ms counter) each d_mm sample was taken at, same
 *               newest-first order. Real per-sample time, not an assumed
 *               fixed period -- the frame loop's tk_slp_tsk() is a delay,
 *               not a period, so successive frames are not exactly 20 ms
 *               apart under any load (bus recovery, a retry burst, the
 *               [EFF] measurement window).
 * n:            number of valid samples, 0 <= n <= VLSQ_WINDOW. Only the
 *               first n entries of both arrays are read.
 *
 * Returns the closing velocity in cm/s, positive = approaching (distance
 * decreasing over time) -- the design doc's sign convention. Returns 0 if
 * n < 2 (not enough samples for a slope yet) or the window's timestamps
 * are degenerate (all identical -- tk_get_otm() did not advance between
 * samples, which should not happen but must not divide by zero if it does).
 */
int32_t vlsq_velocity_cm_s(const int32_t *d_mm, const uint32_t *t_ms, int n);

/*
 * Closing acceleration from a VELOCITY history -- "the same estimator over
 * the v history" (design doc, verbatim). Same shape and same rules as
 * vlsq_velocity_cm_s: v_cm_s[0] = NEWEST, t_ms[0] = its timestamp, n valid
 * samples, 0 <= n <= VLSQ_WINDOW.
 *
 * Returns cm/s^2, positive = closing speed increasing. Returns 0 if n < 2
 * or the window is degenerate, same as above.
 */
int32_t vlsq_accel_cm_s2(const int32_t *v_cm_s, const uint32_t *t_ms, int n);

#endif /* VELOCITY_LSQ_H */
