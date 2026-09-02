/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_vl53l1_port.c — VL53L1X ULD platform shim (Haptic-Sense), Phase 5 Block 2
 *
 * Implements the nine functions ST's ULD leaves to the integrator, on top of
 * this project's L1 primitive (`i2c_rd` / `i2c_wr`, app_i2c.c).
 *
 * WRITTEN FRESH RATHER THAN FILLING ST's STUB IN PLACE. ST's
 * Lib/STSW-IMG009/.../API/platform/vl53l1_platform.c is under the ST SLA and
 * carries their copyright header; this file is Apache-2.0 and is ours.
 * CLAUDE.md §7: never mix headers. ST's stub is renamed out of the build —
 * see the note beside it. The interface header `vl53l1_platform.h` is still
 * ST's and is still included, which is ordinary interface use, exactly as
 * app_i2c.c includes stm32n6xx_hal.h.
 *
 * TASK CONTEXT. Every function here reaches the I2C bus and therefore runs in
 * sensor_task, TK_PRI 3 (CLAUDE.md §3). The ULD is called only from there.
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 *
 * ------------------------------------------------------------------------
 * THE THREE THINGS THAT MAKE THIS FILE DANGEROUS
 * ------------------------------------------------------------------------
 *
 * 1. ADDRESS WIDTH. The ULD passes an EIGHT-bit address; `i2c_rd`/`i2c_wr`
 *    take a SEVEN-bit one and shift internally (app_i2c.c). So `dev >> 1`.
 *    Get it wrong and the part simply never ACKs — loud, at least.
 *    Guarded here by parity, which happens to be exact for this device: the
 *    correct 8-bit address 0x52 is EVEN, and the 7-bit 0x29 someone would
 *    wrongly pass instead is ODD. An odd `dev` is rejected outright.
 *
 * 2. REGISTER-ADDRESS WIDTH. The VL53L1X uses SIXTEEN-bit register indices.
 *    Every call below passes `I2C_REG16` **as the symbol, never the literal
 *    2**. A literal falls silently to the 8-bit branch of the dispatch in
 *    app_i2c.c, the sensor still ACKs, every read returns the wrong register,
 *    and nothing anywhere reports an error. This is the single highest-risk
 *    line in Block 2 and it fails as wrong DATA, not as a fault.
 *    A _Static_assert below refuses to build if the two codes ever collide.
 *
 * 3. DMA BUFFER OWNERSHIP (PROJECT_DEFENSE F-6b). The ULD hands us pointers
 *    to whatever it likes — usually a local array on the sensor task's stack,
 *    of unknown alignment. Those must never be DMA endpoints. Every transfer
 *    here bounces through one aligned file-static buffer and the payload is
 *    memcpy'd across. D-cache is OFF today (app_config.h:21) so this is
 *    currently belt-and-braces, but it is exactly the precondition D-cache
 *    enablement is gated on, and it costs 17 bytes of copying.
 *
 * ------------------------------------------------------------------------
 * ENDIANNESS. The VL53L1X is BIG-ENDIAN on the wire; Cortex-M is little.
 * Multi-byte values are assembled MSB-first by hand below. Confirmed against
 * the ULD's own expectations: VL53L1X_api.c:379 tests `case 0x001D` on a
 * value returned by VL53L1_RdWord, i.e. wire bytes 0x00 0x1D.
 * ------------------------------------------------------------------------
 */

#include "app_vl53l1_port.h"
#include "app_i2c.h"
#include "vl53l1_platform.h"
#include <string.h>

/* ST's ULD returns int8_t: 0 = OK, non-zero = error. Distinct small codes so
 * a failure is identifiable from the printed value; the real ER is latched in
 * the stats block, because int8_t cannot carry it usefully. */
#define PORT_OK			0
#define PORT_E_XFER		(-1)	/* i2c_rd / i2c_wr failed          */
#define PORT_E_PARAM		(-2)	/* bad address, size, NULL pointer */

/* 7-bit address after the shift. CLAUDE.md §2: VL53L1X at 0x29, XSHUT left
 * unconnected, single device — the MB1854 camera FFC must stay out of CN14
 * because its VL53L5CX also answers 0x29. */
#define VL53L1X_ADDR7		0x29u

/*
 * Bounce buffer. Sized from the ULD's own worst case, not from a guess:
 * VL53L1X_api.c:593 is the only multi-byte transfer the core API performs,
 * `VL53L1_ReadMulti(dev, VL53L1_RESULT__RANGE_STATUS, Temp, 17)`. The 91-byte
 * default configuration table goes out one WrByte at a time (:185), not as a
 * block. 32 bytes gives most of a cache line of headroom; `max_count` in the
 * stats reports the largest ever seen so the margin is observable rather than
 * assumed. Anything larger is REFUSED, not truncated.
 */
/*
 * DIAGNOSTIC: INTER-WRITE SPACING. Default 0 = off, no behaviour change.
 *
 * VL53L1X_SensorInit fires 91 WrBytes back to back with no delay of any
 * kind. Each one is a full HAL_I2C_Mem_Write_DMA + semaphore round trip, so
 * the gap between one STOP and the next START is only task-level overhead --
 * microseconds. If HAL sees I2C_FLAG_BUSY still set when the next transfer
 * starts it returns HAL_BUSY, i2c_xfer_once maps that to E_IO, and i2c_xfer
 * answers E_IO with a ~45 ms bus recovery plus a retry. Fifty-odd failures
 * concentrated in the only back-to-back write burst in the project would
 * look exactly like that, and would vary run to run because it is a race.
 *
 * SET THIS TO 1 AND REBUILD TO TEST THAT HYPOTHESIS. It spaces every ULD
 * write by one kernel tick (1-2 ms), which adds ~180 ms to SensorInit and is
 * irrelevant there. If [RNG] xerr drops to 0, the cause is transfer spacing
 * and the real fix is to handle HAL_BUSY with a short re-arm instead of a
 * full bus recovery. If xerr is unchanged, spacing is NOT the cause and you
 * have eliminated it for the price of one rebuild.
 *
 * PUT IT BACK TO 0 once the question is answered -- it also costs the frame
 * loop 1-2 ms per ClearInterrupt, which is real budget at 20 ms.
 */
#define VL53L1_PORT_WRITE_GAP_MS	0

/*
 * WRITE RETRY THROUGH A REFUSAL WINDOW. Added 2026-09-01 from measurement.
 *
 * MEASURED, not assumed (docs/evidence/phase5/PHASE5_SENSORINIT_NACK_20260901.md,
 * logic-analyzer capture sensorinit_fail_20260901.sr decoded on the wire):
 * partway through VL53L1X_SensorInit's 91-write block -- after 30 or 31 writes
 * have ACKed, i.e. ~8.7 ms of sustained traffic -- the VL53L1X stops
 * acknowledging its own ADDRESS entirely. Not a data NACK: the address byte
 * itself goes unanswered. It stays that way for 12.8 ms +/- 0.1 ms, identical
 * across three boots, then resumes and serves 4,236 reads without a single
 * failure. Registers 0x004C..0x0087 -- 60 of the 91 -- are simply never
 * written, and the SensorInit data-ready timeout is a downstream symptom.
 *
 * Retrying here rides through that window. 25 attempts paced by tk_dly_tsk(1)
 * covers 25-50 ms against a measured 12.8 ms, so ~13 attempts is the expected
 * worst case and there is nearly 2x margin.
 *
 * THIS IS A MITIGATION, NOT A DIAGNOSIS. The root cause is still open --
 * XSHUT is measured solid (never once low across a full 8 s capture), so the
 * live candidates are an AVDD sag on the 7SEMI's own rail (XSHUT idles at
 * 2.62 V, not the 3.32 V system rail, and DS12385 gives AVDD a 2.6 V minimum)
 * or an internal busy state in the part. Because it is a mitigation, it is
 * COUNTED AND PRINTED, never silent: wr_retries, wr_retry_max and wr_refused
 * appear on the [PRT] heartbeat line. A sensor that needs retries on the bench
 * may need more of them in a courier-shaken box in Japan, and a counter that
 * quietly absorbs a hardware fault is exactly the defect class this project
 * keeps finding.
 *
 * Only an E_NOEXS refusal is retried. E_IO and E_TMOUT already get one retry
 * plus a bus recovery inside i2c_xfer() and mean something different.
 * Set to 0 to disable and see the raw behaviour.
 */
#define VL53L1_PORT_WRITE_RETRY	25

#define PORT_BUF_SZ		32u
static UB port_buf[PORT_BUF_SZ] __attribute__((aligned(32)));

static vl53l1_port_stats_t pstats;

/* Build-time guard for danger #2. If these two ever became equal, every
 * I2C_REG16 call in this file would silently become an 8-bit one. */
_Static_assert(I2C_REG16 != I2C_REG8,
               "I2C_REG16 and I2C_REG8 must be distinct — see danger #2 above");

/* ------------------------------------------------------------------------ */
/* Shared entry checks. Returns PORT_OK, or a code with stats already        */
/* updated. `dev7_out` receives the shifted 7-bit address.                   */
/* ------------------------------------------------------------------------ */
static int8_t port_check(uint16_t dev, uint32_t count, const void *p, UB *dev7_out)
{
	pstats.calls++;
	pstats.last_addr8 = (UW)dev;

	if (count > pstats.max_count)
		pstats.max_count = count;

	/* Danger #1. An 8-bit I2C address is always even — bit 0 is the R/W
	 * flag. 0x52 is even; the 7-bit 0x29 that someone would mistakenly
	 * pass in its place is odd. So parity alone catches the exact
	 * confusion this shim exists to get right. */
	if ((dev & 0x01u) != 0u) {
		pstats.param_err++;
		return PORT_E_PARAM;
	}

	if (p == NULL || count == 0u || count > PORT_BUF_SZ) {
		pstats.param_err++;
		return PORT_E_PARAM;
	}

	*dev7_out = (UB)(dev >> 1);	/* <-- danger #1, the whole point */
	return PORT_OK;
}

static int8_t port_fail(ER err, uint16_t index)
{
	pstats.xfer_err++;
	pstats.last_er = (W)err;
	pstats.last_index = (UW)index;
	return PORT_E_XFER;
}

/* ------------------------------------------------------------------------ */
/* The nine. Signatures are ST's (vl53l1_platform.h) and must not change.    */
/* ------------------------------------------------------------------------ */

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
int8_t VL53L1_WriteMulti(uint16_t dev, uint16_t index, uint8_t *pdata, uint32_t count)
{
	UB   dev7;
	ER   err;
	int8_t st = port_check(dev, count, pdata, &dev7);

	if (st != PORT_OK)
		return st;

	memcpy(port_buf, pdata, (size_t)count);	/* F-6b: never DMA the caller's buffer */
	err = i2c_wr(dev7, (UW)index, I2C_REG16, port_buf, (UW)count);

#if VL53L1_PORT_WRITE_RETRY > 0
	/* Ride through the measured 12.8 ms address-refusal window. Counted,
	 * never silent -- see the note beside the #define. */
	if (err == E_NOEXS) {
		UINT tries;

		for (tries = 1u; tries <= (UINT)VL53L1_PORT_WRITE_RETRY; tries++) {
			tk_dly_tsk(1);
			memcpy(port_buf, pdata, (size_t)count);
			err = i2c_wr(dev7, (UW)index, I2C_REG16, port_buf, (UW)count);
			pstats.wr_retries++;
			if (err != E_NOEXS)
				break;
		}
		/* D-7. On exhaustion the for-loop exits with `tries` one PAST
		 * the bound, so an unclamped max prints 26 for a 25-retry
		 * limit. Clamp: this counter must never exceed its own bound.
		 * Units: RETRY attempts made (total attempts - 1). */
		if (tries > (UINT)VL53L1_PORT_WRITE_RETRY)
			tries = (UINT)VL53L1_PORT_WRITE_RETRY;
		if (tries > pstats.wr_retry_max)
			pstats.wr_retry_max = tries;
		if (err == E_NOEXS)
			pstats.wr_refused++;
	}
#endif

#if VL53L1_PORT_WRITE_GAP_MS > 0
	/* Diagnostic only -- see the note beside the #define. */
	tk_dly_tsk((RELTIM)VL53L1_PORT_WRITE_GAP_MS);
#endif

	return (err == E_OK) ? PORT_OK : port_fail(err, index);
}

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
int8_t VL53L1_ReadMulti(uint16_t dev, uint16_t index, uint8_t *pdata, uint32_t count)
{
	UB   dev7;
	ER   err;
	int8_t st = port_check(dev, count, pdata, &dev7);

	if (st != PORT_OK)
		return st;

	/* Sentinel pre-fill, the rule that surfaced the GPDMA defect (CLAUDE.md
	 * §3): 0xA5 is neither a plausible register value nor the 0xFF an idle
	 * pulled-up bus returns, so a buffer the DMA never touched is provable
	 * rather than indistinguishable from a real reading. */
	memset(port_buf, 0xA5, (size_t)count);

	err = i2c_rd(dev7, (UW)index, I2C_REG16, port_buf, (UW)count);

#if VL53L1_PORT_WRITE_RETRY > 0
	/* READS ARE RETRIED TOO (2026-09-01). Once the configuration block
	 * actually lands, the part starts refusing READS as well -- 845 of
	 * ~2000 poll-loop reads in the first retry build. Same refusal, same
	 * recovery, so the same bounded ride-through applies. Counted
	 * separately from writes so the two are never conflated. */
	if (err == E_NOEXS) {
		UINT tries;

		for (tries = 1u; tries <= (UINT)VL53L1_PORT_WRITE_RETRY; tries++) {
			tk_dly_tsk(1);
			memset(port_buf, 0xA5, (size_t)count);
			err = i2c_rd(dev7, (UW)index, I2C_REG16, port_buf, (UW)count);
			pstats.rd_retries++;
			if (err != E_NOEXS)
				break;
		}
		/* D-7. On exhaustion the for-loop exits with `tries` one PAST
		 * the bound, so an unclamped max prints 26 for a 25-retry
		 * limit. Clamp: this counter must never exceed its own bound.
		 * Units: RETRY attempts made (total attempts - 1). */
		if (tries > (UINT)VL53L1_PORT_WRITE_RETRY)
			tries = (UINT)VL53L1_PORT_WRITE_RETRY;
		if (tries > pstats.rd_retry_max)
			pstats.rd_retry_max = tries;
		if (err == E_NOEXS)
			pstats.rd_refused++;
	}
#endif

	if (err != E_OK)
		return port_fail(err, index);

	memcpy(pdata, port_buf, (size_t)count);
	return PORT_OK;
}

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
int8_t VL53L1_WrByte(uint16_t dev, uint16_t index, uint8_t data)
{
	return VL53L1_WriteMulti(dev, index, &data, 1);
}

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
int8_t VL53L1_WrWord(uint16_t dev, uint16_t index, uint16_t data)
{
	uint8_t b[2];

	b[0] = (uint8_t)(data >> 8);		/* big-endian on the wire */
	b[1] = (uint8_t)(data & 0xFFu);
	return VL53L1_WriteMulti(dev, index, b, 2);
}

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
int8_t VL53L1_WrDWord(uint16_t dev, uint16_t index, uint32_t data)
{
	uint8_t b[4];

	b[0] = (uint8_t)(data >> 24);
	b[1] = (uint8_t)(data >> 16);
	b[2] = (uint8_t)(data >> 8);
	b[3] = (uint8_t)(data & 0xFFu);
	return VL53L1_WriteMulti(dev, index, b, 4);
}

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
int8_t VL53L1_RdByte(uint16_t dev, uint16_t index, uint8_t *data)
{
	return VL53L1_ReadMulti(dev, index, data, 1);
}

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
int8_t VL53L1_RdWord(uint16_t dev, uint16_t index, uint16_t *data)
{
	uint8_t b[2];
	int8_t  st;

	if (data == NULL) {
		pstats.param_err++;
		return PORT_E_PARAM;
	}
	st = VL53L1_ReadMulti(dev, index, b, 2);
	if (st != PORT_OK)
		return st;

	*data = (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
	return PORT_OK;
}

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
int8_t VL53L1_RdDWord(uint16_t dev, uint16_t index, uint32_t *data)
{
	uint8_t b[4];
	int8_t  st;

	if (data == NULL) {
		pstats.param_err++;
		return PORT_E_PARAM;
	}
	st = VL53L1_ReadMulti(dev, index, b, 4);
	if (st != PORT_OK)
		return st;

	*data = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
	        ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
	return PORT_OK;
}

/*
 * Blocking delay. TK_PRI 3 only — this is the one platform function that does
 * not touch the bus, and the one that would be a disaster anywhere else.
 *
 * `tk_dly_tsk` rounds up to the next kernel tick (1 ms, CNF_TIMER_PERIOD=1),
 * so a request is never SHORTER than asked, which is the safe direction for
 * a sensor settling delay. A non-positive wait returns immediately rather
 * than yielding — the ULD calls this with computed values and 0 is legal.
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 */
int8_t VL53L1_WaitMs(uint16_t dev, int32_t wait_ms)
{
	(void)dev;
	pstats.calls++;

	if (wait_ms <= 0)
		return PORT_OK;

	/* RELTIM is UW (typedef.h:108), not TMO — tk_dly_tsk takes a RELATIVE
	 * time, unsigned. The `wait_ms <= 0` guard above is what makes this
	 * cast safe. */
	return (tk_dly_tsk((RELTIM)wait_ms) == E_OK) ? PORT_OK : PORT_E_XFER;
}

/* ------------------------------------------------------------------------ */

const vl53l1_port_stats_t *vl53l1_port_stats(void)
{
	return &pstats;
}

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
void vl53l1_port_reset_stats(void)
{
	memset(&pstats, 0, sizeof(pstats));
	pstats.last_er = 1;	/* 1 = no failure yet; 0 would read as E_OK */
}
