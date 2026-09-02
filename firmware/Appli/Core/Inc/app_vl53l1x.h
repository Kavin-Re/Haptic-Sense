/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_vl53l1x.h — L2 driver for the VL53L1X ToF sensor (Haptic-Sense, Block 2)
 *
 * Sits on ST's ULD (Lib/STSW-IMG009/.../API/core/VL53L1X_api.c), which sits on
 * our platform shim (app_vl53l1_port.c), which sits on the L1 primitive
 * (app_i2c.c).
 *
 * WHY THIS LAYER EXISTS AT ALL. ST's ULD throws away error status in four
 * separate places (docs/BLOCK2_PREFLIGHT_20260830.md F-2..F-5) and reads
 * uninitialised stack when a transfer fails (F-6). Every function here exists
 * to turn one of those silent failures into a number you can read off the
 * heartbeat. Nothing in this file trusts a returned status on its own.
 *
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 */

#ifndef APP_VL53L1X_H
#define APP_VL53L1X_H

#include "tk/tkernel.h"

/*
 * Init progress / failure point. On success `step` ends at TOF_STEP_DONE.
 * On failure it names the FIRST thing that did not hold, so one printed
 * integer localises the fault without a debugger.
 */
#define TOF_STEP_NOT_RUN	0u
#define TOF_STEP_BOOT		1u	/* FIRMWARE__SYSTEM_STATUS never went nonzero */
#define TOF_STEP_ID		2u	/* model id 0x0000 / 0xFFFF => nothing there   */
#define TOF_STEP_SENSORINIT	3u	/* the 91-write block (F-2)                    */
#define TOF_STEP_DISTMODE	4u	/* SetDistanceMode + readback  (F-3)           */
#define TOF_STEP_BUDGET		5u	/* SetTimingBudget + readback  (F-4)           */
#define TOF_STEP_IMP		6u	/* inter-measurement period, ours not ST's (F-5)*/
#define TOF_STEP_START		7u	/* StartRanging + MODE_START readback          */
#define TOF_STEP_DONE		99u

typedef struct {
	W	init_result;	/* D-5: 1 = not run, E_OK = ranging, otherwise
				 * the ER of the gate that failed. Was left at
				 * 1 on every failure path, so E_TMOUT, E_NOEXS,
				 * E_PAR and E_IO all printed identically.     */
	UW	step;		/* TOF_STEP_*: where it stopped                 */
	UW	sensor_id;	/* expect 0xEACC (DS12385 Table 8 via RdWord)   */
	UW	boot_polls;	/* how many 1 ms polls BootState needed         */
	UW	init_calls;	/* shim calls consumed by SensorInit (>= 91)    */
	UW	init_xfer_err;	/* shim transfer failures during init            */
	UW	cfg_written;	/* config registers written OK — must reach 91    */
	UW	cfg_fail_idx;	/* the register index that refused, 0 = none      */
	UW	dm_rb;		/* GetDistanceMode readback, expect 1           */
	UW	tb_rb;		/* GetTimingBudgetInMs readback, expect 15      */
	UW	osc_cal;	/* RESULT__OSC_CALIBRATE_VAL & 0x3FF            */
	UW	imp_written;	/* the DWord we wrote to INTERMEASUREMENT_PERIOD*/
	UW	imp_rb;		/* readback of the same — must match            */
	UW	mode_start_rb;	/* SYSTEM__MODE_START readback, expect 0x40     */
	UW	vhv_mode;	/* D-1: 1 = warm-up range performed, 0 = neither */
	UW	vhv_polls;	/* D-1: 1 ms polls the warm-up range needed      */

	/* runtime */
	UW	frames;		/* results accepted                             */
	UW	notready;	/* CheckForDataReady SUCCEEDED and said no      */
	UW	drop_ready_err;	/* D-4: CheckForDataReady itself FAILED. Kept
				 * apart from notready, which climbs constantly
				 * in healthy 50 Hz polling and would hide it. */
	UW	drop_clear;	/* D-2: ClearInterrupt failed. Non-zero here and
				 * a frozen `frames` is the signature of a part
				 * that will never interrupt again.            */
	UW	drop_xfer;	/* GetResult returned non-zero, or the shim errored */
	UW	drop_status;	/* transfer fine, result.Status != 0            */
	UW	last_status;	/* most recent non-zero result.Status           */
	UW	last_mm;	/* most recent ACCEPTED distance, mm            */
	UW	min_mm;		/* lifetime, accepted frames only               */
	UW	max_mm;
} vl53l1x_stats_t;

/* Full bring-up: boot gate -> id -> SensorInit -> short mode -> 15 ms budget
 * -> 20 ms inter-measurement -> StartRanging. Every step readback-verified.
 * Returns E_OK only if every gate held. // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
ER vl53l1x_init(void);

/* One non-blocking service call. Reads a frame if one is ready, else returns
 * immediately. Safe to call at the 20 ms sensor cadence.
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
void vl53l1x_service(void);

const vl53l1x_stats_t *vl53l1x_get_stats(void);

#endif /* APP_VL53L1X_H */
