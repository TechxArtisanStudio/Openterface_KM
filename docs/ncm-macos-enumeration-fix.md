# Record: the NCM function now binds on macOS (2026-11-26)

Status: **Record**
Date: 2026-11-26
Firmware: `0bafa81` (mode 8, `PRODUCT=keymod-plus`)
Host: macOS 26.5 (build 25F71), Apple silicon
Device: KeyMod Plus, host port, full speed (12 Mbit/s)

## Symptom

The dongle enumerated as `Openterface Host` (`1a86:fe0c`) and macOS loaded a
driver for the control interface, but no network interface appeared — neither in
System Settings nor in `networksetup -listallhardwareports`.

## Evidence

```
+-o Openterface Host@02112000
  +-o IOUSBHostInterface@0
  | +-o AppleUSBNCMControl          ← matched
  +-o IOUSBHostInterface@1          ← no driver, no en* child
```

The interface properties macOS had parsed:

```
IOUSBHostInterface@1: bInterfaceClass 10, bInterfaceSubClass 0, bInterfaceProtocol 0
```

and the personality that was supposed to claim it, from
`/System/Library/Extensions/AppleUSBNCM.kext/Contents/Info.plist`:

```xml
<key>AppleUSBNCMData0</key>
<dict>
  <key>bInterfaceClass</key>     <integer>10</integer>
  <key>bInterfaceSubClass</key>  <integer>0</integer>
  <key>bInterfaceProtocol</key>  <integer>1</integer>
  <key>IOClass</key>             <string>AppleUSBNCMData</string>
</dict>
```

`AppleUSBNCMControl` matches the communication interface with
`bInterfaceProtocol = "*"`, which is why half the function looked healthy: the
control driver attached, the data driver never did, and no `IOEthernetInterface`
was published.

## Root cause

The data interface declared `bInterfaceProtocol = 0x00`. CDC-NCM requires `0x01`
on the data interface, and macOS matches on it exactly. Both alternate settings
(alt 0 with no endpoints, alt 1 with the bulk pair) carried the wrong value.

Two further defects on the same port, fixed in the same commit:

| Defect | Effect |
|---|---|
| NCM functional descriptor was 14 bytes, with a four-byte `bmNetworkCapabilities` where the spec has one byte | Windows refuses the device at Code 10; the extra `wMaxSegmentSize`/filter fields belong to the Ethernet Networking descriptor |
| class requests used a shifted numbering (`0x87` = input size, `0x89` = format, `0x8A` = max datagram) | no peer sends those codes; Apple probes `0x81`–`0x8A` per NCM 1.0 and would have stalled |

## Fix

* `bInterfaceProtocol = 0x01` on both data alt settings in
  `USBD_NcmConfigDescriptor`.
* NCM functional descriptor shrunk to the spec's 6 bytes
  (`0x06 0x24 0x1A` + `bcdNCMVersion` + `bmNetworkCapabilities`), config
  descriptor 102 → 94 bytes.
* Request table aligned with NCM 1.0: `0x81`/`0x82` net address, `0x83`/`0x84`
  NTB format, `0x85` input size, `0x87`/`0x88` max datagram, `0x89`/`0x8A` CRC
  mode; `0x86` keeps `SET_NTB_INPUT_SIZE` because Linux `cdc_ncm` sends it.

## Verification

```
en23: flags=8863<UP,BROADCAST,SMART,RUNNING,SIMPLEX,MULTICAST> mtu 1500
      ether 02:00:00:00:00:01
      media: autoselect (10baseT/UTP <full-duplex>)
      status: active

Hardware Port: Openterface Host
Device: en23
Ethernet Address: 02:00:00:00:00:01
```

The MAC comes from the `iMACAddress` string, as intended. This capture predates
the UID derivation (`APP/mac_uid.c`): current builds answer
`02:xx:xx:xx:xx:01`, which is the same role byte with a per-unit middle
([cross-os-portability.md](cross-os-portability.md) §4.1). The interface has no
IPv4 address yet: the firmware's DHCP server serves the **target** port, so the
host side is configured statically, exactly as on the Fedora deployment:

```bash
sudo ifconfig en23 inet 192.168.20.1 netmask 255.255.255.0
```

## Leftovers

* The host port still serves no Microsoft OS descriptors, so a **Windows host**
  remains unsupported; macOS and Linux bind through their class drivers.
* The target port keeps `bInterfaceProtocol = 0x00` on its NCM data interface.
  That is deliberate: the Windows 11 path is verified and `UsbNcm.sys` does not
  match on the protocol. If a Mac ever has to drive the target port, the same
  change (protocol `0x01` on both alts) is required there.
* Interface numbers are not stable across re-enumeration — macOS allocated
  `en23` here because earlier USB NICs had taken `en5`–`en7`.
