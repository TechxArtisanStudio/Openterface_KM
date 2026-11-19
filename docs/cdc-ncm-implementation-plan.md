# CDC-NCM Implementation Plan for Windows Support

## Executive Summary

This document outlines the implementation plan for adding **CDC-NCM (Network Control Model)** support to the Openterface KM firmware. The primary goal is to enable native Windows 10/11 support without requiring RNDIS drivers or custom INF files.

**Current State**: CDC-ECM (Ethernet Control Model) on CH32V20x MCU
**Target State**: CDC-NCM with backward-compatible ECM option

---

## 1. Background

### 1.1 Why CDC-NCM?

| Aspect | CDC-ECM (Current) | CDC-NCM (Target) |
|--------|-------------------|------------------|
| **Windows 10/11 Driver** | RNDIS fallback or `usbccm.sys` | `usbncm.sys` (native) |
| **Driver Installation** | May need manual INF | Automatic, no INF needed |
| **Performance** | Per-packet overhead | NTB datagram batching |
| **Linux Support** | `cdc_ether` module | `cdc_ncm` module |
| **macOS Support** | Native | Native (10.14+) |
| **Complexity** | Simpler framing | NTB header overhead |

### 1.2 Windows CDC-NCM Support

- **Windows 10 1903+**: Native `usbncm.sys` driver
- **Windows 11**: Full native support
- **Class Code**: `02/0D/00` (Communications / Network Control Model)
- **No INF Required**: Standard compliant devices enumerate automatically

### 1.3 CH32V20x Constraints

| Resource | Current Usage | NCM Requirement |
|----------|---------------|-----------------|
| Endpoints | 8 (EP0-EP7) | Same (notification + bulk IN/OUT) |
| PMA Memory | ~512 bytes | Same or less (batching helps) |
| Flash | ~128KB used | +4-8KB for NCM code |
| RAM | Limited | +2-4KB for NTB buffers |

---

## 2. CDC-NCM Protocol Overview

### 2.1 Key Differences from CDC-ECM

```
CDC-ECM: One Ethernet frame per USB transfer
         [Ethernet Frame]

CDC-NCM: Multiple datagrams in one NTB (Network Transfer Block)
         [NTB Header][NDP][Frame1][Frame2]...[FrameN]
```

### 2.2 NTB Structure (Network Transfer Block)

```
NTB Header (14 bytes):
┌─────────────────────────────────────────────────────────┐
│ wBlockLength    (4 bytes) - Total NTB length            │
│ wSequence       (2 bytes) - Sequence number             │
│ wNdpIndex       (2 bytes) - Offset to NDP               │
│ wNdpIndexAlt    (2 bytes) - Offset to alternate NDP     │
│ Reserved        (4 bytes) - Must be zero                │
└─────────────────────────────────────────────────────────┘

NDP (Network Datagram Pointer) - 16 bytes minimum:
┌─────────────────────────────────────────────────────────┐
│ dwSignature     (4 bytes) - 'NCM0' or 'NCMH'          │
│ wLength         (2 bytes) - NDP length                  │
│ wNextNdpIndex   (2 bytes) - Next NDP offset (0=last)    │
│ Datagram[0]:                                            │
│   wDatagramIndex  (2 bytes) - Frame offset in NTB       │
│   wDatagramLength (2 bytes) - Frame length              │
│ Datagram[1]: ...                                        │
│ Terminator: 0x0000, 0x0000                              │
└─────────────────────────────────────────────────────────┘
```

### 2.3 NCM Functional Descriptor (Additional to ECM)

**IMPORTANT**: The NCM Functional Descriptor uses subtype `0x1A` (USB_CDC_NCM_TYPE), **NOT** `0x0D`!

The Linux kernel's `cdc_ncm` driver specifically looks for subtype `0x1A` when binding. Using `0x0D` will cause:
```
cdc_ncm: NCM or ECM functional descriptors missing
```

```c
// NCM Functional Descriptor (8 bytes)
0x08,              // bLength
0x24,              // bDescriptorType (CS_INTERFACE)
0x1A,              // bDescriptorSubtype (USB_CDC_NCM_TYPE) ← MUST BE 0x1A!
0x00,              // bmCapabilities
0x00, 0x05,        // wNtbInMaxSize (1280 bytes typical)
0x10, 0x00,        // wNdpInMaxSize (16 bytes for single NDP)
```

**Note**: The original CDC NCM 1.0 specification uses subtype `0x0D` for the NCM Functional Descriptor, but the Linux kernel implementation expects `0x1A`. This is a known discrepancy that must be accounted for when implementing NCM for Linux compatibility.

---

## 3. Implementation Strategy

### 3.1 Approach: Runtime Mode Selection

Follow the existing `USB_WORK_MODE` pattern:

```c
// New mode in usb_work_mode.h
#define USB_WORK_MODE_HID_NCM_BRIDGE  8  /* Host CDC-NCM bridge */
```

This allows:
- Compile-time selection via `USB_WORK_MODE` define
- Runtime switching via CH9329 command (if `USB_WORK_MODE_RUNTIME_PROFILES=1`)
- Coexistence with ECM modes

### 3.2 File Changes Overview

| File | Change Type | Description |
|------|-------------|-------------|
| `APP/include/usb_work_mode.h` | Modify | Add NCM mode definitions |
| `APP/include/usb_mode_config.h` | Modify | Add NCM capability queries |
| `APP/usb_mode_config.c` | Modify | Implement NCM mode support |
| `APP/usbd_desc.c` | Modify | Add NCM descriptors |
| `APP/ncm_bridge.c` | **New** | NCM protocol handler (copy from ecm_bridge.c) |
| `APP/include/ncm_bridge.h` | **New** | NCM bridge header |
| `APP/ch32v20x_usbfs_device.c` | Modify | Add NCM class request handling |
| `APP/USBLIB/CONFIG/usb_prop.c` | Modify | Add USBD NCM class requests |
| `APP/USBLIB/CONFIG/usb_endp.c` | Modify | Add NCM endpoint handling |

---

## 4. Detailed Implementation Steps

### Phase 1: Descriptor Changes

#### 4.1 Device Descriptor

No change needed. Device class remains `0xEF/0x02/0x01` (IAD for composite).

#### 4.2 Configuration Descriptor

Replace ECM subclass with NCM:

```c
// Current ECM IAD (usbd_desc.c ~line 260)
0x08,                    // bLength
0x0B,                    // bDescriptorType (IAD)
0x04,                    // bFirstInterface
0x02,                    // bInterfaceCount
0x02,                    // bFunctionClass: Communications
0x06,                    // bFunctionSubClass: ECM  ← CHANGE
0x00,                    // bFunctionProtocol
0x00,                    // iFunction

// New NCM IAD
0x08,                    // bLength
0x0B,                    // bDescriptorType (IAD)
0x04,                    // bFirstInterface
0x02,                    // bInterfaceCount
0x02,                    // bFunctionClass: Communications
0x0D,                    // bFunctionSubClass: NCM  ← CHANGED
0x00,                    // bFunctionProtocol
0x00,                    // iFunction
```

#### 4.3 Communication Interface Descriptor

```c
// Current ECM Communication Interface
0x09,                    // bLength
0x04,                    // bDescriptorType (Interface)
0x04,                    // bInterfaceNumber
0x00,                    // bAlternateSetting
0x01,                    // bNumEndpoints (notification)
0x02,                    // bInterfaceClass: Communications
0x06,                    // bInterfaceSubClass: ECM  ← CHANGE
0x00,                    // bInterfaceProtocol
0x00,                    // iInterface

// New NCM Communication Interface
0x09,                    // bLength
0x04,                    // bDescriptorType (Interface)
0x04,                    // bInterfaceNumber
0x00,                    // bAlternateSetting
0x01,                    // bNumEndpoints (notification)
0x02,                    // bInterfaceClass: Communications
0x0D,                    // bInterfaceSubClass: NCM  ← CHANGED
0x00,                    // bInterfaceProtocol
0x00,                    // iInterface
```

#### 4.4 Functional Descriptors

NCM requires additional functional descriptor. **IMPORTANT**: The NCM Functional Descriptor must use subtype `0x1A` (USB_CDC_NCM_TYPE) for Linux kernel compatibility, not `0x0D`.

```c
// CDC Functional Descriptors for NCM
0x05, 0x24, 0x00, 0x10, 0x01,        // Header (CDC 1.10)
0x05, 0x24, 0x06, 0x04, 0x05,        // Union: control IF4, data IF5
0x0D, 0x24, 0x0F, 0x06,              // Ethernet Networking (iMACAddress=6)
    0x00, 0x00, 0x00, 0x00,          // bmEthernetStatistics
    0xEA, 0x05,                      // wMaxSegmentSize: 1514
    0x00, 0x00,                      // wNumberMCFilters
    0x00,                            // bNumberPowerFilters
// NEW: NCM Functional Descriptor (8 bytes, subtype 0x1A!)
0x08, 0x24, 0x1A,                    // NCM Functional (subtype 0x1A = USB_CDC_NCM_TYPE)
    0x00,                            // bmCapabilities (no filtering)
    0x00, 0x05,                      // wNtbInMaxSize: 1280 bytes
    0x10, 0x00,                      // wNdpInMaxSize: 16 bytes
```

**Note**: The iMACAddress field (0x06) references a string descriptor containing the MAC address in ASCII hex format.

#### 4.5 Notification Endpoint

Same as ECM (Interrupt IN):

```c
// Notification Endpoint (unchanged)
0x07,                    // bLength
0x05,                    // bDescriptorType (Endpoint)
0x86,                    // bEndpointAddress: EP6 IN
0x03,                    // bmAttributes: Interrupt
0x10, 0x00,              // wMaxPacketSize: 16 bytes
0x10,                    // bInterval: 16ms
```

#### 4.5.1 MAC Address String Descriptor

The Linux kernel requires a MAC address to be provided. This can be done via:
1. **String descriptor** (recommended): Set iMACAddress field in Ethernet Networking descriptor to a valid string index
2. **GET_NET_ADDRESS request** (0x81): Return 6-byte binary MAC address

String descriptor format (ASCII hex, no separators):
```c
// MAC address: 02:00:00:00:00:01
const uint8_t MyNcmMacInfo[] = {
    0x1A, 0x03,  // bLength (26), bDescriptorType (String)
    '0', 0, '2', 0, '0', 0, '0', 0, '0', 0, '0', 0,
    '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '1', 0
};
```

#### 4.6 Data Interface

Same structure as ECM (alt 0 = idle, alt 1 = active):

```c
// Data Interface Alt 0 (idle - zero bandwidth)
0x09, 0x04, 0x05, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,

// Data Interface Alt 1 (active - bulk endpoints)
0x09, 0x04, 0x05, 0x01, 0x02, 0x0A, 0x00, 0x00, 0x00,

// Bulk OUT Endpoint (EP5)
0x07, 0x05, 0x05, 0x02, 0x40, 0x00, 0x00,  // EP5 OUT, 64 bytes

// Bulk IN Endpoint (EP7)
0x07, 0x05, 0x87, 0x02, 0x40, 0x00, 0x00,  // EP7 IN, 64 bytes
```

**Note**: The endpoint numbers (EP5, EP6, EP7) depend on the overall USB configuration. For Mode 8 (HID+NCM bridge), the mapping is:
- EP6 IN: Notification endpoint (interrupt)
- EP5 OUT: NTB data from host (bulk)
- EP7 IN: NTB data to host (bulk)

### Phase 2: NCM Protocol Implementation

#### 4.7 NTB Structure Definition

```c
// APP/include/ncm_bridge.h

#define NCM_NTB_MAX_SIZE        1280    // Match descriptor
#define NCM_NDP_MAX_DATAGRAMS   4       // Max frames per NTB
#define NCM_NTB_HEADER_SIZE     14
#define NCM_NDP_HEADER_SIZE     8       // Signature + length + next
#define NCM_NDP_ENTRY_SIZE      4       // Index + length per datagram

typedef struct {
    uint8_t data[NCM_NTB_MAX_SIZE];
    uint16_t len;
    uint16_t sequence;
} NCM_Ntb_t;

typedef struct {
    uint16_t datagram_index[NCM_NDP_MAX_DATAGRAMS];
    uint16_t datagram_length[NCM_NDP_MAX_DATAGRAMS];
    uint8_t count;
} NCM_Ndp_t;
```

#### 4.8 NTB TX Builder

```c
// APP/ncm_bridge.c

static uint16_t ncm_build_ntb(uint8_t *ntb_buf, uint16_t max_size,
                               const uint8_t **frames, const uint16_t *frame_lens,
                               uint8_t frame_count, uint16_t sequence)
{
    uint16_t offset = NCM_NTB_HEADER_SIZE;
    uint16_t ndp_offset;
    uint8_t i;

    // Reserve space for NDP (will fill in later)
    ndp_offset = offset;
    offset += NCM_NDP_HEADER_SIZE + (frame_count + 1) * NCM_NDP_ENTRY_SIZE;

    // Copy datagrams
    for (i = 0; i < frame_count; i++) {
        uint16_t frame_len = frame_lens[i];
        if (offset + frame_len > max_size) break;

        // Record datagram location in NDP
        // (will be written to NTB below)

        // Copy frame data
        memcpy(&ntb_buf[offset], frames[i], frame_len);

        // Pad to 4-byte boundary (NCM requirement)
        uint16_t padded_len = (frame_len + 3) & ~3;
        offset += padded_len;
    }

    // Fill NTB header
    ntb_buf[0] = (uint8_t)(offset);           // wBlockLength low
    ntb_buf[1] = (uint8_t)(offset >> 8);      // wBlockLength high
    ntb_buf[2] = (uint8_t)(sequence);         // wSequence low
    ntb_buf[3] = (uint8_t)(sequence >> 8);    // wSequence high
    ntb_buf[4] = (uint8_t)(ndp_offset);       // wNdpIndex low
    ntb_buf[5] = (uint8_t)(ndp_offset >> 8);  // wNdpIndex high
    ntb_buf[6] = 0; ntb_buf[7] = 0;           // wNdpIndexAlt = 0

    // Fill NDP
    ntb_buf[ndp_offset + 0] = 'N';            // dwSignature
    ntb_buf[ndp_offset + 1] = 'C';
    ntb_buf[ndp_offset + 2] = 'M';
    ntb_buf[ndp_offset + 3] = '0';
    ntb_buf[ndp_offset + 4] = (uint8_t)(NCM_NDP_HEADER_SIZE + (frame_count + 1) * NCM_NDP_ENTRY_SIZE);
    ntb_buf[ndp_offset + 5] = 0;
    ntb_buf[ndp_offset + 6] = 0;              // wNextNdpIndex = 0 (last)
    ntb_buf[ndp_offset + 7] = 0;

    // Fill NDP entries
    uint16_t entry_offset = ndp_offset + NCM_NDP_HEADER_SIZE;
    uint16_t data_offset = ndp_offset + NCM_NDP_HEADER_SIZE + (frame_count + 1) * NCM_NDP_ENTRY_SIZE;

    for (i = 0; i < frame_count; i++) {
        ntb_buf[entry_offset + 0] = (uint8_t)(data_offset);
        ntb_buf[entry_offset + 1] = (uint8_t)(data_offset >> 8);
        ntb_buf[entry_offset + 2] = (uint8_t)(frame_lens[i]);
        ntb_buf[entry_offset + 3] = (uint8_t)(frame_lens[i] >> 8);
        entry_offset += NCM_NDP_ENTRY_SIZE;
        data_offset += (frame_lens[i] + 3) & ~3;
    }

    // Terminator
    ntb_buf[entry_offset + 0] = 0;
    ntb_buf[entry_offset + 1] = 0;
    ntb_buf[entry_offset + 2] = 0;
    ntb_buf[entry_offset + 3] = 0;

    return offset;
}
```

#### 4.9 NTB RX Parser

```c
// APP/ncm_bridge.c

static uint8_t ncm_parse_ntb(const uint8_t *ntb_buf, uint16_t ntb_len,
                              uint8_t (*frame_callback)(const uint8_t *frame, uint16_t len))
{
    uint16_t block_len, ndp_index;
    uint8_t datagram_count = 0;

    if (ntb_len < NCM_NTB_HEADER_SIZE) return 0;

    // Parse NTB header
    block_len = ntb_buf[0] | ((uint16_t)ntb_buf[1] << 8);
    ndp_index = ntb_buf[4] | ((uint16_t)ntb_buf[5] << 8);

    if (block_len > ntb_len) return 0;
    if (ndp_index >= ntb_len) return 0;

    // Verify NDP signature
    if (ntb_buf[ndp_index] != 'N' || ntb_buf[ndp_index + 1] != 'C' ||
        ntb_buf[ndp_index + 2] != 'M' || ntb_buf[ndp_index + 3] != '0') {
        return 0;
    }

    // Parse NDP entries
    uint16_t entry_offset = ndp_index + NCM_NDP_HEADER_SIZE;

    while (entry_offset + 4 <= ntb_len) {
        uint16_t dg_index = ntb_buf[entry_offset] | ((uint16_t)ntb_buf[entry_offset + 1] << 8);
        uint16_t dg_length = ntb_buf[entry_offset + 2] | ((uint16_t)ntb_buf[entry_offset + 3] << 8);

        entry_offset += NCM_NDP_ENTRY_SIZE;

        // Terminator
        if (dg_index == 0 && dg_length == 0) break;

        // Validate datagram bounds
        if (dg_index + dg_length > ntb_len) continue;

        // Process frame
        if (frame_callback) {
            frame_callback(&ntb_buf[dg_index], dg_length);
        }
        datagram_count++;
    }

    return datagram_count;
}
```

### Phase 3: USB Class Request Handling

#### 4.10 NCM-Specific Class Requests

NCM defines additional class requests that must be handled. The Linux kernel requires at minimum `GET_NTB_PARAMETERS` during the bind phase.

```c
// APP/ch32v20x_usbfs_device.c

// NCM Class Request Codes (CDC NCM 1.0 Table 6.2)
#define USBFS_CDC_NCM_REQ_GET_NTB_PARAMS  0x80u  // MANDATORY - kernel requires this
#define USBFS_CDC_NCM_REQ_GET_NET_ADDR    0x81u  // Optional if string descriptor used
#define USBFS_CDC_NCM_REQ_SET_NET_ADDR    0x82u
#define USBFS_CDC_NCM_REQ_GET_NTB_FMT     0x83u
#define USBFS_CDC_NCM_REQ_SET_NTB_FMT     0x84u
#define USBFS_CDC_NCM_REQ_GET_NTB_IN_SZ   0x85u
#define USBFS_CDC_NCM_REQ_SET_NTB_IN_SZ   0x86u
#define USBFS_CDC_NCM_REQ_GET_MAX_DGRAM   0x87u
#define USBFS_CDC_NCM_REQ_SET_MAX_DGRAM   0x88u
#define USBFS_CDC_NCM_REQ_GET_CRC_MODE    0x89u
#define USBFS_CDC_NCM_REQ_SET_CRC_MODE    0x8Au

// GET_NTB_PARAMETERS handler (returns 28-byte NTB Parameters structure)
case USBFS_CDC_NCM_REQ_GET_NTB_PARAMS:
    if (USB_ModeConfig_ActiveModeBridgeNcm() &&
        ((USBFS_SetupReqType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_INTERF) &&
        ((uint8_t)(USBFS_SetupReqIndex & 0xFF) == USBFS_CDC_NCM_IF_COMM))
    {
        memcpy(USBFS_EP0_Buf, USBFS_CDC_NCM_NtbParams, sizeof(USBFS_CDC_NCM_NtbParams));
        len = USBFS_SetupReqLen;
    }
    break;
```

NTB Parameters structure (28 bytes per CDC NCM 1.0 spec Table 6.3):
```c
static uint8_t USBFS_CDC_NCM_NtbParams[28] = {
    0x1C, 0x00,  /* wLength: 28 bytes */
    0x01, 0x00,  /* bmNtbFormatsSupported: 16-bit NTB only */
    0x00, 0x05, 0x00, 0x00,  /* dwNtbInMaxSize: 1280 bytes (little-endian) */
    0x04, 0x00,  /* wNdpInDivisor: 4 */
    0x00, 0x00,  /* wNdpInPayloadRemainder: 0 */
    0x10, 0x00,  /* wNdpInMaxSize: 16 bytes */
    0x00, 0x05, 0x00, 0x00,  /* dwNtbOutMaxSize: 1280 bytes (little-endian) */
    0x04, 0x00,  /* wNdpOutDivisor: 4 */
    0x00, 0x00,  /* wNdpOutPayloadRemainder: 0 */
    0x10, 0x00,  /* wNdpOutMaxSize: 16 bytes */
};
```

#### 4.11 SET_PACKET_FILTER Handling

NCM uses `SET_PACKET_FILTER` instead of ECM's multicast filtering:

```c
#define NCM_SET_PACKET_FILTER    0x43

// In class request handler:
case NCM_SET_PACKET_FILTER:
    // value = filter flags
    // 0x0001 = DIRECTED (unicast to our MAC)
    // 0x0002 = MULTICAST
    // 0x0004 = BROADCAST
    // 0x0008 = PROMISCUOUS
    // For simplicity, accept all and filter in software
    break;
```

### Phase 4: Integration with Existing Infrastructure

#### 4.12 Bridge API Compatibility

The NCM bridge should expose the same API as ECM:

```c
// APP/include/ncm_bridge.h

#if USB_WORK_MODE_COMPILES_BRIDGE_NCM
void NCM_Bridge_Init(void);
void NCM_Bridge_InitTarget(void);
void NCM_Bridge_UpdateIPConfig(void);
void NCM_Bridge_HostRxDone(const uint8_t *data, uint16_t len);
void NCM_Bridge_TargetRxDone(const uint8_t *data, uint16_t len);
void NCM_Bridge_Poll(void);
void NCM_Bridge_SetBleTxCallback(void (*cb)(void));
uint8_t NCM_Bridge_SendRawFrame(const uint8_t *data, uint16_t len);
void NCM_Bridge_Target_RearmLinkNotify(void);
#else
// Empty macros for non-NCM builds
#endif
```

#### 4.13 Notification Endpoint

The notification mechanism is **identical** to ECM:

```c
// NETWORK_CONNECTION notification (same as ECM)
packet[0] = 0xA1;           // bmRequestType: device-to-host, class, interface
packet[1] = 0x00;           // bNotificationType: NETWORK_CONNECTION
put_u16(&packet[2], 1);     // wValue: 1 = connected
put_u16(&packet[4], if_num);// wIndex: interface number
put_u16(&packet[6], 0);     // wLength: 0
```

#### 4.14 IP Stack Reuse

The IP stack (ARP, DHCP, ICMP, TCP) in `ecm_bridge.c` can be **directly reused**:

```c
// Option A: Extract to common module
// APP/ip_stack.h / APP/ip_stack.c
// Move: handle_arp_frame, handle_icmp_packet, handle_dhcp_packet
// Both ecm_bridge.c and ncm_bridge.c call these

// Option B: Copy to ncm_bridge.c (simpler, more code duplication)
// Just copy the IP handling functions
```

### Phase 5: USB Mode Configuration

#### 4.15 usb_work_mode.h Changes

```c
// Add new mode
#define USB_WORK_MODE_HID_NCM_BRIDGE  8

// Update validation
#if (USB_WORK_MODE != USB_WORK_MODE_HID_ACM) && \
    (USB_WORK_MODE != USB_WORK_MODE_HID_UAC_ACM) && \
    ...
    (USB_WORK_MODE != USB_WORK_MODE_HID_NCM_BRIDGE)  // ADD
#error Unsupported USB_WORK_MODE.
#endif

// Add capability macros
#define USB_WORK_MODE_HAS_HOST_CDC_NCM   (USB_WORK_MODE == USB_WORK_MODE_HID_NCM_BRIDGE)
#define USB_WORK_MODE_BRIDGE_NCM         (USB_WORK_MODE == USB_WORK_MODE_HID_NCM_BRIDGE)
#define USB_WORK_MODE_COMPILES_BRIDGE_NCM (USB_WORK_MODE_BRIDGE_NCM || USB_WORK_MODE_RUNTIME_PROFILES)
```

#### 4.16 usb_mode_config.c Changes

```c
// Add NCM to capability mask
uint32_t USB_ModeConfig_GetCapabilityMask(void)
{
    uint32_t mask = 0;
    // ... existing capabilities ...
#if USB_WORK_MODE_COMPILES_BRIDGE_NCM
    mask |= USB_MODE_CAP_HOST_CDC_NCM;
#endif
    return mask;
}

// Add NCM query function
uint8_t USB_ModeConfig_ModeHasHostCdcNcm(uint8_t mode)
{
    return (mode == USB_WORK_MODE_HID_NCM_BRIDGE) ? 1 : 0;
}
```

---

## 5. Memory Budget Analysis

### 5.1 Flash Usage Estimate

| Component | Size |
|-----------|------|
| NCM descriptors | ~100 bytes |
| NTB builder/parser | ~400 bytes |
| NCM class request handlers | ~200 bytes |
| NCM bridge (IP stack reuse) | ~2KB (shared with ECM) |
| **Total additional** | **~3KB** |

### 5.2 RAM Usage Estimate

| Component | Size |
|-----------|------|
| NTB TX buffer | 1280 bytes |
| NTB RX buffer | 1280 bytes |
| NDP tracking | ~32 bytes |
| Sequence counter | 2 bytes |
| **Total additional** | **~2.5KB** |

### 5.3 PMA (Packet Memory Array) Usage

Current ECM uses 32-byte packets to fit PMA constraints. NCM can use the same approach:

```
EP0: 64 bytes (control)
EP1: 64 bytes (keyboard)
EP2: 64 bytes (mouse)
EP3: 64 bytes (absolute mouse / ECM bulk OUT)
EP4: 64 bytes (media / ECM bulk IN)
EP5: 16 bytes (notification)
EP6: 64 bytes (bulk OUT)
EP7: 64 bytes (bulk IN)
Total: ~400 bytes (fits in 512-byte PMA)
```

---

## 6. Testing Plan

### 6.1 Windows Testing

| Test | Expected Result |
|------|-----------------|
| Plug into Windows 10 1903+ | Device enumerates, no driver prompt |
| Check Device Manager | "Remote NCM Compatible Device" under Network |
| IP configuration | Gets IP via DHCP or static |
| Ping device IP | Replies received |
| SSH to device | Connection established |
| Large file transfer | Works at ~5-10 Mbps |

### 6.2 Linux Testing

**Test Results (Nov 19, 2026)**: ✅ PASSED

```bash
# Check driver binding
lsusb -t
# Should show: Driver=cdc_ncm

# Check dmesg for successful bind
sudo dmesg -T | grep cdc_ncm
# Expected output:
# cdc_ncm 1-6:1.4: MAC-Address: 02:00:00:00:00:01
# cdc_ncm 1-6:1.4: dwNtbInMaxSize=1280 is too small. Using 2048
# cdc_ncm 1-6:1.4: setting rx_max = 2048
# cdc_ncm 1-6:1.4: setting tx_max = 16384
# cdc_ncm 1-6:1.4 usb0: register 'cdc_ncm' at usb-0000:00:15.0-6, CDC NCM (NO ZLP), 02:00:00:00:00:01
# cdc_ncm 1-6:1.4 enp0s21f0u6i4: renamed from usb0

# Check interface
ip link show enp0s21f0u6i4
# Should show: state UP (or NO-CARRIER until data path tested)

# Assign IP and test connectivity
sudo ip link set enp0s21f0u6i4 up
sudo ip addr add 192.168.100.2/24 dev enp0s21f0u6i4
ping 192.168.100.1
```

**Known Issues**:
- Kernel adjusts `dwNtbInMaxSize` from 1280 to 2048 (minimum requirement)
- Interface shows NO-CARRIER until link notifications are sent

**Final Test Results (Nov 19, 2026)**: ✅ PASSED

```bash
# Interface state after link notifications
ip link show enp0s21f0u6i4
# 3: enp0s21f0u6i4: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500
#     link/ether 02:00:00:00:00:01 brd ff:ff:ff:ff:ff:ff
#     state UP mode DEFAULT

# Assign static IP and test connectivity
sudo ip addr add 192.168.100.2/24 dev enp0s21f0u6i4
ping -c 3 192.168.100.1
# PING 192.168.100.1 (192.168.100.1) 56(84) bytes of data.
# 64 bytes from 192.168.100.1: icmp_seq=1 ttl=64 time=2.5ms
# 64 bytes from 192.168.100.1: icmp_seq=2 ttl=64 time=1.2ms
# 64 bytes from 192.168.100.1: icmp_seq=3 ttl=64 time=1.1ms
# --- 192.168.100.1 ping statistics ---
# 3 packets transmitted, 3 received, 0% packet loss
```

### 6.3 macOS Testing

```bash
# Check interface
ifconfig | grep -A5 "CDC"
# Should show enX with IP

# Test connectivity
ping 192.168.100.1
```

### 6.4 Performance Benchmarks

```bash
# iperf3 test
iperf3 -c 192.168.100.1 -t 30

# Expected: 5-15 Mbps (USB FS bulk limit ~12 Mbps)
```

---

## 7. Implementation Timeline

| Phase | Duration | Deliverables | Status |
|-------|----------|--------------|--------|
| **Phase 1**: Descriptors | 1-2 days | NCM descriptors compile, device enumerates | ✅ DONE |
| **Phase 2**: NTB Protocol | 3-5 days | TX/RX NTB builder/parser working | ✅ DONE |
| **Phase 3**: Class Requests | 1-2 days | NCM-specific requests handled | ✅ DONE |
| **Phase 4**: Integration | 2-3 days | Bridge API, IP stack reuse | ✅ DONE |
| **Phase 5**: Testing | 2-3 days | Windows/Linux/macOS validation | 🔄 IN PROGRESS |
| **Total** | **~2 weeks** | Production-ready NCM support | |

**Actual Progress (Nov 19, 2026)**: ✅ COMPLETED
- ✅ Linux cdc_ncm driver binds successfully
- ✅ Network interface created (enp0s21f0u6i4)
- ✅ MAC address correctly reported
- ✅ NTB parameters accepted
- ✅ USBFS-side data path implemented (EP5 OUT, EP6 IN, EP7 IN)
- ✅ Link notification state machine working (DOWN→UP→SPEED→BEACON)
- ✅ Ping test successful (192.168.100.1, 0% packet loss)
- 🔄 Windows testing pending
- 🔄 macOS testing pending

### Critical Bug Fix: BLE Version Mismatch

During testing, the firmware was discovered to hang in `WCHBLE_Init()` due to a BLE library version mismatch check:

```c
// HAL/MCU.c - WCHBLE_Init()
if(!tmos_memcmp(VER_LIB, VER_FILE, strlen(VER_FILE))) {
    PRINT("head file error...\n");
    while(1);  // <-- Infinite loop!
}
```

**Solution**: Disable BLE initialization for NCM bridge mode (mode 8):

```c
// APP/peripheral_main.c
#if defined(ENABLE_BLE) && USB_WORK_MODE != 8
    WCHBLE_Init();
    HAL_Init();
    GAPRole_PeripheralInit();
    Peripheral_Init();
#endif
```

This allows NCM bridge mode to work on devices with BLE enabled in the build.

---

## 8. Risk Mitigation

### 8.1 Technical Risks

| Risk | Mitigation | Actual Finding |
|------|------------|----------------|
| Windows doesn't enumerate | Verify descriptors with `usbview.exe` | Not tested yet |
| NTB parsing fails | Start with single-datagram NTBs | ✅ Single-frame NTBs work |
| Performance worse than ECM | Fall back to ECM for Windows | TBD |
| PMA overflow | Use 32-byte packets like current ECM | ✅ Works with 64-byte packets |
| **Kernel rejects descriptors** | **Verify NCM Functional Descriptor subtype** | **CRITICAL: Must use 0x1A, not 0x0D!** |

### 8.2 Compatibility Risks

| Risk | Mitigation | Actual Finding |
|------|------------|----------------|
| Old Windows (pre-1903) | Keep ECM as fallback mode | Not tested yet |
| Linux kernel < 3.x | `cdc_ncm` backported to 2.6.38+ | ✅ Works on kernel 6.x |
| macOS < 10.14 | Keep ECM as fallback | Not tested yet |
| **Linux kernel ignores descriptor** | **Use correct subtype (0x1A)** | **Fixed: subtype 0x1A required** |

---

## 9. Future Enhancements

### 9.1 NTB Batching Optimization

Current plan: 1 frame per NTB (simplest)
Future: Batch multiple frames per NTB for better throughput

### 9.2 32-bit NTB Format

Current plan: 16-bit NTB (simpler)
Future: Support 32-bit NTB for >64KB transfers

### 9.3 Zero-Padding Optimization

NCM requires 4-byte alignment. Current plan pads with zeros.
Future: Pack frames more efficiently.

---

## 10. References

- [USB CDC NCM Specification 1.0](https://www.usb.org/document-library/network-control-model-devices)
- [Windows CDC-NCM Driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/usb-ncm-model)
- [Linux cdc_ncm Driver](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/usb/cdc_ncm.c)
- WCH CH32V20x USB Device Library

---

## Appendix A: Descriptor Byte Comparison

### ECM vs NCM IAD

```
ECM: 08 0B 04 02 02 06 00 00
NCM: 08 0B 04 02 02 0D 00 00
                         ^^
                         Different!
```

### ECM vs NCM Communication Interface

```
ECM: 09 04 04 00 01 02 06 00 00
NCM: 09 04 04 00 01 02 0D 00 00
                               ^^
                               Different!
```

### NCM Functional Descriptor (new)

**Linux kernel expects subtype 0x1A, not 0x0D!**

```
NCM: 08 24 1A 00 00 05 00 10
     ^^ bLength = 8
        ^^ bDescriptorType = CS_INTERFACE (0x24)
           ^^ bDescriptorSubtype = USB_CDC_NCM_TYPE (0x1A) ← CRITICAL!
              ^^ bmCapabilities = 0
                 ^^^^^ wNtbInMaxSize = 0x0500 = 1280
                          ^^^^^ wNdpInMaxSize = 0x0010 = 16
```

**Why 8 bytes instead of 6?** The Linux kernel expects the full 8-byte NCM Functional Descriptor with both wNtbInMaxSize and wNdpInMaxSize fields.

---

## Appendix B: Quick Start Checklist

- [x] Create `feature/cdc-ncm` branch
- [x] Add `USB_WORK_MODE_HID_NCM_BRIDGE` to `usb_work_mode.h`
- [x] Copy `ecm_bridge.c` → `ncm_bridge.c`
- [x] Modify descriptors in `usbd_desc.c` (subclass 0x06 → 0x0D)
- [x] Add NCM functional descriptor with **subtype 0x1A** (not 0x0D!)
- [x] Add MAC address string descriptor (iMACAddress field)
- [x] Implement NTB builder (start with single-frame NTBs)
- [x] Implement NTB parser
- [x] Add NCM class request handlers (GET_NTB_PARAMETERS mandatory)
- [x] Test enumeration on Linux (cdc_ncm driver binds)
- [x] Test basic ping
- [ ] Test SSH
- [ ] Performance benchmark
- [ ] Windows 10/11 compatibility test
- [ ] macOS compatibility test
