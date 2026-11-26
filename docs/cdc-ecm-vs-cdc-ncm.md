# CDC-ECM vs CDC-NCM: compatibility, throughput, and runtime switching

Status: **Design**
Applies to: mode 7 (`USB_WORK_MODE=hid_acm_ecm_bridge`) and mode 8
(`USB_WORK_MODE=hid_ncm_bridge`), `PRODUCT=keymod-plus`
Source tree: `f6c2892` (2026-11-26)

This note answers three questions that keep coming back:

1. Which operating systems can use each class without installing a driver?
2. Which one is faster **in this firmware**?
3. Can the dongle switch between the two ECM and NCM bridges at runtime, over
   the existing command channel?

The endpoint-level detail lives in
[usb-endpoint-map-mode7.md](usb-endpoint-map-mode7.md) and
[usb-endpoint-map-mode8.md](usb-endpoint-map-mode8.md); this document compares
them and does not repeat the per-field descriptions.

---

## 1. Summary

* **NCM is the only class that satisfies the zero-install requirement on
  Windows.** Windows has never shipped a CDC-ECM class driver; NCM has had one
  (`UsbNcm.sys`) since Windows 11 / Server 2022. On Windows, ECM means an INF.
* **ECM is still the safer bet on macOS and Linux**, where both classes are
  native. ECM's frame format is one Ethernet frame per bulk transfer, with no
  container to parse.
* **Mode 7 is slower than mode 8, not faster.** Its ECM data endpoints are 32-byte
  packets on both ports, while mode 8's NCM endpoints are 64-byte. At full speed
  that halves the bus ceiling (~0.6 MB/s vs ~1.2 MB/s) before any protocol
  overhead. NCM's aggregation advantage is *not* used: this firmware emits one
  datagram per NTB.
* **Runtime 7↔8 switching is not shippable today.** The command (`0x30`), the
  flash record and the reboot path all exist, but four firmware structures are
  compile-time-only: the runtime-support gate, the compiled-mode priority rule,
  the host-port descriptor table, and the endpoint initialisation. RAM also has
  only ~1.8 KB of headroom, and the two bridges' buffers do not fit side by side.

---

## 2. The two classes in one paragraph each

**CDC-ECM** (subclass `06h`) presents a control interface plus a data interface
with an alternate setting. In the active alt setting every bulk transfer carries
exactly one Ethernet frame; there is no header, no length field and no
aggregation. Link state arrives as `NETWORK_CONNECTION` /
`CONNECTION_SPEED_CHANGE` notifications on the control interface's interrupt IN
endpoint, and the only class requests are `SET_ETHERNET_PACKET_FILTER` (`0x43`)
and `GET_ETHERNET_STATISTIC` (`0x44`).

**CDC-NCM** (subclass `0Dh`) keeps the same interface shape but wraps frames in
an **NTB** — a container with an NTH16 header, a datagram pointer table (NDP16)
and 1..N Ethernet frames, all 4-byte aligned. The host negotiates limits through
class requests (`GET_NTB_PARAMETERS` `0x80` … `GET_CRC_MODE` `0x89`), and the
data interface must move to alt 1 before traffic flows. Aggregation is NCM's
whole point: many frames per transfer, fewer USB transactions, lower CPU cost at
high speed.

In this tree: mode 7 runs ECM on both ports, mode 8 runs NCM on both ports.
"Both ports" means the host-facing controller (USBLIB, 512 B PMA) and the
target-facing controller (USBFS, SRAM DMA).

---

## 3. Compatibility matrix

| Platform | CDC-ECM | CDC-NCM | Notes |
|---|---|---|---|
| Windows 11 (21H2+), Server 2022 | ✗ no class driver | ✓ in-box `UsbNcm.sys` | needs MS OS 1.0 descriptors (`MSFT100` + `WINNCM`) or an INF; verified on this project's target port |
| Windows 10 | ✗ no class driver | ⚠ undocumented | `UsbNcm.sys` may be present on late builds, but NCM is not in Microsoft's supported-class table; treat as untested |
| macOS | ✓ native | ✓ native | both bind without user action; NCM arrived later than ECM in the OS's lifetime |
| Linux | ✓ `cdc_ether` | ✓ `cdc_ncm` (2.6.35+) | both mainline; `cdc_ncm` may name the NIC `wwan0` depending on quirks |
| Android / ChromeOS | ✓ kernel | ✓ kernel | untested here |

Three consequences worth spelling out:

* **The Windows gap is structural, not historical.** Windows' network stack binds
  NICs through NDIS miniports, so Microsoft implemented its own NCM miniport
  instead of a generic CDC-ECM one. There is no ECM INF in any Windows SKU.
* **"In-box NCM" is not "bind automatically".** Without the MS OS descriptor the
  target port sits at Code 56 (`CM_PROB_NEED_CLASS_CONFIG`); with a 14-byte NCM
  functional descriptor it sits at Code 10. Both were hit and fixed on this
  project — see [usb-endpoint-map-mode8.md](usb-endpoint-map-mode8.md) §4.
* **The host port still carries only Linux-grade NCM.** Its descriptor block is
  14 bytes and it serves no MS OS descriptor, which Linux accepts and Windows
  will not. A Windows *host* is therefore still an open item, not a supported
  configuration.

---

## 4. Endpoint comparison

Both modes use the same two controllers. What changes is which endpoints each
controller owns, how big they are, and what kind of container crosses them.

### 4.1 Host port — USBLIB controller (512 B PMA)

| Role | Mode 7 (ECM) | Mode 8 (NCM) |
|---|---|---|
| Control | EP0, 64 B, PMA `0x040`/`0x080` | EP0, 64 B, PMA `0x040`/`0x080` |
| Serial (CH9329) | EP1 IN 16 B `0x0C0`, EP5 OUT 64 B `0x0D0`, EP3 IN 64 B `0x110` | — no ACM in this mode |
| Link notification | EP2 IN 16 B, PMA `0x150`, IF2 | EP5 IN 16 B, PMA `0x0C0`, IF0 |
| Data OUT (host → device) | EP6 OUT 32 B, PMA `0x180`, IF3 alt 1 | EP3 OUT 64 B, PMA `0x110`, IF1 alt 1 |
| Data IN (device → host) | EP6 IN 32 B, PMA `0x1A0`, IF3 alt 1 | EP4 IN 64 B, PMA `0x150`, IF1 alt 1 |
| PMA used | `0x1C0` = 448 B | `0x190` = 400 B |
| Interfaces / descriptor | 4 IF, 154 B | 2 IF, 102 B |
| MS OS descriptors | none | none (Linux/macOS peer assumed) |

Mode 7's host port is the busier of the two: three ACM endpoints coexist with the
ECM pair, so ECM data was squeezed into 32-byte slots to stay inside the 512-byte
PMA. Mode 8 has no ACM on this controller, which is exactly why its NCM data
endpoints can be 64 bytes.

### 4.2 Target port — USBFS controller (SRAM DMA, no PMA limit)

| Role | Mode 7 (ECM) | Mode 8 (NCM) |
|---|---|---|
| HID keyboard | EP1 IN, 8 B MPS, 64 B buffer, IF0 | EP1 IN, 8 B MPS, 64 B buffer, IF0 |
| HID relative mouse | EP2 IN, 8 B, IF1 | EP2 IN, 8 B, IF1 |
| HID absolute mouse | EP3 IN, 8 B, IF2 | EP3 IN, 8 B, IF2 |
| HID multimedia | EP4 IN, 8 B, IF3 | EP4 IN, 8 B, IF3 |
| Link notification | EP5 IN 16 B, `EP5_DatabufHD[128]`, IF4 | EP6 IN 16 B, `EP6_DatabufHD[64+64]`, IF4 |
| Data OUT (target → device) | EP7 OUT 32 B, buffer `+0`, IF5 alt 1 | EP5 OUT 64 B, `EP5_DatabufHD[2048]`, IF5 alt 1 |
| Data IN (device → target) | EP7 IN 32 B, buffer `+64`, IF5 alt 1 | EP7 IN 64 B, `EP7_DatabufHD[64+2048]`, IF5 alt 1 |
| Interfaces / descriptor | 6 IF, 188 B | 6 IF, 202 B |
| MS OS descriptors | none (macOS/Linux peer) | **required** for Windows (`WINNCM`, `bFirstInterface = 4`) |

Two asymmetries are easy to miss:

* **The target side is not PMA-limited in either mode**, yet mode 7 still runs
  32-byte packets. That size is inherited from the ECM bridge's host-side PMA
  budget and its 32-byte fragmentation/ZLP boundaries, and applies to the USBFS
  direction as well. Raising it to 64 B on the target side alone would be a
  cheap throughput win for mode 7 — but it is not what the code does today.
* **Mode 8 needs 2 KB windows; mode 7 needs 128 B.** ECM's buffer is sized for one
  32-byte packet pair. NCM must be able to hold a whole NTB (`NCM_NTB_MAX_SIZE` =
  2048), and EP7's TX half sits at `+64` because the endpoint is left
  bidirectional, hence `64 + 2048`.

### 4.3 Class requests and containers

| | Mode 7 (ECM) | Mode 8 (NCM) |
|---|---|---|
| Class requests | `0x43` set packet filter, `0x44` get statistic | `0x80`–`0x8A`, two different numberings per port |
| Wire format | raw Ethernet frame, 14 B header included | NTH16 12 B + NDP16 16 B + datagram(s), 4 B aligned |
| Max transfer | 1514 B frame, split across MPS-sized packets | 2048 B NTB (single datagram today) |
| ZLP | none required | required when the NTB length is a multiple of 64 |
| Alt setting | data IF alt 0 idle / alt 1 active | same, plus NTB parameter negotiation |

### 4.4 What that does to throughput

Full-speed bulk allows at most 19 packets per 1 ms frame:

| | Mode 7 | Mode 8 |
|---|---|---|
| Bulk MPS | 32 B | 64 B |
| Bus ceiling (one direction) | 19 × 32 B/ms ≈ **0.61 MB/s** | 19 × 64 B/ms ≈ **1.22 MB/s** |
| Header per frame | 0 B | 28 B (+ padding), ≈1.9 % on a 1514 B frame |
| Aggregation | none | none in practice (one datagram per NTB) |

These are bus ceilings, not measurements: no `iperf` run has been recorded for
either mode. For the traffic this bridge actually carries — DHCP, DNS, ARP,
small HTTP probes — latency dominates and neither mode is bandwidth-bound. The
32-vs-64 difference matters only for file transfer through the portal, where
mode 8 is roughly twice as fast for the same USB configuration.

---

## 5. What the firmware already has for switching

The mode-switch machinery is complete on the control plane:

| Item | Detail |
|---|---|
| Command | `CMD_USB_WORK_MODE = 0x30` |
| Sub-commands | `0x00` query, `0x01` write `(mode, flags)`, `0x02` clear record, `0x03` save-and-reboot |
| Reply | 12 bytes: `status`, `compiled`, `active`, `stored`, `status_flags`, `flags` L/H, `capability` ×4, reserved |
| Transports | CH9329 frame over host ACM or BLE; **target HID feature report `0x30`** (readable and writable) |
| Persistence | one flash page, magic + version + CRC; discarded when corrupt |
| Activation | reboot; USB descriptors are cached at enumeration and cannot change in place |

The target-side HID path is what makes a bad switch recoverable at all: if the
network disappears, the operator can still write mode 8 back through the HID
report. A network-only recovery path would brick the link.

---

## 6. Why 7↔8 does not work today

Four firmware decisions are compile-time, and one is a hard resource limit.

| # | Where | What blocks the switch |
|---|---|---|
| 1 | `APP/usb_mode_config.c`, `mode_is_runtime_supported()` | with `USB_WORK_MODE_RUNTIME_PROFILES=0` only the compiled mode is accepted, so `USB_ModeConfig_Write(7, …)` returns `STATUS_ERR_PARAM` |
| 2 | `APP/usb_mode_config.c`, `USB_ModeConfig_Init()` | *compiled-mode priority*: a stored mode different from `USB_WORK_MODE` is treated as stale, the record is erased and the device silently boots the compiled mode |
| 3 | `APP/USBLIB/CONFIG/usb_desc.c`, `USBD_GetActiveConfigDescriptorData()` | the `RUNTIME_PROFILES` branch has cases for ACM, ACM+ECM, ECM+UAC, ECM and UAC — but no NCM case; NCM is only reachable from the `#else` (static descriptor) path |
| 4 | `APP/USBLIB/CONFIG/usb_prop.c` | ECM and NCM endpoint setup are `#if`/`#else` alternatives on the same endpoint numbers (EP3/EP4/EP5) with overlapping PMA addresses; nothing reprograms PMA from `active_mode` at re-enumeration |
| 5 | `APP/USBLIB/CONFIG/usb_endp.c` | the NCM branches are guarded by `USB_WORK_MODE_COMPILES_BRIDGE_NCM`, the ECM ones by `..._BRIDGE_ECM`; with runtime profiles both compile, and the callbacks do not dispatch on the active mode |
| 6 | `tools/build_firmware.sh:201` | hardcodes `-DUSB_WORK_MODE_RUNTIME_PROFILES=0`, so the mode 8 image can never carry the other profile even if 1–5 were fixed |

### Resource math

| Resource | Value |
|---|---|
| Flash | 440 KB total, image 226,196 B text → **two images do not fit** (≈452 KB) |
| RAM | 64 KB total: data 1,068 + bss 60,620 = 61,688 B, stack 2,048 B → **≈1.8 KB free** |
| NCM static buffers | 4 × 2048 B NTB buffers + two 2-deep 1514 B queues ≈ **14 KB** |
| ECM static queues | comparable per-direction queues ≈ **7.7 KB** |

Compiling both bridges side by side overshoots the free RAM by roughly 6 KB
before any descriptor table is added. A union of the frame buffers and queues is
a prerequisite, not an optimisation.

---

## 7. Recommendation

**Ship mode 8. Keep mode 7 as a separate build. Do not expose 7↔8 switching.**

1. **Default: mode 8.** It is the only configuration that works on a Windows
   target with zero installation, and it is twice as fast on the same hardware.
2. **Mode 7 stays as the compatibility build** for macOS and Linux hosts, where
   ECM is native and the 32-byte ceiling is acceptable for console traffic. It
   needs no maintenance beyond keeping its descriptor in sync.
3. **Do not implement runtime switching unless a product requirement forces it.**
   If it is forced, the minimum work is: (a) union the two bridges' buffers,
   (b) let a runtime build honour `stored != compiled`, (c) add the host NCM
   branch to the runtime descriptor table, (d) dispatch endpoint init and EP
   callbacks on `active_mode`, (e) stop hardcoding `RUNTIME_PROFILES=0` for this
   product. The regression matrix must include 7→8→7 on Windows 11, macOS and
   Linux — and the 8→7 direction must be exercised with the network already
   down, because that is the state a Windows target lands in.
4. **If a Windows host must be supported**, fix the host-port NCM descriptor
   first: 6-byte NCM functional descriptor plus MS OS `WINNCM`, mirroring the
   target port. That is a descriptor-only change, but it must be re-verified
   against Linux `cdc_ncm`, which is the only peer currently proven on that port.

---

## 8. References

* [usb-endpoint-map-mode7.md](usb-endpoint-map-mode7.md) — mode 7 endpoints, PMA
  map, target ECM buffer layout
* [usb-endpoint-map-mode8.md](usb-endpoint-map-mode8.md) — mode 8 endpoints,
  descriptors, NTB format, notification state machines
* [ncm-bridge.md](ncm-bridge.md) — canonical NCM behaviour (DHCP, DNS greeting,
  forwarding rules)
* [controllable-usb-endpoint-modes.md](controllable-usb-endpoint-modes.md) — mode
  model and the `0x30` command history
* Microsoft, *Supported USB classes* — CDC NCM is listed for Windows 11 /
  Server 2022; CDC ECM has no row
* Microsoft, `NCM-Driver-for-Windows` — the in-box NCM miniport's source
* Linux kernel `drivers/net/usb/cdc_ncm.c` (since 2.6.35) and `cdc_ether.c`
