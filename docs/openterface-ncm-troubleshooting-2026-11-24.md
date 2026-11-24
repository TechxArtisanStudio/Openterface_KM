# CDC-NCM Bridge Troubleshooting & Fix Record

- **Date**: 2026-11-24
- **Hardware**: KeyMod dev kit (CH32V208, dual USB controllers) + control board
- **Firmware**: Openterface_KM `feature/cdc-ncm` branch, keymod-plus build (`USB_WORK_MODE=hid_ncm_bridge`, `USB_WORK_MODE_RUNTIME_PROFILES=0`)
- **Test machines**: Fedora (host port, `bbot@192.168.100.100`), Windows 11 (target port, `admin@192.168.100.63`)
- **Final firmware version**: bcdDevice `0x0402` (REV_0402)

---

## 1. Background

Before end-to-end BLE-ETH tunnel validation, both wired CDC-NCM bridge links of the KeyMod dev kit needed to be verified healthy. IP plan (preset "20" in `tools/set_ecm_ip.py`):

| Role | IP | Link |
|---|---|---|
| Host IP | 192.168.20.1 | Host port (KM chip → dev machine) |
| Dev IP | 192.168.20.3 | Host port (device side, DHCP server) |
| Target Host IP | 192.168.20.2 | Target port (target PC side) |
| Target Dev IP | 192.168.20.5 | Target port (device side) |

## 2. Symptoms

1. **Fedora (host port)**: "Openterface Host" (1a86:fe0c, 1.04) failed to enumerate; kernel reported
   `config 1 has an invalid descriptor of length 0, 0 interfaces` → `.1/.3` missing.
   (Target port `.2/.5` was healthy: DHCP lease in 18 ms, ARP/ICMP all passing.)
2. **Windows (target port)**: composite device HID interfaces fine, but `MI_04` (NCM data interface)
   errored out — progressed through Code 56 (awaiting class config) → Code 10 (cannot start);
   NIC never appeared.

## 3. Investigation

### Phase 1: Target-side verification and IP discovery

- Docs/scripts assume device IP is `.3`; pings all failed (no ARP replies).
- Via the **DHCP server identifier** we found the device's actual IP is **`.5`** — ping succeeded immediately.
- Conclusion: target-side link fully healthy; the `.3` assumption in docs/scripts needs updating.

### Phase 2: Host-port truncated descriptor (Fedora)

Evidence chain:

1. `sudo cat /sys/bus/usb/devices/<dev>/descriptors`: device returned only 27 bytes
   (18-byte device descriptor + 9-byte config header); the header claims `wTotalLength=0x4B` (75), 2 interfaces.
2. authorize toggle and `USBDEVFS_RESET` hard reset both reproduced it → not a timing issue.
3. Raw control transfers via pyusb: requesting 75 bytes returns 75 bytes, but **everything after
   byte 9 is zero**.
4. The same pattern was found at **offset 0x35024** of `output/openterface-km-firmware-keymod-plus.bin`
   — the board runs this exact binary, so the bug is in current source.

**Root cause**: in mode 8 (hid_ncm_bridge), `USB_WORK_MODE_HAS_HOST_CDC_ACM/ECM` are both false,
so the config descriptor **table body in `APP/USBLIB/CONFIG/usb_desc.c` is compiled out entirely**
by `#if` guards, leaving a 9-byte header + C zero padding.

Hardware topology clarified along the way (essential context):

| USB controller | Stack | Port | Device |
|---|---|---|---|
| USBFS @0x50000000 | `ch32v20x_usbfs_device.c` | Target | `fe00` HID×4 + CDC-NCM |
| USB @0x40006000 (PMA) | `APP/USBLIB/` | Host | `fe0c` 1.04 "Openterface Host" |
| Control board (separate chip) | — | — | `fe0c` 1.00 "USB Serial" (flashing/BOOT/power) |

Mode 8's host port is designed to expose a **CDC-NCM NIC** (for `.1/.3`): the endpoint callbacks
(EP5 IN notify / EP3 OUT / EP4 IN) and `NCM_Bridge_HostRxDone/Poll` already existed; missing were
only the descriptor block and endpoint initialization.

### Phase 3: Implementing USBLIB host-side NCM (firmware changes)

| File | Change |
|---|---|
| `usb_desc.c/h` | Added mode-8 NCM config descriptor block (9+93=102 B, with IAD, NCM functional descriptor, iMACAddress=4); `USBD_SIZE_CONFIG_DESC` mode-8 branch; `USBD_GetActiveConfigDescriptorData` dispatch |
| `usb_conf.h` | PMA buffer addresses (`ENDP3_NCM_RXADDR`, etc.) |
| `usb_prop.c` | EP3/EP4/EP5 NCM init in `USBD_Reset`; SET_INTERFACE(alt=1) enables EP3 RX / EP4 TX; **GET_NTB_PARAMETERS** (bRequest 0x80) — a hard requirement for Linux cdc_ncm to bind, it STALLs and gives up without it |
| `usb_endp.c` | `EP3_OUT_Callback`: multi-packet NTB reassembly (PMA can't hold a full NTB, fed 64 B at a time) |
| `ncm_bridge.c` | Host-side TX chunked send (64 B per packet + ZLP); fixed `host_tx_feed` deadlock when NTB length is not a multiple of 64; removed a debug `return;` |
| `ch32v20x_usbfs_device.c` | Target-port Microsoft OS 1.0 descriptors (0xEE string + vendor request 0x52 with CompatibleID "WINNCM", bFirstInterface=4); vendor-request wLength capping |

### Phase 4: Diagnostics channel (memory-read interface, no printf)

- **Discovery**: CH9329 commands on `ttyACM0` are handled by the **control board firmware's own
  whitelist** (0x01/0x02–0x05/0x08/0x17/0x18 etc.); unknown commands (including the KM firmware's 0x33)
  are rejected with `0xF3/0xE3` and **never forwarded** — the earlier conclusion that "0x30/0x27 are
  gated by `USB_WORK_MODE`" was wrong.
- The only reachable entry into the KM chip's CH9329 dispatcher is **BLE GATT fff2 writes**
  (fff1/fff2 are both `GATT_ENCRYPT_WRITE`, requiring an encrypted link; pairing PIN 666666;
  factory BLE name "KVM-GO", this unit's actual name is KeyMod-Fedora).
- Added `CMD_ISP_DEBUG(0x33) sub_cmd 0x10` in the KM firmware: `NCM_Bridge_GetHostDiagnostics()`
  returns a 56-byte memory frame with EP3/4/5 endpoint registers, NTB reassembly state, TX queue,
  ARP/ICMP/DHCP counters, and IP configuration.
- On the Mac, bleak (`/tmp/bleenv`) reads diagnostics over BLE; running the CLI directly trips macOS
  TCC (SIGABRT) — it must be run once from Terminal.app to grant Bluetooth permission.

First diagnostics snapshot (all healthy): `ep3_rx_pkts=74, ntb_sig_ok/parsed/host_frames=23/23/23`,
`dhcp_offer=1/tx_sent=1` (the 372 B OFFER went out fully), `arp_req=6/arp_match=0`
(NM duplicate-address probes, expected).

### Phase 5: Fedora host port verified

- Fedora DHCP obtained **192.168.20.1** (server id = .3); ping .3 OK, device ARP MAC `02:00:00:00:00:32`.
- Large frames 1400 B 5/5, rate test 500 B×100 100/100, zero loss.
- Gotcha: on Fedora, both NICs (`u2u2` = .1 host link, `u5i4` = .2 target link) share the **same
  subnet** — a ping without `-I` rides the lower-metric target link. One "total bridge freeze" was
  actually a mis-targeted test.
- Fixed `g_host_ncm_data_if_active` never being set (SetInterface alt=1 sets / alt=0 clears).

### Phase 6: Windows Code 10 (main line of this session)

Path and evidence:

1. `Get-PnpDevice`: MI_04 matched the Microsoft in-box **usbncm.inf**; ProblemCode **0x38
   (CM_PROB_NEED_CLASS_CONFIG)**.
2. Reading `DriverStore\...\usbncm.inf`: the driver only matches hardware ID **`USB\MS_COMP_WINNCM`**
   — i.e. the device must provide Microsoft OS descriptors (CompatibleID="WINNCM"); a bare
   CDC-NCM class match will never start.
3. After implementing the MS OS 1.0 descriptors, the Windows `usbflags` registry gained
   `1A86FE00040x osvc = 1, 82` — both the 0xEE string and vendor-0x52 queries **succeeded**,
   yet MI_04 still hit Code 10 (and was class-matched, Rank 0xFF3003, meaning the CompatibleID
   wasn't adopted).
4. Detours along the way:
   - Two KeyMod units were plugged into the Windows machine simultaneously (an older one with
     serial 20250721); instance paths had to be disambiguated by REV_040x;
   - Windows keeps a **negative cache** of MS OS descriptors: the firmware must also bump
     `bcdDevice` (0x01→0x02) to force a re-query;
   - `pnputil` node removal + rescan, and control-board power cycles, each verified one cache behavior;
   - Control board power unresponsive/ineffective → **rebooting Fedora restores it** (twice);
   - `ctrl_board.py flash`'s verification step requires the target device on the same machine;
     with the target on Windows it falsely reports "verification failed" — use
     `isp on` + `wchisp flash` manually instead (Verify OK, 224256 B).
5. **Final root cause**: reading the startup handshake in Microsoft's official
   [NCM-Driver-for-Windows](https://github.com/microsoft/NCM-Driver-for-Windows) source revealed two
   descriptor defects (Windows fails validation → `STATUS_DEVICE_HARDWARE_ERROR`; Linux is lenient):
   - **NCM functional descriptor (subtype 0x1A) length wrong**: we emitted 14 bytes (ECM fields
     `wMaxSegmentSize/wNumberMCFilters/bNumberPowerFilters` appended after `bcdNCMVersion`);
     the driver requires **6 bytes** (`bcdNCMVersion[2] + bmNetworkCapabilities[1]`) →
     `bFunctionLength` set to `0x06`, `bmNetworkCapabilities` corrected to a single byte `0x02`
     (NET_ADDRESS);
   - **Data interface `bInterfaceProtocol = 0x00`**: the driver requires
     `USB_DATA_INTERFACE_PROTOCOL_NCM = 0x01` → both mode-8 NCM data interfaces (alt 0 / alt 1)
     changed to `0x01`.
   - `wTotalLength` 202→194 synced (both static asserts updated).

## 4. Final verification

### Fedora (host port, after fix)

| Check | Result |
|---|---|
| Enumeration | cdc_ncm NIC appears (`enp0s21f0u2u2`) ✓ |
| DHCP | 192.168.20.1 (server=.3) ✓ |
| ping .3 | OK, ~5-10 ms ✓ |
| Large frames 1400 B | 5/5, 0% loss ✓ |
| Rate 500 B ×100 | 100/100, 0% loss ✓ |

### Windows (target port, REV_0402)

| Check | Result |
|---|---|
| MI_04 driver | `usbncm.inf`, HWID match, ProblemCode=0 ✓ |
| NIC | "TechxArtisan Openterface Target" Up ✓ |
| DHCP | 192.168.20.2 (server=.3) ✓ |
| ping .5 | 5/5, 0% loss, avg 1 ms ✓ |
| Large frames 1200 B | 3/3, 0% loss, ~3 ms ✓ |

**All four IPs are up**; both wired bridges for the BLE-ETH tunnel are verified.

## 5. Lessons learned

1. **Dual-controller, dual-stack architecture**: USBFS (target) and USBLIB/PMA (host) are two
   independent stacks on the CH32V208 — first establish which stack owns the failing device before
   debugging. The control-board chip only does flashing/BOOT/power; it carries no KM data plane.
2. **Cross-validate descriptor issues**: `/sys/.../descriptors`, raw pyusb control transfers, and
   firmware .bin content search corroborate each other — avoids misdiagnosing as "old firmware".
3. **Windows is far stricter than Linux for NCM**: Linux cdc_ncm tolerates `bInterfaceProtocol=0`
   and oversized NCM functional descriptors; the Microsoft driver validates both per spec.
   **Passing on Linux does not imply working on Windows.**
4. **Windows negative-caches MS OS descriptors**: bump `bcdDevice` whenever descriptors change,
   or Windows keeps reusing the cached `osvc={0,0}` result and never re-queries.
5. **Real topology of the serial command channel**: CH9329 commands on `ttyACM0` are
   intercepted/answered by the control board's whitelist and never reach the KM chip;
   internal KM state diagnostics must go over BLE GATT (encrypted, paired).
6. **Always bind the interface in tests with same-subnet dual NICs** (`ping -I`), or traffic rides
   the lower-metric link and creates phantom failures.
7. **Control-board power GPIO hangs**: rebooting Fedora (which resets the control board) recovers
   it; physical replug also works.
8. **Cross-machine flashing**: `ctrl_board.py flash`'s verification expects the target on the same
   machine; use the manual three steps `isp on` → `wchisp flash` → `isp off` otherwise.

## 6. Appendix: key tools & commands

```bash
# Flashing (target on another machine)
python3 tools/ctrl_board.py isp on                 # enter ISP (BOOT HIGH + power cycle)
~/.cargo/bin/wchisp flash output/openterface-km-firmware-keymod-plus.bin
python3 tools/ctrl_board.py isp off                # exit ISP (BOOT LOW + power cycle)

# Build
./build.sh --compiler <MRS-RISC-V-GCC> --make-arg USB_WORK_MODE=hid_ncm_bridge \
  --make-arg USB_WORK_MODE_RUNTIME_PROFILES=0 keymod-plus

# KM internal diagnostics (BLE GATT, requires pairing; bleak script /tmp/ble_diag.py)
# 0x33 sub 0x10 → 56-byte frame: EPnR / NTB reassembly / ARP / ICMP / DHCP counters / IP config

# Windows-side checks
Get-PnpDevice | ? { $_.InstanceId -like '*FE00*MI_04*' }   # NCM interface status
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\usbflags\1A86FE00*'  # MS OS descriptor cache
# usbncm.inf only matches: USB\MS_COMP_WINNCM

# Verify descriptors embedded in the binary
python3 - <<'EOF'
data = open("output/openterface-km-firmware-keymod-plus.bin","rb").read()
print(data.find(bytes([0x12,0x03]) + "MSFT100".encode("utf-16-le") + bytes([0x52,0x00])))  # 0xEE string
print(data.find(b"WINNCM"))  # CompatibleID
EOF
```

## 7. Open items

- The other KeyMod on the bench (serial 20250721, REV_0400) has the same failing MI_04;
  flashing this firmware will fix it too.
- `diagnose_ecm_ip.sh`, `test_ecm_connectivity.sh` and friends still assume device IP `.3`,
  while Flash actually holds `.5` on the target side — update the scripts or normalize the
  Flash config.
- The control board's power GPIO hangs intermittently (twice today; reboot/replug recovers) —
  worth a dedicated investigation into the control-board firmware.
