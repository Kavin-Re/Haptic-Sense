/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0 BSP 2.0
 *
 *    Copyright (C) 2023-2024 by Ken Sakamura.
 *    This software is distributed under the T-License 2.1.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2024/02.
 *
 *----------------------------------------------------------------------
 */

/*
 * Modified for haptic-sense (changed-file notice, T-License):
 *   2026-07-06: comment added to DEVCNF_USE_HAL_IIC explaining why it stays 0;
 *   no configuration value changed. Header comment re-indented.
 *   Original: tron-forum/mtk3_bsp2 v1.00.03.
 */

/*
 *	config_bsp.h
 *	BSP Configuration Definition (STM32Cube)
 */

#ifndef _MTKBSP_BSP_CONFIG_DEVENV_H_
#define _MTKBSP_BSP_CONFIG_DEVENV_H_

/* ------------------------------------------------------------------------ */
/*
 * Static allocation of system memory
 *     Enabling this setting statically allocates system memory space as variables.
 */
#define USE_STATIC_SYS_MEM (0)      // 1:Valid   0:invalid
#define SYSTEM_MEM_SIZE (10 * 1024) // Memory size to statically allocate.

/* ------------------------------------------------------------------------ */
/*
 *  System memory area information (For debugging)
 */
#define USE_DEBUG_SYSMEMINFO (1) // 1:Valid   0:invalid

/* ------------------------------------------------------------------------ */
/* Device usage settings
 *	1: Use   0: Do not use
 */
#define DEVCNF_USE_HAL_IIC 0 // STAYS 0 (Phase 5 Option A): the app owns HAL I2C and the
                             // HAL_I2C_*CpltCallback symbols directly; compiling this BSP
                             // wrapper in would duplicate those callbacks (link error).
                             // See CLAUDE.md §3 "I2C driver decision".
#define DEVCNF_USE_HAL_ADC 0 // A/D conversion device

#endif /* _MTKBSP_BSP_CONFIG_DEVENV_H_ */
