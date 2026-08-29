/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_drv2605l.h — DRV2605L haptic driver, GPIO ownership (Haptic-Sense)
 *
 * Phase 5 Block 0 (2026-08-30): pin ownership only. The register driver
 * (init, arming, effect selection) lands in Block 1 and grows this file.
 *
 * PIN OWNERSHIP — this file is the ONLY place PE7 and PE13 are configured.
 *   PE7  = DRV_EN   (ARD_D8, CN12 pin 1)  — INIT-ONLY, never toggled at runtime
 *   PE13 = DRV_TRIG (ARD_D6, CN11 pin 7)  — pulsed by hazard_task, TK_PRI 1
 *
 * Both are initialised LOW at reset and may only be driven high after the
 * breakout's 3V3 rail is up. TI SLOS854D §6.1 gives EN and IN/TRIG an absolute
 * maximum of VDD + 0.3 V, so the ceiling tracks VDD: driving either high while
 * the breakout is unpowered exceeds abs max (CLAUDE.md §2, derived rule).
 */

#ifndef APP_DRV2605L_H
#define APP_DRV2605L_H

#include "tk/tkernel.h"

/* Configure PE7 (EN) and PE13 (TRIG) as push-pull outputs, both driven LOW.
 * Call from init context, alongside the other GPIO setup, BEFORE any task
 * that touches either pin is started. */
void drv2605l_gpio_init(void);

/* Raise EN and wait for the device to become addressable.
 * TI SLOS854D §8.4.1.3: with EN low the DRV2605L still ACKs its address but
 * permits no register read or write — so this MUST complete before any I2C
 * register access to 0x5A, or every read returns garbage with no error.
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
ER drv2605l_power_up(void);

/* Drive TRIG. TK_PRI 1 (hazard_task) only — GPIO, no I2C, no printf.
 * NO-OP until drv2605l_init() has armed the device: firing a trigger at a
 * part that is still in standby or mid-configuration is meaningless, and once
 * TRIG is physically wired it would fire effects during init.
 * Block 1b replaces this level with the ~2 us edge pulse. */
void drv2605l_trig_set(BOOL on);

/* Full register init: configure for open-loop ERM, select the waveform, arm
 * for external edge trigger, and verify the arming by readback.
 * Call AFTER app_i2c_init(). Does NOT drive the motor — no GO bit is set and
 * no diagnostics run, so this is safe with nothing connected to OUT+/OUT-.
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
ER drv2605l_init(void);

/* Bring-up observability. Single writer (sensor task, TK_PRI 3); the heartbeat
 * task reads it. 32-bit reads are atomic on ARMv8-M. */
typedef struct {
	W	init_result;	/* 1 = not run; E_OK = armed and verified   */
	UW	device_id;	/* STATUS 0x00 bits 7:5, expect 7 = DRV2605L */
	UW	mode_rb;	/* 0x01 readback, expect 0x01               */
	UW	lib_rb;		/* 0x03 readback, expect 0x02 (Library B)   */
	UW	seq_rb;		/* 0x04 readback, expect 0x01 (effect 1)    */
	UW	armed;		/* 1 once the readback check passed         */
} drv2605l_stats_t;

const drv2605l_stats_t *drv2605l_get_stats(void);

#endif /* APP_DRV2605L_H */
