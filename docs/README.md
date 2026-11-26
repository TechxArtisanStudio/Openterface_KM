# Firmware documentation

Index for the KeyMod / KVM-GO firmware docs. **English is the documentation
language**: new and updated documents are written in English; the Chinese-only
ones are listed in §5 with their status.

Start with the document that matches your job, not with the folder listing.

---

## 1. Read this first

| If you want to… | Read |
|---|---|
| build and flash the firmware | [build-and-flash-guide.md](build-and-flash-guide.md), then [build-flash-cycle.md](build-flash-cycle.md) |
| understand the USB device the firmware presents | [controllable-usb-endpoint-modes.md](controllable-usb-endpoint-modes.md) |
| work on the NCM side channel (DHCP, DNS, portal) | [ncm-bridge.md](ncm-bridge.md) |
| reach files from the target PC | [portal-and-file-access.md](portal-and-file-access.md) |
| touch the CH9329 / HID path | [ch9329-communication-protocol.md](ch9329-communication-protocol.md) |
| work on BLE (tunnel, pairing, OTA) | [ble-eth-protocol.md](ble-eth-protocol.md), [ble-pairing-design.md](ble-pairing-design.md) |
| know whether a fix is already in the tree | §4, *Fix records* |

Two documents are **canonical** and supersede a pile of older notes:
`ncm-bridge.md` (firmware side of the network link) and
`portal-and-file-access.md` (host services and the zero-install contract).
Everything they replace is still in the tree, marked *Superseded*, for history.

---

## 2. Conventions

* **One document per subsystem per question.** Behaviour goes in a reference,
  a session's investigation goes in a dated record. Do not grow a record into a
  reference; write the reference and link back.
* **Status** is declared in the first lines of each document:
  *Canonical* · *Design* · *Reference* · *Plan* · *Record* · *Superseded*.
* **Names**: lowercase with hyphens (`ncm-bridge.md`). Underscore/uppercase
  names are older files kept for link stability.
* **Dates** in records are `YYYY-MM-DD` and the firmware commit is quoted, so a
  claim can be re-checked with `git show`.
* **Verified on hardware** statements must name the firmware commit, the host
  OS and the target OS. Untested proposals belong in a *Plan*.

---

## 3. Build, flash, release

| Document | Status | Covers |
|---|---|---|
| [build-and-flash-guide.md](build-and-flash-guide.md) | Reference | toolchain, `make` flags, WCHISPStudio/MRS flashing, verification |
| [build-flash-cycle.md](build-flash-cycle.md) | Reference | scripted build → ISP → flash loop over the control board UART |
| [build-on-mac.md](build-on-mac.md) | Reference | macOS/Linux toolchain notes, xpack wrapper quirks |
| [software-isp-entry-design.md](software-isp-entry-design.md) | Design | entering the bootloader without touching the BOOT pin |
| [keymod-firmware-update-guide.md](keymod-firmware-update-guide.md) | Guide | end-user update procedure |
| [KEYMOD_PLUS_BETA_NOTICE.md](KEYMOD_PLUS_BETA_NOTICE.md) | Notice | beta firmware caveats for KeyMod Plus |

The repository root also carries `BUILD_README.md` and `TOOLCHAIN_GUIDE.md`
for the WCH MounRiver Studio flow.

---

## 4. Firmware internals

### USB device model

| Document | Status | Covers |
|---|---|---|
| [controllable-usb-endpoint-modes.md](controllable-usb-endpoint-modes.md) | Design | mode model (build-time and runtime profiles), endpoint policy, verification matrix |
| [usb-endpoint-map-mode7.md](usb-endpoint-map-mode7.md) | Reference (ZH) | Mode 7 endpoint map: HID + host ACM + host ECM + target ECM |
| [usb-endpoint-map-mode8.md](usb-endpoint-map-mode8.md) | Reference (ZH) | Mode 8 endpoint map: HID + target CDC-NCM |
| [usb-firmware-industrial-grade-optimization.md](usb-firmware-industrial-grade-optimization.md) | Plan (ZH) | robustness backlog for the USB stack |
| [internal-memory-usage.md](internal-memory-usage.md) | Reference | RAM/flash budget per build configuration |

### Network side channel

| Document | Status | Covers |
|---|---|---|
| **[ncm-bridge.md](ncm-bridge.md)** | **Canonical** | address plan, data path, DHCP options, one-shot DNS greeting, diagnostics, limits |
| **[portal-and-file-access.md](portal-and-file-access.md)** | **Canonical** | host services (nginx/dnsmasq/file manager/wsdd), WebDAV, SMB, zero-install contract |
| [cdc-ncm-implementation-plan.md](cdc-ncm-implementation-plan.md) | Superseded | original CDC-NCM port plan (descriptor, NTB, memory budget) |
| [ncm-descriptor-debug.md](ncm-descriptor-debug.md) | Superseded | NCM descriptor subtype, `GET_NTB_PARAMETERS`, MAC string fixes |
| [openterface-ncm-troubleshooting-2026-11-24.md](openterface-ncm-troubleshooting-2026-11-24.md) | Record | Windows bring-up session (Code 10, host-port descriptor) |
| [ecm-bridge-dhcp.md](ecm-bridge-dhcp.md) | Superseded | ECM predecessor; DHCP/ARP description still matches the NCM path |
| [ecm-ip-config-design.md](ecm-ip-config-design.md) | Design (ZH) | flash layout, validation and migration of the IP config record |
| [FIRMWARE_DHCP_DEBUG_GUIDE.md](FIRMWARE_DHCP_DEBUG_GUIDE.md) | Superseded (ZH) | field debugging recipes for the DHCP server |
| [target-host-file-access.md](target-host-file-access.md) | Superseded | HTTP + WebDAV + discovery notes for the target |
| [portal-host-served-page.md](portal-host-served-page.md) | Superseded | first write-up of the host-served portal page |

### HID, CH9329 and the target port

| Document | Status | Covers |
|---|---|---|
| [ch9329-communication-protocol.md](ch9329-communication-protocol.md) | Reference | frame format, command map, status codes, project extensions |
| [CH9329_EP3_FIX_SUMMARY.md](CH9329_EP3_FIX_SUMMARY.md) | Record | EP3 enable fix |
| [bugfix-ch9329-command-response-failure.md](bugfix-ch9329-command-response-failure.md) | Record (ZH) | missing command responses |
| [SET_COMMAND_TIMING_FIX.md](SET_COMMAND_TIMING_FIX.md) | Record | `SET` command timing |
| [HID_QUEUE_RELIABILITY_FIX.md](HID_QUEUE_RELIABILITY_FIX.md) | Record | queue redesign for reliable HID delivery |
| [h2t-ep2-double-buffer-fix.md](h2t-ep2-double-buffer-fix.md) | Record | host→target audio EP2 double buffer |
| [t2h-ep4-double-buffer-fix.md](t2h-ep4-double-buffer-fix.md) | Record | target→host audio EP4 double buffer |
| [serial_performance_test.md](serial_performance_test.md) | Record | CH9329 serial throughput measurements |

### BLE

| Document | Status | Covers |
|---|---|---|
| [ble-eth-protocol.md](ble-eth-protocol.md) | Reference | BLE-to-Ethernet tunnel protocol |
| [ble-eth-ssh-usage.md](ble-eth-ssh-usage.md) | Guide | running SSH over the tunnel |
| [ble-eth-tunnel-examples.md](ble-eth-tunnel-examples.md) | Guide | worked tunnel examples |
| [ble-eth-transport-plan.md](ble-eth-transport-plan.md) | Plan | transport improvements |
| [ble-eth-changelog-2026-11-25-pipeline-flowcontrol.md](ble-eth-changelog-2026-11-25-pipeline-flowcontrol.md) | Record | pipelined sends, download flow control, idempotent retransmits |
| [ble-eth-tunnel-test-report-2026-11-24.md](ble-eth-tunnel-test-report-2026-11-24.md) | Record | tunnel test report and fixes |
| [ble-ip-tunnel-design.md](ble-ip-tunnel-design.md) | Design (ZH) | generic IP-over-BLE tunnel |
| [ble-pairing-design.md](ble-pairing-design.md) | Design (ZH) | pairing and security model |
| [ble-pairing-security-user-guide.md](ble-pairing-security-user-guide.md) | Guide | pairing code from the user's side |
| [ble-ota-design.md](ble-ota-design.md) | Design (ZH) | BLE OTA update |
| [BLE_ADV_PARAM_0x19_COMMAND_SPECIFICATION.md](BLE_ADV_PARAM_0x19_COMMAND_SPECIFICATION.md) | Reference (ZH) | advertising-parameter command `0x19` |

### Performance, memory and stability

| Document | Status | Covers |
|---|---|---|
| [queue-performance-optimization-plan.md](queue-performance-optimization-plan.md) | Plan | queue send-path optimisation and expected gains |
| [STABILITY_FIX_PLAN.md](STABILITY_FIX_PLAN.md) | Plan (ZH) | stability workstream |
| [usb-serial-recovery-issue.md](usb-serial-recovery-issue.md) | Analysis | serial recovery failure and fix checklist |

---

## 5. Record: fixes verified on hardware

These are dated investigations. Read them for *how a symptom was diagnosed*,
not for current behaviour — the canonical doc wins if they disagree.

| Document | Date | Subject |
|---|---|---|
| [openterface-ncm-troubleshooting-2026-11-24.md](openterface-ncm-troubleshooting-2026-11-24.md) | 2026-11-24 | NCM bring-up on Windows (Code 10, truncated host descriptor) |
| [ble-eth-tunnel-test-report-2026-11-24.md](ble-eth-tunnel-test-report-2026-11-24.md) | 2026-11-24 | BLE tunnel throughput and reliability |
| [ble-eth-changelog-2026-11-25-pipeline-flowcontrol.md](ble-eth-changelog-2026-11-25-pipeline-flowcontrol.md) | 2026-11-25 | pipeline + flow control changes |
| [target-usb-error-71-fix.md](target-usb-error-71-fix.md) | — | target suspend/resume `error -71` |
| [target-usb-host-esp32-flash.md](target-usb-host-esp32-flash.md) | — | flashing an ESP32 through the target port |

The NCM commits worth knowing by name: `d00646c` (NCSI QNAME length),
`e1566da` (one-shot greeting), `5b3aff4` (no gateway + DHCP before discovery
relay), `35e358c` / `d228068` (portal UI: clickable icons, `no-store`).

---

## 6. Chinese-only documents

These are accurate but not yet translated. When one is touched for content
reasons, translate it and drop it from this list.

| Document | Subject | English replacement |
|---|---|---|
| [ecm-ip-config-design.md](ecm-ip-config-design.md) | IP config flash record | §7 of [ncm-bridge.md](ncm-bridge.md) |
| [FIRMWARE_DHCP_DEBUG_GUIDE.md](FIRMWARE_DHCP_DEBUG_GUIDE.md) | DHCP debugging recipes | §4 and §10 of [ncm-bridge.md](ncm-bridge.md) |
| [usb-endpoint-map-mode7.md](usb-endpoint-map-mode7.md), [usb-endpoint-map-mode8.md](usb-endpoint-map-mode8.md) | endpoint maps | none yet — tables are still authoritative |
| [ble-ip-tunnel-design.md](ble-ip-tunnel-design.md), [ble-pairing-design.md](ble-pairing-design.md), [ble-ota-design.md](ble-ota-design.md) | BLE design | none yet |
| [BLE_ADV_PARAM_0x19_COMMAND_SPECIFICATION.md](BLE_ADV_PARAM_0x19_COMMAND_SPECIFICATION.md) | advertising command `0x19` | none yet |
| [bugfix-ch9329-command-response-failure.md](bugfix-ch9329-command-response-failure.md), [STABILITY_FIX_PLAN.md](STABILITY_FIX_PLAN.md), [usb-firmware-industrial-grade-optimization.md](usb-firmware-industrial-grade-optimization.md) | fixes / plans | none yet |

---

## 7. Adding a document

1. Pick the section above; if none fits, the split is wrong — fix the split
   first.
2. Write the reference, not another record, unless the value is the
   investigation itself.
3. Put the status line and the verification date/commit at the top.
4. Link it from this index **and** from the nearest canonical document.
5. If it replaces something, add a one-line banner to the old file pointing
   here instead of deleting it — links from issues and commits must keep
   resolving.
