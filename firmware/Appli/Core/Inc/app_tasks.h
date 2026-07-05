/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_tasks.h — Phase 4 task architecture (Haptic-Sense)
 */

#ifndef APP_TASKS_H
#define APP_TASKS_H

/* Called from main_thread_fct() (main.c, TK_PRI 15). Creates the paired
 * semaphores FIRST, then creates and starts hazard(1) / inference(2) /
 * sensor(3) / heartbeat(10), then returns. */
void app_tasks_run(void);

#endif /* APP_TASKS_H */
