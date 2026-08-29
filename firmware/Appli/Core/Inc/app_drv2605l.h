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

/*
 * Fire ONE waveform: a ~2 us rising edge on TRIG, rate-limited.
 * TK_PRI 1 (hazard_task) only — GPIO + DWT read + tk_get_otm. No I2C, no
 * printf, no blocking call.
 *
 * WHY AN EDGE AND NOT A LEVEL (SLOS854D §8.6.2 Table 5, MODE[2:0] = 1,
 * verbatim): "A rising edge on the IN/TRIG pin sets the GO Bit. A second
 * rising edge on the IN/TRIG pin cancels the waveform if the second rising
 * edge occurs before the GO bit has cleared." The device is armed in EDGE
 * mode, so the old level API was wrong in two ways at once: a level that
 * stays high across a multi-frame hazard produces exactly ONE buzz and then
 * silence, and a level that flaps faster than the effect lasts cancels
 * every playback after the first. Both fail silently.
 *
 * want_interval_ms is the caller's urgency request; it is CLAMPED to
 * [DRV_R3_FLOOR_MS, DRV_TRIG_MAX_MS] and enforced here rather than by the
 * caller, because a violated floor inverts the product thesis — more urgency
 * yielding a WEAKER buzz — with no error anywhere.
 *
 * Returns TRUE if an edge was actually emitted, FALSE if the call was
 * suppressed by the rate limit or the device is not armed.
 */
BOOL drv2605l_trig_fire(UW want_interval_ms);

/* R-3 rate limit. INTERIM VALUE until HAP-T9 (T4) scopes the real effect-1
 * duration; the rule is measured_duration x 1.2. Library B predicts 45-75 ms
 * (SLOS854D Table 1: rise 40-60 ms, brake 5-15 ms), so 75 x 1.2 = 90 ms is
 * the predicted floor and 125 ms is deliberately conservative.
 * DO NOT LOWER THIS WITHOUT THE SCOPE CAPTURE. */
#define DRV_R3_FLOOR_MS		125u
#define DRV_TRIG_MAX_MS		1000u

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
	UW	odc_rb;		/* 0x17 readback, expect 0x8B (reset 0x8C)  */
	UW	status_rb;	/* 0x00 raw byte; bit1 OVER_TEMP, bit0 OC   */
	UW	armed;		/* 1 once the readback check passed         */
	UW	pulses;		/* TRIG edges actually emitted              */
	UW	suppressed;	/* fire() calls refused by the R-3 limit    */
	UW	pulse_cycles;	/* CPU cycles per ~2 us pulse, from CPUCLK  */
	UW	rst_polls;	/* H-D7: MODE reads until DEV_RESET cleared */
	UW	rst_mode;	/* MODE after reset, expect 0x40            */
} drv2605l_stats_t;

const drv2605l_stats_t *drv2605l_get_stats(void);

#endif /* APP_DRV2605L_H */
