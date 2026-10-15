/********************************** (C) COPYRIGHT *******************************
 * File Name          : composite_km_desc.h
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2022/08/18
 * Description        : All descriptors for the keyboard and mouse composite device.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/


/*******************************************************************************/
/* Header File */
#include "usbd_desc.h"
#include "include/Version_selection.h"
#include "include/uac_desc.h"
#include "include/usb_mode_config.h"
#include <string.h>

/*******************************************************************************/
/* Device Descriptor */
const uint8_t MyDevDescr[ ] =
{
    0x12,                                                   // bLength
    0x01,                                                   // bDescriptorType
    0x00, 0x02,                                             // bcdUSB
    0x00,                                                   // bDeviceClass
    0x00,                                                   // bDeviceSubClass
    0x00,                                                   // bDeviceProtocol
    DEF_USBD_UEP0_SIZE,                                     // bMaxPacketSize0
    (uint8_t)DEF_USB_VID, (uint8_t)( DEF_USB_VID >> 8 ),    // idVendor
    (uint8_t)DEF_USB_PID, (uint8_t)( DEF_USB_PID >> 8 ),    // idProduct
    0x00, DEF_IC_PRG_VER,                                   // bcdDevice
    0x01,                                                   // iManufacturer
    0x02,                                                   // iProduct
    0x03,                                                   // iSerialNumber
    0x01,                                                   // bNumConfigurations
};

/* Configuration Descriptor Set */
const uint8_t MyCfgDescr[ ] =
{
    /* Configuration Descriptor */
    0x09,                                                   // bLength
    0x02,                                                   // bDescriptorType
#if USB_WORK_MODE_HAS_TARGET_UAC
    0x12, 0x01,                                             // wTotalLength: 274 = 0x0112 (100 HID + 9 cfg + 165 UAC)
    0x07,                                                   // bNumInterfaces: 4 HID + 1 AC + 2 AS = 7
#elif USB_WORK_MODE_HAS_TARGET_CDC_ACM
    0xAF, 0x00,                                             // wTotalLength: 175 = 0x00AF (100 HID + 9 cfg + 66 CDC ACM)
    0x06,                                                   // bNumInterfaces: 4 HID + 2 CDC ACM = 6
#elif USB_WORK_MODE_HAS_TARGET_CDC_ECM
    0xBC, 0x00,                                             // wTotalLength: 188 = 0x00BC (100 HID + 9 cfg + 79 CDC ECM)
    0x06,                                                   // bNumInterfaces: 4 HID + 2 CDC ECM = 6
#else
    0x6D, 0x00,                                             // wTotalLength: 109 = 0x006D (9 cfg + 4*25 HID)
    0x04,                                                   // bNumInterfaces: 4 HID only
#endif
    0x01,                                                   // bConfigurationValue
    0x00,                                                   // iConfiguration
    0xA0,                                                   // bmAttributes: Bus Powered; Remote Wakeup
    0x32,                                                   // MaxPower: 100mA

    /* Interface Descriptor (Keyboard) */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType
    0x00,                                                   // bInterfaceNumber
    0x00,                                                   // bAlternateSetting
    0x01,                                                   // bNumEndpoints
    0x03,                                                   // bInterfaceClass
    0x01,                                                   // bInterfaceSubClass
    0x01,                                                   // bInterfaceProtocol: Keyboard
    0x00,                                                   // iInterface

    /* HID Descriptor (Keyboard) */
    0x09,                                                   // bLength
    0x21,                                                   // bDescriptorType
    0x11, 0x01,                                             // bcdHID
    0x00,                                                   // bCountryCode
    0x01,                                                   // bNumDescriptors
    0x22,                                                   // bDescriptorType
    0x3E, 0x00,                                             // wDescriptorLength

    /* Endpoint Descriptor (Keyboard) */
    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType
    0x81,                                                   // bEndpointAddress: IN Endpoint 1
    0x03,                                                   // bmAttributes
    0x08, 0x00,                                             // wMaxPacketSize
    0x0A,                                                   // bInterval: 10mS

    /* Interface Descriptor (Mouse) */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType
    0x01,                                                   // bInterfaceNumber
    0x00,                                                   // bAlternateSetting
    0x01,                                                   // bNumEndpoints
    0x03,                                                   // bInterfaceClass
    0x01,                                                   // bInterfaceSubClass
    0x00,                                                   // bInterfaceProtocol: None (report protocol)
    0x00,                                                   // iInterface

    /* HID Descriptor (Mouse) */
    0x09,                                                   // bLength
    0x21,                                                   // bDescriptorType
    0x10, 0x01,                                             // bcdHID
    0x00,                                                   // bCountryCode
    0x01,                                                   // bNumDescriptors
    0x22,                                                   // bDescriptorType
    0x34, 0x00,                                             // wDescriptorLength

    /* Endpoint Descriptor (Mouse) */
    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType
    0x82,                                                   // bEndpointAddress: IN Endpoint 2
    0x03,                                                   // bmAttributes
    0x08, 0x00,                                             // wMaxPacketSize
    0x01,                                                   // bInterval: 1mS

    /* Interface Descriptor (Absolute Mouse / Touch Digitizer) */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType
    0x02,                                                   // bInterfaceNumber
    0x00,                                                   // bAlternateSetting
    0x01,                                                   // bNumEndpoints
    0x03,                                                   // bInterfaceClass (HID)
    0x00,                                                   // bInterfaceSubClass
    0x00,                                                   // bInterfaceProtocol: None
    0x00,                                                   // iInterface

    /* HID Descriptor (Absolute Mouse / Touch Digitizer) */
    0x09,                                                   // bLength
    0x21,                                                   // bDescriptorType
    0x10, 0x01,                                             // bcdHID
    0x00,                                                   // bCountryCode
    0x01,                                                   // bNumDescriptors
    0x22,                                                   // bDescriptorType
    DEF_USBD_REPORT_DESC_LEN_ABS_MS, 0x00,                 // wDescriptorLength (auto: 0x3F abs mouse / 0x36 stylus)

    /* Endpoint Descriptor (Absolute Mouse) */
    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType
    0x83,                                                   // bEndpointAddress: IN Endpoint 3
    0x03,                                                   // bmAttributes
    0x08, 0x00,                                             // wMaxPacketSize
    0x01,                                                   // bInterval: 1mS (125µs at HS, 1ms at FS)

    /* Interface Descriptor (Media/Consumer Control) */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType
    0x03,                                                   // bInterfaceNumber
    0x00,                                                   // bAlternateSetting
    0x01,                                                   // bNumEndpoints
    0x03,                                                   // bInterfaceClass (HID)
    0x00,                                                   // bInterfaceSubClass
    0x00,                                                   // bInterfaceProtocol
    0x00,                                                   // iInterface

    /* HID Descriptor (Media/Consumer Control) */
    0x09,                                                   // bLength
    0x21,                                                   // bDescriptorType
    0x11, 0x01,                                             // bcdHID
    0x00,                                                   // bCountryCode
    0x01,                                                   // bNumDescriptors
    0x22,                                                   // bDescriptorType
    DEF_USBD_REPORT_DESC_LEN_MEDIA, 0x00,                  // wDescriptorLength

    /* Endpoint Descriptor (Media/Consumer Control) */
    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType
    0x84,                                                   // bEndpointAddress: IN Endpoint 4
    0x03,                                                   // bmAttributes (Interrupt)
    0x08, 0x00,                                             // wMaxPacketSize
    0x0A,                                                   // bInterval: 10mS

#if USB_WORK_MODE_HAS_TARGET_CDC_ACM
    /* IAD: CDC ACM (IF4=comm + IF5=data) */
    0x08,                                                   // bLength
    0x0B,                                                   // bDescriptorType (IAD)
    0x04,                                                   // bFirstInterface
    0x02,                                                   // bInterfaceCount
    0x02,                                                   // bFunctionClass: Communications
    0x02,                                                   // bFunctionSubClass: Abstract Control Model
    0x01,                                                   // bFunctionProtocol: AT Commands
    0x00,                                                   // iFunction

    /* Interface Descriptor: CDC ACM Communication (IF4) */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType
    0x04,                                                   // bInterfaceNumber
    0x00,                                                   // bAlternateSetting
    0x01,                                                   // bNumEndpoints
    0x02,                                                   // bInterfaceClass: Communications
    0x02,                                                   // bInterfaceSubClass: Abstract Control Model
    0x01,                                                   // bInterfaceProtocol: AT Commands
    0x00,                                                   // iInterface

    /* CDC Functional Descriptors */
    0x05, 0x24, 0x00, 0x10, 0x01,                           // Header, CDC 1.10
    0x05, 0x24, 0x01, 0x00, 0x05,                           // Call Management, data IF5
    0x04, 0x24, 0x02, 0x02,                                 // ACM capabilities
    0x05, 0x24, 0x06, 0x04, 0x05,                           // Union: control IF4, data IF5

    /* Endpoint Descriptor: CDC notification EP5 IN */
    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType
    0x85,                                                   // bEndpointAddress: EP5 IN
    0x03,                                                   // bmAttributes: Interrupt
    0x08, 0x00,                                             // wMaxPacketSize
    0x10,                                                   // bInterval: 16mS

    /* Interface Descriptor: CDC ACM Data (IF5) */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType
    0x05,                                                   // bInterfaceNumber
    0x00,                                                   // bAlternateSetting
    0x02,                                                   // bNumEndpoints
    0x0A,                                                   // bInterfaceClass: Data
    0x00,                                                   // bInterfaceSubClass
    0x00,                                                   // bInterfaceProtocol
    0x00,                                                   // iInterface

    /* Endpoint Descriptor: CDC data OUT EP6 */
    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType
    0x06,                                                   // bEndpointAddress: EP6 OUT
    0x02,                                                   // bmAttributes: Bulk
    0x40, 0x00,                                             // wMaxPacketSize
    0x00,                                                   // bInterval

    /* Endpoint Descriptor: CDC data IN EP7 */
    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType
    0x87,                                                   // bEndpointAddress: EP7 IN
    0x02,                                                   // bmAttributes: Bulk
    0x40, 0x00,                                             // wMaxPacketSize
    0x00,                                                   // bInterval

#endif /* USB_WORK_MODE_HAS_TARGET_CDC_ACM */

#if USB_WORK_MODE_HAS_TARGET_CDC_ECM
    /* IAD: CDC ECM (IF4=comm + IF5=data) */
    0x08,                                                   // bLength
    0x0B,                                                   // bDescriptorType (IAD)
    0x04,                                                   // bFirstInterface
    0x02,                                                   // bInterfaceCount
    0x02,                                                   // bFunctionClass: Communications
    0x06,                                                   // bFunctionSubClass: Ethernet Networking Control Model
    0x00,                                                   // bFunctionProtocol
    0x00,                                                   // iFunction

    /* Interface Descriptor: CDC ECM Communication (IF4) */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType
    0x04,                                                   // bInterfaceNumber
    0x00,                                                   // bAlternateSetting
    0x01,                                                   // bNumEndpoints
    0x02,                                                   // bInterfaceClass: Communications
    0x06,                                                   // bInterfaceSubClass: Ethernet Networking Control Model
    0x00,                                                   // bInterfaceProtocol
    0x00,                                                   // iInterface

    /* CDC ECM Functional Descriptors */
    0x05, 0x24, 0x00, 0x10, 0x01,                           // Header, CDC 1.10
    0x05, 0x24, 0x06, 0x04, 0x05,                           // Union: control IF4, data IF5
    0x0D, 0x24, 0x0F, 0x04,                                 // Ethernet Networking, iMACAddress=4
    0x00, 0x00, 0x00, 0x00,                                 // bmEthernetStatistics
    0xEA, 0x05,                                             // wMaxSegmentSize: 1514
    0x00, 0x00,                                             // wNumberMCFilters: none
    0x00,                                                   // bNumberPowerFilters

    /* Endpoint Descriptor: CDC ECM notification EP5 IN */
    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType
    0x85,                                                   // bEndpointAddress: EP5 IN
    0x03,                                                   // bmAttributes: Interrupt
    0x10, 0x00,                                             // wMaxPacketSize
    0x10,                                                   // bInterval: 16mS

    /* Interface Descriptor: CDC ECM Data (IF5 alt 0, idle) */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType
    0x05,                                                   // bInterfaceNumber
    0x00,                                                   // bAlternateSetting
    0x00,                                                   // bNumEndpoints
    0x0A,                                                   // bInterfaceClass: Data
    0x00,                                                   // bInterfaceSubClass
    0x00,                                                   // bInterfaceProtocol
    0x00,                                                   // iInterface

    /* Interface Descriptor: CDC ECM Data (IF5 alt 1, active) */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType
    0x05,                                                   // bInterfaceNumber
    0x01,                                                   // bAlternateSetting
    0x02,                                                   // bNumEndpoints
    0x0A,                                                   // bInterfaceClass: Data
    0x00,                                                   // bInterfaceSubClass
    0x00,                                                   // bInterfaceProtocol
    0x00,                                                   // iInterface

    /* Endpoint Descriptor: CDC ECM data OUT EP6 */
    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType
    0x06,                                                   // bEndpointAddress: EP6 OUT
    0x02,                                                   // bmAttributes: Bulk
    0x40, 0x00,                                             // wMaxPacketSize
    0x00,                                                   // bInterval

    /* Endpoint Descriptor: CDC ECM data IN EP7 */
    0x07,                                                   // bLength
    0x05,                                                   // bDescriptorType
    0x87,                                                   // bEndpointAddress: EP7 IN
    0x02,                                                   // bmAttributes: Bulk
    0x40, 0x00,                                             // wMaxPacketSize
    0x00,                                                   // bInterval,

#endif /* USB_WORK_MODE_HAS_TARGET_CDC_ECM */

#if USB_WORK_MODE_HAS_TARGET_UAC
    /* ====== UAC 1.0 — USBFS (Target PC) side ============================
     * IF4: AudioControl
     * IF5: AudioStreaming IN  (EP5 ISO IN  0x85 — Target PC gets mic audio)
     *   alt0: zero-bandwidth
    *   alt1: active 16 kHz stereo 16-bit 64 B/ms
     * IF6: AudioStreaming OUT (EP6 ISO OUT 0x06 — Target PC sends playback)
     *   alt0: zero-bandwidth
    *   alt1: active 16 kHz stereo 16-bit 64 B/ms
     *
    * NOTE: All descriptor fields use UAC_USBFS_* constants (16 kHz / 64 B)
     * which is what the Target PC negotiates with the USBFS controller.
     * =================================================================== */

    /* --- Interface Descriptor: AudioControl (IF4) --- */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType (Interface)
    0x04,                                                   // bInterfaceNumber 4
    0x00,                                                   // bAlternateSetting 0
    0x00,                                                   // bNumEndpoints 0
    0x01,                                                   // bInterfaceClass (Audio)
    0x01,                                                   // bInterfaceSubClass (AudioControl)
    0x00,                                                   // bInterfaceProtocol
    0x00,                                                   // iInterface

    /* CS Interface: AC Header */
    0x0A,                                                   // bLength (8 + 2 baInterfaceNr)
    USB_DESC_TYPE_CS_INTERFACE,                             // bDescriptorType
    UAC_AC_HEADER,                                          // bDescriptorSubtype
    0x00, 0x01,                                             // bcdADC 1.00
    UAC_SR_B0(UAC_AC_INTERFACE_TOTAL_LEN),                 // wTotalLength: 52 = 0x34 byte0
    UAC_SR_B1(UAC_AC_INTERFACE_TOTAL_LEN),                 // wTotalLength: 52 = 0x34 byte1
    0x02,                                                   // bInCollection: 2 streaming interfaces
    0x05,                                                   // baInterfaceNr(1): IF5 (AS IN)
    0x06,                                                   // baInterfaceNr(2): IF6 (AS OUT)

    /* CS Interface: Input Terminal 1 — USB streaming (Target PC sends playback audio) */
    0x0C,                                                   // bLength
    USB_DESC_TYPE_CS_INTERFACE,                             // bDescriptorType
    UAC_AC_INPUT_TERMINAL,                                  // bDescriptorSubtype
    0x01,                                                   // bTerminalID 1
    UAC_SR_B0(UAC_TERMINAL_USB_STREAMING), UAC_SR_B1(UAC_TERMINAL_USB_STREAMING),
    0x00,                                                   // bAssocTerminal
    UAC_CHANNELS,                                           // bNrChannels
    0x03, 0x00,                                             // wChannelConfig: L+R
    0x00,                                                   // iChannelNames
    0x00,                                                   // iTerminal

    /* CS Interface: Output Terminal 2 — Speaker (playback → MCU bridge) */
    0x09,                                                   // bLength
    USB_DESC_TYPE_CS_INTERFACE,                             // bDescriptorType
    UAC_AC_OUTPUT_TERMINAL,                                 // bDescriptorSubtype
    0x02,                                                   // bTerminalID 2
    UAC_SR_B0(UAC_TERMINAL_SPEAKER), UAC_SR_B1(UAC_TERMINAL_SPEAKER),
    0x00,                                                   // bAssocTerminal
    0x01,                                                   // bSourceID (from IT1)
    0x00,                                                   // iTerminal

    /* CS Interface: Input Terminal 3 — Microphone (capture from MCU bridge) */
    0x0C,                                                   // bLength
    USB_DESC_TYPE_CS_INTERFACE,                             // bDescriptorType
    UAC_AC_INPUT_TERMINAL,                                  // bDescriptorSubtype
    0x03,                                                   // bTerminalID 3
    UAC_SR_B0(UAC_TERMINAL_MICROPHONE), UAC_SR_B1(UAC_TERMINAL_MICROPHONE),
    0x00,                                                   // bAssocTerminal
    UAC_CHANNELS,                                           // bNrChannels
    0x03, 0x00,                                             // wChannelConfig: L+R
    0x00,                                                   // iChannelNames
    0x00,                                                   // iTerminal

    /* CS Interface: Output Terminal 4 — USB streaming (mic → Target PC) */
    0x09,                                                   // bLength
    USB_DESC_TYPE_CS_INTERFACE,                             // bDescriptorType
    UAC_AC_OUTPUT_TERMINAL,                                 // bDescriptorSubtype
    0x04,                                                   // bTerminalID 4
    UAC_SR_B0(UAC_TERMINAL_USB_STREAMING), UAC_SR_B1(UAC_TERMINAL_USB_STREAMING),
    0x00,                                                   // bAssocTerminal
    0x03,                                                   // bSourceID (from IT3)
    0x00,                                                   // iTerminal

    /* --- IF5 AudioStreaming IN — alt0 zero-bandwidth --- */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType
    0x05,                                                   // bInterfaceNumber 5
    0x00,                                                   // bAlternateSetting 0 (zero-bw)
    0x00,                                                   // bNumEndpoints 0
    0x01,                                                   // bInterfaceClass (Audio)
    0x02,                                                   // bInterfaceSubClass (AudioStreaming)
    0x00,                                                   // bInterfaceProtocol
    0x00,                                                   // iInterface

    /* --- IF5 AudioStreaming IN — alt1 active (EP5 ISO IN 0x85) --- */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType
    0x05,                                                   // bInterfaceNumber 5
    0x01,                                                   // bAlternateSetting 1 (active)
    0x01,                                                   // bNumEndpoints 1
    0x01,                                                   // bInterfaceClass (Audio)
    0x02,                                                   // bInterfaceSubClass (AudioStreaming)
    0x00,                                                   // bInterfaceProtocol
    0x00,                                                   // iInterface

    /* CS Interface: AS General — linked to OT4 (Target mic → USB) */
    0x07,                                                   // bLength
    USB_DESC_TYPE_CS_INTERFACE,                             // bDescriptorType
    UAC_AS_GENERAL,                                         // bDescriptorSubtype
    0x04,                                                   // bTerminalLink (OT4)
    0x00,                                                   // bDelay
    UAC_SR_B0(UAC_FORMAT_PCM), UAC_SR_B1(UAC_FORMAT_PCM),  // wFormatTag: PCM

    /* CS Interface: Format Type I — 16 kHz stereo 16-bit */
    0x0B,                                                   // bLength
    USB_DESC_TYPE_CS_INTERFACE,                             // bDescriptorType
    UAC_AS_FORMAT_TYPE,                                     // bDescriptorSubtype
    0x01,                                                   // bFormatType: Type I
    UAC_CHANNELS,                                           // bNrChannels: 2
    UAC_BYTES_PER_SAMPLE,                                   // bSubFrameSize: 2
    UAC_BIT_RESOLUTION,                                     // bBitResolution: 16
    0x01,                                                   // bSamFreqType: 1 frequency
    UAC_SR_B0(UAC_SAMPLE_RATE),                             // tSamFreq byte0
    UAC_SR_B1(UAC_SAMPLE_RATE),                             // tSamFreq byte1
    UAC_SR_B2(UAC_SAMPLE_RATE),                             // tSamFreq byte2

    /* Endpoint Descriptor: EP5 ISO IN (0x85) */
    0x09,                                                   // bLength
    0x05,                                                   // bDescriptorType (Endpoint)
    UAC_USBFS_EP_IN_ADDR,                                   // bEndpointAddress: 0x85 (EP5 IN)
    0x01,                                                   // bmAttributes: Isochronous
    UAC_SR_B0(UAC_ISO_PACKET_SIZE), UAC_SR_B1(UAC_ISO_PACKET_SIZE), // wMaxPacketSize
    0x01,                                                   // bInterval: 1 ms
    0x00,                                                   // bRefresh
    0x00,                                                   // bSynchAddress

    /* CS Endpoint: EP5 */
    0x07,                                                   // bLength
    USB_DESC_TYPE_CS_ENDPOINT,                              // bDescriptorType
    UAC_EP_GENERAL,                                         // bDescriptorSubtype
    0x01,                                                   // bmAttributes: sampling frequency control
    0x00,                                                   // bLockDelayUnits
    0x00, 0x00,                                             // wLockDelay

    /* --- IF6 AudioStreaming OUT — alt0 zero-bandwidth --- */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType
    0x06,                                                   // bInterfaceNumber 6
    0x00,                                                   // bAlternateSetting 0 (zero-bw)
    0x00,                                                   // bNumEndpoints 0
    0x01,                                                   // bInterfaceClass (Audio)
    0x02,                                                   // bInterfaceSubClass (AudioStreaming)
    0x00,                                                   // bInterfaceProtocol
    0x00,                                                   // iInterface

    /* --- IF6 AudioStreaming OUT — alt1 active (EP6 ISO OUT 0x06) --- */
    0x09,                                                   // bLength
    0x04,                                                   // bDescriptorType
    0x06,                                                   // bInterfaceNumber 6
    0x01,                                                   // bAlternateSetting 1 (active)
    0x01,                                                   // bNumEndpoints 1
    0x01,                                                   // bInterfaceClass (Audio)
    0x02,                                                   // bInterfaceSubClass (AudioStreaming)
    0x00,                                                   // bInterfaceProtocol
    0x00,                                                   // iInterface

    /* CS Interface: AS General — linked to IT1 (USB playback → MCU bridge) */
    0x07,                                                   // bLength
    USB_DESC_TYPE_CS_INTERFACE,                             // bDescriptorType
    UAC_AS_GENERAL,                                         // bDescriptorSubtype
    0x01,                                                   // bTerminalLink (IT1)
    0x00,                                                   // bDelay
    UAC_SR_B0(UAC_FORMAT_PCM), UAC_SR_B1(UAC_FORMAT_PCM),  // wFormatTag: PCM

    /* CS Interface: Format Type I — 16 kHz stereo 16-bit */
    0x0B,                                                   // bLength
    USB_DESC_TYPE_CS_INTERFACE,                             // bDescriptorType
    UAC_AS_FORMAT_TYPE,                                     // bDescriptorSubtype
    0x01,                                                   // bFormatType: Type I
    UAC_CHANNELS,                                           // bNrChannels: 2
    UAC_BYTES_PER_SAMPLE,                                   // bSubFrameSize: 2
    UAC_BIT_RESOLUTION,                                     // bBitResolution: 16
    0x01,                                                   // bSamFreqType: 1 frequency
    UAC_SR_B0(UAC_SAMPLE_RATE),                             // tSamFreq byte0
    UAC_SR_B1(UAC_SAMPLE_RATE),                             // tSamFreq byte1
    UAC_SR_B2(UAC_SAMPLE_RATE),                             // tSamFreq byte2

    /* Endpoint Descriptor: EP6 ISO OUT (0x06) */
    0x09,                                                   // bLength
    0x05,                                                   // bDescriptorType (Endpoint)
    UAC_USBFS_EP_OUT_ADDR,                                  // bEndpointAddress: 0x06 (EP6 OUT)
    0x09,                                                   // bmAttributes: Isochronous, Adaptive
    UAC_SR_B0(UAC_ISO_PACKET_SIZE), UAC_SR_B1(UAC_ISO_PACKET_SIZE), // wMaxPacketSize
    0x01,                                                   // bInterval: 1 ms
    0x00,                                                   // bRefresh
    0x00,                                                   // bSynchAddress

    /* CS Endpoint: EP6 */
    0x07,                                                   // bLength
    USB_DESC_TYPE_CS_ENDPOINT,                              // bDescriptorType
    UAC_EP_GENERAL,                                         // bDescriptorSubtype
    0x01,                                                   // bmAttributes: sampling frequency control
    0x00,                                                   // bLockDelayUnits
    0x00, 0x00                                              // wLockDelay
#endif /* USB_WORK_MODE_HAS_TARGET_UAC */
};

#if USB_WORK_MODE_HAS_TARGET_UAC
typedef char MyCfgDescr_SizeCheck[(sizeof(MyCfgDescr) == 274) ? 1 : -1];
#elif USB_WORK_MODE_HAS_TARGET_CDC_ACM
typedef char MyCfgDescr_SizeCheck[(sizeof(MyCfgDescr) == 175) ? 1 : -1];
#elif USB_WORK_MODE_HAS_TARGET_CDC_ECM
typedef char MyCfgDescr_SizeCheck[(sizeof(MyCfgDescr) == 188) ? 1 : -1];
#else
typedef char MyCfgDescr_SizeCheck[(sizeof(MyCfgDescr) == 109) ? 1 : -1];
#endif

#if USB_WORK_MODE_RUNTIME_PROFILES
#define USBFS_RUNTIME_HID_CONFIG_DESC_SIZE 109u
#define USBFS_RUNTIME_ACM_BLOCK_SIZE       66u
#define USBFS_RUNTIME_ECM_BLOCK_SIZE       79u
#define USBFS_RUNTIME_UAC_BLOCK_SIZE       UAC_USBFS_BLOCK_LEN
#define USBFS_RUNTIME_MAX_CONFIG_DESC_SIZE 274u

static const uint8_t USBFS_RuntimeHidConfigBase[USBFS_RUNTIME_HID_CONFIG_DESC_SIZE] = {
    0x09, 0x02, 0x6D, 0x00, 0x04, 0x01, 0x00, 0xA0, 0x32,

    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3E, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0A,

    0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x01, 0x00, 0x00,
    0x09, 0x21, 0x10, 0x01, 0x00, 0x01, 0x22, 0x34, 0x00,
    0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x01,

    0x09, 0x04, 0x02, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x10, 0x01, 0x00, 0x01, 0x22, DEF_USBD_REPORT_DESC_LEN_ABS_MS, 0x00,
    0x07, 0x05, 0x83, 0x03, 0x08, 0x00, 0x01,

    0x09, 0x04, 0x03, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, DEF_USBD_REPORT_DESC_LEN_MEDIA, 0x00,
    0x07, 0x05, 0x84, 0x03, 0x08, 0x00, 0x0A,
};

static const uint8_t USBFS_RuntimeAcmBlock[USBFS_RUNTIME_ACM_BLOCK_SIZE] = {
    0x08, 0x0B, 0x04, 0x02, 0x02, 0x02, 0x01, 0x00,
    0x09, 0x04, 0x04, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,
    0x05, 0x24, 0x00, 0x10, 0x01,
    0x05, 0x24, 0x01, 0x00, 0x05,
    0x04, 0x24, 0x02, 0x02,
    0x05, 0x24, 0x06, 0x04, 0x05,
    0x07, 0x05, 0x85, 0x03, 0x08, 0x00, 0x10,
    0x09, 0x04, 0x05, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x06, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x87, 0x02, 0x40, 0x00, 0x00,
};

static const uint8_t USBFS_RuntimeEcmBlock[USBFS_RUNTIME_ECM_BLOCK_SIZE] = {
    0x08, 0x0B, 0x04, 0x02, 0x02, 0x06, 0x00, 0x00,
    0x09, 0x04, 0x04, 0x00, 0x01, 0x02, 0x06, 0x00, 0x00,
    0x05, 0x24, 0x00, 0x10, 0x01,
    0x05, 0x24, 0x06, 0x04, 0x05,
    0x0D, 0x24, 0x0F, 0x04, 0x00, 0x00, 0x00, 0x00, 0xEA, 0x05, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x85, 0x03, 0x10, 0x00, 0x10,
    0x09, 0x04, 0x05, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x05, 0x01, 0x02, 0x0A, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x06, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x87, 0x02, 0x40, 0x00, 0x00,
};

static const uint8_t USBFS_RuntimeUacBlock[USBFS_RUNTIME_UAC_BLOCK_SIZE] = {
    0x09, 0x04, 0x04, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x0A, USB_DESC_TYPE_CS_INTERFACE, UAC_AC_HEADER, 0x00, 0x01,
    UAC_SR_B0(UAC_AC_INTERFACE_TOTAL_LEN), UAC_SR_B1(UAC_AC_INTERFACE_TOTAL_LEN),
    0x02, 0x05, 0x06,
    0x0C, USB_DESC_TYPE_CS_INTERFACE, UAC_AC_INPUT_TERMINAL, 0x01,
    UAC_SR_B0(UAC_TERMINAL_USB_STREAMING), UAC_SR_B1(UAC_TERMINAL_USB_STREAMING),
    0x00, UAC_CHANNELS, 0x03, 0x00, 0x00, 0x00,
    0x09, USB_DESC_TYPE_CS_INTERFACE, UAC_AC_OUTPUT_TERMINAL, 0x02,
    UAC_SR_B0(UAC_TERMINAL_SPEAKER), UAC_SR_B1(UAC_TERMINAL_SPEAKER),
    0x00, 0x01, 0x00,
    0x0C, USB_DESC_TYPE_CS_INTERFACE, UAC_AC_INPUT_TERMINAL, 0x03,
    UAC_SR_B0(UAC_TERMINAL_MICROPHONE), UAC_SR_B1(UAC_TERMINAL_MICROPHONE),
    0x00, UAC_CHANNELS, 0x03, 0x00, 0x00, 0x00,
    0x09, USB_DESC_TYPE_CS_INTERFACE, UAC_AC_OUTPUT_TERMINAL, 0x04,
    UAC_SR_B0(UAC_TERMINAL_USB_STREAMING), UAC_SR_B1(UAC_TERMINAL_USB_STREAMING),
    0x00, 0x03, 0x00,
    0x09, 0x04, 0x05, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    0x09, 0x04, 0x05, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, USB_DESC_TYPE_CS_INTERFACE, UAC_AS_GENERAL, 0x04, 0x00,
    UAC_SR_B0(UAC_FORMAT_PCM), UAC_SR_B1(UAC_FORMAT_PCM),
    0x0B, USB_DESC_TYPE_CS_INTERFACE, UAC_AS_FORMAT_TYPE, 0x01,
    UAC_CHANNELS, UAC_BYTES_PER_SAMPLE, UAC_BIT_RESOLUTION, 0x01,
    UAC_SR_B0(UAC_SAMPLE_RATE), UAC_SR_B1(UAC_SAMPLE_RATE), UAC_SR_B2(UAC_SAMPLE_RATE),
    0x09, 0x05, UAC_USBFS_EP_IN_ADDR, 0x01,
    UAC_SR_B0(UAC_ISO_PACKET_SIZE), UAC_SR_B1(UAC_ISO_PACKET_SIZE), 0x01, 0x00, 0x00,
    0x07, USB_DESC_TYPE_CS_ENDPOINT, UAC_EP_GENERAL, 0x01, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x06, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    0x09, 0x04, 0x06, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, USB_DESC_TYPE_CS_INTERFACE, UAC_AS_GENERAL, 0x01, 0x00,
    UAC_SR_B0(UAC_FORMAT_PCM), UAC_SR_B1(UAC_FORMAT_PCM),
    0x0B, USB_DESC_TYPE_CS_INTERFACE, UAC_AS_FORMAT_TYPE, 0x01,
    UAC_CHANNELS, UAC_BYTES_PER_SAMPLE, UAC_BIT_RESOLUTION, 0x01,
    UAC_SR_B0(UAC_SAMPLE_RATE), UAC_SR_B1(UAC_SAMPLE_RATE), UAC_SR_B2(UAC_SAMPLE_RATE),
    0x09, 0x05, UAC_USBFS_EP_OUT_ADDR, 0x09,
    UAC_SR_B0(UAC_ISO_PACKET_SIZE), UAC_SR_B1(UAC_ISO_PACKET_SIZE), 0x01, 0x00, 0x00,
    0x07, USB_DESC_TYPE_CS_ENDPOINT, UAC_EP_GENERAL, 0x01, 0x00, 0x00, 0x00,
};

typedef char USBFS_RuntimeUacBlock_SizeCheck[(sizeof(USBFS_RuntimeUacBlock) == USBFS_RUNTIME_UAC_BLOCK_SIZE) ? 1 : -1];

static uint8_t USBFS_RuntimeConfigDescriptor[USBFS_RUNTIME_MAX_CONFIG_DESC_SIZE];
static uint8_t USBFS_RuntimeConfigMode = 0xFFu;
static uint16_t USBFS_RuntimeConfigLen;

static void USBFS_BuildRuntimeConfigDescriptor(void)
{
    uint8_t mode = USB_ModeConfig_GetActiveMode();
    uint16_t total_len = USBFS_RUNTIME_HID_CONFIG_DESC_SIZE;
    uint8_t num_interfaces = 4u;

    if (USBFS_RuntimeConfigMode == mode && USBFS_RuntimeConfigLen != 0u) {
        return;
    }

    memcpy(USBFS_RuntimeConfigDescriptor, USBFS_RuntimeHidConfigBase, USBFS_RUNTIME_HID_CONFIG_DESC_SIZE);
    if (USB_ModeConfig_ModeHasHostUac(mode)) {
        memcpy(&USBFS_RuntimeConfigDescriptor[total_len], USBFS_RuntimeUacBlock, USBFS_RUNTIME_UAC_BLOCK_SIZE);
        total_len = (uint16_t)(total_len + USBFS_RUNTIME_UAC_BLOCK_SIZE);
        num_interfaces = 7u;

        // Adjust interface and endpoint numbers in subsequent blocks if needed
        if (USB_ModeConfig_ModeHasHostCdcAcm(mode)) {
            // ACM block should start after UAC's interfaces (4,5,6)
            uint16_t acm_block_start = total_len;
            memcpy(&USBFS_RuntimeConfigDescriptor[total_len], USBFS_RuntimeAcmBlock, USBFS_RUNTIME_ACM_BLOCK_SIZE);
            total_len = (uint16_t)(total_len + USBFS_RUNTIME_ACM_BLOCK_SIZE);
            // Patch ACM block interface numbers: +3 (to start at interface 7 instead of 4)
            for (uint16_t i = 0; i < USBFS_RUNTIME_ACM_BLOCK_SIZE; i += 9) {
                // Interface descriptor is 9 bytes: 0x09, 0x04, interface_num, ...
                if (i + 2 < USBFS_RUNTIME_ACM_BLOCK_SIZE) {
                    USBFS_RuntimeConfigDescriptor[acm_block_start + i + 2] += 3;
                }
            }
            // Patch ACM block endpoint addresses: +2 (to start at endpoint 7 instead of 5)
            // UAC uses endpoints 5-6, so ACM should start at 7
            for (uint16_t i = 0; i < USBFS_RUNTIME_ACM_BLOCK_SIZE - 2; i++) {
                if (USBFS_RuntimeConfigDescriptor[acm_block_start + i] == 0x07 &&
                    USBFS_RuntimeConfigDescriptor[acm_block_start + i + 1] == 0x05) {
                    // This is an endpoint descriptor, patch the address
                    uint8_t addr = USBFS_RuntimeConfigDescriptor[acm_block_start + i + 2];
                    uint8_t ep_num = addr & 0x0F;
                    uint8_t direction = addr & 0x80;
                    USBFS_RuntimeConfigDescriptor[acm_block_start + i + 2] = direction | (ep_num + 2);
                }
            }
            num_interfaces = 9u;
        }
        else if (USB_ModeConfig_ModeHasHostCdcEcm(mode)) {
            // ECM block should start after UAC's interfaces (4,5,6)
            uint16_t ecm_block_start = total_len;
            memcpy(&USBFS_RuntimeConfigDescriptor[total_len], USBFS_RuntimeEcmBlock, USBFS_RUNTIME_ECM_BLOCK_SIZE);
            total_len = (uint16_t)(total_len + USBFS_RUNTIME_ECM_BLOCK_SIZE);
            // Patch ECM block interface numbers: +3 (to start at interface 7 instead of 4)
            for (uint16_t i = 0; i < USBFS_RUNTIME_ECM_BLOCK_SIZE; i += 9) {
                // Interface descriptor is 9 bytes: 0x09, 0x04, interface_num, ...
                if (i + 2 < USBFS_RUNTIME_ECM_BLOCK_SIZE) {
                    USBFS_RuntimeConfigDescriptor[ecm_block_start + i + 2] += 3;
                }
            }
            // Patch ECM block endpoint addresses: +2 (to start at endpoint 7 instead of 5)
            // UAC uses endpoints 5-6, so ECM should start at 7
            for (uint16_t i = 0; i < USBFS_RUNTIME_ECM_BLOCK_SIZE - 2; i++) {
                if (USBFS_RuntimeConfigDescriptor[ecm_block_start + i] == 0x07 &&
                    USBFS_RuntimeConfigDescriptor[ecm_block_start + i + 1] == 0x05) {
                    // This is an endpoint descriptor, patch the address
                    uint8_t addr = USBFS_RuntimeConfigDescriptor[ecm_block_start + i + 2];
                    uint8_t ep_num = addr & 0x0F;
                    uint8_t direction = addr & 0x80;
                    USBFS_RuntimeConfigDescriptor[ecm_block_start + i + 2] = direction | (ep_num + 2);
                }
            }
            num_interfaces = 9u;
        }
    }
    else {
        if (USB_ModeConfig_ModeHasHostCdcAcm(mode)) {
            memcpy(&USBFS_RuntimeConfigDescriptor[total_len], USBFS_RuntimeAcmBlock, USBFS_RUNTIME_ACM_BLOCK_SIZE);
            total_len = (uint16_t)(total_len + USBFS_RUNTIME_ACM_BLOCK_SIZE);
            num_interfaces = 6u;
        }
        if (USB_ModeConfig_ModeHasHostCdcEcm(mode)) {
            memcpy(&USBFS_RuntimeConfigDescriptor[total_len], USBFS_RuntimeEcmBlock, USBFS_RUNTIME_ECM_BLOCK_SIZE);
            total_len = (uint16_t)(total_len + USBFS_RUNTIME_ECM_BLOCK_SIZE);

            // Adjust interface and endpoint numbers in ECM block if ACM block was also added
            if (USB_ModeConfig_ModeHasHostCdcAcm(mode)) {
                // ECM block starts at offset total_len - USBFS_RUNTIME_ECM_BLOCK_SIZE
                uint16_t ecm_block_start = total_len - USBFS_RUNTIME_ECM_BLOCK_SIZE;
                // Patch ECM block interface numbers: +2 (to start at interface 6 instead of 4)
                for (uint16_t i = 0; i < USBFS_RUNTIME_ECM_BLOCK_SIZE; i += 9) {
                    // Interface descriptor is 9 bytes: 0x09, 0x04, interface_num, ...
                    if (i + 2 < USBFS_RUNTIME_ECM_BLOCK_SIZE) {
                        USBFS_RuntimeConfigDescriptor[ecm_block_start + i + 2] += 2;
                    }
                }
                // Patch ECM block endpoint addresses: +2 (to start at endpoint 7 instead of 5)
                // ACM uses endpoints 5-6, so ECM should start at 7
                for (uint16_t i = 0; i < USBFS_RUNTIME_ECM_BLOCK_SIZE - 2; i++) {
                    if (USBFS_RuntimeConfigDescriptor[ecm_block_start + i] == 0x07 &&
                        USBFS_RuntimeConfigDescriptor[ecm_block_start + i + 1] == 0x05) {
                        // This is an endpoint descriptor, patch the address
                        uint8_t addr = USBFS_RuntimeConfigDescriptor[ecm_block_start + i + 2];
                        uint8_t ep_num = addr & 0x0F;
                        uint8_t direction = addr & 0x80;
                        USBFS_RuntimeConfigDescriptor[ecm_block_start + i + 2] = direction | (ep_num + 2);
                    }
                }
                num_interfaces = 8u;
            }
        }
    }

    USBFS_RuntimeConfigDescriptor[2] = (uint8_t)total_len;
    USBFS_RuntimeConfigDescriptor[3] = (uint8_t)(total_len >> 8);
    USBFS_RuntimeConfigDescriptor[4] = num_interfaces;
    USBFS_RuntimeConfigMode = mode;
    USBFS_RuntimeConfigLen = total_len;
}
#endif

const uint8_t *USBFS_GetActiveConfigDescriptorData(uint16_t *len)
{
#if USB_WORK_MODE_RUNTIME_PROFILES
    USBFS_BuildRuntimeConfigDescriptor();
    if (len != 0) {
        *len = USBFS_RuntimeConfigLen;
    }
    return USBFS_RuntimeConfigDescriptor;
#else
    if (len != 0) {
        *len = DEF_USBD_CONFIG_DESC_LEN;
    }
    return MyCfgDescr;
#endif
}

uint8_t USBFS_GetActiveConfigAttributes(void)
{
    return USBFS_GetActiveConfigDescriptorData(0)[7];
}

/* Keyboard Report Descriptor */
const uint8_t KeyRepDesc[ ] =
{
    0x05, 0x01,                                             // Usage Page (Generic Desktop)
    0x09, 0x06,                                             // Usage (Keyboard)
    0xA1, 0x01,                                             // Collection (Application)
    0x05, 0x07,                                             // Usage Page (Key Codes)
    0x19, 0xE0,                                             // Usage Minimum (224)
    0x29, 0xE7,                                             // Usage Maximum (231)
    0x15, 0x00,                                             // Logical Minimum (0)
    0x25, 0x01,                                             // Logical Maximum (1)
    0x75, 0x01,                                             // Report Size (1)
    0x95, 0x08,                                             // Report Count (8)
    0x81, 0x02,                                             // Input (Data,Variable,Absolute)
    0x95, 0x01,                                             // Report Count (1)
    0x75, 0x08,                                             // Report Size (8)
    0x81, 0x01,                                             // Input (Constant)
    0x95, 0x03,                                             // Report Count (3)
    0x75, 0x01,                                             // Report Size (1)
    0x05, 0x08,                                             // Usage Page (LEDs)
    0x19, 0x01,                                             // Usage Minimum (1)
    0x29, 0x03,                                             // Usage Maximum (3)
    0x91, 0x02,                                             // Output (Data,Variable,Absolute)
    0x95, 0x05,                                             // Report Count (5)
    0x75, 0x01,                                             // Report Size (1)
    0x91, 0x01,                                             // Output (Constant,Array,Absolute)
    0x95, 0x06,                                             // Report Count (6)
    0x75, 0x08,                                             // Report Size (8)
    0x26, 0xFF, 0x00,                                       // Logical Maximum (255)
    0x05, 0x07,                                             // Usage Page (Key Codes)
    0x19, 0x00,                                             // Usage Minimum (0)
    0x29, 0x91,                                             // Usage Maximum (145)
    0x81, 0x00,                                             // Input(Data,Array,Absolute)
    0xC0                                                    // End Collection
};

/* Mouse Report Descriptor */
const uint8_t MouseRepDesc[ ] =
{
    0x05, 0x01,                                             // Usage Page (Generic Desktop)
    0x09, 0x02,                                             // Usage (Mouse)
    0xA1, 0x01,                                             // Collection (Application)
    0x09, 0x01,                                             // Usage (Pointer)
    0xA1, 0x00,                                             // Collection (Physical)
    0x05, 0x09,                                             // Usage Page (Button)
    0x19, 0x01,                                             // Usage Minimum (Button 1)
    0x29, 0x03,                                             // Usage Maximum (Button 3)
    0x15, 0x00,                                             // Logical Minimum (0)
    0x25, 0x01,                                             // Logical Maximum (1)
    0x75, 0x01,                                             // Report Size (1)
    0x95, 0x03,                                             // Report Count (3)
    0x81, 0x02,                                             // Input (Data,Variable,Absolute)
    0x75, 0x05,                                             // Report Size (5)
    0x95, 0x01,                                             // Report Count (1)
    0x81, 0x01,                                             // Input (Constant,Array,Absolute)
    0x05, 0x01,                                             // Usage Page (Generic Desktop)
    0x09, 0x30,                                             // Usage (X)
    0x09, 0x31,                                             // Usage (Y)
    0x09, 0x38,                                             // Usage (Wheel)
    0x15, 0x81,                                             // Logical Minimum (-127)
    0x25, 0x7F,                                             // Logical Maximum (127)
    0x75, 0x08,                                             // Report Size (8)
    0x95, 0x03,                                             // Report Count (3)
    0x81, 0x06,                                             // Input (Data,Variable,Relative)
    0xC0,                                                   // End Collection
    0xC0                                                    // End Collection
};

/* EP3 Report Descriptor — selected at compile time by EP3_USE_STYLUS
 *
 * EP3_USE_STYLUS not defined  →  AbsMouseRepDesc = Generic Desktop Mouse + Absolute
 *                                  6-byte report: [buttons | X_L|X_H | Y_L|Y_H | wheel]
 *                                  Works on: Windows, iOS
 *
 * EP3_USE_STYLUS defined      →  AbsMouseRepDesc = Digitizer Pen / Stylus
 *                                  5-byte report: [InRange+Tip | X_L|X_H | Y_L|Y_H]
 *                                  Works on: Windows, iOS, Android
 *
 * To switch: edit Version_selection.h — comment/uncomment #define EP3_USE_STYLUS
 */
#ifdef EP3_USE_STYLUS
/* --- Digitizer Pen / Stylus (54 bytes = 0x36) --- */
const uint8_t AbsMouseRepDesc[ ] =
{
    0x05, 0x0D,                                             // Usage Page (Digitizers)
    0x09, 0x02,                                             // Usage (Pen)
    0xA1, 0x01,                                             // Collection (Application)
    0x09, 0x20,                                             //   Usage (Stylus)
    0xA1, 0x00,                                             //   Collection (Physical)
    0x09, 0x42,                                             //     Usage (Tip Switch)   bit0
    0x09, 0x32,                                             //     Usage (In Range)     bit1
    0x15, 0x00,                                             //     Logical Minimum (0)
    0x25, 0x01,                                             //     Logical Maximum (1)
    0x75, 0x01,                                             //     Report Size (1)
    0x95, 0x02,                                             //     Report Count (2)
    0x81, 0x02,                                             //     Input (Data,Variable,Absolute)
    0x95, 0x06,                                             //     Report Count (6) — padding
    0x81, 0x03,                                             //     Input (Constant,Variable,Absolute)
    0x05, 0x01,                                             //     Usage Page (Generic Desktop)
    0x09, 0x30,                                             //     Usage (X)
    0x09, 0x31,                                             //     Usage (Y)
    0x16, 0x00, 0x00,                                       //     Logical Minimum (0)
    0x26, 0xFF, 0x7F,                                       //     Logical Maximum (32767)
    0x36, 0x00, 0x00,                                       //     Physical Minimum (0)
    0x46, 0xFF, 0x7F,                                       //     Physical Maximum (32767)
    0x75, 0x10,                                             //     Report Size (16)
    0x95, 0x02,                                             //     Report Count (2)
    0x81, 0x02,                                             //     Input (Data,Variable,Absolute)
    0xC0,                                                   //   End Collection
    0xC0                                                    // End Collection
    /* Total: 54 bytes = 0x36 */
};
#else
/* --- Generic Desktop Mouse + Absolute (63 bytes = 0x3F) --- */
const uint8_t AbsMouseRepDesc[ ] =
{
    0x05, 0x01,                                             // Usage Page (Generic Desktop)
    0x09, 0x02,                                             // Usage (Mouse)
    0xA1, 0x01,                                             // Collection (Application)
    0x09, 0x01,                                             // Usage (Pointer)
    0xA1, 0x00,                                             // Collection (Physical)
    0x05, 0x09,                                             // Usage Page (Button)
    0x19, 0x01,                                             // Usage Minimum (Button 1)
    0x29, 0x03,                                             // Usage Maximum (Button 3)
    0x15, 0x00,                                             // Logical Minimum (0)
    0x25, 0x01,                                             // Logical Maximum (1)
    0x95, 0x03,                                             // Report Count (3)
    0x75, 0x01,                                             // Report Size (1)
    0x81, 0x02,                                             // Input (Data,Variable,Absolute)
    0x95, 0x01,                                             // Report Count (1)
    0x75, 0x05,                                             // Report Size (5)
    0x81, 0x01,                                             // Input (Constant,Array,Absolute)
    0x05, 0x01,                                             // Usage Page (Generic Desktop)
    0x09, 0x30,                                             // Usage (X)
    0x09, 0x31,                                             // Usage (Y)
    0x15, 0x00,                                             // Logical Minimum (0)
    0x26, 0xFF, 0x7F,                                       // Logical Maximum (32767)
    0x75, 0x10,                                             // Report Size (16)
    0x95, 0x02,                                             // Report Count (2)
    0x81, 0x02,                                             // Input (Data,Variable,Absolute)
    0x09, 0x38,                                             // Usage (Wheel)
    0x15, 0x81,                                             // Logical Minimum (-127)
    0x25, 0x7F,                                             // Logical Maximum (127)
    0x75, 0x08,                                             // Report Size (8)
    0x95, 0x01,                                             // Report Count (1)
    0x81, 0x06,                                             // Input (Data,Variable,Relative)
    0xC0,                                                   // End Collection
    0xC0                                                    // End Collection
};
#endif /* EP3_USE_STYLUS */

/* Media Key (Consumer Control + ACPI) Report Descriptor
 * Matches CH9329 CMD=0x03 protocol exactly:
 *
 * Report ID 0x01 (ACPI, 1 data byte):
 *   Byte2: bit0=Power, bit1=Sleep, bit2=Wake-up, bit3-7=reserved
 *
 * Report ID 0x02 (Media, 3 data bytes):
 *   Byte2: bit0=Volume+, bit1=Volume-, bit2=Mute,      bit3=Play/Pause,
 *          bit4=Next,    bit5=Prev,   bit6=CD Stop,    bit7=Eject
 *   Byte3: bit0=E-Mail,  bit1=Search, bit2=Favorites,  bit3=WWW Home,
 *          bit4=Back,    bit5=Forward,bit6=WWW Stop,   bit7=Refresh
 *   Byte4: bit0=Media,   bit1=Explorer,bit2=Calculator,bit3=ScreenSave,
 *          bit4=MyPC,    bit5=Minimize,bit6=Record,    bit7=Rewind
 */
const uint8_t MediaKeyRepDesc[ ] =
{
    /* ---- Report ID 0x01: ACPI / System Control ---- */
    /* Total: 20 bytes */
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x80,        // Usage (System Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x03,        //   Report Count (3)
    0x09, 0x81,        //   Usage (System Power Down) bit0
    0x09, 0x82,        //   Usage (System Sleep)      bit1
    0x09, 0x83,        //   Usage (System Wake Up)    bit2
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0x95, 0x05,        //   Report Count (5) — padding
    0x81, 0x01,        //   Input (Constant)
    0xC0,              // End Collection

    /* ---- Report ID 0x02: Consumer Control (3 data bytes) ---- */
    /* Total: 82 bytes */
    0x05, 0x0C,        // Usage Page (Consumer Devices)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    /* --- Byte2: 1-byte usages (all <= 0xFF, Consumer Page) --- */
    0x95, 0x08,        //   Report Count (8)
    0x09, 0xE9,        //   Usage (Volume Increment)   bit0  (Volume+)
    0x09, 0xEA,        //   Usage (Volume Decrement)   bit1  (Volume-)
    0x09, 0xE2,        //   Usage (Mute)               bit2
    0x09, 0xCD,        //   Usage (Play/Pause)         bit3
    0x09, 0xB5,        //   Usage (Scan Next Track)    bit4
    0x09, 0xB6,        //   Usage (Scan Prev Track)    bit5
    0x09, 0xB7,        //   Usage (Stop)               bit6  (CD Stop)
    0x09, 0xB8,        //   Usage (Eject)              bit7
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    /* --- Byte3: AL/AC usages (2-byte usages, Consumer Page) --- */
    0x95, 0x08,        //   Report Count (8)
    0x0A, 0x8A, 0x01,  //   Usage (AL Email Reader)       bit0  (0x018A)
    0x0A, 0x21, 0x02,  //   Usage (AC Search)             bit1  (0x0221)
    0x0A, 0x2C, 0x02,  //   Usage (AC Bookmarks/Fav)      bit2  (0x022C)
    0x0A, 0x23, 0x02,  //   Usage (AC Home)               bit3  (0x0223)
    0x0A, 0x24, 0x02,  //   Usage (AC Back)               bit4  (0x0224)
    0x0A, 0x25, 0x02,  //   Usage (AC Forward)            bit5  (0x0225)
    0x0A, 0x26, 0x02,  //   Usage (AC Stop)               bit6  (0x0226) WWW Stop
    0x0A, 0x27, 0x02,  //   Usage (AC Refresh)            bit7  (0x0227)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    /* --- Byte4: AL/mixed usages (2-byte usages, Consumer Page) --- */
    0x95, 0x08,        //   Report Count (8)
    0x0A, 0x83, 0x01,  //   Usage (AL Consumer Control)   bit0  (0x0183) Media
    0x0A, 0x94, 0x01,  //   Usage (AL Local Browser)      bit1  (0x0194) Explorer
    0x0A, 0x92, 0x01,  //   Usage (AL Calculator)         bit2  (0x0192)
    0x0A, 0x84, 0x01,  //   Usage (AL Screen Saver)       bit3  (0x0184)
    0x0A, 0xC4, 0x01,  //   Usage (AL My Computer)        bit4  (0x01C4)
    0x0A, 0x06, 0x02,  //   Usage (AC Minimize)           bit5  (0x0206)
    0x09, 0xB2,        //   Usage (Record)                bit6  (0x00B2)
    0x09, 0xB4,        //   Usage (Rewind)                bit7  (0x00B4)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0xC0,              // End Collection

    /* ---- Report ID 0x30: Vendor USB mode control Feature report ---- */
    0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,        // Usage (Vendor Usage 1)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x30,        //   Report ID (0x30)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x3F,        //   Report Count (63 bytes + report ID = 64 bytes)
    0x09, 0x02,        //   Usage (Vendor Usage 2)
    0xB1, 0x02,        //   Feature (Data, Variable, Absolute)
    0xC0               // End Collection
};

/* Touch Screen Report Descriptor */
const uint8_t TouchScreenRepDesc[] = {
    0x05, 0x0D,                // Usage Page (Digitizers)
    0x09, 0x04,                // Usage (Touch Screen)
    0xA1, 0x01,                // Collection (Application)
    0x09, 0x22,                //   Usage (Finger)
    0xA1, 0x00,                //   Collection (Physical)
    0x09, 0x42,                //     Usage (Tip Switch)
    0x15, 0x00,                //     Logical Minimum (0)
    0x25, 0x01,                //     Logical Maximum (1)
    0x75, 0x01,                //     Report Size (1)
    0x95, 0x01,                //     Report Count (1)
    0x81, 0x02,                //     Input (Data,Var,Abs)
    0x95, 0x07,                //     Report Count (7) - padding
    0x81, 0x03,                //     Input (Const,Var,Abs)
    0x05, 0x01,                //     Usage Page (Generic Desktop)
    0x09, 0x30,                //     Usage (X)
    0x09, 0x31,                //     Usage (Y)
    0x16, 0x00, 0x00,          //     Logical Min (0)
    0x26, 0xFF, 0x7F,          //     Logical Max (32767)
    0x36, 0x00, 0x00,          //     Physical Min (0)
    0x46, 0xFF, 0x7F,          //     Physical Max (32767)
    0x75, 0x10,                //     Report Size (16)
    0x95, 0x02,                //     Report Count (2)
    0x81, 0x02,                //     Input (Data,Var,Abs)
    0xC0,                      //   End Collection
    0xC0                       // End Collection
};


/* Qualifier Descriptor */
const uint8_t  MyQuaDesc[ ] =
{
    0x0A,                                                   // bLength
    0x06,                                                   // bDescriptorType
    0x00, 0x02,                                             // bcdUSB
    0x00,                                                   // bDeviceClass
    0x00,                                                   // bDeviceSubClass
    0x00,                                                   // bDeviceProtocol
    0x40,                                                   // bMaxPacketSize0
    0x00,                                                   // bNumConfigurations
    0x00                                                    // bReserved
};

/* Language Descriptor */
const uint8_t MyLangDescr[ ] =
{
    0x04,       // Descriptor length (4 bytes)
    0x03,       // Descriptor type (STRING)
    0x09, 0x04  // LANGID: English (United States)
};

/* Manufacturer Descriptor */
const uint8_t MyManuInfo[ ] =
{
    0x1A, // Descriptor length (13 chars * 2 + 2)
    0x03, // Descriptor type (STRING)
    'T',0, // TechxArtisan
    'e',0,
    'c',0,
    'h',0,
    'x',0,
    'A',0,
    'r',0,
    't',0,
    'i',0,
    's',0,
    'a',0,
    'n',0
};

/* Product Information */
const uint8_t MyProdInfo[ ]  =
{
    0x26, // Descriptor length (18 chars * 2 + 2)
    0x03, // Descriptor type (STRING)
    'O', 0,
    'p', 0,
    'e', 0,
    'n', 0,
    't', 0,
    'e', 0,
    'r', 0,
    'f', 0,
    'a', 0,
    'c', 0,
    'e', 0,
    ' ', 0,
    'T', 0,
    'a', 0,
    'r', 0,
    'g', 0,
    'e', 0,
    't', 0
};

/* Serial Number Information */
const uint8_t  MySerNumInfo[ ] =
{
    0x12, // Descriptor length (8 chars * 2 + 2)
    0x03, // Descriptor type (STRING)
    '2', 0,
    '0', 0,
    '2', 0,
    '5', 0,
    '0', 0,
    '7', 0,
    '2', 0,
    '1', 0
};

/* CDC ECM MAC address string: ASCII hex, no separators. */
const uint8_t MyEcmMacInfo[ ] =
{
    0x1A,
    0x03,
    '0', 0, '2', 0, '0', 0, '0', 0, '0', 0, '0', 0,
    '0', 0, '0', 0, '0', 0, '0', 0, '3', 0, '0', 0
};
