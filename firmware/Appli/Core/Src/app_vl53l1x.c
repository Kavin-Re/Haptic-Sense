/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_vl53l1x.c — L2 driver for the VL53L1X ToF sensor (Haptic-Sense, Block 2)
 *
 * Every gate in this file traces to a specific defect found by reading ST's
 * ULD source on 2026-08-30. The findings are written up in
 * docs/BLOCK2_PREFLIGHT_20260830.md; the short version is in each comment.
 *
 * TASK CONTEXT: sensor_task, TK_PRI 3. All I2C (CLAUDE.md §3).
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 */

#include "app_vl53l1x.h"
#include "app_vl53l1_port.h"
#include "app_i2c.h"
#include "VL53L1X_api.h"
#include "vl53l1_platform.h"
#include <string.h>

/*
 * EIGHT-bit address. The ULD and the platform shim both take 8-bit; the shim
 * shifts once (`dev >> 1`) to reach the L1 primitive's 7-bit convention, and
 * REJECTS an odd value outright, so passing the 7-bit 0x29 here fails loudly
 * instead of silently addressing the wrong part. 0x29 << 1 = 0x52.
 * CLAUDE.md §2; DS12385 default address.
 */
#define TOF_DEV8		0x52u

/* DS12385 §4.2 Table 8 via RdWord: 0x010F=0xEA, 0x0110=0xCC. ST's own header
 * comment says 0xEEAC (VL53L1X_api.h:197) and is a typo — the datasheet wins. */
#define TOF_ID_EXPECT		0xEACCu

#define TOF_DISTANCE_MODE_SHORT	1u	/* plan v2 step 12: short mode FIRST */
#define TOF_BUDGET_MS		15u	/* 15 ms exists ONLY in short mode   */
#define TOF_IMP_MS		20u	/* 50 Hz; must be >= budget          */

#define TOF_BOOT_POLL_MAX	50u	/* bounded: a wedged part must not livelock P3 */

/*
 * D-1 (2026-09-03). THE WARM-UP RANGE AND THE TWO VHV WRITES ARE ONE UNIT.
 * Performing the second half without the first is a silent-wrong-data defect.
 *
 * ST's VL53L1X_SensorInit (VL53L1X_api.c:184-203), verbatim in order:
 *
 *     for (Addr = 0x2D; Addr <= 0x87; Addr++)  WrByte(...)
 *     StartRanging -> poll data-ready -> ClearInterrupt -> StopRanging
 *     WrByte(VL53L1_VHV_CONFIG__TIMEOUT_MACROP_LOOP_BOUND / * 0x0008 * /, 0x09)
 *                                              / * "two bounds VHV"          * /
 *     WrByte(0x0B, 0)   / * "start VHV from the previous temperature"        * /
 *
 * That one-shot range IS the VHV calibration. 0x08 <- 0x09 then narrows the
 * VHV loop bound to two, and 0x0B <- 0 tells the part to START FROM THE
 * PREVIOUS RESULT instead of recalibrating. Issuing those two writes WITHOUT
 * the warm-up instructs the part to reuse a calibration that was never
 * computed, at a reduced loop bound. Nothing returns an error. The part
 * ranges, and reports distances that are simply biased.
 *
 * That is defect class #2 of this project -- "a value indistinguishable from a
 * real reading" -- reappearing in a new place. The previous revision of this
 * file did exactly that: it dropped the warm-up (correctly: it contains the
 * 1000-iteration poll that was timing out under the 2.04 V brownout) and KEPT
 * both writes. Its comment framed the deviation as being about the timeout
 * only. It was also about the calibration, and it did not say so.
 *
 * Two self-consistent replacements, selected here:
 *
 *   TOF_VHV_WARMUP 1  ST's intent. Restore the one-shot range, but polled by
 *                     OUR bounded loop rather than ST's 1000-iteration one,
 *                     then both writes. Calibration performed; the flags mean
 *                     what they say. Costs one ranging period at init and
 *                     reintroduces a poll that has never once been observed
 *                     to complete on this hardware.
 *
 *   TOF_VHV_WARMUP 0  Neither half. Leave 0x08 and 0x0B at their reset values
 *                     so the part performs a full VHV calibration on the
 *                     first real range. Never claims a result that does not
 *                     exist. Costs a slower first frame and nothing else.
 *
 * DEFAULTS TO 0: it cannot fail, and it is strictly safer than the code it
 * replaces. This is a DEVELOPER DECISION deliberately left as a one-token
 * flip (CLAUDE.md SS9). Once [RTY] reads all-zero at VIN >= 3.2 V, setting
 * this to 1 and confirming `[RNG] vhv=1 vpolls=<small>` is a ten-minute
 * experiment, and it is the honest way to find out whether the warm-up
 * matters for this application. Compare accepted distances against a tape
 * measure in both modes before choosing permanently.
 *
 * *(evidence: the ST call sequence above, read in-repo. inference: that the
 * bias is non-zero. NOT quantified -- DS12385 does not document VHV, so the
 * magnitude is unknown and must not be asserted.)*
 */
#define TOF_VHV_WARMUP		0
#define TOF_VHV_POLL_MAX	40u	/* 40 x 1 ms against a 15 ms budget: 2.6x */

/*
 * D-A (2026-09-03). E_OK IS 0, AND SO IS BSS.
 *
 * init_result's "1 = not run" sentinel used to be assigned inside
 * vl53l1x_init() -- the one function that does not run in the failure case.
 * With the 7SEMI absent, app_tasks.c skips the init (correctly: it is gated on
 * the raw probe) but calls vl53l1x_service() unconditionally, and service's
 * guard was `if (tstats.init_result != E_OK) return;`. The BSS zero PASSED
 * that guard, so the frame loop ran against a sensor that was never
 * configured: every CheckForDataReady NACKed into the shim's 25-attempt retry,
 * each paced by tk_dly_tsk(1) -- 25-50 ms of delay inside a 20 ms frame
 * budget, for ever, while [HB] frames kept climbing and [RNG] read init=0,
 * which app_tasks.c documents as the PASS value.
 *
 * Two fixes, because either alone is insufficient: the sentinel is set HERE,
 * and `tof_running` is a separate flag set ONLY on the success path. A guard
 * whose "safe" value is zero is a guard that trusts uninitialised memory.
 */
static vl53l1x_stats_t tstats = {
	.init_result = 1,		/* 1 = not run; 0 would read as E_OK */
	.min_mm      = 0xFFFFFFFFu,
};

/* Set TRUE only when vl53l1x_init() returned E_OK; the sole gate on the frame
 * loop. // WRITTEN AND READ FROM PRIORITY 3 SENSOR TASK ONLY */
static BOOL tof_running = FALSE;

/*
 * ST's default configuration table, written one register at a time by
 * VL53L1X_SensorInit. Declared `const uint8_t VL51L1X_DEFAULT_CONFIGURATION[]`
 * at VL53L1X_api.c:60 -- NOT static, so it is ours to use.
 */
extern const uint8_t VL51L1X_DEFAULT_CONFIGURATION[];

/*
 * THE 91-WRITE CONFIGURATION BLOCK, OURS INSTEAD OF ST's, FOR TWO REASONS.
 *
 * 1. F-2. VL53L1X_SensorInit accumulates the 91 writes into `status` with |=
 *    and then OVERWRITES that status at VL53L1X_api.c:190 and :197, so it can
 *    return 0 with any number of them failed. Here every write is checked and
 *    the first refusal stops the loop and names the register.
 *
 * 2. TWO ENTRIES IN ST's TABLE ARE WRONG FOR THIS BOARD. Verbatim from
 *    VL53L1X_api.c:61-63:
 *
 *      0x00, / * 0x2d : set bit 2 and 5 to 1 for fast plus mode (1MHz I2C) * /
 *      0x00, / * 0x2e : bit 0 if I2C pulled up at 1.8V,
 *                       else set bit 0 to 1 (pull up at AVDD) * /
 *      0x00, / * 0x2f : bit 0 if GPIO pulled up at 1.8V,
 *                       else set bit 0 to 1 (pull up at AVDD) * /
 *
 *    0x2E and 0x2F configure the I2C and GPIO PAD pull-up reference. ST ships
 *    both as 0x00, which declares "pulled up at 1.8 V".
 *
 *    0x2E (I2C PADS) IS CITED. The 7SEMI's I2C pull-ups measure 2 x 9.9 kOhm
 *    to VIN (CLAUDE.md §2, DMM, boards disconnected) and VIN is the 3.32 V
 *    system rail (V-W-1, CN8 3.32 V). Per ST's own comment bit 0 must be 1.
 *
 *    D-8. 0x2F (GPIO PAD) IS NOT CITED BY THAT MEASUREMENT, and the previous
 *    revision of this comment used it for both. The same measurement campaign
 *    closed the OPPOSITE finding for GPIO1: CLAUDE.md's VL53L1X ledger records
 *    "GPIO1 HAS NO PULL-UP ON THE 7SEMI ... GPIO1<->VIN measured 2.46 MOhm",
 *    confirmed a second time on the analyzer. So NEITHER branch of ST's
 *    comment describes this board's GPIO1: it is not pulled up at 1.8 V and it
 *    is not pulled up at AVDD. It is not pulled up at all.
 *
 *    The patch is retained because 0x2F selects the pad's SUPPLY REFERENCE,
 *    not whether a pull-up exists, and AVDD (3.3 V) is the correct reference
 *    for a pad this board drives against 3.3 V logic if it is ever used at
 *    all. Block 2 does not use it -- the frame loop polls CheckForDataReady
 *    over I2C and nothing reads PD0 (CLAUDE.md §2 map: TOF_INT = PD0 = CN11
 *    pin 3). *(0x2E: evidence. 0x2F: inference, and unexercised. If EXTI on
 *    GPIO1 is ever wanted, revisit this line together with the 10 kOhm
 *    GPIO1->VIN pull-up the ledger already specifies.)*
 *
 *    WHY THIS IS THE LEADING SUSPECT for the 12.8 ms address-refusal window:
 *    every transaction that works -- the L1 probe, BootState, GetSensorId,
 *    4,236 reads -- happens with the pads in their RESET state. The refusals
 *    begin only after this block starts writing, i.e. after 0x2E has told the
 *    part its 3.3 V bus is a 1.8 V bus. It does not by itself explain why the
 *    onset is at write ~31 rather than write ~3, so it is a strong lead and
 *    not yet a proven cause. *(inference, medium-high confidence)*
 *
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 */
#define TOF_CFG_FIRST		0x2Du
#define TOF_CFG_LAST		0x87u
#define TOF_PAD_I2C_AVDD	0x2Eu
#define TOF_PAD_GPIO_AVDD	0x2Fu

static ER tof_write_config(void)
{
	UW addr;
	uint8_t v;

	tstats.cfg_written  = 0u;
	tstats.cfg_fail_idx = 0u;

	for (addr = TOF_CFG_FIRST; addr <= TOF_CFG_LAST; addr++) {
		v = VL51L1X_DEFAULT_CONFIGURATION[addr - TOF_CFG_FIRST];

		if (addr == TOF_PAD_I2C_AVDD || addr == TOF_PAD_GPIO_AVDD)
			v = (uint8_t)(v | 0x01u);	/* pads referenced to AVDD, not 1.8 V */

		if (VL53L1_WrByte(TOF_DEV8, (uint16_t)addr, v) != 0) {
			tstats.cfg_fail_idx = addr;
			return E_IO;
		}
		tstats.cfg_written++;
	}

	/* D-1. See the TOF_VHV_WARMUP note above: the warm-up range and the two
	 * VHV flag writes stand or fall together. Never one without the other. */
#if TOF_VHV_WARMUP
	{
		uint8_t vready = 0;
		UW i;

		if (VL53L1X_StartRanging(TOF_DEV8) != 0)
			return E_IO;

		/* Bounded, paced, and it FAILS rather than spinning -- the same
		 * M-2 rule as the boot gate. ST's version loops 1000 times. */
		for (i = 0; i < TOF_VHV_POLL_MAX; i++) {
			tstats.vhv_polls = i + 1u;
			if (VL53L1X_CheckForDataReady(TOF_DEV8, &vready) == 0 &&
			    vready != 0u)
				break;
			tk_dly_tsk(1);
		}
		if (vready == 0u)
			return E_TMOUT;	/* warm-up never completed: do NOT write
					 * the flags -- that is the whole defect */

		if (VL53L1X_ClearInterrupt(TOF_DEV8) != 0)
			return E_IO;
		if (VL53L1X_StopRanging(TOF_DEV8) != 0)
			return E_IO;

		/* Only NOW is "start VHV from the previous temperature" true. */
		if (VL53L1_WrByte(TOF_DEV8, VL53L1_VHV_CONFIG__TIMEOUT_MACROP_LOOP_BOUND, 0x09) != 0)
			return E_IO;
		if (VL53L1_WrByte(TOF_DEV8, 0x0Bu, 0x00u) != 0)
			return E_IO;
	}
#endif
	tstats.vhv_mode = (UW)TOF_VHV_WARMUP;

	return E_OK;
}

/* ------------------------------------------------------------------------ */

static void tof_fail(UW step)
{
	tstats.step = step;
}

/*
 * INTER-MEASUREMENT PERIOD, COMPUTED AND WRITTEN BY US, NOT BY THE ULD.
 *
 * VL53L1X_SetInterMeasurementInMs (VL53L1X_api.c:460-471) has three problems
 * this replaces:
 *   1. It discards the return value of its own WrDWord entirely — the status
 *      it returns describes only the RdWord that preceded it (F-5).
 *   2. It does not check IMP >= timing budget. The handoff already flags this
 *      as "the API does not check it — enforce it". If IMP < budget the part
 *      free-runs and the 50 Hz cadence is a fiction.
 *   3. It multiplies by the double literal 1.075, which would be the first
 *      floating-point arithmetic in a task in this project. The FPU context IS
 *      saved here (mtk3_bsp2/config/config.h:126 USE_FPU 1, and only that
 *      config dir is on the include path), so it would be safe — but once a
 *      task executes an FP instruction, CONTROL.FPCA latches and every later
 *      preemption of it stacks S16-S31, which the Block 9 PH6-3 re-run would
 *      then be measuring. Integer arithmetic avoids the whole question.
 *
 * 1.075 == 43/40 EXACTLY. ClockPLL is masked to 0x3FF = 1023 max, so the
 * largest intermediate is 1023 * 20 * 43 = 879,780 — no uint32 overflow.
 * Checked against the float form: 1023*20 = 20460; 20460*43/40 = 21994;
 * trunc(20460 * 1.075) = 21994. Bit-identical.
 */
static ER tof_set_imp(UW imp_ms, UW budget_ms)
{
	uint16_t clk = 0;
	uint32_t imp, rb = 0;

	if (imp_ms < budget_ms)
		return E_PAR;		/* the check ST's API does not make */

	if (VL53L1_RdWord(TOF_DEV8, VL53L1_RESULT__OSC_CALIBRATE_VAL, &clk) != 0)
		return E_IO;

	clk = (uint16_t)(clk & 0x3FFu);
	tstats.osc_cal = (UW)clk;
	if (clk == 0u)
		return E_IO;		/* would write 0 = free-run, silently */

	imp = ((uint32_t)clk * (uint32_t)imp_ms * 43u) / 40u;
	tstats.imp_written = (UW)imp;

	if (VL53L1_WrDWord(TOF_DEV8, VL53L1_SYSTEM__INTERMEASUREMENT_PERIOD, imp) != 0)
		return E_IO;

	/* Readback. A write whose status is checked is still only a claim. */
	if (VL53L1_RdDWord(TOF_DEV8, VL53L1_SYSTEM__INTERMEASUREMENT_PERIOD, &rb) != 0)
		return E_IO;
	tstats.imp_rb = (UW)rb;

	return (rb == imp) ? E_OK : E_IO;
}

/* ------------------------------------------------------------------------ */
/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK                                  */
/* ------------------------------------------------------------------------ */
static ER tof_init_inner(void)
{
	const vl53l1_port_stats_t *ps;
	uint8_t  boot = 0, ready = 0;
	uint16_t id = 0, dm = 0, tb = 0;
	UW i;
	ER err;

	ps = vl53l1_port_stats();

	/* --- 1. BOOT GATE. Bounded, paced, and it fails rather than spins:
	 * an unbounded poll on a wedged part would livelock the whole sensor
	 * pipeline silently (the M-2 lesson from the MPU6050 audit). --- */
	tof_fail(TOF_STEP_BOOT);
	for (i = 0; i < TOF_BOOT_POLL_MAX; i++) {
		tstats.boot_polls = i + 1u;
		if (VL53L1X_BootState(TOF_DEV8, &boot) == 0 && boot != 0u)
			break;
		tk_dly_tsk(1);
	}
	if (boot == 0u)
		return E_TMOUT;

	/* --- 2. IDENTITY. LOG, DO NOT HARD-FAIL on a value mismatch: the
	 * handoff's rule, because ST's own header (0xEEAC) and ST's own
	 * datasheet (0xEACC, DS12385 Table 8) disagree. Only "nothing there"
	 * is a real failure. --- */
	tof_fail(TOF_STEP_ID);
	(void)VL53L1X_GetSensorId(TOF_DEV8, &id);
	tstats.sensor_id = (UW)id;
	if (id == 0x0000u || id == 0xFFFFu)
		return E_NOEXS;

	/* --- 3. THE 91 WRITES. VL53L1X_SensorInit accumulates their status
	 * with |= and then OVERWRITES it at VL53L1X_api.c:190 and :197, so it
	 * can return 0 with any number of them failed (F-2). Its return value
	 * is therefore recorded but NOT trusted: the gate is the shim's own
	 * counters. `calls` must have advanced by at least the 91 WrBytes. --- */
	tof_fail(TOF_STEP_SENSORINIT);
	err = tof_write_config();
	tstats.init_calls    = ps->calls;
	tstats.init_xfer_err = ps->xfer_err;
	if (err != E_OK)
		return err;
	if (tstats.cfg_written != 91u)
		return E_IO;
	/* GATE ON WRITES, NOT ON EVERY TRANSFER. The F-2 defect is about write
	 * status being discarded, so wr_refused -- writes that exhausted every
	 * retry -- is the honest counter. xfer_err also counts read failures,
	 * which are a different fault and must not fail this gate. */
	if (ps->wr_refused != 0u)
		return E_IO;

	/* --- 4. SHORT MODE, THEN READ IT BACK. SetDistanceMode uses six
	 * consecutive plain assignments (VL53L1X_api.c:423-428), so only the
	 * LAST write's status survives — five can fail silently (F-3).
	 * GetDistanceMode leaves *DM untouched if the register matches neither
	 * 0x14 nor 0x0A, so dm is pre-set to a value neither branch produces. --- */
	tof_fail(TOF_STEP_DISTMODE);
	(void)VL53L1X_SetDistanceMode(TOF_DEV8, TOF_DISTANCE_MODE_SHORT);
	dm = 0xFFFFu;
	(void)VL53L1X_GetDistanceMode(TOF_DEV8, &dm);
	tstats.dm_rb = (UW)dm;
	if (dm != TOF_DISTANCE_MODE_SHORT)
		return E_IO;

	/* --- 5. 15 ms BUDGET, THEN READ IT BACK. SetTimingBudgetInMs never
	 * captures its WrWord return values at all (F-4). 15 ms is legal ONLY
	 * in short mode, which is why step 4 comes first. --- */
	tof_fail(TOF_STEP_BUDGET);
	(void)VL53L1X_SetTimingBudgetInMs(TOF_DEV8, (uint16_t)TOF_BUDGET_MS);
	tb = 0xFFFFu;
	(void)VL53L1X_GetTimingBudgetInMs(TOF_DEV8, &tb);
	tstats.tb_rb = (UW)tb;
	if (tb != TOF_BUDGET_MS)
		return E_IO;

	/* --- 6. INTER-MEASUREMENT PERIOD, ours. See tof_set_imp(). --- */
	tof_fail(TOF_STEP_IMP);
	err = tof_set_imp(TOF_IMP_MS, TOF_BUDGET_MS);
	if (err != E_OK)
		return err;

	/* --- 7. START, THEN READ MODE_START BACK.
	 *
	 * D-6. THIS IS THE ONLY GATE IN THIS FILE WHOSE EXPECTED VALUE IS NOT
	 * CITED TO A SPECIFICATION. Every other one compares against a
	 * datasheet value (id 0xEACC, DS12385 Table 8) or a ULD-documented
	 * mapping (dm == 1 via PHASECAL_CONFIG__TIMEOUT_MACROP == 0x14;
	 * tb == 15 via RANGE_CONFIG__TIMEOUT_MACROP_A_HI == 0x001D). This one
	 * compares against WHAT StartRanging WROTE -- WrByte(SYSTEM__MODE_START,
	 * 0x40) at VL53L1X_api.c:240, the register being 0x0087 per
	 * VL53L1X_api.h:66 -- and
	 * [UNVERIFIED] no source in this repo states that the device PRESERVES
	 * that register while ranging rather than self-clearing a start bit.
	 *
	 * VERIFICATION STEP, before this gate is trusted: check the behaviour
	 * of SYSTEM__MODE_START (0x0087) in the VL53L1X register map (ST UM2356
	 * or the ST full API's VL53L1_core.c) and record the answer here.
	 *
	 * READ THIS FIRST IF THE FIRST FLASH REPORTS `step=7`: if `mstart=` is
	 * anything other than 0x40 while [RTY] is all-zero and every earlier
	 * gate held, suspect THIS GATE, not the sensor and not the supply.
	 * Confirm by commenting the comparison out and checking that frames
	 * climb; then fix the expectation rather than the hardware. --- */
	tof_fail(TOF_STEP_START);
	if (VL53L1X_StartRanging(TOF_DEV8) != 0)
		return E_IO;
	{
		uint8_t ms = 0;
		if (VL53L1_RdByte(TOF_DEV8, SYSTEM__MODE_START, &ms) != 0)
			return E_IO;
		tstats.mode_start_rb = (UW)ms;
		if (ms != 0x40u)
			return E_IO;
	}

	/* --- final: nothing may have failed anywhere along the way --- */
	tstats.init_xfer_err = ps->xfer_err;
	if (ps->xfer_err != 0u)
		return E_IO;

	(void)ready;
	tstats.step = TOF_STEP_DONE;
	return E_OK;
}

/*
 * D-5. EVERY FAILURE PATH NOW REPORTS ITS OWN ER.
 *
 * tof_init_inner() has twelve return statements. Only the success path used to
 * touch init_result, so E_TMOUT (boot gate never rose), E_NOEXS (nothing at
 * 0x29), E_PAR (imp_ms < budget_ms -- a CODE bug, not a hardware one) and
 * E_IO (a transfer or a readback mismatch) ALL printed as `init=1`, the value
 * that also means "not run". `step` named which gate stopped; nothing named
 * what kind of failure it was. At step 6 in particular that collapsed a
 * programming error and a bus fault into one indistinguishable line.
 *
 * The wrapper owns the counter reset as well, so the shim counters can never
 * be zeroed by one call path and read by another.
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 */
ER vl53l1x_init(void)
{
	ER err;

	tof_running = FALSE;		/* D-A: no frame loop until THIS run says so */
	memset(&tstats, 0, sizeof(tstats));
	tstats.init_result = 1;		/* 1 = not run; 0 would read as E_OK */
	tstats.min_mm = 0xFFFFFFFFu;

	/* Zero the shim counters FIRST. From here on, `xfer_err` is the only
	 * honest report of whether the writes below actually landed — the
	 * ULD's own return values demonstrably are not (F-2..F-5). */
	vl53l1_port_reset_stats();

	err = tof_init_inner();
	tstats.init_result = err;
	tof_running = (err == E_OK) ? TRUE : FALSE;
	return err;
}

/* ------------------------------------------------------------------------ */
/*
 * ONE FRAME. The order of the three checks below is the whole point.
 *
 * VL53L1X_GetResult (VL53L1X_api.c:587-601) declares `uint8_t Temp[17]` on the
 * stack, ORs the ReadMulti status into `status`, and then uses Temp[0] and
 * Temp[13..14] UNCONDITIONALLY. Our shim correctly does not copy anything out
 * on a failed transfer, so Temp[] keeps whatever the stack held — most likely
 * the PREVIOUS successful frame's 17 bytes, at the same stack depth. A failed
 * read therefore presents as the last good measurement, for ever, silently.
 * And one of the 32 values of Temp[0] & 0x1F (namely 9) maps through
 * status_rtn[] to status 0 = valid.
 *
 * So: transfer status FIRST, shim counters SECOND, result.Status only THIRD.
 * The plan's "gate d(t) on result.Status" is not sufficient on its own (F-6).
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 */
void vl53l1x_service(void)
{
	const vl53l1_port_stats_t *ps = vl53l1_port_stats();
	VL53L1X_Result_t res;
	uint8_t ready = 0;
	UW err_before, err_after;
	int8_t st;

	/* D-A: gate on the explicit success flag, never on a stats field whose
	 * zero value is indistinguishable from E_OK. */
	if (!tof_running)
		return;

	/*
	 * D-4. A FAILED CheckForDataReady IS NOT "DATA NOT READY". These were
	 * one counter. At the 20 ms service cadence against a 20 ms
	 * inter-measurement period, `notready` climbs continuously in perfectly
	 * healthy operation -- so a transfer failure folded into it is
	 * PERMANENTLY INVISIBLE: the call could be failing every single time
	 * and the counter would look exactly like normal polling. The only
	 * other signal would be `frames` going flat, which is also what a
	 * sensor that simply stopped ranging looks like. Separate buckets.
	 */
	if (VL53L1X_CheckForDataReady(TOF_DEV8, &ready) != 0) {
		tstats.drop_ready_err++;
		return;
	}
	if (ready == 0u) {
		tstats.notready++;
		return;
	}

	memset(&res, 0, sizeof(res));
	err_before = ps->xfer_err;
	st = (int8_t)VL53L1X_GetResult(TOF_DEV8, &res);
	err_after  = ps->xfer_err;	/* D-3: snapshot BEFORE ClearInterrupt */

	/*
	 * D-2. ClearInterrupt is MANDATORY every frame (plan v2 step 12) --
	 * including on a dropped frame, or the part never produces another one.
	 * Its status used to be discarded by a `(void)` cast, in a comment that
	 * named that exact consequence. A silent failure here freezes `frames`
	 * while `notready` climbs for ever, which is indistinguishable from a
	 * sensor that stopped ranging by itself. Counted, never silent.
	 */
	if (VL53L1X_ClearInterrupt(TOF_DEV8) != 0)
		tstats.drop_clear++;

	/*
	 * D-3. THE DELTA MUST BRACKET GetResult ALONE. It used to be compared
	 * against ps->xfer_err read HERE, which straddles the ClearInterrupt
	 * above -- so a ClearInterrupt failure discarded a perfectly good
	 * distance reading and charged it to drop_xfer, blaming the wrong call
	 * in both directions.
	 */
	if (st != 0 || err_after != err_before) {
		tstats.drop_xfer++;	/* res is stack garbage — do not read it */
		return;
	}
	if (res.Status != 0u) {
		tstats.drop_status++;
		tstats.last_status = (UW)res.Status;
		return;
	}

	tstats.frames++;
	tstats.last_mm = (UW)res.Distance;
	if (tstats.last_mm < tstats.min_mm)
		tstats.min_mm = tstats.last_mm;
	if (tstats.last_mm > tstats.max_mm)
		tstats.max_mm = tstats.last_mm;
	/* Same accepted-frame read as last_mm above, zero extra I2C cost --
	 * added 2026-09-16, see vl53l1x_stats_t. */
	tstats.ambient = (UW)res.Ambient;
	tstats.sig_per_spad = (UW)res.SigPerSPAD;
}

const vl53l1x_stats_t *vl53l1x_get_stats(void)
{
	return &tstats;
}
