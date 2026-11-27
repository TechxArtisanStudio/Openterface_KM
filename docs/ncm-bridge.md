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
| Unicast IPv4 frame between the two ports | forwarded verbatim, with proxy ARP for the peer (configured *and* last-seen address) |
| IPv4 broadcast (discovery protocols) | relayed to the other port |
| DHCP (`0.0.0.0:68 → 255.255.255.255:67`) | answered locally by the bridge (§4) |
| DNS (`UDP/53`, query) | answered locally inside the first-visit window, then passed through (§5) |
| ICMP echo to the bridge address | answered locally |
| Everything else | forwarded untouched |

The peer OS therefore sees a plain Ethernet segment with one neighbour on it.

### Proxy-ARP and forwarding follow the address the peer actually uses

`target_host_ip` is the address the target is *expected* to use. A target whose
IPv4 was set by hand on the far side answers on a different one, and the Host
has no way to learn it — so everything that has to recognise "this frame belongs
to the target" matches two addresses: the configured one, and the one the peer
was last seen speaking from (`s_target_seen_ip`, filled by
`target_learn_peer_ip()` from the ARP sender field or the IPv4 source of every
frame the target sends).

- proxy-ARP answers for either address, claiming **exactly the address that was
  asked about** rather than the configured one;
- the Host→Target unicast filter forwards frames addressed to either;
- the silence probe asks the configured address first, then the learned one.

Measured on the bench with the target moved to `192.168.20.77` while
`target_host_ip` stayed `.2`: `ping` 0% loss, `ssh` login to `.77` succeeds,
8 MiB target→host 0.55 MiB/s, 8 MiB host→target 0.52 MiB/s — both unchanged
from the `.2` baseline, 0 duplicates. Before this change `.77` was unreachable
while the pipe itself was fine: the Host's ARP for `.77` went unanswered and the
unicast filter would have dropped the frames anyway.

The learned address is cleared on target-side init, so a swapped or re-addressed
machine starts from the configured address again instead of keeping the old one
proxy-ARP'd.

One caveat: the learned address can only be answered once the peer has spoken
from it. A Host ARPing an address the target has never used gets no reply, and
nothing on this side can force that first frame. In practice a Windows target
emits traffic — and a gratuitous ARP — as soon as an address is assigned, so the
window is about a second, and the Host's own ARP retries (1 s apart) cover it.

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
`enp0s21f0u1u2` change on every re-enumeration). Each NCM function advertises
a per-unit address in its `iMACAddress` string, so each peer adopts that as
*its own* interface MAC. Since the addresses come from the chip UID, the
predictable part is only the shape `02:xx:xx:xx:xx:rr`
([cross-os-portability.md](cross-os-portability.md) §4.1):

| Role | Shape | Who sees it |
|---|---|---|
| host NIC | `02:xx:xx:xx:xx:01` | host peer adopts it; also returned by `GET_NET_ADDRESS` |
| host peer | `02:xx:xx:xx:xx:32` | source MAC of frames we inject toward the host |
| target NIC | `02:xx:xx:xx:xx:00` | target peer adopts it |
| target peer | `02:xx:xx:xx:xx:33` | source MAC of frames we inject toward the target |

The peer MAC must never equal the NIC MAC of the same link; an OS drops frames
whose source is its own address.

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
  forwarding**, on both ports. The DHCP step must stay first: a `DHCPDISCOVER`
  is an IPv4 broadcast, so the discovery relay would copy it to the other port
  and return before the local server saw it. That ordering bug made the target
  fall back to `169.254.x.x` (fixed in `5b3aff4`); the same bug in the mirror
  path left the *host* port unable to hand out an address until `96ce641`, and
  because development always used a statically configured host it stayed
  invisible for months.
* Proxy ARP answers for the peer addresses on the segment, so neither side
  needs the other's real MAC.
* Endpoint numbers differ per build mode and per USB controller. Do not
  hardcode them when reading logs; use the mode maps in
  [usb-endpoint-map-mode8.md](usb-endpoint-map-mode8.md) and
  [controllable-usb-endpoint-modes.md](controllable-usb-endpoint-modes.md).

The bridge keeps a small counter set per direction; §8 shows how to read it
over the control channel when no console is available.

---

## 4. DHCP server

Both ports run one. `handle_target_dhcp_packet()` serves the target,
`handle_dhcp_packet()` serves the host; they parse the same `DISCOVER` /
`REQUEST` pair and differ only in the address they hand out.

### Target port

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

### Host port

| Option | Value | Rationale |
|---|---|---|
| 53 message type | 2 / 5 | OFFER / ACK |
| 54 server id | `dev_ip` (`192.168.20.3`) | the host side has no other stable identity |
| 51 lease time | 3600 s | the host is not a transient guest; there is no greeting to refresh |
| 1 subnet mask | `255.255.255.0` | |
| 6 DNS | `8.8.8.8`, **only** while `portal_enabled && portal_mode == 1` | a decoy: the firmware answers every query itself in that state (§5) |
| 3 router | **never sent** | same reason as below |

This is what makes the dongle OS-neutral in practice: a peer left on "obtain an
address automatically" — the default on every desktop OS — gets `host_ip` with no
manual configuration. Verified on macOS 26.5 (see
[cross-os-portability.md](cross-os-portability.md)).

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

`0x33/0x13`, 38 bytes:

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
| 18 | current silence-probe interval in seconds (10..120) |
| 19 | small-frame rescues in the host TX queue, saturating at 255 (was padding) |
| 20..21 | target TX queue overflow (low 16 bits of the u32 counter) |
| 22..23 | `poll_skipped` — poll re-entries refused by the guard |
| 24..25 | duplicate re-arms refused (`dup_blocks`) |
| 26..27 | `tog_bad` — EP5 OUT packets ACKed with the wrong data toggle |
| 28..29 | `SET_INTERFACE(IF5)` count (low 16 bits) |
| 30..31 | liveness probes sent (ARP requests after going quiet) |
| 32..33 | `unproven` — probes that got no answer from any known address |
| 34..37 | last source IPv4 the peer was seen speaking from |

`tog_bad` pairs with the TX watchdog: each `target_ep_resync()` resets the
expected RX toggle to DATA0, and if the peer is on the other toggle its next
packet is ACKed and discarded, so `tog_bad` rises by one per reset that actually
cost a chunk.

The silence path reads differently. On a healthy idle link `probes` rises
(one 42-byte ARP request every 10 s) and nothing else moves: no bounce, so
`set_if5` and `tog_bad` stay flat, and `probe_interval` stays 10. Two probes per
cycle means the configured `target_host_ip` went unanswered and the address in
`seen_ip` — learned from the peer's own frames — was used instead; that is the
normal state of affairs for a target whose IPv4 was set by hand. `unproven`
rising means neither address answered, in which case the interval doubles
(10 s → 120 s cap) and the endpoints are left alone rather than reset on a
guess. `probe_interval` reading 120 is therefore the "peer is silent at every
address we know" state, not a fault counter.

Reading `g_portal_hijacked` is the quickest way to tell "the greeting is
working" (it increments in bursts of ≤ 4 per peer) from "the peer is being
hijacked forever" (it climbs monotonically).

`poll_skipped` is the load indicator that matters for the duplicate-frame
question: it counts the times the EP4 IN completion landed inside a running
`NCM_Bridge_Poll()` body. It rises by ~4 250 per 8 MiB target→host transfer
(≈300/s), which is why that path needed a guard at all. `dup_blocks` counts
chunks that would have been armed twice; it must stay 0 — a non-zero value means
`host_tx_feed()` has been reordered back to arm-before-advance.

Drop accounting lives in the *other* frame: `0x33/0x10` offsets 9..10 are
`g_host_tx_queue.overflow`, which moves only when a frame was thrown away
because both slots were busy. A run that loses ICMP replies without that counter
moving lost them somewhere else; one where it moves by thousands lost them in
the queue. The two builds in `ncm-tmos-scheduling.md` §"The CI artifact drops far
more small frames than a local build" differ by exactly that counter (+4 vs
+1 585 per 8 MiB run), and that is how the drop was located instead of guessed.

Over BLE the `0x13` reply needs a connection that negotiated MTU 247; a
reconnect that lands on the stale duplicate identity reports MTU 60 and returns
nothing at all. It can also take longer than a second under load.

---

## 9. Known limitations

These are consequences of the design, not bugs to file:

* **"Unidentified network".** Without a gateway Windows cannot classify the
  link; NLM forces the *Public* profile even if you set *Private*, and
  Explorer's *Network* view lists nothing (it no longer spins). `\\KEYMOD-HOST`
  and `\\192.168.20.1` work normally.
* **WS-Discovery needs a rename-proof listener.** `wsdd` binds the interface
  *name*; a replug changes it (`u1u2` → `u5u2`) and the daemon dies with
  `Bad file descriptor`. Resolve the interface by MAC shape
  (`02:xx:xx:xx:xx:01`) and restart on change;
  `tools/find_host_nic.sh` does exactly that.
* **No internet through the bridge.** There is no NAT and no upstream. After
  the greeting, names that the host resolver cannot answer simply fail.
* **Five-minute leases.** Intentional; expect a renewal every 5 minutes.
* **Hub power.** A bus-powered dongle behind an unpowered hub can drop off the
  bus mid-write (`error -71`, `device not accepting address`). Flash from a
  root port when possible; `xhci_hcd` unbind/bind recovers the controller
  without a reboot.
* **One greeting per power-on.** A replug does not re-arm it (§5).
* **Saturation drops small packets, in either direction, with or without BLE.**
  Each direction owns a 2-frame queue; TCP keeps it full, so an ICMP request
  arriving at a full queue is tail-dropped (~5 ms of queue, hence low RTT). The
  target→host queue keeps one extra reserved slot for frames ≤ 128 B, so
  replies travelling against the load survive the saturated ring (measured 81 %
  → 0 % loss); the host→target queue has no slot, so a probe sent *into* the
  load is still dropped (measured 51 % loss while uploading at 0.554 MiB/s).
  With the probe in the opposite direction to the load
  the same link only queues (0–2 % loss, ~200 ms). The radio is not a factor —
  measured both ways. The mechanism, the measurements and what was tried are in
  [ncm-tmos-scheduling.md](ncm-tmos-scheduling.md).

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
ip neigh show | grep 192.168.20             # peer at .2, MAC 02:xx:xx:xx:xx:32
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
