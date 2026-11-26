# Mode 8 endpoint map — HID + CDC-NCM on both USB ports

Status: **Reference**
Applies to: `USB_WORK_MODE=hid_ncm_bridge` (mode 8), `PRODUCT=keymod-plus`
Describes: firmware tree at `4aa479b` (2026-11-26)

This is the endpoint-level map of mode 8: which endpoint does what, in which
buffer, behind which descriptor. What the link *does* with the frames (DHCP, the
one-shot DNS greeting, the portal, forwarding rules) lives in
[ncm-bridge.md](ncm-bridge.md) and is not repeated here.

Descriptor lengths below are asserted at compile time by the
`USBFS_RuntimeNcmBlock_SizeCheck` and `USBD_NcmConfigDescriptor_SizeCheck`
typedefs, so a stale length breaks the build. Endpoint numbers and buffer sizes
are read from the endpoint-initialisation code and have no such guard: if you
change one, update this file in the same commit.

---

## 1. What mode 8 is

The CH32V208 has two USB device controllers and mode 8 uses **both**:

| Controller | Stack | Port | USB class presented |
|---|---|---|---|
| USBLIB (PMA, 512 B) | `APP/USBLIB` | host port — the operator's machine | CDC-NCM only |
| USBFS (SRAM DMA) | `APP/ch32v20x_usbfs_device.c` | target port — the machine being controlled | 4 × HID + CDC-NCM |

```
 Fedora ──USB──► [USBLIB, PMA 512 B]    IF0 NCM comm   EP5 IN (interrupt)
  host port                             IF1 NCM data   EP3 OUT / EP4 IN (bulk)
                        CH32V208, mode 8
 Windows ──USB──► [USBFS, SRAM DMA]     IF0..IF3 HID   EP1..EP4 IN (interrupt)
  target port                           IF4 NCM comm   EP6 IN (interrupt)
                                        IF5 NCM data   EP5 OUT / EP7 IN (bulk)
```

Three consequences that surprise people:

* **Both ports get an IP.** The host port is not "just" a keyboard link in this
  mode; each side of the dongle hands its peer an Ethernet segment.
* **Mode 7 is the ECM version of the same idea** (host ECM + target ECM). Mode 8
  exists because Windows ships an in-box NCM driver (`UsbNcm.sys`) and no ECM one.
* **The host port carries no HID.** Keyboard, mouse, absolute mouse and media
  keys exist only on the target port. Input reaches those endpoints over the BLE
  link, not over the host port.

Verified deployment: Fedora on the host port, Windows 11 on the target port.

---

## 2. Target port — USBFS controller

### 2.1 Endpoint assignment

| EP | Dir | Type | MPS | Buffer | Bound interface | Purpose |
|---|---|---|---|---|---|---|
| EP0 | IN/OUT | Control | 64 | `USBFS_EP0_Buf[64]` | all | control transfers |
| EP1 | IN | Interrupt | 8 | `USBFS_EP1_Buf[64]` | IF0 HID keyboard | report, interval 10 ms |
| EP2 | IN | Interrupt | 8 | `USBFS_EP2_Buf[64]` | IF1 HID relative mouse | interval 1 ms |
| EP3 | IN | Interrupt | 8 | `USBFS_EP3_Buf[64]` | IF2 HID absolute mouse | interval 1 ms |
| EP4 | IN | Interrupt | 8 | `USBFS_EP4_Buf[64]` | IF3 HID multimedia | interval 10 ms |
| EP5 | OUT | Bulk | 64 | `EP5_DatabufHD[2048]` | IF5 NCM data | target → device NTBs |
| EP6 | IN | Interrupt | 16 | `EP6_DatabufHD[64+64]` | IF4 NCM comm | link notifications |
| EP7 | IN | Bulk | 64 | `EP7_DatabufHD[64+2048]` | IF5 NCM data | device → target NTBs |

EP7 is *configured* as bidirectional (TX|RX) even though nothing ever arrives on
its OUT direction; see 2.2.

### 2.2 Endpoint mode registers

```c
UEP4_1_MOD  = UEP1_TX_EN | UEP4_TX_EN;
UEP2_3_MOD  = UEP2_TX_EN | UEP3_TX_EN;
UEP5_6_MOD  = UEP5_RX_EN | UEP6_TX_EN;      /* NCM: EP5 OUT, EP6 IN     */
UEP_MOD( 3) = UEP7_TX_EN | UEP7_RX_EN;      /* NCM: EP7 IN (+ idle RX)  */
```

The RX half of EP7 is enabled "like ECM mode, USBFS may require it for bulk".
No RX handler is registered for EP7 in this mode, so the half is dead weight —
but removing it is not free (see §8, item 5).

### 2.3 Buffer layout

USBFS does not use PMA: every endpoint buffer is ordinary SRAM, and the TX/RX
halves of one endpoint live in the same array:

```
EP5_DatabufHD[2048]        EP6_DatabufHD[64+64]        EP7_DatabufHD[64+2048]
 +0     RX window 2048      +0  TX half 64              +0  RX half 64 (unused)
                            +64 unused                  +64 TX half 2048
```

* **EP5 (RX only)** — the whole 2048 B array is the receive window, because an
  NTB may be up to `NCM_NTB_MAX_SIZE` (2048 B).
* **EP6 (TX only)** — TX starts at `+0`; notification packets are 8 or 16 bytes,
  so the second 64-byte half is unused slack kept for symmetry with EP7.
* **EP7 (TX + RX)** — `USBFS_Endp_DataUp()` adds a 64-byte offset whenever both
  TX_EN and RX_EN are set (without `BUF_MOD`), so the TX half must be large
  enough for a full NTB: hence `64 + 2048`. Writing an NTB at `+0` here would
  land on the RX half and truncate the transfer.

### 2.4 Interface alt setting

IF5 alt 0 advertises **no** endpoints, alt 1 advertises EP5 OUT + EP7 IN. The
driver moves to alt 1 when the peer interface goes up.

`SET_INTERFACE` on IF5 calls `NCM_Bridge_Target_SetDataAlt()`, which

* drops a half-received NTB and any queued TX belonging to the previous alt;
* keeps `g_target_ncm_data_if_active = 1` **in both alts** — hosts toggle alt
  0/1 quickly and ARP/DHCP must keep flowing in that window;
* re-arms the link notification sequence on alt 1.

---

## 3. Host port — USBLIB controller

The USBLIB controller uses the 512 B PMA, addressed in 8-byte units.

### 3.1 Endpoint assignment

| EP | Dir | Type | MPS | PMA | Bound interface | Purpose |
|---|---|---|---|---|---|---|
| EP0 | IN/OUT | Control | 64 | RX `0x040`, TX `0x080` | all | control transfers |
| EP5 | IN | Interrupt | 16 | `0x0C0` (16 B) | IF0 NCM comm | link notifications |
| EP3 | OUT | Bulk | 64 | `0x110` (64 B) | IF1 NCM data | host → device NTBs |
| EP4 | IN | Bulk | 64 | `0x150` (64 B) | IF1 NCM data | device → host NTBs |

PMA budget: BTABLE `0x000–0x03F`, payload up to `0x190` = 400 of 512 bytes. No
HID, ACM or UAC endpoints exist on this controller in mode 8.

### 3.2 Packetization

PMA holds one 64-byte packet per direction, so an NTB leaves the device in
64-byte chunks (`host_tx_feed()`), each copied from `s_host_ntb_tx` into the EP4
buffer. The USBFS path chunks at 64 bytes as well, but its TX half is a 2048-byte
SRAM window, so the copy is source-to-endpoint-buffer instead of source-to-PMA.

---

## 4. Descriptors the peer sees

### 4.1 Target port (USBFS) — 202 bytes, 6 interfaces

| IF | Function | Endpoint(s) |
|---|---|---|
| 0 | HID keyboard | EP1 IN 8 B, 10 ms |
| 1 | HID relative mouse | EP2 IN 8 B, 1 ms |
| 2 | HID absolute mouse | EP3 IN 8 B, 1 ms |
| 3 | HID multimedia | EP4 IN 8 B, 10 ms |
| 4 | CDC-NCM communication (IAD function 0x0D) | EP6 IN 16 B |
| 5 | CDC-NCM data | alt 0 empty / alt 1: EP5 OUT 64 B + EP7 IN 64 B |

Layout: `USBFS_RuntimeHidConfigBase` (109 B) + `USBFS_RuntimeNcmBlock` (93 B).
The NCM block is copied in when `USB_ModeConfig_ActiveModeBridgeNcm()` is true,
so the same builder serves every other mode.

Windows needs **Microsoft OS 1.0 descriptors** before it will start this NIC:

| Item | Value |
|---|---|
| String index `0xEE` | `"MSFT100"`, `bMS_VendorCode = 0x52` |
| Vendor request | `0x52`, `wIndex = 0x0004` |
| CompatibleID feature descriptor | 40 bytes, `bFirstInterface = 4`, `CompatibleID = "WINNCM"` |

Without them `UsbNcm.sys` is not selected and the device sits at Code 56
(`CM_PROB_NEED_CLASS_CONFIG`).

### 4.2 Host port (USBLIB) — 102 bytes, 2 interfaces

| IF | Function | Endpoint(s) |
|---|---|---|
| 0 | CDC-NCM communication (IAD function 0x0D) | EP5 IN 16 B |
| 1 | CDC-NCM data | alt 0 empty / alt 1: EP3 OUT 64 B + EP4 IN 64 B |

No Microsoft OS descriptors are served on this controller: the peer is expected
to bind the class driver itself (Linux `cdc_ncm`, macOS). A Windows peer on the
host port would need an INF.

### 4.3 The NCM functional descriptor differs on purpose

| Port | `bFunctionLength` | Payload |
|---|---|---|
| target (USBFS) | **6** | `bcdNCMVersion = 0x0100`, `bmNetworkCapabilities = 0x02` (NET_ADDRESS) |
| host (USBLIB) | 14 | the same two fields plus `wMaxSegmentSize = 1514`, `wNumberMCFilters = 0`, `bNumberPowerFilters = 0` |

`UsbNcm.sys` validates `bFunctionLength == 6` and refuses to start the device
(Code 10) if the ECM-style trailing fields are present. The 14-byte form on the
host port is what the class drivers on that side accept. **Do not unify the two
blocks** — the field count belongs to the peer OS, not to the firmware's taste.

### 4.4 NCM class requests

The two controllers do **not** answer the same request codes — each set is what
its peer's driver actually sends:

| Target port (USBFS) | Code | Reply |
|---|---|---|
| `GET_NTB_PARAMETERS` | `0x80` | 28-byte parameter structure (§5.1) |
| `GET_NET_ADDRESS` | `0x81` | 6-byte MAC |
| `SET_NET_ADDRESS` | `0x82` | accepted, ignored |
| `GET_NTB_FORMAT` | `0x83` | 0 (NTB-16 only) |
| `SET_NTB_FORMAT` | `0x84` | accepted, ignored |
| `GET_NTB_INPUT_SIZE` | `0x85` | 2048 (4 bytes) |
| `SET_NTB_INPUT_SIZE` | `0x86` | accepted, ignored |
| `GET_MAX_DATAGRAM_SIZE` | `0x87` | 1514 (2 bytes) |
| `SET_MAX_DATAGRAM_SIZE` | `0x88` | accepted, ignored |
| `GET_CRC_MODE` / `SET_CRC_MODE` | `0x89` / `0x8A` | 0 (no CRC) / ignored |

| Host port (USBLIB) | Code | Reply |
|---|---|---|
| `GET_NTB_PARAMETERS` | `0x80` | the same 28-byte structure |
| `GET_NTB_INPUT_SIZE` | `0x87` | 2048 in, 2048 out (8 bytes) |
| `SET_NTB_INPUT_SIZE` | `0x86` | accepted, ignored |
| `GET_NTB_FORMAT` / `SET_NTB_FORMAT` | `0x89` / `0x88` | 0 / ignored |
| `GET_MAX_DATAGRAM_SIZE` / `SET_MAX_DATAGRAM_SIZE` | `0x8A` / `0x8B` | 1514 / ignored |
| `SET_PACKET_FILTER` | `0x43` | accepted, ignored |

The host port follows the NCM 1.0 numbering (`0x87` = input size) because Linux
`cdc_ncm` sends exactly that; the target port's numbering is what `UsbNcm.sys`
probes and has been stable against it. Both return the identical
`GET_NTB_PARAMETERS` table, so the NTB limits in §5.1 apply to either side.

Validation is asymmetric too: the target-port handlers require recipient =
interface and `wIndex == 4` and stall otherwise, while the host-port handlers
dispatch on the request code alone and ignore `wIndex`.

---

## 5. Data path

### 5.1 NTB-16 container

Single-datagram NTBs, 2048 B maximum. Header is **12** bytes, not 14:

| Offset | Field |
|---|---|
| 0–3 | `dwSignature = "NCMH"` |
| 4–5 | `wHeaderLength = 12` |
| 6–7 | `wSequence` |
| 8–9 | `wBlockLength` (total, including padding) |
| 10–11 | `wNdpIndex` |

NDP at `wNdpIndex`: `"NCM0"` signature, `wLength`, `wNextNdpIndex = 0`, then one
`(wDatagramIndex, wDatagramLength)` entry and a `(0, 0)` terminator. Frames below
60 bytes are zero-padded, and the datagram length covers that padding; payload
start is 4-byte aligned. `NCM_NDP_MAX_DATAGRAMS = 4` exists in `ncm_bridge.h`,
but `NCM_BuildNtb()` always emits a single datagram, so the effective limit today
is one frame per NTB.

Parameters advertised in `GET_NTB_PARAMETERS`:

| Field | Value |
|---|---|
| `wLength` | 28 |
| `bmNtbFormatsSupported` | 1 (16-bit NTB) |
| `dwNtbInMaxSize` / `dwNtbOutMaxSize` | 2048 / 2048 |
| `wNdpIn/OutDivisor` | 4 |
| `wNdpIn/OutPayloadRemainder` | 0 |
| `wNdpIn/OutAlignment` | 4 |
| `wNtbOutMaxDatagrams` | 16 |

### 5.2 Receive

```
target bulk OUT ──► NCM_Bridge_TargetRxDone() ──► s_target_rx_buf[2048]
                                                   │  accumulate until wBlockLength
                                                   ▼
                                            NCM_ParseNtb() ──► handle_target_frame()
host  bulk OUT ──► NCM_Bridge_HostRxPacket() ──► s_host_rx_buf[2048]  (same scheme)
```

The first chunk of a transfer must carry the `"NCMH"` signature and a
`wBlockLength` between 12 and 2048; otherwise the whole transfer is dropped
silently (no counter, no packet to the peer). Chunks are concatenated until
`wBlockLength` is reached, then the NTB is parsed once.

### 5.3 Transmit

* one frame per NTB, from a 2-deep FIFO of 1514-byte frames (`NCM_BRIDGE_QUEUE_DEPTH`);
* the NTB is built into `s_target_ntb_tx` / `s_host_ntb_tx` (2048 B each);
* USBFS streams it in 64-byte chunks with `DEF_UEP_CPY_LOAD`, because the DMA
  reads asynchronously and only the EP7 TX half is safe to hand out;
* a length that is an exact multiple of 64 is followed by a **ZLP**, otherwise
  the peer keeps waiting for the rest of the transfer and the NTB never retires;
* a full queue drops the frame (the callers for ARP/ICMP/DHCP do not retry).

### 5.4 Link notifications

Two independent state machines, with different sequences and different endpoints:

| | Target port (USBFS EP6, IF4) | Host port (USBLIB EP5, IF0) |
|---|---|---|
| Sequence | DOWN → 100 ms → UP → 50 ms → SPEED → 1000 ms → BEACON → stop | DOWN → 800 ms → UP → 50 ms → SPEED → stop |
| `NETWORK_CONNECTION` | 8 bytes, `0xA1 0x00` + value at offset 2, `wIndex = 4` | 8 bytes, `wIndex = 0` |
| `CONNECTION_SPEED_CHANGE` | 16 bytes, `0xA1 0x2A`, `wLength = 8`, 12 Mbit/s up and down | same |
| Spacing | ≥ 50 ms between packets; skipped while the endpoint is busy | ≥ 50 ms |
| Restart | alt 1, bus reset, re-enumeration (`NCM_Bridge_InitTarget`) | link reset (`NCM_Bridge_HostLinkReset`) |

Both notifications are at most 16 bytes; the 64-byte halves in the buffer table
come from the TX/RX offset rule in §2.3, not from the packet size.

---

## 6. Frame handling order

Not symmetric — the target side answers DHCP first, the host side relays
broadcasts first:

| Step | Target RX (`handle_target_frame`) | Host RX (`handle_host_frame`) |
|---|---|---|
| 1 | DHCP (local server, IF-side address) | discovery relay |
| 2 | discovery relay (IPv4 broadcast / multicast, IPv6 multicast) | ARP |
| 3 | ARP (proxy ARP for the peer) | ICMP echo |
| 4 | BLE-Eth tunnel hint, then ICMP echo | DHCP |
| 5 | DNS greeting (`handle_dns_hijack`) | DNS greeting |
| 6 | forward to host if `dst == host_ip` | forward to target if `dst == target_host_ip` |
| 7 | otherwise drop | otherwise drop |

The target-side order is load-bearing: `DHCPDISCOVER` is an IPv4 broadcast, so
relaying first would consume it and the target would fall back to `169.254.x.x`
(fixed in `5b3aff4`). The host-side order has the same latent hazard but nothing
exercises it — the host-side NIC is configured statically in the current
deployment.

MAC identity (see [ncm-bridge.md](ncm-bridge.md) §2): both NICs advertise
`02:00:00:00:00:01` in their `iMACAddress` string, so each peer adopts that as
*its own* interface MAC. Frames the firmware injects carry
`02:00:00:00:00:32` towards the host port and `02:00:00:00:00:33` towards the
target port. The two must not collide, or the peer filters our frames as
loopback.

---

## 7. Build, flash, verify

```bash
cd build
make clean && make -j8 PRODUCT=keymod-plus \
  USB_WORK_MODE=hid_ncm_bridge USB_WORK_MODE_RUNTIME_PROFILES=0
riscv-none-embed-objcopy -O binary openterface-km-firmware.elf /tmp/km.bin
```

Runtime profiles off is deliberate: it drops the ACM/ECM/UAC blocks from the
image and leaves only the mode 8 descriptor paths.

Flash with the device in ISP mode (USB ID `4348:55e0`):

```bash
wchisp flash /tmp/km.bin        # expect "Verify OK"
```

Check both ports:

```bash
lsusb -t                       # two NCM functions, one per controller
ip -br addr show | grep 192.168.20     # host side holds .1/24
ip neigh show  | grep 192.168.20       # peer at .2, MAC 02:00:00:00:00:32
```

On the target, `ipconfig /all` must show an address in `192.168.20.0/24`, an
**empty** default gateway, a 300 s lease, and the notification-driven adapter
state must be `Up` (not `Media disconnected`).

---

## 8. Gotchas worth remembering

1. **6-byte NCM functional descriptor on the target port.** Windows' `UsbNcm.sys`
   checks the length; the ECM-style 14 bytes gives Code 10. The host-port block
   keeps 14 bytes on purpose (§4.3).
2. **MS OS descriptors are required for Windows.** Without string `0xEE` plus the
   `WINNCM` CompatibleID the adapter sits at Code 56, not Code 10.
3. **NTBs above 2048 bytes are dropped silently.** `wBlockLength` is validated
   against `sizeof(s_target_rx_buf)`; oversize transfers reset the reassembly
   state and never reach a frame handler.
4. **ZLP after an exact multiple of 64.** Missing it looks like "the NTB is never
   received" while the endpoint reports success.
5. **EP7 is left bidirectional on purpose.** Clearing `UEP7_RX_EN` looks harmless
   (no handler uses that direction) but the TX half would then move from `+64` to
   `+0`, and bulk IN on this part has only ever worked with the bidirectional
   configuration. If you change it, retest both directions and shrink the array
   only afterwards.
6. **Alt 0/1 toggling is normal.** The data interface stays "active" in both alts
   by design; a partial NTB is discarded on the transition, so a frame that was
   mid-transfer during the toggle is lost.
7. **Host-port handler order.** Broadcast relay precedes DHCP there; harmless
   today because that NIC is static, but fix it before enabling DHCP on the host
   side.

History: the descriptor-transfer failure of 2026-11-19 and the Code 10 diagnosis
are in [ncm-descriptor-debug.md](ncm-descriptor-debug.md) and
[openterface-ncm-troubleshooting-2026-11-24.md](openterface-ncm-troubleshooting-2026-11-24.md).

---

## 9. Code map

| File | What lives there |
|---|---|
| `APP/usbd_desc.c` | `USBFS_RuntimeHidConfigBase`, `USBFS_RuntimeNcmBlock`, `USBFS_BuildRuntimeConfigDescriptor()` |
| `APP/USBLIB/CONFIG/usb_desc.c` | `USBD_NcmConfigDescriptor`, `USBD_StringNcmMac` |
| `APP/ch32v20x_usbfs_device.c` | USBFS endpoint init, `EP*_DatabufHD`, NCM class requests, MS OS descriptors, EP5 OUT dispatch |
| `APP/USBLIB/CONFIG/usb_prop.c` | USBLIB EP3/EP4/EP5 setup for mode 8 |
| `APP/USBLIB/CONFIG/usb_endp.c` | EP3 OUT → `NCM_Bridge_HostRxPacket`, EP4 IN completion |
| `APP/ncm_bridge.c` | NTB build/parse, both frame handlers, both notification machines, TX streaming |
| `APP/include/ncm_bridge.h` | API, `NCM_NTB_MAX_SIZE`, diagnostic counters |
| `APP/include/usb_work_mode.h` | mode 8 definition and the `USB_WORK_MODE_*` feature macros |
| `APP/include/ecm_ip_config.h` | the four addresses and the mask used by both ports |
