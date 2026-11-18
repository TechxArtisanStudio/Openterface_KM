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

```c
// NCM Functional Descriptor (6 bytes)
0x06,              // bLength
0x24,              // bDescriptorType (CS_INTERFACE)
0x0D,              // bDescriptorSubtype (NCM Functional)
0x00,              // bmCapabilities
0x00, 0x05,        // wNtbInMaxSize (1280 bytes typical)
0x00, 0x10,        // wNdpInMaxSize (16 bytes for single NDP)
```

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

NCM requires additional functional descriptor:

```c
// CDC Functional Descriptors for NCM
0x05, 0x24, 0x00, 0x10, 0x01,        // Header (CDC 1.10)
0x05, 0x24, 0x06, 0x04, 0x05,        // Union: control IF4, data IF5
0x0D, 0x24, 0x0F, 0x04,              // Ethernet Networking (iMACAddress=4)
    0x00, 0x00, 0x00, 0x00,          // bmEthernetStatistics
    0xEA, 0x05,                      // wMaxSegmentSize: 1514
    0x00, 0x00,                      // wNumberMCFilters
    0x00,                            // bNumberPowerFilters
// NEW: NCM Functional Descriptor
0x06, 0x24, 0x0D,                    // NCM Functional (6 bytes)
    0x00,                            // bmCapabilities (no filtering)
    0x00, 0x05,                      // wNtbInMaxSize: 1280 bytes
    0x00, 0x10,                      // wNdpInMaxSize: 16 bytes
```

#### 4.5 Notification Endpoint

Same as ECM (Interrupt IN):

```c
// Notification Endpoint (unchanged)
0x07,                    // bLength
0x05,                    // bDescriptorType (Endpoint)
0x85,                    // bEndpointAddress: EP5 IN
0x03,                    // bmAttributes: Interrupt
0x10, 0x00,              // wMaxPacketSize: 16 bytes
0x10,                    // bInterval: 16ms
```

#### 4.6 Data Interface

Same structure as ECM (alt 0 = idle, alt 1 = active):

```c
// Data Interface Alt 0 (idle - zero bandwidth)
0x09, 0x04, 0x05, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,

// Data Interface Alt 1 (active - bulk endpoints)
0x09, 0x04, 0x05, 0x01, 0x02, 0x0A, 0x00, 0x00, 0x00,

// Bulk OUT Endpoint
0x07, 0x05, 0x06, 0x02, 0x40, 0x00, 0x00,  // EP6 OUT, 64 bytes

// Bulk IN Endpoint
0x07, 0x05, 0x87, 0x02, 0x40, 0x00, 0x00,  // EP7 IN, 64 bytes
```

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

NCM defines additional class requests that must be handled:

```c
// APP/ch32v20x_usbfs_device.c

#define NCM_SET_NTB_INPUT_SIZE   0x86  // Set max NTB input size
#define NCM_GET_NTB_INPUT_SIZE   0x87  // Get max NTB input size
#define NCM_SET_NTB_FORMAT       0x88  // Set NTB format (16/32 bit)
#define NCM_GET_NTB_FORMAT       0x89  // Get NTB format

// In USBFS class request handler:
case NCM_SET_NTB_INPUT_SIZE:
    // Accept but ignore (we use fixed size)
    USBFS_DevConfig = USBFS_DevConfig; // NOP
    break;

case NCM_GET_NTB_INPUT_SIZE:
    // Return our max NTB size
    EP0_DatabufHD[0] = 0x00;  // 1280 = 0x0500
    EP0_DatabufHD[1] = 0x05;
    EP0_DatabufHD[2] = 0x00;
    EP0_DatabufHD[3] = 0x00;
    // Setup EP0 TX for 4 bytes
    break;

case NCM_SET_NTB_FORMAT:
case NCM_GET_NTB_FORMAT:
    // We only support 16-bit format
    if (req_code == NCM_SET_NTB_FORMAT && value != 0) {
        // Stall - unsupported format
    }
    break;
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

```bash
# Check driver binding
lsusb -t
# Should show: Driver=cdc_ncm

# Check interface
ip link show usb0
# Should be UP with IP

# Test connectivity
ping 192.168.100.1
ssh user@192.168.100.1
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

| Phase | Duration | Deliverables |
|-------|----------|--------------|
| **Phase 1**: Descriptors | 1-2 days | NCM descriptors compile, device enumerates |
| **Phase 2**: NTB Protocol | 3-5 days | TX/RX NTB builder/parser working |
| **Phase 3**: Class Requests | 1-2 days | NCM-specific requests handled |
| **Phase 4**: Integration | 2-3 days | Bridge API, IP stack reuse |
| **Phase 5**: Testing | 2-3 days | Windows/Linux/macOS validation |
| **Total** | **~2 weeks** | Production-ready NCM support |

---

## 8. Risk Mitigation

### 8.1 Technical Risks

| Risk | Mitigation |
|------|------------|
| Windows doesn't enumerate | Verify descriptors with `usbview.exe` |
| NTB parsing fails | Start with single-datagram NTBs |
| Performance worse than ECM | Fall back to ECM for Windows |
| PMA overflow | Use 32-byte packets like current ECM |

### 8.2 Compatibility Risks

| Risk | Mitigation |
|------|------------|
| Old Windows (pre-1903) | Keep ECM as fallback mode |
| Linux kernel < 3.x | `cdc_ncm` backported to 2.6.38+ |
| macOS < 10.14 | Keep ECM as fallback |

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

```
NCM: 06 24 0D 00 00 05 00 10
     ^^ bLength = 6
        ^^ bDescriptorType = CS_INTERFACE (0x24)
           ^^ bDescriptorSubtype = NCM Functional (0x0D)
              ^^ bmCapabilities = 0
                 ^^^^^ wNtbInMaxSize = 0x0500 = 1280
                          ^^^^^ wNdpInMaxSize = 0x0010 = 16
```

---

## Appendix B: Quick Start Checklist

- [ ] Create `feature/cdc-ncm` branch
- [ ] Add `USB_WORK_MODE_HID_NCM_BRIDGE` to `usb_work_mode.h`
- [ ] Copy `ecm_bridge.c` → `ncm_bridge.c`
- [ ] Modify descriptors in `usbd_desc.c` (subclass 0x06 → 0x0D)
- [ ] Add NCM functional descriptor
- [ ] Implement NTB builder (start with single-frame NTBs)
- [ ] Implement NTB parser
- [ ] Add NCM class request handlers
- [ ] Test enumeration on Windows 10
- [ ] Test basic ping
- [ ] Test SSH
- [ ] Performance benchmark
- [ ] Linux/macOS compatibility test
