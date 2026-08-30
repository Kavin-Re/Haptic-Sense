/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_vl53l1_port.h — observability for the VL53L1X ULD platform shim (Haptic-Sense)
 *
 * The nine platform functions themselves are declared by ST's
 * `vl53l1_platform.h` (Lib/STSW-IMG009/.../API/platform/), which is the
 * interface contract. This header adds only what the ULD has no concept of:
 * a way for the heartbeat task to see what the shim has been doing.
 */

#ifndef APP_VL53L1_PORT_H
#define APP_VL53L1_PORT_H

#include "tk/tkernel.h"

/*
 * Shim observability. Single writer (sensor_task, TK_PRI 3); the heartbeat
 * task reads it. 32-bit reads are atomic on ARMv8-M.
 *
 * WHY THIS EXISTS. The ULD collapses every failure into a single int8_t and
 * the API layer ORs those together across dozens of calls, so by the time a
 * status reaches the application it says only "something went wrong somewhere
 * in the last 91 writes". last_er and last_index name which transfer and why.
 */
typedef struct {
	UW	calls;		/* platform functions entered                   */
	UW	xfer_err;	/* i2c_rd/i2c_wr returned non-E_OK              */
	W	last_er;	/* the ER from the most recent failure          */
	UW	last_index;	/* register index of the most recent failure    */
	UW	param_err;	/* bad address, oversized count, NULL pointer   */
	UW	max_count;	/* largest count seen — bounce-buffer headroom  */
	UW	last_addr8;	/* the 8-bit address the ULD last passed in     */
} vl53l1_port_stats_t;

const vl53l1_port_stats_t *vl53l1_port_stats(void);

/* Zero the counters. Call once before VL53L1X_SensorInit() so a bring-up run
 * is not read against a previous attempt's totals.
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
void vl53l1_port_reset_stats(void);

#endif /* APP_VL53L1_PORT_H */
