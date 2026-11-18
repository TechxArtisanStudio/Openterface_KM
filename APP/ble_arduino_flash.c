/********************************** (C) COPYRIGHT *******************************
 * File Name          : ble_arduino_flash.c
 * Author             : Openterface
 * Version            : V1.0.0
 * Date               : 2026/11/18
 * Description        : BLE-based Arduino flash command handler implementation.
 *                      Only available on keymod-plus (BOARD_VERSION=3).
 *********************************************************************************
 * Copyright (c) 2026 Openterface. All rights reserved.
 *******************************************************************************/

#ifdef ENABLE_USB_HOST

#include "include/ble_arduino_flash.h"
#include "include/stk500.h"
#include "usbd_composite_km.h"
#include "string.h"

/*******************************************************************************/
/* Debug Output */
#define BLE_FLASH_DEBUG  1

#if BLE_FLASH_DEBUG
#define FLASH_PRINTF(...)  printf(__VA_ARGS__)
#else
#define FLASH_PRINTF(...)
#endif

/*******************************************************************************/
/* Global State */
static uint8_t g_ble_flash_initialized = 0;
static uint16_t g_ble_flash_conn_handle = 0;

/*******************************************************************************/
/* Helper Functions */

/*********************************************************************
 * @fn      BLE_Arduino_Flash_BuildResponse
 *
 * @brief   Build BLE flash response frame
 *
 * @param   status - Response status
 * @param   data - Response data
 * @param   data_len - Data length
 * @param   response - Output buffer
 * @param   response_max - Max response size
 *
 * @return  Response length
 */
static uint16_t BLE_Arduino_Flash_BuildResponse(uint8_t status, const uint8_t *data,
                                                 uint16_t data_len, uint8_t *response,
                                                 uint16_t response_max)
{
    uint16_t total_len = 1 + 2 + data_len;  /* STATUS + LEN(2) + DATA */

    if (total_len > response_max) {
        return 0;
    }

    uint16_t idx = 0;
    response[idx++] = status;
    response[idx++] = (data_len >> 8) & 0xFF;
    response[idx++] = data_len & 0xFF;

    if (data && data_len > 0) {
        memcpy(&response[idx], data, data_len);
        idx += data_len;
    }

    return idx;
}

/*******************************************************************************/
/* BLE Arduino Flash API */

/*********************************************************************
 * @fn      BLE_Arduino_Flash_Init
 *
 * @brief   Initialize BLE Arduino flash handler
 *
 * @return  none
 */
void BLE_Arduino_Flash_Init(void)
{
    if (!g_ble_flash_initialized) {
        STK500_Init();
        g_ble_flash_initialized = 1;
        FLASH_PRINTF("BLE Arduino Flash initialized\r\n");
    }
}

/*********************************************************************
 * @fn      BLE_Arduino_Flash_ProcessCommand
 *
 * @brief   Process BLE flash command
 *
 * @param   data - Command data (CMD + LEN + DATA)
 * @param   len - Data length
 * @param   response - Response buffer
 * @param   response_max - Max response size
 * @param   response_len - Actual response length
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t BLE_Arduino_Flash_ProcessCommand(const uint8_t *data, uint16_t len,
                                          uint8_t *response, uint16_t response_max,
                                          uint16_t *response_len)
{
    uint8_t cmd;
    uint16_t data_len;
    const uint8_t *cmd_data;
    uint8_t ret = STATUS_SUCCESS;
    uint8_t resp_data[256];
    uint16_t resp_data_len = 0;

    if (!g_ble_flash_initialized) {
        BLE_Arduino_Flash_Init();
    }

    if (len < 3) {
        *response_len = BLE_Arduino_Flash_BuildResponse(BLE_FLASH_STATUS_ERROR,
                                                         NULL, 0, response, response_max);
        return STATUS_ERR_PARAM;
    }

    cmd = data[0];
    data_len = (data[1] << 8) | data[2];
    cmd_data = &data[3];

    if (len < 3 + data_len) {
        *response_len = BLE_Arduino_Flash_BuildResponse(BLE_FLASH_STATUS_ERROR,
                                                         NULL, 0, response, response_max);
        return STATUS_ERR_FRAME;
    }

    FLASH_PRINTF("BLE Flash: CMD=0x%02X LEN=%d\r\n", cmd, data_len);

    switch (cmd) {
        case BLE_FLASH_CMD_SYNC:
            ret = STK500_Sync();
            if (ret == STATUS_SUCCESS) {
                resp_data[0] = 0x00;  /* Success */
                resp_data_len = 1;
            }
            break;

        case BLE_FLASH_CMD_GET_SIG:
        {
            uint8_t signature[3];
            ret = STK500_GetSignature(signature);
            if (ret == STATUS_SUCCESS) {
                memcpy(resp_data, signature, 3);
                resp_data_len = 3;
            }
            break;
        }

        case BLE_FLASH_CMD_ENTER_PROG:
            ret = STK500_EnterProgMode();
            if (ret == STATUS_SUCCESS) {
                resp_data[0] = 0x00;
                resp_data_len = 1;
            }
            break;

        case BLE_FLASH_CMD_LEAVE_PROG:
            ret = STK500_LeaveProgMode();
            if (ret == STATUS_SUCCESS) {
                resp_data[0] = 0x00;
                resp_data_len = 1;
            }
            break;

        case BLE_FLASH_CMD_ERASE:
            ret = STK500_ChipErase();
            if (ret == STATUS_SUCCESS) {
                resp_data[0] = 0x00;
                resp_data_len = 1;
            }
            break;

        case BLE_FLASH_CMD_LOAD_ADDR:
            if (data_len >= 4) {
                uint32_t address = cmd_data[0] | (cmd_data[1] << 8) |
                                   (cmd_data[2] << 16) | (cmd_data[3] << 24);
                ret = STK500_LoadAddress(address);
                if (ret == STATUS_SUCCESS) {
                    resp_data[0] = 0x00;
                    resp_data_len = 1;
                }
            } else {
                ret = STATUS_ERR_PARAM;
            }
            break;

        case BLE_FLASH_CMD_PROG_PAGE:
            if (data_len > 0) {
                ret = STK500_ProgramPage(cmd_data, data_len, 0);
                if (ret == STATUS_SUCCESS) {
                    resp_data[0] = 0x00;
                    resp_data_len = 1;
                }
            } else {
                ret = STATUS_ERR_PARAM;
            }
            break;

        case BLE_FLASH_CMD_READ_PAGE:
            if (data_len >= 2) {
                uint16_t page_len = (cmd_data[0] << 8) | cmd_data[1];
                if (page_len <= sizeof(resp_data) - 1) {
                    ret = STK500_ReadPage(&resp_data[1], page_len, 0);
                    if (ret == STATUS_SUCCESS) {
                        resp_data[0] = 0x00;
                        resp_data_len = 1 + page_len;
                    }
                } else {
                    ret = STATUS_ERR_PARAM;
                }
            } else {
                ret = STATUS_ERR_PARAM;
            }
            break;

        case BLE_FLASH_CMD_FLASH_FW:
            if (data_len > 0) {
                ret = STK500_FlashFirmware(cmd_data, data_len);
                if (ret == STATUS_SUCCESS) {
                    resp_data[0] = 0x00;
                    resp_data_len = 1;
                }
            } else {
                ret = STATUS_ERR_PARAM;
            }
            break;

        case BLE_FLASH_CMD_GET_STATUS:
            resp_data[0] = g_stk500_ctx.state;
            resp_data[1] = g_stk500_ctx.error_code;
            resp_data[2] = g_stk500_ctx.signature[0];
            resp_data[3] = g_stk500_ctx.signature[1];
            resp_data[4] = g_stk500_ctx.signature[2];
            resp_data_len = 5;
            ret = STATUS_SUCCESS;
            break;

        case BLE_FLASH_CMD_SET_PAGE_SIZE:
            if (data_len >= 2) {
                g_stk500_ctx.page_size = (cmd_data[0] << 8) | cmd_data[1];
                resp_data[0] = 0x00;
                resp_data_len = 1;
                ret = STATUS_SUCCESS;
            } else {
                ret = STATUS_ERR_PARAM;
            }
            break;

        default:
            ret = STATUS_ERR_CMD;
            break;
    }

    /* Build response */
    uint8_t status = (ret == STATUS_SUCCESS) ? BLE_FLASH_STATUS_OK : BLE_FLASH_STATUS_ERROR;
    *response_len = BLE_Arduino_Flash_BuildResponse(status, resp_data, resp_data_len,
                                                     response, response_max);

    return ret;
}

/*********************************************************************
 * @fn      BLE_Arduino_Flash_SendNotification
 *
 * @brief   Send BLE flash notification
 *
 * @param   data - Notification data
 * @param   len - Data length
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t BLE_Arduino_Flash_SendNotification(const uint8_t *data, uint16_t len)
{
    /* Use existing BLE USB service to send notification */
    /* This would integrate with ble_usb_notify() */
    /* For now, just return success */
    return STATUS_SUCCESS;
}

#endif /* ENABLE_USB_HOST */
