/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_i2c.c — Phase 5 L0/L1: semaphore-wrapped DMA I2C1 primitive (Haptic-Sense)
 *
 * Design: docs/PHASE5_L1_i2c_xfer_design.md (approved with changes 2026-07-06).
 * Option A (commit b1b51c8): this file owns ST HAL I2C and the
 * HAL_I2C_*Callback symbols directly; DEVCNF_USE_HAL_IIC stays 0 so the BSP
 * wrapper (mtk3_bsp2 hal_i2c.c) never compiles in (duplicate-symbol guard).
 *
 * Task context: EVERYTHING here runs in sensor_task, TK_PRI 3 — except the
 * four thin IRQ handlers, which run in kernel task-independent context via
 * tk_def_int(TA_HLNG) (knl_hll_inthdr brackets them; interrupt.c:33-46).
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 *
 * Semaphore naming (CLAUDE.md rule):
 *   producer = I2C/DMA completion callback (ISR context)
 *   consumer = sensor_task (TK_PRI 3), inside i2c_xfer()
 *   channel  = i2c_done_sem (init 0, max 1)
 */

#include "app_i2c.h"
#include "i2c_timing.h"
#include "stm32n6xx_hal.h"

/* ------------------------------------------------------------------------ */
/* Hardware constants — every value grounded in the design doc §1-§3         */
/* ------------------------------------------------------------------------ */

/* IRQ numbers: stm32n657xx.h:140-141,156-157 */
#define I2C1_EV_INTNO		100
#define I2C1_ER_INTNO		101
#define GPDMA1_CH0_INTNO	84	/* RX channel */
#define GPDMA1_CH1_INTNO	85	/* TX channel */

/* Kernel-safe NVIC level: 1..15 legal for tk_* from ISR; level 0 forbidden
 * (BASEPRI mask = INTPRI_VAL(INTPRI_MAX_EXTINT_PRI=1), int_armv8m.c:55-64).
 * Level 1 = SysTick's own level (sysdef.h:79). Design doc §3. */
#define I2C_IRQ_LEVEL		1

/* GPDMA channel TrustZone attributes — secure+privileged channel, secure
 * source and secure destination. Same set as scrl_spi.c:488 and ST's
 * stm32n6570_discovery_audio.c. See i2c1_dma_init() for why this is
 * mandatory rather than defensive. */
#define DMA_CHAN_ATTR		(DMA_CHANNEL_PRIV | DMA_CHANNEL_SEC | \
				 DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC)

/* ------------------------------------------------------------------------ */
/* L0 state — all static, USE_IMALLOC=0 (CLAUDE.md §7)                       */
/* ------------------------------------------------------------------------ */

static I2C_HandleTypeDef hi2c1;		/* file-local: devinit import trap is
					 * compiled out at DEVCNF_USE_HAL_IIC=0 */
static DMA_HandleTypeDef hdma_i2c1_rx;	/* GPDMA1 ch0, request 95 */
static DMA_HandleTypeDef hdma_i2c1_tx;	/* GPDMA1 ch1, request 96 */

static ID i2c_done_sem;			/* init 0, max 1 */

/* Per-transaction state. i2c_signalled is the double-fire guard (review
 * requirement 2026-07-06): HAL has exactly one ErrorCallback call site
 * (stm32n6xx_hal_i2c.c) but exactly-once across EV/ER IRQ interleavings is
 * not cheaply provable from the state machine — so the guard makes it moot.
 * Both IRQs run at the same NVIC level (can't preempt each other), so the
 * test-and-set below is sequential, not racy. */
static volatile ER   i2c_xfer_err;
static volatile BOOL i2c_signalled;
static volatile BOOL i2c_busy;

static app_i2c_stats_t stats;

/* Gate-test DMA buffer: 32-byte aligned, 32-byte padded (M55 cache line,
 * design §4.1). D-cache is OFF in this build (app_config.h:21) — rule
 * enforced anyway so a future D-cache enable cannot corrupt. */
static UB gate_buf[32] __attribute__((aligned(32)));

/* Pre-fill value for gate_buf: any byte that is neither a plausible register
 * value nor the all-ones idle-bus value, so an untouched buffer is provable. */
#define GATE_SENTINEL	0xA5u

/* ------------------------------------------------------------------------ */
/* HAL callbacks — ISR context, inside knl_hll_inthdr's TASK_INDEPENDENT     */
/* bracket (no manual ENTER/LEAVE needed, unlike BSP hal_i2c.c:120).         */
/* Producer side of i2c_done_sem.                                            */
/* ------------------------------------------------------------------------ */

static void i2c_complete(I2C_HandleTypeDef *hi2c, ER err)
{
	if (hi2c != &hi2c1)
		return;
	if (i2c_signalled)		/* double-fire guard: signal at most once */
		return;
	i2c_signalled = TRUE;
	i2c_xfer_err = err;
	tk_sig_sem(i2c_done_sem, 1);	/* producer: callback -> sensor_task */
}

void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef *hi2c) { i2c_complete(hi2c, E_OK); }
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c) { i2c_complete(hi2c, E_OK); }
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)     { i2c_complete(hi2c, E_IO); }
void HAL_I2C_AbortCpltCallback(I2C_HandleTypeDef *hi2c) { i2c_complete(hi2c, E_ABORT); }

/* ------------------------------------------------------------------------ */
/* IRQ handlers — registered via tk_def_int(TA_HLNG), enabled via EnableInt  */
/* (kernel mechanism, never bare NVIC — design §3)                           */
/* ------------------------------------------------------------------------ */

static void i2c1_ev_ihdr(UINT intno)   { (void)intno; HAL_I2C_EV_IRQHandler(&hi2c1); }
static void i2c1_er_ihdr(UINT intno)   { (void)intno; HAL_I2C_ER_IRQHandler(&hi2c1); }
static void i2c1_dmarx_ihdr(UINT intno){ (void)intno; HAL_DMA_IRQHandler(&hdma_i2c1_rx); }
static void i2c1_dmatx_ihdr(UINT intno){ (void)intno; HAL_DMA_IRQHandler(&hdma_i2c1_tx); }

/* ------------------------------------------------------------------------ */
/* L0 init — MSP, DMA, IRQ registration. sensor_task (TK_PRI 3) only.        */
/* ------------------------------------------------------------------------ */

/*
 * Pin/clock/power bring-up. Sequence adapted from ST's own MspInit for this
 * board (stm32n6570_discovery_bus.c:560-601 — cited as source, code re-typed
 * under this file's Apache license; the sequence is ST's, the file is ours).
 * ORDER MATTERS (review requirement 2026-07-06): VddIO4 FIRST — the I2C1
 * pins are on the VddIO4 power domain and are dead until it is enabled.
 */
static void i2c1_msp_init(void)
{
	GPIO_InitTypeDef gpio_init = {0};

	/* 1. Power domain BEFORE any pin config (bus.c:568) */
	HAL_PWREx_EnableVddIO4();

	/* 2. GPIO clocks (bus.c via discovery_bus.h:66-68) */
	__HAL_RCC_GPIOH_CLK_ENABLE();	/* SCL PH9 */
	__HAL_RCC_GPIOC_CLK_ENABLE();	/* SDA PC1 */

	/* 3. Pins: AF4, open-drain, NO pull — onboard 1.5 kOhm are the pull-ups
	 * (CLAUDE.md §2: add nothing). discovery_bus.h:74-79. */
	gpio_init.Mode = GPIO_MODE_AF_OD;
	gpio_init.Pull = GPIO_NOPULL;
	gpio_init.Speed = GPIO_SPEED_FREQ_HIGH;
	gpio_init.Alternate = GPIO_AF4_I2C1;

	gpio_init.Pin = GPIO_PIN_9;
	HAL_GPIO_Init(GPIOH, &gpio_init);
	gpio_init.Pin = GPIO_PIN_1;
	HAL_GPIO_Init(GPIOC, &gpio_init);

	/* 4. I2C1 peripheral clock + reset pulse (discovery_bus.h:63,70-71) */
	__HAL_RCC_I2C1_CLK_ENABLE();
	__HAL_RCC_I2C1_FORCE_RESET();
	__HAL_RCC_I2C1_RELEASE_RESET();
}

/* GPDMA channel config. Requests 95/96 = LL_GPDMA1_REQUEST_I2C1_RX/TX
 * (stm32n6xx_ll_dma.h:1287-1288); ch0=RX, ch1=TX (design decision #3). */
static ER i2c1_dma_init(void)
{
	DMA_IsolationConfigTypeDef isolation;

	__HAL_RCC_GPDMA1_CLK_ENABLE();	/* stm32n6xx_hal_rcc.h:865 */

	hdma_i2c1_rx.Instance                 = GPDMA1_Channel0;
	hdma_i2c1_rx.Init.Request             = GPDMA1_REQUEST_I2C1_RX;	/* 95, hal_dma.h:550 */
	hdma_i2c1_rx.Init.BlkHWRequest        = DMA_BREQ_SINGLE_BURST;
	hdma_i2c1_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
	hdma_i2c1_rx.Init.SrcInc              = DMA_SINC_FIXED;
	hdma_i2c1_rx.Init.DestInc             = DMA_DINC_INCREMENTED;
	hdma_i2c1_rx.Init.SrcDataWidth        = DMA_SRC_DATAWIDTH_BYTE;
	hdma_i2c1_rx.Init.DestDataWidth       = DMA_DEST_DATAWIDTH_BYTE;
	hdma_i2c1_rx.Init.Priority            = DMA_HIGH_PRIORITY;
	hdma_i2c1_rx.Init.SrcBurstLength      = 1;
	hdma_i2c1_rx.Init.DestBurstLength     = 1;
	hdma_i2c1_rx.Init.TransferAllocatedPort =
		DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT0;
	hdma_i2c1_rx.Init.TransferEventMode   = DMA_TCEM_BLOCK_TRANSFER;
	hdma_i2c1_rx.Init.Mode                = DMA_NORMAL;
	if (HAL_DMA_Init(&hdma_i2c1_rx) != HAL_OK)
		return E_IO;
	__HAL_LINKDMA(&hi2c1, hdmarx, hdma_i2c1_rx);

	hdma_i2c1_tx = hdma_i2c1_rx;	/* same template, then differences: */
	hdma_i2c1_tx.Instance             = GPDMA1_Channel1;
	hdma_i2c1_tx.Init.Request         = GPDMA1_REQUEST_I2C1_TX;	/* 96, hal_dma.h:551 */
	hdma_i2c1_tx.Init.Direction       = DMA_MEMORY_TO_PERIPH;
	hdma_i2c1_tx.Init.SrcInc          = DMA_SINC_INCREMENTED;
	hdma_i2c1_tx.Init.DestInc         = DMA_DINC_FIXED;
	if (HAL_DMA_Init(&hdma_i2c1_tx) != HAL_OK)
		return E_IO;
	__HAL_LINKDMA(&hi2c1, hdmatx, hdma_i2c1_tx);

	/* TRUSTZONE CHANNEL ATTRIBUTES — MANDATORY (added 2026-08-29).
	 *
	 * Root cause of the silent-no-transfer defect: this build is TZEN
	 * secure (GPIOO resolves to GPIOO_S; CLAUDE.md §2), the DMA buffers
	 * live in the SECURE AXISRAM alias (gate_buf @ 0x34013160, .map) and
	 * I2C1 is a secure peripheral. A GPDMA channel left at its reset
	 * attributes performs NON-SECURE accesses and can reach neither end of
	 * the transfer. It arms, moves zero bytes, and because the I2C
	 * generates its own STOP after NBYTES under AUTOEND, HAL still reaches
	 * HAL_I2C_MemRxCpltCallback and reports HAL_OK. The failure is
	 * completely silent — proven on hardware 2026-08-29: four transfers,
	 * ok=4 err=0 recov=0, and all four buffers still held the 0xA5
	 * sentinel.
	 *
	 * Pattern and CID copied from the two in-tree precedents:
	 *   Lib/screenl/Src/scrl_spi.c:488-495  (this project's own SPI5 path)
	 *   STM32Cube_FW_N6/Drivers/BSP/STM32N6570-DK/
	 *       stm32n6570_discovery_audio.c:3230, 3349, 3514  (ST's BSP)
	 * StaticCid = CID1 matches main.c:334 (RIMC_master.MasterCID =
	 * RIF_CID_1).
	 *
	 * Distinct error codes so a failure is identifiable from the printed
	 * [I2C] init= value without a debugger:
	 *   E_ID    -> ConfigChannelAttributes failed
	 *   E_NOSPT -> SetIsolationAttributes failed
	 * Attributes latch: HAL_DMA_ConfigChannelAttributes has no effect if
	 * called a second time (stm32n6xx_hal_dma.c:175), so this must run
	 * once and correctly. i2c1_bus_recover() deliberately does NOT re-run
	 * i2c1_dma_init(), so the attributes survive recovery. */
	if (HAL_DMA_ConfigChannelAttributes(&hdma_i2c1_rx, DMA_CHAN_ATTR) != HAL_OK)
		return E_ID;
	if (HAL_DMA_ConfigChannelAttributes(&hdma_i2c1_tx, DMA_CHAN_ATTR) != HAL_OK)
		return E_ID;

	isolation.CidFiltering = DMA_ISOLATION_ON;
	isolation.StaticCid    = DMA_CHANNEL_STATIC_CID_1;
	if (HAL_DMA_SetIsolationAttributes(&hdma_i2c1_rx, &isolation) != HAL_OK)
		return E_NOSPT;
	if (HAL_DMA_SetIsolationAttributes(&hdma_i2c1_tx, &isolation) != HAL_OK)
		return E_NOSPT;

	return E_OK;
}

ER app_i2c_init(void)
{
	T_CSEM sem_config;
	T_DINT dint;
	ER err;

	/* --- semaphore first: must exist before any transfer can start --- */
	sem_config.sematr  = TA_TFIFO;
	sem_config.exinf   = NULL;
	sem_config.isemcnt = 0;		/* consumer always waits for a fresh completion */
	sem_config.maxsem  = 1;		/* third layer of the double-fire defence */
	i2c_done_sem = tk_cre_sem(&sem_config);
	if (i2c_done_sem <= E_OK)
		return (ER)i2c_done_sem;

	/* --- clock logging (design §6 verification items, incl. DWT question) --- */
	stats.clk_pclk1_hz  = HAL_RCC_GetPCLK1Freq();
	stats.clk_sysclk_hz = HAL_RCC_GetSysClockFreq();

	/* --- MSP + peripheral init --- */
	i2c1_msp_init();

	hi2c1.Instance              = I2C1;
	hi2c1.Init.Timing           = I2C_GetTiming(stats.clk_pclk1_hz, I2C_BUS_HZ);
	hi2c1.Init.OwnAddress1      = 0;
	hi2c1.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
	hi2c1.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
	hi2c1.Init.OwnAddress2      = 0;
	hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
	hi2c1.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
	hi2c1.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
	if (hi2c1.Init.Timing == 0u)
		return E_PAR;		/* calculator failed: clock source unexpected */
	if (HAL_I2C_Init(&hi2c1) != HAL_OK)
		return E_IO;

	err = i2c1_dma_init();
	if (err != E_OK)
		return err;

	/* --- kernel IRQ registration (design §3): tk_def_int + EnableInt --- */
	dint.intatr = TA_HLNG;
	dint.inthdr = (FP)i2c1_ev_ihdr;
	err = tk_def_int(I2C1_EV_INTNO, &dint);
	if (err != E_OK) return err;
	dint.inthdr = (FP)i2c1_er_ihdr;
	err = tk_def_int(I2C1_ER_INTNO, &dint);
	if (err != E_OK) return err;
	dint.inthdr = (FP)i2c1_dmarx_ihdr;
	err = tk_def_int(GPDMA1_CH0_INTNO, &dint);
	if (err != E_OK) return err;
	dint.inthdr = (FP)i2c1_dmatx_ihdr;
	err = tk_def_int(GPDMA1_CH1_INTNO, &dint);
	if (err != E_OK) return err;

	EnableInt(I2C1_EV_INTNO, I2C_IRQ_LEVEL);
	EnableInt(I2C1_ER_INTNO, I2C_IRQ_LEVEL);
	EnableInt(GPDMA1_CH0_INTNO, I2C_IRQ_LEVEL);
	EnableInt(GPDMA1_CH1_INTNO, I2C_IRQ_LEVEL);

	return E_OK;
}

/* ------------------------------------------------------------------------ */
/* Bus recovery — 9 SCL pulses + STOP (design §4.4). TK_PRI 3 only.          */
/* ------------------------------------------------------------------------ */

static void i2c1_bus_recover(void)
{
	GPIO_InitTypeDef gpio_init = {0};
	INT i;

	stats.recoveries++;

	/* VERIFIED against HAL source (review item 1, 2026-07-06): the
	 * HAL_I2C_DeInit body (stm32n6xx_hal_i2c.c) disables the peripheral,
	 * calls the weak MspDeInit (not defined by us), and resets State/
	 * ErrorCode/Mode — it NEVER touches hdmarx/hdmatx (grep count 0 in
	 * both DeInit and Init). The __HAL_LINKDMA association survives
	 * recovery; no re-link needed. Defensively abort both DMA channels
	 * anyway: a botched transfer could leave a channel BUSY, and a BUSY
	 * channel rejects the next start (abort on an idle channel is a
	 * harmless HAL_ERROR). */
	(void)HAL_DMA_Abort(&hdma_i2c1_rx);
	(void)HAL_DMA_Abort(&hdma_i2c1_tx);
	(void)HAL_I2C_DeInit(&hi2c1);

	/* SCL (PH9) as GPIO open-drain output; SDA (PC1) left as input via AF
	 * release: reconfigure as plain input to observe it. */
	gpio_init.Mode = GPIO_MODE_OUTPUT_OD;
	gpio_init.Pull = GPIO_NOPULL;
	gpio_init.Speed = GPIO_SPEED_FREQ_HIGH;
	gpio_init.Pin = GPIO_PIN_9;
	HAL_GPIO_Init(GPIOH, &gpio_init);
	gpio_init.Mode = GPIO_MODE_INPUT;
	gpio_init.Pin = GPIO_PIN_1;
	HAL_GPIO_Init(GPIOC, &gpio_init);

	/* up to 9 pulses paced by tk_dly_tsk(1) — no busy wait. Kernel tick is
	 * 1 ms (CNF_TIMER_PERIOD=1, config.h:29) and tk_dly_tsk rounds up to
	 * the next tick, so each delay is 1-2 ms: recovery worst case is
	 * ~ (9 pulses x 2 delays + 3 STOP delays) x 2 ms ~= 45 ms (review
	 * item 4). Pulses stop early the moment SDA reads high. */
	for (i = 0; i < 9; i++) {
		if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_1) == GPIO_PIN_SET)
			break;
		HAL_GPIO_WritePin(GPIOH, GPIO_PIN_9, GPIO_PIN_RESET);
		tk_dly_tsk(1);
		HAL_GPIO_WritePin(GPIOH, GPIO_PIN_9, GPIO_PIN_SET);
		tk_dly_tsk(1);
	}

	/* STOP condition: SDA low -> SCL high -> SDA high */
	gpio_init.Mode = GPIO_MODE_OUTPUT_OD;
	gpio_init.Pin = GPIO_PIN_1;
	HAL_GPIO_Init(GPIOC, &gpio_init);
	HAL_GPIO_WritePin(GPIOC, GPIO_PIN_1, GPIO_PIN_RESET);
	tk_dly_tsk(1);
	HAL_GPIO_WritePin(GPIOH, GPIO_PIN_9, GPIO_PIN_SET);
	tk_dly_tsk(1);
	HAL_GPIO_WritePin(GPIOC, GPIO_PIN_1, GPIO_PIN_SET);
	tk_dly_tsk(1);

	/* re-mux to AF4 + full peripheral re-init; tk_def_int table and NVIC
	 * enables survive untouched */
	i2c1_msp_init();
	(void)HAL_I2C_Init(&hi2c1);
}

/* ------------------------------------------------------------------------ */
/* L1 core — the semaphore-wrapped transfer (design §4.2)                    */
/* ------------------------------------------------------------------------ */

static ER i2c_xfer_once(BOOL is_read, UB dev7, UW reg, UINT regsz, UB *buf, UW len)
{
	HAL_StatusTypeDef hal_sts;
	ER err;
	UW memaddsz = (regsz == I2C_REG16) ? I2C_MEMADD_SIZE_16BIT
					   : I2C_MEMADD_SIZE_8BIT;

	/* drain any stale completion (self-healing against a late double-fire
	 * that slipped a prior transaction's window; maxsem=1 bounds it to one) */
	(void)tk_wai_sem(i2c_done_sem, 1, TMO_POL);

	i2c_signalled = FALSE;		/* arm the once-only guard */
	i2c_xfer_err = E_OK;

	if (is_read)
		hal_sts = HAL_I2C_Mem_Read_DMA(&hi2c1, (uint16_t)(dev7 << 1),
					       (uint16_t)reg, (uint16_t)memaddsz,
					       buf, (uint16_t)len);
	else
		hal_sts = HAL_I2C_Mem_Write_DMA(&hi2c1, (uint16_t)(dev7 << 1),
						(uint16_t)reg, (uint16_t)memaddsz,
						buf, (uint16_t)len);
	if (hal_sts != HAL_OK)
		return E_IO;		/* bus never started */

	/* consumer: callback(P) -> i2c_done_sem -> sensor_task(C).
	 * Task sleeps; CPU free; priority 1-2 preempt at will. */
	err = tk_wai_sem(i2c_done_sem, 1, I2C_XFER_TMO_MS);
	if (err == E_TMOUT) {
		stats.timeouts++;
		(void)HAL_I2C_Master_Abort_IT(&hi2c1, (uint16_t)(dev7 << 1));	/* hal_i2c.h:663 */
		/* abort completion may signal late — next xfer's drain eats it */
		return E_TMOUT;
	}
	if (err != E_OK)
		return err;

	if (i2c_xfer_err == E_OK && is_read) {
		/* inert while D-cache is off (app_config.h:21) — kept per
		 * CLAUDE.md §3, same policy as the Phase 4 call site */
		SCB_InvalidateDCache_by_Addr((uint32_t *)buf, (int32_t)len);
	}
	return i2c_xfer_err;
}

/*
 * Public L1: one retry on failure, bus recovery between attempts on
 * E_TMOUT/E_IO. Worst case ~ (50 timeout + ~45 recovery + 50 retry) ms
 * ~= 145 ms, bounded — the caller (sensor loop) treats a failed frame as
 * DROPPED and continues; it never stalls the task permanently (review
 * requirement 2026-07-06).
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 */
static ER i2c_xfer(BOOL is_read, UB dev7, UW reg, UINT regsz, UB *buf, UW len)
{
	ER err;

	/* NOT A LOCK (review item 2, 2026-07-06): check-then-set is not atomic.
	 * This is a single-client DEBUG TRIPWIRE only — the contract is that
	 * sensor_task (TK_PRI 3) is the sole caller, so no race exists to lose.
	 * If a second client is ever added, replace with tk_loc_mtx, do not
	 * "fix" this flag. */
	if (i2c_busy)
		return E_OBJ;		/* single-client contract violated */
	i2c_busy = TRUE;

	err = i2c_xfer_once(is_read, dev7, reg, regsz, buf, len);
	if (err != E_OK) {
		if (err == E_TMOUT || err == E_IO)
			i2c1_bus_recover();
		err = i2c_xfer_once(is_read, dev7, reg, regsz, buf, len);
	}

	if (err == E_OK)
		stats.xfer_ok++;
	else
		stats.xfer_err++;

	i2c_busy = FALSE;
	return err;
}

ER i2c_rd(UB dev7, UW reg, UINT regsz, UB *buf, UW len)
{
	return i2c_xfer(TRUE, dev7, reg, regsz, buf, len);
}

ER i2c_wr(UB dev7, UW reg, UINT regsz, const UB *buf, UW len)
{
	return i2c_xfer(FALSE, dev7, reg, regsz, (UB *)buf, len);
}

/* ------------------------------------------------------------------------ */
/* MANDATORY GATE (review requirement): standalone L1 DMA proof              */
/* ------------------------------------------------------------------------ */

/*
 * One register read through the full chain (HAL DMA -> GPDMA -> IRQ ->
 * callback -> semaphore) BEFORE any L2/ULD code exists. If GPDMA1 lacks
 * RIF/TrustZone master attributes, THIS is where it faults — in isolation.
 *
 * PROBE TABLE (2026-08-29): 0x5A first — the SmartElex DRV2605L is the only
 * part soldered today. 0x68/0x69 follow as NEGATIVE CONTROLS: with no IMU on
 * the bus they must NACK, which proves the probe discriminates rather than
 * ACKing everything. First ACK wins and the loop stops, so a healthy DRV2605L
 * costs one transaction.
 *
 * DRV2605L expectations, ALL verified against the local datasheet copy
 * docs/datasheets/drv2605l_datasheet.pdf (TI SLOS854D Rev D, March 2018):
 *   - 7-bit address 0x5A                                     (§8.5.1.1; also
 *     silkscreened "I2C ADDR 0x5A" on the SmartElex board, photo 2026-08-29)
 *   - register 0x00 = STATUS, reset value 0xE0               (§8.6 reg map)
 *   - STATUS bits 7:5 = DEVICE_ID, RO, default 7 = DRV2605L  (§8.6.1 Table 4)
 *     3 = DRV2605 (non-L), 4 = DRV2604, 6 = DRV2604L.
 *     => PASS is whoami == 0xE0. The value 3 quoted in the 2026-08-29 handoff
 *     §5.4 was wrong; it is the non-L part number.
 *   - EN low: the device "can still acknowledge (ACK) during an I2C
 *     transaction, however, no read or write is possible"    (§8.4.1.3)
 *     => gate_result == E_OK with a garbage whoami means the EN jumper is the
 *     fault, NOT the solder joints or the bus.
 *
 * MPU6050 expectations (not soldered yet, negative control only): WHO_AM_I at
 * register 0x75 reads 0x68 regardless of AD0; which address ACKs is the AD0
 * evidence. Verified RM-MPU-6000A rev 4.0 §4.34, per
 * docs/design/mpu6050_port_design_v1.md §0.
 *
 * FALSE-FAILURE IMMUNITY (review item 3, 2026-07-06): gate_result==E_OK
 * requires only that the transaction COMPLETES through the full chain
 * (device-address ACK -> index write -> repeated-start read -> DMA -> IRQ ->
 * callback -> semaphore). Register-mapped I2C slaves ACK any register index
 * — a wrong index returns wrong DATA, it does not NACK — so a wrong register
 * constant below can only change the printed whoami byte, never flip the gate
 * to failure. The gate fails ONLY on real bus/DMA/IRQ failure.
 *
 * COST NOTE: i2c_xfer() treats a NACK (E_IO from HAL_I2C_ERROR_AF) as a fault
 * and runs i2c1_bus_recover() + one retry, ~145 ms per absent address. Three
 * absent addresses therefore cost ~435 ms ONCE at boot and leave recoveries
 * ~6. That is a known defect of the shared path, not of this gate — the fix
 * (capture hi2c->ErrorCode, map AF to a distinct code, skip recovery on a
 * plain NACK) is deferred to the bus-scan work.
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 */
/*
 * H-D9 — WRITE-DIRECTION PROOF (2026-08-30). TK_PRI 3 only.
 *
 * Until this passes, `hdma_i2c1_tx` has never moved a byte in this project:
 * a HAL Mem_Read sends its register index through the TXIS interrupt, not
 * through TX DMA, so the four reads that proved the RX path say nothing about
 * the TX path. Every device driver in this design is writes -- VL53L1X_SensorInit
 * alone is ~91 of them -- so this is the gate in front of all of them.
 *
 * Target: DRV2605L register 0x02 RTP_INPUT, reset value 0x00 (SLOS854D Table 3).
 * Safe to scribble on: MODE (0x01) resets to 0x40, i.e. STANDBY=1 with MODE[2:0]
 * = 0 (internal trigger), so RTP mode is not selected and the register drives
 * nothing; no motor is connected either. The original value is read first and
 * restored afterwards, and the restore is itself verified -- so a pass proves
 * TWO independent writes, not one.
 *
 * Every DMA touches gate_buf only (F-6c: one aligned file-static buffer per
 * transfer), pre-filled with GATE_SENTINEL before each read so an untransferred
 * buffer can never be mistaken for a reading.
 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK
 */
#define DRV_ADDR7		0x5Au
#define DRV_REG_RTP_INPUT	0x02u
#define DRV_SCRATCH		0x27u	/* not the sentinel, not the reset value */

static ER drv2605l_write_probe(void)
{
	UB orig;		/* CPU-only copy; never a DMA endpoint */
	ER err;

	gate_buf[0] = GATE_SENTINEL;
	err = i2c_rd(DRV_ADDR7, DRV_REG_RTP_INPUT, I2C_REG8, gate_buf, 1);
	if (err != E_OK)
		return err;
	orig = gate_buf[0];

	gate_buf[0] = DRV_SCRATCH;
	err = i2c_wr(DRV_ADDR7, DRV_REG_RTP_INPUT, I2C_REG8, gate_buf, 1);
	if (err != E_OK)
		return err;		/* the transfer itself failed */

	gate_buf[0] = GATE_SENTINEL;
	err = i2c_rd(DRV_ADDR7, DRV_REG_RTP_INPUT, I2C_REG8, gate_buf, 1);
	if (err != E_OK)
		return err;
	stats.gate_wr_seen = gate_buf[0];
	if (gate_buf[0] != DRV_SCRATCH)
		return E_IO;		/* transfer "succeeded" but the byte never landed */

	gate_buf[0] = orig;
	err = i2c_wr(DRV_ADDR7, DRV_REG_RTP_INPUT, I2C_REG8, gate_buf, 1);
	if (err != E_OK)
		return err;

	gate_buf[0] = GATE_SENTINEL;
	err = i2c_rd(DRV_ADDR7, DRV_REG_RTP_INPUT, I2C_REG8, gate_buf, 1);
	if (err != E_OK)
		return err;
	if (gate_buf[0] != orig)
		return E_IO;		/* restore did not take */

	return E_OK;
}

void app_i2c_gate_test(void)
{
	static const struct {
		UB addr;	/* 7-bit */
		UB reg;		/* register index to read */
	} probes[3] = {
		{ 0x5Au, 0x00u },	/* DRV2605L STATUS  -> expect 0xE0 */
		{ 0x68u, 0x75u },	/* MPU6050 WHO_AM_I -> expect 0x68, AD0 low  */
		{ 0x69u, 0x75u },	/* MPU6050 WHO_AM_I -> expect 0x68, AD0 high */
	};
	/* DRV2605L follow-up registers with DISTINCT non-zero reset values
	 * (SLOS854D Table 3 Register Map Overview):
	 *   0x00 STATUS       -> 0xE0
	 *   0x01 MODE         -> 0x40
	 *   0x03 LIBRARY_SEL  -> 0x01
	 * All three are single-byte reads; no auto-increment is assumed. */
	static const UB drv_regs[3] = { 0x00u, 0x01u, 0x03u };
	INT i;
	ER err = E_IO;
	UW packed = 0;

	stats.gate_wr = 1;		/* 1 = not run (0 would read as E_OK) */

	for (i = 0; i < 3; i++) {
		gate_buf[0] = GATE_SENTINEL;
		err = i2c_rd(probes[i].addr, (UW)probes[i].reg, I2C_REG8,
			     gate_buf, 1);
		if (err == E_OK) {
			stats.gate_addr = probes[i].addr;
			stats.gate_whoami = gate_buf[0];
			break;
		}
	}
	stats.gate_result = (W)err;

	/* DMA-WROTE-THE-BUFFER PROOF (2026-08-29). gate_buf is pre-filled with
	 * GATE_SENTINEL, never 0x00, so an untouched buffer is distinguishable
	 * from a device that genuinely drove zeros. On a bus with pull-ups an
	 * absent talker reads 0xFF, so 0x00 previously had exactly two possible
	 * causes and this separates them:
	 *   whoami == 0x0140E0 -> all three registers correct, device healthy
	 *   whoami == 0xA5A5A5 -> DMA never wrote; HAL reached MemRxCplt off the
	 *                         STOPF path with the byte still in RXDR
	 *   whoami == 0x000000 -> device really drove zeros
	 *   whoami == 0xFFFFFF -> nothing driving during the data phase
	 * // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
	if (err == E_OK && stats.gate_addr == 0x5Au) {
		for (i = 0; i < 3; i++) {
			gate_buf[0] = GATE_SENTINEL;
			if (i2c_rd(0x5Au, (UW)drv_regs[i], I2C_REG8,
				   gate_buf, 1) != E_OK)
				break;
			packed |= ((UW)gate_buf[0]) << (8 * i);
		}
		stats.gate_whoami = packed;
		stats.gate_wr = (W)drv2605l_write_probe();
	}
}

const app_i2c_stats_t *app_i2c_stats(void)
{
	return &stats;
}
