/********************************** (C) COPYRIGHT *******************************
 * File Name          : flash_isp_debug.c
 * Author             : Claude
 * Version            : V1.0
 * Date               : 2026-11-18
 * Description        : Phase 1 ISP bootloader entry investigation
 *                      Implements diagnostic functions to verify if CH32V208
 *                      supports software-triggered ROM bootloader entry.
 *********************************************************************************
 * Copyright (c) 2026 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

#include "include/flash_isp_debug.h"
#include "ch32v20x.h"
#include <stdio.h>

/*********************************************************************
 * LOCAL FUNCTIONS
 */

static void delay_cycles(volatile uint32_t count)
{
    while (count--) {
        __asm volatile("nop");
    }
}

/*********************************************************************
 * @fn      FLASH_DumpRegisters
 *
 * @brief   Dump all FLASH controller registers for debugging.
 *          Helps discover if BOOT_MODEKEYR exists at a different offset.
 *
 * @return  none
 */
void FLASH_DumpRegisters(void)
{
    printf("\r\n=== FLASH Controller Register Dump ===\r\n");
    printf("FLASH Base Address: 0x%08X\r\n", (uint32_t)FLASH);
    printf("\r\n");
    printf("FLASH->ACTLR    = 0x%08X  (offset +0x00)\r\n", FLASH->ACTLR);
    printf("FLASH->KEYR     = 0x%08X  (offset +0x04)\r\n", FLASH->KEYR);
    printf("FLASH->OBKEYR   = 0x%08X  (offset +0x08)\r\n", FLASH->OBKEYR);
    printf("FLASH->STATR    = 0x%08X  (offset +0x0C)\r\n", FLASH->STATR);
    printf("FLASH->CTLR     = 0x%08X  (offset +0x10)\r\n", FLASH->CTLR);
    printf("FLASH->ADDR     = 0x%08X  (offset +0x14)\r\n", FLASH->ADDR);
    printf("FLASH->RESERVED = 0x%08X  (offset +0x18)\r\n", FLASH->RESERVED);
    printf("FLASH->OBR      = 0x%08X  (offset +0x1C)\r\n", FLASH->OBR);
    printf("FLASH->WPR      = 0x%08X  (offset +0x20)\r\n", FLASH->WPR);
    printf("FLASH->MODEKEYR = 0x%08X  (offset +0x24)\r\n", FLASH->MODEKEYR);
    printf("\r\n");

    /* Check registers beyond MODEKEYR */
    volatile uint32_t *reg_ptr = (volatile uint32_t *)((uint32_t)&FLASH->MODEKEYR + 4);
    for (int i = 0; i < 4; i++) {
        printf("FLASH->?+0x%02X   = 0x%08X  (potential BOOT_MODEKEYR?)\r\n",
               0x28 + (i * 4), *reg_ptr);
        reg_ptr++;
    }
    printf("=====================================\r\n\r\n");
}

/*********************************************************************
 * @fn      FLASH_TestModeBit
 *
 * @brief   Test if STATR bit 14 (MODE) is writable.
 *          This bit controls boot from system memory on CH32V003.
 *
 * @return  1 if bit is writable, 0 if read-only or doesn't exist
 */
uint8_t FLASH_TestModeBit(void)
{
    uint32_t statr_before, statr_after;

    printf("\r\n=== Testing FLASH_STATR Bit 14 (MODE) ===\r\n");

    /* Read current value */
    statr_before = FLASH->STATR;
    printf("Before: FLASH->STATR = 0x%08X\r\n", statr_before);

    /* Check if bit 14 is already set */
    if (statr_before & FLASH_STATR_MODE_BIT) {
        printf("MODE bit is already SET\r\n");
        return 1;
    }

    /* Attempt to set bit 14 */
    printf("Attempting to set bit 14 (0x4000)...\r\n");
    FLASH->STATR = FLASH_STATR_MODE_BIT;

    /* Small delay for write to complete */
    delay_cycles(1000);

    /* Read back */
    statr_after = FLASH->STATR;
    printf("After:  FLASH->STATR = 0x%08X\r\n", statr_after);

    /* Check if bit stuck */
    if (statr_after & FLASH_STATR_MODE_BIT) {
        printf("SUCCESS: MODE bit is writable!\r\n");
        printf("Bit 14 can be set - bootloader entry mechanism may exist.\r\n");

        /* Clear the bit we just set */
        FLASH->STATR = FLASH_STATR_MODE_BIT;
        delay_cycles(1000);

        printf("=================================\r\n\r\n");
        return 1;
    } else {
        printf("FAILED: MODE bit is read-only or doesn't exist.\r\n");
        printf("Bit 14 could not be set - mechanism may not exist on CH32V208.\r\n");
        printf("==========================================\r\n\r\n");
        return 0;
    }
}

/*********************************************************************
 * @fn      FLASH_ScanForBootModeRegister
 *
 * @brief   Scan memory after MODEKEYR to find potential BOOT_MODEKEYR.
 *          Looks for writable registers that might be the boot mode key register.
 *
 * @return  none
 */
void FLASH_ScanForBootModeRegister(void)
{
    volatile uint32_t *reg_ptr;
    uint32_t base_addr = (uint32_t)FLASH;

    printf("\r\n=== Scanning for BOOT_MODEKEYR ===\r\n");
    printf("Scanning FLASH registers from offset 0x28 to 0x40...\r\n\r\n");

    /* Scan from offset 0x28 to 0x40 (after MODEKEYR) */
    for (uint32_t offset = 0x28; offset <= 0x40; offset += 4) {
        reg_ptr = (volatile uint32_t *)(base_addr + offset);

        printf("Offset +0x%02X: ", offset);
        printf("Current value = 0x%08X\r\n", *reg_ptr);

        /* Try writing a test pattern */
        uint32_t original = *reg_ptr;
        *reg_ptr = 0xDEADBEEF;
        delay_cycles(100);

        uint32_t after_write = *reg_ptr;
        if (after_write == 0xDEADBEEF) {
            printf("              ^^^ WRITABLE! Could be BOOT_MODEKEYR candidate\r\n");
            /* Restore original value */
            *reg_ptr = original;
        } else if (after_write != original) {
            printf("              ^^^ Partially writable (0x%08X)\r\n", after_write);
            /* Restore original value */
            *reg_ptr = original;
        } else {
            printf("              ^^^ Read-only or locked\r\n");
        }
    }

    printf("\r\nScan complete.\r\n");
    printf("If a writable register was found, it may be BOOT_MODEKEYR.\r\n");
    printf("=====================================\r\n\r\n");
}

/*********************************************************************
 * @fn      FLASH_EnterBootloader
 *
 * @brief   Attempt to enter ROM bootloader via software.
 *          Uses the CH32V003 mechanism: unlock BOOT_MODEKEYR, set MODE bit, reset.
 *
 *          WARNING: This will reset the device if successful!
 *          Device should re-enumerate as USB VID:PID 1A86:55E0.
 *
 * @return  0 if function returns (failed), should not return if successful
 */
uint8_t FLASH_EnterBootloader(void)
{
    printf("\r\n");
    printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\r\n");
    printf("!!!  ENTERING BOOTLOADER MODE      !!!\r\n");
    printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\r\n");
    printf("\r\n");

    printf("Step 1: Unlocking BOOT_MODEKEYR...\r\n");
    printf("  Writing KEY1 = 0x45670123 to MODEKEYR\r\n");
    FLASH->MODEKEYR = FLASH_BOOT_MODE_KEY1;
    delay_cycles(1000);

    printf("  Writing KEY2 = 0xCDEF89AB to MODEKEYR\r\n");
    FLASH->MODEKEYR = FLASH_BOOT_MODE_KEY2;
    delay_cycles(1000);

    printf("  MODEKEYR after unlock: 0x%08X\r\n", FLASH->MODEKEYR);
    printf("\r\n");

    printf("Step 2: Setting MODE bit in STATR...\r\n");
    printf("  STATR before: 0x%08X\r\n", FLASH->STATR);
    printf("  Writing 0x4000 to STATR\r\n");
    FLASH->STATR = FLASH_STATR_MODE_BIT;
    delay_cycles(1000);

    printf("  STATR after:  0x%08X\r\n", FLASH->STATR);

    if (!(FLASH->STATR & FLASH_STATR_MODE_BIT)) {
        printf("\r\n");
        printf("FAILED: MODE bit could not be set!\r\n");
        printf("Bootloader entry mechanism may not exist on CH32V208.\r\n");
        printf("Device will NOT reset.\r\n");
        printf("\r\n");
        return 0;
    }

    printf("\r\n");
    printf("Step 3: MODE bit set successfully!\r\n");
    printf("  Triggering software reset in 1 second...\r\n");
    printf("\r\n");
    printf("  Device should re-enumerate as USB 1A86:55E0\r\n");
    printf("  Use 'wchisp' tool to flash firmware.\r\n");
    printf("\r\n");

    /* Wait for USB transmission to complete */
    for (int i = 0; i < 10; i++) {
        printf("  %d...\r\n", 10 - i);
        Delay_Ms(100);
    }

    printf("\r\n");
    printf("  RESETTING NOW...\r\n");
    printf("\r\n");

    /* Trigger software reset */
    NVIC_SystemReset();

    /* Should never reach here */
    printf("\r\n");
    printf("ERROR: System reset failed!\r\n");
    printf("Device is in unknown state. Power cycle to recover.\r\n");
    printf("\r\n");

    return 0;
}

/*********************************************************************
 * @fn      FLASH_RunPhase1Diagnostics
 *
 * @brief   Run all Phase 1 diagnostic tests sequentially.
 *          Outputs results via printf for serial debugging.
 *
 * @return  none
 */
void FLASH_RunPhase1Diagnostics(void)
{
    printf("\r\n");
    printf("######################################################\r\n");
    printf("##  Phase 1: ISP Bootloader Entry Investigation     ##\r\n");
    printf("##  Target MCU: CH32V208 (RISC-V rv32imacxw)        ##\r\n");
    printf("######################################################\r\n");
    printf("\r\n");

    /* Test 1: Dump registers */
    FLASH_DumpRegisters();

    /* Test 2: Scan for BOOT_MODEKEYR */
    FLASH_ScanForBootModeRegister();

    /* Test 3: Test MODE bit */
    uint8_t mode_bit_writable = FLASH_TestModeBit();

    /* Summary */
    printf("\r\n");
    printf("######################################################\r\n");
    printf("##  Phase 1 Summary                                 ##\r\n");
    printf("######################################################\r\n");
    printf("\r\n");

    if (mode_bit_writable) {
        printf("[PASS] STATR bit 14 (MODE) is writable\r\n");
        printf("\r\n");
        printf("Next steps:\r\n");
        printf("  1. Review register dump for BOOT_MODEKEYR candidates\r\n");
        printf("  2. If ready, call FLASH_EnterBootloader() to test full sequence\r\n");
        printf("  3. Device will reset and should appear as USB 1A86:55E0\r\n");
        printf("  4. Use 'wchisp' to verify ISP mode and flash test firmware\r\n");
    } else {
        printf("[FAIL] STATR bit 14 (MODE) is NOT writable\r\n");
        printf("\r\n");
        printf("Conclusion:\r\n");
        printf("  The CH32V003 bootloader entry mechanism does NOT work on CH32V208.\r\n");
        printf("  Alternative approaches needed:\r\n");
        printf("    - Hardware modification (add BOOT0 control transistor)\r\n");
        printf("    - Custom software bootloader at flash start\r\n");
        printf("    - Physical BOOT button remains as only ISP entry method\r\n");
    }

    printf("\r\n");
    printf("######################################################\r\n");
    printf("\r\n");
}
