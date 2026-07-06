/**
  ******************************************************************************
  * @file    i2c_timing.h
  * @brief   Public prototype for the ST-adapted I2C TIMINGR calculator
  *          (see i2c_timing.c header for provenance and license).
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2022 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

#ifndef I2C_TIMING_H
#define I2C_TIMING_H

#include <stdint.h>

/* Compute I2C TIMINGR for the given kernel-clock and bus frequency (Hz).
 * Returns 0 on failure. Adapted from stm32n6570_discovery_bus.c. */
uint32_t I2C_GetTiming(uint32_t clock_src_freq, uint32_t i2c_freq);

#endif /* I2C_TIMING_H */
