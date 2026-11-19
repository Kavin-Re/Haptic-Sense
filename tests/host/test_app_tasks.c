/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for the pure decision logic inside
 * firmware/Appli/Core/Src/app_tasks.c:
 *
 *   - hazard_urgency_interval_ms(): closing speed -> haptic pulse interval
 *   - norm1000(): feature normalisation to milli-units
 *   - inference_task: the hazard decision = model OR (d < 80 cm AND
 *     v > 20 cm/s), and the seq/seq_check canary stamping
 *   - hazard_task: canary check and the TRIG request it issues
 *   - sensor_task / sensor_fill_frame(): feature-frame assembly, the
 *     held-sample rules and the FRESH/HELD/INVALID tri-state
 *
 * HOW: app_tasks.c is #included UNMODIFIED into this translation unit, built
 * with -DHAZARD_CLASSIFIER_USE_NPU (the shipped configuration) against the
 * stub headers in stubs/. Every kernel, HAL and driver call it makes is
 * implemented below. The task bodies are infinite loops, so the blocking
 * stubs (tk_wai_sem / tk_slp_tsk) longjmp back to the test after a set
 * number of iterations.
 *
 * WHAT THIS IS NOT: there is no scheduler here. Preemption, priorities and
 * the paired-semaphore handshake under real concurrency are verified on
 * hardware (docs/evidence/), not by these tests.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "minunit.h"

#include "app_tasks.c"	/* firmware source, as-is */

/* ------------------------------------------------------------------------ */
/* Stub state                                                                */
/* ------------------------------------------------------------------------ */

GPIO_TypeDef host_gpiod, host_gpioh, host_gpioo;
host_coredebug_t host_coredebug;
host_dwt_t host_dwt;

#define MAX_IDS 32
static jmp_buf g_env;
static int g_wait_budget[MAX_IDS];	/* waits allowed before bailing out   */
static int g_wait_count[MAX_IDS];
static int g_sig_count[MAX_IDS];
static ER  g_sig_result[MAX_IDS];
static int g_sleep_budget;
static UW  g_now_ms;
static int g_next_id = 1;

#define MAX_EVENTS 64
static char g_events[MAX_EVENTS][24];	/* call trace for ordering checks      */
static int g_n_events;

static struct {
	void (*fn)(INT, void *);
	PRI pri;
	void *stack;
	SZ stksz;
} g_tasks[8];
static int g_n_tasks, g_n_sems_at_first_task, g_n_started;

static char g_last_line[512];
static char g_last_csv[512];
static int g_csv_lines;

/* driver stubs */
static vl53l1x_stats_t g_tof;
static mpu6050_stats_t g_imu;
static drv2605l_stats_t g_drv;
static app_i2c_stats_t g_i2c;
static vl53l1_port_stats_t g_port;
static int g_trig_calls, g_poll_calls, g_measure_calls, g_npu_init_calls;
static UW g_trig_last_interval;
static UB g_model_bit;
static W g_model_last_in[HAZ_FEAT_COUNT];
static int g_model_calls;

static void event(const char *what, int id)
{
	if (g_n_events < MAX_EVENTS)
		snprintf(g_events[g_n_events++], sizeof(g_events[0]), "%s:%d", what, id);
}

static void reset_trace(void)
{
	g_n_events = 0;
	memset(g_wait_count, 0, sizeof(g_wait_count));
	memset(g_sig_count, 0, sizeof(g_sig_count));
}

/* --- kernel -------------------------------------------------------------- */
ID tk_cre_sem(const T_CSEM *pk_csem) { (void)pk_csem; return g_next_id++; }

ER tk_wai_sem(ID semid, INT cnt, TMO tmout)
{
	(void)cnt; (void)tmout;
	if (g_wait_budget[semid] <= 0)
		longjmp(g_env, 1);
	g_wait_budget[semid]--;
	g_wait_count[semid]++;
	event("wai", semid);
	return E_OK;
}

ER tk_sig_sem(ID semid, INT cnt)
{
	(void)cnt;
	g_sig_count[semid]++;
	event("sig", semid);
	return g_sig_result[semid];
}

ER tk_ref_sem(ID semid, T_RSEM *pk_rsem) { (void)semid; pk_rsem->semcnt = 0; return E_OK; }

ID tk_cre_tsk(const T_CTSK *pk_ctsk)
{
	if (g_n_tasks == 0)
		g_n_sems_at_first_task = g_next_id - 1;
	g_tasks[g_n_tasks].fn = pk_ctsk->task;
	g_tasks[g_n_tasks].pri = pk_ctsk->itskpri;
	g_tasks[g_n_tasks].stack = pk_ctsk->bufptr;
	g_tasks[g_n_tasks].stksz = pk_ctsk->stksz;
	g_n_tasks++;
	return g_next_id++;
}

ER tk_sta_tsk(ID tskid, INT stacd) { (void)tskid; (void)stacd; g_n_started++; return E_OK; }

ER tk_slp_tsk(TMO tmout)
{
	(void)tmout;
	if (--g_sleep_budget <= 0)
		longjmp(g_env, 1);
	return E_OK;
}

ER tk_dly_tsk(RELTIM dlytim) { (void)dlytim; return E_OK; }
ER tk_get_otm(SYSTIM *pk_tim) { pk_tim->hi = 0; pk_tim->lo = g_now_ms; return E_OK; }

INT tm_printf(const UB *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vsnprintf(g_last_line, sizeof(g_last_line), (const char *)format, ap);
	va_end(ap);
	if (strncmp(g_last_line, "[CSV]", 5) == 0) {
		g_csv_lines++;
		memcpy(g_last_csv, g_last_line, sizeof(g_last_csv));
	}
	return 0;
}

/* --- HAL ----------------------------------------------------------------- */
void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init) { (void)port; (void)init; }
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState s) { (void)port; (void)pin; (void)s; }
void HAL_GPIO_TogglePin(GPIO_TypeDef *port, uint16_t pin) { (void)port; (void)pin; }
uint32_t HAL_RCC_GetCpuClockFreq(void) { return 800000000u; }

/* --- drivers ------------------------------------------------------------- */
void drv2605l_gpio_init(void) {}
ER drv2605l_power_up(void) { return E_OK; }
ER drv2605l_init(void) { return E_OK; }
void drv2605l_poll(void) { g_poll_calls++; }
void drv2605l_measure_service(void) { g_measure_calls++; }
const drv2605l_stats_t *drv2605l_get_stats(void) { return &g_drv; }
BOOL drv2605l_trig_fire(UW want_interval_ms)
{
	g_trig_calls++;
	g_trig_last_interval = want_interval_ms;
	return TRUE;
}

ER app_i2c_init(void) { return E_OK; }
void app_i2c_gate_test(void) {}
void app_i2c_tof_probe(void) {}
const app_i2c_stats_t *app_i2c_stats(void) { return &g_i2c; }

ER vl53l1x_init(void) { return E_OK; }
void vl53l1x_service(void) {}
const vl53l1x_stats_t *vl53l1x_get_stats(void) { return &g_tof; }
const vl53l1_port_stats_t *vl53l1_port_stats(void) { return &g_port; }

void mpu6050_gpio_init(void) {}
ER mpu6050_init(void) { return E_OK; }
void mpu6050_service(void) {}
const mpu6050_stats_t *mpu6050_get_stats(void) { return &g_imu; }

void hazard_npu_init(void) { g_npu_init_calls++; }
uint8_t hazard_classify_npu(const int32_t norm_feat[HAZ_FEAT_COUNT], hazard_npu_diag_t *diag)
{
	if (diag != NULL)
		memset(diag, 0, sizeof(*diag));
	memcpy(g_model_last_in, norm_feat, sizeof(g_model_last_in));
	g_model_calls++;
	return g_model_bit;
}

/* ------------------------------------------------------------------------ */
/* Helpers: run exactly N iterations of a task body                          */
/* ------------------------------------------------------------------------ */

static void run_inference(int iterations)
{
	g_wait_budget[data_ready_sem] = iterations;
	g_wait_budget[result_free_sem] = iterations;
	if (setjmp(g_env) == 0)
		inference_task_fct(0, NULL);
}

static void run_hazard(int iterations)
{
	g_wait_budget[result_ready_sem] = iterations;
	if (setjmp(g_env) == 0)
		hazard_task_fct(0, NULL);
}

static void run_sensor(int frames)
{
	g_sleep_budget = frames;
	if (setjmp(g_env) == 0)
		sensor_task_fct(0, NULL);
}

/* One accepted ToF frame of `mm` at time `t_ms`, then one sensor cycle. */
static void tof_frame(UW mm, UW t_ms)
{
	g_tof.init_result = E_OK;
	g_tof.frames++;
	g_tof.last_mm = mm;
	g_now_ms = t_ms;
	run_sensor(1);
}

/* One sensor cycle in which the ToF produced nothing new. */
static void tof_no_frame(UW t_ms)
{
	g_now_ms = t_ms;
	run_sensor(1);
}

static UB decide(W d_mm, W v_cm_s, UB model_bit)
{
	feature_buf[0] = d_mm;
	feature_buf[FEAT_IDX_VCLOSE] = v_cm_s;
	g_model_bit = model_bit;
	run_inference(1);
	return result_slot.hazard;
}

/* ------------------------------------------------------------------------ */
/* Tests                                                                     */
/* ------------------------------------------------------------------------ */

static void test_startup_creates_semaphores_before_tasks(void)
{
	/* app_tasks_run() was called once from main() before any test. */
	CHECK_EQ(g_n_sems_at_first_task, 3);
	CHECK(data_ready_sem > 0 && result_free_sem > 0 && result_ready_sem > 0);
	CHECK(data_ready_sem != result_free_sem);
	CHECK(result_free_sem != result_ready_sem);
	CHECK_EQ(g_n_tasks, 4);
	CHECK_EQ(g_n_started, 4);
	CHECK_EQ(g_npu_init_calls, 1);	/* NPU initialised before tasks start */
}

static void test_task_priorities_match_documented_table(void)
{
	/* README / CLAUDE.md §3: hazard 1, inference 2, sensor 3, heartbeat 10. */
	CHECK(g_tasks[0].fn == hazard_task_fct);
	CHECK_EQ(g_tasks[0].pri, 1);
	CHECK(g_tasks[1].fn == inference_task_fct);
	CHECK_EQ(g_tasks[1].pri, 2);
	CHECK(g_tasks[2].fn == sensor_task_fct);
	CHECK_EQ(g_tasks[2].pri, 3);
	CHECK(g_tasks[3].fn == heartbeat_task_fct);
	CHECK_EQ(g_tasks[3].pri, 10);

	/* The lock-free feature frame is only safe while the producer
	 * (sensor) has LOWER priority than the consumer (inference). */
	CHECK(SENSOR_TK_PRI > INFERENCE_TK_PRI);
	CHECK(INFERENCE_TK_PRI > HAZARD_TK_PRI);

	/* Each task has its own static stack. */
	CHECK(g_tasks[0].stack != g_tasks[1].stack);
	CHECK(g_tasks[1].stack != g_tasks[2].stack);
	CHECK(g_tasks[2].stack != g_tasks[3].stack);
	CHECK_EQ(g_tasks[0].stksz, TASK_STACK_SIZE);
}

static void test_urgency_interval_mapping(void)
{
	/* README: ~400 ms at 20 cm/s, 4 ms shorter per extra cm/s, 75 ms floor. */
	CHECK_EQ(hazard_urgency_interval_ms(20), 400);
	CHECK_EQ(hazard_urgency_interval_ms(21), 396);
	CHECK_EQ(hazard_urgency_interval_ms(50), 280);
	CHECK_EQ(hazard_urgency_interval_ms(80), 160);

	/* The floor is reached just ABOVE 100 cm/s: exactly 100 gives 80 ms. */
	CHECK_EQ(hazard_urgency_interval_ms(100), 80);
	CHECK_EQ(hazard_urgency_interval_ms(101), 76);
	CHECK_EQ(hazard_urgency_interval_ms(102), 75);
	CHECK_EQ(hazard_urgency_interval_ms(103), 75);
	CHECK_EQ(hazard_urgency_interval_ms(291), 75);	/* fastest swipe seen */
	CHECK_EQ(hazard_urgency_interval_ms(400000), 75);

	/* At or below the threshold (reachable when the MODEL raises the
	 * hazard without the rule): slowest rate, never longer. */
	CHECK_EQ(hazard_urgency_interval_ms(19), 400);
	CHECK_EQ(hazard_urgency_interval_ms(0), 400);
	CHECK_EQ(hazard_urgency_interval_ms(-150), 400);
	CHECK_EQ(hazard_urgency_interval_ms(INT32_MIN), 400);
}

static void test_urgency_interval_is_monotonic_and_bounded(void)
{
	W v;
	UW prev = hazard_urgency_interval_ms(-500);

	for (v = -500; v <= 2000; v++) {
		UW iv = hazard_urgency_interval_ms(v);

		CHECK(iv <= prev);			/* faster never buzzes slower */
		CHECK(iv >= DRV_R3_FLOOR_MS);		/* R-3 floor                  */
		CHECK(iv <= (UW)HAZ_URG_BASE_MS);
		CHECK(iv <= DRV_TRIG_MAX_MS);
		prev = iv;
	}
}

static void test_norm1000(void)
{
	/* distance: 50..1300 mm -> -1000..+1000, midpoint 675 mm */
	CHECK_EQ(norm1000(675, NORM_DIST_OFFSET, NORM_DIST_SCALE), 0);
	CHECK_EQ(norm1000(1300, NORM_DIST_OFFSET, NORM_DIST_SCALE), 1000);
	CHECK_EQ(norm1000(50, NORM_DIST_OFFSET, NORM_DIST_SCALE), -1000);
	CHECK_EQ(norm1000(800, NORM_DIST_OFFSET, NORM_DIST_SCALE), 200);
	CHECK_EQ(norm1000(4000, NORM_DIST_OFFSET, NORM_DIST_SCALE), 1000);	/* clamp */
	CHECK_EQ(norm1000(0, NORM_DIST_OFFSET, NORM_DIST_SCALE), -1000);	/* clamp */

	/* velocity +-250 cm/s, zero-centred, truncating toward zero */
	CHECK_EQ(norm1000(20, 0, NORM_VEL_SCALE), 80);
	CHECK_EQ(norm1000(-125, 0, NORM_VEL_SCALE), -500);
	CHECK_EQ(norm1000(291, 0, NORM_VEL_SCALE), 1000);
	CHECK_EQ(norm1000(1, 0, NORM_ACCEL_SCALE), 0);
	CHECK_EQ(norm1000(-1, 0, NORM_ACCEL_SCALE), 0);

	/* accelerometer: 1 g on a +-4 g range */
	CHECK_EQ(norm1000(1000, 0, NORM_ACC_MG_SCALE), 250);

	/* ToF counts are zero-anchored: only [0, 1000] is produced */
	CHECK_EQ(norm1000(0, 0, NORM_AMBIENT_SCALE), 0);
	CHECK_EQ(norm1000(250, 0, NORM_AMBIENT_SCALE), 1000);
	CHECK_EQ(norm1000(57544, 0, NORM_SIGSPAD_SCALE), 1000);
}

static void test_rule_fires_without_the_model(void)
{
	/* The SAFETY-NET FALLBACK: d < 800 mm AND v > 20 cm/s, model silent. */
	CHECK_EQ(decide(500, 50, 0), 1);
	CHECK_EQ(decide(799, 21, 0), 1);
	CHECK_EQ(decide(0, 400, 0), 1);		/* point blank, very fast */
	CHECK_EQ(decide(50, 291, 0), 1);
}

static void test_rule_boundaries_are_strict(void)
{
	CHECK_EQ(decide(800, 50, 0), 0);	/* exactly 80 cm: not inside */
	CHECK_EQ(decide(799, 20, 0), 0);	/* exactly 20 cm/s: not faster */
	CHECK_EQ(decide(800, 20, 0), 0);
	CHECK_EQ(decide(801, 300, 0), 0);
}

static void test_still_or_retreating_object_is_not_a_hazard(void)
{
	CHECK_EQ(decide(300, 0, 0), 0);		/* hand held still */
	CHECK_EQ(decide(300, -80, 0), 0);	/* moving away     */
	CHECK_EQ(decide(100, 19, 0), 0);
}

static void test_model_alone_can_raise_a_hazard(void)
{
	/* Predictive case: far away / slow by the rule, model says "about to". */
	CHECK_EQ(decide(1200, 10, 1), 1);
	CHECK_EQ(decide(2000, -100, 1), 1);
}

static void test_rule_never_suppresses_the_model(void)
{
	/* Full truth table: decision == model OR rule. */
	static const struct { W d, v; } pt[] = {
		{ 500, 50 }, { 500, 5 }, { 1500, 50 }, { 1500, 5 },
		{ 799, 21 }, { 800, 21 }, { 799, 20 }, { 0, 0 },
	};
	unsigned i;

	for (i = 0; i < sizeof(pt) / sizeof(pt[0]); i++) {
		int rule = (pt[i].d < 800 && pt[i].v > 20);

		CHECK_EQ(decide(pt[i].d, pt[i].v, 0), rule);
		CHECK_EQ(decide(pt[i].d, pt[i].v, 1), 1);
	}
}

static void test_model_receives_normalised_features(void)
{
	int i;

	for (i = 0; i < FEAT_COUNT; i++)
		feature_buf[i] = 0;
	feature_buf[0] = 675;			/* -> 0     */
	feature_buf[1] = 1300;			/* -> 1000  */
	feature_buf[9] = 50;			/* -> -1000 */
	feature_buf[FEAT_IDX_VCLOSE] = 125;	/* -> 500   */
	feature_buf[FEAT_IDX_ACCEL] = -2500;	/* -> -500  */
	feature_buf[FEAT_IDX_AX] = 1000;	/* -> 250   */
	feature_buf[FEAT_IDX_AY] = -4000;	/* -> -1000 */
	feature_buf[FEAT_IDX_AZ] = 9999;	/* -> 1000 (clamp) */
	feature_buf[FEAT_IDX_AMBIENT] = 125;	/* -> 500   */
	feature_buf[FEAT_IDX_SIGSPAD] = 5000;	/* -> 200   */
	g_model_bit = 0;
	run_inference(1);

	CHECK_EQ(HAZ_FEAT_COUNT, FEAT_COUNT);
	CHECK_EQ(g_model_last_in[0], 0);
	CHECK_EQ(g_model_last_in[1], 1000);
	CHECK_EQ(g_model_last_in[2], -1000);	/* raw 0 mm clamps */
	CHECK_EQ(g_model_last_in[9], -1000);
	CHECK_EQ(g_model_last_in[FEAT_IDX_VCLOSE], 500);
	CHECK_EQ(g_model_last_in[FEAT_IDX_ACCEL], -500);
	CHECK_EQ(g_model_last_in[FEAT_IDX_AX], 250);
	CHECK_EQ(g_model_last_in[FEAT_IDX_AY], -1000);
	CHECK_EQ(g_model_last_in[FEAT_IDX_AZ], 1000);
	CHECK_EQ(g_model_last_in[FEAT_IDX_AMBIENT], 500);
	CHECK_EQ(g_model_last_in[FEAT_IDX_SIGSPAD], 200);
}

static void test_result_slot_carries_raw_values_and_matching_canary(void)
{
	UW infs0 = stat_inferences;

	(void)decide(432, 77, 0);
	CHECK_EQ(result_slot.distance_mm, 432);		/* raw mm, not normalised */
	CHECK_EQ(result_slot.v_close_cm_s, 77);		/* raw cm/s for urgency   */
	CHECK_EQ(result_slot.seq, result_slot.seq_check);
	CHECK_EQ(stat_inferences, infs0 + 1u);

	/* The sequence number is local to the task body, which this harness
	 * re-enters from the top on every run_inference() call -- so count
	 * within ONE entry: three results -> seq 1, 2, 3. */
	run_inference(3);
	CHECK_EQ(result_slot.seq, 3);
	CHECK_EQ(result_slot.seq, result_slot.seq_check);
	CHECK_EQ(stat_inferences, infs0 + 4u);
}

static void test_inference_handshake_order(void)
{
	char want[3][24];

	reset_trace();
	(void)decide(500, 50, 0);

	/* wait for a frame, wait for the free slot, publish the result */
	snprintf(want[0], sizeof(want[0]), "wai:%d", data_ready_sem);
	snprintf(want[1], sizeof(want[1]), "wai:%d", result_free_sem);
	snprintf(want[2], sizeof(want[2]), "sig:%d", result_ready_sem);
	CHECK_EQ(g_n_events, 3);
	CHECK(strcmp(g_events[0], want[0]) == 0);
	CHECK(strcmp(g_events[1], want[1]) == 0);
	CHECK(strcmp(g_events[2], want[2]) == 0);
}

static void test_hazard_task_fires_trig_with_urgency_interval(void)
{
	UW ev0 = stat_hazard_events;

	(void)decide(500, 50, 0);
	g_trig_calls = 0;
	reset_trace();
	run_hazard(1);
	CHECK_EQ(g_trig_calls, 1);
	CHECK_EQ(g_trig_last_interval, 280);
	CHECK_EQ(stat_hazard_events, ev0 + 1u);
	CHECK_EQ(g_sig_count[result_free_sem], 1);	/* slot handed back */

	/* Model-only hazard on a slow target: slowest rate, still an alert. */
	(void)decide(1200, 5, 1);
	run_hazard(1);
	CHECK_EQ(g_trig_calls, 2);
	CHECK_EQ(g_trig_last_interval, 400);
}

static void test_hazard_task_stays_quiet_without_hazard(void)
{
	UW ev0 = stat_hazard_events;

	(void)decide(1500, 0, 0);
	g_trig_calls = 0;
	reset_trace();
	run_hazard(1);
	CHECK_EQ(g_trig_calls, 0);
	CHECK_EQ(stat_hazard_events, ev0);
	CHECK_EQ(g_sig_count[result_free_sem], 1);	/* slot still handed back */
}

static void test_canary_detects_a_torn_result(void)
{
	UW errs0 = stat_canary_errs;

	(void)decide(1500, 0, 0);
	run_hazard(1);
	CHECK_EQ(stat_canary_errs, errs0);		/* intact slot: silent */

	result_slot.seq_check = result_slot.seq + 1u;	/* simulate a torn read */
	run_hazard(1);
	CHECK_EQ(stat_canary_errs, errs0 + 1u);

	result_slot.seq_check = result_slot.seq;
	run_hazard(1);
	CHECK_EQ(stat_canary_errs, errs0 + 1u);
}

/* ---- sensor path: these run in order and share sensor_fill_frame()'s
 * static history, exactly as the firmware does. ---- */

static void test_no_tof_sensor_never_trips_the_rule(void)
{
	int i, hazards = 0, fresh = 0;

	/* ToF init never succeeded: the frame loop runs on the synthetic
	 * approach/retreat ramp (50 cm/s, down to 400 mm). The velocity
	 * estimator is gated on REAL frames, so v stays 0 and the rule must
	 * not produce phantom alerts with no sensor on the bus. */
	g_tof.init_result = 1;
	g_model_bit = 0;
	for (i = 0; i < 400; i++) {
		g_now_ms += 20u;
		run_sensor(1);
		run_inference(1);
		hazards += result_slot.hazard;
		fresh += (feature_frame.tof_state == VSTATE_FRESH);
		CHECK_EQ(feature_frame.tof_valid, 0);
	}
	CHECK_EQ(hazards, 0);
	CHECK_EQ(fresh, 0);
	CHECK_EQ(feature_frame.tof_state, VSTATE_INVALID);
	CHECK_EQ(feature_buf[FEAT_IDX_VCLOSE], 0);
	CHECK(feature_buf[0] >= SYN_D_NEAR_MM && feature_buf[0] <= SYN_D_FAR_MM);
}

static void test_real_frames_build_history_and_velocity(void)
{
	UW t = 100000u;
	int i;

	/* Five accepted frames, 10 mm closer every 20 ms = 50 cm/s closing. */
	for (i = 0; i < 5; i++, t += 20u)
		tof_frame(1000u - 10u * (UW)i, t);

	CHECK_EQ(feature_frame.tof_valid, 1);
	CHECK_EQ(feature_frame.tof_state, VSTATE_FRESH);
	CHECK_EQ(feature_frame.tof_age, 0);
	CHECK_EQ(feature_buf[0], 960);		/* newest first */
	CHECK_EQ(feature_buf[1], 970);
	CHECK_EQ(feature_buf[4], 1000);
	CHECK_EQ(feature_buf[FEAT_IDX_VCLOSE], 50);

	/* Ambient / signal-per-SPAD ride along with the accepted frame. */
	g_tof.ambient = 144;
	g_tof.sig_per_spad = 6600;
	tof_frame(950u, t);
	CHECK_EQ(feature_buf[FEAT_IDX_AMBIENT], 144);
	CHECK_EQ(feature_buf[FEAT_IDX_SIGSPAD], 6600);
	CHECK_EQ(feature_buf[FEAT_IDX_VCLOSE], 50);
}

static void test_missing_frames_hold_then_go_invalid(void)
{
	W v_before = feature_buf[FEAT_IDX_VCLOSE];
	W a_before = feature_buf[FEAT_IDX_ACCEL];
	W d_before = feature_buf[0];
	UW t = 200000u;

	/* Cycle with no new ToF frame: distance, velocity and acceleration
	 * hold; nothing is recomputed from a repeated sample. */
	tof_no_frame(t);
	CHECK_EQ(feature_frame.tof_valid, 0);
	CHECK_EQ(feature_frame.tof_state, VSTATE_HELD);
	CHECK_EQ(feature_frame.tof_age, 1);
	CHECK_EQ(feature_buf[0], d_before);
	CHECK_EQ(feature_buf[1], d_before);	/* history still shifts */
	CHECK_EQ(feature_buf[FEAT_IDX_VCLOSE], v_before);
	CHECK_EQ(feature_buf[FEAT_IDX_ACCEL], a_before);

	tof_no_frame(t + 20u);
	CHECK_EQ(feature_frame.tof_state, VSTATE_HELD);
	CHECK_EQ(feature_frame.tof_age, VALID_MAX_HOLD);

	tof_no_frame(t + 40u);			/* older than VALID_MAX_HOLD */
	CHECK_EQ(feature_frame.tof_state, VSTATE_INVALID);
	CHECK_EQ(feature_frame.tof_age, VALID_MAX_HOLD + 1u);

	/* REVIEW FINDING, locked here as current behaviour: an INVALID ToF
	 * state does not clear the held velocity, and inference_task does not
	 * look at tof_state -- so a stalled sensor leaves the last decision's
	 * inputs in place. */
	CHECK_EQ(feature_buf[FEAT_IDX_VCLOSE], v_before);

	/* First fresh frame recovers immediately. */
	tof_frame((UW)d_before, t + 60u);
	CHECK_EQ(feature_frame.tof_state, VSTATE_FRESH);
	CHECK_EQ(feature_frame.tof_age, 0);
	CHECK_EQ(feature_frame.tof_valid, 1);
}

static void test_imu_validity_and_tristate(void)
{
	UW t = 300000u;

	g_imu.ax_mg = -33; g_imu.ay_mg = -2; g_imu.az_mg = 1110;
	g_imu.last_valid = 1;
	tof_frame(900u, t);
	CHECK_EQ(feature_buf[FEAT_IDX_AX], -33);
	CHECK_EQ(feature_buf[FEAT_IDX_AY], -2);
	CHECK_EQ(feature_buf[FEAT_IDX_AZ], 1110);
	CHECK_EQ(feature_frame.imu_valid, 1);
	CHECK_EQ(feature_frame.imu_state, VSTATE_FRESH);

	g_imu.last_valid = 0;			/* burst read failed */
	tof_frame(900u, t + 20u);
	CHECK_EQ(feature_frame.imu_valid, 0);
	CHECK_EQ(feature_frame.imu_state, VSTATE_HELD);
	CHECK_EQ(feature_buf[FEAT_IDX_AZ], 1110);	/* last value held */
	tof_frame(900u, t + 40u);
	tof_frame(900u, t + 60u);
	CHECK_EQ(feature_frame.imu_state, VSTATE_INVALID);
	CHECK_EQ(feature_frame.imu_age, 3);

	g_imu.last_valid = 1;
	tof_frame(900u, t + 80u);
	CHECK_EQ(feature_frame.imu_state, VSTATE_FRESH);
	CHECK_EQ(feature_frame.imu_age, 0);
}

static void test_sentinels_make_zeroed_memory_read_invalid(void)
{
	CHECK_EQ(VSTATE_INVALID, 0);		/* BSS zero must mean INVALID */
	CHECK(VSTATE_FRESH != VSTATE_HELD);
	CHECK(VSTATE_FRESH != VSTATE_INVALID);
}

static void test_end_to_end_approach_alerts_inside_80cm_only(void)
{
	UW t = 400000u, d;
	int alerts_outside = 0, alerts_inside = 0, quiet_inside = 0;

	/* Let the estimator settle on a far, stationary target first. */
	for (d = 0; d < 8; d++, t += 20u)
		tof_frame(1200u, t);

	g_model_bit = 0;	/* isolate the rule */
	g_trig_calls = 0;
	for (d = 1200u; d >= 400u; d -= 10u, t += 20u) {	/* 50 cm/s closing */
		int before = g_trig_calls;

		tof_frame(d, t);
		run_inference(1);
		run_hazard(1);
		if (g_trig_calls != before) {
			CHECK_EQ(g_trig_last_interval, 280);
			if (d < 800u) alerts_inside++; else alerts_outside++;
		} else if (d < 800u) {
			quiet_inside++;
		}
	}
	CHECK_EQ(alerts_outside, 0);
	CHECK_EQ(quiet_inside, 0);
	CHECK_EQ(alerts_inside, 40);	/* 790 .. 400 mm in 10 mm steps */

	/* Target stops: velocity decays over the 5-sample window, then quiet. */
	for (d = 0; d < 8; d++, t += 20u) {
		tof_frame(400u, t);
		run_inference(1);
	}
	CHECK_EQ(result_slot.hazard, 0);

	/* Target retreats: never a hazard. */
	for (d = 400u; d <= 700u; d += 10u, t += 20u) {
		tof_frame(d, t);
		run_inference(1);
		CHECK_EQ(result_slot.hazard, 0);
	}
}

static void test_csv_logger_labels_with_the_frame_k_ahead(void)
{
	/*
	 * CSV_LOG_ENABLE is 1 in the committed firmware, so sensor_task prints
	 * one [CSV] row per frame once the k+1 ring has filled. The printed
	 * row is the frame from CSV_LABEL_SHIFT_K cycles ago; its label is
	 * the hazard rule evaluated on the CURRENT frame.
	 */
	UW t = 500000u;
	unsigned i;
	int lines0;

	CHECK_EQ(CSV_LOG_ENABLE, 1);
	CHECK_EQ(CSV_LABEL_SHIFT_K, 12);
	CHECK(g_csv_lines > 0);		/* earlier tests filled the ring */

	/* k+1 quiet frames at 1300 mm (norm +1000), then one fast close-in
	 * sample: the row printed now is an old far-away frame, label 1. */
	for (i = 0; i < CSV_RING_LEN + VLSQ_WINDOW; i++, t += 20u)
		tof_frame(1300u, t);
	CHECK(strstr(g_last_csv, "[CSV] d0=1000 ") != NULL);
	CHECK(strstr(g_last_csv, " label=0") != NULL);

	lines0 = g_csv_lines;
	tof_frame(300u, t);		/* 1000 mm closer in 20 ms */
	CHECK_EQ(g_csv_lines, lines0 + 1);
	CHECK(feature_buf[FEAT_IDX_VCLOSE] > HAZARD_VCLOSE_CM_S);
	CHECK(strstr(g_last_csv, "[CSV] d0=1000 ") != NULL);	/* still the old row */
	CHECK(strstr(g_last_csv, " label=1") != NULL);		/* labelled by "now" */
	CHECK(strstr(g_last_csv, " tvs=165 ") != NULL);		/* VSTATE_FRESH = 0xA5 */
}

static void test_sensor_loop_bookkeeping(void)
{
	UW frames0 = stat_frames, ovr0 = stat_dataq_ovr;

	/* 50 frames in one run of the loop = one ~1 Hz DRV2605L health poll. */
	g_poll_calls = 0;
	g_measure_calls = 0;
	reset_trace();
	run_sensor(50);
	CHECK_EQ(stat_frames, frames0 + 50u);
	CHECK_EQ(g_sig_count[data_ready_sem], 50);
	CHECK_EQ(g_poll_calls, 1);

	/* The host DWT stub never counts, so app_gpio_init() must have
	 * reported the cycle counter dead and the DWT-timed effect
	 * measurement must stay gated off (finding D-J in the source). */
	CHECK_EQ(dwt_ok, 0);
	CHECK_EQ(g_measure_calls, 0);

	/* A full data_ready queue is counted, not ignored. */
	g_sig_result[data_ready_sem] = E_QOVR;
	run_sensor(3);
	g_sig_result[data_ready_sem] = E_OK;
	CHECK_EQ(stat_dataq_ovr, ovr0 + 3u);
	CHECK_EQ(stat_frames, frames0 + 53u);
}

static void test_heartbeat_prints_status_line(void)
{
	g_sleep_budget = 1;
	if (setjmp(g_env) == 0)
		heartbeat_task_fct(0, NULL);
	CHECK(strncmp(g_last_line, "[DWT] ok=0", 10) == 0);	/* last line of a beat */
}

int main(void)
{
	app_tasks_run();	/* creates semaphores + tasks through the stubs */

	RUN(test_startup_creates_semaphores_before_tasks);
	RUN(test_task_priorities_match_documented_table);
	RUN(test_urgency_interval_mapping);
	RUN(test_urgency_interval_is_monotonic_and_bounded);
	RUN(test_norm1000);
	RUN(test_rule_fires_without_the_model);
	RUN(test_rule_boundaries_are_strict);
	RUN(test_still_or_retreating_object_is_not_a_hazard);
	RUN(test_model_alone_can_raise_a_hazard);
	RUN(test_rule_never_suppresses_the_model);
	RUN(test_model_receives_normalised_features);
	RUN(test_result_slot_carries_raw_values_and_matching_canary);
	RUN(test_inference_handshake_order);
	RUN(test_hazard_task_fires_trig_with_urgency_interval);
	RUN(test_hazard_task_stays_quiet_without_hazard);
	RUN(test_canary_detects_a_torn_result);
	RUN(test_no_tof_sensor_never_trips_the_rule);
	RUN(test_real_frames_build_history_and_velocity);
	RUN(test_missing_frames_hold_then_go_invalid);
	RUN(test_imu_validity_and_tristate);
	RUN(test_sentinels_make_zeroed_memory_read_invalid);
	RUN(test_end_to_end_approach_alerts_inside_80cm_only);
	RUN(test_csv_logger_labels_with_the_frame_k_ahead);
	RUN(test_sensor_loop_bookkeeping);
	RUN(test_heartbeat_prints_status_line);
	return MU_REPORT();
}
