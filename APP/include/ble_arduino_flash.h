/********************************** (C) COPYRIGHT *******************************
 * File Name          : ble_arduino_flash.h
 * Author             : Openterface
 * Version            : V1.0.0
 * Date               : 2026/11/18
 * Description        : BLE-based Arduino flash command handler.
 *                      Receives flash commands via BLE and forwards to Arduino via UART.
 *                      Only available on keymod-plus (BOARD_VERSION=3).
 *********************************************************************************
 * Copyright (c) 2026 Openterface. All rights reserved.
 *******************************************************************************/

#ifndef __BLE_ARDUINO_FLASH_H
#define __BLE_ARDUINO_FLASH_H

#ifdef ENABLE_USB_HOST

#include "debug.h"

/*******************************************************************************/
/* BLE Arduino Flash Commands */

/* Command codes (sent via BLE) */
#define BLE_FLASH_CMD_SYNC          0x01    /* Sync with bootloader */
#define BLE_FLASH_CMD_GET_SIG       0x02    /* Get device signature */
#define BLE_FLASH_CMD_ENTER_PROG    0x03    /* Enter programming mode */
#define BLE_FLASH_CMD_LEAVE_PROG    0x04    /* Leave programming mode */
#define BLE_FLASH_CMD_ERASE         0x05    /* Erase chip */
#define BLE_FLASH_CMD_LOAD_ADDR     0x06    /* Load address */
#define BLE_FLASH_CMD_PROG_PAGE     0x07    /* Program page */
#define BLE_FLASH_CMD_READ_PAGE     0x08    /* Read page */
#define BLE_FLASH_CMD_FLASH_FW      0x09    /* Flash entire firmware */
#define BLE_FLASH_CMD_GET_STATUS    0x0A    /* Get flash status */
#define BLE_FLASH_CMD_SET_PAGE_SIZE 0x0B    /* Set page size */

/* Response status codes */
#define BLE_FLASH_STATUS_OK         0x00
#define BLE_FLASH_STATUS_ERROR      0x01
#define BLE_FLASH_STATUS_BUSY       0x02
#define BLE_FLASH_STATUS_TIMEOUT    0x03

/*******************************************************************************/
/* BLE Flash Frame Format */

/* Request frame: CMD + LEN(2) + DATA */
/* Response frame: STATUS + LEN(2) + DATA */

#define BLE_FLASH_MAX_DATA_LEN      256

/*******************************************************************************/
/* API Functions */

/**
 * @brief Initialize BLE Arduino flash handler
 */
void BLE_Arduino_Flash_Init(void);

/**
 * @brief Process BLE flash command
 * @param data Command data (CMD + LEN + DATA)
 * @param len Data length
 * @param response Response buffer
 * @param response_max Max response size
 * @param response_len Actual response length
 * @return STATUS_SUCCESS or error code
 */
uint8_t BLE_Arduino_Flash_ProcessCommand(const uint8_t *data, uint16_t len,
                                          uint8_t *response, uint16_t response_max,
                                          uint16_t *response_len);

/**
 * @brief Send BLE flash notification
 * @param data Notification data
 * @param len Data length
 * @return STATUS_SUCCESS or error code
 */
uint8_t BLE_Arduino_Flash_SendNotification(const uint8_t *data, uint16_t len);

#endif /* ENABLE_USB_HOST */

#endif /* __BLE_ARDUINO_FLASH_H */
