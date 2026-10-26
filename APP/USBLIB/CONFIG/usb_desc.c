/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_desc.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2019/10/15
 * Description        : USB Descriptors optimized for Android compatibility.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
*******************************************************************************/
#include "usb_lib.h"
#include "usb_desc.h"
#include "../../include/Version_selection.h"
#include "../../include/uac_desc.h"
#include "../../include/usb_mode_config.h"
#include <string.h>

/* USB Device Descriptors */
const uint8_t  USBD_DeviceDescriptor[] = {
    USBD_SIZE_DEVICE_DESC,           // bLength
    0x01,                           // bDescriptorType
    0x10, 0x01,                     // bcdUSB 1.10
#if USB_WORK_MODE_HAS_HOST_CDC_ECM
    /* Composite device with IAD (CDC ACM + CDC ECM) */
    0xEF,                           // bDeviceClass: Miscellaneous (IAD for composite)
    0x02,                           // bDeviceSubClass: Common Class
    0x01,                           // bDeviceProtocol: Interface Association Descriptor
#else
    /* Single-function CDC device (CDC ACM only) */
    0x02,                           // bDeviceClass: Communications (CDC)
    0x00,                           // bDeviceSubClass: No subclass
    0x00,                           // bDeviceProtocol: No specific protocol
#endif
    DEF_USBD_UEP0_SIZE,             // bMaxPacketSize0
    0x86, 0x1A,                     // idVendor
    0x0C, 0xFE,                     // idProduct
    0x04, 0x01,                     // bcdDevice
    0x01,                           // iManufacturer
    0x02,                           // iProduct
    0x00,                           // iSerialNumber
    0x01,                           // bNumConfigurations
};

/* USB Configuration Descriptors */
const uint8_t  USBD_ConfigDescriptor[] = {
    /* Configuration Descriptor */
    0x09,                           // bLength
    0x02,                           // bDescriptorType
    USBD_SIZE_CONFIG_DESC & 0xFF, USBD_SIZE_CONFIG_DESC >> 8, // wTotalLength
#if USB_WORK_MODE_HAS_HOST_CDC_ACM && USB_WORK_MODE_HAS_HOST_CDC_ECM && USB_WORK_MODE_HAS_TARGET_CDC_ECM
    0x06,                           // bNumInterfaces: ACM(2) + Host ECM(2) + Target ECM(2) = 6
#elif USB_WORK_MODE_HAS_HOST_CDC_ACM && USB_WORK_MODE_HAS_HOST_CDC_ECM
    0x04,                           // bNumInterfaces: ACM(2) + ECM(2) = 4
#elif USB_WORK_MODE_HAS_HOST_CDC_ECM && USB_WORK_MODE_HAS_HOST_UAC
    0x05,                           // bNumInterfaces: ECM(2) + UAC(3) = 5
#elif USB_WORK_MODE_HAS_HOST_CDC_ECM
    0x02,                           // bNumInterfaces: CDC-ECM only
#elif USB_WORK_MODE_HAS_HOST_UAC
    0x05,                           // bNumInterfaces: ACM(2) + UAC(3) = 5
#else
    0x02,                           // bNumInterfaces: CDC-ACM only
#endif
    0x01,                           // bConfigurationValue
    0x00,                           // iConfiguration
    0x80,                           // bmAttributes: Bus Powered; Remote Wakeup
    0x32,                           // MaxPower: 100mA

#if USB_WORK_MODE_HAS_HOST_CDC_ACM
    /* IAD: CDC ACM (IF0=comm + IF1=data) */
    0x08,                           // bLength
    0x0B,                           // bDescriptorType (IAD)
    0x00,                           // bFirstInterface: 0
    0x02,                           // bInterfaceCount: 2 (IF0+IF1)
    0x02,                           // bFunctionClass: Communications
    0x02,                           // bFunctionSubClass: Abstract Control Model
    0x01,                           // bFunctionProtocol: AT Commands
    0x00,                           // iFunction

    /* Interface 0 (CDC) descriptor */
    0x09,                           // bLength
    0x04,                           // bDescriptorType (Interface)
    0x00,                           // bInterfaceNumber 0
    0x00,                           // bAlternateSetting
    0x01,                           // bNumEndpoints 1
    0x02,                           // bInterfaceClass (Communications)
    0x02,                           // bInterfaceSubClass (Abstract Control Model)
    0x01,                           // bInterfaceProtocol (AT Commands)
    0x00,                           // iInterface (String Index)

    /* Functional Descriptors */
    0x05,0x24,0x00, 0x10, 0x01,
    0x05, 0x24, 0x01, 0x00, 0x01,
    0x04, 0x24, 0x02, 0x02,
    0x05, 0x24, 0x06, 0x00, 0x01,

    /* Interrupt upload endpoint descriptor */
    0x07,                           // bLength
    0x05,                           // bDescriptorType (Endpoint)
    0x81,                           // bEndpointAddress (IN/D2H)
    0x03,                           // bmAttributes (Interrupt)
    0x10, 0x00,                     // wMaxPacketSize 16 (PMA reserved for double-buffered UAC audio)
    0x01,                           // bInterval 1 (unit depends on device speed)

    /* Interface 1 (data interface) descriptor */
    0x09,                           // bLength
    0x04,                           // bDescriptorType (Interface)
    0x01,                           // bInterfaceNumber 1
    0x00,                           // bAlternateSetting
    0x02,                           // bNumEndpoints 2
    0x0A,                           // bInterfaceClass (Data Interface)
    0x00,                           // bInterfaceSubClass
    0x00,                           // bInterfaceProtocol
    0x00,                           // iInterface (String Index)

    /* Endpoint descriptor — CDC bulk OUT */
    0x07,                           // bLength
    0x05,                           // bDescriptorType (Endpoint)
    0x05,                           // bEndpointAddress (OUT/H2D)
    0x02,                           // bmAttributes (Bulk)
    0x10, 0x00,                     // wMaxPacketSize 16 (PMA reserved for double-buffered UAC audio)
    0x00,                           // bInterval 0

    /* Endpoint descriptor — CDC bulk IN */
    0x07,                           // bLength
    0x05,                           // bDescriptorType (Endpoint)
    0x83,                           // bEndpointAddress (IN/D2H)
    0x02,                           // bmAttributes (Bulk)
#if USB_WORK_MODE_HAS_HOST_CDC_ACM && USB_WORK_MODE_HAS_HOST_CDC_ECM
    0x40, 0x00,                     // wMaxPacketSize 64 (ACM+ECM mode: 64 bytes for CH9329 responses)
#else
    0x10, 0x00,                     // wMaxPacketSize 16 (PMA reserved for double-buffered UAC audio)
#endif
    0x00,                           // bInterval 0

#if USB_WORK_MODE_HAS_HOST_CDC_ECM
    /* ====== CDC ECM (combined with ACM): IF2=comm + IF3=data ==========
     * Endpoints shifted to avoid conflict with ACM (EP1/EP3/EP5):
     *   EP2 IN  (0x82) — interrupt notification
     *   EP6 OUT (0x06) — bulk data host→device
     *   EP4 IN  (0x84) — bulk data device→host
     * =================================================================== */

    /* IAD: CDC ECM (IF2=comm + IF3=data) */
    0x08,                           // bLength
    0x0B,                           // bDescriptorType (IAD)
    0x02,                           // bFirstInterface: 2
    0x02,                           // bInterfaceCount: 2
    0x02,                           // bFunctionClass: Communications
    0x06,                           // bFunctionSubClass: Ethernet Networking Control Model
    0x00,                           // bFunctionProtocol
    0x00,                           // iFunction

    /* Interface 2: CDC ECM communication */
    0x09,                           // bLength
    0x04,                           // bDescriptorType (Interface)
    0x02,                           // bInterfaceNumber 2
    0x00,                           // bAlternateSetting
    0x01,                           // bNumEndpoints 1
    0x02,                           // bInterfaceClass (Communications)
    0x06,                           // bInterfaceSubClass (Ethernet Networking Control Model)
    0x00,                           // bInterfaceProtocol
    0x00,                           // iInterface

    /* CDC ECM Functional Descriptors */
    0x05, 0x24, 0x00, 0x10, 0x01,   // Header, CDC 1.10
    0x05, 0x24, 0x06, 0x02, 0x03,   // Union: control IF2, data IF3
    0x0D, 0x24, 0x0F, 0x04,         // Ethernet Networking, iMACAddress=4
    0x00, 0x00, 0x00, 0x00,         // bmEthernetStatistics
    0xEA, 0x05,                     // wMaxSegmentSize: 1514
    0x00, 0x00,                     // wNumberMCFilters: none
    0x00,                           // bNumberPowerFilters

    /* Interrupt notification endpoint — EP2 IN (0x82) */
    0x07,                           // bLength
    0x05,                           // bDescriptorType (Endpoint)
    0x82,                           // bEndpointAddress (IN/D2H)
    0x03,                           // bmAttributes (Interrupt)
    0x10, 0x00,                     // wMaxPacketSize 16
    0x01,                           // bInterval

    /* Interface 3: CDC ECM data, alt 0 idle */
    0x09,                           // bLength
    0x04,                           // bDescriptorType (Interface)
    0x03,                           // bInterfaceNumber 3
    0x00,                           // bAlternateSetting
    0x00,                           // bNumEndpoints
    0x0A,                           // bInterfaceClass (Data Interface)
    0x00,                           // bInterfaceSubClass
    0x00,                           // bInterfaceProtocol
    0x00,                           // iInterface

    /* Interface 3: CDC ECM data, alt 1 active */
    0x09,                           // bLength
    0x04,                           // bDescriptorType (Interface)
    0x03,                           // bInterfaceNumber 3
    0x01,                           // bAlternateSetting
    0x02,                           // bNumEndpoints
    0x0A,                           // bInterfaceClass (Data Interface)
    0x00,                           // bInterfaceSubClass
    0x00,                           // bInterfaceProtocol
    0x00,                           // iInterface

    /* Endpoint descriptor — CDC ECM bulk OUT — EP6 OUT (0x06) */
    0x07,                           // bLength
    0x05,                           // bDescriptorType (Endpoint)
    0x06,                           // bEndpointAddress (OUT/H2D)
    0x02,                           // bmAttributes (Bulk)
    0x40, 0x00,                     // wMaxPacketSize 64 (ECM bridge transmits 64-byte bulk packets)
    0x00,                           // bInterval

    /* Endpoint descriptor — CDC ECM bulk IN — EP4 IN (0x84) */
    0x07,                           // bLength
    0x05,                           // bDescriptorType (Endpoint)
    0x84,                           // bEndpointAddress (IN/D2H)
    0x02,                           // bmAttributes (Bulk)
    0x40, 0x00,                     // wMaxPacketSize 64 (ECM bridge transmits 64-byte bulk packets)
    0x00,                           // bInterval

#if USB_WORK_MODE_HAS_TARGET_CDC_ECM
    /* ====== Target CDC ECM: IF4=comm + IF5=data ==========
     * Endpoints:
     *   EP7 OUT (0x07) — bulk data host→device (Target ECM)
     *   EP7 IN  (0x87) — bulk data device→host (Target ECM)
     * Note: CH32V20x only has EP0-EP7 (no EP8). EP7 supports both directions.
     * =================================================================== */

    /* IAD: Target CDC ECM (IF4=comm + IF5=data) */
    0x08,                           // bLength
    0x0B,                           // bDescriptorType (IAD)
    0x04,                           // bFirstInterface: 4
    0x02,                           // bInterfaceCount: 2
    0x02,                           // bFunctionClass: Communications
    0x06,                           // bFunctionSubClass: Ethernet Networking Control Model
    0x00,                           // bFunctionProtocol
    0x00,                           // iFunction

    /* Interface 4: Target CDC ECM communication */
    0x09,                           // bLength
    0x04,                           // bDescriptorType (Interface)
    0x04,                           // bInterfaceNumber 4
    0x00,                           // bAlternateSetting
    0x00,                           // bNumEndpoints 0 (no notification endpoint)
    0x02,                           // bInterfaceClass (Communications)
    0x06,                           // bInterfaceSubClass (Ethernet Networking Control Model)
    0x00,                           // bInterfaceProtocol
    0x00,                           // iInterface

    /* Target CDC ECM Functional Descriptors */
    0x05, 0x24, 0x00, 0x10, 0x01,   // Header, CDC 1.10
    0x05, 0x24, 0x06, 0x04, 0x05,   // Union: control IF4, data IF5
    0x0D, 0x24, 0x0F, 0x05,         // Ethernet Networking, iMACAddress=5
    0x00, 0x00, 0x00, 0x00,         // bmEthernetStatistics
    0xEA, 0x05,                     // wMaxSegmentSize: 1514
    0x00, 0x00,                     // wNumberMCFilters: none
    0x00,                           // bNumberPowerFilters

    /* Interface 5: Target CDC ECM data, alt 0 idle */
    0x09,                           // bLength
    0x04,                           // bDescriptorType (Interface)
    0x05,                           // bInterfaceNumber 5
    0x00,                           // bAlternateSetting
    0x00,                           // bNumEndpoints
    0x0A,                           // bInterfaceClass (Data Interface)
    0x00,                           // bInterfaceSubClass
    0x00,                           // bInterfaceProtocol
    0x00,                           // iInterface

    /* Interface 5: Target CDC ECM data, alt 1 active */
    0x09,                           // bLength
    0x04,                           // bDescriptorType (Interface)
    0x05,                           // bInterfaceNumber 5
    0x01,                           // bAlternateSetting
    0x02,                           // bNumEndpoints
    0x0A,                           // bInterfaceClass (Data Interface)
    0x00,                           // bInterfaceSubClass
    0x00,                           // bInterfaceProtocol
    0x00,                           // iInterface

    /* Endpoint descriptor — Target CDC ECM bulk OUT — EP7 OUT (0x07) */
    0x07,                           // bLength
    0x05,                           // bDescriptorType (Endpoint)
    0x07,                           // bEndpointAddress (OUT/H2D)
    0x02,                           // bmAttributes (Bulk)
    0x40, 0x00,                     // wMaxPacketSize 64
    0x00,                           // bInterval

    /* Endpoint descriptor — Target CDC ECM bulk IN — EP7 IN (0x87) */
    0x07,                           // bLength
    0x05,                           // bDescriptorType (Endpoint)
    0x87,                           // bEndpointAddress (IN/D2H)
    0x02,                           // bmAttributes (Bulk)
    0x40, 0x00,                     // wMaxPacketSize 64
    0x00,                           // bInterval
#endif /* USB_WORK_MODE_HAS_TARGET_CDC_ECM (inside ACM+ECM block) */

#endif /* USB_WORK_MODE_HAS_HOST_CDC_ECM (inside ACM block) */

#elif USB_WORK_MODE_HAS_HOST_CDC_ECM
    /* IAD: CDC ECM (IF0=comm + IF1=data) */
    0x08,                           // bLength
    0x0B,                           // bDescriptorType (IAD)
    0x00,                           // bFirstInterface: 0
    0x02,                           // bInterfaceCount: 2
    0x02,                           // bFunctionClass: Communications
    0x06,                           // bFunctionSubClass: Ethernet Networking Control Model
    0x00,                           // bFunctionProtocol
    0x00,                           // iFunction

    /* Interface 0: CDC ECM communication */
    0x09,                           // bLength
    0x04,                           // bDescriptorType (Interface)
    0x00,                           // bInterfaceNumber 0
    0x00,                           // bAlternateSetting
    0x01,                           // bNumEndpoints 1
    0x02,                           // bInterfaceClass (Communications)
    0x06,                           // bInterfaceSubClass (Ethernet Networking Control Model)
    0x00,                           // bInterfaceProtocol
    0x00,                           // iInterface

    /* CDC ECM Functional Descriptors */
    0x05, 0x24, 0x00, 0x10, 0x01,   // Header, CDC 1.10
    0x05, 0x24, 0x06, 0x00, 0x01,   // Union: control IF0, data IF1
    0x0D, 0x24, 0x0F, 0x04,         // Ethernet Networking, iMACAddress=4
    0x00, 0x00, 0x00, 0x00,         // bmEthernetStatistics
    0xEA, 0x05,                     // wMaxSegmentSize: 1514
    0x00, 0x00,                     // wNumberMCFilters: none
    0x00,                           // bNumberPowerFilters

    /* Interrupt notification endpoint */
    0x07,                           // bLength
    0x05,                           // bDescriptorType (Endpoint)
    0x81,                           // bEndpointAddress (IN/D2H)
    0x03,                           // bmAttributes (Interrupt)
    0x10, 0x00,                     // wMaxPacketSize 16
    0x01,                           // bInterval

    /* Interface 1: CDC ECM data, alt 0 idle */
    0x09,                           // bLength
    0x04,                           // bDescriptorType (Interface)
    0x01,                           // bInterfaceNumber 1
    0x00,                           // bAlternateSetting
    0x00,                           // bNumEndpoints
    0x0A,                           // bInterfaceClass (Data Interface)
    0x00,                           // bInterfaceSubClass
    0x00,                           // bInterfaceProtocol
    0x00,                           // iInterface

    /* Interface 1: CDC ECM data, alt 1 active */
    0x09,                           // bLength
    0x04,                           // bDescriptorType (Interface)
    0x01,                           // bInterfaceNumber 1
    0x01,                           // bAlternateSetting
    0x02,                           // bNumEndpoints
    0x0A,                           // bInterfaceClass (Data Interface)
    0x00,                           // bInterfaceSubClass
    0x00,                           // bInterfaceProtocol
    0x00,                           // iInterface

    /* Endpoint descriptor — CDC ECM bulk OUT */
    0x07,                           // bLength
    0x05,                           // bDescriptorType (Endpoint)
    0x05,                           // bEndpointAddress (OUT/H2D)
    0x02,                           // bmAttributes (Bulk)
    0x40, 0x00,                     // wMaxPacketSize 64 (ECM bridge transmits 64-byte bulk packets)
    0x00,                           // bInterval

    /* Endpoint descriptor — CDC ECM bulk IN */
    0x07,                           // bLength
    0x05,                           // bDescriptorType (Endpoint)
    0x83,                           // bEndpointAddress (IN/D2H)
    0x02,                           // bmAttributes (Bulk)
    0x40, 0x00,                     // wMaxPacketSize 64 (ECM bridge transmits 64-byte bulk packets)
    0x00,                           // bInterval
#endif

#if USB_WORK_MODE_HAS_HOST_CDC_ECM && USB_WORK_MODE_HAS_HOST_UAC
    /* ====== UAC 1.0 — USBD (Target) side (combined with ECM) ===========
     * IF2: AudioControl
     * IF3: AudioStreaming IN — EP4 ISO IN
     * IF4: AudioStreaming OUT — EP2 ISO OUT
     * =================================================================== */

    /* IAD: UAC (IF2=AC + IF3=AS_IN + IF4=AS_OUT) */
    0x08,                           // bLength
    0x0B,                           // bDescriptorType (IAD)
    0x02,                           // bFirstInterface: 2
    0x03,                           // bInterfaceCount: 3 (IF2+IF3+IF4)
    0x01,                           // bFunctionClass: Audio
    0x01,                           // bFunctionSubClass: AudioControl
    0x00,                           // bFunctionProtocol
    0x00,                           // iFunction

    /* --- Interface Descriptor: AudioControl (IF2) --- */
    0x09,                           // bLength
    0x04,                           // bDescriptorType (Interface)
    0x02,                           // bInterfaceNumber 2
    0x00,                           // bAlternateSetting 0
    0x00,                           // bNumEndpoints 0
    0x01,                           // bInterfaceClass (Audio)
    0x01,                           // bInterfaceSubClass (AudioControl)
    0x00,                           // bInterfaceProtocol
    0x00,                           // iInterface

    /* CS Interface: AC Header — 2 streaming interfaces (IF3 IN, IF4 OUT) */
    0x0A,                           // bLength (8 fixed + 2 baInterfaceNr = 10)
    USB_DESC_TYPE_CS_INTERFACE,     // bDescriptorType
    UAC_AC_HEADER,                  // bDescriptorSubtype
    0x00, 0x01,                     // bcdADC 1.00
    0x34, 0x00,                     // wTotalLength: 10+12+9+12+9 = 52 = 0x34
    0x02,                           // bInCollection: 2 streaming interfaces
    0x03,                           // baInterfaceNr(1): IF3 (AudioStreaming IN)
    0x04,                           // baInterfaceNr(2): IF4 (AudioStreaming OUT)

    /* CS Interface: Input Terminal 1 — Microphone */
    0x0C,                           // bLength
    USB_DESC_TYPE_CS_INTERFACE,     // bDescriptorType
    UAC_AC_INPUT_TERMINAL,          // bDescriptorSubtype
    0x01,                           // bTerminalID 1
    UAC_SR_B0(UAC_TERMINAL_MICROPHONE), UAC_SR_B1(UAC_TERMINAL_MICROPHONE),
    0x00,                           // bAssocTerminal
    UAC_CHANNELS,                   // bNrChannels
    0x03, 0x00,                     // wChannelConfig: L+R
    0x00,                           // iChannelNames
    0x00,                           // iTerminal

    /* CS Interface: Output Terminal 2 — USB Streaming OUT */
    0x09,                           // bLength
    USB_DESC_TYPE_CS_INTERFACE,     // bDescriptorType
    UAC_AC_OUTPUT_TERMINAL,         // bDescriptorSubtype
    0x02,                           // bTerminalID 2
    UAC_SR_B0(UAC_TERMINAL_USB_STREAMING), UAC_SR_B1(UAC_TERMINAL_USB_STREAMING),
    0x00,                           // bAssocTerminal
    0x01,                           // bSourceID (IT1 — Microphone)
    0x00,                           // iTerminal

    /* CS Interface: Input Terminal 3 — USB Streaming IN */
    0x0C,                           // bLength
    USB_DESC_TYPE_CS_INTERFACE,     // bDescriptorType
    UAC_AC_INPUT_TERMINAL,          // bDescriptorSubtype
    0x03,                           // bTerminalID 3
    UAC_SR_B0(UAC_TERMINAL_USB_STREAMING), UAC_SR_B1(UAC_TERMINAL_USB_STREAMING),
    0x00,                           // bAssocTerminal
    UAC_CHANNELS,                   // bNrChannels
    0x03, 0x00,                     // wChannelConfig: L+R
    0x00,                           // iChannelNames
    0x00,                           // iTerminal

    /* CS Interface: Output Terminal 4 — Speaker */
    0x09,                           // bLength
    USB_DESC_TYPE_CS_INTERFACE,     // bDescriptorType
    UAC_AC_OUTPUT_TERMINAL,         // bDescriptorSubtype
    0x04,                           // bTerminalID 4
    UAC_SR_B0(UAC_TERMINAL_SPEAKER), UAC_SR_B1(UAC_TERMINAL_SPEAKER),
    0x00,                           // bAssocTerminal
    0x03,                           // bSourceID (IT3 — USB Streaming IN)
    0x00,                           // iTerminal

    /* --- IF3 AudioStreaming IN — alt0, zero-bandwidth --- */
    0x09, 0x04, 0x03, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,

    /* --- IF3 AudioStreaming IN — alt1, active --- */
    0x09, 0x04, 0x03, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,

    /* CS Interface: AS General — linked to OT2 */
    0x07, USB_DESC_TYPE_CS_INTERFACE, UAC_AS_GENERAL,
    0x02,                           // bTerminalLink (OT2)
    0x00,
    UAC_SR_B0(UAC_FORMAT_PCM), UAC_SR_B1(UAC_FORMAT_PCM),

    /* CS Interface: Format Type I */
    0x0B, USB_DESC_TYPE_CS_INTERFACE, UAC_AS_FORMAT_TYPE,
    0x01,                           // bFormatType: Type I
    UAC_CHANNELS,                   // bNrChannels: 2
    UAC_BYTES_PER_SAMPLE,           // bSubFrameSize: 2
    UAC_BIT_RESOLUTION,             // bBitResolution: 16
    0x01,                           // bSamFreqType: 1 frequency
    UAC_SR_B0(UAC_USBD_SAMPLE_RATE), UAC_SR_B1(UAC_USBD_SAMPLE_RATE), UAC_SR_B2(UAC_USBD_SAMPLE_RATE),

    /* Endpoint: EP4 ISO IN — single buffer, 64 bytes */
    0x09, 0x05, UAC_USBD_EP_IN_ADDR, 0x01,
    0x40, 0x00,                     // wMaxPacketSize: 64
    0x01, 0x00, 0x00,               // bInterval=1ms, bRefresh=0, bSynchAddr=0

    /* CS Endpoint */
    0x07, USB_DESC_TYPE_CS_ENDPOINT, UAC_EP_GENERAL, 0x01, 0x00, 0x00, 0x00,

    /* ================================================================
     * IF4: AudioStreaming OUT — alt0 = zero-bandwidth, alt1 = active
     * ================================================================ */

    /* IF4 alt0 — zero-bandwidth */
    0x09, 0x04,                     // bLength, bDescriptorType (Interface)
    0x04,                           // bInterfaceNumber: 4
    0x00,                           // bAlternateSetting: 0
    0x00,                           // bNumEndpoints: 0
    0x01,                           // bInterfaceClass: Audio
    0x02,                           // bInterfaceSubClass: AudioStreaming
    0x00, 0x00,                     // bInterfaceProtocol, iInterface

    /* IF4 alt1 — active streaming */
    0x09, 0x04,                     // bLength, bDescriptorType (Interface)
    0x04,                           // bInterfaceNumber: 4
    0x01,                           // bAlternateSetting: 1
    0x01,                           // bNumEndpoints: 1
    0x01,                           // bInterfaceClass: Audio
    0x02,                           // bInterfaceSubClass: AudioStreaming
    0x00, 0x00,                     // bInterfaceProtocol, iInterface

    /* CS Interface: AS General — linked to IT3 */
    0x07,                           // bLength
    USB_DESC_TYPE_CS_INTERFACE,     // bDescriptorType
    UAC_AS_GENERAL,                 // bDescriptorSubtype
    0x03,                           // bTerminalLink: IT3
    0x00,                           // bDelay
    UAC_SR_B0(UAC_FORMAT_PCM), UAC_SR_B1(UAC_FORMAT_PCM), // wFormatTag: PCM

    /* CS Interface: Format Type I */
    0x0B,                           // bLength
    USB_DESC_TYPE_CS_INTERFACE,     // bDescriptorType
    UAC_AS_FORMAT_TYPE,             // bDescriptorSubtype
    0x01,                           // bFormatType: Type I
    UAC_CHANNELS,                   // bNrChannels
    UAC_BYTES_PER_SAMPLE,           // bSubFrameSize
    UAC_BIT_RESOLUTION,             // bBitResolution
    0x01,                           // bSamFreqType: 1 frequency
    UAC_SR_B0(UAC_USBD_SAMPLE_RATE), UAC_SR_B1(UAC_USBD_SAMPLE_RATE), UAC_SR_B2(UAC_USBD_SAMPLE_RATE),

    /* Endpoint: EP2 ISO OUT — single buffer, 64 bytes/ms */
    0x09,                           // bLength
    0x05,                           // bDescriptorType (Endpoint)
    UAC_USBD_EP_OUT_ADDR,           // bEndpointAddress: EP2 OUT (0x02)
    0x09,                           // bmAttributes: Isochronous, Adaptive
    0x40, 0x00,                     // wMaxPacketSize: 64
    0x01, 0x00, 0x00,               // bInterval=1ms, bRefresh=0, bSynchAddr=0

    /* CS Endpoint */
    0x07, USB_DESC_TYPE_CS_ENDPOINT, UAC_EP_GENERAL, 0x01, 0x00, 0x00, 0x00,

#endif /* USB_WORK_MODE_HAS_HOST_CDC_ECM && USB_WORK_MODE_HAS_HOST_UAC */

    #if USB_WORK_MODE_HAS_HOST_UAC
    /* ====== UAC 1.0 — USBD (Target) side ================================
     * IF2: AudioControl (1 streaming interface: IF3 only)
     * IF3: AudioStreaming IN — EP4 ISO IN (Host audio → Target, 64 B/ms)
     *   Single-buffer ISO, 64 bytes = max packet at FS, ~2ch@16kHz or 1ch@32kHz
        *   Constrained by USBD PMA: 512 B total, 448 B already used by EP0-3
        * =================================================================== */

    /* IAD: UAC (IF2=AC + IF3=AS_IN + IF4=AS_OUT) */
    0x08,                           // bLength
    0x0B,                           // bDescriptorType (IAD)
    0x02,                           // bFirstInterface: 2
    0x03,                           // bInterfaceCount: 3 (IF2+IF3+IF4)
    0x01,                           // bFunctionClass: Audio
    0x01,                           // bFunctionSubClass: AudioControl
    0x00,                           // bFunctionProtocol
    0x00,                           // iFunction

    /* --- Interface Descriptor: AudioControl (IF2) --- */
    0x09,                           // bLength
    0x04,                           // bDescriptorType (Interface)
    0x02,                           // bInterfaceNumber 2
    0x00,                           // bAlternateSetting 0
    0x00,                           // bNumEndpoints 0
    0x01,                           // bInterfaceClass (Audio)
    0x01,                           // bInterfaceSubClass (AudioControl)
    0x00,                           // bInterfaceProtocol
    0x00,                           // iInterface

    /* CS Interface: AC Header — 2 streaming interfaces (IF3 IN, IF4 OUT) */
    0x0A,                           // bLength (8 fixed + 2 baInterfaceNr = 10)
    USB_DESC_TYPE_CS_INTERFACE,     // bDescriptorType
    UAC_AC_HEADER,                  // bDescriptorSubtype
    0x00, 0x01,                     // bcdADC 1.00
    0x34, 0x00,                     // wTotalLength: 10+12+9+12+9 = 52 = 0x34
    0x02,                           // bInCollection: 2 streaming interfaces
    0x03,                           // baInterfaceNr(1): IF3 (AudioStreaming IN)
    0x04,                           // baInterfaceNr(2): IF4 (AudioStreaming OUT)

    /* CS Interface: Input Terminal 1 — Microphone (physical input on Target side) */
    0x0C,                           // bLength
    USB_DESC_TYPE_CS_INTERFACE,     // bDescriptorType
    UAC_AC_INPUT_TERMINAL,          // bDescriptorSubtype
    0x01,                           // bTerminalID 1
    UAC_SR_B0(UAC_TERMINAL_MICROPHONE), UAC_SR_B1(UAC_TERMINAL_MICROPHONE),
    0x00,                           // bAssocTerminal
    UAC_CHANNELS,                   // bNrChannels
    0x03, 0x00,                     // wChannelConfig: L+R
    0x00,                           // iChannelNames
    0x00,                           // iTerminal

    /* CS Interface: Output Terminal 2 — USB Streaming OUT (mic → Host PC capture)
     * This terminal drives EP4 ISO IN (device→host).
     * bTerminalLink in IF3 CS AS General points here (0x02). */
    0x09,                           // bLength
    USB_DESC_TYPE_CS_INTERFACE,     // bDescriptorType
    UAC_AC_OUTPUT_TERMINAL,         // bDescriptorSubtype
    0x02,                           // bTerminalID 2
    UAC_SR_B0(UAC_TERMINAL_USB_STREAMING), UAC_SR_B1(UAC_TERMINAL_USB_STREAMING),
    0x00,                           // bAssocTerminal
    0x01,                           // bSourceID (IT1 — Microphone)
    0x00,                           // iTerminal

    /* CS Interface: Input Terminal 3 — USB Streaming IN (Host PC playback → Target spk)
    * This terminal feeds EP2 ISO OUT (host→device).
     * bTerminalLink in IF4 CS AS General points here (0x03). */
    0x0C,                           // bLength
    USB_DESC_TYPE_CS_INTERFACE,     // bDescriptorType
    UAC_AC_INPUT_TERMINAL,          // bDescriptorSubtype
    0x03,                           // bTerminalID 3
    UAC_SR_B0(UAC_TERMINAL_USB_STREAMING), UAC_SR_B1(UAC_TERMINAL_USB_STREAMING),
    0x00,                           // bAssocTerminal
    UAC_CHANNELS,                   // bNrChannels
    0x03, 0x00,                     // wChannelConfig: L+R
    0x00,                           // iChannelNames
    0x00,                           // iTerminal

    /* CS Interface: Output Terminal 4 — Speaker (physical output on Target side) */
    0x09,                           // bLength
    USB_DESC_TYPE_CS_INTERFACE,     // bDescriptorType
    UAC_AC_OUTPUT_TERMINAL,         // bDescriptorSubtype
    0x04,                           // bTerminalID 4
    UAC_SR_B0(UAC_TERMINAL_SPEAKER), UAC_SR_B1(UAC_TERMINAL_SPEAKER),
    0x00,                           // bAssocTerminal
    0x03,                           // bSourceID (IT3 — USB Streaming IN)
    0x00,                           // iTerminal

    /* --- IF3 AudioStreaming IN — alt0, zero-bandwidth --- */
    0x09, 0x04, 0x03, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,

    /* --- IF3 AudioStreaming IN — alt1, active (EP4, 64 bytes/ms) --- */
    0x09, 0x04, 0x03, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,

    /* CS Interface: AS General — linked to OT2 (USB Streaming OUT = Capture path)
     * EP4 ISO IN sends mic audio from Target to Host PC. */
    0x07, USB_DESC_TYPE_CS_INTERFACE, UAC_AS_GENERAL,
    0x02,                           // bTerminalLink (OT2 — USB Streaming OUT)
    0x00,
    UAC_SR_B0(UAC_FORMAT_PCM), UAC_SR_B1(UAC_FORMAT_PCM),

    /* CS Interface: Format Type I — 64 B/ms = stereo 16kHz or mono 32kHz */
    0x0B, USB_DESC_TYPE_CS_INTERFACE, UAC_AS_FORMAT_TYPE,
    0x01,                           // bFormatType: Type I
    UAC_CHANNELS,                   // bNrChannels: 2
    UAC_BYTES_PER_SAMPLE,           // bSubFrameSize: 2
    UAC_BIT_RESOLUTION,             // bBitResolution: 16
    0x01,                           // bSamFreqType: 1 frequency
    UAC_SR_B0(UAC_USBD_SAMPLE_RATE), UAC_SR_B1(UAC_USBD_SAMPLE_RATE), UAC_SR_B2(UAC_USBD_SAMPLE_RATE),

    /* Endpoint: EP4 ISO IN — single buffer, 64 bytes */
    0x09, 0x05, UAC_USBD_EP_IN_ADDR, 0x01,
    0x40, 0x00,                     // wMaxPacketSize: 64
    0x01, 0x00, 0x00,               // bInterval=1ms, bRefresh=0, bSynchAddr=0

    /* CS Endpoint */
    0x07, USB_DESC_TYPE_CS_ENDPOINT, UAC_EP_GENERAL, 0x01, 0x00, 0x00, 0x00,

    /* ================================================================
     * IF4: AudioStreaming OUT — Target→Host (microphone capture)
     *   alt0 = zero-bandwidth (no EP)
    *   alt1 = active, EP2 ISO OUT 64 bytes/ms (16kHz stereo 16-bit)
     * ================================================================ */

    /* IF4 alt0 — zero-bandwidth */
    0x09, 0x04,                     // bLength, bDescriptorType (Interface)
    0x04,                           // bInterfaceNumber: 4
    0x00,                           // bAlternateSetting: 0
    0x00,                           // bNumEndpoints: 0
    0x01,                           // bInterfaceClass: Audio
    0x02,                           // bInterfaceSubClass: AudioStreaming
    0x00, 0x00,                     // bInterfaceProtocol, iInterface

    /* IF4 alt1 — active streaming */
    0x09, 0x04,                     // bLength, bDescriptorType (Interface)
    0x04,                           // bInterfaceNumber: 4
    0x01,                           // bAlternateSetting: 1
    0x01,                           // bNumEndpoints: 1
    0x01,                           // bInterfaceClass: Audio
    0x02,                           // bInterfaceSubClass: AudioStreaming
    0x00, 0x00,                     // bInterfaceProtocol, iInterface

    /* CS Interface: AS General — linked to IT3 (USB Streaming IN = Render path)
    * EP2 ISO OUT receives playback audio from Host PC to play on Target. */
    0x07,                           // bLength
    USB_DESC_TYPE_CS_INTERFACE,     // bDescriptorType
    UAC_AS_GENERAL,                 // bDescriptorSubtype
    0x03,                           // bTerminalLink: IT3 (USB Streaming IN → speaker)
    0x00,                           // bDelay
    UAC_SR_B0(UAC_FORMAT_PCM), UAC_SR_B1(UAC_FORMAT_PCM), // wFormatTag: PCM

    /* CS Interface: Format Type I — 16kHz stereo 16-bit */
    0x0B,                           // bLength
    USB_DESC_TYPE_CS_INTERFACE,     // bDescriptorType
    UAC_AS_FORMAT_TYPE,             // bDescriptorSubtype
    0x01,                           // bFormatType: Type I
    UAC_CHANNELS,                   // bNrChannels
    UAC_BYTES_PER_SAMPLE,           // bSubFrameSize
    UAC_BIT_RESOLUTION,             // bBitResolution
    0x01,                           // bSamFreqType: 1 frequency
    UAC_SR_B0(UAC_USBD_SAMPLE_RATE), UAC_SR_B1(UAC_USBD_SAMPLE_RATE), UAC_SR_B2(UAC_USBD_SAMPLE_RATE),

    /* Endpoint: EP2 ISO OUT — single buffer, 64 bytes/ms */
    0x09,                           // bLength
    0x05,                           // bDescriptorType (Endpoint)
    UAC_USBD_EP_OUT_ADDR,           // bEndpointAddress: EP2 OUT (0x02)
    0x09,                           // bmAttributes: Isochronous, Adaptive
    0x40, 0x00,                     // wMaxPacketSize: 64
    0x01, 0x00, 0x00,               // bInterval=1ms, bRefresh=0, bSynchAddr=0

    /* CS Endpoint */
    0x07, USB_DESC_TYPE_CS_ENDPOINT, UAC_EP_GENERAL, 0x01, 0x00, 0x00, 0x00,
#endif /* USB_WORK_MODE_HAS_HOST_UAC */

};

typedef char USBD_ConfigDescriptor_SizeCheck[
    (sizeof(USBD_ConfigDescriptor) == USBD_SIZE_CONFIG_DESC) ? 1 : -1
];

#if USB_WORK_MODE_RUNTIME_PROFILES
#define USBD_RUNTIME_ACM_CONFIG_DESC_SIZE 75u
#define USBD_RUNTIME_UAC_BLOCK_SIZE       (8u + UAC_USBD_BLOCK_LEN)
#define USBD_RUNTIME_UAC_CONFIG_DESC_SIZE (USBD_RUNTIME_ACM_CONFIG_DESC_SIZE + USBD_RUNTIME_UAC_BLOCK_SIZE)
static const uint8_t USBD_RuntimeAcmConfigDescriptor[USBD_RUNTIME_ACM_CONFIG_DESC_SIZE] = {
    0x09, 0x02,
    USBD_RUNTIME_ACM_CONFIG_DESC_SIZE & 0xFF, USBD_RUNTIME_ACM_CONFIG_DESC_SIZE >> 8,
    0x02, 0x01, 0x00, 0x80, 0x32,

    0x08, 0x0B, 0x00, 0x02, 0x02, 0x02, 0x01, 0x00,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,
    0x05, 0x24, 0x00, 0x10, 0x01,
    0x05, 0x24, 0x01, 0x00, 0x01,
    0x04, 0x24, 0x02, 0x02,
    0x05, 0x24, 0x06, 0x00, 0x01,
    0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x01,

    0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x05, 0x02, 0x10, 0x00, 0x00,
    0x07, 0x05, 0x83, 0x02, 0x10, 0x00, 0x00,
};

static const uint8_t USBD_RuntimeUacBlock[USBD_RUNTIME_UAC_BLOCK_SIZE] = {
    0x08, 0x0B, 0x02, 0x03, 0x01, 0x01, 0x00, 0x00,
    0x09, 0x04, 0x02, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x0A, USB_DESC_TYPE_CS_INTERFACE, UAC_AC_HEADER, 0x00, 0x01,
    0x34, 0x00, 0x02, 0x03, 0x04,
    0x0C, USB_DESC_TYPE_CS_INTERFACE, UAC_AC_INPUT_TERMINAL, 0x01,
    UAC_SR_B0(UAC_TERMINAL_MICROPHONE), UAC_SR_B1(UAC_TERMINAL_MICROPHONE),
    0x00, UAC_CHANNELS, 0x03, 0x00, 0x00, 0x00,
    0x09, USB_DESC_TYPE_CS_INTERFACE, UAC_AC_OUTPUT_TERMINAL, 0x02,
    UAC_SR_B0(UAC_TERMINAL_USB_STREAMING), UAC_SR_B1(UAC_TERMINAL_USB_STREAMING),
    0x00, 0x01, 0x00,
    0x0C, USB_DESC_TYPE_CS_INTERFACE, UAC_AC_INPUT_TERMINAL, 0x03,
    UAC_SR_B0(UAC_TERMINAL_USB_STREAMING), UAC_SR_B1(UAC_TERMINAL_USB_STREAMING),
    0x00, UAC_CHANNELS, 0x03, 0x00, 0x00, 0x00,
    0x09, USB_DESC_TYPE_CS_INTERFACE, UAC_AC_OUTPUT_TERMINAL, 0x04,
    UAC_SR_B0(UAC_TERMINAL_SPEAKER), UAC_SR_B1(UAC_TERMINAL_SPEAKER),
    0x00, 0x03, 0x00,
    0x09, 0x04, 0x03, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    0x09, 0x04, 0x03, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, USB_DESC_TYPE_CS_INTERFACE, UAC_AS_GENERAL, 0x02, 0x00,
    UAC_SR_B0(UAC_FORMAT_PCM), UAC_SR_B1(UAC_FORMAT_PCM),
    0x0B, USB_DESC_TYPE_CS_INTERFACE, UAC_AS_FORMAT_TYPE, 0x01,
    UAC_CHANNELS, UAC_BYTES_PER_SAMPLE, UAC_BIT_RESOLUTION, 0x01,
    UAC_SR_B0(UAC_USBD_SAMPLE_RATE), UAC_SR_B1(UAC_USBD_SAMPLE_RATE), UAC_SR_B2(UAC_USBD_SAMPLE_RATE),
    0x09, 0x05, UAC_USBD_EP_IN_ADDR, 0x01, 0x40, 0x00, 0x01, 0x00, 0x00,
    0x07, USB_DESC_TYPE_CS_ENDPOINT, UAC_EP_GENERAL, 0x01, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    0x09, 0x04, 0x04, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, USB_DESC_TYPE_CS_INTERFACE, UAC_AS_GENERAL, 0x03, 0x00,
    UAC_SR_B0(UAC_FORMAT_PCM), UAC_SR_B1(UAC_FORMAT_PCM),
    0x0B, USB_DESC_TYPE_CS_INTERFACE, UAC_AS_FORMAT_TYPE, 0x01,
    UAC_CHANNELS, UAC_BYTES_PER_SAMPLE, UAC_BIT_RESOLUTION, 0x01,
    UAC_SR_B0(UAC_USBD_SAMPLE_RATE), UAC_SR_B1(UAC_USBD_SAMPLE_RATE), UAC_SR_B2(UAC_USBD_SAMPLE_RATE),
    0x09, 0x05, UAC_USBD_EP_OUT_ADDR, 0x09, 0x40, 0x00, 0x01, 0x00, 0x00,
    0x07, USB_DESC_TYPE_CS_ENDPOINT, UAC_EP_GENERAL, 0x01, 0x00, 0x00, 0x00,
};

static uint8_t USBD_RuntimeUacConfigDescriptor[USBD_RUNTIME_UAC_CONFIG_DESC_SIZE];
static uint8_t USBD_RuntimeUacConfigBuilt;

typedef char USBD_RuntimeUacBlock_SizeCheck[(sizeof(USBD_RuntimeUacBlock) == USBD_RUNTIME_UAC_BLOCK_SIZE) ? 1 : -1];

static void USBD_BuildRuntimeUacConfigDescriptor(void)
{
    if (USBD_RuntimeUacConfigBuilt) {
        return;
    }
    memcpy(USBD_RuntimeUacConfigDescriptor, USBD_RuntimeAcmConfigDescriptor, USBD_RUNTIME_ACM_CONFIG_DESC_SIZE);
    memcpy(&USBD_RuntimeUacConfigDescriptor[USBD_RUNTIME_ACM_CONFIG_DESC_SIZE], USBD_RuntimeUacBlock, USBD_RUNTIME_UAC_BLOCK_SIZE);
    USBD_RuntimeUacConfigDescriptor[2] = (uint8_t)USBD_RUNTIME_UAC_CONFIG_DESC_SIZE;
    USBD_RuntimeUacConfigDescriptor[3] = (uint8_t)(USBD_RUNTIME_UAC_CONFIG_DESC_SIZE >> 8);
    USBD_RuntimeUacConfigDescriptor[4] = 0x05;
    USBD_RuntimeUacConfigBuilt = 1u;
}

/* ECM base descriptor (config header + ECM interfaces, no UAC) */
#define USBD_RUNTIME_ECM_CONFIG_DESC_SIZE 88u
static const uint8_t USBD_RuntimeEcmConfigDescriptor[USBD_RUNTIME_ECM_CONFIG_DESC_SIZE] = {
    0x09, 0x02,
    USBD_RUNTIME_ECM_CONFIG_DESC_SIZE & 0xFF, USBD_RUNTIME_ECM_CONFIG_DESC_SIZE >> 8,
    0x02, 0x01, 0x00, 0x80, 0x32,

    0x08, 0x0B, 0x00, 0x02, 0x02, 0x06, 0x00, 0x00,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x06, 0x00, 0x00,
    0x05, 0x24, 0x00, 0x10, 0x01,
    0x05, 0x24, 0x06, 0x00, 0x01,
    0x0D, 0x24, 0x0F, 0x04,
    0x00, 0x00, 0x00, 0x00,
    0xEA, 0x05,
    0x00, 0x00,
    0x00,
    0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x01,

    0x09, 0x04, 0x01, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x01, 0x01, 0x02, 0x0A, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x05, 0x02, 0x40, 0x00, 0x00,  /* EP5 bulk OUT: 64 bytes */
    0x07, 0x05, 0x83, 0x02, 0x40, 0x00, 0x00,  /* EP3 bulk IN: 64 bytes */
};

/* Combined ECM + UAC descriptor size */
#define USBD_RUNTIME_ECM_UAC_CONFIG_DESC_SIZE (USBD_RUNTIME_ECM_CONFIG_DESC_SIZE + USBD_RUNTIME_UAC_BLOCK_SIZE)
static uint8_t USBD_RuntimeEcmUacConfigDescriptor[USBD_RUNTIME_ECM_UAC_CONFIG_DESC_SIZE];
static uint8_t USBD_RuntimeEcmUacConfigBuilt;

static void USBD_BuildRuntimeEcmUacConfigDescriptor(void)
{
    if (USBD_RuntimeEcmUacConfigBuilt) {
        return;
    }
    memcpy(USBD_RuntimeEcmUacConfigDescriptor, USBD_RuntimeEcmConfigDescriptor, USBD_RUNTIME_ECM_CONFIG_DESC_SIZE);
    memcpy(&USBD_RuntimeEcmUacConfigDescriptor[USBD_RUNTIME_ECM_CONFIG_DESC_SIZE], USBD_RuntimeUacBlock, USBD_RUNTIME_UAC_BLOCK_SIZE);
    USBD_RuntimeEcmUacConfigDescriptor[2] = (uint8_t)USBD_RUNTIME_ECM_UAC_CONFIG_DESC_SIZE;
    USBD_RuntimeEcmUacConfigDescriptor[3] = (uint8_t)(USBD_RUNTIME_ECM_UAC_CONFIG_DESC_SIZE >> 8);
    USBD_RuntimeEcmUacConfigDescriptor[4] = 0x05;
    USBD_RuntimeEcmUacConfigBuilt = 1u;
}

/* ====== Combined ACM + ECM runtime descriptor ======
 * ACM uses: EP1 INT, EP5 BULK OUT, EP3 BULK IN  (IF0/IF1)
 * ECM uses: EP2 INT, EP6 BULK OUT, EP4 BULK IN  (IF2/IF3)
 * Total: 9(cfg) + 66(ACM) + 79(ECM) = 154B */
#define USBD_RUNTIME_COMBINED_ECM_BLOCK_SIZE 79u
#define USBD_RUNTIME_ACM_ECM_CONFIG_DESC_SIZE 154u

static const uint8_t USBD_RuntimeCombinedEcmBlock[USBD_RUNTIME_COMBINED_ECM_BLOCK_SIZE] = {
    /* IAD: CDC ECM (IF2=comm + IF3=data) */
    0x08, 0x0B, 0x02, 0x02, 0x02, 0x06, 0x00, 0x00,
    /* Interface 2: CDC ECM communication */
    0x09, 0x04, 0x02, 0x00, 0x01, 0x02, 0x06, 0x00, 0x00,
    /* Functional descriptors */
    0x05, 0x24, 0x00, 0x10, 0x01,
    0x05, 0x24, 0x06, 0x02, 0x03,   /* Union: control IF2, data IF3 */
    0x0D, 0x24, 0x0F, 0x04,
    0x00, 0x00, 0x00, 0x00,
    0xEA, 0x05,
    0x00, 0x00,
    0x00,
    /* EP2 INT IN (0x82) */
    0x07, 0x05, 0x82, 0x03, 0x10, 0x00, 0x01,
    /* Interface 3: CDC ECM data, alt 0 idle */
    0x09, 0x04, 0x03, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,
    /* Interface 3: CDC ECM data, alt 1 active */
    0x09, 0x04, 0x03, 0x01, 0x02, 0x0A, 0x00, 0x00, 0x00,
    /* EP6 OUT (0x06) bulk: 32 bytes (reduced to fit 512B PMA) */
    0x07, 0x05, 0x06, 0x02, 0x20, 0x00, 0x00,
    /* EP4 IN (0x84) bulk: 32 bytes (reduced to fit 512B PMA) */
    0x07, 0x05, 0x84, 0x02, 0x20, 0x00, 0x00,
};

typedef char USBD_RuntimeCombinedEcmBlock_SizeCheck[
    (sizeof(USBD_RuntimeCombinedEcmBlock) == USBD_RUNTIME_COMBINED_ECM_BLOCK_SIZE) ? 1 : -1
];

static uint8_t USBD_RuntimeAcmEcmConfigDescriptor[USBD_RUNTIME_ACM_ECM_CONFIG_DESC_SIZE];
static uint8_t USBD_RuntimeAcmEcmConfigBuilt;

static void USBD_BuildRuntimeAcmEcmConfigDescriptor(void)
{
    if (USBD_RuntimeAcmEcmConfigBuilt) {
        return;
    }
    /* Start with the ACM config descriptor (75B), then append ECM block (79B) */
    memcpy(USBD_RuntimeAcmEcmConfigDescriptor, USBD_RuntimeAcmConfigDescriptor, USBD_RUNTIME_ACM_CONFIG_DESC_SIZE);
    memcpy(&USBD_RuntimeAcmEcmConfigDescriptor[USBD_RUNTIME_ACM_CONFIG_DESC_SIZE],
           USBD_RuntimeCombinedEcmBlock, USBD_RUNTIME_COMBINED_ECM_BLOCK_SIZE);
    /* Patch config header: wTotalLength and bNumInterfaces */
    USBD_RuntimeAcmEcmConfigDescriptor[2] = (uint8_t)USBD_RUNTIME_ACM_ECM_CONFIG_DESC_SIZE;
    USBD_RuntimeAcmEcmConfigDescriptor[3] = (uint8_t)(USBD_RUNTIME_ACM_ECM_CONFIG_DESC_SIZE >> 8);
    USBD_RuntimeAcmEcmConfigDescriptor[4] = 0x04;  /* bNumInterfaces: ACM(2) + ECM(2) */

    /* Fix EP3 and EP5 wMaxPacketSize from 16 to 64 bytes for CH9329 responses */
    /* EP5 OUT endpoint descriptor is at offset 61-67 */
    /* wMaxPacketSize is at offset 65-66 (little-endian: low byte first) */
    USBD_RuntimeAcmEcmConfigDescriptor[65] = 0x40;  /* Low byte: 64 */
    USBD_RuntimeAcmEcmConfigDescriptor[66] = 0x00;  /* High byte: 0 */

    /* EP3 IN endpoint descriptor is at offset 68-74 */
    /* wMaxPacketSize is at offset 72-73 (little-endian: low byte first) */
    USBD_RuntimeAcmEcmConfigDescriptor[72] = 0x40;  /* Low byte: 64 */
    USBD_RuntimeAcmEcmConfigDescriptor[73] = 0x00;  /* High byte: 0 */

    USBD_RuntimeAcmEcmConfigBuilt = 1u;
}

/* ====== Target ECM runtime block (for mode 7: ACM+Host ECM+Target ECM) ======
 * Target ECM uses: EP7 OUT (0x07) + EP7 IN (0x87)  (IF4=comm + IF5=data)
 * Note: EP7 supports both IN and OUT directions. CH32V20x only has EP0-EP7 (no EP8).
 * Size: 8(IAD) + 9(IF4) + 23(func) + 9(IF5 alt0) + 9(IF5 alt1) + 7(EP7 OUT) + 7(EP7 IN) = 72B */
#define USBD_RUNTIME_TARGET_ECM_BLOCK_SIZE 72u
#define USBD_RUNTIME_ACM_ECM_BRIDGE_CONFIG_DESC_SIZE 226u

static const uint8_t USBD_RuntimeTargetEcmBlock[USBD_RUNTIME_TARGET_ECM_BLOCK_SIZE] = {
    /* IAD: Target CDC ECM (IF4=comm + IF5=data) */
    0x08, 0x0B, 0x04, 0x02, 0x02, 0x06, 0x00, 0x00,
    /* Interface 4: Target CDC ECM communication */
    0x09, 0x04, 0x04, 0x00, 0x00, 0x02, 0x06, 0x00, 0x00,
    /* Functional descriptors */
    0x05, 0x24, 0x00, 0x10, 0x01,
    0x05, 0x24, 0x06, 0x04, 0x05,   /* Union: control IF4, data IF5 */
    0x0D, 0x24, 0x0F, 0x05,         /* Ethernet Networking, iMACAddress=5 */
    0x00, 0x00, 0x00, 0x00,
    0xEA, 0x05,
    0x00, 0x00,
    0x00,
    /* Interface 5: Target CDC ECM data, alt 0 idle */
    0x09, 0x04, 0x05, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,
    /* Interface 5: Target CDC ECM data, alt 1 active */
    0x09, 0x04, 0x05, 0x01, 0x02, 0x0A, 0x00, 0x00, 0x00,
    /* EP7 OUT (0x07) bulk: 32 bytes (reduced to fit 512B PMA) */
    0x07, 0x05, 0x07, 0x02, 0x20, 0x00, 0x00,
    /* EP7 IN (0x87) bulk: 32 bytes (reduced to fit 512B PMA) */
    0x07, 0x05, 0x87, 0x02, 0x20, 0x00, 0x00,
};

typedef char USBD_RuntimeTargetEcmBlock_SizeCheck[
    (sizeof(USBD_RuntimeTargetEcmBlock) == USBD_RUNTIME_TARGET_ECM_BLOCK_SIZE) ? 1 : -1
];

static uint8_t USBD_RuntimeAcmEcmBridgeConfigDescriptor[USBD_RUNTIME_ACM_ECM_BRIDGE_CONFIG_DESC_SIZE];
static uint8_t USBD_RuntimeAcmEcmBridgeConfigBuilt;

static void USBD_BuildRuntimeAcmEcmBridgeConfigDescriptor(void)
{
    if (USBD_RuntimeAcmEcmBridgeConfigBuilt) {
        return;
    }
    /* Start with the ACM+ECM config descriptor (154B), then append Target ECM block (72B) */
    USBD_BuildRuntimeAcmEcmConfigDescriptor();
    memcpy(USBD_RuntimeAcmEcmBridgeConfigDescriptor, USBD_RuntimeAcmEcmConfigDescriptor, USBD_RUNTIME_ACM_ECM_CONFIG_DESC_SIZE);
    memcpy(&USBD_RuntimeAcmEcmBridgeConfigDescriptor[USBD_RUNTIME_ACM_ECM_CONFIG_DESC_SIZE],
           USBD_RuntimeTargetEcmBlock, USBD_RUNTIME_TARGET_ECM_BLOCK_SIZE);
    /* Patch config header: wTotalLength and bNumInterfaces */
    USBD_RuntimeAcmEcmBridgeConfigDescriptor[2] = (uint8_t)USBD_RUNTIME_ACM_ECM_BRIDGE_CONFIG_DESC_SIZE;
    USBD_RuntimeAcmEcmBridgeConfigDescriptor[3] = (uint8_t)(USBD_RUNTIME_ACM_ECM_BRIDGE_CONFIG_DESC_SIZE >> 8);
    USBD_RuntimeAcmEcmBridgeConfigDescriptor[4] = 0x06;  /* bNumInterfaces: ACM(2) + Host ECM(2) + Target ECM(2) */

    USBD_RuntimeAcmEcmBridgeConfigBuilt = 1u;
}
#endif

const uint8_t *USBD_GetActiveConfigDescriptorData(uint16_t *len)
{
#if USB_WORK_MODE_RUNTIME_PROFILES
    if (USB_ModeConfig_ActiveModeHasHostCdcAcm() && USB_ModeConfig_ActiveModeHasHostCdcEcm() && USB_ModeConfig_ActiveModeHasTargetCdcEcm()) {
        USBD_BuildRuntimeAcmEcmBridgeConfigDescriptor();
        if (len != 0) {
            *len = USBD_RUNTIME_ACM_ECM_BRIDGE_CONFIG_DESC_SIZE;
        }
        return USBD_RuntimeAcmEcmBridgeConfigDescriptor;
    }
    if (USB_ModeConfig_ActiveModeHasHostCdcAcm() && USB_ModeConfig_ActiveModeHasHostCdcEcm()) {
        USBD_BuildRuntimeAcmEcmConfigDescriptor();
        if (len != 0) {
            *len = USBD_RUNTIME_ACM_ECM_CONFIG_DESC_SIZE;
        }
        return USBD_RuntimeAcmEcmConfigDescriptor;
    }
    if (USB_ModeConfig_ActiveModeHasHostCdcEcm() && USB_ModeConfig_ActiveModeHasHostUac()) {
        USBD_BuildRuntimeEcmUacConfigDescriptor();
        if (len != 0) {
            *len = USBD_RUNTIME_ECM_UAC_CONFIG_DESC_SIZE;
        }
        return USBD_RuntimeEcmUacConfigDescriptor;
    }
    if (USB_ModeConfig_ActiveModeHasHostCdcEcm()) {
        if (len != 0) {
            *len = USBD_RUNTIME_ECM_CONFIG_DESC_SIZE;
        }
        return USBD_RuntimeEcmConfigDescriptor;
    }
    if (USB_ModeConfig_ActiveModeHasHostUac()) {
        USBD_BuildRuntimeUacConfigDescriptor();
        if (len != 0) {
            *len = USBD_RUNTIME_UAC_CONFIG_DESC_SIZE;
        }
        return USBD_RuntimeUacConfigDescriptor;
    }
    if (len != 0) {
        *len = USBD_RUNTIME_ACM_CONFIG_DESC_SIZE;
    }
    return USBD_RuntimeAcmConfigDescriptor;
#else
    if (len != 0) {
        *len = USBD_SIZE_CONFIG_DESC;
    }
    return USBD_ConfigDescriptor;
#endif
}

uint8_t USBD_GetActiveConfigAttributes(void)
{
    return USBD_GetActiveConfigDescriptorData(0)[7];
}


/* USB String Descriptors */
const uint8_t USBD_StringLangID[USBD_SIZE_STRING_LANGID] = {
	USBD_SIZE_STRING_LANGID,
	USB_STRING_DESCRIPTOR_TYPE,
	0x09,
	0x04 
};

/* USB Device String Vendor */
const uint8_t USBD_StringVendor[USBD_SIZE_STRING_VENDOR] = {
	USBD_SIZE_STRING_VENDOR,    
	USB_STRING_DESCRIPTOR_TYPE,           
	'w',0,'c',0,'h',0,'.',0,'c',0,'n',0
};

/* USB Device String Product */
const uint8_t USBD_StringProduct[USBD_SIZE_STRING_PRODUCT] = {
    USBD_SIZE_STRING_PRODUCT,
    USB_STRING_DESCRIPTOR_TYPE,
    'O', 0, 'p', 0, 'e', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
    ' ', 0, 'H', 0, 'o', 0, 's', 0, 't', 0};

/* USB Device String Serial */
uint8_t USBD_StringSerial[USBD_SIZE_STRING_SERIAL] = {
	USBD_SIZE_STRING_SERIAL,          
	USB_STRING_DESCRIPTOR_TYPE,                   
	'0', 0, '1', 0, '2', 0, '3', 0, '4', 0, '5', 0 , '6', 0, '7', 0, '8', 0, '9', 0
};

const uint8_t USBD_StringAudioFunction[USBD_SIZE_STRING_AUDIO_FUNC] = {
    USBD_SIZE_STRING_AUDIO_FUNC,
    USB_STRING_DESCRIPTOR_TYPE,
    'O', 0, 'p', 0, 'e', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
    ' ', 0, 'H', 0, 'o', 0, 's', 0, 't', 0
};

const uint8_t USBD_StringSpeaker[USBD_SIZE_STRING_SPEAKER] = {
    USBD_SIZE_STRING_SPEAKER,
    USB_STRING_DESCRIPTOR_TYPE,
    'O', 0, 'P', 0, 'F', 0, ' ', 0,
    'H', 0, 'O', 0, 'S', 0, 'T', 0, ' ', 0,
    'A', 0, '-', 0, 'O', 0, 'U', 0, 'T', 0
};

const uint8_t USBD_StringMicrophone[USBD_SIZE_STRING_MICROPHONE] = {
    USBD_SIZE_STRING_MICROPHONE,
    USB_STRING_DESCRIPTOR_TYPE,
    'O', 0, 'P', 0, 'F', 0, ' ', 0,
    'H', 0, 'O', 0, 'S', 0, 'T', 0, ' ', 0,
    'A', 0, '-', 0, 'I', 0, 'N', 0
};

const uint8_t USBD_StringEcmMac[USBD_SIZE_STRING_ECM_MAC] = {
    USBD_SIZE_STRING_ECM_MAC,
    USB_STRING_DESCRIPTOR_TYPE,
    '0', 0, '2', 0, '0', 0, '0', 0, '0', 0, '0', 0,
    '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '2', 0
};

/* Target ECM MAC string descriptor (different from Host ECM MAC) */
const uint8_t USBD_StringTargetEcmMac[USBD_SIZE_STRING_TARGET_ECM_MAC] = {
    USBD_SIZE_STRING_TARGET_ECM_MAC,
    USB_STRING_DESCRIPTOR_TYPE,
    '0', 0, '2', 0, '0', 0, '0', 0, '0', 0, '0', 0,
    '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '3', 0
};


