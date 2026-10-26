# CH9329-Compatible Communication Protocol Summary

## Scope

This firmware implements a CH9329-compatible command protocol for backward compatibility with the original Openterface MiniKVM software stack.

If you meant WCH CH3929 specifically, that is a different part family. This repository does not contain CH3929-specific protocol code; the implementation here is the CH9329-style framing and command set used to drive keyboard and mouse emulation.

## Transport In This Firmware

The protocol framing is independent from the physical transport.

- Incoming frames are fed into the parser from either BLE or the UART2 / CDC ACM path.
- Parsed commands are translated into USB HID reports and queued for the target-facing USB device stack.
- Protocol responses use the same CH9329 frame format and are queued back to the upstream side.

In other words, the protocol is the control layer, while BLE or CDC ACM is just the carrier.

## Frame Format

Each request frame uses this layout:

| Byte | Meaning |
| --- | --- |
| 0 | Header 1 = `0x57` |
| 1 | Header 2 = `0xAB` |
| 2 | Address |
| 3 | Command |
| 4 | Payload length `N` |
| 5..(4+N) | Payload |
| 5+N | Checksum |

Checksum is the 8-bit additive sum of every byte before the checksum field.

Example request:

```text
57 AB 00 02 08 <8 bytes keyboard payload> CS
```

### Parser Behavior

- If the header is wrong, the parser drops one byte and keeps scanning.
- If the full frame has not arrived yet, the parser waits for more data.
- If checksum validation fails, the parser skips the entire frame length.
- Valid frames are dispatched immediately.

## Response Encoding

Responses reuse the same frame structure, but the command byte is modified:

- Positive response or data reply: `cmd | 0x80`
- Negative response or error status: `cmd | 0xC0`

The response payload is usually a 1-byte status for write-like commands, or a structured data block for read/status commands.

## Status Codes

| Code | Meaning |
| --- | --- |
| `0x00` | Success |
| `0xE1` | Timeout |
| `0xE2` | Header error |
| `0xE3` | Unsupported command |
| `0xE4` | Checksum error |
| `0xE5` | Parameter error |
| `0xE6` | Frame / execution error |
| `0xE9` | Flash page erase failed (hardware fault) |
| `0xEA` | Flash word program failed (hardware fault) |
| `0xEB` | Flash write-back verification failed (data mismatch) |

## Implemented Commands

| Cmd | Name | Payload | Response | Notes |
| --- | --- | --- | --- | --- |
| `0x01` | Get info | none | 8-byte status block | CH9329 compatibility readback |
| `0x02` | Keyboard report | 8 bytes | 1-byte status | Standard keyboard input |
| `0x03` | Media / ACPI report | 2 or 4 bytes | 1-byte status | Media keys and ACPI power keys |
| `0x04` | Absolute mouse | at least 7 bytes | 1-byte status | Absolute HID mouse or stylus mapping |
| `0x05` | Relative mouse | at least 5 bytes | 1-byte status | Relative HID mouse mapping |
| `0x08` | Get parameter config | none | 44-byte fixed blob | Compatibility response only |
| `0x17` | SD switch | board-specific | board-specific | Openterface extension |
| `0x18` | Temperature | board-specific | board-specific | DS18B20 extension |
| `0x22` | BLE security | 1-byte sub-command | status + sub-command data | Openterface extension for BLE pairing/bond management |
| `0x23` | BLE set name | 1-20 bytes name or `0x00` | 1-byte status | Openterface extension for BLE device name configuration |
| `0x26` | Set ECM IP config | sub-command + data | 1-byte status | Openterface extension for ECM network IP configuration |
| `0x27` | Get ECM IP config | none | 21-byte config block | Openterface extension for reading ECM network configuration |
| `0x30` | USB work mode | optional control payload | 12-byte status block | Openterface extension for runtime USB personality selection |
| `0x31` | System reset | none | 1-byte status | Openterface extension for system reboot |

## Command Details

### `0x01` Get Info

Returns a fixed 8-byte payload:

| Byte | Meaning |
| --- | --- |
| 0 | Hardware selector (board version): `0x01` = KVM-GO, `0x02` = MiniKVM, `0x03` = KeyMod |
| 1 | Target USB enumeration state: `0x01` connected, `0x00` not connected |
| 2 | Keyboard LED bits (`CapsLock`, `NumLock`, `ScrollLock`) |
| 3 | Firmware version number (e.g., `0x05` = version 5) |
| 4..7 | Reserved / placeholder bytes |

This is used as a compact device-status query, not just a static version read.

### `0x02` Keyboard Report

Expected payload length is exactly 8 bytes.

Current mapping in firmware:

- `data[0]`: modifier bitmap
- `data[1]`: ignored / reserved
- `data[2]..data[7]`: up to 6 keycodes
- Any keycode in the `0xE0..0xE7` range is folded into the modifier bitmap

The resulting HID report is:

```text
[modifier][reserved=0][key1][key2][key3][key4][key5][key6]
```

Important behavior note: the current implementation only releases keys when the sender explicitly sends an all-zero keyboard report.

### `0x03` Media / ACPI Report

Two subformats are accepted:

- ACPI: `data[0] = 0x01`, `data[1] = acpi_code`
- Media: `data[0] = 0x02`, `data[1..3] = 24-bit bitmap`

Duplicate media frames are suppressed if they match the last report.

### `0x04` Absolute Mouse

Expected payload is at least 7 bytes:

```text
[reserved][buttons][x_lo][x_hi][y_lo][y_hi][wheel]
```

Behavior:

- Coordinates are read as little-endian 16-bit values.
- Coordinates are remapped into the HID absolute range used by the firmware.
- Output format depends on the build:
  - default absolute mouse report: 6 bytes
  - stylus build with `EP3_USE_STYLUS`: 5 bytes

### `0x05` Relative Mouse

Expected payload is at least 5 bytes:

```text
[reserved][buttons][dx][dy][wheel]
```

The firmware forwards this as a standard 4-byte relative HID mouse report.

### `0x08` Get Parameter Configuration

This command returns a fixed 44-byte compatibility blob. The current firmware does not decode or build this dynamically; it is present to match CH9329-compatible software expectations.

### `0x17` SD Switch

This is an Openterface-specific extension and is not part of the minimal keyboard/mouse CH9329 compatibility set. It is delegated to the SD card switching logic.

### `0x18` Temperature

This is an Openterface-specific extension for the DS18B20 temperature sensor path.

### `0x22` BLE Security

Openterface extension managing BLE pairing and bonds (see `docs/ble-security.md`).
The payload is a single sub-command byte; responses follow the standard
`cmd | 0x80` (success) / `cmd | 0xC0` (error) encoding.

| Sub-cmd | Name | Allowed origin | Response payload |
| --- | --- | --- | --- |
| `0x01` | Open pairing window | USB, BLE | status, remaining window seconds (u16 LE); when issued over USB additionally the 6-digit passkey (u32 LE). The passkey is never sent over BLE. |
| `0x02` | Erase all bonds | USB only | status. Erases every bond and clears the BLE whitelist. BLE-originated requests are rejected with a frame error. |
| `0x03` | Bond count | USB, BLE | status, number of stored bonds (u8) |

Status values follow the status code table above. `0x01` and `0x03` can also be
tunneled through the BLE rawpass characteristic once the link is encrypted.

### `0x23` BLE Set Name

This is an Openterface extension for dynamic BLE device name configuration. The name is persisted to flash and survives power cycles.

**Payload format:**

- 1-20 bytes: set new BLE device name (ASCII string, no null terminator required)
- Empty payload or `0x00` byte: returns `0xE5` parameter error (use `0x31` with `0x00` to reset to default)

**Response:**

- Single byte status (`0x00` = success, `0xE5` = parameter error, `0xE9` = flash erase failed, `0xEA` = flash program failed, `0xEB` = flash verification failed)
- Response command byte uses `cmd | 0x80` for success, `cmd | 0xC0` for error (matches protocol convention)

**Examples:**

Set name to `"kvmgo-test"`:

```text
Request : 57 AB <addr> 23 0A 6B 76 6D 67 6F 2D 74 65 73 74 <sum>
Response: 57 AB <addr> A3 01 00 <sum>
```

Empty payload (error):

```text
Request : 57 AB <addr> 23 00 <sum>
Response: 57 AB <addr> E3 01 E5 <sum>
```

**Important limitations:**

1. **Automatic reboot after name change**: After a successful name change, the device will automatically reboot to fully apply the new name. The response is sent before reboot, so the host will receive confirmation before the device disconnects.
   
2. **Compile-time dependency**: This command is only available when the firmware is built with `ENABLE_BLE` defined. If BLE is disabled at compile time, the command returns `0xE3` (unsupported command).

3. **Name length limit**: Maximum 20 characters. Longer names are silently truncated.

4. **Flash wear**: Each name change writes to flash. Avoid frequent name changes in automated scripts.

5. **No reset functionality**: This command only sets new names. To reset to factory default without reboot, use `0x31` with `0x00` payload.

**Usage notes:**

After sending a successful `0x23` command, the device will reboot automatically. The host should expect a disconnection followed by reconnection as the device re-advertises with the new name.

**Recommended workflow:**

```text
1. Send 0x23 with new name
2. Wait for success response
3. Device will automatically reboot
4. After reboot, scan for the device with the new name
```

### `0x26` Set ECM IP Configuration

This is an Openterface extension for configuring the ECM network IP addresses. Changes take effect immediately without requiring USB reconnection or device reboot. The configuration is persisted to flash and survives power cycles.

**Availability:** Only available when the firmware is built with ECM bridge support (modes 4, 5, 6, 7). If ECM is not compiled in, the command returns `0xE3` (unsupported command).

**Payload format:**

The payload starts with a sub-command byte followed by data depending on the sub-command:

| Sub-cmd | Name | Payload length | Description |
| --- | --- | --- | --- |
| `0x00` | Set All | 21 bytes | Set all IP addresses at once |
| `0x01` | Set Host Side | 13 bytes | Set host-side IPs (host IP, dev IP, subnet) |
| `0x02` | Set Target Side | 9 bytes | Set target-side IPs (target host IP, target dev IP) |
| `0x03` | Reset to Default | 1 byte | Reset all IPs to factory defaults |

**Sub-command `0x00` - Set All:**

```text
Payload: [0x00][host_ip(4)][dev_ip(4)][subnet(4)][target_host_ip(4)][target_dev_ip(4)]
Total: 21 bytes
```

**Sub-command `0x01` - Set Host Side:**

```text
Payload: [0x01][host_ip(4)][dev_ip(4)][subnet(4)]
Total: 13 bytes
```

**Sub-command `0x02` - Set Target Side:**

```text
Payload: [0x02][target_host_ip(4)][target_dev_ip(4)]
Total: 9 bytes
```

**Sub-command `0x03` - Reset to Default:**

```text
Payload: [0x03]
Total: 1 byte
```

**Response:**

- Single byte status (`0x00` = success, `0xE5` = parameter error, `0xE9` = flash erase failed, `0xEA` = flash program failed, `0xEB` = flash verification failed)
- Response command byte uses `cmd | 0x80` for success, `cmd | 0xC0` for error

**Default IP Configuration:**

| Parameter | Default Value |
| --- | --- |
| Host IP | 192.168.20.1 |
| Dev IP (firmware) | 192.168.20.3 |
| Subnet Mask | 255.255.255.0 |
| Target Host IP | 192.168.20.2 |
| Target Dev IP | 192.168.20.5 |

**Examples:**

Set all IPs to 192.168.50.x subnet:

```text
Request : 57 AB <addr> 26 15 00 C0 A8 32 01 C0 A8 32 03 FF FF FF 00 C0 A8 32 02 C0 A8 32 05 <sum>
Response: 57 AB <addr> A6 01 00 <sum>
```

Set host side only (192.168.30.x):

```text
Request : 57 AB <addr> 26 0D 01 C0 A8 1E 01 C0 A8 1E 03 FF FF FF 00 <sum>
Response: 57 AB <addr> A6 01 00 <sum>
```

Reset to defaults:

```text
Request : 57 AB <addr> 26 01 03 <sum>
Response: 57 AB <addr> A6 01 00 <sum>
```

**Important notes:**

1. **Immediate effect**: IP changes take effect immediately. The firmware sends GARP (Gratuitous ARP) and triggers link state change to notify the host.

2. **DHCP renewal**: After IP change, the host may need to renew its DHCP lease to get the new IP. On macOS: `networksetup -setdhcp "Openterface Host"`

3. **Flash wear**: Each configuration change writes to flash. Avoid frequent changes in automated scripts.

4. **Validation**: The firmware validates IP addresses before writing. Invalid addresses return `0xE5` parameter error.

### `0x27` Get ECM IP Configuration

This is an Openterface extension for reading the current ECM network IP configuration. Returns the configuration from RAM cache without accessing flash.

**Availability:** Only available when the firmware is built with ECM bridge support (modes 4, 5, 6, 7). If ECM is not compiled in, the command returns `0xE3` (unsupported command).

**Payload format:**

No payload required.

**Response:**

The response is a 21-byte configuration block:

```text
[status(1)][host_ip(4)][dev_ip(4)][subnet(4)][target_host_ip(4)][target_dev_ip(4)]
```

| Byte | Meaning |
| --- | --- |
| 0 | Status (`0x00` = success) |
| 1-4 | Host IP address |
| 5-8 | Dev IP address (firmware) |
| 9-12 | Subnet mask |
| 13-16 | Target host IP |
| 17-20 | Target dev IP |

**Example:**

```text
Request : 57 AB <addr> 27 00 <sum>
Response: 57 AB <addr> A7 15 00 C0 A8 32 01 C0 A8 32 03 FF FF FF 00 C0 A8 32 02 C0 A8 32 05 <sum>
          (192.168.50.1, 192.168.50.3, 255.255.255.0, 192.168.50.2, 192.168.50.5)
```

### `0x30` USB Work Mode

This is a newer Openterface extension for persistent USB mode selection and status reporting.

It is the command to use when host software needs to discover which USB personality the firmware was built for, which one is currently selected, and which ones the running image is capable of supporting.

Unlike the simpler keyboard and mouse commands, `0x30` returns a structured status payload even when the requested subcommand fails. The caller should inspect the first payload byte for operation status instead of relying only on the response command byte.

#### Working Mode Values

The `0x30` command uses numeric mode values defined by the firmware build:

| Mode | Symbol in code | Meaning |
| --- | --- | --- |
| `1` | `USB_WORK_MODE_HID_ACM` | HID on target side, CDC ACM on host side, no bridge |
| `2` | `USB_WORK_MODE_HID_UAC_ACM` | HID plus target/host UAC, with the existing UAC audio bridge |
| `3` | `USB_WORK_MODE_HID_ACM_BRIDGE` | HID plus target CDC ACM, bridged to host CDC ACM |
| `4` | `USB_WORK_MODE_HID_ECM_BRIDGE` | HID plus target CDC ECM, bridged to host CDC ECM |
| `5` | `USB_WORK_MODE_HID_UAC_ECM` | HID plus UAC audio plus CDC ECM |
| `6` | `USB_WORK_MODE_HID_ACM_ECM` | Host CDC-ACM + Host CDC-ECM combined (keymod-plus) |
| `7` | `USB_WORK_MODE_HID_ACM_ECM_BRIDGE` | Host CDC-ACM + Host CDC-ECM + Target CDC-ECM |

Practical interpretation of each mode:

#### Mode `1` - HID + Host CDC ACM

This is the lightest non-audio profile.

- Target-facing USB exposes the HID control functions only: keyboard, relative mouse, absolute mouse or stylus, and media keys.
- Host-facing USB exposes a CDC ACM interface.
- There is no active host-to-target bridge path other than the CH9329 command/control channel itself.

Use this when you want keyboard and mouse control without target-side audio or serial bridging.

#### Mode `2` - HID + UAC Bridge + Host CDC ACM

This is the audio-capable profile.

- Target-facing USB exposes HID plus UAC audio interfaces.
- Host-facing USB exposes CDC ACM plus UAC audio.
- The firmware enables the existing UAC audio bridge between the two USB sides.

Use this when the device should carry both keyboard/mouse control and target audio.

#### Mode `3` - HID + Target CDC ACM + Host CDC ACM

This is the serial bridge profile.

- Target-facing USB exposes HID plus CDC ACM.
- Host-facing USB exposes CDC ACM.
- The bridge path is raw ACM byte transport.

Use this when you want a COM-port style data path between upstream host and target, alongside HID control.

#### Mode `4` - HID + Target CDC ECM + Host CDC ECM

This is the Ethernet bridge profile and also the runtime-capable build profile.

- Target-facing USB exposes HID plus CDC ECM.
- Host-facing USB exposes CDC ECM.
- The bridge path is Ethernet frame transport.
- In the current firmware design, the Mode `4` binary is the one that can also boot into stored Mode `1`, `2`, `3`, or `4` at startup.

Use this when the firmware should expose ECM networking and when you want runtime-switchable USB profiles.

Accepted subcommands:

- no payload or `00`: read current status
- `01 <mode> [flags_lo flags_hi]`: write a persistent mode record into flash
- `02`: clear the stored mode record from flash
- `03`: send status and then reboot immediately

Canonical request payloads are:

```text
[]                     -> read status
[00]                   -> read status
[01, mode]             -> write mode with flags = 0
[01, mode, fl, fh]     -> write mode with explicit 16-bit flags
[02]                   -> clear stored mode record
[03]                   -> acknowledge and reboot
```

### `0x30` Request / Apply Model

The intended host workflow is:

1. Send a read request and inspect supported modes.
2. If needed, send `01 <mode>` to store a new mode selection.
3. Send `03` to reboot and let USB re-enumerate in the new profile.

The write step persists configuration in flash, but the USB-visible descriptor set only becomes authoritative after reset and re-enumeration.

### `0x30` Response Layout

The response is a 12-byte status block:

| Byte | Meaning |
| --- | --- |
| 0 | Status |
| 1 | Compiled mode |
| 2 | Active mode |
| 3 | Stored mode |
| 4 | Config status |
| 5 | Flags low |
| 6 | Flags high |
| 7..10 | Capability mask, little-endian |
| 11 | Reserved |

### `0x30` Field Meanings

#### Byte 0: operation status

This uses the same status codes listed earlier in this document.

- `0x00`: request accepted
- `0xE5`: invalid parameter, unsupported mode, or malformed write request
- `0xE6`: clear operation failed

In practice, a read request should usually return `0x00`.

#### Byte 1: compiled mode

This is the mode baked into the firmware image at build time.

Examples:

- A normal static Mode `2` build reports compiled mode `2`.
- A runtime-capable Mode `4` build reports compiled mode `4`, even when it later boots into stored Mode `1`, `2`, or `3`.

This field answers the question: what binary am I talking to?

#### Byte 2: active mode

This is the mode selected by the firmware configuration layer for current operation.

At boot, it is normally:

- the stored mode, if the running image supports it
- otherwise the compiled mode

This field answers the question: which mode is the firmware currently trying to operate as?

#### Byte 3: stored mode

This is the mode value read from the persistent flash record.

If there is no valid flash record, the implementation falls back to the compiled mode as the stored value reported through this API.

This field answers the question: what mode selection was last saved?

#### Byte 4: config status bitmask

This byte is a bitmask, not an enum.

| Bit | Name | Meaning |
| --- | --- | --- |
| bit 0 | `USB_MODE_CONFIG_STATUS_VALID` | A valid flash record was found and passed CRC / version checks |
| bit 1 | `USB_MODE_CONFIG_STATUS_MISMATCH` | The stored mode exists but the running image did not activate it |

Observed combinations:

| Value | Meaning |
| --- | --- |
| `0x00` | No valid stored record; firmware is using compiled defaults |
| `0x01` | Valid stored record exists and was applied successfully |
| `0x03` | Valid stored record exists, but the running image fell back instead of activating it |

That last case matters when a mode record was written by a runtime-capable image and later the device boots a narrower static image that cannot support the stored selection.

#### Bytes 5-6: flags

This is a 16-bit little-endian flags field stored alongside the selected mode.

Important note: in the current codebase, these flag bits are preserved and reported back, but no documented flag meanings are implemented yet. Host software should currently treat them as reserved unless a separate extension defines them.

#### Bytes 7-10: capability mask

This is a 32-bit little-endian bitmask where bit `n` indicates support for mode `n` in the currently running image.

Examples:

- `0x00000004` means only Mode `2` is supported, because bit 2 is set.
- `0x0000001E` means Modes `1`, `2`, `3`, and `4` are all supported, because bits 1 through 4 are set.

This field is the safest way for host software to decide which mode values it may request.

### `0x30` Examples

Read status:

```text
Request : 57 AB <addr> 30 00 <sum>
Response: 57 AB <addr> B0 0C <status block...> <sum>
```

Write Mode `3` with zero flags:

```text
Request : 57 AB <addr> 30 02 01 03 <sum>
Response: 57 AB <addr> B0 0C <status block...> <sum>
```

Request reboot after writing:

```text
Request : 57 AB <addr> 30 01 03 <sum>
Response: 57 AB <addr> B0 01 00 <sum>
```

The same state model is also mirrored through the vendor HID Feature report with report ID `0x30`, so desktop tools can manage USB mode selection even when CDC ACM or ECM binding is inconvenient.

### `0x31` System Reset

This is an Openterface extension for triggering a full system reboot. It is primarily used after BLE configuration changes (such as `0x23` BLE Set Name) to ensure all protocol stack state is fully reinitialized.

**Payload format:**

- No payload (empty): normal system reset
- Single `0x00` byte: reset BLE name to factory default (`"KVM-GO"` for KVM-GO, `"Mini-KVM"` for MiniKVM, `"KeyMod"` for KeyMod) before rebooting

**Response:**

- Single byte status (`0x00` = success, `0xE9` = flash erase failed when resetting BLE name)
- Response command byte uses `cmd | 0x80` for success, `cmd | 0xC0` for error
- Device reboots immediately after sending response

**Examples:**

Normal system reset:

```text
Request : 57 AB <addr> 31 00 <sum>
Response: 57 AB <addr> B1 01 00 <sum>
[Device reboots]
```

Reset BLE name to factory default and reboot:

```text
Request : 57 AB <addr> 31 01 00 <sum>
Response: 57 AB <addr> B1 01 00 <sum>
[BLE name reset to factory default (board-specific), device reboots]
```

**Important limitations:**

1. **Immediate reboot**: The device reboots within milliseconds after sending the response. Any pending operations will be aborted.

2. **Connection loss**: If issued over BLE, the connection will be dropped immediately after the response. Host software should expect a disconnection event.

3. **No confirmation**: The command does not wait for acknowledgment from the host. Once sent, the reboot is unconditional.

4. **Use case**: This command is intended for configuration workflows where a full reinitialization is required. For normal operation, the device manages its own state without requiring manual resets.

**When to use:**

- After changing BLE device name with `0x23`
- To reset BLE name to factory default (with `0x00` payload)
- After other configuration changes that require full stack reinitialization
- As a recovery mechanism when the device is in an inconsistent state

## Practical Interpretation

For host software, the safest mental model is:

1. Build a CH9329 frame.
2. Send it over BLE or CDC ACM.
3. Wait for a framed response with the same address and command class.
4. Treat keyboard and mouse commands as control-plane messages that are translated into local HID reports by firmware.

This means the protocol is not a raw HID tunnel. It is a small command protocol whose output side happens to be USB HID.

## Limitations And Compatibility Notes

- The implementation is compatibility-oriented, not a bit-for-bit clone of every possible CH9329 feature.
- `0x17`, `0x18`, `0x23`, `0x26`, `0x27`, `0x30`, and `0x31` are project-specific extensions.
- Outbound response payloads are capped at 58 bytes by the response packet buffer.
- The current keyboard path assumes explicit key-release reports from the sender.
- BLE-related commands (`0x22`, `0x23`) require `ENABLE_BLE` compile-time option. If BLE is disabled, these commands return `0xE3` (unsupported command).
- After changing BLE device name with `0x23`, the device automatically reboots to fully apply the change. The response is sent before reboot, so the host will receive confirmation before the device disconnects.

## Code Anchors

The authoritative implementation is in these files:

- `APP/include/usbd_composite_km.h`
- `APP/usbd_compostie_km.c`
- `APP/keyboard_handler.c`
- `APP/mouse_handler.c`
- `APP/include/ecm_ip_config.h`
- `APP/ecm_ip_config.c`
- `APP/ecm_bridge.c`
