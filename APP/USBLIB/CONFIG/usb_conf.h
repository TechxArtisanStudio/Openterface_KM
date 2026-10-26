/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_conf.h
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2021/08/08
 * Description        : This file contains all the functions prototypes for the  
 *                      USB configration firmware library.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/ 
#ifndef __USB_CONF_H
#define __USB_CONF_H

#include "../../include/usb_work_mode.h"


#define EP_NUM                          (15)

/* Buffer Description Table */
/* buffer table base address */
/* buffer table base address */
#define BTABLE_ADDRESS      (0x00)

/* EP0  */
/* rx/tx buffer base address */
#define ENDP0_RXADDR        (0x40)
#define ENDP0_TXADDR        (0x80)

/* USBD PMA layout — supports both ECM-only and UAC+ECM combined modes:
 *   BTABLE:  0x000~0x03F  (64B)
 *   EP0 RX:  0x040~0x07F  (64B)
 *   EP0 TX:  0x080~0x0BF  (64B)
 *   EP1 TX:  0x0C0~0x0CF  (16B)  CDC interrupt IN
 *   EP5 RX:  0x0D0~0x10F  (64B)  CDC ACM bulk OUT
 *   EP3 TX:  0x110~0x14F  (64B)  CDC ACM bulk IN
 *   EP4 TX0: 0x150~0x18F  (64B)  UAC ISO IN buffer 0 (MCU→Host, mic path)
 *   EP4 TX1: 0x190~0x1CF  (64B)  UAC ISO IN buffer 1
 *   EP2 RX0: 0x1D0~0x20F  (64B)  UAC ISO OUT (Host→MCU, spk path)
 *   EP2 RX1: 0x210~0x24F  (64B)  UAC ISO OUT double-buffer
 *   Total:   0x250 = 592B (fits in 1KB PMA)
 *
 * Combined ACM+ECM mode (USB_WORK_MODE_HID_ACM_ECM) PMA layout:
 *   EP1 TX:  0x0C0~0x0CF  (16B)  ACM INT IN
 *   EP5 RX:  0x0D0~0x10F  (64B)  ACM bulk OUT
 *   EP3 TX:  0x110~0x14F  (64B)  ACM bulk IN
 *   EP2 TX:  0x150~0x15F  (16B)  ECM INT IN (reuses UAC space)
 *   EP4 TX:  0x160~0x19F  (64B)  ECM bulk IN
 *   EP6 RX:  0x1A0~0x1DF  (64B)  ECM bulk OUT
 *   Total:   0x1E0 = 480B
 *
 * Combined ACM+ECM+Target ECM mode (mode 7) PMA layout:
 *   EP1 TX:  0x0C0~0x0CF  (16B)  ACM INT IN
 *   EP5 RX:  0x0D0~0x10F  (64B)  ACM bulk OUT
 *   EP3 TX:  0x110~0x14F  (64B)  ACM bulk IN
 *   EP2 TX:  0x150~0x15F  (16B)  Host ECM INT IN
 *   EP4 TX:  0x160~0x17F  (32B)  Host ECM bulk IN (reduced from 64B)
 *   EP6 RX:  0x180~0x19F  (32B)  Host ECM bulk OUT (reduced from 64B)
 *   EP7 RX:  0x1A0~0x1BF  (32B)  Target ECM bulk OUT (reduced from 64B)
 *   EP7 TX:  0x1C0~0x1DF  (32B)  Target ECM bulk IN (reduced from 64B)
 *   Total:   0x1E0 = 480B (fits in 512B PMA) */
#define ENDP1_TXADDR        (0xC0)                          /* 16B, CDC int   */
#define ENDP5_BULK_RXADDR   (0xD0)                          /* 64B, CDC ACM bulk OUT */
#define ENDP3_TXADDR        (0x110)                         /* 64B, CDC ACM bulk IN  */
#define ENDP4_ISO_TXADDR    (0x150)                         /* 64B, UAC ISO IN buffer 0 */
#define ENDP4_ISO_TXADDR1   (0x190)                         /* 64B, UAC ISO IN buffer 1 */
#define ENDP2_ISO_RXADDR    (0x1D0)                         /* 64B, UAC ISO OUT buffer 0 */
#define ENDP2_ISO_RXADDR1   (0x210)                         /* 64B, UAC ISO OUT buffer 1 */

/* Combined ACM+ECM mode: additional endpoints for ECM (no UAC).
 * These reuse the PMA space normally occupied by UAC endpoints. */
#define ENDP2_TX_ECM_ADDR       (0x150)                     /* 16B, ECM INT IN */
#define ENDP4_TX_ECM_ADDR       (0x160)                     /* 32B, ECM bulk IN (reduced) */
#define ENDP6_RX_ECM_ADDR       (0x180)                     /* 32B, ECM bulk OUT (reduced) */

/* Combined ACM+ECM+Target ECM mode (mode 7): additional endpoints for Target ECM.
 * Target ECM uses EP7 OUT (0x07) and EP7 IN (0x87) — EP7 supports bidirectional.
 * Note: CH32V20x only has EP0-EP7 (8 endpoints), no EP8.
 * Buffer sizes reduced to 32B to fit within 512B PMA. */
#define ENDP7_RX_TARGET_ECM_ADDR (0x1A0)                    /* 32B, Target ECM bulk OUT */
#define ENDP7_TX_TARGET_ECM_ADDR (0x1C0)                    /* 32B, Target ECM bulk IN */

/* Target ECM endpoint busy flag (mode 7) */
#define TARGET_ECM_BULK_IN_BUSY  USBD_Endp7_Busy

/* Serial data endpoint for CDC-ACM. */
#define USBD_SERIAL_ENDP       ENDP3
#define USBD_SERIAL_BUSY       USBD_Endp3_Busy


/* Combined ACM+ECM mode endpoint configuration:
 * For USBLIB stack (no HID):
 *   ACM: EP1 INT, EP5 BULK OUT, EP3 BULK IN
 *   ECM: EP2 INT, EP4 BULK IN,  EP6 BULK OUT
 * For USBFS stack (with HID, reduced HID interfaces):
 *   ACM: EP5 INT, EP6 BULK OUT, EP7 BULK IN
 *   ECM: EP3 BULK OUT, EP4 BULK IN (no interrupt endpoint) */
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
#ifdef USBFS_DEVICE
/* USBFS stack with HID interfaces */
#define ECM_USBD_INT_ENDP        ENDP0  /* No interrupt endpoint in USBFS combined mode */
#define ECM_USBD_BULK_IN_ENDP    ENDP4
#define ECM_USBD_BULK_OUT_ENDP   ENDP3
#define ECM_USBD_INT_BUSY        USBD_Endp0_Busy  /* Dummy - not used */
#define ECM_USBD_BULK_IN_BUSY    USBD_Endp4_Busy
#define ECM_USBD_EP_INT_IN       0  /* Not used */
#define ECM_USBD_EP_BULK_IN      EP4_IN
#else
/* USBLIB stack without HID interfaces */
#define ECM_USBD_INT_ENDP        ENDP2
#define ECM_USBD_BULK_IN_ENDP    ENDP4
#define ECM_USBD_BULK_OUT_ENDP   ENDP6
#define ECM_USBD_INT_BUSY        USBD_Endp2_Busy
#define ECM_USBD_BULK_IN_BUSY    USBD_Endp4_Busy
#define ECM_USBD_EP_INT_IN       EP2_IN
#define ECM_USBD_EP_BULK_IN      EP4_IN
#endif
#else
#define ECM_USBD_INT_ENDP        ENDP1
#define ECM_USBD_BULK_IN_ENDP    ENDP3
#define ECM_USBD_BULK_OUT_ENDP   ENDP5
#define ECM_USBD_INT_BUSY        USBD_Endp1_Busy
#define ECM_USBD_BULK_IN_BUSY    USBD_Endp3_Busy
#define ECM_USBD_EP_INT_IN       EP1_IN
#define ECM_USBD_EP_BULK_IN      EP3_IN
#endif


/* ISTR events */
/* IMR_MSK */
/* mask defining which events has to be handled */
/* by the device application software */
#define IMR_MSK (CNTR_CTRM  | CNTR_WKUPM | CNTR_SUSPM | CNTR_ERRM  | CNTR_SOFM \
                 | CNTR_ESOFM | CNTR_RESETM )

/* #define CTR_CALLBACK */
/* #define DOVR_CALLBACK */
/* #define ERR_CALLBACK */
/* #define WKUP_CALLBACK */
/* #define SUSP_CALLBACK */
/* #define RESET_CALLBAC K*/
/* #define SOF_CALLBACK */
/* #define ESOF_CALLBACK */


/* CTR service routines */
/* associated to defined endpoints */
// #define  EP1_IN_Callback   NOP_Process
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
/* EP2 IN and EP6 OUT handled in usb_endp.c for combined ACM+ECM mode */
#else
#define  EP2_IN_Callback   NOP_Process
#endif
// #define  EP3_IN_Callback   NOP_Process
// #define  EP4_IN_Callback   NOP_Process  /* UAC ISO IN handled in uac_handler */
#define  EP5_IN_Callback   NOP_Process
#define  EP6_IN_Callback    NOP_Process
#if USB_WORK_MODE_HAS_TARGET_CDC_ECM && USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
/* EP7 IN handled in usb_endp.c for combined ACM+ECM+Target ECM mode (mode 7) */
#else
#define  EP7_IN_Callback    NOP_Process
#endif

#define  EP1_OUT_Callback   NOP_Process
// #define  EP2_OUT_Callback   NOP_Process
#define  EP3_OUT_Callback   NOP_Process
// #define  EP4_OUT_Callback   NOP_Process  /* EP4 ISO OUT handled in usb_endp.c */
// #define  EP5_OUT_Callback   NOP_Process  /* CDC bulk OUT relocated to EP5 */
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
/* EP6 OUT handled in usb_endp.c for combined ACM+ECM mode */
#else
#define  EP6_OUT_Callback   NOP_Process
#endif
#if USB_WORK_MODE_HAS_TARGET_CDC_ECM && USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
/* EP7 OUT handled in usb_endp.c for combined ACM+ECM+Target ECM mode (mode 7) */
#else
#define  EP7_OUT_Callback   NOP_Process
#endif

#endif /* __USB_CONF_H */






