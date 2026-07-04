/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_heartbeat.h — Phase 3 minimal heartbeat application (Haptic-Sense)
 */

#ifndef APP_HEARTBEAT_H
#define APP_HEARTBEAT_H

/* Called from main_thread_fct() (main.c, TK_PRI 15). Creates and starts
 * heartbeat_task at TK_PRI 10, then returns. */
void heartbeat_run(void);

#endif /* APP_HEARTBEAT_H */
