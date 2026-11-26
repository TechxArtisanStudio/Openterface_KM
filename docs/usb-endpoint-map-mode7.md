# Mode 7 endpoint map (HID + Host ACM + Host ECM + Target ECM)

This document describes the complete USB endpoint layout under
`USB_WORK_MODE=hid_acm_ecm_bridge` (mode 7). It covers the endpoint assignment of
both controllers, the PMA/DMA memory maps, the interface mapping as seen from the
host, the data flow, and a comparison against the anomalies before the fix.

---

## 0. Topology

```
                    ┌───────────────────────────────┐
                    │        CH32V208 (mode 7)      │
  Mac / Host  ──────┤ USBLIB @0x40005C00 (512B PMA) │
  (host role)       │   · CDC ACM                   │
                    │   · Host ECM                  │
                    │                               │
                    │ USBFS @0x50000000 (SRAM DMA)  │
  Target PC  ───────┤   · HID ×4                    │
  (device role)     │   · Target ECM                │
                    └───────────────────────────────┘
```

The two controllers share the same physical USB port, but their endpoint spaces,
buffering schemes and interrupts are completely independent:

| Controller | Base address | Buffer | Faces |
|---|---|---|---|
| USBLIB (old library) | `0x40005C00`, IRQ 36 | 512B PMA @`0x40006000` | Mac / Host |
| USBFS (new library) | `0x50000000`, IRQ 59 | ordinary SRAM (`UEP_DMA + 0x20000000`) | Target PC |

Key principle: **Host ACM / Host ECM live only on USBLIB, Target ECM lives only on
USBFS**. Both descriptor sets must respect that boundary, otherwise you get
duplicate interfaces and misrouted data.

---

## 1. The USBLIB controller (Mac side)

### 1.1 Endpoint assignment (after the fix)

| EP | Direction | Type | MPS | PMA address | Bound interface | Notes |
|---|---|---|---|---|---|---|
| EP0 | IN/OUT | Control | 64 | `0x040` / `0x080` | all | control transfers |
| EP1 | IN | Interrupt | 16 | `0x0C0` | IF0 ACM comm | ACM notifications |
| EP5 | OUT | Bulk | 64 | `0x0D0` | IF1 ACM data | Mac → device |
| EP3 | IN | Bulk | 64 | `0x110` | IF1 ACM data | device → Mac (CH9329 replies) |
| EP2 | IN | Interrupt | 16 | `0x150` | IF2 Host ECM comm | ECM notifications |
| EP6 | OUT | Bulk | 32 | `0x180` | IF3 Host ECM data | Mac → device |
| EP6 | IN | Bulk | 32 | `0x1A0` | IF3 Host ECM data | device → Mac |

Host ECM data uses a **single bidirectional endpoint EP6** (OUT+IN on the same
number), symmetrical with the EP7 bidirectional endpoint on the Target side; EP4
is no longer assigned to Host ECM in combined mode. Target ECM occupies no USBLIB
endpoint: it lives on USBFS (EP5 notification + EP7 bidirectional data).

### 1.2 PMA address map (512B)

```
0x000 ┌─────────────────────────────┐
      │ BTABLE (8 EP × 8B = 64B)    │
0x040 │ EP0 RX              64B     │
0x080 │ EP0 TX              64B     │
0x0C0 │ EP1 TX   ACM notify  16B    │
0x0D0 │ EP5 RX   ACM data↓   64B    │
0x110 │ EP3 TX   ACM data↑   64B    │
0x150 │ EP2 TX   HostECM ntf 16B    │
0x180 │ EP6 RX   HostECM ↓   32B    │
0x1A0 │ EP6 TX   HostECM ↑   32B    │
0x1C0 ├──────── free 64B ────────────┤
0x200 └─────────────────────────────┘
```

Total usage `0x1C0 = 448B`. The ACM data directions use full-speed 64B (EP5/EP3),
while the Host ECM directions are configured at 32B to save PMA (the EP4 TX buffer
at `0x160` is no longer used).

---

## 2. The USBFS controller (Target PC side)

### 2.1 Endpoint assignment (after the fix)

| EP | Direction | Type | MPS | DMA buffer | Bound interface |
|---|---|---|---|---|---|
| EP0 | IN/OUT | Control | 64 | `USBFS_EP0_Buf[64]` | all |
| EP1 | IN | Interrupt | 8 | `USBFS_EP1_Buf` | IF0 HID keyboard |
| EP2 | IN | Interrupt | 8 | `USBFS_EP2_Buf` | IF1 HID relative mouse |
| EP3 | IN | Interrupt | 8 | `USBFS_EP3_Buf` | IF2 HID absolute mouse |
| EP4 | IN | Interrupt | 8 | `USBFS_EP4_Buf` | IF3 HID multimedia |
| EP5 | IN | Interrupt | 16 | `EP5_DatabufHD[128]` | IF4 Target ECM comm (notifications) |
| EP7 | OUT | Bulk | 32 | `EP7_DatabufHD + 0` | IF5 Target ECM data |
| EP7 | IN | Bulk | 32 | `EP7_DatabufHD + 64` | IF5 Target ECM data |

### 2.2 Endpoint mode registers

```
UEP4_1_MOD = EP1_TX_EN | EP4_TX_EN
UEP2_3_MOD = EP2_TX_EN | EP3_TX_EN
UEP5_6_MOD = EP5_TX_EN
UEP_MOD(3) = EP7_TX_EN | EP7_RX_EN
```

### 2.3 DMA buffer layout

USBFS does not use PMA; each endpoint buffer lives in ordinary SRAM:

```
EPn_DatabufHD[128]
    +0   ┌───────────────┐
         │ RX half (64B) │  ← used only when the endpoint is configured as RX
    +64  ├───────────────┤
         │ TX half (64B) │  ← when TX_EN and RX_EN are both set and BUF_MOD is off,
         │               │    TX always writes at +64
    +127 └───────────────┘
```

EP7 enables TX+RX, so RX sits at `+0` and TX at `+64`. A single-endpoint buffer
must therefore be ≥ 96B, and the current 128B satisfies that.

---

## 3. Host view: interface ↔ endpoint mapping

### 3.1 What the Mac sees (USBLIB, 4 interfaces / 154B)

```
IF0  CDC ACM control  ─ EP1 IN  16B (notification)
IF1  CDC ACM data     ─ EP5 OUT 64B / EP3 IN 64B
IF2  CDC ECM control  ─ EP2 IN  16B (notification)
IF3  CDC ECM data     ─ EP6 OUT 32B / EP6 IN 32B (one bidirectional endpoint)
```

### 3.2 What the Target PC sees (USBFS, 6 interfaces / 181B)

```
IF0  HID keyboard     ─ EP1 IN 8B
IF1  HID rel. mouse   ─ EP2 IN 8B
IF2  HID abs. mouse   ─ EP3 IN 8B
IF3  HID multimedia   ─ EP4 IN 8B
IF4  CDC ECM control  ─ EP5 IN 16B (notification)
IF5  CDC ECM data     ─ alt0 empty / alt1: EP7 OUT 32B + EP7 IN 32B
```

A CDC-ECM control interface must declare exactly one interrupt IN endpoint;
`cdc_ether` depends on it to receive `NETWORK_CONNECTION` notifications. Without a
notification endpoint the interface stays in NO-CARRIER.

The notification is a one-shot edge and Linux drops it: the host only starts
polling the notification endpoint after `usbnet_open()`, while
`usbnet_link_change()` ignores link-up outright while the interface is not yet
`IFF_UP`. Once carrier is 0, a driver with `FLAG_POINTTOPOINT` also tears down RX
URB submission, so the interface fails in both directions at once. The firmware
therefore keeps beaconing `NETWORK_CONNECTION=1` after down→up→speed
(`ECM_LINK_UP_BEACON_MS`), so that whenever the host starts polling it immediately
reads up.

---

## 4. Data flow

```
Mac ──EP6 OUT──► ECM_Bridge_HostRxDone ──► g_host_to_target
                                              │
                                              ▼ poll_target_tx()
                                        USBFS EP7 IN ──► Target PC

Target PC ──USBFS EP7 OUT──► ECM_Bridge_TargetRxDone ──► g_target_to_host
                                                             │
                                                             ▼ poll_host_tx()
                                                   USBLIB EP6 IN ──► Mac
```

Notification paths:

```
Target ECM link state ─► poll_target_notification() ─► USBFS EP5 IN (wIndex=IF4)
Host   ECM link state ─► poll_host_notification()   ─► USBLIB EP2 IN (wIndex=IF2)
```

---

## 5. Anomalies before the fix (for comparison)

| # | Location | Anomaly | Impact |
|---|---|---|---|
| 1 | USBLIB EP7 (IF4/5) | Target ECM duplicated onto the Mac side | duplicate interfaces + data loopback |
| 2 | USBLIB EP6 IN | descriptor moved to EP6 IN, but PMA/transmit code never followed | ACM serial port never replies |
| 3 | USBLIB EP3 IN | present in hardware, absent from the descriptor | replies sent to an endpoint the host never reads |
| 4 | USBLIB EP6 OUT / EP4 IN | 32B of PMA accessed as 64B | slots overwrite each other |
| 5 | USBFS EP3 | descriptor says IN / hardware configured as RX | HID absolute mouse unusable |
| 6 | USBFS EP5 | enabled but not declared | notifications cannot be sent |
| 7 | USBFS EP6 | enabled but not declared | dirty data enters the Target RX path |
| 8 | USBFS IF4 | `bNumEndpoints=1` with no endpoint descriptor | cdc_ether bind/notification failure |
| 9 | USBFS EP7 | 32B hardware / 64B transmit | ZLP rules broken, frames stick together |
| 10 | static vs runtime descriptors | HID count 2 vs 4, HID offsets hard-coded | HID enumeration reads garbage |
| 11 | `ecm_bridge.c` | `#ifdef USBFS_DEVICE` never true | Target replies sent to the Mac |
| 12 | `ecm_bridge.c` | Target notifications use EP5, but mode 7 has no EP5 | link state never received |
| 13 | `SET_INTERFACE` | matches IF5 against IF7 | the alt=1 activation logic never runs |
| 14 | notification wIndex | Host ECM writes 4, actual interface is IF2 | the Mac discards the notification |
| 15 | USBFS ECM descriptor selection | decided by `HasHostCdcEcm` alone, so modes 4/5 also get mode 7's EP7 block | descriptors disagree with the EP6/EP7 hardware configuration |

---

## 6. Open items (not yet done)

- `GET_DESCRIPTOR(HID)` still reads data at the fixed offsets 18/43/68/93 of the
  static `MyCfgDescr`, which does not match the runtime layout of the four HID
  interfaces.
- `docs/controllable-usb-endpoint-modes.md` has not yet been extended with modes
  6/7.
- mode 5 (HID+UAC+ECM) runtime descriptors shift the ECM endpoints by +2 overall
  (EP6→EP8, EP7→EP9), outside the USBFS EP0~EP7 range; the UAC and ECM endpoint
  assignment needs to be replanned.

---

## 7. Where the fix landed

| File | Change |
|---|---|
| `APP/usbd_desc.c` | add the EP5 IN notification endpoint to the Target ECM control interface (block 72→79B); only modes 6/7 use the EP5+EP7 block, modes 4/5 go back to the classic EP5/EP6/EP7 layout |
| `APP/ch32v20x_usbfs_device.c` | `SET_INTERFACE` data-interface base changed to IF5; USBFS endpoint init configures HID EP1~EP4 + EP5 notification + EP7 bidirectional; remove the leftover EP3 RX / EP6 configuration and `EP3_DatabufHD` |
| `APP/ecm_bridge.c` | Target transmit always goes through USBFS EP7, with 32B fragmentation and 32B ZLP boundaries; Host notification interface number changed to IF2 |
| `APP/km_ring.c` | restore per-endpoint HID report dispatch (EP1 keyboard 8B / EP2 relative mouse 4B / EP3 absolute mouse 5~6B / EP4 multimedia 2/4B) and drop the Report ID prefix |
| `APP/USBLIB/CONFIG/usb_desc.c` | remove the duplicate Target ECM descriptor block; ACM data IN stays on EP3; Host ECM data becomes EP6 OUT `0x06` + EP6 IN `0x86` (32B bidirectional) |
| `APP/USBLIB/CONFIG/usb_conf.h` | PMA layout tightened to `0x1C0`; add `ENDP6_TX_ECM_ADDR (0x1A0)`; point `ECM_USBD_BULK_IN_ENDP`/`_BUSY`/`EP_BULK_IN` at EP6 / `USBD_Endp6_Busy` / `EP6_IN` |
| `APP/USBLIB/CONFIG/usb_prop.c` | Host ECM uses EP6 bidirectionally: configure EP6 TX/RX in both reset and `SET_INTERFACE(IF3 alt1)`; remove USBLIB EP7 initialisation |
| `APP/USBLIB/CONFIG/usb_endp.c` | add `EP6_IN_Callback` and `USBD_Endp6_Busy`; change the ECM bulk IN branch of `USBD_ENDPx_DataUp` from EP4 to EP6; handle EP6 OUT as 32B |
