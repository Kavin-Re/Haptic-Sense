/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * HOST-TEST STUB of "stm32n6xx_hal.h". NOT ST's HAL and never compiled into
 * firmware. It models just enough of the GPIO / RCC / DWT surface touched
 * by app_tasks.c for that file to compile and run on a PC: registers are
 * plain variables and pin writes are recorded by the test file.
 */
#ifndef HOST_STUB_STM32N6XX_HAL_H
#define HOST_STUB_STM32N6XX_HAL_H

#include <stdint.h>

typedef struct { uint32_t id; } GPIO_TypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET } GPIO_PinState;
typedef struct {
	uint32_t Pin, Mode, Pull, Speed, Alternate;
} GPIO_InitTypeDef;

extern GPIO_TypeDef host_gpiod, host_gpioh, host_gpioo;
#define GPIOD	(&host_gpiod)
#define GPIOH	(&host_gpioh)
#define GPIOO	(&host_gpioo)

#define GPIO_PIN_1		0x0002u
#define GPIO_PIN_5		0x0020u
#define GPIO_PIN_6		0x0040u
#define GPIO_MODE_OUTPUT_PP	1u
#define GPIO_NOPULL		0u
#define GPIO_SPEED_FREQ_LOW	0u

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
void HAL_GPIO_TogglePin(GPIO_TypeDef *port, uint16_t pin);
uint32_t HAL_RCC_GetCpuClockFreq(void);

#define __HAL_RCC_GPIOO_CLK_ENABLE()	((void)0)
#define __HAL_RCC_GPIOH_CLK_ENABLE()	((void)0)
#define __HAL_RCC_GPIOD_CLK_ENABLE()	((void)0)
#define __NOP()				((void)0)

/* Core debug / DWT cycle counter, as plain variables. */
typedef struct { uint32_t DEMCR; } host_coredebug_t;
typedef struct { uint32_t CTRL, CYCCNT; } host_dwt_t;
extern host_coredebug_t host_coredebug;
extern host_dwt_t host_dwt;
#define CoreDebug			(&host_coredebug)
#define DWT				(&host_dwt)
#define CoreDebug_DEMCR_TRCENA_Msk	(1u << 24)
#define DWT_CTRL_CYCCNTENA_Msk		(1u << 0)
#define DWT_CTRL_NOCYCCNT_Msk		(1u << 25)

#endif /* HOST_STUB_STM32N6XX_HAL_H */
