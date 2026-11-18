/********************************** (C) COPYRIGHT *******************************
 * File Name          : esp32_flash.h
 * Author             : Openterface
 * Version            : V1.0.0
 * Date               : 2026/11/18
 * Description        : ESP32 flash protocol implementation (esptool protocol).
 *                      Implements SLIP encoding and esptool serial protocol.
 *                      Only available on keymod-plus (BOARD_VERSION=3).
 *********************************************************************************
 * Copyright (c) 2026 Openterface. All rights reserved.
 *******************************************************************************/

#ifndef __ESP32_FLASH_H
#define __ESP32_FLASH_H

#ifdef ENABLE_USB_HOST

#include "usb_host_cdc.h"

/*******************************************************************************/
/* SLIP Protocol Constants */

#define SLIP_END        0xC0    /* Marks end of packet */
#define SLIP_ESC        0xDB    /* Escape character */
#define SLIP_ESC_END    0xDC    /* Escaped END byte (0xC0 -> 0xDB 0xDC) */
#define SLIP_ESC_ESC    0xDD    /* Escaped ESC byte (0xDB -> 0xDB 0xDD) */

/*******************************************************************************/
/* ESP32 Serial Protocol Commands */

/* Command direction */
#define ESP_CMD_REQUEST     0x00
#define ESP_CMD_RESPONSE    0x01

/* Command codes */
#define ESP_CMD_FLASH_BEGIN     0x02    /* Prepare to flash data */
#define ESP_CMD_FLASH_DATA      0x03    /* Send flash data */
#define ESP_CMD_FLASH_END       0x04    /* Finish flashing */
#define ESP_CMD_MEM_BEGIN       0x05    /* Prepare to write to memory */
#define ESP_CMD_MEM_END         0x06    /* Finish memory write */
#define ESP_CMD_MEM_DATA        0x07    /* Send memory data */
#define ESP_CMD_SYNC            0x08    /* Synchronize (initial handshake) */
#define ESP_CMD_WRITE_REG       0x09    /* Write register */
#define ESP_CMD_READ_REG        0x0A    /* Read register */
#define ESP_CMD_SPI_SET_PARAMS  0x0B    /* Set SPI flash parameters */
#define ESP_CMD_SPI_ATTACH      0x0D    /* Attach to SPI flash */
#define ESP_CMD_CHANGE_BAUD     0x0F    /* Change baud rate */
#define ESP_CMD_FLASH_DEFL_BEGIN 0x10   /* Begin flash (deflated) */
#define ESP_CMD_FLASH_DEFL_DATA  0x11   /* Flash data (deflated) */
#define ESP_CMD_FLASH_DEFL_END   0x12   /* End flash (deflated) */
#define ESP_CMD_SPI_FLASH_MD5    0x13   /* Get flash MD5 */
#define ESP_CMD_ERASE_FLASH      0xD0   /* Erase flash */
#define ESP_CMD_ERASE_REGION     0xD1   /* Erase flash region */
#define ESP_CMD_READ_FLASH       0xD2   /* Read flash */
#define ESP_CMD_RUN_USER_CODE    0xD3   /* Run user code */

/* Error codes */
#define ESP_ERR_OK              0x00
#define ESP_ERR_MESSAGE         0x01
#define ESP_ERR_CMD             0x02
#define ESP_ERR_FAILED_SEQUENCE 0x03
#define ESP_ERR_BAD_DATA_LEN    0x04
#define ESP_ERR_BAD_DATA_CRC    0x05
#define ESP_ERR_BAD_DATA_CHECKSUM 0x06
#define ESP_ERR_FLASH_WRITE     0x07
#define ESP_ERR_FLASH_READ      0x08
#define ESP_ERR_INVALID_PARAM   0x09

/*******************************************************************************/
/* ESP32 Protocol Structures */

/* ESP32 command header */
typedef struct {
    uint8_t  direction;     /* 0x00=request, 0x01=response */
    uint8_t  command;       /* Command code */
    uint16_t size;          /* Data size (little endian) */
    uint32_t checksum;      /* Checksum (little endian) */
} __attribute__((packed)) esp_cmd_header_t;

/* SYNC command data */
typedef struct {
    uint8_t  sync_sequence[36];  /* 0x07 0x07 0x12 0x20 + 32x 0x55 */
} __attribute__((packed)) esp_sync_data_t;

/* FLASH_BEGIN/MEM_BEGIN command data */
typedef struct {
    uint32_t size;          /* Total size of data to write */
    uint32_t num_blocks;    /* Number of blocks to write */
    uint32_t block_size;    /* Size of each block */
    uint32_t offset;        /* Offset in flash/memory */
} __attribute__((packed)) esp_flash_begin_data_t;

/* FLASH_DATA/MEM_DATA command data */
typedef struct {
    uint32_t data_len;      /* Length of data in this block */
    uint32_t seq_num;       /* Sequence number (0-based) */
    uint32_t reserved0;     /* Reserved (0) */
    uint32_t reserved1;     /* Reserved (0) */
    /* Followed by data bytes */
} __attribute__((packed)) esp_flash_data_header_t;

/* FLASH_END/MEM_END command data */
typedef struct {
    uint32_t execute;       /* 0=stay in bootloader, 1=run application */
} __attribute__((packed)) esp_flash_end_data_t;

/* SPI_SET_PARAMS command data */
typedef struct {
    uint32_t reserved;      /* Reserved (0) */
    uint32_t id;            /* Flash ID */
    uint32_t total_size;    /* Total flash size in bytes */
    uint32_t block_size;    /* Block size (64KB typical) */
    uint32_t sector_size;   /* Sector size (4KB typical) */
    uint32_t page_size;     /* Page size (256 bytes typical) */
    uint32_t status_mask;   /* Status mask */
} __attribute__((packed)) esp_spi_set_params_data_t;

/* CHANGE_BAUD command data */
typedef struct {
    uint32_t baud_rate;     /* New baud rate */
    uint32_t old_baud_rate; /* Current baud rate */
} __attribute__((packed)) esp_change_baud_data_t;

/*******************************************************************************/
/* ESP32 Flash State */

typedef enum {
    ESP_FLASH_STATE_IDLE = 0,
    ESP_FLASH_STATE_SYNC,
    ESP_FLASH_STATE_READY,
    ESP_FLASH_STATE_WRITING,
    ESP_FLASH_STATE_DONE,
    ESP_FLASH_STATE_ERROR
} esp_flash_state_t;

typedef struct {
    esp_flash_state_t state;
    uint32_t total_size;
    uint32_t written_size;
    uint32_t block_size;
    uint32_t offset;
    uint32_t seq_num;
    uint8_t  error_code;
} esp_flash_context_t;

/*******************************************************************************/
/* Global ESP32 Flash Context */
extern esp_flash_context_t g_esp_flash_ctx;

/*******************************************************************************/
/* SLIP Functions */

/**
 * @brief Encode data using SLIP protocol
 * @param in Input data buffer
 * @param in_len Input data length
 * @param out Output buffer for encoded data
 * @param out_max Maximum output buffer size
 * @return Encoded data length, or -1 if buffer too small
 */
int SLIP_Encode(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t out_max);

/**
 * @brief Decode SLIP encoded data
 * @param in Input encoded data
 * @param in_len Input data length
 * @param out Output buffer for decoded data
 * @param out_max Maximum output buffer size
 * @return Decoded data length, or -1 if error
 */
int SLIP_Decode(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t out_max);

/*******************************************************************************/
/* ESP32 Protocol Functions */

/**
 * @brief Initialize ESP32 flash context
 */
void ESP32_Flash_Init(void);

/**
 * @brief Synchronize with ESP32 (initial handshake)
 * @return STATUS_SUCCESS or error code
 */
uint8_t ESP32_Flash_Sync(void);

/**
 * @brief Begin flash write operation
 * @param size Total size of firmware
 * @param offset Flash offset (usually 0)
 * @return STATUS_SUCCESS or error code
 */
uint8_t ESP32_Flash_Begin(uint32_t size, uint32_t offset);

/**
 * @brief Write flash data block
 * @param data Data buffer
 * @param len Data length (must be block_size from FLASH_BEGIN)
 * @return STATUS_SUCCESS or error code
 */
uint8_t ESP32_Flash_Data(const uint8_t *data, uint16_t len);

/**
 * @brief End flash write operation
 * @param execute 0=stay in bootloader, 1=run application
 * @return STATUS_SUCCESS or error code
 */
uint8_t ESP32_Flash_End(uint8_t execute);

/**
 * @brief Set SPI flash parameters
 * @param total_size Total flash size in bytes
 * @return STATUS_SUCCESS or error code
 */
uint8_t ESP32_Flash_SetSPIParams(uint32_t total_size);

/**
 * @brief Change baud rate
 * @param baud New baud rate
 * @return STATUS_SUCCESS or error code
 */
uint8_t ESP32_Flash_ChangeBaud(uint32_t baud);

/**
 * @brief Get flash MD5 checksum
 * @param offset Flash offset
 * @param size Size to checksum
 * @param md5 Output buffer for 16-byte MD5
 * @return STATUS_SUCCESS or error code
 */
uint8_t ESP32_Flash_GetMD5(uint32_t offset, uint32_t size, uint8_t *md5);

/**
 * @brief Send raw ESP32 command
 * @param cmd Command code
 * @param data Command data
 * @param data_len Data length
 * @param response Response buffer
 * @param response_max Max response size
 * @param response_len Actual response length
 * @return STATUS_SUCCESS or error code
 */
uint8_t ESP32_Flash_SendCommand(uint8_t cmd, const uint8_t *data, uint16_t data_len,
                                 uint8_t *response, uint16_t response_max, uint16_t *response_len);

/**
 * @brief Calculate checksum for flash data
 * @param data Data buffer
 * @param len Data length
 * @return Checksum value
 */
uint32_t ESP32_Flash_CalcChecksum(const uint8_t *data, uint16_t len);

#endif /* ENABLE_USB_HOST */

#endif /* __ESP32_FLASH_H */
