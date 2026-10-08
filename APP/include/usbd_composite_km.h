/********************************** (C) COPYRIGHT *******************************
 * File Name          : usbd_composite_km.h
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2022/08/18
 * Description        : USB keyboard and mouse processing.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/


#ifndef __USBD_COMPOSITE_KM_H
#define __USBD_COMPOSITE_KM_H

/*******************************************************************************/
/* Header Files */
#include "debug.h"
#include "string.h"
#include "usbd_desc.h"
#include "usb_type.h"  // For bool type definition
#include <stdbool.h>
/*******************************************************************************/
/* Global Variable Declaration */

#define STATUS_SUCCESS             0x00  // Operation completed successfully
#define STATUS_ERR_TIMEOUT         0xE1  // Operation timed out (no response within expected window)
#define STATUS_ERR_HEADER          0xE2  // Invalid frame header (expected 0x57 0xAB)
#define STATUS_ERR_CMD             0xE3  // Unsupported or unrecognized command code
#define STATUS_ERR_CHECKSUM        0xE4  // Frame checksum verification failed
#define STATUS_ERR_PARAM           0xE5  // Invalid parameter (out of range, wrong length, or NULL)
#define STATUS_ERR_FRAME           0xE6  // Frame format error, malformed frame, or execution failed
#define STATUS_ERR_FLASH_ERASE     0xE9  // Flash page erase operation failed (hardware fault)
#define STATUS_ERR_FLASH_PROGRAM   0xEA  // Flash word program operation failed (hardware fault)
#define STATUS_ERR_FLASH_VERIFY    0xEB  // Flash write-back verification failed (data mismatch)
/*******************************************************************************/
#define CH9329_MAX_DATA_LEN 100
#define RX_BUF_SIZE   2048
// -------------------- Protocol Definitions --------------------
#define CH9329_FRAME_HEAD1         0x57  // Frame start byte 1 (constant)
#define CH9329_FRAME_HEAD2         0xAB  // Frame start byte 2 (constant)
#define CMD_GET_INFO               0x01  // Get device info: board version, USB state, keyboard LEDs, FW version
#define CMD_SEND_KB_GENERAL_DATA   0x02  // Send standard HID keyboard report (modifier + 6 keycodes)
#define CMD_SEND_KB_MEDIA_DATA     0x03  // Send media/consumer keys (play, volume) or ACPI power keys
#define CMD_SEND_MS_ABS_DATA       0x04  // Send absolute mouse/stylus report (X, Y, buttons, wheel)
#define CMD_SEND_MS_REL_DATA       0x05  // Send relative mouse report (delta X, delta Y, buttons, wheel)
#define CMD_GET_PARA_CFG           0x08  // Read CH9329 parameter configuration (fixed 44-byte reply)
#define CMD_SD_SWITCH              0x17  // Switch/query SD card routing path (Openterface extension)
#define CMD_DS18B20_GET_TEMP       0x18  // Read temperature from DS18B20 onewire sensor (Openterface extension)
#define CMD_BLE_SECURITY           0x22  // BLE security management: pair window, erase bonds, bond count
#define CMD_BLE_ADV_PARAM          0x19  // BLE advertising parameters: channel map, continuous TX test
#define CMD_BLE_SET_NAME           0x23  // Set BLE device name (persisted to Flash) or reset to default
#define CMD_SYSTEM_RESET           0x31  // Trigger system reset/reboot; payload 0x00 resets BLE name
#define CMD_USB_WORK_MODE          0x30  // Query/change USB personality (HID+ACM, HID+UAC+ACM, bridges)
#define CMD_SET_ECM_IP_CONFIG      0x26  // Set ECM bridge IP configuration (persisted to Flash)
#define CMD_GET_ECM_IP_CONFIG      0x27  // Get ECM bridge IP configuration (from RAM cache)
// ACK Status Codes
#define STATUS_SUCCESS             0x00  // Operation completed successfully
#define STATUS_ERR_TIMEOUT         0xE1  // Operation timed out (no response within expected window)
#define STATUS_ERR_HEADER          0xE2  // Invalid frame header (expected 0x57 0xAB)
#define STATUS_ERR_CMD             0xE3  // Unsupported or unrecognized command code
#define STATUS_ERR_CHECKSUM        0xE4  // Frame checksum verification failed
#define STATUS_ERR_PARAM           0xE5  // Invalid parameter (out of range, wrong length, or NULL)
#define STATUS_ERR_FRAME           0xE6  // Frame format error, malformed frame, or execution failed
#define STATUS_ERR_FLASH_ERASE     0xE9  // Flash page erase operation failed (hardware fault)
#define STATUS_ERR_FLASH_PROGRAM   0xEA  // Flash word program operation failed (hardware fault)
#define STATUS_ERR_FLASH_VERIFY    0xEB  // Flash write-back verification failed (data mismatch)
/*******************************************************************************/
void USB_Sleep_Wakeup_CFG( void );
void MCU_Sleep_Wakeup_Operate( void );
void CH9329_SendResponse(uint8_t addr, uint8_t cmd_code, uint8_t* pdata, uint8_t len, uint8_t resp_mode);
void CH9329_Cmd_GetInfo_Reply(uint8_t addr);
int CH9329_DataParser(uint8_t* buf, uint16_t len);
void CH9329_RxBuffer_Add(uint8_t *data, uint16_t len) ;
void USB_DataRx_To_KMHandle(void) ;
void CH9329_SetAutoReleaseMode(bool sender_handles_release) ;
bool CH9329_GetAutoReleaseMode(void) ;
void CH9329_ResetAutoDetection(void) ;
void KB_SetLEDStatus(uint8_t led_status) ;
uint8_t KB_GetLEDStatus(void) ;
void CH9329_HandleRelativeMouse(uint8_t addr, uint8_t cmd_code, uint8_t* data, uint8_t data_len);
void CH9329_HandleAbsoluteMouse(uint8_t addr, uint8_t cmd_code, uint8_t* data, uint8_t data_len) ;

#endif
