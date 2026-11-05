# BLE-to-Ethernet Tunneling Protocol

## Overview

This protocol enables TCP tunneling over BLE using the CH9329-compatible frame format. It is designed for:

- **SSH over BLE**: Tunnel TCP port 22 through a BLE connection
- **Low-latency control traffic**: Optimized for small interactive packets
- **Transparent crypto**: SSH crypto is end-to-end; this layer only tunnels raw TCP bytes

The protocol runs on the CH32V208 / CH32V20x MCU firmware and communicates with a host app (desktop/mobile) via BLE GATT characteristics.

## Transport

The protocol uses the existing BLE USB service characteristics:

| Characteristic | UUID   | Direction | Description |
|---------------|--------|-----------|-------------|
| RX            | 0xFFF2 | Host->Dev | Write / Write-no-response for commands and data |
| TX            | 0xFFF1 | Dev->Host | Notify for responses and incoming data |

The BLE MTU is negotiated at connection time. The protocol fragments messages to fit within the MTU limit.

## Frame Format

Each frame uses the CH9329-compatible layout:

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| 0 | Header 1 | 1 | `0x57` |
| 1 | Header 2 | 1 | `0xAB` |
| 2 | Address | 1 | Channel/address (reserved, use `0x00`) |
| 3 | Command | 1 | Command byte |
| 4 | Payload Length | 1 | `N` (0-249) |
| 5 | Payload | N | Command-specific data |
| 5+N | Checksum | 1 | 8-bit additive sum of bytes 0..(4+N) |

Notes:

- The protocol frame is designed to fit in a single 255-byte BLE notification (`5 + N + 1 <= 255`), hence the `N <= 249` payload cap.
- Implementations may intentionally cap notifications/writes to smaller “safe” chunks (for example 180 bytes) for high-flow stability.

### Parser Behavior

- Scans for `57 AB` header sync
- If header byte 2 is not `AB`, drops one byte and resyncs
- Validates checksum before dispatch
- Invalid checksum: skips the entire frame and resyncs

### Response Encoding

Follows the CH9329 convention:

- **Positive response**: `cmd | 0x80`
- **Error response**: `cmd | 0xC0`

## Commands

### Connection Management

#### CONNECT (`0x10`) - Establish TCP Tunnel

**Request payload (6 bytes):**

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| 0-3 | Destination IP | 4 | Big-endian IPv4 address |
| 4-5 | Destination Port | 2 | Big-endian port number |

**Response (`0x90`) payload (2 bytes):**

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| 0 | Connection ID | 1 | Assigned connection ID (0-1) or `0xFF` on failure |
| 1 | Status | 1 | `0x00` = success, error code otherwise |

Behavior notes:

- The firmware reports **success only after the TCP handshake is established** (SYN+ACK received). This prevents the host from sending SSH bytes before the target-side TCP state exists.
- The firmware retries TCP SYN a small number of times and returns a deterministic timeout (`0xE1`) instead of hanging without a response.

**Example:**

```
Connect to 192.168.1.100:22
Request : 57 AB 00 10 06 C0 A8 01 64 00 16 CS
Response: 57 AB 00 90 02 00 00 CS   (conn_id=0, success)
```

#### DISCONNECT (`0x12`) - Close TCP Tunnel

**Request payload (1 byte):**

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| 0 | Connection ID | 1 | Connection ID to close |

**Response (`0x92`) payload (2 bytes):**

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| 0 | Connection ID | 1 | Echoed connection ID |
| 1 | Status | 1 | `0x00` = success |

**Remote-closed push notification (`0xD2`):**

When the remote TCP side closes the connection, the device pushes:

```
57 AB 00 D2 01 <conn_id> CS
```

#### NET_PING (`0x24`) - ICMP Echo to a Target IP

Unlike PING (`0x1E`), which only echoes bytes back over BLE to test the radio
path, NET_PING sends real ICMP echo requests out of the target-side CDC ECM
link and reports what came back. This is how a phone or laptop with only a BLE
connection checks whether the machine behind the cable is reachable.

**Request payload (5 bytes):**

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| 0-3 | Destination IP | 4 | Big-endian IPv4 address |
| 4 | Echo Count | 1 | 1-50; `0x00` selects the default of 4 |

**Response (`0xA4`) payload (5 bytes):**

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| 0 | Status | 1 | `0x00` if at least one reply arrived, else `0xE1` TIMEOUT / `0xE8` NO_ROUTE / `0xE5` PARAM_ERR / `0xE7` CONN_FULL |
| 1 | Echoes Sent | 1 | Requests transmitted |
| 2 | Echoes Received | 1 | Replies matched by ICMP id and sequence |
| 3-4 | Mean RTT | 2 | Big-endian milliseconds, `0` when no reply |

Behavior notes:

- One echo per second, one in flight at a time (the cadence is the 1 s
  `SBP_PERIODIC_EVT` tick, `1600 × 0.625 ms`).
- **ARP resolution comes first and can take up to 10 s**: 2 s per attempt, 4
  retries, then a `TIMEOUT` response with `Echoes Sent = 0`. So the host must
  allow roughly `10 + count + 1` seconds, not `count + 1`. Declaring failure
  earlier just produces a false negative.
- `Echoes Sent = 0` plus `TIMEOUT` means the target-side link never resolved
  that IP — the machine is absent, off-subnet, or the target ECM link is down.
  This is a different fault from `Sent = 4 / Recv = 1`, which means L2 was fine
  and the host itself ignored echoes.
- The ping occupies a connection slot for its whole lifetime, so with
  `BLE_ETH_MAX_CONNECTIONS = 2` only one CONNECT tunnel can run alongside it.
- Replies are matched on ICMP id plus sequence, and the ICMP id is unique per
  NET_PING request, so a late answer to a previous ping cannot be credited to
  the next one.

**Example:**

```
Ping 192.168.30.2 four times
  payload = C0 A8 1E 02 04
  checksum = 57+AB+00+24+05+C0+A8+1E+02+04 = 0x2B7 -> low byte 0xB7
Request : 57 AB 00 24 05 C0 A8 1E 02 04 B7
Response: 57 AB 00 A4 05 00 04 04 00 02 B5   (success, 4/4, mean RTT 2 ms)
```

### Configuration

#### SET_DEFAULT (`0x20`) / GET_DEFAULT (`0x21`) - Default SSH Target

`SET_DEFAULT` stores the auto-connect target in flash. `GET_DEFAULT` reads it
back.

- SET_DEFAULT request payload (6 bytes): `[IP 4][port 2]`, response `0xA0` with
  a 1-byte status.
- GET_DEFAULT request has no payload, response `0xA1` with a 7-byte payload:
  `[IP 4][port 2][enabled 1]`.

#### SET_ECM_IP (`0x22`) / GET_ECM_IP (`0x23`) - ECM Network Configuration

These commands change the ECM bridge addressing at runtime (persisted to the
dual-page flash record).

**SET_ECM_IP request payload:**

| sub_cmd | Name | Payload |
|---------|------|---------|
| `0x00` | Set all | `[host 4][dev 4][mask 4][target_host 4][target_dev 4]` (21 bytes total) |
| `0x01` | Set host side | `[host 4][dev 4][mask 4]` (13 bytes total) |
| `0x02` | Set target side | `[target_host 4][target_dev 4]` (9 bytes total) |
| `0x04` | Reset to default | 1 byte |

Response `0xA2` with a 1-byte status (`0x00` success, `0xE5` invalid
parameters, `0xE9` flash error).

**GET_ECM_IP request payload:** 1 byte `[sub_cmd]` — `0x00` all, `0x01` host
side, `0x02` target side, `0x03` bridge link status, `0x05` target-side
re-enumeration / DHCP diagnostics, `0x06` target ECM link-notification
diagnostics.

Response `0xA3`:

| sub_cmd | Payload |
|---------|---------|
| `0x00` | `[host 4][dev 4][mask 4][target_host 4][target_dev 4]` (21 bytes) |
| `0x01` | `[host 4][dev 4][mask 4]` (13 bytes) |
| `0x02` | `[target_host 4][target_dev 4]` (9 bytes) |
| `0x03` | `[host_ecm_active 1][target_ecm_active 1]` (2 bytes) |
| `0x05` | Diagnostics, see below (12 bytes) |
| `0x06` | Link-notification diagnostics, see below (9 bytes) |

The `0x05` diagnostics reply is laid out as:

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| 0 | Status | 1 | `0x00` on success |
| 1 | Detach D+ level | 1 | D+ sampled at the end of the last target detach: `0x00` = target host saw the disconnect, `0x01` = line stayed high (no disconnect), `0xFF` = no re-enumeration yet |
| 2-3 | Re-enum count | 2 | Little-endian, number of target detach/attach cycles |
| 4-5 | Bus reset count | 2 | Little-endian, number of USBFS bus resets seen from the target |
| 6-7 | DHCP requests | 2 | Little-endian, DISCOVER/REQUEST frames handled for the target |
| 8-9 | DHCP NAKs | 2 | Little-endian, out-of-scope REQUESTs answered with NAK |
| 10 | Target ECM data if | 1 | `1` when the target ECM data interface is active |
| 11 | Host ECM data if | 1 | `1` when the host ECM data interface is active |

The `0x06` link-notification diagnostics reply is laid out as:

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| 0 | Status | 1 | `0x00` on success |
| 1 | Notify state | 1 | Target link notification machine: `0` idle, `1` link-down pending, `2` link-up pending, `3` speed-change pending, `4` beaconing link-up |
| 2 | EP5 busy | 1 | `1` while a notification is still latched in USBFS EP5 IN, i.e. the target host has not read it |
| 3-4 | Notifications sent | 2 | Little-endian, `NETWORK_CONNECTION`/`SPEED_CHANGE` packets handed to EP5 |
| 5-6 | EP5 IN completions | 2 | Little-endian, interrupt-IN transactions the target host actually consumed |
| 7-8 | ECM frames sent | 2 | Little-endian, `g_ecm_tx_count` |

`Notifications sent` growing while `EP5 IN completions` stays at zero means the
target host never polls the notification endpoint — its ECM interface is not
open, so no carrier and no link. Both growing while the host still reports
`NO-CARRIER` means the host is consuming the packets and not acting on them.

> **Overlapping command bytes:** CH9329 defines `0x22` (BLE security, payload
> `[sub_cmd]` with sub-commands `0x01..0x03`) and `0x23` (BLE set-name, payload
> is the name text). The firmware distinguishes the two protocols by payload
> shape: `0x23` with a single payload byte in `0x00..0x06` is treated as
> BLE-Eth GET_ECM_IP, and `0x22` with 9/13/21 payload bytes (or a single `0x04`)
> as BLE-Eth SET_ECM_IP. Everything else, including `0x22` with a single
> `0x01`/`0x02`/`0x03`, still reaches the CH9329 handler. Reset deliberately
> uses `0x04` because `0x22 [0x03]` is CH9329 bond-count — treating it as a
> reset would wipe the stored IP configuration whenever an app queried bonds.

All four addresses must be in the same subnet, unique, and the mask must be
contiguous; otherwise `0xE5` is returned and nothing is written. The new
addressing takes effect immediately: both ECM sides soft re-enumerate (the
Mac-facing side via the USBLIB pull-up, the target-facing side via the USBFS
internal D+ pull-up, held low for 1 s so the target host tears down the old
network device). After the target side re-attaches, the firmware replays the
link-state notification sequence and NAKs any DHCP REQUEST that names an
address outside the new target scope, forcing the client to drop its stale
lease and start a fresh DISCOVER.

### Data Transfer

#### DATA (`0x11`) - Send/Receive Tunneled Data

**Request payload (fragmented):**

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| 0 | Fragment Flags | 1 | See fragmentation section |
| 1 | Sequence Number | 1 | Fragment sequence (0-based) |
| 2 | Connection ID | 1 | Target connection ID |
| 3+ | TCP Payload | N | TCP payload data to tunnel |

**Response / ACK (`0x91`) payload:**

The firmware supports multiple ACK formats (advertised via INFO feature bits):

- **Legacy ACK (1 byte)**: `[status]`
- **Metadata ACK (4 bytes)**: `[status, conn_id, seq, expected_seq]`
- **Metadata + receiver window (5 bytes)**: `[status, conn_id, seq, expected_seq, rx_window]`
    - `rx_window` is the receiver-recommended sender in-flight fragment window (1..15).

Demux note:

- Command `0x91` is used for both **DATA_PUSH** (fragments from the device) and **DATA_RESP** (ACKs from the device).
- Host implementations should demultiplex by payload shape:
    - ACK payloads are `len ∈ {1,4,5}` and start with a status byte (`0x00` or `0xEx`).
    - DATA payloads start with a fragment header: `[frag_flags, seq, conn_id, ...]`.

**Incoming data push (`0x91`):**

When data arrives from the remote TCP side, the device pushes DATA frames with the same command byte (`0x91` as notification). The host app distinguishes requests from notifications by context.

### Device Info

#### INFO (`0x1F`) - Get Device Capabilities

**Request:** No payload (length = 0)

**Response (`0x9F`) payload (8 bytes):**

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| 0 | Protocol Version | 1 | `0x01` |
| 1 | Max Connections | 1 | Number of simultaneous tunnels (2) |
| 2-3 | Max Fragment Payload | 2 | Big-endian, max data per fragment |
| 4 | Features | 1 | Feature bitmask (see below) |
| 5-7 | Reserved | 3 | Zero |

**Features bitmask:**

| Bit | Name | Description |
|-----|------|-------------|
| 0 | TCP Tunnel | TCP tunneling supported |
| 1 | SSH Optimized | SSH traffic optimization available |
| 2 | DATA ACK metadata | DATA_RESP uses `[status, conn_id, seq, expected_seq]` |
| 3 | DATA ACK rx_window | DATA_RESP uses `[status, conn_id, seq, expected_seq, rx_window]` |
| 4 | ICMP ping | NET_PING (`0x24`) is supported |

## Fragmentation

BLE has MTU limitations. The protocol supports fragmenting larger TCP payloads:

### Fragment Flags Byte

| Bit | Name | Description |
|-----|------|-------------|
| 7 | MORE_FRAGMENTS | 1 = more fragments follow |
| 6 | FIRST_FRAGMENT | 1 = this is the first fragment |
| 5-4 | Reserved | Must be 0 |
| 3-0 | Total Count | Total number of fragments (1-15) |

### Fragment Rules

- **Single fragment**: `FIRST=1, MORE=0, count=1, seq=0`
- **Multi-fragment**: First has `FIRST=1, MORE=1`, middle has `FIRST=0, MORE=1`, last has `FIRST=0, MORE=0`
- **Max reassembled size (firmware)**: 1536 bytes (`ble_eth_reassembly_t.data[1536]`, ~= 6 fragments x 246 bytes)
- **Out-of-order detection**: If sequence doesn't match expected, reassembly aborts
- **Maximum data per fragment**: `BLE_ETH_MAX_PAYLOAD_LEN - 3` = 246 bytes

Sender guidance:

- The fragment header supports `count=1..15`, but the current firmware reassembly buffer only fits about 6 full-size fragments.
- Host implementations should cap a single reassembled message to the firmware buffer (or split higher-level TCP data into multiple protocol messages).

### Fragmentation Example

Sending 500 bytes of SSH data on connection 0:

```
Fragment 0 (246 bytes data, payload_len=249=0xF9):
57 AB 00 91 F9 <C3: FIRST|MORE|count=3> <seq=0> <conn_id=0> <246 bytes> CS

Fragment 1 (246 bytes data, payload_len=249=0xF9):
57 AB 00 91 F9 <83: MORE|count=3> <seq=1> <conn_id=0> <246 bytes> CS

Fragment 2 (8 bytes data, payload_len=11=0x0B):
57 AB 00 91 0B <03: count=3> <seq=2> <conn_id=0> <8 bytes> CS
```

## Status Codes

| Code | Name | Description |
|------|------|-------------|
| `0x00` | SUCCESS | Operation completed |
| `0xE1` | TIMEOUT | Operation timed out |
| `0xE2` | HEADER_ERR | Frame header error |
| `0xE3` | BAD_CMD | Unsupported command |
| `0xE4` | CHECKSUM_ERR | Checksum mismatch |
| `0xE5` | PARAM_ERR | Invalid parameter |
| `0xE6` | FRAME_ERR | Frame format error |
| `0xE7` | CONN_FULL | No free connection slots |
| `0xE8` | NO_ROUTE | Cannot reach destination |

## Connection Lifecycle

```
Host                                    Device
  |                                       |
  |------ CONNECT (ip, port) ----------->|
  |<----- CONNECT_RESP (conn_id) --------|
  |                                       |
  |------ DATA (conn_id, payload) ------>|
  |<----- DATA_RESP (ACK) --------------|
  |                                       |
  |<----- DATA_PUSH (conn_id, data) -----|  (incoming from remote)
  |------ DATA_RESP (ACK) ------------->|
  |                                       |
  |------ DISCONNECT (conn_id) --------->|
  |<----- DISCONN_RESP -----------------|
  |                                       |
```

### Timeout Behavior

- If a connection is idle for more than 30 seconds, the device may close it
- The device pushes `CONN_CLOSED` (`0xD2`) before closing
- Host should respond with DISCONNECT to confirm

## SSH Over BLE

The protocol is optimized for SSH traffic:

1. **Small packets**: SSH interactive sessions use small packets (typically < 100 bytes), fitting in single BLE frames
2. **Low fragmentation**: Most SSH packets don't require fragmentation
3. **Bidirectional**: Both directions support fragmented transfer
4. **Transparent**: Crypto (SSH key exchange, encryption) is handled end-to-end by the SSH client and server; the MCU just tunnels raw TCP bytes

### Example SSH Session Setup

```
1. Host sends CONNECT to target_ip:22
2. Device establishes TCP connection to target
3. Device responds with CONNECT_RESP (conn_id=0)
4. SSH client sends SSH banner via DATA frames
5. Device tunnels to remote SSH server
6. Remote SSH server responds, device pushes DATA to host
7. SSH handshake proceeds through the tunnel
8. Encrypted SSH session runs transparently
```

## Host App Integration

### Sending Data

```c
// Build a CONNECT frame
uint8_t buf[32];
uint8_t ip[4] = {192, 168, 1, 100};
uint16_t frame_len = BLE_Eth_BuildConnect(buf, ip, 22);

// Write to BLE characteristic
ble_write(buf, frame_len);

// Build a DATA frame (small payload)
uint8_t ssh_data[] = "SSH-2.0-OpenSSH_8.9\r\n";
frame_len = BLE_Eth_BuildData(buf, conn_id, ssh_data, sizeof(ssh_data));
ble_write(buf, frame_len);
```

### Receiving Data

```c
// In BLE notification callback
void on_ble_notify(const uint8_t *data, uint16_t len) {
    BLE_Eth_Parse(data, len);
}

// In event callback
void on_ble_eth_event(ble_eth_evt_t *evt) {
    switch (evt->type) {
    case BLE_ETH_EVT_DATA_READY:
        // Reassembled TCP data from remote
        tcp_send_to_host(evt->info.data.conn_id,
                        evt->info.data.data,
                        evt->info.data.len);
        break;
    case BLE_ETH_EVT_CONN_OPENED:
        tunnel_active = true;
        break;
    case BLE_ETH_EVT_CONN_CLOSED:
        tunnel_active = false;
        break;
    }
}
```

## Implementation Files

| File | Description |
|------|-------------|
| `APP/include/ble_eth_protocol.h` | Protocol definitions, types, and API declarations |
| `APP/ble_eth_protocol.c` | Protocol implementation: parser, connection management, fragmentation |

## Design Constraints

- **MCU**: CH32V208 / CH32V20x (448 KB flash span including a reserved 4 KB config page, 64 KB SRAM)
- **Max connections**: 2 simultaneous tunnels
- **Max fragment data**: 246 bytes per fragment (payload cap 249 minus 3-byte fragment header)
- **Max reassembled payload (firmware)**: 1536 bytes (~6 fragments)
- **BLE MTU**: Negotiated at connection, default 23 bytes (20 bytes payload)
- **TX queue depth**: 8 chunks (non-blocking send)
- **Recommended high-flow notify chunk**: 180 bytes per notification (implementation-defined stability cap)
