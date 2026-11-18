/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_otg_mode.h
 * Author             : Openterface
 * Version            : V1.0.0
 * Date               : 2026/11/18
 * Description        : USB OTG mode switching between Device and Host modes.
 *                      Enables ESP32 firmware flashing through Target USB port.
 *                      Only available on keymod-plus (BOARD_VERSION=3).
 *********************************************************************************
 * Copyright (c) 2026 Openterface. All rights reserved.
 *******************************************************************************/

#ifndef __USB_OTG_MODE_H
#define __USB_OTG_MODE_H

#ifdef ENABLE_USB_HOST

#include "debug.h"
#include "ch32v20x_usb.h"

/*******************************************************************************/
/* USB OTG Mode States */
typedef enum {
    USB_OTG_MODE_DEVICE = 0,      /* Normal HID/CDC device mode */
    USB_OTG_MODE_HOST,            /* USB Host mode for ESP32 flash */
    USB_OTG_MODE_SWITCHING        /* Transition state (do not use USB) */
} usb_otg_mode_t;

/*******************************************************************************/
/* USB Host Transfer Status */
typedef enum {
    USB_HOST_SUCCESS = 0,
    USB_HOST_ERR_TIMEOUT,
    USB_HOST_ERR_STALL,
    USB_HOST_ERR_NAK,
    USB_HOST_ERR_NO_DEVICE,
    USB_HOST_ERR_DISCONNECT,
    USB_HOST_ERR_TOGGLE,
    USB_HOST_ERR_PID,
    USB_HOST_ERR_BUSY
} usb_host_err_t;

/*******************************************************************************/
/* USB Device Descriptor (simplified for enumeration) */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_desc_t;

/*******************************************************************************/
/* USB Configuration Descriptor Header */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed)) usb_config_desc_t;

/*******************************************************************************/
/* USB Interface Descriptor */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed)) usb_interface_desc_t;

/*******************************************************************************/
/* USB Endpoint Descriptor */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed)) usb_endpoint_desc_t;

/*******************************************************************************/
/* USB Host Device Info (populated after enumeration) */
typedef struct {
    uint8_t  address;                    /* Device address (1-127) */
    uint8_t  speed;                      /* 0=full speed, 1=low speed */
    uint8_t  enumerated;                 /* 1 if successfully enumerated */
    usb_device_desc_t device_desc;       /* Device descriptor */
    usb_config_desc_t config_desc;       /* Configuration descriptor header */
    /* CDC-ACM specific (populated by Phase 2) */
    uint8_t  cdc_comm_interface;         /* CDC Communication interface number */
    uint8_t  cdc_data_interface;         /* CDC Data interface number */
    uint8_t  cdc_notification_ep;        /* Notification endpoint address */
    uint8_t  cdc_data_in_ep;             /* Data IN endpoint address */
    uint8_t  cdc_data_out_ep;            /* Data OUT endpoint address */
    uint16_t cdc_data_in_maxpkt;         /* Data IN max packet size */
    uint16_t cdc_data_out_maxpkt;        /* Data OUT max packet size */
} usb_host_device_t;

/*******************************************************************************/
/* Global Variables */
extern volatile usb_otg_mode_t g_usb_otg_mode;
extern usb_host_device_t g_usb_host_device;

/* Host endpoint buffers */
extern __attribute__((aligned(4))) uint8_t USB_Host_Tx_Buf[64];
extern __attribute__((aligned(4))) uint8_t USB_Host_Rx_Buf[64];
extern __attribute__((aligned(4))) uint8_t USB_Host_EP0_Buf[64];

/*******************************************************************************/
/* API Functions */

/**
 * @brief Initialize USB OTG mode (starts in Device mode)
 */
void USB_OTG_ModeInit(void);

/**
 * @brief Get current USB OTG mode
 * @return Current mode
 */
usb_otg_mode_t USB_OTG_GetMode(void);

/**
 * @brief Switch from Device mode to Host mode
 * @return STATUS_SUCCESS or error code
 */
uint8_t USB_OTG_SwitchToHost(void);

/**
 * @brief Switch from Host mode back to Device mode
 * @return STATUS_SUCCESS or error code
 */
uint8_t USB_OTG_SwitchToDevice(void);

/*******************************************************************************/
/* USB Host Mode Functions (internal use) */

/**
 * @brief Initialize USB Host mode registers
 */
void USB_Host_Init(void);

/**
 * @brief Handle device detect interrupt (attach/detach)
 */
void USB_Host_DetectDevice(void);

/**
 * @brief Enumerate connected USB device
 * @return STATUS_SUCCESS or error code
 */
uint8_t USB_Host_EnumerateDevice(void);

/**
 * @brief Perform USB bus reset
 */
void USB_Host_BusReset(void);

/**
 * @brief Send GET_DESCRIPTOR request
 * @param type Descriptor type
 * @param index Descriptor index
 * @param len Requested length
 * @return STATUS_SUCCESS or error code
 */
uint8_t USB_Host_GetDescriptor(uint8_t type, uint8_t index, uint16_t len);

/**
 * @brief Send SET_ADDRESS request
 * @param addr Device address (1-127)
 * @return STATUS_SUCCESS or error code
 */
uint8_t USB_Host_SetAddress(uint8_t addr);

/**
 * @brief Send SET_CONFIGURATION request
 * @param config Configuration value
 * @return STATUS_SUCCESS or error code
 */
uint8_t USB_Host_SetConfiguration(uint8_t config);

/**
 * @brief Perform control transfer on EP0
 * @param bmRequestType Request type
 * @param bRequest Request code
 * @param wValue Value field
 * @param wIndex Index field
 * @param wLength Length field
 * @param data Data buffer (for IN/OUT)
 * @return STATUS_SUCCESS or error code
 */
uint8_t USB_Host_ControlTransfer(uint8_t bmRequestType, uint8_t bRequest,
                                  uint16_t wValue, uint16_t wIndex,
                                  uint16_t wLength, uint8_t *data);

/**
 * @brief Handle transfer complete interrupt
 */
void USB_Host_TransferComplete(void);

/**
 * @brief Handle SOF interrupt
 */
void USB_Host_SOFHandler(void);

#endif /* ENABLE_USB_HOST */

#endif /* __USB_OTG_MODE_H */
