/********************************** (C) COPYRIGHT *******************************
 * File Name          : usbd_descriptor.h
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2022/08/18
 * Description        : All descriptors for the keyboard and mouse composite device.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/


#ifndef __USBD_DESC_H
#define __USBD_DESC_H

/*******************************************************************************/
/* Header File */
#include "stdint.h"

#include "Version_selection.h"

/*******************************************************************************/
/* Macro Definition */

/* File Version */
#define DEF_FILE_VERSION              0x04

/* USB Device Info */
#define DEF_USB_VID                   0x1A86
#define DEF_USB_PID                   0xFE00

/* USB Device Descriptor, Device Serial Number(bcdDevice) */
#define DEF_IC_PRG_VER                DEF_FILE_VERSION

/* USB Device Endpoint Size */
#define DEF_USBD_UEP0_SIZE            64     /* usb hs/fs device end-point 0 size */
/* HS */
#define DEF_USBD_HS_PACK_SIZE         512    /* usb hs device max bluk/int pack size */
#define DEF_USBD_HS_ISO_PACK_SIZE     1024   /* usb hs device max iso pack size */
/* FS */
#define DEF_USBD_FS_PACK_SIZE         64     /* usb fs device max bluk/int pack size */
#define DEF_USBD_FS_ISO_PACK_SIZE     1023   /* usb fs device max iso pack size */
/* LS */
#define DEf_USBD_LS_UEP0_SIZE         8      /* usb ls device end-point 0 size */
#define DEF_USBD_LS_PACK_SIZE         64     /* usb ls device max int pack size */

/* USB Device Endpoint1-6 Size */
/* HS */
#define DEF_USB_EP1_HS_SIZE           DEF_USBD_HS_PACK_SIZE
#define DEF_USB_EP2_HS_SIZE           DEF_USBD_HS_PACK_SIZE
#define DEF_USB_EP3_HS_SIZE           DEF_USBD_HS_PACK_SIZE
#define DEF_USB_EP4_HS_SIZE           DEF_USBD_HS_PACK_SIZE
#define DEF_USB_EP5_HS_SIZE           DEF_USBD_HS_PACK_SIZE
#define DEF_USB_EP6_HS_SIZE           DEF_USBD_HS_PACK_SIZE
/* FS */
#define DEF_USB_EP1_FS_SIZE           DEF_USBD_FS_PACK_SIZE
#define DEF_USB_EP2_FS_SIZE           DEF_USBD_FS_PACK_SIZE
#define DEF_USB_EP3_FS_SIZE           DEF_USBD_FS_PACK_SIZE
#define DEF_USB_EP4_FS_SIZE           DEF_USBD_FS_PACK_SIZE
#define DEF_USB_EP5_FS_SIZE           DEF_USBD_FS_PACK_SIZE
#define DEF_USB_EP6_FS_SIZE           DEF_USBD_FS_PACK_SIZE
/* LS */
/* ... */

/* USB Device Descriptor Length */
/* Note: If a descriptor does not exist, set the length to 0. */
#define DEF_USBD_DEVICE_DESC_LEN      ( (uint16_t)MyDevDescr[ 0 ] )
#define DEF_USBD_CONFIG_DESC_LEN   ( (uint16_t)MyCfgDescr[ 2 ] + ( (uint16_t)MyCfgDescr[ 3 ] << 8 ) )
#define DEF_USBD_REPORT_DESC_LEN_KB       0x3E
#define DEF_USBD_REPORT_DESC_LEN_REL_MS   0x34
#define DEF_USBD_REPORT_DESC_LEN_MEDIA    0x8D  /* ACPI + Consumer Control + vendor Feature ReportID=0x30 */

/* EP3 report descriptor length — automatically follows EP3_USE_STYLUS */
#ifdef EP3_USE_STYLUS
  #define DEF_USBD_REPORT_DESC_LEN_ABS_MS   0x36  /* 54 bytes: Digitizer Pen/Stylus  */
#else
  #define DEF_USBD_REPORT_DESC_LEN_ABS_MS   0x3F  /* 63 bytes: Generic Desktop Mouse + Absolute */
#endif

/* Combined HID report descriptor length (Keyboard + RelMouse + AbsMouse + Media + Vendor)
 * Report IDs: 0x01=KB, 0x02=RelMouse, 0x03=AbsMouse, 0x04=ACPI, 0x05=Consumer, 0x30=Vendor Feature
 *
 * IMPORTANT: These values MUST match the actual CombinedHidRepDesc[] array size in usbd_desc.c.
 * A mismatch causes the host to read past the descriptor into garbage memory,
 * resulting in HID parsing errors ("bogus close delimiter", "unknown main item tag").
 * Counted from source: non-stylus = 324 bytes (0x144), stylus = 315 bytes (0x13B). */
#ifdef EP3_USE_STYLUS
  #define DEF_USBD_REPORT_DESC_LEN_COMBINED 0x013B  /* 315 bytes */
#else
  #define DEF_USBD_REPORT_DESC_LEN_COMBINED 0x0144  /* 324 bytes */
#endif
#define DEF_USBD_LANG_DESC_LEN        ( (uint16_t)MyLangDescr[ 0 ] )
#define DEF_USBD_MANU_DESC_LEN        ( (uint16_t)MyManuInfo[ 0 ] )
#define DEF_USBD_PROD_DESC_LEN        ( (uint16_t)MyProdInfo[ 0 ] )
#define DEF_USBD_SN_DESC_LEN          ( (uint16_t)MySerNumInfo[ 0 ] )
#define DEF_USBD_ECM_MAC_DESC_LEN     ( (uint16_t)MyEcmMacInfo[ 0 ] )
#define DEF_USBD_QUALFY_DESC_LEN      ( (uint16_t)MyQuaDesc[ 0 ])
#define DEF_USBD_BOS_DESC_LEN         0
#define DEF_USBD_FS_OTH_DESC_LEN      0
#define DEF_USBD_HS_OTH_DESC_LEN      0


/*******************************************************************************/
/* Descriptor Declaration */
extern const uint8_t MyDevDescr[ ];
extern const uint8_t MyCfgDescr[ ];
extern const uint8_t KeyRepDesc[ ];
extern const uint8_t MouseRepDesc[ ];
extern const uint8_t AbsMouseRepDesc[ ];   /* EP3 descriptor — content depends on EP3_USE_STYLUS */
extern const uint8_t MediaKeyRepDesc[ ];
extern const uint8_t CombinedHidRepDesc[ ]; /* Combined HID report descriptor (KB+Mouse+AbsMouse+Media) */
extern const uint8_t MyQuaDesc[ ];
extern const uint8_t MyLangDescr[ ];
extern const uint8_t MyManuInfo[ ];
extern const uint8_t MyProdInfo[ ];
extern const uint8_t MySerNumInfo[ ];
extern const uint8_t MyEcmMacInfo[ ];
extern const uint8_t MyHostEcmMacInfo[ ];
extern const uint8_t MyTargetEcmMacInfo[ ];
const uint8_t *USBFS_GetActiveConfigDescriptorData(uint16_t *len);
uint8_t USBFS_GetActiveConfigAttributes(void);

#endif
