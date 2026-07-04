/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_heartbeat.c — Phase 3 minimal heartbeat application (Haptic-Sense)
 *
 * Proves the full edit→build→sign→flash→boot pipeline with our own code:
 * LD1 (green, PO1, active HIGH) toggles at 1 Hz and a counter heartbeat
 * prints on the STLINK VCP (USART1, 115200) via tm_printf.
 *
 * Task context: heartbeat_task, TK_PRI 10.
 * heartbeat_run() itself executes in main_thread context (TK_PRI 15,
 * created in main.c usermain()).
 */

/*
 * TASK PRIORITY VERIFICATION (Red Zone #7 pre-work) — verified 2026-07-04
 *
 * Priority range: 1 (highest) .. 32 (lowest), per CNF_MAX_TSKPRI in
 * Appli/mtk3_bsp2/config/config.h:27.
 *
 * Every tk_cre_tsk() site compiled into this build:
 *   - kernel initial task  TK_PRI  1  (mtk3_bsp2/mtkernel/include/sys/inittask.h:26)
 *       Runs usermain(), then parks itself PERMANENTLY at main.c:107 via
 *       tk_slp_tsk(TMO_FEVR). Dormant, never runnable again — it will not
 *       preempt. Phase 4's Hazard task can share TK_PRI 1 (µT-Kernel allows
 *       multiple tasks per priority level), but this occupant must be known.
 *   - main_thread          TK_PRI 15  (main.c:94)
 *   - heartbeat_task       TK_PRI 10  (this file)
 *
 * The reference app's tasks at TK_PRI 10/11/14/14 (app.c:1040-1043) are
 * EXCLUDED from this build (.cproject Core sourceEntries) and create nothing.
 *
 * Conclusion: TK_PRI 2, 3 completely unused; TK_PRI 1 held only by the
 * permanently-dormant init task; TK_PRI 10 used only by heartbeat_task.
 * Priorities 1-3 remain reserved for the Phase 4 architecture (CLAUDE.md §3).
 */

#include "app_heartbeat.h"

#include "stm32n6xx_hal.h"
#include "tk/tkernel.h"
#include "tm/tmonitor.h"

#define HEARTBEAT_STACK_SIZE	4096
#define HEARTBEAT_TK_PRI	10
#define HEARTBEAT_PERIOD_MS	500	/* toggle every 500 ms -> 1 Hz blink */

static UB heartbeat_stack[HEARTBEAT_STACK_SIZE];
static ID heartbeat_task_id;

/*
 * LD1 GPIO init — runs in main_thread context (TK_PRI 15), before the
 * heartbeat task starts.
 *
 * CAUTION (Phase 3 design §3): GPIO port O carries XSPI1 (PSRAM) signals on
 * PO0/PO2/PO3/PO4 — the memory this application executes from. Configure
 * ONLY PO1. HAL_GPIO_Init performs pin-masked read-modify-write on
 * MODER/OSPEEDR/PUPDR (only GPIO_PIN_1 bits touched); HAL_GPIO_TogglePin
 * uses BSRR (atomic, single pin). No port-wide writes anywhere.
 */
static void heartbeat_led_init(void)
{
	GPIO_InitTypeDef gpio_init = {0};

	__HAL_RCC_GPIOO_CLK_ENABLE();	/* verified: stm32n6xx_hal_rcc.h:981 */

	gpio_init.Pin = GPIO_PIN_1;	/* ONLY PO1 — never GPIO_PIN_All on port O */
	gpio_init.Mode = GPIO_MODE_OUTPUT_PP;
	gpio_init.Pull = GPIO_NOPULL;
	gpio_init.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOO, &gpio_init);

	HAL_GPIO_WritePin(GPIOO, GPIO_PIN_1, GPIO_PIN_RESET);	/* LD1 off (active HIGH) */
}

/*
 * Heartbeat task body — TK_PRI 10.
 * tm_printf in a task body is acceptable ONLY in this phase and only at
 * TK_PRI 10 (Phase 3 design §4.2). The Phase 4 no-printf rule applies to
 * priority 1-2 tasks, which do not exist yet.
 */
static void heartbeat_task_fct(INT stacd, void *exinf)
{
	UW n = 0;
	SYSTIM tim;

	for (;;) {
		HAL_GPIO_TogglePin(GPIOO, GPIO_PIN_1);

		tk_get_otm(&tim);
		tm_printf((UB *)"[HB] %u uptime_ms=%u\n", n, tim.lo);
		n++;

		tk_slp_tsk(HEARTBEAT_PERIOD_MS);	/* no producer calls tk_wup_tsk: returns E_TMOUT after 500 ms */
	}
}

/*
 * Entry point, called from main_thread_fct() (main.c, TK_PRI 15) in place
 * of the reference app's app_run().
 */
void heartbeat_run(void)
{
	T_CTSK task_config;

	heartbeat_led_init();

	task_config.tskatr = TA_HLNG | TA_RNG3 | TA_USERBUF;
	task_config.stksz = HEARTBEAT_STACK_SIZE;
	task_config.itskpri = HEARTBEAT_TK_PRI;
	task_config.task = heartbeat_task_fct;
	task_config.bufptr = heartbeat_stack;
	task_config.exinf = NULL;

	heartbeat_task_id = tk_cre_tsk(&task_config);
	if (heartbeat_task_id <= E_OK) {
		tm_printf((UB *)"[HB] FATAL: tk_cre_tsk failed (%d)\n", (INT)heartbeat_task_id);
		return;
	}

	tk_sta_tsk(heartbeat_task_id, 0);
	tm_printf((UB *)"[HB] heartbeat task started (TK_PRI %d)\n", HEARTBEAT_TK_PRI);
}
