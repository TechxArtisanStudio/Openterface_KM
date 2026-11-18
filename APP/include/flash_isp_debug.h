/********************************** (C) COPYRIGHT *******************************
 * File Name          : flash_isp_debug.h
 * Author             : Claude
 * Version            : V1.0
 * Date               : 2026-11-18
 * Description        : Phase 1 ISP bootloader entry investigation
 *                      Functions to verify CH32V208 ROM bootloader entry mechanism
 *********************************************************************************
 * Copyright (c) 2026 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

#ifndef FLASH_ISP_DEBUG_H
#define FLASH_ISP_DEBUG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************************
 * CONSTANTS
 */

/* FLASH_STATR bit 14 - Boot Mode bit (unverified on CH32V208) */
#define FLASH_STATR_MODE_BIT      ((uint32_t)0x4000)

/* BOOT_MODEKEYR unlock keys (from CH32V003 documentation) */
#define FLASH_BOOT_MODE_KEY1      ((uint32_t)0x45670123)
#define FLASH_BOOT_MODE_KEY2      ((uint32_t)0xCDEF89AB)

/*********************************************************************
 * FUNCTIONS
 */

/**
 * @brief Dump all FLASH controller registers for debugging.
 *        Helps discover if BOOT_MODEKEYR exists at a different offset.
 */
void FLASH_DumpRegisters(void);

/**
 * @brief Test if STATR bit 14 (MODE) is writable.
 *        This bit is documented for CH32V003 but not CH32V208.
 * @return 1 if bit is writable, 0 if read-only or doesn't exist
 */
uint8_t FLASH_TestModeBit(void);

/**
 * @brief Check if there's a register after MODEKEYR.
 *        Scans memory after MODEKEYR offset to find potential BOOT_MODEKEYR.
 */
void FLASH_ScanForBootModeRegister(void);

/**
 * @brief Attempt to enter ROM bootloader via software.
 *        WARNING: This will reset the device if successful!
 *        Device should re-enumerate as USB VID:PID 1A86:55E0.
 *
 * @return 0 if function returns (failed), should not return if successful
 */
uint8_t FLASH_EnterBootloader(void);

/**
 * @brief Run all Phase 1 diagnostic tests sequentially.
 *        Outputs results via printf.
 */
void FLASH_RunPhase1Diagnostics(void);

#ifdef __cplusplus
}
#endif

#endif /* FLASH_ISP_DEBUG_H */
