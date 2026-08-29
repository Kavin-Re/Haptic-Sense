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

/*
 * Periodic health + config-validity read. Call at roughly 1 Hz from
 * sensor_task. NO-OP until drv2605l_init() has armed the device.
 *
 * This is the ONLY thing that can ever report a DRV2605L fault. STATUS bits 1
 * (OVER_TEMP) and 0 (OC_DETECT) are latching and CLEAR ON READ (SLOS854D
 * Table 4), so a value seen once is gone; faults_seen latches them for the
 * life of the boot. OC_DETECT is the device's own verdict that the load
 * impedance is below threshold -- the runtime counterpart to V-W-6, and the
 * difference between "the motor does not buzz" and "the motor does not buzz
 * AND the driver says the coil is out of spec".
 *
 * It also re-reads MODE as the sole authority on config validity
 * (plan v2 Block 1 step 5).
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 */
void drv2605l_poll(void);

/*
 * HAP-T9 / H-D3 — measure the real effect-1 playback duration, in firmware.
 *
 * Call once per sensor frame. It is a NO-OP except in the short window after
 * one of the first few TRIG pulses, and it stops for good after
 * DRV_EFF_SAMPLES_MAX attempts -- it must not keep costing I2C for the life of
 * the demo, and it must never spin the motor on its own.
 *
 * METHOD. Register 0x0C bit 0 is GO. SLOS854D §8.6.2 Table 5, MODE[2:0] = 1:
 * "A rising edge on the IN/TRIG pin sets the GO Bit", and §8.4.3: "The GO bit
 * remains high until the playback of the haptic waveform sequence is
 * complete." So drv2605l_trig_fire() stamps DWT->CYCCNT at the rising edge,
 * and this function polls 0x0C until GO clears and subtracts. CPUCLK is
 * hardware-confirmed at 800 MHz, so cycles convert to microseconds exactly.
 *
 * IT MEASURES NO EXTRA BUZZES. It piggybacks on hazard pulses that were going
 * to fire anyway, so nothing new is heard and nothing is added to a cold-boot
 * demo -- the objection that removed the MODE=6 diagnostic from init.
 *
 * WHAT IT CANNOT DO, stated so the number is not over-read: GO spans the WHOLE
 * sequence, rise plus brake. It cannot separate them the way a differential
 * scope capture across OUT+/OUT- through the SLOS854D Fig. 11 filter can. It
 * bounds them: a total under 75 ms means rise cannot exceed 80 ms, which is
 * enough to settle H-D4 (Library B vs C/D). Take the scope capture too when a
 * two-channel scope is available.
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 */
void drv2605l_measure_service(void);

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
	UW	polls;		/* drv2605l_poll() calls; ok = 24 + 2*polls */
	UW	faults_seen;	/* STICKY OR of OVER_TEMP|OC_DETECT         */
	UW	cfg_lost;	/* polls where MODE was no longer 0x01      */
	/* HAP-T9 effect-duration measurement, microseconds */
	UW	eff_n;		/* valid samples                            */
	UW	eff_last_us;
	UW	eff_min_us;
	UW	eff_max_us;
	UW	eff_late;	/* GO already clear at first poll: DISCARDED */
	UW	eff_stuck;	/* GO never cleared, or a read failed        */
} drv2605l_stats_t;

const drv2605l_stats_t *drv2605l_get_stats(void);

#endif /* APP_DRV2605L_H */
