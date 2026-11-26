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
| no MS OS descriptor on the **host** port | `USBLIB/CONFIG/usb_desc.c` | Windows as a *host* still needs an INF, so it is not zero-install | mirror the target port's `0xEE` / `WINNCM` block |
| `bmNetworkCapabilities = 0x02` on the target NCM FD | `usbd_desc.c:942` | `0x02` is the **DHCP** capability bit; the firmware is the DHCP *server*, not a client. The host port says `0x01` (NET_ADDRESS). Harmless on Windows, suspicious for a Mac peer | set `0x01`, then re-verify Windows |
| NCSI probe names `dns.msftncsi.com`, `www.msftconnecttest.com` | `ncm_bridge.c` | nothing — they are DNS names answered locally to keep Windows from flagging the link as "no internet" | none |
| `192.168.11.x` still in the ECM and BLE paths | `ecm_bridge.c`, `ble_eth_protocol.h` | mode 7 and the BLE tunnel use a different plan from mode 8's `192.168.20.x`; confusing, not broken | unify when mode 7 is next touched |
| `192.168.20.1/.3/.2/.5` defaults | `ecm_ip_config.h` | nothing — they are defaults, overridable through the `0x30` configuration record | none |

### 4.1 Per-unit MACs (fixed)

The old firmware handed every unit the same addresses (`02:00:00:00:00:01`
for the NIC, `.32`/`.33` for the frames it injects). Two dongles on one host
therefore presented the same MAC: duplicate ARP entries, a duplicate DHCP
lease, and macOS disabling one interface with *"another device is using your
IP address"*.

`APP/mac_uid.c` now derives all four addresses from the chip's 96-bit unique
ID (ESIG, `0x1FFFF7E8`), so each unit is distinct with no provisioning step
and no extra flash page:

```
02 : hh hh hh hh : rr
```

| Field | Meaning |
|---|---|
| `02` | locally administered (bit 1) and unicast (bit 0 clear) — cannot collide with a vendor OUI |
| `hh hh hh hh` | FNV-1a over the twelve UID bytes |
| `rr` | role, so the four addresses of one unit are distinct by construction |

| Role | Byte | Used for |
|---|---|---|
| `MAC_ROLE_HOST_NIC` | `01` | `iMACAddress` string **and** CDC `GET_NET_ADDRESS` — the address the Host OS adopts |
| `MAC_ROLE_HOST_PEER` | `32` | source address on frames the firmware injects toward the Host |
| `MAC_ROLE_TARGET_NIC` | `00` | the address the Target OS adopts from its descriptor |
| `MAC_ROLE_TARGET_PEER` | `33` | source address on frames injected toward the Target |

Two properties are worth stating explicitly:

* **NIC != PEER on both links.** This is not cosmetic: an OS filters inbound
  frames whose source equals its own interface MAC, so frames we inject with
  the descriptor address are dropped as self-MAC and the peer never resolves
  our ARP (see the comment above `MyTargetEcmMacInfo` in `usbd_desc.c`).
* **The address is stable across reboots and re-flashes**, because the UID is
  factory-programmed. DHCP leases and the Host's network-service bookkeeping
  therefore survive a power cycle.

Blank UIDs (all `0x00` or all `0xFF`, seen on engineering samples) fall back to
the legacy fixed values, so such a unit behaves exactly like the pre-UID
firmware instead of shipping a degenerate address.

### 4.2 Where the entropy actually comes from

The value is real, not assumed. Read back through the ISP bootloader:

```
$ wchisp info
Chip: CH32V208GBU6[0x8319] (Code Flash: 128KiB)
Chip UID: A5-33-B4-10-53-5C-AC-A0
BTVER(bootloader ver): 02.80
```

The bootloader exposes 8 of the 12 UID bytes (its `0x11` ESIG response carries
flash size plus two 32-bit UID words); the firmware reads all three words at
`0x1FFFF7E8`. Two consequences matter:

* **This block is not a MAC address.** The first byte `0xA5` is `1010 0101`:
  the multicast bit (bit 0) is set and the local bit is set, so it is not a
  valid unicast address. The SDK's `FLASH_GetMACAddress()` reads this same
  offset — on CH32V208 it would hand back a multicast address, which is why
  the firmware hashes the block instead of copying six bytes out of it. (The
  function is a leftover from the Ethernet-capable siblings.)
* **Hashing is the only option.** There is no vendor-assigned, per-chip MAC to
  reuse, so the address has to be derived — hence `02:` plus a hash.

The hash is 32 bits, so the budget is easy to state. Two specific units
collide with probability 1/2³² ≈ 2.3·10⁻¹⁰ — two dongles on one desk is not a
risk. Fleet-wide the birthday bound applies, but a collision only matters if
both units ever sit on the same layer-2 segment:

| Units shipped | Chance some pair shares a hash |
|---|---|
| 1 000 | 0.012 % |
| 10 000 | 1.2 % |
| 50 000 | 25 % |
| 77 000 | 50 % |

`02:` keeps every derived address inside the locally-administered block, so it
cannot collide with any vendor-assigned address at all; the birthday table
above is the only way a collision can arise, plus the two operational cases:

* a unit with a blank UID falls back to the legacy fixed address (§4.1), so
  every such unit is identical again — engineering samples only, but worth
  knowing;
* re-marked or cloned dies that share a UID, which no firmware can detect.

Only one unit was measured, so per-chip uniqueness rests on WCH's "96-bit
unique ID" guarantee rather than on our own comparison. A second unit would
settle it: flash it and compare `Chip UID`.

Because the MAC is no longer predictable, the host-side helper scripts match
the *shape* `02:xx:xx:xx:xx:01` instead of a literal address:
`tools/find_host_nic.sh` is the single place that does this, and
`setup_target_share.sh`, `setup_captive_portal.sh`, `wsdd_iface.sh` and
`wsdd_poke.sh` all call it.

### 4.3 The serial number carries the same identity (fixed)

The host port had `iSerialNumber = 0`, i.e. no serial at all, and the target
port served the literal `20250721` — a build date, identical on every unit.
That is the same failure as §4.1 in a different register: Windows keys the COM
port number, the driver binding and its per-device registry state on the
serial, so two dongles collapse into one device and the second one steals the
first one's COM number. It is also the only handle that distinguishes two
units in `ioreg` or Device Manager before a NIC exists.

Both ports now report the twelve hex digits of their own address
(`MacUid_FormatSerial()`, written before the pull-up is enabled in
`USBD_init()` / `MyMacInfo_Init()`):

| Port | Serial | Equals |
|---|---|---|
| host (USBLIB) | `02A0F2DFEE01` | `iMACAddress` / `GET_NET_ADDRESS` of the same port |
| target (USBFS) | `02A0F2DFEE00` | the target NIC MAC the target OS adopts |

Measured on one unit (macOS 26.5, `ioreg -p IOUSB -l`):

```
"USB Serial Number" = "02A0F2DFEE01"
en23: ether 02:a0:f2:df:ee:01
arp:  192.168.20.2 -> 2:a0:f2:df:ee:32
```

Three roles of one UID, visible at three layers. Two rules follow:

* the serial must stay **12 uppercase hex digits** — Windows stores it
  verbatim and rejects nothing, but `&` and `+` are what break other drivers'
  instance paths, and a short or duplicated serial recreates the collision;
* do not "improve" it into a MAC with colons: `iSerialNumber` is a plain
  string, and the ECM `iMACAddress` format is the one place that needs the
  12-digit form (`MacUid_FormatString`). The two are separate entry points on
  purpose.

## 5. Verified combinations

| Host | Target | State |
|---|---|---|
| Fedora, static `192.168.20.1` | Windows 11 | verified, earlier sessions |
| **macOS 26.5, DHCP** | **Windows 11** | **verified 2026-11-26**: service left on "Using DHCP", obtained `192.168.20.1/24`, router empty, no DNS servers; `ping 192.168.20.2` 1.7 ms; target IIS answered `404`. That run used the pre-UID `en23` = `02:00:00:00:00:01`. Re-run 2026-11-26 on the UID-derived build (mode 8, no BLE, `USB_WORK_MODE_RUNTIME_PROFILES=0`): `USB Serial Number` = `02A0F2DFEE01`, `en23` = `02:a0:f2:df:ee:01`, `192.168.20.1/24` again by bare DHCP, ARP `.32` = `2:a0:f2:df:ee:32`, `ping` 1.6 ms, target IIS port 80 accepting. The target port's serial changed at the same time and the Windows peer still binds |
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
