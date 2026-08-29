/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_drv2605l.c — DRV2605L haptic driver, GPIO ownership (Haptic-Sense)
 *
 * See app_drv2605l.h for the pin-ownership contract.
 *
 * WHY PE7 MOVED (2026-08-30): through Phase 4 the hazard task drove PE7 as a
 * hazard-LEVEL indicator for the logic analyzer, with no DRV2605L attached.
 * A real board is now on that pin, and EN low is device shutdown: SLOS854D
 * §8.4.1.3 says the part still ACKs its address but permits no register access.
 * Leaving the Phase 4 behaviour in place would drop EN on every non-hazard
 * frame and make the Priority-3 register reads fail intermittently, in a way
 * that looks exactly like a wiring fault. PE7 is now EN, raised once at init
 * and never touched again; the hazard pattern moved to PE13 (DRV_TRIG), which
 * is where the design always intended it.
 *
 * This does NOT affect the RZ3 preemption evidence: that campaign measured
 * PH5 (TIMING_D0) to PD6 (TIMING_D1), both untouched (CLAUDE.md §3).
 */

#include "app_drv2605l.h"
#include "stm32n6xx_hal.h"

#define DRV_EN_PORT		GPIOE
#define DRV_EN_PIN		GPIO_PIN_7	/* ARD_D8,  CN12 pin 1 */
#define DRV_TRIG_PORT		GPIOE
#define DRV_TRIG_PIN		GPIO_PIN_13	/* ARD_D6,  CN11 pin 7 */

/* EN rise -> first legal I2C register access. SLOS854D does not give this
 * directly; 1 kernel tick (1-2 ms, CNF_TIMER_PERIOD=1) is far more than the
 * ~250 us the design doc assumes and costs nothing at boot. HAP-T11 shrinks it
 * on the bench and records the real minimum. */
#define DRV_EN_SETTLE_TICKS	1

void drv2605l_gpio_init(void)
{
	GPIO_InitTypeDef gpio_init = {0};

	__HAL_RCC_GPIOE_CLK_ENABLE();	/* already on via CONSOLE_Config (PE5/PE6) */

	/* Drive the ODR low BEFORE switching the pins to output, so neither pin
	 * can glitch high for even one cycle while the breakout's rail state is
	 * unknown (abs max tracks VDD — see the header). */
	HAL_GPIO_WritePin(DRV_EN_PORT, DRV_EN_PIN, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(DRV_TRIG_PORT, DRV_TRIG_PIN, GPIO_PIN_RESET);

	gpio_init.Mode  = GPIO_MODE_OUTPUT_PP;
	gpio_init.Pull  = GPIO_NOPULL;
	gpio_init.Speed = GPIO_SPEED_FREQ_LOW;

	gpio_init.Pin = DRV_EN_PIN;
	HAL_GPIO_Init(DRV_EN_PORT, &gpio_init);

	gpio_init.Pin = DRV_TRIG_PIN;
	HAL_GPIO_Init(DRV_TRIG_PORT, &gpio_init);
}

/* // ONLY CALL FROM PRIORITY 3 SENSOR TASK */
ER drv2605l_power_up(void)
{
	HAL_GPIO_WritePin(DRV_EN_PORT, DRV_EN_PIN, GPIO_PIN_SET);
	return tk_dly_tsk(DRV_EN_SETTLE_TICKS);
}

/* // ONLY CALL FROM PRIORITY 1 HAZARD TASK — GPIO only, no I2C, no printf */
void drv2605l_trig_set(BOOL on)
{
	HAL_GPIO_WritePin(DRV_TRIG_PORT, DRV_TRIG_PIN,
			  on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
