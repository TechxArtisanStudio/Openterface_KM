# Generic IP-over-BLE tunnel design

- Branch: `fix/stability-p0-p1`
- Baseline: `main` @ `19ef5c8`
- Applies to: KeyMod Mini (mode 4), KeyMod Plus / KVM-GO (mode 7), MiniKVM (modes with Target ECM)
- Status: technically complete, but **recommended to be parked**. Once the controller was confirmed to be a phone (§1 verification: iOS and Android both rely on an in-app SSH client plus the BLE-Eth TCP proxy, neither has any raw-IP foundation), this design does not solve the current pain. §11 gives the alternative priorities.

---

## 1. Investigation: no IP tunnel has ever existed

Pin the facts down first, so the rest of the design is not built on a false premise.

**Search scope and results:**

| Location | Searched for | Result |
|------|----------|------|
| `APP/`, `main.c` | `utun` / `tuntap` / `forward` / `route` / `nat` / `conntrack` / IP fragment reassembly | No hits. The firmware has no IP forwarding or virtual-interface concept of any kind |
| `docs/ble-eth-ssh-usage.md:48` | protocol capability boundary | Already stated in the document itself: `The BLE-Eth tunnel carries TCP only. It does not forward ICMP.` |
| `tools/ble_eth_tunnel.py` (1870 lines), `tools/ssh_through_ble_eth.py` (627 lines) | host-side tunnel implementation | Contains `BleTcpProxy` (`ble_eth_tunnel.py:1663`), which is an **application-layer proxy between a TCP socket and BLE-Eth**: it listens on a local port and pushes the byte stream into the tunnel. It never touches the IP layer, creates no interface, and does not modify the routing table |
| `keymod-test` (PyQt side) | tunnel / route / interface | None |
| `Openterface_KeyCmd_iOS` | `NEPacketTunnelProvider`, Network Extension target, `com.apple.developer.networking.networkextension` entitlement | None of the three exist. `KeyCmd/KeyCmd.entitlements` only has `com.apple.security.app-sandbox` and user-selected files |
| `Openterface_KeyCmd_Android` | `VpnService`, `BIND_VPN_SERVICE`, `ParcelFileDescriptor`, `tun2socks` | None of them exist. `AndroidManifest.xml` declares a single `BluetoothService`; no VPN service, no VPN permission |

**But the phones have had usable SSH for a long time — it simply never goes through the IP layer:**

| | iOS | Android |
|---|-----|---------|
| SSH client | **libssh2** (`import CSSH`, from the `Libssh2Prebuild` XCFramework) [SSHClient.swift:3,89](file:///Users/txa/project/Openterface_KeyCmd_iOS/KeyCmd/Services/SSH/SSHClient.swift#L89) | **JSch** `com.github.mwiede:jsch:0.2.17` (`app/build.gradle:204`, `SshClient.java`) |
| How it is grafted onto BLE | `socketpair(AF_UNIX, SOCK_STREAM)` bridge, so libssh2 believes it is reading and writing an ordinary fd ([SSHClient.swift:385-407](file:///Users/txa/project/Openterface_KeyCmd_iOS/KeyCmd/Services/SSH/SSHClient.swift#L399)) | `session.setSocketFactory(...)` plus a custom `BleEthSocket extends Socket` whose `connect()` throws |
| Actual byte path | `BleEthTransport` builds `CONNECT 0x10` + `DATA 0x11` fragment frames | The same protocol, Java version `BleEthTransport.java` |

In other words: **phone SSH = an in-app SSH client plus the BLE-Eth TCP proxy, entirely independent of the IP layer.** That is exactly why "it worked on 0.0.4 / 0.0.5": `0b666f8` (2026-05-22, support ble-ecm path) already introduced `0x10/0x11/0x1F`, and `a11973c` (2026-06-05, add SSH over BLE-Eth functionality) and `5211f03` (2026-06-18, `SET_DEFAULT 0x20`) both predate tag `v0.0.4` (33f4ed5, 2026-07-25), as does `v0.0.5` (321d4e4, 2026-09-24). The tunnel's 4-tuple matching logic is **field-for-field identical** between `v0.0.4:ble_eth_protocol.c:1282` and today's `:1714` — the firmware semantics phone SSH depends on have not changed in a year.

**So "we did this before" refers to two things**: the `BleTcpProxy` at `tools/ble_eth_tunnel.py:1663` (a PC-side proxy) and the in-app SSH clients in the two phone apps (app-side proxies). Both are **per-protocol, hand-written adaptation at the application layer**; neither touches the IP layer, creates an interface, or modifies the routing table. A generic IP-over-BLE tunnel is a different thing, and it genuinely has never been done.

The **incremental value** of this design therefore narrows to a single sentence: it lets programs with **no built-in adaptation** (`ping`, UDP, mDNS, any third-party app) run over Bluetooth without code changes. **SSH, which is already adapted, gains nothing at all.**

---

## 2. Goals and non-goals

### Goals

1. The BLE link becomes the device's **third IP path** (the existing two are Host USB ECM and Target USB ECM), with a priority that does not depend on whether USB is plugged in.
2. On the host, `ping <target_host_ip>` and `ssh <target_host_ip>` work directly, with no `-p` and no proxy configuration.
3. Off by default, one command to enable, one to disable; when disabled, behaviour is byte-for-byte identical to today's firmware.
4. Do not break HID keyboard/mouse real-time behaviour (that is this product's main job; the tunnel is an add-on).

### Non-goals

1. No bandwidth ambitions. BLE cannot do high throughput and should not replace USB ECM for large files.
2. IPv6, multi-hop routing, TLS offload, DHCP server over BLE — none of it.
3. Phase 1 does not support iOS (prerequisite in §8: the entitlement is missing).
4. Do not fix the Host ECM ↔ Target ECM L2 bridge (§5.5 explains why it is unrelated to this design and why it is a separate bug).

---

## 3. Existing assets that can be reused

The design needs no new low-level pipe; every hook already exists and has run on hardware:

| Capability | Location | Use |
|------|------|------|
| Inject a raw Ethernet frame into the Target link | `ECM_Bridge_SendRawFrame()` [APP/ecm_bridge.c:2592](file:///Users/txa/project/Openterface_KM/APP/ecm_bridge.c#L2592) | The egress of IP forwarding. BLE-Eth already uses it: [`ecm_bridge_tx_handler()` APP/peripheral.c:537](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L537) |
| Receive Ethernet frames from the Target side | `handle_target_frame()` [APP/ecm_bridge.c:2651](file:///Users/txa/project/Openterface_KM/APP/ecm_bridge.c#L2651) | The ingress of IP forwarding; at `:2686` it already hands frames to `BLE_Eth_HandleTargetFrame()` |
| Extract IPv4 from an Ethernet frame and dispatch by protocol | `BLE_Eth_HandleTargetFrame()` [APP/ble_eth_protocol.c:1714](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L1714) | The natural generalisation point: today the logic is "parse the IPv4 header → accept only proto==6 (TCP), return false otherwise"; only the "TCP only" step needs to become "hand it to the IP router" |
| Frame reassembly + fragmentation + ACK state machine | `g_reassembly` (1536 B), `BLE_Eth_PushTargetData()` [APP/ble_eth_protocol.c:1238](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L1238) | Carries IP packets; no new transport layer needed |
| BLE write-frame routing allow-list | `Peripheral_IsBleEthCommandByte()` [APP/peripheral.c:239](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L239) | Any new control command must be registered here, otherwise the CH9329/HID path consumes it a second time (the cause of `STABILITY_FIX_PLAN` P0-2) |
| Target-link ARP / GARP | `handle_arp_frame()` [APP/ecm_bridge.c:293](file:///Users/txa/project/Openterface_KM/APP/ecm_bridge.c#L293), `g_target_garp_sent` | Forwarding needs the MAC of `target_host_ip`; the mechanism already exists |
| ICMP request/reply (both sides) | `handle_icmp_packet()` (host side), `handle_target_icmp_packet()` [APP/ecm_bridge.c:967](file:///Users/txa/project/Openterface_KM/APP/ecm_bridge.c#L967) | Once the tunnel is up, ICMP flows through the forwarding path naturally; these two handlers only stand in when "the destination is my own IP", so they do not conflict |
| ECM IP config page | `ECM_IPConfig` [APP/include/ecm_ip_config.h:29-42](file:///Users/txa/project/Openterface_KM/APP/include/ecm_ip_config.h#L29) | Supplies ready-made addresses such as `dev_ip` / `target_dev_ip`; the SNAT egress takes `target_dev_ip` directly |

---

## 4. Hard constraints (all verified in code or build artefacts)

| Constraint | Value | Location | Impact on the design |
|------|------|------|--------------|
| **Link-layer packet limit** | `BLE_BUFF_MAX_LEN = 64` ⇒ **ATT_MTU at most 60** | [HAL/include/config.h:115](file:///Users/txa/project/Openterface_KM/HAL/include/config.h#L115), `ATT_MTU = BufMaxLen-4` [LIB/wchble.h:91](file:///Users/txa/project/Openterface_KM/LIB/wchble.h#L91), `build/makefile:256` | **This is the real first bottleneck.** MTU 60 ⇒ at most **57 B** of net data per notification, and the apps have been written against that number for a long time (Android `SAFE_BLE_STREAM_CHUNK = 57`) |
| **Packets per connection event** | `BLE_TX_NUM_EVENT = 1` | [HAL/include/config.h:121](file:///Users/txa/project/Openterface_KM/HAL/include/config.h#L121), `build/makefile:259` | 1 packet / interval, not "several". The §4 throughput budget must be computed on 1 |
| Controller buffers | `BLE_BUFF_NUM = 5` | [HAL/include/config.h:118](file:///Users/txa/project/Openterface_KM/HAL/include/config.h#L118) | Together with `BufMaxLen` this determines the `BLE_MEMHEAP_SIZE` requirement |
| Notification limit in code | 180 B — **unreachable under the current configuration** | `BLE_ETH_SAFE_NOTIFY_LEN` [APP/peripheral.c:46](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L46), `max_notify_len = min(peripheralMTU-3, 180)` ([APP/peripheral.c:697-707](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L697)) | `peripheralMTU-3 ≤ 57`, so `min()` is always 57. The 180 is a **reachability ceiling, not the status quo** |
| Per-fragment payload the protocol allows | 246 B (`249-3`) | `BLE_ETH_MAX_PAYLOAD_LEN` [APP/include/ble_eth_protocol.h:41](file:///Users/txa/project/Openterface_KM/APP/include/ble_eth_protocol.h#L41), `BLE_ETH_FRAG_HEADER_LEN=3` | At MTU 60, one 246 B fragment costs **5 notifications**; `BLE_Eth_GetNextTxChunk`/`ConsumeTxChunk` resume with a retained remainder, so no data is lost |
| TX notification queue | 8 slots × 255 B | `BLE_ETH_TX_QUEUE_DEPTH` [APP/ble_eth_protocol.c:37](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L37) | A burst overflows it; visible in the `tx_queue_full_drop` counter |
| RX reassembly buffer | **1536 B, and only one in the whole system** | `g_reassembly.data` [APP/include/ble_eth_protocol.h:316](file:///Users/txa/project/Openterface_KM/APP/include/ble_eth_protocol.h#L316) | A single IP packet must be ≤ 6×246=1476 B; **and the IP channel competes with the TCP tunnel for the same reassembler** (§6.2) |
| Fragment counter is only 4 bit | ≤ 15 fragments | `BLE_ETH_FRAG_COUNT_MASK 0x0FU` | Hard ceiling of 3690 B per message, which is enough |
| TCP/tunnel connection slots | **2** | `BLE_ETH_MAX_CONNECTIONS` [APP/include/ble_eth_protocol.h:103](file:///Users/txa/project/Openterface_KM/APP/include/ble_eth_protocol.h#L103) | Cannot take the "one slot per connection" route again, see §6.1 |
| ECM queue depth per direction | **2** frames, frame buffer 1514 B | `ECM_BRIDGE_QUEUE_DEPTH` / `ECM_BRIDGE_MTU_FRAME_SIZE` [APP/ecm_bridge.c:23-24](file:///Users/txa/project/Openterface_KM/APP/ecm_bridge.c#L23) | Forwarding must be "drop when the queue is full", never block the main loop |
| Free RAM | **about 10.76 KB** (`_end`→`_heap_end` gap) | `docs/internal-memory-usage.md:86`; the current keymod-mini report is RAM 77.95% | New statics must be budgeted byte by byte, see §7 |
| BLE conn interval (requested) | 10 × 1.25 ms = **12.5 ms** | `DEFAULT_DESIRED_MIN/MAX_CONN_INTERVAL` [APP/peripheral.c:65-68](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L65) | The central may refuse it; the real interval must be measured in Phase 0 |
| App write path | iOS: 128 B chunks + 10 ms spacing + all `.withResponse` ([BLEManager.swift:19,424-477](file:///Users/txa/project/Openterface_KeyCmd_iOS/KeyCmd/Managers/BLEManager.swift#L19)); Android: 57 B chunks + 10 ms + `WRITE_TYPE_DEFAULT` | Both comments state "the firmware needs a BLE-layer ACK before it forwards" | **The uplink is serialised by write-with-response into one round trip at a time** — this is the biggest win available at the app layer and is unrelated to the firmware |

**Throughput budget (current real configuration, not the ideal one):** MTU 60 ⇒ at most 57 B of net data per notification; `BLE_TX_NUM_EVENT = 1` ⇒ one packet per 12.5 ms ⇒ **raw one-way ceiling 57 B / 12.5 ms ≈ 4.56 kB/s ≈ 36.5 kbit/s** (before protocol headers and ACKs).

Projected onto BLE-Eth frames: one 246 B fragment = a 255 B frame = 5 notifications ≈ **62.5 ms**, and today every inbound fragment also returns a DATA_ACK ([APP/ble_eth_protocol.c:590](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L590)), which occupies an outbound slot ⇒ bidirectional traffic discounts the effective throughput further. Superimposed on "the app uses write-with-response", this fully explains why phone SSH connects but feels sticky.

**§4 therefore cannot stand on its own**: unless `BLE_BUFF_MAX_LEN` / `BLE_TX_NUM_EVENT` are addressed first, both an IP tunnel and the existing TCP proxy hit the same 36.5 kbit/s wall. **That work should be prioritised above this design** (see §11.5).

---

## 5. Key design decisions

### 5.1 Carry raw IPv4, not Ethernet frames

The temptation is to move the Target ECM Ethernet frames onto BLE verbatim (a "real bridge"). **No**, because:

1. Once L2 is passed through, the host has to ARP for the Target subnet. But on the host side the device only answers ARP for **its own IP** — `handle_arp_frame()` [APP/ecm_bridge.c:293](file:///Users/txa/project/Openterface_KM/APP/ecm_bridge.c#L293) matches only `ecm_dev_ip` / `ecm_target_dev_ip`. Passing L2 through means implementing proxy ARP (answering for the whole subnet), which raises complexity a notch immediately.
2. `utun` is a point-to-point device; the OS does no ARP at the tun layer and hands it IP packets directly. **Raw IPv4 makes the host-side ARP problem disappear entirely.**
3. It saves the 14 B Ethernet header and 4 B FCS — 18 B of payload free on every fragment.
4. The MAC addresses inside the frame have no meaning on the BLE link; passing them through injects irrelevant state into the protocol.

Only IPv4 packets (without the link layer) travel over BLE; both ends do their own MAC resolution.

### 5.2 Tunnel MTU = 576

Not an arbitrary choice:

- `576` is the minimum datagram every IPv4 implementation must be able to reassemble per RFC 791, so no target machine will refuse it.
- Keep the fragment size **at the protocol's existing 246 B**; do not shrink it to get "one fragment per notification". At MTU 60 a `246 B` fragment already takes 5 notifications, and shrinking to 48 B keeps the notification count identical while **multiplying the per-fragment DATA_ACK overhead by five** (ACK logic at [APP/ble_eth_protocol.c:590](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L590)). `ceil(576/246) = 3` fragments, inside `BLE_Eth_PushTargetData()`'s 6-fragment ceiling ([APP/ble_eth_protocol.c:1246](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L1246)) and inside the 1536 B reassembly buffer — **neither limit needs changing**.
- The cost of one 576 B packet on the air: 3 fragments × 5 notifications = 15 notifications, at 1 packet / 12.5 ms ≈ **188 ms one way**. That settles it: the tunnel can carry small request/response traffic, not throughput.
- On the TCP side, clamp MSS to `536` (576 − 20 IP − 20 TCP) so `ssh` never produces oversized packets at the source and 99% of traffic is compliant by construction.
- Oversized UDP packets: **drop, do not fragment**, and in Phase 3 return `ICMP Fragmentation Needed`. Phase 1 drops and counts.

### 5.3 Separate subnet on the BLE side + SNAT out through the Target link

The BLE tunnel uses **its own** subnet and never reuses `192.168.30.0/24` (that is the ECM config page's semantic space, and by default both host and target sides sit in the same /24, so mixing them is guaranteed to fight the physical ECM adapter's routes).

```
Defaults (all overridable via 0x25):
  tunnel subnet   10.88.0.0/24
  host-side utun  10.88.0.2   ← "my own address" from the OS's point of view
  device-side gw  10.88.0.1   ← the utun's default / next hop
  SNAT egress IP  = target_dev_ip (default 192.168.30.5), taken from ECM_IPConfig
  route injection route add -net 192.168.30.0/24 10.88.0.1   (this route only, never default)
```

Only two forwarding rules:

- **BLE → Target**: for packets whose source IP is in `10.88.0.0/24` → resolve the MAC of `target_host_ip` via the Target link's ARP → rewrite `src_ip = target_dev_ip`, recompute the IP checksum → `ECM_Bridge_SendRawFrame()`.
- **Target → BLE**: inside `handle_target_frame()`, packets with `dst_ip == target_dev_ip` that hit conntrack → restore `dst_ip` from the table → push into the BLE channel.

SNAT rather than NAT64-style route advertisement, because **the Target machine needs no configuration and no return route** — from its point of view every connection comes from the neighbouring `192.168.30.5`. This is the key to "zero intrusion on the target side" and the only realistic shape for the KVM scenario (that machine belongs to someone else; you cannot ask them to install a driver and add routes).

### 5.4 The data plane reuses DATA (`0x11`), with `conn_id = 0xFE` marking the IP channel

`conn_id` in the fragment header is a standalone byte, and connection slots only run `0..1` (`BLE_ETH_MAX_CONNECTIONS`), so `0xFE` is a naturally reserved "virtual channel number".

Why this rather than a new command byte or a spare bit in `frag_flags`:

1. Frame construction, validation, ACK, reassembly and back-pressure **need not change by a single line** — `BLE_Eth_PushTargetData()` and the RX reassembler only look at `conn_id`.
2. The only change is one branch at dispatch: `if (conn_id == BLE_ETH_IP_CONN_ID) → ip_router_input()`, bypassing `g_connections[]`.
3. `frag_flags` bits 5/4 are free, but occupying a bit an older host cannot recognise makes `FrameParser` behaviour unpredictable; `conn_id` is a field the host already reads, so the change surface is smaller and reversible.
4. Every path that validates `conn_id < BLE_ETH_MAX_CONNECTIONS` (`CONNECT(0x10)` / `DISCONNECT(0x12)` / `ALLOC`) only needs a `!= 0xFE` guard (the same pattern as the already-landed `!is_icmp` guard, see [APP/ble_eth_protocol.c:502](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L502)).

On the host, `DataReassembler` / `FrameParser` in `tools/ble_eth_tunnel.py` must relax their `conn_id` validation to match; that is Phase 2 work.

### 5.5 Control plane: add `0x25 SET_IP_MODE` / `0xA5`

Command-byte collisions were checked: CH9329 occupies `0x22/0x23/0x26/0x27` (`CMD_BLE_SECURITY`, `CMD_BLE_SET_NAME`, `CMD_SET_ECM_IP_CONFIG`, `CMD_GET_ECM_IP_CONFIG`, see [APP/include/usbd_composite_km.h:51-57](file:///Users/txa/project/Openterface_KM/APP/include/usbd_composite_km.h#L51)), and BLE-Eth already uses `0x10-0x12/0x1E/0x1F/0x20-0x24`. **`0x25` is clean in both address spaces**, as is `0xA5`. Do not touch `0x26/0x27` again — that is the legacy collision zone.

```
0x25 SET_IP_MODE request payload:
  [0]     enable            0=off, 1=on
  [1-4]   tunnel_host_ip    (ignored when off; default 10.88.0.2)
  [5-8]   tunnel_gw_ip      (default 10.88.0.1)
  [9-12]  tunnel_subnet     (default 255.255.255.0)
  [13-14] mtu               big-endian, accepts only <= 576, anything else is PARAM_ERR
  [15]    forward_icmp      0=TCP/UDP only, 1=ICMP too

0xA5 response payload (8 B):
  [0] status  [1] active  [2] mtu  [3] fwd_icmp
  [4-5] fwd_pkts_out (BE16)  [6-7] fwd_pkts_in (BE16)
```

`enable` must be a **runtime switch and must never write flash** (`STABILITY_FIX_PLAN` already lists "meaningless flash erase on config write" as a problem). Tunnel state is volatile; power loss means off.

### 5.6 conntrack: 8 static entries, not a general-purpose stack

```c
typedef struct {
    uint32_t orig_dst_ip;   /* Target IP as seen by the host */
    uint32_t orig_src_ip;   /* source IP on the utun side */
    uint16_t orig_src_port;
    uint16_t nat_src_port;  /* source port allocated by SNAT */
    uint8_t  proto;         /* 6 / 17 / 1 */
    uint16_t icmp_id;       /* reused when proto==1 */
    uint32_t last_ms;       /* aged out after 30 s idle */
} ip_ct_entry_t;            /* 24 B × 8 = 192 B */
```

Only symmetric 5-tuple matching plus port recycling; no state tracking, no TCP state machine (that is `ble_eth_conn_t`'s job and is not needed here). Eight entries are enough for "one Mac running three ssh sessions, ping and scp at once"; when full, new flows are rejected and counted, never silently dropped.

---

## 6. Architecture and data path

```
            ┌──────────────────────── host (Mac / Linux / Android) ──────────────────────┐
            │  apps (ssh / ping / scp / anything)                                        │
            │        │ IP routing: 192.168.30.0/24 → 10.88.0.1                            │
            │  utun / VpnService  (10.88.0.2)                                            │
            │        │ raw IPv4 packets                                                  │
            │  tools/ble_ip_tunnel.py or the Android VPN service                          │
            └────────┼──────────────────────────────────────────────────────────────────┘
                     │  BLE GATT: FFF2 write / FFF1 notify
                     │  BLE-Eth DATA(0x11), conn_id=0xFE, fragment<=171B, MTU 576
   ══════════════════╪═══════════════════════════════════════════════════════════════════
                     │  APP/ble_eth_protocol.c
        RX: reassembly done ─┤→ conn_id==0xFE? ─→ ip_router_input() ─→ conntrack ─→ build Ethernet frame
                      │                          │                      └→ ECM_Bridge_SendRawFrame()
        TX: ip_router_output() ←──────────────────┤                         → g_host_to_target → EP7 IN
                      │  handle_target_frame() intercepts dst==target_dev_ip        → Target USB ECM
                      └→ ecm_bridge.c:2651/2686                                     → Target PC (192.168.30.2)
   ═══════════════════════════════════════════════════════════════════════════════════════
        Coexisting without interference: Host USB ECM (USBD) | HID keyboard/mouse (target-side EP) | CH9329 commands
```

### 6.2 Relationship with the existing TCP tunnel and `0x24 NET_PING` (the trade-off that must be stated first)

**There is only one `g_reassembly` in the system**, so the IP channel, the TCP tunnel and `0x24` **cannot receive large messages in parallel**. Phase 1 takes the most honest route: when `SET_IP_MODE(enable=1)` is set, **actively tear down the existing TCP tunnel and refuse new `CONNECT`s** (returning `CONN_FULL`); restore on disable. This is not laziness — it makes the "shared reassembler" fact explicit instead of building a hidden state machine that would interleave frames under concurrency.

Once the tunnel is up, `ping` **works for free** (ICMP is just another forwarded protocol). `0x24 NET_PING` is not retired: it is **the only probe that needs no host-side privilege** (utun on macOS needs `sudo`, `0x24` does not), so the two do not overlap in purpose.

---

## 7. Memory and flash budget

| Item | Bytes | Notes |
|----|------|------|
| `ip_ct_entry_t[8]` | 192 | §5.6 |
| Target-link ARP cache `4 × 12` | 48 | Reuses the existing ARP logic, only adds a cache |
| Inbound IP staging `576 × 1` | 576 | Outbound can reference `g_reassembly.data` at an offset, no extra buffer |
| Counters + config shadow + `ip_router` state | ~96 | |
| **Total** | **about 0.9 KB** | **8.5%** of the 10.76 KB free |

At the end of Phase 1 `size` must be re-run on all four targets (keymod-mini currently RAM 77.95% / FLASH 48.63%; `build.sh` accepts **one** target per invocation, so multiple targets need a loop). **If RAM use exceeds 82%, cut the inbound staging buffer and reference the ECM frame buffer directly, rather than growing `g_reassembly`.**

Flash increment is estimated below 2 KB (one new file `APP/ip_router.c` plus a dispatch branch); 48.6% is currently safe.

---

## 8. Host-side virtual adapter: platform permission reality

This is the external constraint that decides whether the design can ship at all, and it is harder than the firmware work.

| Platform | Mechanism | Permission | Verdict |
|------|------|------|------|
| macOS | `utun` (`/dev/tun` is deprecated; use the `UTUNCONTROL` ioctl, or `IPUtil` / NEPacketTunnelProvider) | **requires root**; the only root-free path is `NEPacketTunnelProvider`, which needs a signed app plus entitlement | A CLI helper with `sudo` is acceptable; shipping a product requires NE |
| Linux | `/dev/net/tun` + `TUNSETIFF` | root or `CAP_NET_ADMIN` | Smoothest; the helper binary can be given setcap, so no permanent root |
| Windows | Wintun (needs administrator) / tun2socks | **administrator** | Feasible; `tools/` already has a Windows script ecosystem to hang it on |
| Android | `VpnService.establish()` | **no root**, just one system consent dialog | The only legitimate privilege-free channel, and `VpnService` is inherently "hand me raw IP packets point-to-point", which aligns with this design. **But KeyCmd_Android has no VPN code at all today** (§1 verification), so `VpnService` + consent flow + foreground/background lifecycle must be built from zero |
| iOS | `NEPacketTunnelProvider` | Paid Apple Developer + **Network Extension entitlement approval** + a new app-extension target | KeyCmd has none of the three (§1). **This is an admission problem, not a workload problem**, and is a separate project |

**This section gained weight after this round of verification.** The controller is confirmed to be a phone, and **neither phone platform has any raw-IP infrastructure**, so this design is not "continue what exists" but "build two stacks from zero": Android needs a new `VpnService`, iOS needs entitlement approval plus a new extension target. By contrast, both apps already have working SSH (libssh2 / JSch + BLE-Eth), so **"the phone can ssh" creates zero demand for an IP tunnel.**

---

## 9. Implementation phases

### Phase 0 — Feasibility measurement (no firmware changes, 0.5 day)

Its output decides whether every number below holds, so **it must come first**:

1. **Measure the negotiated conn interval** and the real MTU after a macOS connection (the central may refuse 12.5 ms; `peripheralMTU` comes from `exchangeMTUReq.clientRxMTU`, [APP/peripheral.c:793](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L793)).
2. Stress notifications/s with `0x1E PING` / `0x11 DATA` to calibrate the §4 throughput budget, in particular how large the discount is from **a per-fragment ACK eating half the slots**.
3. Separately verify a pure Python path under `sudo` that creates a utun interface, injects a route and reads/writes raw IPv4 packets (no device involved).

If measured net throughput is under 20 kbit/s, Phase 1 should be downgraded to "forward ICMP only (make `ping` work); SSH keeps using the existing TCP proxy".

### Phase 1 — Firmware IP layer (2–3 days)

| Change | Location |
|------|------|
| Add `APP/ip_router.c` + `APP/include/ip_router.h`: `ip_router_input/output`, conntrack, Target-link ARP resolution, checksums | new files |
| Generalise IPv4 extraction: turn `BLE_Eth_HandleTargetFrame()`'s `if (ip[9] != 6) return false` into `else → ip_router_input()` | [APP/ble_eth_protocol.c:1714](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L1714) |
| RX reassembly dispatch recognises `conn_id == 0xFE` | near `process_fragment()` ([APP/ble_eth_protocol.c:584-604](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L584)) |
| `!= 0xFE` guards in `conn_id` allocation / `CONNECT` / `DISCONNECT` | `BLE_Eth_AllocConnection()`, [APP/ble_eth_protocol.c:502](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L502) |
| `0x25` dispatch + `0xA5` response + INFO feature bit 5 `BLE_ETH_FEAT_IP_TUNNEL 0x20U` | `ble_eth_protocol.c` + [APP/include/ble_eth_protocol.h:71-103](file:///Users/txa/project/Openterface_KM/APP/include/ble_eth_protocol.h#L71) |
| Register `0x25` in `Peripheral_IsBleEthCommandByte()` | [APP/peripheral.c:239](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L239) |
| Clamp MSS to 536 / advertise MTU | TCP MSS rewrite inside `ip_router` (optional in Phase 3; 576 is enough for ssh) |
| Mode gating: allow enable only when `USB_ModeConfig_ActiveModeHasTargetCdcEcm()` is true | [APP/usb_mode_config.c:233](file:///Users/txa/project/Openterface_KM/APP/usb_mode_config.c#L233) |
| Documentation | add a `0x25` section plus the feature bit to `docs/ble-eth-protocol.md`; add `docs/ble-ip-tunnel-usage.md` |

**Build gate**: all four targets green + `size` within the §7 budget.
**Hardware gate**: `ping` 0% loss, `ssh` interactive, HID keyboard/mouse dropping no keys (watch that the HID queue overflow counter does not grow).

### Phase 2 — Host-side helper (1–2 days)

`tools/ble_ip_tunnel.py`: BLE connect → `0x25 enable` → create utun/TUN (`--interface` handed to the OS) → move raw IPv4 both ways → `--routes` injects the Target subnet → on exit `0x25 disable` and remove the routes. One implementation for macOS/Linux, a separate `wintun` branch for Windows.

### Phase 3 — Stabilisation (priority decided by Phase 0/1 data)

1. **Batch fragment ACKs** (one ACK per message, or accumulate against an rx_window): the largest free throughput win.
2. Return `ICMP Fragmentation Needed` to the host so PMTUD works.
3. **Serialised scheduling** for sharing the reassembler with the TCP tunnel (if the product requires both to coexist, the IP channel needs its own 1536 B reassembler at +1.5 KB RAM — re-evaluate then).
4. Automatic tunnel recovery after BLE disconnect/reconnect and after Target re-enumeration.
5. Observability for the three drop classes — conntrack full / ECM queue full / TX queue full (extend `0xA5` or add `0x28 GET_IP_STATS`).

### Phase 4 — Android `VpnService` (separate, not estimated)
### Phase 5 — iOS Network Extension (prerequisite: entitlement approval + new extension target, not estimated)

---

## 10. Risks and open questions

| # | Risk | Severity | Handling |
|---|------|--------|------|
| R1 | **Wider attack surface**: once BLE pairing becomes an IP entry point, any successfully paired host gains entry to the Target's internal network, and BLE has no link-layer authentication (it is bare GATT today) | **High** | Off by default; `enable` requires a bonded session established via `0x22 CMD_BLE_SECURITY`; auto-disable after 10 minutes of no traffic; allow only the single `target_host_ip`, no whole-subnet forwarding |
| R2 | Throughput may not reach the SSH comfort line (only Phase 0 can tell) | Medium | Phase 0 is the gate; if it fails, fall back to "ICMP forwarding only" |
| R3 | The single reassembler makes the IP tunnel and the TCP tunnel mutually exclusive, which users will report as a bug | Medium | State it explicitly in `docs/ble-eth-protocol.md`; `0xA5` returns `active`; the UI shows "TCP proxy unavailable" as soon as the tunnel is on |
| R4 | After SNAT, every connection on the Target side comes from `192.168.30.5`, so troubleshooting cannot trace back to a person | Medium | The conntrack table keeps `orig_src_ip`; export the mapping through `GET_IP_STATS` |
| R5 | The existing one-ACK-per-fragment behaviour is amplified on the IP channel and may present as "ping works but scp hangs" | Medium | Phase 3 item 1; Phase 1 explicitly promises nothing about large files |
| R6 | If the ordering between the forwarding path and `handle_target_icmp_packet()` (the stand-in reply for destinations that are ourselves) is wrong when `dst_ip==target_dev_ip`, the device will "answer ping on behalf of the target" | Medium | Fix the dispatch order: conntrack hit → forward; miss and dst==self → stand in. Add it to the unit-test cases |
| R7 | On macOS, the `utun` address fights the real `en` adapter's routes | Low | Use the dedicated `10.88.0.0/24` and add only the Target subnet's specific route, never default |
| R8 | The known Host ECM ↔ Target ECM L2 bridge is itself broken: `handle_arp_frame()` answers only for the device's own IP, while DHCP hands out a /24, so the documentation's "Host PC and Target PC can ping each other" simply does not hold on the USB path | — | **This design neither fixes nor depends on it** (BLE carries raw IPv4, bypassing ARP). But it needs its own project, otherwise users will judge the tunnel against their USB-bridge expectations |

---

## 11. Engineering judgement: park this and do three cheaper things first

The controller is confirmed to be a phone (both iOS and Android exist and both were verified). That fact settles the conclusion.

1. **Phone SSH is unrelated to an IP tunnel.** Both apps ship a complete SSH client (iOS libssh2 + a `socketpair` bridge, Android JSch + a custom `SocketFactory`), and underneath it is BLE-Eth `0x10/0x11/0x12`. **Build the IP tunnel and phone SSH does not improve by a single byte.** "It works on 0.0.4/0.0.5" confirms that the BLE-Eth TCP proxy path has always worked, not that an IP path was missing.

2. **Neither phone platform has a raw-IP foundation, and iOS is blocked on admission rather than workload.** Android needs a `VpnService` built from zero; iOS needs Network Extension entitlement approval plus a new extension target. The firmware side needs only 0.9 KB RAM and 2–3 days, **but 90% of the real cost sits in the apps and in the account**, which I could not have estimated before this round of verification. Now it is certain: asymmetric, not worth it.

3. **What actually holds the phone experience back is the link-layer configuration, not the protocol layer.** `BLE_BUFF_MAX_LEN = 64` ⇒ ATT_MTU can only reach 60 ⇒ 57 B of net data per notification (Android has hard-coded 57 for a long time), and with `BLE_TX_NUM_EVENT = 1` ⇒ **one packet per 12.5 ms ≈ 36.5 kbit/s raw one-way ceiling**. `BLE_ETH_SAFE_NOTIFY_LEN 180` is dead code that can never be reached under the current configuration. This is why SSH "works but feels sticky". **No protocol layer gets around that wall; raising it benefits SSH, `0x24` ping and any future tunnel at once.**

### Suggested practical priorities

| Rank | Item | Cost | Benefit |
|------|------|------|------|
| **1** | Try `BLE_BUFF_MAX_LEN=251` (ATT_MTU 247) + `BLE_TX_NUM_EVENT=4`. Both are `?=` variables in `build/makefile:256,259`, so it is a value change with no source edits; **but `BLE_ETH_SAFE_NOTIFY_LEN` must move from 180 to 244 at the same time** ([APP/peripheral.c:46](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L46)), otherwise 180 instantly becomes the new ceiling | 0.5 day plus one `BLE_MEMHEAP_SIZE` review (currently 7 KB, [HAL/include/config.h:112](file:///Users/txa/project/Openterface_KM/HAL/include/config.h#L112)); the WCH library's internal buffer needs are **not visible in the source**, so connection stability must be measured | Net data per notification goes from 57 B to 235 B, multiplied by 4 packets/event: the theoretical ceiling rises from 36.5 kbit/s to ≈ **780 kbit/s**; even half of that is an order-of-magnitude improvement |
| **2** | On the app side, change the blanket write-with-response policy to "withResponse for control frames only, noResponse for DATA" | A few lines per app; **note that both comments record that the switch to all-withResponse was made because "the handshake succeeded but key presses never reached the server"** (`BLEManager.swift:438-467`, see `docs/ble-eth-protocol-spec.md:277-298`), so that failure must be reproduced before touching this | The uplink stops costing one round trip per write; only pays off combined with item 1 |
| **3** | Add ping to the phones: firmware `0x24 NET_PING` is already implemented (uncommitted), **neither app has any ping capability** (iOS has no `0x24`; Android has not even defined `0x1E`). One call plus UI each, **zero privilege** | about 0.5 day per app | Directly satisfies the original "ping over Bluetooth" request |
| **4** | Fix the inconsistent default target IP: Android `DEFAULT_KEYCMD_HOST = 192.168.11.2`, iOS `defaultKeyCmdHost = 192.168.12.1` with the dialog defaulting to `192.168.11.2`, firmware fallback `192.168.11.2:22`, while the ECM subnet in the field is `192.168.30.0/24` (Target PC = `.30.2`). `docs/ble-eth-ssh-usage.md` and `ble-eth-tunnel-examples.md` themselves document two different target IPs | Small; or have the app auto-discover via `0x23 GET_ECM_IP` | **This is very likely the real reason "phone ssh cannot connect"**, and it has nothing to do with an IP tunnel |
| 5 | This IP-over-BLE design | 2–3 days of firmware + raw-IP foundations in two apps + iOS account approval | Start only when a product requirement appears for "must run third-party apps that have no adaptation". Re-calibrate the §4 numbers from Phase 0 then |

**In one sentence**: every technical decision here holds, but on a phone it does not address the user's current pain. Items 1–4 above total under a week and deliver far more than the tunnel.

---

## 12. Acceptance criteria (Phase 1–2)

1. macOS: after `sudo python3 tools/ble_ip_tunnel.py --name keymod` comes up, `ping -c 4 192.168.30.2` shows **0% loss**, with **no** configuration added on the Target machine.
2. `ssh user@192.168.30.2` is interactive; after `exit`, `0x25 disable` takes effect.
3. With the tunnel on, HID keyboard/mouse drop no keys and do not stutter (HID queue overflow counter is 0).
4. Feature bit 5 of `0x1F INFO` correctly reflects firmware capability; an old host tool meeting unsupported firmware must fail loudly rather than time out silently.
5. BLE disconnect → reconnect → re-enable leaves the tunnel usable; the tunnel is usable after `SET_INTERFACE(alt=1)` re-enumeration on the Target side.
6. All four build targets (keymod-mini / keymod-plus / kvmgo / minikvm) are green, with `size` RAM/FLASH increments inside the §7 budget.
7. The `0x25` section of `docs/ble-eth-protocol.md` matches the implementation byte for byte, and the example frames validate with `tools/debug_ble_eth.py`.
