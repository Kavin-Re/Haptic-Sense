/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * boot_log.h -- one-line structured boot/status messages on the tm_printf
 * console (USART1, ST-LINK VCP, 115200 baud).
 *
 * Written for Haptic-Sense on 2026-09-25 to replace serial_protocol.h, which
 * was removed because its copyright header did not match its origin (see
 * docs/PROVENANCE.md). Implemented from main.c's call sites only: it provides
 * exactly what the build uses and nothing else.
 *
 * Line format, kept identical to the previous boot log so earlier UART
 * captures stay directly comparable:
 *
 *     [<uptime ms, 8 digits>] [<level>] [<module>] [<event>] <json object>\n
 *
 *     [00000000] [INFO] [RTOS] [RTOS_START] {}
 *     [00000000] [INFO] [RTOS] [TASK_START] {"name":"main_thread","id":<id>}
 *
 * Context: task context only (usermain / main_thread, TK_PRI 1 inittask and
 * 15). tm_printf is polled UART output -- never call these from the TK_PRI 1
 * hazard path, the TK_PRI 2 inference path, or an ISR.
 */
#ifndef BOOT_LOG_H
#define BOOT_LOG_H

#include <stdint.h>
#include "stm32n6xx_hal.h"	/* HAL_GetTick(): hal_timebase_mtk.c */
#include "tk/tkernel.h"
#include "tm/tmonitor.h"

#define BOOT_LOG_LEVEL_INFO	"INFO"

#define BOOT_LOG_MOD_RTOS	"RTOS"
#define BOOT_LOG_MOD_INIT	"INIT"

/* Common prefix: "[ms] [level] [module] [event] " */
static inline void boot_log_prefix(const char *level, const char *module,
				   const char *event)
{
	tm_printf((UB *)"[%08u] [%s] [%s] [%s] ",
		  (UW)HAL_GetTick(), level, module, event);
}

/* Event with an empty payload: ... {} */
static inline void boot_log(const char *module, const char *event)
{
	boot_log_prefix(BOOT_LOG_LEVEL_INFO, module, event);
	tm_printf((UB *)"{}\n");
}

/* Event with one string field: ... {"<key>":"<value>"} */
static inline void boot_log_str(const char *module, const char *event,
				const char *key, const char *value)
{
	boot_log_prefix(BOOT_LOG_LEVEL_INFO, module, event);
	tm_printf((UB *)"{\"%s\":\"%s\"}\n", key, value);
}

/* Task lifecycle event: ... {"name":"<task>","id":<tk_cre_tsk result>} */
static inline void boot_log_task(const char *module, const char *event,
				 const char *name, ID id)
{
	boot_log_prefix(BOOT_LOG_LEVEL_INFO, module, event);
	tm_printf((UB *)"{\"name\":\"%s\",\"id\":%d}\n", name, (INT)id);
}

#endif /* BOOT_LOG_H */
