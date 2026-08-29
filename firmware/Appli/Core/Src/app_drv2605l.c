/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_drv2605l.c — DRV2605L haptic driver, GPIO ownership (Haptic-Sense)
 *
 * See app_drv2605l.h for the pin-ownership contract.
 *
 * WHY PE7 MOVED (2026-08-30): through Phase 4 the hazard task drove PE7 as a
 * hazard-LEVEL indicator for the logic analyzer, with no DRV2605L attached.
 * A real board is now on that pin, and EN low is device shutdown: SLOS854D
 * §8.4.1.3 says the part still ACKs its address but permits no register access.
 * Leaving the Phase 4 behaviour in place would drop EN on every non-hazard
 * frame and make the Priority-3 register reads fail intermittently, in a way
 * that looks exactly like a wiring fault. PE7 is now EN, raised once at init
 * and never touched again; the hazard pattern moved to PE13 (DRV_TRIG), which
 * is where the design always intended it.
 *
 * This does NOT affect the RZ3 preemption evidence: that campaign measured
 * PH5 (TIMING_D0) to PD6 (TIMING_D1), both untouched (CLAUDE.md §3).
 */

#include "app_drv2605l.h"
#include "app_i2c.h"
#include "stm32n6xx_hal.h"

#define DRV_EN_PORT		GPIOE
#define DRV_EN_PIN		GPIO_PIN_7	/* ARD_D8,  CN12 pin 1 */
#define DRV_TRIG_PORT		GPIOE
#define DRV_TRIG_PIN		GPIO_PIN_13	/* ARD_D6,  CN11 pin 7 */

/* EN rise -> first legal I2C register access. SLOS854D does not give this
 * directly; 1 kernel tick (1-2 ms, CNF_TIMER_PERIOD=1) is far more than the
 * ~250 us the design doc assumes and costs nothing at boot. HAP-T11 shrinks it
 * on the bench and records the real minimum. */
#define DRV_EN_SETTLE_TICKS	1

/* P3 writes, P1 reads. Single writer, single reader, no read-modify-write, so
 * no lock is needed; this is NOT the race-prone P1->P3 flag that F-1 removed. */
static volatile BOOL drv_armed;

/* Declared here, ABOVE the GPIO and TRIG functions, because both now write
 * to it (pulse_cycles at init, pulses/suppressed at runtime). It used to sit
 * below them, next to the register driver. */
static drv2605l_stats_t dstats;

/* TRIG pulse state. Written ONLY by hazard_task (TK_PRI 1), read only there. */
static UW   drv_last_pulse_ms;
static BOOL drv_pulse_seen;

/* Pulse width in CPU cycles, derived at init from the ACTUAL CPU clock rather
 * than hardcoded. The handoff's "dwt_spin_cycles(1200) ~ 2 us" assumed a
 * 600 MHz core; CPUCLK on this board is 800 MHz (hardware-confirmed
 * 2026-08-30), where 1200 cycles is 1.5 us. SLOS854D §8.4.5.1: "The pulse
 * width should be at least 1 us to ensure detection" — 1.5 us would still
 * pass, but deriving it removes the constant from the failure surface
 * entirely. cpu_hz / 500000 = 2 us worth of cycles. */
static UW drv_pulse_cycles = 1600u;	/* 2 us at 800 MHz until init runs */


void drv2605l_gpio_init(void)
{
	GPIO_InitTypeDef gpio_init = {0};

	__HAL_RCC_GPIOE_CLK_ENABLE();	/* already on via CONSOLE_Config (PE5/PE6) */

	/* Drive the ODR low BEFORE switching the pins to output, so neither pin
	 * can glitch high for even one cycle while the breakout's rail state is
	 * unknown (abs max tracks VDD — see the header). */
	HAL_GPIO_WritePin(DRV_EN_PORT, DRV_EN_PIN, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(DRV_TRIG_PORT, DRV_TRIG_PIN, GPIO_PIN_RESET);

	gpio_init.Mode  = GPIO_MODE_OUTPUT_PP;
	gpio_init.Pull  = GPIO_NOPULL;
	gpio_init.Speed = GPIO_SPEED_FREQ_LOW;

	gpio_init.Pin = DRV_EN_PIN;
	HAL_GPIO_Init(DRV_EN_PORT, &gpio_init);

	gpio_init.Pin = DRV_TRIG_PIN;
	HAL_GPIO_Init(DRV_TRIG_PORT, &gpio_init);

	/* Derive the pulse width from the real CPU clock (see drv_pulse_cycles).
	 * Runs in main_thread (TK_PRI 15) before any task is started, so it is
	 * ordered before the first hazard_task call by construction. */
	{
		UW cpu_hz = (UW)HAL_RCC_GetCpuClockFreq();
		if (cpu_hz >= 1000000u)
			drv_pulse_cycles = cpu_hz / 500000u;	/* 2 us */
		dstats.pulse_cycles = drv_pulse_cycles;
	}
}

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
ER drv2605l_power_up(void)
{
	HAL_GPIO_WritePin(DRV_EN_PORT, DRV_EN_PIN, GPIO_PIN_SET);
	return tk_dly_tsk(DRV_EN_SETTLE_TICKS);
}

/*
 * Busy-spin for a cycle count, TK_PRI 1, ~2 us. A kernel delay cannot be used
 * here: the shortest is one tick (1-2 ms) and it would block the highest
 * priority task in the system for 1000x the required time.
 *
 * THE ITERATION GUARD IS NOT DEFENSIVE PADDING. DWT->CYCCNT is enabled in
 * app_gpio_init() but the enable CAN be rejected on this target (secure
 * non-invasive debug under TZEN is owned by the FSBL, not by us) and the
 * counter then reads a constant. A pure `while (CYCCNT - t0 < n)` on a frozen
 * counter never terminates, and it would hang TK_PRI 1 — the highest priority
 * task — which stops the entire system with no error and no output. The guard
 * turns that into a slightly-wrong pulse width instead.
 */
static void drv_spin_cycles(UW cycles)
{
	UW t0 = DWT->CYCCNT;
	UW guard = (cycles << 2) + 1000u;

	while ((UW)(DWT->CYCCNT - t0) < cycles) {	/* UW: wrap-safe */
		if (--guard == 0u)
			break;			/* counter stopped — never hang */
	}
}

/* // ONLY CALL FROM PRIORITY 1 HAZARD TASK — GPIO only, no I2C, no printf */
BOOL drv2605l_trig_fire(UW want_interval_ms)
{
	SYSTIM now;
	UW iv;

	if (!drv_armed)
		return FALSE;		/* not configured yet — see the header */

	iv = want_interval_ms;
	if (iv < DRV_R3_FLOOR_MS)
		iv = DRV_R3_FLOOR_MS;
	if (iv > DRV_TRIG_MAX_MS)
		iv = DRV_TRIG_MAX_MS;

	/* tk_get_otm is a non-blocking read of the operating-time counter; it
	 * never waits, so it is legal at TK_PRI 1. UW subtraction is wrap-safe
	 * (tim.lo wraps at 2^32 ms ~= 49.7 days). */
	if (tk_get_otm(&now) != E_OK)
		return FALSE;

	if (drv_pulse_seen && (UW)(now.lo - drv_last_pulse_ms) < iv) {
		dstats.suppressed++;
		return FALSE;
	}

	HAL_GPIO_WritePin(DRV_TRIG_PORT, DRV_TRIG_PIN, GPIO_PIN_SET);
	drv_spin_cycles(drv_pulse_cycles);
	HAL_GPIO_WritePin(DRV_TRIG_PORT, DRV_TRIG_PIN, GPIO_PIN_RESET);

	drv_last_pulse_ms = now.lo;
	drv_pulse_seen = TRUE;
	dstats.pulses++;
	return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Register driver — open-loop ERM, ROM library, external edge trigger       */
/*                                                                          */
/* Every value below is verified against the local datasheet copy            */
/* docs/datasheets/drv2605l_datasheet.pdf (TI SLOS854D Rev D, March 2018).   */
/* Decision 4 (plan v2 §1): R-1 — EN is init-only and is never toggled at    */
/* runtime; the kill path is a STANDBY write from TK_PRI 3, not a GPIO yank. */
/* The MODE=6 actuator diagnostic from DRV2605L_P3_INIT_ARMING_DESIGN.md is  */
/* deliberately NOT in this sequence: it spins the motor, and running it at  */
/* every power-up in front of a judge is not a feature. Run it by hand as    */
/* HAP-T12 when a motor is attached.                                        */
/* ------------------------------------------------------------------------ */

#define DRV_ADDR7		0x5Au	/* SLOS854D §8.5.1.1; silkscreened on the board */

#define DRV_REG_STATUS		0x00u	/* reset 0xE0 */
#define DRV_REG_MODE		0x01u	/* reset 0x40 */
#define DRV_REG_LIBRARY		0x03u	/* reset 0x01 */
#define DRV_REG_WAVSEQ1		0x04u	/* reset 0x01 */
#define DRV_REG_WAVSEQ2		0x05u	/* reset 0x00 */
#define DRV_REG_ODCLAMP		0x17u	/* reset 0x8C */
#define DRV_REG_FEEDBACK	0x1Au	/* reset 0x36 */
#define DRV_REG_CONTROL3	0x1Du	/* reset 0xA0 */

#define DRV_MODE_STANDBY_CLR	0x00u	/* STANDBY=0, MODE=0 internal trigger  */
#define DRV_MODE_EDGE_TRIG	0x01u	/* STANDBY=0, MODE=1 external EDGE     */
#define DRV_DEVICE_ID_L		7u	/* §8.6.1 Table 4: 7 = DRV2605L, 3 = non-L */
#define DRV_LIBRARY_B		0x02u	/* §8.6.4 Table 7: 2 = TS2200 Library B */
#define DRV_EFFECT_STRONG_CLICK	0x01u	/* §12.1.2: 1 = "Strong Click - 100%"  */

/* Sentinel for every read. Neither a plausible register value nor the 0xFF an
 * idle pulled-up bus returns — the rule that surfaced the GPDMA defect. */
#define DRV_SENTINEL		0xA5u

/* One aligned file-static buffer for every transfer (PROJECT_DEFENSE F-6c). */
static UB drv_buf[32] __attribute__((aligned(32)));


/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
static ER drv_wr8(UB reg, UB val)
{
	drv_buf[0] = val;
	return i2c_wr(DRV_ADDR7, (UW)reg, I2C_REG8, drv_buf, 1);
}

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
static ER drv_rd8(UB reg, UB *out)
{
	ER err;

	drv_buf[0] = DRV_SENTINEL;
	err = i2c_rd(DRV_ADDR7, (UW)reg, I2C_REG8, drv_buf, 1);
	if (err == E_OK)
		*out = drv_buf[0];
	return err;
}

/*
 * Configuration sequence. ORDER MATTERS: everything is written while the part
 * is out of standby but still on the internal trigger, and the arm (MODE=1) is
 * the LAST write, so the device cannot respond to a trigger edge mid-config.
 *
 * NOTE ON VERIFICATION: 0x1A (0x36), 0x1D (0xA0) and 0x05 (0x00) are written
 * with their POWER-ON RESET values, and 0x04's 0x01 is also the reset value.
 * They are here as explicit statements of intent, but a readback of any of
 * them proves nothing — it cannot distinguish "my write landed" from "the part
 * just reset". Only three registers actually change from reset, and those are
 * the ones drv2605l_init() verifies:
 *     0x17 OD_CLAMP   0x8C -> 0x8B
 *     0x03 LIBRARY    0x01 -> 0x02
 *     0x01 MODE       0x40 -> 0x01
 */
static const struct {
	UB reg;
	UB val;
} drv_cfg[] = {
	/* Leave software standby, stay on the internal trigger while configuring.
	 * §8.6.2 Table 5: STANDBY reset = 1, so the part boots asleep. */
	{ DRV_REG_MODE,     DRV_MODE_STANDBY_CLR },

	/* ERM (not LRA). N_ERM_LRA is bit 7 of 0x1A; 0x36 is the reset value and
	 * already selects ERM (§8.6.20 Figure 49, R/W-0). Explicit anyway. */
	{ DRV_REG_FEEDBACK, 0x36u },

	/* ERM_OPEN_LOOP = 1 (bit 5 of CONTROL3). 0xA0 is the reset value
	 * (§8.6.23 Figure 52). Open loop is the locked baseline — closed loop is
	 * deferred per drv2605l_port_design_v1.md §4.2/§5. */
	{ DRV_REG_CONTROL3, 0xA0u },

	/* OD_CLAMP sets the FULL-SCALE reference for open-loop drive (§8.6.17,
	 * and §8.5.2.2: "also serves as the full-scale reference voltage for
	 * open-loop operation"). SLOS854D Eq. 6: V(ERM-OL_AV) = 21.59 mV x
	 * OD_CLAMP, so 0x8B = 139 x 21.59 mV = 3.001 V for a 3 V-class ERM.
	 * The 0x8C default would give 3.022 V. No auto-calibration follows
	 * because this design is open-loop, so the datasheet's "modify then
	 * calibrate" note does not apply. */
	{ DRV_REG_ODCLAMP,  0x8Bu },

	/* TS2200 Library B: rated 3 V, rise 40-60 ms, brake 5-15 ms (Table 1) —
	 * the fastest-braking of the four 3 V libraries, which is what a hazard
	 * alert wants. H-D4 stays OPEN until the scope confirms the real rise and
	 * brake fall inside those windows. */
	{ DRV_REG_LIBRARY,  DRV_LIBRARY_B },

	/* Waveform sequence: effect 1, then a zero terminator. Playback begins at
	 * 0x04 when GO is set and stops at the first zero (§8.3.5.2.1). */
	{ DRV_REG_WAVSEQ1,  DRV_EFFECT_STRONG_CLICK },
	{ DRV_REG_WAVSEQ2,  0x00u },
};

#define DRV_CFG_COUNT	(sizeof(drv_cfg) / sizeof(drv_cfg[0]))

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
ER drv2605l_init(void)
{
	UB v;
	ER err;
	UINT i;

	dstats.init_result = 1;		/* 1 = not run; 0 would read as E_OK */
	drv_armed = FALSE;

	/* Device identity. EN must already be high (drv2605l_power_up), or this
	 * read returns garbage with no error: §8.4.1.3 says the part still ACKs
	 * its address with EN low but permits no register access. */
	err = drv_rd8(DRV_REG_STATUS, &v);
	if (err != E_OK)
		goto done;
	/* Keep the WHOLE byte. STATUS bit 3 DIAG_RESULT, bit 1 OVER_TEMP and
	 * bit 0 OC_DETECT are latching fault flags that CLEAR ON READ
	 * (SLOS854D Table 4, verbatim: "This bit clears upon read"). Masking
	 * them off at the only place the register is read means an
	 * overcurrent or overtemperature event can never be reported by this
	 * firmware -- it would present as a weak or silent motor with
	 * armed=1 and no error anywhere. Same failure class as RZ9. */
	dstats.status_rb = (UW)v;
	dstats.device_id = (UW)(v >> 5);		/* bits 7:5, NOT v & 0x07 */
	if (dstats.device_id != DRV_DEVICE_ID_L) {
		err = E_NOEXS;
		goto done;
	}

	for (i = 0; i < DRV_CFG_COUNT; i++) {
		err = drv_wr8(drv_cfg[i].reg, drv_cfg[i].val);
		if (err != E_OK)
			goto done;
	}

	/* Arm LAST. TRIG must be low across this write (PROJECT_DEFENSE F-7) —
	 * a rising edge on a part that has just entered edge-trigger mode would
	 * fire an effect during init. drv2605l_trig_set() is gated on drv_armed,
	 * which is still FALSE here, so hazard_task cannot raise it. */
	err = drv_wr8(DRV_REG_MODE, DRV_MODE_EDGE_TRIG);
	if (err != E_OK)
		goto done;

	/* Verify the arming by readback — the three registers that actually
	 * differ from their reset values. */
	err = drv_rd8(DRV_REG_MODE, &v);
	if (err != E_OK)
		goto done;
	dstats.mode_rb = v;

	err = drv_rd8(DRV_REG_LIBRARY, &v);
	if (err != E_OK)
		goto done;
	dstats.lib_rb = v;

	err = drv_rd8(DRV_REG_WAVSEQ1, &v);
	if (err != E_OK)
		goto done;
	dstats.seq_rb = v;

	/* OD_CLAMP is the THIRD register that actually differs from reset, and
	 * until 2026-08-30 it was written and never checked while WAVSEQ1 --
	 * whose expected value 0x01 IS its reset value (Table 3) -- stood in
	 * the pass condition in its place. A readback of WAVSEQ1 cannot
	 * distinguish "my write landed" from "the part reset", which is what
	 * the comment above this table already says; the code disagreed with
	 * its own comment. 0x8B is neither the 0x8C reset value nor the 0xA5
	 * sentinel, so this readback discriminates all three states. */
	err = drv_rd8(DRV_REG_ODCLAMP, &v);
	if (err != E_OK)
		goto done;
	dstats.odc_rb = v;

	/* PASS CONDITION -- only registers whose expected value differs from
	 * BOTH the reset value and DRV_SENTINEL may appear here.
	 *   0x01 MODE     0x40 -> 0x01   discriminates
	 *   0x03 LIBRARY  0x01 -> 0x02   discriminates (mask 0x07, HI_Z is b4)
	 *   0x17 OD_CLAMP 0x8C -> 0x8B   discriminates
	 * seq_rb is deliberately NOT here: (seq_rb & 0x7F) == 0 is false for
	 * the 0xA5 sentinel (0x25) AND for the 0x01 reset value, so it passes
	 * in every failure mode it was supposed to catch. It stays as printed
	 * observability only. */
	if (dstats.mode_rb != DRV_MODE_EDGE_TRIG ||
	    (dstats.lib_rb & 0x07u) != DRV_LIBRARY_B ||
	    dstats.odc_rb != 0x8Bu) {
		err = E_OBJ;		/* transfers fine, device not configured */
		goto done;
	}

	drv_armed = TRUE;
	dstats.armed = 1u;
	err = E_OK;

done:
	dstats.init_result = (W)err;
	return err;
}

const drv2605l_stats_t *drv2605l_get_stats(void)
{
	return &dstats;
}

