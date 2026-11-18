/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_host_cdc.h
 * Author             : Openterface
 * Version            : V1.0.0
 * Date               : 2026/11/18
 * Description        : USB Host CDC-ACM class driver for ESP32 flashing.
 *                      Only available on keymod-plus (BOARD_VERSION=3).
 *********************************************************************************
 * Copyright (c) 2026 Openterface. All rights reserved.
 *******************************************************************************/

#ifndef __USB_HOST_CDC_H
#define __USB_HOST_CDC_H

#ifdef ENABLE_USB_HOST

#include "usb_otg_mode.h"

/*******************************************************************************/
/* CDC-ACM Class Constants */

/* CDC Class-Specific Descriptor Types */
#define CDC_DT_CS_INTERFACE         0x24
#define CDC_DT_CS_ENDPOINT          0x25

/* CDC Functional Descriptor Subtypes */
#define CDC_SUBTYPE_HEADER          0x00
#define CDC_SUBTYPE_CALL_MGMT       0x01
#define CDC_SUBTYPE_ACM             0x02
#define CDC_SUBTYPE_UNION           0x06

/* CDC ACM Request Codes */
#define CDC_REQ_SEND_ENCAPSULATED   0x00
#define CDC_REQ_GET_ENCAPSULATED    0x01
#define CDC_REQ_SET_COMM_FEATURE    0x02
#define CDC_REQ_GET_COMM_FEATURE    0x03
#define CDC_REQ_CLEAR_COMM_FEATURE  0x04
#define CDC_REQ_SET_LINE_CODING     0x20
#define CDC_REQ_GET_LINE_CODING     0x21
#define CDC_REQ_SET_CONTROL_LINE    0x22
#define CDC_REQ_SEND_BREAK          0x23

/* CDC Control Line State Bits */
#define CDC_CTRL_DTE_PRESENT        0x01  /* DTR */
#define CDC_CTRL_CARRIER_ON         0x02  /* RTS */

/* CDC Line Coding Structure */
typedef struct {
    uint32_t dwDTERate;      /* Data terminal rate in bits per second */
    uint8_t  bCharFormat;    /* Stop bits: 0=1, 1=1.5, 2=2 */
    uint8_t  bParityType;    /* Parity: 0=None, 1=Odd, 2=Even, 3=Mark, 4=Space */
    uint8_t  bDataBits;      /* Data bits: 5, 6, 7, 8, 16 */
} __attribute__((packed)) cdc_line_coding_t;

/* CDC Functional Descriptors */
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;    /* CDC_DT_CS_INTERFACE */
    uint8_t bDescriptorSubtype; /* CDC_SUBTYPE_* */
} __attribute__((packed)) cdc_func_desc_header_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype; /* CDC_SUBTYPE_HEADER */
    uint16_t bcdCDC;
} __attribute__((packed)) cdc_header_func_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype; /* CDC_SUBTYPE_CALL_MGMT */
    uint8_t bmCapabilities;
    uint8_t bDataInterface;
} __attribute__((packed)) cdc_call_mgmt_func_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype; /* CDC_SUBTYPE_ACM */
    uint8_t bmCapabilities;
} __attribute__((packed)) cdc_acm_func_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype; /* CDC_SUBTYPE_UNION */
    uint8_t bControlInterface;
    uint8_t bSubordinateInterface0; /* Variable length */
} __attribute__((packed)) cdc_union_func_desc_t;

/*******************************************************************************/
/* CDC-ACM Endpoint Information */
typedef struct {
    uint8_t  ep_addr;           /* Endpoint address */
    uint8_t  ep_type;           /* Endpoint type (bulk, interrupt) */
    uint16_t max_packet_size;   /* Max packet size */
    uint8_t  interval;          /* Polling interval (for interrupt) */
} cdc_endpoint_info_t;

/*******************************************************************************/
/* CDC-ACM Device State */
typedef struct {
    /* Configuration */
    uint8_t  comm_interface;    /* Communication interface number */
    uint8_t  data_interface;    /* Data interface number */
    uint8_t  alt_setting;       /* Current alternate setting */

    /* Endpoints */
    cdc_endpoint_info_t notification_ep;  /* Interrupt IN */
    cdc_endpoint_info_t data_in_ep;       /* Bulk IN */
    cdc_endpoint_info_t data_out_ep;      /* Bulk OUT */

    /* Line coding */
    cdc_line_coding_t line_coding;

    /* Control line state */
    uint16_t control_line_state;

    /* Callback for received data */
    void (*rx_callback)(const uint8_t *data, uint16_t len);

    /* Connection state */
    uint8_t configured;       /* 1 if fully configured */
    uint8_t data_if_active;   /* 1 if data interface is active (SET_INTERFACE) */
} cdc_acm_device_t;

/*******************************************************************************/
/* Global CDC Device Instance */
extern cdc_acm_device_t g_cdc_acm_device;

/*******************************************************************************/
/* API Functions */

/**
 * @brief Initialize CDC-ACM device state
 */
void USB_Host_CDC_Init(void);

/**
 * @brief Parse CDC descriptors from configuration descriptor
 * @param config_buf Configuration descriptor buffer
 * @param config_len Length of configuration descriptor
 * @return STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_ParseDescriptors(const uint8_t *config_buf, uint16_t config_len);

/**
 * @brief Set line coding (baud rate, data bits, stop bits, parity)
 * @param line_coding Pointer to line coding structure
 * @return STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_SetLineCoding(const cdc_line_coding_t *line_coding);

/**
 * @brief Get current line coding
 * @param line_coding Pointer to store line coding
 * @return STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_GetLineCoding(cdc_line_coding_t *line_coding);

/**
 * @brief Set control line state (DTR/RTS)
 * @param state Control line state (bit 0=DTR, bit 1=RTS)
 * @return STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_SetControlLineState(uint16_t state);

/**
 * @brief Send break signal
 * @param duration Break duration in milliseconds (0=release)
 * @return STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_SendBreak(uint16_t duration);

/**
 * @brief Activate data interface (SET_INTERFACE)
 * @param alt_setting Alternate setting (usually 0 or 1)
 * @return STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_ActivateDataInterface(uint8_t alt_setting);

/**
 * @brief Send data to CDC device (Bulk OUT)
 * @param data Data buffer
 * @param len Data length
 * @return STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_SendData(const uint8_t *data, uint16_t len);

/**
 * @brief Receive data from CDC device (Bulk IN)
 * @param data Buffer to receive data
 * @param max_len Maximum length to receive
 * @param actual_len Pointer to store actual length received
 * @return STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_ReceiveData(uint8_t *data, uint16_t max_len, uint16_t *actual_len);

/**
 * @brief Register RX callback for received data
 * @param callback Function to call when data is received
 */
void USB_Host_CDC_SetRxCallback(void (*callback)(const uint8_t *data, uint16_t len));

/**
 * @brief Configure host endpoints for CDC data transfer
 * Must be called after descriptors are parsed
 */
void USB_Host_CDC_ConfigureEndpoints(void);

/**
 * @brief Bulk transfer complete handler (internal use)
 * Called from USB_Host_TransferComplete when bulk transfer completes
 * @param status Transfer status
 * @param rx_len Received data length (for IN transfers)
 */
void USB_Host_CDC_BulkTransferComplete(uint8_t status, uint16_t rx_len);

/*******************************************************************************/
/* Default Line Coding (115200 8N1) */
#define CDC_LINE_CODING_DEFAULT { \
    .dwDTERate = 115200, \
    .bCharFormat = 0, \
    .bParityType = 0, \
    .bDataBits = 8 \
}

#endif /* ENABLE_USB_HOST */

#endif /* __USB_HOST_CDC_H */