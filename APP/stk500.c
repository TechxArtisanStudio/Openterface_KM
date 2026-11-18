/********************************** (C) COPYRIGHT *******************************
 * File Name          : stk500.c
 * Author             : Openterface
 * Version            : V1.0.0
 * Date               : 2026/11/18
 * Description        : STK500v1 protocol implementation for Arduino flashing.
 *                      Only available on keymod-plus (BOARD_VERSION=3).
 *********************************************************************************
 * Copyright (c) 2026 Openterface. All rights reserved.
 *******************************************************************************/

#ifdef ENABLE_USB_HOST

#include "include/stk500.h"
#include "include/ch32v20x_usbfs_device.h"
#include "UART/UART.h"
#include "usbd_composite_km.h"
#include "string.h"

/*******************************************************************************/
/* Debug Output */
#define STK_DEBUG  1

#if STK_DEBUG
#define STK_PRINTF(...)  printf(__VA_ARGS__)
#else
#define STK_PRINTF(...)
#endif

/*******************************************************************************/
/* Global STK500 Context */
stk500_context_t g_stk500_ctx;

/* UART receive buffer for responses */
static uint8_t g_stk500_rx_buf[512];
static uint16_t g_stk500_rx_len = 0;
static volatile uint8_t g_stk500_response_ready = 0;

/*******************************************************************************/
/* Helper Functions */

/*********************************************************************
 * @fn      STK500_CalcCRC
 *
 * @brief   Calculate XOR checksum for STK500 message
 *
 * @param   data - Data buffer
 * @param   len - Data length
 *
 * @return  Checksum value
 */
uint8_t STK500_CalcCRC(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
    }
    return crc;
}

/*********************************************************************
 * @fn      STK500_WaitForResponse
 *
 * @brief   Wait for STK500 response from UART
 *
 * @param   response - Output buffer
 * @param   max_len - Maximum response length
 * @param   timeout_ms - Timeout in milliseconds
 *
 * @return  Response length, or 0 if timeout
 */
uint16_t STK500_WaitForResponse(uint8_t *response, uint16_t max_len, uint32_t timeout_ms)
{
    uint32_t start_time = 0;
    uint16_t len = 0;

    g_stk500_rx_len = 0;
    g_stk500_response_ready = 0;

    /* Wait for response */
    while (timeout_ms > 0) {
        if (g_stk500_response_ready) {
            len = g_stk500_rx_len;
            if (len > max_len) len = max_len;
            memcpy(response, g_stk500_rx_buf, len);
            return len;
        }
        Delay_Ms(1);
        timeout_ms--;
    }

    return 0;
}

/*********************************************************************
 * @fn      STK500_ReceiveUART
 *
 * @brief   Receive data from UART2 (called from UART2_DataRx_Deal)
 *
 * @return  none
 */
void STK500_ReceiveUART(void)
{
    /* Read available UART data */
    while (Uart.Rx_RemainLen > 0 && g_stk500_rx_len < sizeof(g_stk500_rx_buf)) {
        g_stk500_rx_buf[g_stk500_rx_len++] = UART2_Rx_Buf[Uart.Rx_DealPtr++];
        if (Uart.Rx_DealPtr >= DEF_UARTx_RX_BUF_LEN) {
            Uart.Rx_DealPtr = 0;
        }
        Uart.Rx_RemainLen--;
    }

    /* Check if we have a complete frame (ends with 0x0D) */
    if (g_stk500_rx_len > 2 && g_stk500_rx_buf[g_stk500_rx_len - 1] == STK_CRC_EOP) {
        g_stk500_response_ready = 1;
    }
}

/*******************************************************************************/
/* STK500 API Functions */

/*********************************************************************
 * @fn      STK500_Init
 *
 * @brief   Initialize STK500 context
 *
 * @return  none
 */
void STK500_Init(void)
{
    memset(&g_stk500_ctx, 0, sizeof(g_stk500_ctx));
    g_stk500_ctx.state = STK_STATE_IDLE;
    g_stk500_ctx.seq_num = 1;
    g_stk500_ctx.page_size = 128;  /* Default for ATmega328P */
    g_stk500_ctx.flash_size = 32768; /* 32KB for ATmega328P */

    STK_PRINTF("STK500 initialized\r\n");
}

/*********************************************************************
 * @fn      STK500_BuildRequest
 *
 * @brief   Build STK500 request frame
 *
 * @param   cmd - Command code
 * @param   data - Command data (can be NULL)
 * @param   data_len - Data length
 * @param   frame - Output buffer for frame
 * @param   frame_max - Max frame buffer size
 *
 * @return  Frame length, or -1 if error
 */
int STK500_BuildRequest(uint8_t cmd, const uint8_t *data, uint16_t data_len,
                        uint8_t *frame, uint16_t frame_max)
{
    uint16_t msg_size = 1 + data_len;  /* cmd + data */
    uint16_t frame_len = 1 + 2 + 1 + msg_size + 1;  /* ESC + SIZE(2) + TOKEN + msg + CRC_EOP */

    if (frame_len > frame_max) {
        return -1;
    }

    uint16_t idx = 0;
    frame[idx++] = STK_ESC;                     /* Start token */
    frame[idx++] = g_stk500_ctx.seq_num++;      /* Sequence number */
    frame[idx++] = (msg_size >> 8) & 0xFF;      /* Size high byte */
    frame[idx++] = msg_size & 0xFF;             /* Size low byte */
    frame[idx++] = STK_SEQ;                     /* Constant token */
    frame[idx++] = cmd;                         /* Command */

    if (data && data_len > 0) {
        memcpy(&frame[idx], data, data_len);
        idx += data_len;
    }

    /* Calculate CRC (XOR of message bytes excluding ESC, SEQ, SIZE, TOKEN) */
    uint8_t crc = 0;
    for (uint16_t i = 4; i < idx; i++) {
        crc ^= frame[i];
    }
    frame[idx++] = crc;                         /* CRC */

    frame[idx++] = STK_CRC_EOP;                 /* End of packet */

    return idx;
}

/*********************************************************************
 * @fn      STK500_ParseResponse
 *
 * @brief   Parse STK500 response frame
 *
 * @param   frame - Input frame buffer
 * @param   frame_len - Frame length
 * @param   parsed - Parsed frame structure
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t STK500_ParseResponse(const uint8_t *frame, uint16_t frame_len,
                             stk500_frame_t *parsed)
{
    if (frame_len < 7) {
        return STATUS_ERR_FRAME;
    }

    if (frame[0] != STK_ESC) {
        return STATUS_ERR_HEADER;
    }

    uint8_t seq = frame[1];
    uint16_t size = (frame[2] << 8) | frame[3];
    uint8_t token = frame[4];

    if (token != STK_RESP_SEQ) {
        return STATUS_ERR_CMD;
    }

    uint16_t data_len = size - 1;  /* Exclude status byte */
    if (frame_len < 5 + data_len + 2) {  /* ESC+SEQ+SIZE+TOKEN + data + CRC + EOP */
        return STATUS_ERR_FRAME;
    }

    /* Verify CRC */
    uint8_t crc = frame[frame_len - 2];
    uint8_t calc_crc = 0;
    for (uint16_t i = 4; i < frame_len - 2; i++) {
        calc_crc ^= frame[i];
    }
    if (crc != calc_crc) {
        STK_PRINTF("STK500: CRC mismatch (got 0x%02X, expected 0x%02X)\r\n", crc, calc_crc);
        return STATUS_ERR_CHECKSUM;
    }

    /* Verify end token */
    if (frame[frame_len - 1] != STK_CRC_EOP) {
        return STATUS_ERR_FRAME;
    }

    if (parsed) {
        parsed->seq = seq;
        parsed->size = size;
        parsed->token = token;
        memcpy(parsed->data, &frame[5], data_len);
    }

    /* Check status byte (first byte of data) */
    uint8_t status = frame[5];
    if (status != STK_INSYNC) {
        STK_PRINTF("STK500: Command failed, status=0x%02X\r\n", status);
        return STATUS_ERR_FRAME;
    }

    return STATUS_SUCCESS;
}

/*********************************************************************
 * @fn      STK500_SendCommand
 *
 * @brief   Send STK500 command via UART and wait for response
 *
 * @param   cmd - Command code
 * @param   data - Command data (can be NULL)
 * @param   data_len - Data length
 * @param   response - Response buffer
 * @param   response_max - Max response size
 * @param   response_len - Actual response length
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t STK500_SendCommand(uint8_t cmd, const uint8_t *data, uint16_t data_len,
                           uint8_t *response, uint16_t response_max, uint16_t *response_len)
{
    uint8_t frame[STK_MAX_MSG_SIZE + 10];
    int frame_len;

    frame_len = STK500_BuildRequest(cmd, data, data_len, frame, sizeof(frame));
    if (frame_len < 0) {
        return STATUS_ERR_PARAM;
    }

    STK_PRINTF("STK500: Sending cmd=0x%02X len=%d\r\n", cmd, frame_len);

    /* Send frame via UART2 */
    for (int i = 0; i < frame_len; i++) {
        while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET);
        USART_SendData(USART2, frame[i]);
    }

    /* Wait for transmission complete */
    while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET);

    /* Wait for response */
    uint16_t rx_len = STK500_WaitForResponse(response, response_max, 2000);  /* 2s timeout */

    if (rx_len == 0) {
        STK_PRINTF("STK500: Timeout waiting for response\r\n");
        return STATUS_ERR_TIMEOUT;
    }

    if (response_len) {
        *response_len = rx_len;
    }

    STK_PRINTF("STK500: Received %d bytes\r\n", rx_len);

    /* Parse response */
    stk500_frame_t parsed;
    uint8_t ret = STK500_ParseResponse(response, rx_len, &parsed);

    if (ret != STATUS_SUCCESS) {
        STK_PRINTF("STK500: Parse failed: %d\r\n", ret);
        return ret;
    }

    return STATUS_SUCCESS;
}

/*********************************************************************
 * @fn      STK500_Sync
 *
 * @brief   Synchronize with Arduino bootloader
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t STK500_Sync(void)
{
    uint8_t response[64];
    uint16_t resp_len;

    STK_PRINTF("STK500: Syncing...\r\n");

    /* Send sync command (no data) */
    uint8_t ret = STK500_SendCommand(STK_GET_SYNC, NULL, 0, response, sizeof(response), &resp_len);

    if (ret == STATUS_SUCCESS) {
        g_stk500_ctx.state = STK_STATE_SYNC;
        STK_PRINTF("STK500: Sync OK\r\n");
    }

    return ret;
}

/*********************************************************************
 * @fn      STK500_GetSignature
 *
 * @brief   Get device signature bytes
 *
 * @param   signature - Output buffer for 3 signature bytes
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t STK500_GetSignature(uint8_t *signature)
{
    uint8_t response[64];
    uint16_t resp_len;

    STK_PRINTF("STK500: Getting signature...\r\n");

    uint8_t ret = STK500_SendCommand(STK_READ_SIGN, NULL, 0, response, sizeof(response), &resp_len);

    if (ret == STATUS_SUCCESS) {
        /* Response data starts after status byte */
        if (resp_len >= 4) {  /* ESC+SEQ+SIZE+TOKEN + STATUS + 3 bytes + CRC + EOP */
            /* The parsed data is in response[5..7] */
            memcpy(signature, &response[6], 3);  /* Skip ESC, SEQ, SIZE_H, SIZE_L, TOKEN, STATUS */
            g_stk500_ctx.signature[0] = signature[0];
            g_stk500_ctx.signature[1] = signature[1];
            g_stk500_ctx.signature[2] = signature[2];
            STK_PRINTF("STK500: Signature: 0x%02X 0x%02X 0x%02X\r\n",
                       signature[0], signature[1], signature[2]);
        }
    }

    return ret;
}

/*********************************************************************
 * @fn      STK500_EnterProgMode
 *
 * @brief   Enter programming mode
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t STK500_EnterProgMode(void)
{
    uint8_t response[64];
    uint16_t resp_len;

    STK_PRINTF("STK500: Entering programming mode...\r\n");

    uint8_t ret = STK500_SendCommand(STK_ENTER_PROGMODE, NULL, 0, response, sizeof(response), &resp_len);

    if (ret == STATUS_SUCCESS) {
        g_stk500_ctx.state = STK_STATE_PROGRAMMING;
        STK_PRINTF("STK500: Entered programming mode\r\n");
    }

    return ret;
}

/*********************************************************************
 * @fn      STK500_LeaveProgMode
 *
 * @brief   Leave programming mode
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t STK500_LeaveProgMode(void)
{
    uint8_t response[64];
    uint16_t resp_len;

    STK_PRINTF("STK500: Leaving programming mode...\r\n");

    uint8_t ret = STK500_SendCommand(STK_LEAVE_PROGMODE, NULL, 0, response, sizeof(response), &resp_len);

    if (ret == STATUS_SUCCESS) {
        g_stk500_ctx.state = STK_STATE_DONE;
        STK_PRINTF("STK500: Left programming mode\r\n");
    }

    return ret;
}

/*********************************************************************
 * @fn      STK500_ChipErase
 *
 * @brief   Erase chip
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t STK500_ChipErase(void)
{
    uint8_t response[64];
    uint16_t resp_len;

    STK_PRINTF("STK500: Erasing chip...\r\n");

    uint8_t ret = STK500_SendCommand(STK_CHIP_ERASE, NULL, 0, response, sizeof(response), &resp_len);

    if (ret == STATUS_SUCCESS) {
        STK_PRINTF("STK500: Chip erased\r\n");
    }

    return ret;
}

/*********************************************************************
 * @fn      STK500_LoadAddress
 *
 * @brief   Set address for programming
 *
 * @param   address - Address in words (address * 2 = byte address)
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t STK500_LoadAddress(uint32_t address)
{
    uint8_t response[64];
    uint16_t resp_len;
    uint8_t data[4];

    /* STK500 uses word addresses (16-bit words), so divide by 2 */
    uint32_t word_addr = address / 2;
    data[0] = word_addr & 0xFF;
    data[1] = (word_addr >> 8) & 0xFF;
    data[2] = (word_addr >> 16) & 0xFF;
    data[3] = (word_addr >> 24) & 0xFF;

    STK_PRINTF("STK500: Load address 0x%08X (word: 0x%08X)\r\n", address, word_addr);

    uint8_t ret = STK500_SendCommand(STK_LOAD_ADDRESS, data, 4, response, sizeof(response), &resp_len);

    if (ret == STATUS_SUCCESS) {
        g_stk500_ctx.address = address;
        STK_PRINTF("STK500: Address loaded\r\n");
    }

    return ret;
}

/*********************************************************************
 * @fn      STK500_ProgramPage
 *
 * @brief   Program flash page
 *
 * @param   data - Data buffer
 * @param   len - Data length (must be page_size)
 * @param   is_eeprom - 0=flash, 1=EEPROM
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t STK500_ProgramPage(const uint8_t *data, uint16_t len, uint8_t is_eeprom)
{
    uint8_t response[64];
    uint16_t resp_len;

    if (len > g_stk500_ctx.page_size) {
        STK_PRINTF("STK500: Data length %d exceeds page size %d\r\n", len, g_stk500_ctx.page_size);
        return STATUS_ERR_PARAM;
    }

    uint8_t cmd = is_eeprom ? STK_PROG_DATA : STK_PROG_FLASH;

    STK_PRINTF("STK500: Programming %s page (%d bytes)...\r\n",
              is_eeprom ? "EEPROM" : "flash", len);

    uint8_t ret = STK500_SendCommand(cmd, data, len, response, sizeof(response), &resp_len);

    if (ret == STATUS_SUCCESS) {
        g_stk500_ctx.address += len;
        STK_PRINTF("STK500: Page programmed\r\n");
    }

    return ret;
}

/*********************************************************************
 * @fn      STK500_ReadPage
 *
 * @brief   Read flash page
 *
 * @param   data - Output buffer
 * @param   len - Data length (must be page_size)
 * @param   is_eeprom - 0=flash, 1=EEPROM
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t STK500_ReadPage(uint8_t *data, uint16_t len, uint8_t is_eeprom)
{
    uint8_t response[256];
    uint16_t resp_len;

    uint8_t cmd = is_eeprom ? STK_READ_PAGE : STK_READ_PAGE;  /* Same command for both */

    STK_PRINTF("STK500: Reading %s page (%d bytes)...\r\n",
              is_eeprom ? "EEPROM" : "flash", len);

    uint8_t ret = STK500_SendCommand(cmd, NULL, 0, response, sizeof(response), &resp_len);

    if (ret == STATUS_SUCCESS && resp_len >= 6 + len + 2) {
        /* Extract data from response */
        memcpy(data, &response[6], len);
        STK_PRINTF("STK500: Page read\r\n");
    }

    return ret;
}

/*********************************************************************
 * @fn      STK500_SetDevice
 *
 * @brief   Set device parameters
 *
 * @param   device_params - Device parameter structure
 * @param   len - Length
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t STK500_SetDevice(const uint8_t *device_params, uint16_t len)
{
    uint8_t response[64];
    uint16_t resp_len;

    STK_PRINTF("STK500: Setting device parameters...\r\n");

    uint8_t ret = STK500_SendCommand(STK_SET_DEVICE, device_params, len, response, sizeof(response), &resp_len);

    if (ret == STATUS_SUCCESS) {
        STK_PRINTF("STK500: Device parameters set\r\n");
    }

    return ret;
}

/*********************************************************************
 * @fn      STK500_GetParameter
 *
 * @brief   Get parameter value
 *
 * @param   param - Parameter code
 * @param   value - Output value
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t STK500_GetParameter(uint8_t param, uint8_t *value)
{
    uint8_t response[64];
    uint16_t resp_len;
    uint8_t data[1] = {param};

    STK_PRINTF("STK500: Getting parameter 0x%02X...\r\n", param);

    uint8_t ret = STK500_SendCommand(STK_GET_PARAM, data, 1, response, sizeof(response), &resp_len);

    if (ret == STATUS_SUCCESS && resp_len >= 7) {
        *value = response[7];  /* ESC+SEQ+SIZE_H+SIZE_L+TOKEN+STATUS+VALUE */
        STK_PRINTF("STK500: Parameter 0x%02X = 0x%02X\r\n", param, *value);
    }

    return ret;
}

/*******************************************************************************/
/* High-level Flash Programming Functions */

/**
 * @brief Program entire firmware
 * @param firmware Firmware data
 * @param size Firmware size
 * @return STATUS_SUCCESS or error code
 */
uint8_t STK500_FlashFirmware(const uint8_t *firmware, uint32_t size)
{
    uint8_t ret;

    STK_PRINTF("STK500: Flashing firmware (%d bytes)...\r\n", size);

    /* Sync with bootloader */
    ret = STK500_Sync();
    if (ret != STATUS_SUCCESS) return ret;

    /* Get signature */
    uint8_t signature[3];
    ret = STK500_GetSignature(signature);
    if (ret != STATUS_SUCCESS) return ret;

    /* Enter programming mode */
    ret = STK500_EnterProgMode();
    if (ret != STATUS_SUCCESS) return ret;

    /* Erase chip */
    ret = STK500_ChipErase();
    if (ret != STATUS_SUCCESS) return ret;

    /* Program firmware in pages */
    uint32_t offset = 0;
    while (offset < size) {
        uint16_t chunk = size - offset;
        if (chunk > g_stk500_ctx.page_size) chunk = g_stk500_ctx.page_size;

        /* Load address */
        ret = STK500_LoadAddress(offset);
        if (ret != STATUS_SUCCESS) return ret;

        /* Program page */
        ret = STK500_ProgramPage(&firmware[offset], chunk, 0);
        if (ret != STATUS_SUCCESS) return ret;

        offset += chunk;
    }

    /* Leave programming mode */
    ret = STK500_LeaveProgMode();

    if (ret == STATUS_SUCCESS) {
        STK_PRINTF("STK500: Firmware flashed successfully!\r\n");
    }

    return ret;
}

#endif /* ENABLE_USB_HOST */