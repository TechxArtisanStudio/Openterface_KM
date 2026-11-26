# Mode 8 endpoint map (HID + Target CDC NCM)

This document describes the complete USB endpoint layout under
`USB_WORK_MODE=hid_ncm_bridge` (mode 8). It covers the endpoint assignment of the
USBFS controller, the SRAM DMA memory map, the interface mapping as seen from the
host, and the data flow.

---

## 0. Topology

```
                    ┌───────────────────────────────┐
                    │        CH32V208 (mode 8)      │
                    │                               │
                    │ USBFS @0x50000000 (SRAM DMA)  │
  Windows PC ───────┤   · HID ×4                    │
  (host role)       │   · Target CDC NCM            │
                    └───────────────────────────────┘
```

Mode 8 uses only the USBFS controller (Target side), presenting itself to a
Windows PC as a CDC NCM network adapter. The host side (USBLIB controller) is
unused.

---

## 1. The USBFS controller (Target PC side / the device Windows PC sees)

### 1.1 Endpoint assignment

| EP | Direction | Type | MPS | DMA buffer | Bound interface | Notes |
|---|---|---|---|---|---|---|
| EP0 | IN/OUT | Control | 64 | `USBFS_EP0_Buf[64]` | all | control transfers |
| EP1 | IN | Interrupt | 8 | `USBFS_EP1_Buf` | IF0 HID keyboard | |
| EP2 | IN | Interrupt | 8 | `USBFS_EP2_Buf` | IF1 HID relative mouse | |
| EP3 | IN | Interrupt | 8 | `USBFS_EP3_Buf` | IF2 HID absolute mouse | |
| EP4 | IN | Interrupt | 8 | `USBFS_EP4_Buf` | IF3 HID multimedia | |
| EP6 | IN | Interrupt | 16 | `EP6_DatabufHD[128]` | IF4 NCM comm | NCM notifications |
| EP5 | OUT | Bulk | 64 | `EP5_DatabufHD[128]` | IF5 NCM data | host → device |
| EP7 | IN | Bulk | 64 | `EP7_DatabufHD[128]` | IF5 NCM data | device → host |

### 1.2 Endpoint mode registers

```
UEP4_1_MOD = EP1_TX_EN | EP4_TX_EN
UEP2_3_MOD = EP2_TX_EN | EP3_TX_EN
UEP5_6_MOD = EP5_RX_EN | EP6_TX_EN
UEP_MOD(3) = EP7_TX_EN
```

### 1.3 DMA buffer layout

USBFS does not use PMA; each endpoint buffer lives in ordinary SRAM:

```
EPn_DatabufHD[128]
    +0   ┌───────────────┐
         │ RX half (64B) │  ← used when the endpoint is configured as RX
    +64  ├───────────────┤
         │ TX half (64B) │  ← written at +64 whenever TX_EN and RX_EN are both set
    +127 └───────────────┘
```

EP6/EP7 enable TX+RX, so RX sits at `+0` and TX at `+64`. A single-endpoint buffer
must therefore be ≥ 96B, and the current 128B satisfies that.

---

## 2. Host view: interface ↔ endpoint mapping

### 2.1 What Windows sees (USBFS, 6 interfaces)

```
IF0  HID keyboard     ─ EP1 IN 8B
IF1  HID rel. mouse   ─ EP2 IN 8B
IF2  HID abs. mouse   ─ EP3 IN 8B
IF3  HID multimedia   ─ EP4 IN 8B
IF4  CDC NCM control  ─ EP6 IN 16B (notification)
IF5  CDC NCM data     ─ alt0 empty / alt1: EP5 OUT 64B + EP7 IN 64B
```

A CDC-NCM control interface must declare one interrupt IN endpoint (EP6);
`usbncm.sys` depends on it to receive `NETWORK_CONNECTION` notifications. Without a
notification endpoint the adapter stays in NO-CARRIER.

---

## 3. Data flow

```
Windows PC ──EP5 OUT──► NCM_Bridge_HostRxDone ──► g_host_tx_queue
                                                             │
                                                             ▼ NCM_Bridge_Poll()
                                                       USBFS EP7 IN ──► Windows PC

NCM_Bridge_HostRxDone():
  - parse the NTB (Network Transfer Block)
  - extract the Ethernet frame
  - handle ARP/ICMP/DHCP
  - enqueue into g_host_tx_queue

NCM_Bridge_Poll():
  - if EP7 IN is idle and g_host_tx_queue is not empty
  - build the NTB (single frame)
  - send to USBFS EP7 IN
```

Notification path:

```
NCM link state ─► poll_host_notification() ─► USBFS EP6 IN (wIndex=IF4)
```

Notification state machine: `DOWN (1) → UP (2) → SPEED (3) → BEACON (4)`
- DOWN: send NETWORK_CONNECTION=0 (link down)
- UP: send NETWORK_CONNECTION=1 (link up)
- SPEED: send CONNECTION_SPEED_CHANGE (12 Mbps)
- BEACON: periodically send NETWORK_CONNECTION=1

---

## 4. NCM protocol implementation

### 4.1 NTB structure

```
NTB Header (14 bytes):
  wBlockLength    (2 bytes) - Total NTB length (little-endian)
  wSequence       (2 bytes) - Sequence number
  wNdpIndex       (2 bytes) - Offset to NDP
  wNdpIndexAlt    (2 bytes) - 0 (no alternate NDP)
  Reserved        (4 bytes) - Zero

NDP (Network Datagram Pointer) - 16 bytes for a single frame:
  dwSignature     (4 bytes) - 'NCM0'
  wLength         (2 bytes) - NDP length
  wNextNdpIndex   (2 bytes) - 0 (last)
  Datagram[0]:
    wDatagramIndex  (2 bytes) - Frame offset
    wDatagramLength (2 bytes) - Frame length
  Terminator: 0x0000, 0x0000
```

### 4.2 Key parameters

| Parameter | Value | Notes |
|---|---|---|
| `wMaxSegmentSize` | 1514 | standard Ethernet MTU |
| `wNtbInMaxSize` | 16384 (0x4000) | maximum inbound NTB |
| `wNdpInMaxSize` | 1024 (0x0400) | maximum NDP |
| `wNtbOutMaxSize` | 16384 (0x4000) | maximum outbound NTB |
| `bmNetworkCapabilities` | 0x0001 | BIT0=NET_ADDRESS |

### 4.3 Class request handling

| Request | Code | Handling |
|---|---|---|
| GET_NTB_PARAMETERS | 0x80 | return the 28-byte parameter structure |
| GET_NTB_INPUT_SIZE | 0x87 | return wNtbInMaxSize |
| SET_NTB_INPUT_SIZE | 0x86 | accepted and ignored |
| GET_NTB_FORMAT | 0x89 | return the 16-bit format |
| SET_NTB_FORMAT | 0x88 | 16-bit only |
| GET_MAX_DATAGRAM | 0x8A | return wMaxSegmentSize |
| SET_MAX_DATAGRAM | 0x8B | accepted and ignored |
| SET_PACKET_FILTER | 0x43 | accepted and ignored |

---

## 5. Key code locations

| File | Relevant functions/structures |
|---|---|
| `APP/usbd_desc.c` | `USBFS_RuntimeNcmBlock`, `USBFS_BuildRuntimeConfigDescriptor()` |
| `APP/ncm_bridge.c` | `NCM_BuildNtb()`, `NCM_ParseNtb()`, `poll_host_notification()`, `NCM_Bridge_Poll()` |
| `APP/ch32v20x_usbfs_device.c` | USBFS endpoint initialisation, class request handling |
| `APP/USBLIB/CONFIG/usb_prop.c` | USBD endpoint initialisation |
| `APP/USBLIB/CONFIG/usb_endp.c` | EP5 OUT / EP6 IN / EP7 IN callbacks |
| `APP/include/ncm_bridge.h` | API declarations |
| `APP/include/usb_work_mode.h` | Mode 8 definition |

---

## 6. Build and test

### 6.1 Build command

```bash
export PATH="/home/bbot/tools/MRS-linux-x64/resources/app/resources/linux/components/WCH/Toolchain/RISC-V Embedded GCC/bin:$PATH"
make PRODUCT=keymod-plus USB_WORK_MODE=hid_ncm_bridge
```

### 6.2 Flashing

```bash
python3 tools/ctrl_board.py flash build/openterface-km-firmware.bin
```

### 6.3 Windows verification

1. Device Manager should show a "Remote NCM Compatible Device" or "CDC NCM" class device
2. A network adapter should appear, either obtaining an address automatically (DHCP) or using a static IP
3. `ping <device_ip>` should succeed
4. `ssh user@<device_ip>` should connect

### 6.4 Linux verification

```bash
# check the driver binding
lsusb -t | grep cdc_ncm
# should show: Driver=cdc_ncm

# check the interface
ip link show enp0s21f0u6i4
# should show UP and have an IP

# test connectivity
ping 192.168.100.1
ssh user@192.168.100.1
```

---

## 7. Known issues

1. **Linux kernel descriptor parsing**: the Linux `cdc_ncm` driver expects the NCM Functional Descriptor subtype to be `0x1A` (USB_CDC_NCM_TYPE) rather than the `0x0D` defined by the specification. The current firmware includes both descriptors (0x0D and 0x1A) for compatibility.

2. **Carrier/Link-up**: the notification endpoint sends NETWORK_CONNECTION=1, but the interface may stay in NO-CARRIER until the host polls the notification endpoint after `IFF_UP`.

3. **USBFS controller**: mode 8 uses the USBFS controller (Target side); endpoint configuration is done in `ch32v20x_usbfs_device.c`.

## 8. Resolved issues

1. **Descriptor transfer error (Nov 19, 2026)**: the USBFS controller transferred the wrong NCM descriptor data.
   - Root cause: the firmware was not recompiled after the static NCM descriptor code was added
   - Fix:
     - create a static `USBFS_StaticNcmConfigDescriptor` in `usbd_desc.c`
     - add a direct reference in the USBFS interrupt handler (`ch32v20x_usbfs_device.c`)
     - recompile the firmware with `USB_WORK_MODE=hid_ncm_bridge`
   - the detailed debugging process is in `docs/ncm-descriptor-debug.md`
