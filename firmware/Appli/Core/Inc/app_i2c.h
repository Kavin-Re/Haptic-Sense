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

/*
 * BUCKET SELECTOR, NOT A TARGET. I2C_GetTiming() compares this against each
 * I2C_Charac[] entry's freq_min/freq_max to pick a speed class, then DISCARDS
 * it and computes against that entry's own .freq. Any value in [320000,480000]
 * produces a byte-identical TIMINGR. Changing this number does not change the
 * bus clock -- the delivered frequency is set by
 * I2C_Charac[I2C_SPEED_FREQ_FAST].freq in i2c_timing.c, currently 350000 for
 * the reason documented there. Measured delivered clock: see
 * docs/BUS2_SCL_FREQUENCY_20260830.md.
 */
#define I2C_BUS_HZ		400000u	/* selects the fast-mode bucket */
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
	UW	nacks;		/* address/data NACK: device not present   */
	W	gate_result;	/* E_OK once the L1 gate test passed      */
	W	gate_wr;	/* H-D9: 1 = not run, E_OK = write proven */
	UW	gate_wr_seen;	/* byte read back after the scratch write */
	UW	gate_whoami;	/* WHO_AM_I byte read by the gate test    */
	UW	gate_addr;	/* 7-bit address that answered (AD0 q.)   */
	UW	clk_pclk1_hz;	/* logged at init — design §6 verification */
	UW	clk_sysclk_hz;	/* IC2 sysb_ck — NOT the CPU clock         */
	UW	clk_cpu_hz;	/* IC1 CPUCLK — this is what DWT counts    */
	/* Block 2 L1 raw probe — VL53L1X reference registers.
	 * Values: docs/datasheets/vl53l1x_datasheet.pdf (DS12385 Rev 8) §4.2
	 * Table 8. See app_i2c_tof_probe() for what each field distinguishes. */
	W	tof_result;	/* 1 = not run; E_OK = every check passed  */
	UW	tof_step;	/* first failing step 1..4; 0 = none       */
	UW	tof_id;		/* 0x010F,0x0110 read singly: 0xEACC       */
	UW	tof_blk;	/* 3-byte block read at 0x010F: 0xEACC10   */
	UW	tof_ctl;	/* REG8 control read; 0x1FF = did not run  */
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

/* BLOCK 2 STEP L1: VL53L1X raw register probe on the L1 primitive, with a
 * REG8 negative control. Runs BEFORE any shim or ULD code. TK_PRI 3 only. */
void app_i2c_tof_probe(void);

const app_i2c_stats_t *app_i2c_stats(void);

#endif /* APP_I2C_H */
