/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_mpu6050.h — L2 driver for the MPU6050 IMU (Haptic-Sense, Block 3)
 *
 * Design: docs/design/mpu6050_port_design_v1.md (2026-07-09), audited
 * docs/audits/redzone_audit_addendum_mpu6050_2026-07-11.md. Sits directly on
 * the L1 primitive (app_i2c.c) — no ULD, no shim, unlike the VL53L1X.
 *
 * CROSS-SENSOR CONTAMINATION GUARDS (design doc §0 — read before touching
 * this file):
 *   - 8-bit register index, I2C_REG8. NEVER I2C_REG16 (the VL53L1X value) —
 *     that would emit a phantom high byte and silently misaddress every
 *     register.
 *   - 7-bit address 0x68/0x69, passed to i2c_rd/i2c_wr UNSHIFTED. NEVER the
 *     VL53L1X's dev>>1 habit — that yields 0x34, a NACK on every transaction.
 *   - WHO_AM_I (0x75) == 0x68 is the register's RESET VALUE, not an echo of
 *     which address ACKed (AD0 is not reflected in this register, RM §4.34).
 *     The address that ACKed is the only evidence of which of 0x68/0x69 this
 *     unit is on.
 *
 * INTEGER ONLY. Scaling is raw*1000/8192 (see mpu6050_service()), no float —
 * same rationale as velocity_lsq.h: no task here may touch the FPU
 * (app_vl53l1x.c:263-273, CONTROL.FPCA / preemption-latency determinism).
 */

#ifndef APP_MPU6050_H
#define APP_MPU6050_H

#include "tk/tkernel.h"

/*
 * Bring-up / runtime observability. Single writer (sensor task, TK_PRI 3);
 * heartbeat task reads it. 32-bit reads are atomic on ARMv8-M.
 *
 * init_result: 1 = not run; E_OK once armed and verified — same "1, not 0"
 * sentinel convention as drv2605l_stats_t.init_result and vl53l1x_stats_t
 * .init_result (E_OK is 0, and this struct is BSS-zeroed, so the sentinel is
 * set explicitly at definition, not inside the init function — the D-H
 * defect class, app_drv2605l.c and the six BSS sentinels fixed 2026-09-03).
 */
typedef struct {
	W	init_result;	/* 1 = not run; E_OK = armed and verified   */
	UW	addr7;		/* 0x68 or 0x69 — whichever ACKed the probe */
	UW	whoami;		/* 0x75 readback, expect 0x68 (RM §4.34)    */
	UW	rst_polls;	/* DEVICE_RESET polls consumed (<= 5, M-2)  */
	UW	rst_pwrmgmt_rb;	/* PWR_MGMT_1 immediately after reset,
				 * expect 0x40 (SLEEP=1, POR default)       */
	UW	pwrmgmt_rb;	/* PWR_MGMT_1 after the wake write, expect 0x01 */
	UW	smplrt_rb;	/* 0x19 readback, expect 19 (-> 50 Hz)       */
	UW	cfg_rb;		/* 0x1A (DLPF_CFG) readback, expect 4        */
	UW	gyro_cfg_rb;	/* 0x1B readback — OBSERVABILITY ONLY: the
				 * written value (0x00, FS_SEL=0) equals the
				 * POR reset value, so this readback cannot
				 * discriminate "my write landed" from "the
				 * part reset" — the same trap the DRV2605L
				 * driver hit on WAVSEQ1 (app_drv2605l.c,
				 * drv2605l_init()'s "NOTE ON VERIFICATION").
				 * Deliberately excluded from the pass
				 * condition below. */
	UW	accel_cfg_rb;	/* 0x1C readback, expect 0x08 (+-4g, AFS_SEL=1,
				 * 8192 LSB/g -- CLAUDE.md:116, M-1 CLOSED)  */
	UW	int_cfg_rb;	/* 0x37 readback, expect 0x30 (LATCH_INT_EN |
				 * INT_RD_CLEAR)                             */
	UW	int_en_rb;	/* 0x38 readback, expect 0x01 (DATA_RDY_EN)  */
	UW	armed;		/* 1 once every discriminating readback matched */

	/* runtime (mpu6050_service) */
	UW	reads;		/* successful 14-byte burst reads            */
	UW	stale;		/* INT was low at poll time. DIAGNOSTIC ONLY as
				 * of 2026-09-04 -- does not gate the read, does
				 * not mean the sample was skipped (see
				 * mpu6050_service()) */
	UW	rderr;		/* the 14-byte burst i2c_rd itself failed     */

	/* latest accel sample, mg (raw * 1000 / 8192, +-4g range). Held over
	 * verbatim on a stale or errored poll -- these are what
	 * app_tasks.c's sensor_fill_frame() reads into feature_buf. */
	W	ax_mg;
	W	ay_mg;
	W	az_mg;

	/* Live PWR_MGMT_1 re-check, added 2026-09-04. mpu6050_init() only
	 * verifies wake ONCE at boot; if the part browns out or otherwise
	 * reverts to SLEEP afterward (leading suspect: a sensor VCC measured
	 * 2.43 V against the board's 3.32 V rail the same day), nothing
	 * before this noticed. Mirrors the DRV2605L pattern of periodically
	 * re-reading config rather than trusting an init-time snapshot
	 * forever. Checked every mpu6050_service() call. */
	UW	pwrmgmt_live_rb;	/* live 0x6B readback; != 0x01 means asleep */
	UW	pwrmgmt_drift;		/* count of times it was != 0x01 (SLEEP seen) */
	UW	pwrmgmt_live_rderr;	/* the live check's own i2c_rd failed */

	/* PH6-1 (docs/design/PH6-1_feature_frame_validity.md): validity
	 * transport for THIS cycle's ax/ay/az sample, read by sensor_task
	 * and copied into the protected feature_frame_t.imu_valid field
	 * (never passed as a bare cross-task flag -- M-4). Deliberately NOT
	 * gated on the INT/DATA_RDY pin: that pin stopped asserting after
	 * ~120 samples on this board while the sensor kept producing good
	 * data (see the note above mpu6050_service()'s INT poll). Gated
	 * instead on what this session actually validated as reliable:
	 * armed, this burst read succeeded, and the live PWR_MGMT_1
	 * re-check confirms the part is awake THIS cycle. */
	UW	last_valid;
} mpu6050_stats_t;

/* Configure PE9 (IMU_INT, ARD_D3, CN11 pin 4) as a plain input, no pull --
 * with INT_PIN_CFG's INT_OPEN=0 (push-pull driver, set in mpu6050_init()),
 * the device drives the line itself and needs no pull assist. Call from
 * init context (app_gpio_init()), before any task that touches this pin. */
void mpu6050_gpio_init(void);

/*
 * Full bring-up: address probe (0x68 then 0x69) -> WHO_AM_I gate -> bounded
 * DEVICE_RESET poll -> wake (PLL/X-gyro clock ref) -> config chain -> arm.
 * Every discriminating register readback-verified before armed=1.
 * Returns E_OK only if every gate held; fails loudly (never guesses) on an
 * address-probe miss, a WHO_AM_I mismatch, reset poll exhaustion, or any
 * config write/readback mismatch.
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 */
ER mpu6050_init(void);

/*
 * One non-blocking service call: UNCONDITIONAL 14-byte burst read from 0x3B
 * -- reassembles ax/ay/az (big-endian, RM §3) and scales to mg. PE9 is still
 * read every call and counted in stale, but as observability only (2026-09-04
 * -- see mpu6050_service()'s body comment): DATA_RDY on this part/board went
 * flat after an initial burst with wiring and GPIO config both independently
 * ruled out, so the design doc §4 Option A gate was dropped rather than
 * chase which of the two remaining physical causes it is. On an i2c_rd
 * failure the frame is marked rderr and the previous ax/ay/az values are
 * left untouched (reused, per the 12-feature fallback CLAUDE.md §6 already
 * anticipates). NO-OP until mpu6050_init() has armed the device. Safe to
 * call at the 20 ms sensor cadence.
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 */
void mpu6050_service(void);

const mpu6050_stats_t *mpu6050_get_stats(void);

#endif /* APP_MPU6050_H */
