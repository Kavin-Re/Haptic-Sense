/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_i2c.h — Phase 5 L0/L1: semaphore-wrapped DMA I2C1 primitive (Haptic-Sense)
 *
 * Design: docs/PHASE5_L1_i2c_xfer_design.md (approved with changes 2026-07-06).
 * EVERY function here: // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 */

#ifndef APP_I2C_H
#define APP_I2C_H

#include "tk/tkernel.h"

#define I2C_BUS_HZ		400000u	/* Fast-mode target; 100 kHz fallback pre-approved */
#define I2C_XFER_TMO_MS		50	/* design §4.3: >10x slowest transfer, 2.5 frames  */

/* Register-address size per device (HAL mem-address size codes) */
#define I2C_REG8		1u	/* == I2C_MEMADD_SIZE_8BIT  (MPU6050, DRV2605L) */
#define I2C_REG16		2u	/* == I2C_MEMADD_SIZE_16BIT (VL53L1X)           */

/* Bring-up / gate-test observability — single writer (sensor task);
 * heartbeat task reads via app_i2c_stats() (32-bit reads atomic on ARMv8-M). */
typedef struct {
	UW	xfer_ok;	/* completed transfers                    */
	UW	xfer_err;	/* failed after retry                     */
	UW	timeouts;	/* tk_wai_sem E_TMOUT count               */
	UW	recoveries;	/* bus-recovery invocations               */
	W	gate_result;	/* E_OK once the L1 gate test passed      */
	UW	gate_whoami;	/* WHO_AM_I byte read by the gate test    */
	UW	gate_addr;	/* 7-bit address that answered (AD0 q.)   */
	UW	clk_pclk1_hz;	/* logged at init — design §6 verification */
	UW	clk_sysclk_hz;	/* logged at init — design §6 verification */
} app_i2c_stats_t;

/* Init I2C1 + GPDMA + kernel IRQ registration. TK_PRI 3 context only. */
ER app_i2c_init(void);

/* L1 primitive: synchronous to caller, non-blocking to CPU (DMA + tk_wai_sem).
 * regsz = I2C_REG8 | I2C_REG16. One internal retry, then bus recovery. */
ER i2c_rd(UB dev7, UW reg, UINT regsz, UB *buf, UW len);
ER i2c_wr(UB dev7, UW reg, UINT regsz, const UB *buf, UW len);

/* MANDATORY GATE (review requirement 2026-07-06): standalone L1 DMA proof —
 * a single register read (MPU6050 WHO_AM_I, trying 0x68 then 0x69) BEFORE any
 * L2/ULD code exists. Result lands in stats; heartbeat prints it. */
void app_i2c_gate_test(void);

const app_i2c_stats_t *app_i2c_stats(void);

#endif /* APP_I2C_H */
