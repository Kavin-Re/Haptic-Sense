/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * hal_timebase_mtk.c -- STM32 HAL time base served by the uT-Kernel 3.0
 * system timer.
 *
 * Written for Haptic-Sense on 2026-09-25 to replace mtkernel_bsp.c, which was
 * removed because its copyright header did not match its origin (see
 * docs/PROVENANCE.md). Behaviour of the three functions below is unchanged;
 * mtkernel_bsp.c's TIM4_Config()/TIM4_Get_Value() had no callers and were
 * already discarded by --gc-sections, so they are not carried over.
 *
 * WHY THIS FILE IS LOAD-BEARING
 * The HAL's own time base (stm32n6xx_hal.c, all three functions __weak)
 * programs SysTick in HAL_InitTick() and counts uwTick from SysTick_Handler.
 * On this build SysTick belongs to the kernel: knl_start_hw_timer()
 * (mtk3_bsp2/sysdepend/stm32_cube/cpu/core/armv8m/sys_timer.h) reprograms
 * it as the 1 ms system timer (CNF_TIMER_PERIOD = 1, config.h:29). Without
 * these strong definitions HAL_InitTick() would fight the kernel for SysTick
 * and every HAL timeout (I2C, DMA, RCC, XSPI) would be measured against a
 * counter the kernel does not advance.
 *
 * Context: HAL_GetTick() is safe from any context. HAL_Delay() blocks the
 * calling task via tk_dly_tsk() and must never run in handler mode.
 */
#include <assert.h>
#include <stdint.h>
#include "stm32n6xx_hal.h"
#include "tk/tkernel.h"

/* SysTick is owned by the kernel -- nothing to configure here. */
HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
	(void)TickPriority;
	return HAL_OK;
}

/*
 * Milliseconds since kernel start: low word of the kernel system time.
 * Wraps after 2^32 ms (~49.7 days), the same as the HAL's own uwTick, and
 * HAL timeout code already computes (now - start) with unsigned wrap.
 */
uint32_t HAL_GetTick(void)
{
	SYSTIM now = { 0, 0 };

	(void)tk_get_tim(&now);
	return (uint32_t)now.lo;
}

/* Sleep the calling task; other tasks keep running during the delay. */
void HAL_Delay(uint32_t Delay)
{
	assert(__get_IPSR() == 0U);	/* task context only */
	(void)tk_dly_tsk((RELTIM)Delay);
}
