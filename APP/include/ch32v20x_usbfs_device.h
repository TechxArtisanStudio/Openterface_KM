/********************************** (C) COPYRIGHT *******************************
* File Name          : ch32v20x_usbfs_device.h
* Author             : WCH
* Version            : V1.0.0
* Date               : 2021/06/06
* Description        : This file contains all the functions prototypes for the
*                      USBFS firmware library.
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/
#ifndef __CH32V20X_USBFS_DEVICE_H_
#define __CH32V20X_USBFS_DEVICE_H_

#include "debug.h"
#include "string.h"
#include "usbd_desc.h"
#include "uac_desc.h"
#include "ch32v20x_usb.h"
#ifdef ENABLE_USB_HOST
#include "usb_otg_mode.h"
#endif
#ifndef NULL
#define NULL                    0
#endif

#ifndef VOID
#define VOID                    void
#endif
#ifndef CONST
#define CONST                   const
#endif
#ifndef BOOL
typedef unsigned char           BOOL;
#endif
#ifndef BOOLEAN
typedef unsigned char           BOOLEAN;
#endif
#ifndef CHAR
typedef char                    CHAR;
#endif
#ifndef INT8
typedef char                    INT8;
#endif
#ifndef INT16
typedef short                   INT16;
#endif
#ifndef INT32
typedef long                    INT32;
#endif
#ifndef UINT8
typedef unsigned char           UINT8;
#endif
#ifndef UINT16
typedef unsigned short          UINT16;
#endif
#ifndef UINT32
typedef unsigned long           UINT32;
#endif
#ifndef UINT8V
typedef unsigned char volatile  UINT8V;
#endif
#ifndef UINT16V
typedef unsigned short volatile UINT16V;
#endif
#ifndef UINT32V
typedef unsigned long volatile  UINT32V;
#endif

#ifndef PVOID
typedef void                    *PVOID;
#endif
#ifndef PCHAR
typedef char                    *PCHAR;
#endif
#ifndef PCHAR
typedef const char              *PCCHAR;
#endif
#ifndef PINT8
typedef char                    *PINT8;
#endif
#ifndef PINT16
typedef short                   *PINT16;
#endif
#ifndef PINT32
typedef long                    *PINT32;
#endif
#ifndef PUINT8
typedef unsigned char           *PUINT8;
#endif
#ifndef PUINT16
typedef unsigned short          *PUINT16;
#endif
#ifndef PUINT32
typedef unsigned long           *PUINT32;
#endif
#ifndef PUINT8V
typedef volatile unsigned char  *PUINT8V;
#endif
#ifndef PUINT16V
typedef volatile unsigned short *PUINT16V;
#endif
#ifndef PUINT32V
typedef volatile unsigned long  *PUINT32V;
#endif

/* General */
#define pUSBFS_SetupReqPak          ((PUSB_SETUP_REQ)USBFS_EP0_Buf)

/* end-point number */
#define DEF_UEP_IN                  0x80
#define DEF_UEP_OUT                 0x00
#define DEF_UEP_BUSY                0x01
#define DEF_UEP_FREE                0x00
#define DEF_UEP_NUM                 0x08
#define DEF_UEP0                    0x00
#define DEF_UEP1                    0x01
#define DEF_UEP2                    0x02
#define DEF_UEP3                    0x03
#define DEF_UEP4                    0x04
#define DEF_UEP5                    0x05
#define DEF_UEP6                    0x06
#define DEF_UEP7                    0x07
#define pEP2_IN_DataBuf     (pEP2_RAM_Addr+64)
#define USBFSD_UEP_MOD_BASE         0x5000000C
#define USBFSD_UEP_DMA_BASE         0x50000010
#define USBFSD_UEP_LEN_BASE         0x50000030
#define USBFSD_UEP_CTL_BASE         0x50000032
#define USBFSD_UEP_RX_EN            0x08
#define USBFSD_UEP_TX_EN            0x04
#define USBFSD_UEP_BUF_MOD          0x01
#define DEF_UEP_DMA_LOAD            0 /* Direct the DMA address to the data to be processed */
#define DEF_UEP_CPY_LOAD            1 /* Use memcpy to move data to a buffer */
#define USBFSD_UEP_MOD( N )         (*((volatile uint8_t *)( USBFSD_UEP_MOD_BASE + N )))
#define USBFSD_UEP_TX_CTRL( N )     (*((volatile uint8_t *)( USBFSD_UEP_CTL_BASE + N * 0x04 )))
#define USBFSD_UEP_RX_CTRL( N )     (*((volatile uint8_t *)( USBFSD_UEP_CTL_BASE + N * 0x04 + 1 )))
#define USBFSD_UEP_DMA( N )         (*((volatile uint32_t *)( USBFSD_UEP_DMA_BASE + N * 0x04 )))
#define USBFSD_UEP_BUF( N )         ((uint8_t *)(*((volatile uint32_t *)( USBFSD_UEP_DMA_BASE + N * 0x04 ))) + 0x20000000)
#define USBFSD_UEP_TLEN( N )        (*((volatile uint16_t *)( USBFSD_UEP_LEN_BASE + N * 0x04 )))

/******************************************************************************/
/* Variable Declaration */

/* Global */
extern const    uint8_t  *pUSBFS_Descr;

/* Setup Request */
extern volatile uint8_t  USBFS_SetupReqCode;
extern volatile uint8_t  USBFS_SetupReqType;
extern volatile uint16_t USBFS_SetupReqValue;
extern volatile uint16_t USBFS_SetupReqIndex;
extern volatile uint16_t USBFS_SetupReqLen;

/* USB Device Status */
extern volatile uint8_t  USBFS_DevConfig;
extern volatile uint8_t  USBFS_DevAddr;
extern volatile uint8_t  USBFS_DevSleepStatus;
extern volatile uint8_t  USBFS_DevEnumStatus;
extern volatile uint8_t  USBFS_Connect;
/* HID Class Command */
extern volatile uint8_t  USBFS_HidIdle[ 4 ];
extern volatile uint8_t  USBFS_HidProtocol[ 4 ];

/* Endpoint Buffer */
extern __attribute__ ((aligned(4))) uint8_t USBFS_EP0_Buf[ DEF_USBD_UEP0_SIZE ];     //ep0(64)
extern __attribute__ ((aligned(4))) uint8_t USBFS_EP1_Buf[ DEF_USB_EP1_FS_SIZE ];    //ep1_in(64)
extern __attribute__ ((aligned(4))) uint8_t USBFS_EP2_Buf[ DEF_USB_EP2_FS_SIZE ];    //ep2_in(64)
extern __attribute__ ((aligned(4))) uint8_t USBFS_EP3_Buf[ DEF_USB_EP3_FS_SIZE ];    //ep3_in(64)
extern __attribute__ ((aligned(4))) uint8_t USBFS_EP4_Buf[ DEF_USB_EP4_FS_SIZE ];    //ep4_in(64)
#ifdef ENABLE_UAC
extern __attribute__ ((aligned(4))) uint8_t USBFS_EP5_Buf[ UAC_ISO_PACKET_SIZE ];    //ep5_in  ISO TX single-buf
extern __attribute__ ((aligned(4))) uint8_t USBFS_EP6_Buf[ UAC_ISO_PACKET_SIZE ];    //ep6_out ISO RX single-buf
#endif /* ENABLE_UAC */

/* USB IN Endpoint Busy Flag */
extern volatile uint8_t  USBFS_Endp_Busy[ DEF_UEP_NUM ];

/******************************************************************************/
/* Function Declaration */
extern void USBFS_Device_Init( FunctionalState sta );
extern void USBFS_Device_Endp_Init( void );
extern void USBFS_RCC_Init( void );
extern uint8_t USBFS_Endp_DataUp( uint8_t endp, uint8_t *pbuf, uint16_t len, uint8_t mod );
extern void USBFS_Send_Resume( void );
extern uint8_t USBSendData(void);
extern uint32_t USBFS_CheckEndpointBusyTimeout( void );

#ifdef ENABLE_USB_HOST
/* USB OTG mode functions (only available when ENABLE_USB_HOST is defined) */
extern volatile usb_otg_mode_t g_usb_otg_mode;
#endif

#endif /* __CH32V20x_USBFS_DEVICE_H_ */

