/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_tasks.c — Phase 4 task architecture with synthetic data (Haptic-Sense)
 *
 * Four-task µT-Kernel architecture per CLAUDE.md §3 / Phase 4 design doc.
 * ZERO I2C (DEVCNF_USE_HAL_IIC stays 0), zero NPU — synthetic sensor data
 * exercises the identical buffers, semaphores and task bodies that Phase 5
 * (I2C DMA) and Phase 6 (NeuralART) will swap payloads into.
 *
 * Task inventory (TK_PRI, lower = higher priority):
 *   1  hazard_task     GPIO only. NO I2C, NO printf, NO blocking I/O — ever.
 *   2  inference_task  stub classifier. NO I2C, NO printf.
 *   3  sensor_task     synthetic generator @ 50 Hz. ALL future I2C lives here.
 *  10  heartbeat_task  1 Hz status printer — the ONLY task allowed printf.
 *  15  main_thread     (main.c) creates everything via app_tasks_run(), exits.
 */

/*
 * TASK PRIORITY VERIFICATION (Red Zone #7) — verified 2026-07-05
 *
 * Priority range: 1 (highest) .. 32 (lowest), per CNF_MAX_TSKPRI in
 * Appli/mtk3_bsp2/config/config.h:27.
 *
 * Every tk_cre_tsk() site compiled into this build:
 *   - kernel initial task  TK_PRI  1  (mtk3_bsp2/mtkernel/include/sys/inittask.h:26)
 *       Runs usermain(), then parks PERMANENTLY on tk_slp_tsk(TMO_FEVR) at
 *       main.c:107 — dormant, never runnable again, never preempts.
 *       µT-Kernel allows multiple tasks per priority level, so hazard_task
 *       at TK_PRI 1 coexists safely with this dormant occupant.
 *   - main_thread          TK_PRI 15  (main.c:94) — exits via tk_ext_tsk()
 *   - hazard_task          TK_PRI  1  (this file)
 *   - inference_task       TK_PRI  2  (this file)
 *   - sensor_task          TK_PRI  3  (this file)
 *   - heartbeat_task       TK_PRI 10  (this file)
 *
 * Phase 3's app_heartbeat.c and the reference app.c tasks are EXCLUDED from
 * this build (.cproject Core sourceEntries) and create nothing.
 *
 * Idle behavior: the µT-Kernel STM32 port's low_pow() is an EMPTY function
 * (mtk3_bsp2/sysdepend/stm32_cube/power_save.c:28-30) — idle spins, never
 * enters WFI or Stop mode. The §3 "NEVER Stop mode" constraint holds by
 * construction; there is no deep-sleep wake latency on the hazard path.
 */

#include "app_tasks.h"

#include "app_i2c.h"
#include "app_drv2605l.h"
#include "stm32n6xx_hal.h"
#include "tk/tkernel.h"
#include "tm/tmonitor.h"

/* ------------------------------------------------------------------------ */
/* Configuration                                                             */
/* ------------------------------------------------------------------------ */

#define HAZARD_TK_PRI		1
#define INFERENCE_TK_PRI	2
#define SENSOR_TK_PRI		3
#define HEARTBEAT_TK_PRI	10

#define TASK_STACK_SIZE		4096

#define SENSOR_PERIOD_MS	20	/* 50 Hz frame rate (CLAUDE.md §6) */
#define HEARTBEAT_PERIOD_MS	1000

/* Hazard label rule (CLAUDE.md §6): d < 80 cm AND closing velocity > 20 cm/s */
#define HAZARD_DIST_MM		800
#define HAZARD_VCLOSE_CM_S	20

/*
 * Feature buffer layout — CLAUDE.md §6.
 *
 * !!! SPEC DISCREPANCY (flagged 2026-07-05, resolve before Edge Impulse
 * training): §6 declares "Feature vector (13)" but its own enumeration
 * [d(t)..d(t-9), v, a, ax, ay, az] counts 10+1+1+3 = 15. The 12-feature
 * IMU-dropped fallback (10+2) is consistent with a 10-deep distance history,
 * so "13" is the suspect number. This file implements the ENUMERATION (15
 * slots, macro-driven). Verification step: recount when defining the Edge
 * Impulse impulse, then fix §6's headline count (or the history depth) and
 * this macro together.
 */
#define FEAT_DIST_HIST		10	/* d(t) .. d(t-9), mm  -> [0..9]  */
#define FEAT_IDX_VCLOSE		(FEAT_DIST_HIST)	/* cm/s  -> [10] */
#define FEAT_IDX_ACCEL		(FEAT_DIST_HIST + 1)	/* cm/s² -> [11] */
#define FEAT_IDX_AX		(FEAT_DIST_HIST + 2)	/* mg    -> [12] */
#define FEAT_IDX_AY		(FEAT_DIST_HIST + 3)	/* mg    -> [13] */
#define FEAT_IDX_AZ		(FEAT_DIST_HIST + 4)	/* mg    -> [14] */
#define FEAT_COUNT		(FEAT_DIST_HIST + 5)	/* 15 — see note  */

/* Synthetic approach/retreat scenario (sensor_task) */
#define SYN_D_FAR_MM		2000
#define SYN_D_NEAR_MM		400
#define SYN_STEP_MM		10	/* 10 mm / 20 ms = 50 cm/s closing */

/* data_ready_sem backlog headroom: 1 s of frames — overload canary range */
#define DATA_READY_MAXSEM	50

/* ------------------------------------------------------------------------ */
/* Shared state — all static, no malloc (CLAUDE.md §7)                       */
/* ------------------------------------------------------------------------ */

/*
 * Result slot — single buffer guarded by the paired semaphores (Red Zone #4).
 * seq/seq_check are the torture-test canary: producer stamps seq first and
 * seq_check last; a consumer that ever sees seq != seq_check has read a
 * half-written result, which the paired-semaphore pattern must make
 * impossible.
 */
typedef struct {
	UW	seq;		/* stamped FIRST by producer  */
	W	distance_mm;
	W	v_close_cm_s;	/* + = approaching            */
	UB	hazard;		/* 0/1                        */
	UW	seq_check;	/* stamped LAST: == seq       */
} hazard_result_t;

static hazard_result_t result_slot;

/*
 * Feature buffer — written by sensor_task (P3), read by inference_task (P2).
 * SAFE WITHOUT ITS OWN LOCK because the producer's priority (3) is LOWER
 * than the consumer's (2): sensor_task can never preempt inference_task
 * mid-read, and inference_task only reads after data_ready_sem is signalled.
 * IF THAT PRIORITY RELATION EVER CHANGES, pair this buffer like the result
 * slot. Phase 5's I2C DMA completion path must write through THIS buffer.
 */
static W feature_buf[FEAT_COUNT];

/* Paired semaphores (Red Zone #4) — created BEFORE any tk_sta_tsk */
static ID data_ready_sem;	/* init 0: sensor_task(P)    -> inference_task(C) */
static ID result_free_sem;	/* init 1: hazard_task(C) returns the result slot */
static ID result_ready_sem;	/* init 0: inference_task(P) -> hazard_task(C)    */

/* Task ids + stacks */
static ID hazard_task_id, inference_task_id, sensor_task_id, heartbeat_task_id;
static UB hazard_stack[TASK_STACK_SIZE];
static UB inference_stack[TASK_STACK_SIZE];
static UB sensor_stack[TASK_STACK_SIZE];
static UB heartbeat_stack[TASK_STACK_SIZE];

/* Statistics — one writer per counter; 32-bit aligned reads are atomic on
 * ARMv8-M, so heartbeat_task may read without locks. */
static volatile UW stat_frames;		/* writer: sensor_task    */
static volatile UW stat_dataq_ovr;	/* writer: sensor_task    */
static volatile UW stat_inferences;	/* writer: inference_task */
static volatile UW stat_hazard_events;	/* writer: hazard_task    */
static volatile UW stat_canary_errs;	/* writer: hazard_task    */

/* T1 (2026-08-30) — DWT liveness, written once at init, read by heartbeat.
 * 0 = CYCCNT is NOT counting and every DWT-derived number is void. */
static volatile UW dwt_ok;
static UW hb_prev_cyc;			/* heartbeat-local, TK_PRI 10 only */
static UW hb_prev_ms;
static UW hb_dwt_intervals;		/* completed intervals; <2 = not settled */

#ifdef DEBUG_TIMING
/* DWT corroboration (Red Zone #3). dwt_t0 written by inference_task at
 * D0-set, read by hazard_task at D1-set — strictly ordered by the
 * result_ready_sem handshake. cycles / 600 = µs at 600 MHz. */
static volatile UW dwt_t0;
static volatile UW dwt_dt_min = 0xFFFFFFFFu;	/* lifetime min, cycles */
static volatile UW dwt_dt_max;			/* lifetime max, cycles */
static volatile UW dwt_dt_sum;			/* reset each HB print  */
static volatile UW dwt_dt_cnt;			/* reset each HB print  */
#endif

/*
 * Urgency -> pulse INTERVAL, in ms. TK_PRI 1, pure integer arithmetic.
 * RETUNED 2026-08-30 once HAP-T9 measured the effect at 58.7 ms.
 *
 * The effect CONTENT never changes at runtime (one armed waveform, effect 1);
 * urgency is encoded purely as how often it fires.
 *
 *     v =  20 cm/s (the hazard threshold) -> HAZ_URG_BASE_MS   400 ms
 *     v =  50 cm/s                        ->                   280 ms
 *     v =  80 cm/s                        ->                   160 ms
 *     v = 100 cm/s and above              -> DRV_R3_FLOOR_MS    75 ms
 *
 * WHY THE OLD CURVE WAS WRONG. It anchored the slow end at 1000 ms, so
 * 50 cm/s — a brisk approach, and the only velocity the synthetic generator
 * produces — mapped to 670 ms and yielded TWO pulses across an 828 ms hazard
 * burst (measured: pulses 4->6 per burst). Two buzzes do not read as an
 * alert. Anything that reaches the hazard rule at all (inside 80 cm AND
 * closing faster than 20 cm/s) is already urgent, so the slow anchor belongs
 * near 400 ms, not 1000. At 280 ms the same burst gives 3-4 pulses, and a
 * real ~1.4 s approach gives 5-6.
 *
 * drv2605l_trig_fire() clamps to [DRV_R3_FLOOR_MS, DRV_TRIG_MAX_MS]
 * regardless, so this function cannot violate R-3 even if these constants are
 * later edited. That is the point of enforcing the floor in the driver.
 *
 * STILL TRUE ON THE BENCH: the synthetic generator emits a CONSTANT 50 cm/s
 * (SYN_STEP_MM 10 per 20 ms frame), so this returns a constant 280 ms until
 * real ToF frames arrive in Block 4. Graded urgency is NOT demonstrable on
 * synthetic data — a uniform buzz rate is not a fault.
 */
#define HAZ_URG_BASE_MS		400	/* interval at the hazard threshold */
#define HAZ_URG_SLOPE		4	/* ms shorter per extra cm/s        */

static UW hazard_urgency_interval_ms(W v_cm_s)
{
	W iv;

	if (v_cm_s <= (W)HAZARD_VCLOSE_CM_S)
		return (UW)HAZ_URG_BASE_MS;

	iv = (W)HAZ_URG_BASE_MS -
	     ((v_cm_s - (W)HAZARD_VCLOSE_CM_S) * (W)HAZ_URG_SLOPE);
	if (iv < (W)DRV_R3_FLOOR_MS)
		iv = (W)DRV_R3_FLOOR_MS;
	return (UW)iv;
}

/* ------------------------------------------------------------------------ */
/* hazard_task — TK_PRI 1                                                    */
/* GPIO only. NO I2C, NO printf, NO blocking I/O — ever (CLAUDE.md §3).      */
/* Consumer of result_ready_sem; producer of result_free_sem.                */
/* ------------------------------------------------------------------------ */
static void hazard_task_fct(INT stacd, void *exinf)
{
	hazard_result_t r;

	for (;;) {
		/* consumer: inference_task(P) -> result_ready_sem -> hazard_task(C) */
		tk_wai_sem(result_ready_sem, 1, TMO_FEVR);

#ifdef DEBUG_TIMING
		/* TIMING_D1 (PD6/D7): first action after wake */
		HAL_GPIO_WritePin(GPIOD, GPIO_PIN_6, GPIO_PIN_SET);
		{
			UW dt = DWT->CYCCNT - dwt_t0;	/* UW wrap-safe */
			if (dt < dwt_dt_min) dwt_dt_min = dt;
			if (dt > dwt_dt_max) dwt_dt_max = dt;
			dwt_dt_sum += dt;
			dwt_dt_cnt++;
		}
#endif
		r = result_slot;	/* read the guarded slot */

		/* torture-test canary: must NEVER fire if the paired-semaphore
		 * pattern is correct */
		if (r.seq != r.seq_check)
			stat_canary_errs++;

		/* DRV_TRIG (PE13/D6, CN11 pin 7): ~2 us EDGE per fire, rate
		 * limited by R-3 inside the driver (T2, 2026-08-30 — replaces
		 * the Phase 4 hazard LEVEL placeholder).
		 * Priority-1 task touches GPIO ONLY (CLAUDE.md §2); all
		 * DRV2605L I2C configuration happens at init from TK_PRI 3.
		 * TRIG is not wired to the breakout yet — T3 step 3. */
		if (r.hazard) {
			(void)drv2605l_trig_fire(
				hazard_urgency_interval_ms(r.v_close_cm_s));
			stat_hazard_events++;
		}

		/* producer: hazard_task(P) -> result_free_sem -> inference_task(C) */
		tk_sig_sem(result_free_sem, 1);

#ifdef DEBUG_TIMING
		/* D1 low before next wait so every event has a fresh rising edge */
		HAL_GPIO_WritePin(GPIOD, GPIO_PIN_6, GPIO_PIN_RESET);
#endif
	}
}

/* ------------------------------------------------------------------------ */
/* inference_task — TK_PRI 2                                                 */
/* NO I2C, NO printf (CLAUDE.md §3). Stub classifier = CLAUDE.md §6 label    */
/* rule as code; Phase 6 replaces ONLY the classify step with NeuralART —    */
/* buffers, semaphores and ordering stay identical.                          */
/* Consumer of data_ready_sem + result_free_sem; producer of result_ready_sem*/
/* ------------------------------------------------------------------------ */
static void inference_task_fct(INT stacd, void *exinf)
{
	W d_mm, v_cm_s;
	UB hazard;
	UW seq = 0;

	for (;;) {
		/* consumer: sensor_task(P) -> data_ready_sem -> inference_task(C) */
		tk_wai_sem(data_ready_sem, 1, TMO_FEVR);

		/* stub classifier — hazard = (d < 800 mm && v_close > 20 cm/s) */
		d_mm   = feature_buf[0];		/* newest distance */
		v_cm_s = feature_buf[FEAT_IDX_VCLOSE];
		hazard = (d_mm < HAZARD_DIST_MM && v_cm_s > HAZARD_VCLOSE_CM_S) ? 1 : 0;

		/* paired-semaphore handshake (Red Zone #4):
		 * consumer: hazard_task(P) -> result_free_sem -> inference_task(C) */
		tk_wai_sem(result_free_sem, 1, TMO_FEVR);

		seq++;
		result_slot.seq          = seq;	/* canary: stamped FIRST */
		result_slot.distance_mm  = d_mm;
		result_slot.v_close_cm_s = v_cm_s;
		result_slot.hazard       = hazard;
		result_slot.seq_check    = seq;	/* canary: stamped LAST  */

		stat_inferences++;

#ifdef DEBUG_TIMING
		/* TIMING_D0 (PH5/D4): set at the hazard-signal instant.
		 * tk_sig_sem below wakes hazard_task (TK_PRI 1 > ours) which
		 * preempts IMMEDIATELY — D0-low executes only after hazard_task
		 * has finished and blocked again. Δt(D0up -> D1up) on the LA =
		 * preemption + context-switch latency. */
		HAL_GPIO_WritePin(GPIOH, GPIO_PIN_5, GPIO_PIN_SET);
		dwt_t0 = DWT->CYCCNT;
#endif
		/* producer: inference_task(P) -> result_ready_sem -> hazard_task(C) */
		tk_sig_sem(result_ready_sem, 1);
#ifdef DEBUG_TIMING
		HAL_GPIO_WritePin(GPIOH, GPIO_PIN_5, GPIO_PIN_RESET);
#endif
	}
}

/* ------------------------------------------------------------------------ */
/* sensor_task — TK_PRI 3                                                    */
/* ALL future I2C lives here, DMA only (CLAUDE.md §3). Phase 4 body is a     */
/* synthetic approach/retreat generator writing through the IDENTICAL        */
/* feature buffer the Phase 5 DMA path will use.                             */
/* Producer of data_ready_sem.                                               */
/* ------------------------------------------------------------------------ */

/*
 * Fill one 50 Hz frame into feature_buf from the synthetic scenario.
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 * (Phase 5: this is exactly where the VL53L1X/MPU6050 DMA results land.)
 */
static void sensor_fill_frame(void)
{
	static W d_mm = SYN_D_FAR_MM;
	static W dir = -1;		/* -1 = approaching, +1 = retreating */
	static W v_prev_cm_s = 0;
	W v_cm_s, a_cm_s2, d_prev;
	INT i;

	d_prev = d_mm;
	d_mm += dir * SYN_STEP_MM;
	if (d_mm <= SYN_D_NEAR_MM) dir = +1;	/* turn around, retreat  */
	if (d_mm >= SYN_D_FAR_MM)  dir = -1;	/* turn around, approach */

	/* closing velocity: (Δmm per 20 ms) * 50 frames/s = mm/s; /10 = cm/s.
	 * SYN_STEP_MM=10 -> 50 cm/s while approaching (> 20 cm/s threshold). */
	v_cm_s = (d_prev - d_mm) * 5;
	/* acceleration: Δ(cm/s) per frame * 50 = cm/s² (nonzero at turnarounds) */
	a_cm_s2 = (v_cm_s - v_prev_cm_s) * 50;
	v_prev_cm_s = v_cm_s;

	/* shift distance history: [0] newest .. [9] oldest */
	for (i = FEAT_DIST_HIST - 1; i > 0; i--)
		feature_buf[i] = feature_buf[i - 1];
	feature_buf[0] = d_mm;

	feature_buf[FEAT_IDX_VCLOSE] = v_cm_s;
	feature_buf[FEAT_IDX_ACCEL]  = a_cm_s2;
	feature_buf[FEAT_IDX_AX] = 0;		/* mg — synthetic IMU at rest */
	feature_buf[FEAT_IDX_AY] = 0;
	feature_buf[FEAT_IDX_AZ] = 1000;	/* 1 g */
}

/* Phase 5 L1 bring-up state — writer: sensor_task; reader: heartbeat_task */
static volatile W i2c_init_result = 1;	/* 1 = not yet run; E_OK/E_xx after */

static void sensor_task_fct(INT stacd, void *exinf)
{
	ER err;
	UW sensor_poll_div = 0u;

	/* Phase 5 MANDATORY GATE (L1 design doc, review 2026-07-06): init I2C1
	 * + prove DMA moves bytes with ONE register read, in isolation, BEFORE
	 * any L2/ULD code exists. Failure is captured, not fatal — the synthetic
	 * pipeline below keeps running either way (bounded worst case ~290 ms:
	 * 2 addresses x ~145 ms per-transfer worst case incl. recovery+retry).
	 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
	/* EN must be high BEFORE any register access to 0x5A: with EN low the
	 * DRV2605L ACKs its address but permits no read or write (SLOS854D
	 * §8.4.1.3), which presents as a healthy bus returning garbage.
	 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
	(void)drv2605l_power_up();

	i2c_init_result = (W)app_i2c_init();
	if (i2c_init_result == E_OK) {
		app_i2c_gate_test();

		/* Block 1a: configure and arm the DRV2605L. Writes registers
		 * only — no GO bit, no diagnostics — so it is safe with nothing
		 * connected to OUT+/OUT-. Result lands in drv2605l_get_stats().
		 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
		(void)drv2605l_init();
	}

	for (;;) {
		sensor_fill_frame();

		/* Cache maintenance (CLAUDE.md §3) — placed NOW, exercised in
		 * Phase 5. Harmless on CPU-written synthetic data; MANDATORY on
		 * the I2C DMA path. Kept from day 1 so Phase 5 cannot forget it.
		 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
		SCB_InvalidateDCache_by_Addr((uint32_t *)feature_buf, sizeof(feature_buf));

		/* producer: sensor_task(P) -> data_ready_sem -> inference_task(C).
		 * Counting semaphore: backlog growth = inference overload canary
		 * (heartbeat_task reports the count via tk_ref_sem). */
		err = tk_sig_sem(data_ready_sem, 1);
		if (err == E_QOVR)
			stat_dataq_ovr++;	/* queue full: frame dropped */

		stat_frames++;

		/* HAP-T9 (H-D3): measure real effect-1 duration by timing the
		 * GO bit. No-op except in the window after one of the first few
		 * TRIG pulses, and permanently silent after 5 attempts. Rides
		 * on hazard pulses that fire anyway, so it adds no buzz.
		 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
		drv2605l_measure_service();

		/* DRV2605L health + config validity, ~1 Hz. TK_PRI 3 owns all
		 * I2C (CLAUDE.md §3), so this is the only context it may run
		 * in. Two 1-byte register reads via DMA, ~250 us total once per
		 * 50 frames — well inside the 20 ms frame budget.
		 *
		 * MANDATORY BEFORE THE MOTOR IS CONNECTED (T3): OC_DETECT and
		 * OVER_TEMP latch and then CLEAR ON READ, so without this poll
		 * a driver fault is unobservable and a weak or silent motor has
		 * no distinguishing evidence.
		 *
		 * COUNTING NOTE: this makes the [I2C] ok= counter grow by 2 per
		 * second for ever. ok is no longer a fixed 24 — it is
		 * 24 + 2*polls, and [DRV] prints polls so the two can be
		 * reconciled. A stalled ok= now means a stalled sensor task. */
		if (++sensor_poll_div >= (1000u / SENSOR_PERIOD_MS)) {
			sensor_poll_div = 0u;
			drv2605l_poll();
		}

		tk_slp_tsk(SENSOR_PERIOD_MS);	/* 50 Hz frame pacing */
	}
}

/* ------------------------------------------------------------------------ */
/* heartbeat_task — TK_PRI 10                                                */
/* Demoted from Phase 3 to slow status printer. The ONLY task allowed printf.*/
/* ------------------------------------------------------------------------ */
static void heartbeat_task_fct(INT stacd, void *exinf)
{
	SYSTIM tim;
	T_RSEM rsem;
	INT backlog;

	for (;;) {
		HAL_GPIO_TogglePin(GPIOO, GPIO_PIN_1);	/* LD1 liveness, pin-masked */

		tk_get_otm(&tim);
		backlog = (tk_ref_sem(data_ready_sem, &rsem) == E_OK) ? rsem.semcnt : -1;

		tm_printf((UB *)"[HB] up_ms=%u frames=%u inf=%u hazard=%u canary_err=%u q=%d qovr=%u\n",
			  tim.lo, stat_frames, stat_inferences, stat_hazard_events,
			  stat_canary_errs, backlog, stat_dataq_ovr);

		{	/* Phase 5 L1 gate + clock verification (design doc §6) */
			const app_i2c_stats_t *s = app_i2c_stats();
			tm_printf((UB *)"[I2C] init=%d gate=%d whoami=0x%x wr=%d wrseen=0x%x addr=0x%x ok=%u err=%u tmo=%u recov=%u pclk1=%u sysclk=%u\n",
				  (INT)i2c_init_result, (INT)s->gate_result,
				  s->gate_whoami, (INT)s->gate_wr, s->gate_wr_seen,
				  s->gate_addr,
				  s->xfer_ok, s->xfer_err, s->timeouts, s->recoveries,
				  s->clk_pclk1_hz, s->clk_sysclk_hz);
			tm_printf((UB *)"[CLK] cpu=%u sysb=%u pclk1=%u\n",
				  s->clk_cpu_hz, s->clk_sysclk_hz,
				  s->clk_pclk1_hz);
		}

		{	/* Block 1a: DRV2605L configuration + arming readback */
			const drv2605l_stats_t *d = drv2605l_get_stats();
			tm_printf((UB *)"[DRV] init=%d id=%u mode=0x%x lib=0x%x seq=0x%x odc=0x%x sts=0x%x armed=%u\n",
				  (INT)d->init_result, d->device_id,
				  d->mode_rb, d->lib_rb, d->seq_rb,
				  d->odc_rb, d->status_rb, d->armed);
			/* sts= is a SNAPSHOT taken once in drv2605l_init(), not a
			 * live read — OC_DETECT/OVER_TEMP clear on read and this
			 * value never updates. A runtime poll is still owed
			 * before the motor is connected (T3). */
			tm_printf((UB *)"[TRG] pulses=%u suppressed=%u cyc=%u rst=%u rstmode=0x%x\n",
				  d->pulses, d->suppressed, d->pulse_cycles,
				  d->rst_polls, d->rst_mode);
			/* faults= is STICKY: OVER_TEMP|OC_DETECT latch and clear
			 * on read, so a nonzero value means a fault happened at
			 * some point, not that one is happening now. Nonzero at
			 * any time after the motor is connected is a STOP. */
			tm_printf((UB *)"[HLT] polls=%u faults=0x%x cfglost=%u\n",
				  d->polls, d->faults_seen, d->cfg_lost);
			/* HAP-T9: effect-1 playback duration, microseconds.
			 * Predicted 45000-75000 (SLOS854D Table 1, Library B:
			 * rise 40-60 ms + brake 5-15 ms). R-3 floor = max x 1.2.
			 * late/stuck are DISCARDED samples, not measurements. */
			if (d->eff_n > 0u || d->eff_late > 0u || d->eff_stuck > 0u)
				tm_printf((UB *)"[EFF] n=%u last=%u min=%u max=%u late=%u stuck=%u\n",
					  d->eff_n, d->eff_last_us,
					  d->eff_min_us, d->eff_max_us,
					  d->eff_late, d->eff_stuck);
		}

		{	/*
			 * T1 / G-8 — WHICH CLOCK FEEDS DWT->CYCCNT.
			 *
			 * cyc_per_ms is computed here rather than by hand from
			 * two heartbeat lines, so the answer cannot be an
			 * arithmetic slip at 1 a.m.
			 *
			 * PREDICTED 800000. Derived, not guessed, from
			 * main.c:206-212 and :249-255 with HSI_VALUE = 64 MHz
			 * (stm32n6xx_hal_conf.h:130):
			 *   PLL1 = 64/PLLM 2 * PLLN 25 / P1 1 / P2 1 = 800 MHz
			 *   CPUCLK  = IC1 <- PLL1 / divider 1 = 800 MHz
			 *   sysb_ck = IC2 <- PLL1 / divider 2 = 400 MHz
			 * DWT->CYCCNT counts the PROCESSOR clock, i.e. CPUCLK.
			 * The 400 MHz the board prints as "sysclk" is IC2, a
			 * DIFFERENT clock tree, and is correct for what it is.
			 * CLAUDE.md §3's /600000 is wrong by 800/600 = 1.333x.
			 *
			 * WRAP: CYCCNT is 32-bit, so it wraps every 2^32/800e6
			 * = 5.37 s. HEARTBEAT_PERIOD_MS is 1000, so the UW
			 * subtraction below is wrap-safe. IF THE HEARTBEAT
			 * PERIOD IS EVER RAISED ABOVE ~5 s THIS NUMBER SILENTLY
			 * BECOMES GARBAGE.
			 */
			UW cyc  = DWT->CYCCNT;
			UW dms  = tim.lo - hb_prev_ms;
			UW cpms = (dms != 0u) ? ((cyc - hb_prev_cyc) / dms) : 0u;

			/*
			 * PRINT 0 UNTIL TWO INTERVALS HAVE COMPLETED.
			 *
			 * The first reading divides by an uptime that started
			 * before the DWT was zeroed in app_gpio_init(), and the
			 * second spans the boot window, where up_ms advanced
			 * 1148 ms against only 1035 ms of CPU cycles (measured
			 * 2026-08-30: 602178 then 721524, settling to
			 * 799999 / 800293 / 799998 thereafter -- 4 ppm).
			 *
			 * THAT 113 ms DISCREPANCY IS NOT EXPLAINED. It is
			 * confined to the boot interval and steady state is
			 * exact, so it does not affect the result, but it is
			 * recorded rather than rationalised. Test that would
			 * settle it: toggle a GPIO on each heartbeat and compare
			 * kernel up_ms against the logic analyzer across the
			 * first three seconds.
			 *
			 * Suppressed because these logs become Block 9 archive
			 * material and a stray "602178" is a number somebody
			 * could later quote as a measurement. 0 = not settled.
			 */
			if (hb_dwt_intervals < 2u) {
				hb_dwt_intervals++;
				cpms = 0u;
			}

			tm_printf((UB *)"[DWT] ok=%u cyc=%u cyc_per_ms=%u\n",
				  dwt_ok, cyc, cpms);

			hb_prev_cyc = cyc;
			hb_prev_ms  = tim.lo;
		}

#ifdef DEBUG_TIMING
		if (dwt_dt_cnt > 0) {
			/* cycles / 600 = µs at 600 MHz (CLAUDE.md §3) */
			tm_printf((UB *)"[HB] dt_us min=%u max=%u mean=%u n=%u\n",
				  dwt_dt_min / 600u, dwt_dt_max / 600u,
				  (dwt_dt_sum / dwt_dt_cnt) / 600u, dwt_dt_cnt);
			dwt_dt_sum = 0;		/* per-interval mean; min/max lifetime */
			dwt_dt_cnt = 0;
		}
#ifdef DEBUG_CHATTER
		/* Load generation for the measurement campaign (§5.3): burn ~800 ms
		 * of each cycle at TK_PRI 10 so priority-1 wakeups must genuinely
		 * preempt running code. OFF by default — campaign only. */
		{
			SYSTIM t0, tn;
			tk_get_otm(&t0);
			do {
				tk_get_otm(&tn);
			} while ((tn.lo - t0.lo) < 800u);
			tk_slp_tsk(HEARTBEAT_PERIOD_MS - 800u);
			continue;
		}
#endif
#endif
		tk_slp_tsk(HEARTBEAT_PERIOD_MS);
	}
}

/* ------------------------------------------------------------------------ */
/* Initialization — runs in main_thread context (TK_PRI 15, main.c)          */
/* ------------------------------------------------------------------------ */

static void app_gpio_init(void)
{
	GPIO_InitTypeDef gpio_init = {0};

	gpio_init.Mode = GPIO_MODE_OUTPUT_PP;
	gpio_init.Pull = GPIO_NOPULL;
	gpio_init.Speed = GPIO_SPEED_FREQ_LOW;

	/* LD1 (PO1, active HIGH). Port O carries XSPI1 on PO0/PO2/PO3/PO4 —
	 * ONLY PO1, pin-masked calls only (hardware-confirmed 2026-07-05,
	 * CLAUDE.md §2). */
	__HAL_RCC_GPIOO_CLK_ENABLE();
	gpio_init.Pin = GPIO_PIN_1;
	HAL_GPIO_Init(GPIOO, &gpio_init);
	HAL_GPIO_WritePin(GPIOO, GPIO_PIN_1, GPIO_PIN_RESET);

	/* DRV_EN (PE7/D8, CN12 pin 1) and DRV_TRIG (PE13/D6, CN11 pin 7) are
	 * owned entirely by app_drv2605l.c — both configured push-pull and
	 * driven LOW before they become outputs. Do not configure either pin
	 * here; abs max on both tracks the breakout's VDD (CLAUDE.md §2). */
	drv2605l_gpio_init();

#ifdef DEBUG_TIMING
	/* TIMING_D0 = PH5 (D4), TIMING_D1 = PD6 (D7) — CLAUDE.md §2/§3 */
	__HAL_RCC_GPIOH_CLK_ENABLE();
	gpio_init.Pin = GPIO_PIN_5;
	HAL_GPIO_Init(GPIOH, &gpio_init);
	HAL_GPIO_WritePin(GPIOH, GPIO_PIN_5, GPIO_PIN_RESET);

	__HAL_RCC_GPIOD_CLK_ENABLE();
	gpio_init.Pin = GPIO_PIN_6;
	HAL_GPIO_Init(GPIOD, &gpio_init);
	HAL_GPIO_WritePin(GPIOD, GPIO_PIN_6, GPIO_PIN_RESET);

#endif	/* DEBUG_TIMING */

	/*
	 * DWT CYCLE COUNTER — ENABLED UNCONDITIONALLY (T1, 2026-08-30).
	 *
	 * These three lines were inside #ifdef DEBUG_TIMING, which is NOT
	 * defined in this build. So outside the Phase 4 campaign DWT->CYCCNT
	 * has never run, and dwt_spin_cycles() — which T2 needs for the
	 * DRV2605L trigger pulse — could not work. Leaving the counter on
	 * costs nothing; it is a free-running counter with no interrupt.
	 *
	 * CoreDebug / CoreDebug_DEMCR_TRCENA_Msk are marked \deprecated in
	 * this CMSIS (core_cm55.h:3194, 3620) in favour of DCB /
	 * DCB_DEMCR_TRCENA_Msk. Both name the same register; the deprecated
	 * spelling is kept DELIBERATELY because it is the exact sequence that
	 * produced the Phase 4 RZ3 evidence and this build is frozen 18 Sep.
	 *
	 * LOUD FAILURE CHECK, replacing the old "VERIFY it advances" comment:
	 *   - NOCYCCNT (core_cm55.h:1314) reads 1 when the cycle counter is
	 *     NOT IMPLEMENTED on this part.
	 *   - A counter that reads the same value twice after CYCCNTENA is set
	 *     means the enable was REJECTED. Prime suspect on this target is
	 *     secure non-invasive debug being disabled under TZEN — the FSBL
	 *     owns that, not us.
	 * Either way dwt_ok goes to 0 and the heartbeat says so, instead of
	 * the firmware printing plausible zeros for ever.
	 */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

	if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0u) {
		dwt_ok = 0u;		/* cycle counter not implemented */
	} else {
		UW c0, c1;
		c0 = DWT->CYCCNT;
		__NOP(); __NOP(); __NOP(); __NOP();
		__NOP(); __NOP(); __NOP(); __NOP();
		c1 = DWT->CYCCNT;
		dwt_ok = (c1 != c0) ? 1u : 0u;
	}
}

/*
 * Entry point — called from main_thread_fct() (main.c, TK_PRI 15).
 * ORDER MATTERS: semaphores are created BEFORE any tk_sta_tsk of tasks 1-3
 * (Phase 4 design §4.1 / pre-implementation checklist).
 */
void app_tasks_run(void)
{
	T_CSEM sem_config;
	T_CTSK task_config;

	/* --- 1. Paired semaphores (Red Zone #4) — FIRST --- */
	sem_config.sematr = TA_TFIFO;
	sem_config.exinf = NULL;

	/* sensor_task(P) -> inference_task(C); counting, backlog = overload canary */
	sem_config.isemcnt = 0;
	sem_config.maxsem = DATA_READY_MAXSEM;
	data_ready_sem = tk_cre_sem(&sem_config);

	/* hazard_task(C) returns the slot; starts free */
	sem_config.isemcnt = 1;
	sem_config.maxsem = 1;
	result_free_sem = tk_cre_sem(&sem_config);

	/* inference_task(P) -> hazard_task(C); starts empty */
	sem_config.isemcnt = 0;
	sem_config.maxsem = 1;
	result_ready_sem = tk_cre_sem(&sem_config);

	if (data_ready_sem <= E_OK || result_free_sem <= E_OK || result_ready_sem <= E_OK) {
		tm_printf((UB *)"[TASKS] FATAL: tk_cre_sem failed (%d %d %d)\n",
			  (INT)data_ready_sem, (INT)result_free_sem, (INT)result_ready_sem);
		return;
	}

	/* --- 2. GPIO + instrumentation --- */
	app_gpio_init();

	/* --- 3. Tasks, created+started highest priority first: each blocks on
	 * its consumer semaphore immediately, so nothing runs until sensor_task
	 * produces the first frame --- */
	task_config.tskatr = TA_HLNG | TA_RNG3 | TA_USERBUF;
	task_config.stksz = TASK_STACK_SIZE;
	task_config.exinf = NULL;

	task_config.task = hazard_task_fct;		/* TK_PRI 1 */
	task_config.itskpri = HAZARD_TK_PRI;
	task_config.bufptr = hazard_stack;
	hazard_task_id = tk_cre_tsk(&task_config);

	task_config.task = inference_task_fct;		/* TK_PRI 2 */
	task_config.itskpri = INFERENCE_TK_PRI;
	task_config.bufptr = inference_stack;
	inference_task_id = tk_cre_tsk(&task_config);

	task_config.task = sensor_task_fct;		/* TK_PRI 3 */
	task_config.itskpri = SENSOR_TK_PRI;
	task_config.bufptr = sensor_stack;
	sensor_task_id = tk_cre_tsk(&task_config);

	task_config.task = heartbeat_task_fct;		/* TK_PRI 10 */
	task_config.itskpri = HEARTBEAT_TK_PRI;
	task_config.bufptr = heartbeat_stack;
	heartbeat_task_id = tk_cre_tsk(&task_config);

	if (hazard_task_id <= E_OK || inference_task_id <= E_OK ||
	    sensor_task_id <= E_OK || heartbeat_task_id <= E_OK) {
		tm_printf((UB *)"[TASKS] FATAL: tk_cre_tsk failed (%d %d %d %d)\n",
			  (INT)hazard_task_id, (INT)inference_task_id,
			  (INT)sensor_task_id, (INT)heartbeat_task_id);
		return;
	}

	tk_sta_tsk(hazard_task_id, 0);
	tk_sta_tsk(inference_task_id, 0);
	tk_sta_tsk(sensor_task_id, 0);
	tk_sta_tsk(heartbeat_task_id, 0);

	tm_printf((UB *)"[TASKS] phase4 architecture up: hazard=1 inference=2 sensor=3 heartbeat=10\n");
}
