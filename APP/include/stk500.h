/********************************** (C) COPYRIGHT *******************************
 * File Name          : stk500.h
 * Author             : Openterface
 * Version            : V1.0.0
 * Date               : 2026/11/18
 * Description        : STK500v1 protocol handler for Arduino flashing.
 *                      Implements the STK500v1 protocol used by Arduino bootloaders.
 *                      Only available on keymod-plus (BOARD_VERSION=3).
 *********************************************************************************
 * Copyright (c) 2026 Openterface. All rights reserved.
 *******************************************************************************/

#ifndef __STK500_H
#define __STK500_H

#ifdef ENABLE_USB_HOST

#include "debug.h"

/*******************************************************************************/
/* STK500v1 Protocol Constants */

/* Frame tokens */
#define STK_OK              0x10    /* Response OK */
#define STK_FAILED          0x11    /* Operation failed */
#define STK_UNKNOWN         0x12    /* Unknown response */
#define STK_NODEVICE        0x13    /* No device found */
#define STK_INSYNC          0x14    /* In sync with programmer */
#define STK_NOSYNC          0x15    /* Not in sync */
#define STK_ADC_ERROR       0x16    /* ADC error */
#define STK_CRC_ERROR       0x17    /* CRC error */

/* Frame delimiters */
#define STK_ESC             0x1B    /* Message start / escape */
#define STK_SEQ             0x0E    /* Constant sequence token in request */
#define STK_RESP_SEQ        0x10    /* Constant sequence token in response */
#define STK_CRC_EOP         0x0D    /* End of packet */

/* Commands */
#define STK_GET_SYNC        0x30    /* Sync with programmer */
#define STK_GET_SIGN        0x31    /* Read signature bytes */
#define STK_SET_PARAM       0x40    /* Set parameter */
#define STK_GET_PARAM       0x41    /* Get parameter */
#define STK_SET_DEVICE      0x42    /* Set device parameters */
#define STK_SET_DEVICE_EXT  0x45    /* Set extended device parameters */
#define STK_ENTER_PROGMODE  0x50    /* Enter programming mode */
#define STK_LEAVE_PROGMODE  0x51    /* Leave programming mode */
#define STK_CHIP_ERASE      0x52    /* Erase chip */
#define STK_CHECK_AUTOINC   0x53    /* Check auto-increment */
#define STK_LOAD_ADDRESS    0x55    /* Set address for programming */
#define STK_UNIVERSAL       0x56    /* Universal programming command */
#define STK_PROG_FLASH      0x60    /* Program flash (old) */
#define STK_PROG_DATA       0x61    /* Program EEPROM (old) */
#define STK_PROG_FUSE       0x62    /* Program fuse bits */
#define STK_PROG_LOCK       0x63    /* Program lock bits */
#define STK_PROG_PAGE       0x64    /* Program page (new) */
#define STK_READ_PAGE       0x74    /* Read page */
#define STK_READ_SIGN       0x75    /* Read signature bytes */
#define STK_READ_OSCCAL     0x76    /* Read oscillator calibration */
#define STK_READ_FUSE       0x77    /* Read fuse bits */
#define STK_READ_LOCK       0x78    /* Read lock bits */
#define STK_READ_OSCCAL_EXT 0x79    /* Read oscillator calibration (ext) */

/* Parameters */
#define STK_PARAM_HW_VER    0x80    /* Hardware version */
#define STK_PARAM_SW_MAJ    0x81    /* Software major version */
#define STK_PARAM_SW_MIN    0x82    /* Software minor version */
#define STK_PARAM_VTARGET   0x83    /* Target voltage */
#define STK_PARAM_VADJUST   0x84    /* Adjustable voltage */
#define STK_PARAM_OSC_PSCALE 0x85   /* Oscillator prescale */
#define STK_PARAM_OSC_CMATCH 0x86   /* Oscillator compare match */
#define STK_PARAM_SCK_DURATION 0x87 /* SCK duration */
#define STK_PARAM_TOPCARD   0x88    /* Topcard detected */
#define STK_PARAM_STATUS    0x89    /* Status */
#define STK_PARAM_DATA      0x8A    /* Data */
#define STK_PARAM_RESET_CAP 0x8B    /* Reset capacitor */
#define STK_PARAM_SYNC_CRC  0x8C    /* Synchronous CRC */

/*******************************************************************************/
/* STK500 Frame Structure */

/* Maximum message size */
#define STK_MAX_MSG_SIZE    256
#define STK_FRAME_OVERHEAD  7       /* ESC + SEQ + SIZE(2) + TOKEN + CRC_EOP + status */

/* STK500 frame */
typedef struct {
    uint8_t  seq;                   /* Sequence number */
    uint16_t size;                  /* Message size */
    uint8_t  token;                 /* Message token (0x0E for request, 0x10 for response) */
    uint8_t  data[STK_MAX_MSG_SIZE]; /* Message data */
} stk500_frame_t;

/*******************************************************************************/
/* STK500 State */

typedef enum {
    STK_STATE_IDLE = 0,
    STK_STATE_SYNC,
    STK_STATE_READY,
    STK_STATE_PROGRAMMING,
    STK_STATE_DONE,
    STK_STATE_ERROR
} stk500_state_t;

typedef struct {
    stk500_state_t state;
    uint8_t  seq_num;               /* Current sequence number */
    uint32_t address;               /* Current address for programming */
    uint32_t flash_size;            /* Flash size in bytes */
    uint16_t page_size;             /* Page size in bytes */
    uint8_t  device_type;           /* Device type */
    uint8_t  signature[3];          /* Device signature */
    uint8_t  error_code;            /* Last error code */
} stk500_context_t;

/*******************************************************************************/
/* Global STK500 Context */
extern stk500_context_t g_stk500_ctx;

/*******************************************************************************/
/* API Functions */

/**
 * @brief Initialize STK500 context
 */
void STK500_Init(void);

/**
 * @brief Build STK500 request frame
 * @param cmd Command code
 * @param data Command data (can be NULL)
 * @param data_len Data length
 * @param frame Output buffer for frame
 * @param frame_max Max frame buffer size
 * @return Frame length, or -1 if error
 */
int STK500_BuildRequest(uint8_t cmd, const uint8_t *data, uint16_t data_len,
                        uint8_t *frame, uint16_t frame_max);

/**
 * @brief Parse STK500 response frame
 * @param frame Input frame buffer
 * @param frame_len Frame length
 * @param parsed Parsed frame structure
 * @return STATUS_SUCCESS or error code
 */
uint8_t STK500_ParseResponse(const uint8_t *frame, uint16_t frame_len,
                             stk500_frame_t *parsed);

/**
 * @brief Send STK500 command via UART and wait for response
 * @param cmd Command code
 * @param data Command data (can be NULL)
 * @param data_len Data length
 * @param response Response buffer
 * @param response_max Max response size
 * @param response_len Actual response length
 * @return STATUS_SUCCESS or error code
 */
uint8_t STK500_SendCommand(uint8_t cmd, const uint8_t *data, uint16_t data_len,
                           uint8_t *response, uint16_t response_max, uint16_t *response_len);

/**
 * @brief Synchronize with Arduino bootloader
 * @return STATUS_SUCCESS or error code
 */
uint8_t STK500_Sync(void);

/**
 * @brief Get device signature bytes
 * @param signature Output buffer for 3 signature bytes
 * @return STATUS_SUCCESS or error code
 */
uint8_t STK500_GetSignature(uint8_t *signature);

/**
 * @brief Enter programming mode
 * @return STATUS_SUCCESS or error code
 */
uint8_t STK500_EnterProgMode(void);

/**
 * @brief Leave programming mode
 * @return STATUS_SUCCESS or error code
 */
uint8_t STK500_LeaveProgMode(void);

/**
 * @brief Erase chip
 * @return STATUS_SUCCESS or error code
 */
uint8_t STK500_ChipErase(void);

/**
 * @brief Set address for programming
 * @param address Address in words (address * 2 = byte address)
 * @return STATUS_SUCCESS or error code
 */
uint8_t STK500_LoadAddress(uint32_t address);

/**
 * @brief Program flash page
 * @param data Data buffer
 * @param len Data length (must be page_size)
 * @param is_eeprom 0=flash, 1=EEPROM
 * @return STATUS_SUCCESS or error code
 */
uint8_t STK500_ProgramPage(const uint8_t *data, uint16_t len, uint8_t is_eeprom);

/**
 * @brief Read flash page
 * @param data Output buffer
 * @param len Data length (must be page_size)
 * @param is_eeprom 0=flash, 1=EEPROM
 * @return STATUS_SUCCESS or error code
 */
uint8_t STK500_ReadPage(uint8_t *data, uint16_t len, uint8_t is_eeprom);

/**
 * @brief Set device parameters
 * @param device_params Device parameter structure
 * @return STATUS_SUCCESS or error code
 */
uint8_t STK500_SetDevice(const uint8_t *device_params, uint16_t len);

/**
 * @brief Get parameter value
 * @param param Parameter code
 * @param value Output value
 * @return STATUS_SUCCESS or error code
 */
uint8_t STK500_GetParameter(uint8_t param, uint8_t *value);

/**
 * @brief Program entire firmware
 * @param firmware Firmware data
 * @param size Firmware size
 * @return STATUS_SUCCESS or error code
 */
uint8_t STK500_FlashFirmware(const uint8_t *firmware, uint32_t size);

#endif /* ENABLE_USB_HOST */

#endif /* __STK500_H */
