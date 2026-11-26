# Cross-OS Portability

Status: **Reference**
Last verified: 2026-11-26 — macOS 26.5 host, Windows 11 target, mode 8.

This document answers one question: *if I plug this dongle into a different
computer, does it work, or does it only work on the machine it was developed
against?* It separates what the USB class specification already guarantees from
what the firmware merely happened to do, and lists the values that are still
hardcoded and what they break.

## 1. Where OS dependence hides

There are three layers, and only the first two live in the firmware:

| Layer | Owner | OS-specific? |
|---|---|---|
| USB descriptors (class, subclass, protocol, functional descriptors, MS OS block) | firmware | **yes** — drivers match on these bytes |
| Wire behaviour (NTB layout, DHCP, ARP, DNS, ICMP) | firmware | no — bytes on the wire, RFC-shaped |
| Host-side services (dnsmasq, nginx portal, Samba share) | host OS | **yes** — and this is the layer that never migrated |

The trap is the first row. A bridge can be perfectly conformant on the wire and
still be single-OS, because driver binding keys are descriptor bytes that no
protocol validator ever looks at. Two of the four defects below were exactly
that: the device enumerated correctly, no error appeared in any log, and the
peer OS simply declined to create an interface.

## 2. Matching keys, per peer

| Peer | Binds to | Also requires |
|---|---|---|
| macOS (`AppleUSBNCMControl` / `AppleUSBNCMData`) | comm IF class `0x02` / subclass `0x0D`; data IF class `0x0A` / subclass `0x00` / **protocol `0x01`** | nothing else |
| Linux (`cdc_ncm`) | data IF class `0x0A` / subclass `0x00` | nothing else — ignores protocol |
| Windows 11 / Server 2022 (`UsbNcm.sys`) | comm IF class `0x02` / subclass `0x0D`, via the IAD | MS OS `0xEE` + `WINNCM`, **or** an INF |

Two consequences that drove the fixes:

* **`bInterfaceProtocol` on the data interface is a matching key, not decoration.**
  `0x00` is legal for ECM and wrong for NCM; macOS silently skips the interface.
* **The MS OS descriptor is what buys zero-install on Windows.** A NCM function
  without it makes Windows ask for a driver even though the class driver exists.

## 3. What was OS-biased, and is now fixed

| Defect | Effect | Commit |
|---|---|---|
| host-port data IF declared protocol `0x00` | macOS loaded the control driver and created no interface | `0bafa81` |
| host-port NCM functional descriptor was 14 bytes, class requests used a shifted numbering | Windows would fail device start (Code 10); macOS probes the spec codes and stalled | `0bafa81` |
| host port answered DHCP **after** the broadcast relay | a DISCOVER is an IPv4 broadcast, so the relay swallowed it; the peer fell back to `169.254` and only a *manually configured* host worked | `96ce641` |
| host port always advertised `8.8.8.8` as DNS | a resolver on a link with no uplink; every lookup timed out | `96ce641` |
| target-port data IF declared protocol `0x00` | the target port was effectively Windows-only — a Mac plugged in there got no interface | `96ce641` |

The third one is the interesting failure mode. Development always used a Fedora
host with a static address, so nobody ever *asked* the host port for DHCP. The
bug was invisible for the entire life of the feature and would have surfaced as
"macOS doesn't work" on any machine left on the default network setting.

## 4. What is still hardcoded

| Item | Where | What it breaks | Suggested fix |
|---|---|---|---|
| MAC `02:00:00:00:00:01` for **every** unit | `iMACAddress` string; `NCM_DEV_MAC` `.32` / `NCM_TARGET_DEV_MAC` `.33` on the wire | two dongles on one host present the same MAC: duplicate ARP, duplicate DHCP lease, macOS disables one interface with "another device is using your IP address" | derive the last three bytes from the chip UID, keep the `02:` locally-administered prefix |
| no MS OS descriptor on the **host** port | `USBLIB/CONFIG/usb_desc.c` | Windows as a *host* still needs an INF, so it is not zero-install | mirror the target port's `0xEE` / `WINNCM` block |
| `bmNetworkCapabilities = 0x02` on the target NCM FD | `usbd_desc.c:942` | `0x02` is the **DHCP** capability bit; the firmware is the DHCP *server*, not a client. The host port says `0x01` (NET_ADDRESS). Harmless on Windows, suspicious for a Mac peer | set `0x01`, then re-verify Windows |
| NCSI probe names `dns.msftncsi.com`, `www.msftconnecttest.com` | `ncm_bridge.c` | nothing — they are DNS names answered locally to keep Windows from flagging the link as "no internet" | none |
| `192.168.11.x` still in the ECM and BLE paths | `ecm_bridge.c`, `ble_eth_protocol.h` | mode 7 and the BLE tunnel use a different plan from mode 8's `192.168.20.x`; confusing, not broken | unify when mode 7 is next touched |
| `192.168.20.1/.3/.2/.5` defaults | `ecm_ip_config.h` | nothing — they are defaults, overridable through the `0x30` configuration record | none |

## 5. Verified combinations

| Host | Target | State |
|---|---|---|
| Fedora, static `192.168.20.1` | Windows 11 | verified, earlier sessions |
| **macOS 26.5, DHCP** | **Windows 11** | **verified 2026-11-26**: service left on "Using DHCP", obtained `192.168.20.1/24`, router empty, no DNS servers; `ping 192.168.20.2` 1.7 ms; target IIS answered `404` |
| macOS | macOS | unverified. Protocol is now correct on the target port, but that port still advertises the DHCP capability bit (§4) |
| Linux | Linux | unverified, though both ends are the same `cdc_ncm` path |

The macOS + DHCP row is the meaningful one: it is the first time the host side
was **not** hand-configured, so it is the first real test of "plug in and it
works".

## 6. What "cross-OS" does and does not cover

* **The dongle is portable.** Any host with a CDC-NCM class driver gets an
  interface, an address, and a working link to the target — no manual IP, no
  INF, no driver install. Verified on macOS and Linux.
* **The host tooling is not.** The captive portal, the file server and the Samba
  share are host-side services. They exist as systemd units and nginx/dnsmasq
  config on the Fedora box; macOS has none of them. Plugging into a Mac gives
  you a network, not the portal. Porting that is a separate piece of work
  (`tools/host_file_server.py` is portable Python; the DNS and redirect halves
  are the part that would need rewriting).
* **Windows as a host is still an open item**, for the single reason in §4: no
  MS OS descriptor on that port.

## 7. Rules for the next change

1. Touching a descriptor byte? Ask which driver's matching key it is. The three
   that matter are the data interface protocol, the NCM functional descriptor
   length, and the MS OS block.
2. Touching a frame handler? Ask which peer answers this. Anything that answers a
   **broadcast** — DHCP, NCSI, discovery — must run before
   `bridge_forward_discovery()`, on both ports.
3. Never verify a peer only with a hand-configured address. Leave the interface
   on DHCP at least once; that is the configuration every user will actually hit.
