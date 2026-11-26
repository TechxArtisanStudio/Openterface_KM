# CDC-NCM side channel — firmware reference

Status: **canonical** (replaces the scattered notes listed at the end)
Applies to: `USB_WORK_MODE=hid_ncm_bridge` (mode 8), `PRODUCT=keymod-plus`
Last verified: 2026-11-26, firmware commit `5b3aff4`, on KeyMod Plus hardware
plus a Windows 11 target.

This document describes *what the firmware does* on the NCM link and *why*.
If you are looking for "how do I reach the files from the target PC", read
[portal-and-file-access.md](portal-and-file-access.md) instead.

---

## 1. What the bridge is for

The two USB ports are a **side channel**, not an uplink. The dongle emulates
keyboard/mouse towards the target PC and, on the same cable, carries an
Ethernet segment that lets the two machines exchange files and serves one
web page.

That single decision explains most of the behaviour below:

* the target PC must keep its normal default route (Wi-Fi / wired LAN);
* the bridge only has to deliver **on-link** reachability;
* therefore the DHCP server never advertises a router (§4).

Traffic classes handled by the firmware:

| Traffic | Handling |
|---|---|
| Unicast IPv4 frame between the two ports | forwarded verbatim, with proxy ARP for the peer |
| IPv4 broadcast (discovery protocols) | relayed to the other port |
| DHCP (`0.0.0.0:68 → 255.255.255.255:67`) | answered locally by the bridge (§4) |
| DNS (`UDP/53`, query) | answered locally inside the first-visit window, then passed through (§5) |
| ICMP echo to the bridge address | answered locally |
| Everything else | forwarded untouched |

The peer OS therefore sees a plain Ethernet segment with one neighbour on it.

---

## 2. Address plan

Defaults, stored in `ECM_IPConfig_t` (see §7) and overridable at runtime:

| Role | Default | Notes |
|---|---|---|
| `host_ip` | `192.168.20.1` | address of the host-side link; also the portal and DNS server |
| `target_host_ip` | `192.168.20.2` | the only lease the DHCP server hands out |
| `dev_ip` | `192.168.20.3` | host-side firmware address |
| `target_dev_ip` | `192.168.20.5` | target-side firmware address; also the DHCP server identifier |
| `subnet_mask` | `255.255.255.0` | |

`192.168.20.0/24` is deliberately outside any common home/office subnet so
the peer never confuses it with its uplink.

Frame identity is by MAC, not by interface name (names such as
`enp0s21f0u1u2` change on every re-enumeration). Both NCM functions advertise
`02:00:00:00:00:01` in their `iMACAddress` string, so each peer adopts that as
*its own* interface MAC. The source MAC the firmware puts on the frames it
injects must not collide with it: `02:00:00:00:00:32` towards the host port,
`02:00:00:00:00:33` towards the target port.

---

## 3. Data path

Both directions run the same pipeline:

```
peer ──USB bulk OUT──► NTB reassembly ──► frame handler ──► peer TX queue ──► NTB ──► bulk IN
```

* **NTB** is NCM's aggregation container (`NTB-16`, 2 KB max, up to 4
  datagrams). A ZLP terminates an NTB whose length is an exact multiple of the
  packet size.
* The frame handler runs, in this order, **DHCP → discovery relay → ARP/ICMP →
  forwarding**. The DHCP step must stay first: a `DHCPDISCOVER` is an IPv4
  broadcast, so the discovery relay would copy it to the other port and return
  before the local server saw it. That ordering bug made the target fall back
  to `169.254.x.x` (fixed in `5b3aff4`).
* Proxy ARP answers for the peer addresses on the segment, so neither side
  needs the other's real MAC.
* Endpoint numbers differ per build mode and per USB controller. Do not
  hardcode them when reading logs; use the mode maps in
  [usb-endpoint-map-mode8.md](usb-endpoint-map-mode8.md) and
  [controllable-usb-endpoint-modes.md](controllable-usb-endpoint-modes.md).

The bridge keeps a small counter set per direction; §8 shows how to read it
over the control channel when no console is available.

---

## 4. DHCP server (target port)

`handle_target_dhcp_packet()` parses `DISCOVER` / `REQUEST` and replies
`OFFER` / `ACK` with this option set:

| Option | Value | Rationale |
|---|---|---|
| 53 message type | 2 / 5 | OFFER / ACK |
| 54 server id | `target_dev_ip` (`192.168.20.5`) | stable identity across re-enumeration |
| 51 lease time | **300 s** | the lease carries the DNS hint, so policy changes must land quickly |
| 1 subnet mask | `255.255.255.0` | |
| 6 DNS | `portal_ip`, or `host_ip` when `portal_ip` is `0.0.0.0` | **only** while `portal_enabled && portal_mode == 1` (§5) |
| 3 router | **never sent** | see below |

### Why there is no router option

Advertise a gateway and Windows immediately installs `0.0.0.0/0` on the USB
NIC. Every internet session then dies inside the dongle while the Wi-Fi link
sits idle — the symptom that started this rewrite: *"I plugged the dongle in
and all my Wi-Fi traffic went to the USB Ethernet."*

Nothing on the far side needs a router: the portal, the DNS answer and the
SMB share all live on-link. Do not add it back.

### Why DNS is conditional

Advertising a resolver unconditionally makes the target ship every lookup
through the dongle even though its own uplink is healthy. When the greeting is
off, the bridge stays silent and the target keeps its own DNS servers.

Consequence to remember during bring-up: while the option *is* sent, the host
must actually run a resolver on `host_ip:53` (see
[portal-and-file-access.md](portal-and-file-access.md)); otherwise lookups
black-hole after the greeting window.

---

## 5. One-shot captive-portal greeting

### Goal

Make the target OS open its built-in sign-in window **once**, pointing at the
file manager, then behave like an ordinary wire. Earlier revisions spoofed
every DNS answer for the whole session, so the peer could never resolve
anything real — every request landed on the portal address.

### Mechanism

`handle_dns_hijack()` inspects every `UDP/53` query that crosses the link,
regardless of which server the peer addressed. It only answers:

* `QR=0`, exactly one question, `QDCOUNT == 1`;
* type `A`, class `IN`. `AAAA`, `MX` and friends pass through untouched —
  answering them with an A record confuses resolvers *and* would burn the
  first-visit budget.

Then, in `portal_mode == 1`:

| Query | Answer |
|---|---|
| `dns.msftncsi.com` | portal address while this peer's first visit is being served; `131.107.255.255` afterwards |
| `www.msftconnecttest.com` | portal address **always** (so `/connecttest.txt` is served locally and NCSI settles) |
| any other name | portal address while admitted; **passed through** once spent |

Per-peer first-visit window (`NCM_PortalClient_t`, 4 slots, oldest recycled):

| Limit | Default | Macro |
|---|---|---|
| elapsed time from the first admitted answer | 10 s | `NCM_PORTAL_FIRST_VISIT_MS` |
| admitted answers | 4 | `NCM_PORTAL_FIRST_VISIT_MAX` |

Either limit ends the window, and the peer's DNS is then a plain wire. Two
machines on the segment keep independent budgets.

The reply is built by hand: L2/L3/UDP swapped, transaction ID preserved,
`TTL=10`, flags `QR|AA|RD|RA`, `ARCOUNT=0` (the EDNS OPT record is dropped),
no UDP checksum, IPv4 checksum recomputed.

### Latch

`NCM_Bridge_PortalGreetRestart()` is called on USB (re-)enumeration but only
**latches once per power-on**. Re-arming on every re-enumeration kept the OS
convinced a portal was present and relaunched its browser on each NCSI poll.
A replug therefore does *not* give you another greeting — power-cycle the
dongle if you need one.

### The QNAME bug that made this look impossible

The reference QNAME for the NCSI probe was written with the label length
`9u` although `msftncsi` is eight characters. `dns_name_equals_ci()` compares
lengths first, so the probe never matched and every query took the generic
hijack branch — indistinguishable from always-portal mode. It is now `8u`,
guarded by compile-time asserts on the total length (18 bytes) and on the HTTP
probe name (25 bytes). If you edit either array, the build fails rather than
silently regressing.

### `portal_mode == 2`

The enum and comments still describe "hijack every query forever" for lab use,
but the mode is **unreachable in current firmware**: `ECM_IPConfig_SetPortal()`
rejects `mode > 1`, and `ECM_IPConfig_Init()` clamps a persisted `> 1` back to
`1`. Treat mode 2 as dead code; use a packet capture if you need to fake an
always-portal peer.

---

## 6. What the host side must provide

The firmware only decides *what address* a name resolves to. The web side has
to exist:

1. a resolver on `host_ip:53` (dnsmasq in the reference setup), reachable from
   `192.168.20.0/24`;
2. an HTTP server on `host_ip:80` that returns the exact NCSI probe bodies and
   redirects everything else to the file manager;
3. the file manager on `host_ip:8080`;
4. **no NAT and no forwarding**. The link is not an uplink. If you NAT it, the
   target may silently prefer it over Wi-Fi.

All four are covered in [portal-and-file-access.md](portal-and-file-access.md).

---

## 7. Configuration and persistence

`ECM_IPConfig_t` (27 bytes) is stored in a dual-page flash record with a magic,
schema version, CRC32 and write counter for wear levelling. Portal fields:

| Field | Default | Meaning |
|---|---|---|
| `portal_enabled` | `1` | master switch for DNS interception |
| `portal_mode` | `1` | `0` off, `1` greet once, `2` legacy (unreachable, §5) |
| `portal_ip` | `0.0.0.0` | answer address; `0.0.0.0` means "follow `host_ip`" |

Runtime access, over the CH9329 `0x33` (ISP debug) command:

| Sub-command | Direction | Payload |
|---|---|---|
| `0x14` | read | returns `[enabled, mode, ip0..ip3]` (6 bytes) |
| `0x15` | write | sends `[enabled, mode, ip0..ip3]` |

Both are also reachable from the BLE configuration channel. Writes go through
`ECM_IPConfig_SetPortal()`, which validates and persists; a partial IP update
preserves the portal fields.

Migration: builds before the greeting fix could persist `portal_mode = 2`.
`ECM_IPConfig_Init()` silently clamps it to `1` in RAM on first boot after the
upgrade (persisted on the next config save), so a stale device stops pinning
the peer in "portal detected" state without a factory reset.

---

## 8. Diagnostics

No `printf` on this path — everything is a memory read over the control
channel (`0x33`, sub-command `0x10` for the host side, `0x13` for the bridge).

`0x33/0x10`, 56 bytes:

| Offset | Field |
|---|---|
| 0 | version (`0x01`) |
| 1 | `data_if_active` |
| 2..4 | `EPnR[EP3..EP5]` low byte |
| 5 | NTB sequence |
| 6..8 | host TX queue count / rd / wr |
| 9..10 | host TX queue overflow (u16) |
| 11..14 | pending NTB length / offset |
| 15 | ZLP pending |
| 16..19 | RX length / needed |
| 20..27 | device IP, host IP |
| 28..55 | u16 counters: `ep3_rx_pkts`, `ntb_sig_ok`, `ntb_sig_bad`, `ntb_parsed`, `host_frames`, `arp_req`, `arp_match`, `arp_reply`, `icmp_echo`, `dhcp_offer`, `tx_sent`, `zlp_sent`, `rx_oflow`, `poll_calls` |

`0x33/0x13`, 20 bytes:

| Offset | Field |
|---|---|
| 0 | version (`0x01`) |
| 1 | `s_bridge_enabled` |
| 2 | EP5 status |
| 3..4 | target RX length |
| 5..6 | target RX needed |
| 7..8 | target frames |
| 9..10 | discovery frames relayed |
| 11..12 | target→host frames |
| 13 | target TX queue depth |
| 14..15 | **`g_portal_hijacked`** — DNS answers spoofed since boot |
| 16..17 | raw EP5 OUT interrupts (toggle-desync detector) |
| 18 | silence-watchdog recoveries |
| 19 | reserved |

Reading `g_portal_hijacked` is the quickest way to tell "the greeting is
working" (it increments in bursts of ≤ 4 per peer) from "the peer is being
hijacked forever" (it climbs monotonically).

---

## 9. Known limitations

These are consequences of the design, not bugs to file:

* **"Unidentified network".** Without a gateway Windows cannot classify the
  link; NLM forces the *Public* profile even if you set *Private*, and
  Explorer's *Network* view lists nothing (it no longer spins). `\\KEYMOD-HOST`
  and `\\192.168.20.1` work normally.
* **WS-Discovery needs a rename-proof listener.** `wsdd` binds the interface
  *name*; a replug changes it (`u1u2` → `u5u2`) and the daemon dies with
  `Bad file descriptor`. Resolve the interface by MAC (`02:00:00:00:00:01`)
  and restart on change.
* **No internet through the bridge.** There is no NAT and no upstream. After
  the greeting, names that the host resolver cannot answer simply fail.
* **Five-minute leases.** Intentional; expect a renewal every 5 minutes.
* **Hub power.** A bus-powered dongle behind an unpowered hub can drop off the
  bus mid-write (`error -71`, `device not accepting address`). Flash from a
  root port when possible; `xhci_hcd` unbind/bind recovers the controller
  without a reboot.
* **One greeting per power-on.** A replug does not re-arm it (§5).

---

## 10. Verification checklist

Build (host that has the WCH toolchain on `PATH`):

```bash
cd build
make clean && make -j8 PRODUCT=keymod-plus \
  USB_WORK_MODE=hid_ncm_bridge USB_WORK_MODE_RUNTIME_PROFILES=0
riscv-none-embed-objcopy -O binary openterface-km-firmware.elf /tmp/km.bin
```

Flash (device in ISP mode, USB ID `4348:55e0`):

```bash
wchisp flash /tmp/km.bin        # expect "Verify OK"
```

Link and lease:

```bash
ip -br addr show | grep 192.168.20          # host side holds .1/24
ip neigh show | grep 192.168.20             # peer at .2, MAC 02:00:00:00:00:32
```

DHCP option check on a peer (Windows): `ipconfig /all` must show an address in
`192.168.20.0/24`, an **empty** default gateway, a 300 s lease, and DNS equal
to `192.168.20.1` only while the greeting is armed.

DNS behaviour, injected from the host so the source IP differs from the host's
own address:

```bash
IF=enp0s21f0u1u2
sudo ip addr add 192.168.20.9/24 dev $IF
sudo ip route replace 8.8.8.8/32 via 192.168.20.2 dev $IF
for i in 1 2 3 4 5; do dig +time=1 +tries=1 @8.8.8.8 -b 192.168.20.9 \
  "t$i.example.com" A +short; done
dig +time=1 +tries=1 @8.8.8.8 -b 192.168.20.9 dns.msftncsi.com A +short
sudo ip route del 8.8.8.8/32 via 192.168.20.2; sudo ip addr del 192.168.20.9/24 dev $IF
```

Expected: the first four answers are `192.168.20.1`, the fifth is empty
(passed through), and `dns.msftncsi.com` settles on `131.107.255.255`.

---

## 11. Related documents

Canonical:

* [portal-and-file-access.md](portal-and-file-access.md) — host-side portal,
  file manager, SMB, zero-install contract.
* [controllable-usb-endpoint-modes.md](controllable-usb-endpoint-modes.md) —
  build-time/runtime mode model.
* [build-and-flash-guide.md](build-and-flash-guide.md),
  [build-flash-cycle.md](build-flash-cycle.md) — toolchain and flashing.

Superseded by this document (kept for history):

* `cdc-ncm-implementation-plan.md` — original NCM port plan.
* `openterface-ncm-troubleshooting-2026-11-24.md` — session log of the Windows
  bring-up.
* `ecm-bridge-dhcp.md` — ECM predecessor; its DHCP/ARP description still
  matches the NCM path.
* `ncm-descriptor-debug.md` — descriptor and `GET_NTB_PARAMETERS` fixes.
* `FIRMWARE_DHCP_DEBUG_GUIDE.md` (Chinese) — field debugging recipes.
