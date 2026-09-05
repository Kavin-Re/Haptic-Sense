/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_mpu6050.c — L2 driver for the MPU6050 IMU (Haptic-Sense, Block 3)
 *
 * See app_mpu6050.h for the cross-sensor contamination guards and the
 * bring-up contract. Register values and citations below trace to
 * docs/design/mpu6050_port_design_v1.md {RM}/{PS} unless noted.
 */

#include "app_mpu6050.h"
#include "app_i2c.h"
#include "stm32n6xx_hal.h"

#define MPU_INT_PORT		GPIOE
#define MPU_INT_PIN		GPIO_PIN_9	/* ARD_D3, CN11 pin 4 */

/* ------------------------------------------------------------------------ */
/* Registers (RM-MPU-6000A-00 rev 4.0)                                       */
/* ------------------------------------------------------------------------ */
#define MPU_REG_SMPLRT_DIV	0x19u	/* reset 0x00 */
#define MPU_REG_CONFIG		0x1Au	/* reset 0x00 -- DLPF_CFG            */
#define MPU_REG_GYRO_CONFIG	0x1Bu	/* reset 0x00 -- FS_SEL               */
#define MPU_REG_ACCEL_CONFIG	0x1Cu	/* reset 0x00 -- AFS_SEL              */
#define MPU_REG_INT_PIN_CFG	0x37u	/* reset 0x00                         */
#define MPU_REG_INT_ENABLE	0x38u	/* reset 0x00                         */
#define MPU_REG_ACCEL_XOUT_H	0x3Bu	/* burst start; 14 bytes to GYRO_ZOUT_L */
#define MPU_REG_PWR_MGMT_1	0x6Bu	/* reset 0x40 (SLEEP=1)               */
#define MPU_REG_WHO_AM_I	0x75u	/* reset 0x68, AD0 not reflected      */

#define MPU_PWR_DEV_RESET	0x80u	/* bit 7, self-clearing (RM §4.30)    */
#define MPU_PWR_RESET_VALUE	0x40u	/* PWR_MGMT_1 immediately after reset */
#define MPU_PWR_WAKE_PLL_XG	0x01u	/* SLEEP=0 CYCLE=0 TEMP_DIS=0 CLKSEL=1 */
#define MPU_RESET_POLL_MAX	5u	/* x 1 kernel tick (1-2 ms), M-2       */

#define MPU_DLPF_CFG_4		0x04u	/* accel 21 Hz / gyro 20 Hz BW, Option A */
#define MPU_SMPLRT_DIV_50HZ	19u	/* 1000 / (1 + 19) = 50 Hz             */
#define MPU_GYRO_FS_250DPS	0x00u	/* FS_SEL=0 -- not read back for the
					 * pass condition, see app_mpu6050.h  */
#define MPU_ACCEL_FS_4G		0x08u	/* AFS_SEL=1, bits[4:3]=01 -> 8192 LSB/g,
					 * CLAUDE.md:116 (M-1 CLOSED)          */
#define MPU_INT_LATCH_RDCLR	0x30u	/* LATCH_INT_EN(bit5) | INT_RD_CLEAR(bit4) */
#define MPU_INT_DATA_RDY_EN	0x01u

#define MPU_ADDR7_LOW		0x68u	/* AD0 low                            */
#define MPU_ADDR7_HIGH		0x69u	/* AD0 high                           */
#define MPU_WHOAMI_EXPECT	0x68u	/* always, regardless of which address */

/* Sentinel for every read -- neither a plausible register value nor the 0xFF
 * an idle pulled-up bus returns. Same discipline as DRV_SENTINEL/GATE_SENTINEL
 * (app_drv2605l.c, app_i2c.c) -- the rule that first surfaced the GPDMA
 * defect (RED ZONE #9). */
#define MPU_SENTINEL		0xA5u

/* Accel sensitivity for AFS_SEL=1 (+-4g): 8192 LSB/g (RM §4.18 / PS §6.2,
 * CLAUDE.md:116). mg = raw * 1000 / 8192. int32 headroom: worst case
 * |raw|=32768 -> |raw*1000| = 32,768,000, 65x inside int32 (design doc §5). */
#define MPU_ACCEL_LSB_PER_G	8192

/* One aligned file-static buffer for every transfer (PROJECT_DEFENSE F-6c --
 * same rule DRV_BUF/GATE_BUF follow). 32 bytes covers the 14-byte burst with
 * room to spare; nothing here is a DMA destination touched by any other
 * task, so no SCB cache-maintenance call belongs on this buffer (the RZ9-
 * adjacent mistake app_tasks.c used to make on feature_buf's D-cache
 * call, removed under PH6-1, 2026-09-05). */
static UB mpu_buf[32] __attribute__((aligned(32)));

/* D-H-class sentinel: BSS-zeroed, and E_OK is 0, so init_result must be set
 * to the "not run" value HERE, not inside mpu6050_init() -- the exact defect
 * fixed 2026-09-03 across six other sentinels (see app_mpu6050.h). */
static mpu6050_stats_t mstats = {
	.init_result = 1,		/* 1 = not run; 0 would read as E_OK */
};

/* Latched at init, used by every subsequent call. Never hard-coded --
 * design doc §1: "the fix is to wire AD0 to GND explicitly" if this is ever
 * unstable across power cycles; the probe is what decides it each boot. */
static UB mpu_addr7;


/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
static ER mpu_wr8(UB reg, UB val)
{
	mpu_buf[0] = val;
	return i2c_wr(mpu_addr7, (UW)reg, I2C_REG8, mpu_buf, 1);
}

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
static ER mpu_rd8(UB reg, UB *out)
{
	ER err;

	mpu_buf[0] = MPU_SENTINEL;
	err = i2c_rd(mpu_addr7, (UW)reg, I2C_REG8, mpu_buf, 1);
	if (err == E_OK)
		*out = mpu_buf[0];
	return err;
}

void mpu6050_gpio_init(void)
{
	GPIO_InitTypeDef gpio_init = {0};

	__HAL_RCC_GPIOE_CLK_ENABLE();	/* already on -- shared with DRV_EN/TRIG */

	gpio_init.Mode  = GPIO_MODE_INPUT;
	gpio_init.Pull  = GPIO_NOPULL;	/* push-pull driver once INT_PIN_CFG is
					 * written (INT_OPEN=0) -- no pull needed */
	gpio_init.Speed = GPIO_SPEED_FREQ_LOW;
	gpio_init.Pin   = MPU_INT_PIN;
	HAL_GPIO_Init(MPU_INT_PORT, &gpio_init);
}

/*
 * Address probe (design doc §1). Tries 0x68 (AD0 low) then 0x69 (AD0 high),
 * WHO_AM_I read at each. The address that ACKs is latched into mpu_addr7 for
 * every later call -- never hard-coded, never guessed. Distinct from
 * app_i2c_gate_test(): that function's probe loop stops at the FIRST ACK
 * across all three devices, and the DRV2605L (0x5A) always answers first on
 * this bus -- so it never actually reaches the 0x68/0x69 probes despite
 * carrying them. This driver owns its own resolution.
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 */
static ER mpu_probe_address(void)
{
	static const UB candidates[2] = { MPU_ADDR7_LOW, MPU_ADDR7_HIGH };
	UINT i;
	ER err = E_NOEXS;

	for (i = 0; i < 2u; i++) {
		UB who = MPU_SENTINEL;

		mpu_buf[0] = MPU_SENTINEL;
		err = i2c_rd(candidates[i], (UW)MPU_REG_WHO_AM_I, I2C_REG8,
			     mpu_buf, 1);
		if (err != E_OK)
			continue;	/* NACK -- try the other candidate */

		who = mpu_buf[0];
		mpu_addr7 = candidates[i];
		mstats.addr7 = candidates[i];
		mstats.whoami = (UW)who;

		/* WHO_AM_I does not reflect AD0 (RM §4.34) -- it must read
		 * 0x68 regardless of which address ACKed. A device that ACKs
		 * but reads back something else is not this part; fail
		 * loudly rather than proceed on a guess. */
		if (who != MPU_WHOAMI_EXPECT) {
			err = E_NOEXS;
			continue;
		}
		return E_OK;
	}
	return err;	/* neither address ACKed, or WHO_AM_I never matched */
}

/*
 * Config chain: one row of { register, value } per design doc §3 table,
 * written AFTER the wake step. Order matters only insofar as PWR_MGMT_1's
 * wake write must precede these -- the RM does not state whether config
 * writes land during sleep, so mpu6050_init() does not rely on it (same
 * caution the design doc states verbatim).
 */
static const struct {
	UB reg;
	UB val;
} mpu_cfg[] = {
	{ MPU_REG_CONFIG,       MPU_DLPF_CFG_4 },
	{ MPU_REG_SMPLRT_DIV,   MPU_SMPLRT_DIV_50HZ },
	{ MPU_REG_GYRO_CONFIG,  MPU_GYRO_FS_250DPS },
	{ MPU_REG_ACCEL_CONFIG, MPU_ACCEL_FS_4G },
	{ MPU_REG_INT_PIN_CFG,  MPU_INT_LATCH_RDCLR },
	{ MPU_REG_INT_ENABLE,   MPU_INT_DATA_RDY_EN },
};

#define MPU_CFG_COUNT	(sizeof(mpu_cfg) / sizeof(mpu_cfg[0]))

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
ER mpu6050_init(void)
{
	UB v;
	ER err;
	UINT i;

	mstats.init_result = 1;	/* 1 = not run; 0 would read as E_OK */
	mstats.armed = 0u;

	/* Address + identity gate BEFORE any write (design doc §1, §6 F1). */
	err = mpu_probe_address();
	if (err != E_OK)
		goto done;

	/*
	 * DEVICE_RESET, bounded and paced -- closes audit M-2. An unbounded
	 * poll on a device that never clears bit7 (unpowered, wedged) would
	 * livelock the sensor pipeline permanently; a failed read mid-reset
	 * is EXPECTED (the part may NACK while its internal reset is still
	 * in flight), so it is retried, not treated as fatal -- same pattern
	 * as drv2605l_init()'s DEV_RESET poll (app_drv2605l.c).
	 */
	err = mpu_wr8(MPU_REG_PWR_MGMT_1, MPU_PWR_DEV_RESET);
	if (err != E_OK)
		goto done;

	v = MPU_SENTINEL;
	for (i = 0; i < MPU_RESET_POLL_MAX; i++) {
		(void)tk_dly_tsk(1);
		mstats.rst_polls++;
		if (mpu_rd8(MPU_REG_PWR_MGMT_1, &v) != E_OK)
			continue;		/* NACK mid-reset is expected */
		if ((v & MPU_PWR_DEV_RESET) == 0u)
			break;
	}
	mstats.rst_pwrmgmt_rb = (UW)v;
	if (i >= MPU_RESET_POLL_MAX) {
		err = E_TMOUT;		/* DEVICE_RESET never self-cleared */
		goto done;
	}
	if (v != MPU_PWR_RESET_VALUE) {
		err = E_OBJ;		/* reset did not restore defaults */
		goto done;
	}

	/* Wake: SLEEP=0, CLKSEL=1 (PLL w/ X-gyro reference) -- "highly
	 * recommended... for improved stability" over the internal
	 * oscillator (RM §4.30). Gyro stays powered for this even though the
	 * feature vector uses no gyro features: standby-ing the clocking
	 * axis auto-switches to the +-5%-tolerance internal oscillator and
	 * drags the 50 Hz timebase with it (RM §4.31). */
	err = mpu_wr8(MPU_REG_PWR_MGMT_1, MPU_PWR_WAKE_PLL_XG);
	if (err != E_OK)
		goto done;

	/* PLL settling 1-10 ms (PS §6.6), gyro ZRO settling 30 ms (PS §6.1),
	 * accel wake >= 4 ms (RM §4.28) -- one delay covers all three. */
	(void)tk_dly_tsk(50);

	for (i = 0; i < MPU_CFG_COUNT; i++) {
		err = mpu_wr8(mpu_cfg[i].reg, mpu_cfg[i].val);
		if (err != E_OK)
			goto done;
	}

	/* Readback every config register -- one 1-byte read each, catches any
	 * silently-failed write before first-light data is trusted (design
	 * doc §3 step 10). PWR_MGMT_1 is read again too: 0x01 discriminates
	 * cleanly from the 0x40 this boot already proved via the reset gate
	 * above, so it is the strongest single proof these writes landed on
	 * THIS boot and not a previous one (the exact hole H-D7 closed for
	 * the DRV2605L, app_drv2605l.c "DEV_RESET FIRST"). */
	err = mpu_rd8(MPU_REG_PWR_MGMT_1, &v);
	if (err != E_OK)
		goto done;
	mstats.pwrmgmt_rb = (UW)v;

	err = mpu_rd8(MPU_REG_SMPLRT_DIV, &v);
	if (err != E_OK)
		goto done;
	mstats.smplrt_rb = (UW)v;

	err = mpu_rd8(MPU_REG_CONFIG, &v);
	if (err != E_OK)
		goto done;
	mstats.cfg_rb = (UW)v;

	/* Read and store for observability, but NOT part of the pass
	 * condition below -- see app_mpu6050.h's gyro_cfg_rb comment. The
	 * written value (0x00) equals the POR reset value, so this readback
	 * cannot tell "my write landed" from "the part reset". */
	err = mpu_rd8(MPU_REG_GYRO_CONFIG, &v);
	if (err != E_OK)
		goto done;
	mstats.gyro_cfg_rb = (UW)v;

	err = mpu_rd8(MPU_REG_ACCEL_CONFIG, &v);
	if (err != E_OK)
		goto done;
	mstats.accel_cfg_rb = (UW)v;

	err = mpu_rd8(MPU_REG_INT_PIN_CFG, &v);
	if (err != E_OK)
		goto done;
	mstats.int_cfg_rb = (UW)v;

	err = mpu_rd8(MPU_REG_INT_ENABLE, &v);
	if (err != E_OK)
		goto done;
	mstats.int_en_rb = (UW)v;

	/* PASS CONDITION -- every register here differs from its POR reset
	 * value at the value actually written, so each one discriminates a
	 * real write from a stale/reset state. gyro_cfg_rb is deliberately
	 * absent (see above). */
	if (mstats.pwrmgmt_rb   != MPU_PWR_WAKE_PLL_XG ||
	    mstats.smplrt_rb    != MPU_SMPLRT_DIV_50HZ ||
	    mstats.cfg_rb       != MPU_DLPF_CFG_4 ||
	    mstats.accel_cfg_rb != MPU_ACCEL_FS_4G ||
	    mstats.int_cfg_rb   != MPU_INT_LATCH_RDCLR ||
	    mstats.int_en_rb    != MPU_INT_DATA_RDY_EN) {
		err = E_OBJ;		/* transfers fine, device not configured */
		goto done;
	}

	mstats.armed = 1u;
	err = E_OK;

done:
	mstats.init_result = (W)err;
	return err;
}

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
void mpu6050_service(void)
{
	W  ax_raw, ay_raw, az_raw;
	UW awake_now = 0;	/* PH6-1: live PWR_MGMT_1 re-check said awake THIS cycle */

	if (!mstats.armed) {
		mstats.last_valid = 0;	/* PH6-1: no device, no valid sample */
		return;
	}

	/* Live PWR_MGMT_1 re-check (2026-09-04). See the field comment on
	 * pwrmgmt_live_rb in app_mpu6050.h -- init only proves wake ONCE, at
	 * boot; this catches the part reverting to SLEEP afterward (leading
	 * suspect: measured sensor VCC of 2.43 V against the board's 3.32 V
	 * rail). One extra cheap 1-byte read alongside the 14-byte burst
	 * below is nothing on a bus already carrying the DRV2605L and
	 * VL53L1X. */
	{
		UB pm;
		if (mpu_rd8(MPU_REG_PWR_MGMT_1, &pm) == E_OK) {
			mstats.pwrmgmt_live_rb = (UW)pm;
			if (pm != MPU_PWR_WAKE_PLL_XG)
				mstats.pwrmgmt_drift++;	/* device is asleep right now */
			else
				awake_now = 1;		/* PH6-1 validity input */
		} else {
			mstats.pwrmgmt_live_rderr++;
		}
	}

	/* INT no longer GATES the read (2026-09-04, hardware finding). Design
	 * doc §4 Option A was latched-level polling on PE9 -- INT is
	 * active-high, push-pull, latched until cleared, and any register
	 * read clears it (INT_RD_CLEAR=1, RM §4.15). That held for an
	 * initial burst of ~120 samples right after arming, then DATA_RDY
	 * stopped asserting entirely: a 10-minute capture afterward showed
	 * reads=120 frozen while stale climbed at the full ~47 Hz poll rate,
	 * zero new samples the whole window. Wiring continuity was checked
	 * (good) and the driver-side GPIO config was independently
	 * re-verified (no other code touches PE9/GPIOE pin 9) -- both ruled
	 * out. Remaining candidates: a joint that only holds contact under
	 * probe pressure (same failure class as H-D9), or the DATA_RDY
	 * unreliability documented against cheap GY-521 clone boards
	 * specifically. Rather than block on which, the burst read below now
	 * runs UNCONDITIONALLY every service() call. The pin is still read
	 * and counted below, but purely as an observability signal for
	 * whichever cause it turns out to be -- it no longer decides whether
	 * a sample is taken. */
	if (HAL_GPIO_ReadPin(MPU_INT_PORT, MPU_INT_PIN) != GPIO_PIN_SET)
		mstats.stale++;		/* diagnostic only -- does NOT gate the read below */

	/* One 14-byte burst from ACCEL_XOUT_H. The double-banked registers
	 * only guarantee same-instant coherence across a single burst (RM
	 * §4.18-4.20) -- six single reads could tear a sample across two
	 * instants. Reading all 14 (accel+temp+gyro) rather than two 6-byte
	 * reads costs 2 wasted bytes and buys one transaction, one semaphore
	 * round-trip, and the coherence guarantee across accel AND gyro
	 * (design doc §2.3), even though only accel feeds the feature vector. */
	if (i2c_rd(mpu_addr7, (UW)MPU_REG_ACCEL_XOUT_H, I2C_REG8,
		   mpu_buf, 14) != E_OK) {
		mstats.rderr++;		/* leave ax/ay/az at their last value */
		mstats.last_valid = 0;	/* PH6-1: stale hold-over, not a fresh sample */
		return;
	}

	/* Big-endian reassembly, explicit -- same rule as the ToF shim, no
	 * memcpy (design doc §2.1). buf[0..5] = ax,ay,az; buf[6..7] = temp
	 * (unused); buf[8..13] = gyro (unused, but read for coherence). */
	ax_raw = (W)(H)(((UH)mpu_buf[0] << 8) | (UH)mpu_buf[1]);
	ay_raw = (W)(H)(((UH)mpu_buf[2] << 8) | (UH)mpu_buf[3]);
	az_raw = (W)(H)(((UH)mpu_buf[4] << 8) | (UH)mpu_buf[5]);

	/* mg = raw * 1000 / 8192, integer only (design doc §5, headroom
	 * verified there: worst case 65x inside int32). */
	mstats.ax_mg = (ax_raw * 1000) / MPU_ACCEL_LSB_PER_G;
	mstats.ay_mg = (ay_raw * 1000) / MPU_ACCEL_LSB_PER_G;
	mstats.az_mg = (az_raw * 1000) / MPU_ACCEL_LSB_PER_G;

	mstats.reads++;
	/* PH6-1: fresh sample AND confirmed-awake this cycle -- see the
	 * field comment on last_valid (app_mpu6050.h) for why this is not
	 * gated on INT/DATA_RDY. */
	mstats.last_valid = awake_now;
}

const mpu6050_stats_t *mpu6050_get_stats(void)
{
	return &mstats;
}
