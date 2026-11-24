# BLE-ETH Tunnel Test Report & Fix Record

- **Date**: 2026-11-24
- **Hardware**: KeyMod dev kit (CH32V208, dual USB controllers), control board
- **Firmware**: Openterface_KM `feature/cdc-ncm`, keymod-plus build (`USB_WORK_MODE=hid_ncm_bridge`)
- **BLE device**: `KeyMod-NCM` (`1CC44426-51DA-E45A-73C8-2B01006A2D1E`), pairing PIN `666666`
- **Test host**: Mac mini (bleak via `/tmp/bleenv`, Bluetooth TCC granted to WorkBuddy)
- **Target**: Windows 11 (`admin@192.168.100.63`), echo server on `192.168.20.2:9000`

---

## 1. Test goal

Validate the BLE-ETH tunnel end to end:

```
Mac (bleak, GATT fff2 write / fff1 notify)
   └─ BLE ─→ KeyMod-NCM firmware (BLE_Eth protocol, TCP client)
                └─ target port CDC-NCM ─→ Windows echo server (192.168.20.2:9000)
```

Protocol: CH9329-style framing (`57 AB addr cmd len payload csum`), commands
`CONNECT 0x10 / DATA 0x11 / DISCONNECT 0x12 / PING 0x1E / NET_PING 0x24`, DATA frames
carry fragmentation headers and are reassembled on both ends.

## 2. Fixes landed in this session

### Fix 1: oversized frames silently dropped by the RX parser

`APP/include/ble_eth_protocol.h`:

```c
/* before */
#define BLE_ETH_MAX_PAYLOAD_LEN   238U  /* 244-byte frame = ATT MTU 247 - 3 */
/* after */
#define BLE_ETH_MAX_PAYLOAD_LEN   249U  /* protocol limit; TX is chunked at 244 B by the notify pump */
```

**Symptom**: any PING payload > 238 B got no PONG at all; payloads ≤ 238 B worked.

**Root cause**: the parser rejects frames whose `payload_len > BLE_ETH_MAX_PAYLOAD_LEN`
— the whole 245-byte frame (two 180 B host writes) was discarded at the length byte.
The 238 limit was introduced by the in-flight "one frame = one notification" change;
the host library still speaks the protocol limit (249 B) and does not fragment PING.

**Evidence chain** (via the new `0x33/0x11` diagnostics, see §3): after the two writes,
`rx_evt_count`/`rx_bytes` showed both writes reached `BLE_Eth_Parse`, but the parser
snapshot read `state=WAIT_HEAD1, rx_index=5, csum=0x0F` — the header-sum left over from
a frame rejected at the length byte (the rejection path does not reset `rx_index`).

### Fix 2: stack overflow + truncated frames in the target→host DATA path

`APP/ble_eth_protocol.c`, `BLE_Eth_PushTargetData()`:

```c
uint8_t full_buf[BLE_ETH_TX_CHUNK_SIZE];          /* 244 B */
...
uint16_t total_payload = FRAG_HEADER_LEN + chunk_len;   /* 3 + 246 = 249 */
memcpy(&full_buf[off], &data[offset], chunk_len);  /* 5+3+246 = 254 B into 244 B! */
```

With Fix 1 in place, `max_frag_data` grew to 246, making each DATA_PUSH frame 255 B —
an 11-byte stack overflow, and the frame was stuffed truncated into a single TX chunk,
which desynchronised the host byte stream (`Checksum mismatch ... cmd=DATA_PUSH len=249`).

**Fix**: cap fragment size so every DATA_PUSH frame stays within one TX chunk
(244 B = 6 frame + 3 fragment header + 235 data), matching the 7 × 235-byte receive
reassembly buffer sizing:

```c
uint16_t max_frag_data = BLE_ETH_TX_CHUNK_SIZE - BLE_ETH_FRAME_MIN_LEN - BLE_ETH_FRAG_HEADER_LEN;
if (max_frag_data > BLE_ETH_MAX_PAYLOAD_LEN - BLE_ETH_FRAG_HEADER_LEN) {
    max_frag_data = BLE_ETH_MAX_PAYLOAD_LEN - BLE_ETH_FRAG_HEADER_LEN;
}
```

## 3. Diagnostics infrastructure added (kept in the tree)

- **`CMD_ISP_DEBUG (0x33) sub_cmd 0x11`** — BLE-Eth debug snapshot, 37 B, memory-read
  only (no printf): TX push ok/drop, push-front fallback, frames parsed, ping count,
  pong queued, RX write-callback invocations/bytes, notify ok/fail, parser
  `state/rx_index/payload_len/checksum`, TX queue count/rd/wr/head-partial, ACK queue
  count/hold, reliability counters.
- **`sub_cmd 0x10`** (existing NCM bridge diagnostics) unchanged.
- **Gotcha discovered**: `CH9329_SendResponse()` hard-caps responses at 64 B
  (`packet[64]`, `len > 58 → len = 58`) and sends them with a single `app_usb_notify()`.
  Oversized diagnostic frames get silently truncated — keep diagnostic payloads ≤ 58 B.

Reading diagnostics over BLE requires the encrypted link (fff1/fff2 are
`GATT_ENCRYPT_WRITE`); the control-board UART whitelist never forwards 0x33 to the KM chip.

## 4. Test results

### Official suite (`tests/test_eth_bridge.py`): 13/14 PASS

| Test | Result |
|---|---|
| Protocol frame build/parse (CONNECT/INFO/DISCONNECT/PING) | PASS ×4 |
| Checksum validation & parser resync | PASS ×2 |
| BLE connect + INFO exchange | PASS |
| PING/PONG 5 B / 247 B / 249 B | PASS ×3 |
| TCP tunnel CONNECT to 192.168.20.2:9000 | PASS |
| ECM bridge diagnostics | PASS |
| SSH banner exchange | FAIL — environment artifact: the suite expects an SSH server banner on the target port; port 9000 runs the echo server. Not a tunnel defect. |

### Custom end-to-end tests

| Test | Result |
|---|---|
| NET_PING (ICMP through target NCM) to 192.168.20.2 | 3/3, ~1 ms |
| Tunnel open (conn_id=0) + server banner | PASS |
| Echo round-trip 25/10/200 B | PASS, byte-exact |
| Echo round-trip 1000/2000/4000 B (fragmented) | PASS, byte-exact, 2.4–3.3 KB/s |

The 4000 B test exercises: host-side fragmentation (INFO max_frag = 246), device
reassembly (1664 B buffer), TCP through the target NCM bridge, device-side
fragmentation (235 B), host reassembly, and DATA-ACK windowing.

## 5. Debugging journey (condensed)

1. **Large PING hang** — static analysis of the TX pump found nothing wrong; added the
   `0x33/0x11` counters → both host writes reached `BLE_Eth_Parse` (`rx_evt_count=3`),
   parser stuck at the header → length-byte rejection → Fix 1.
2. **DATA checksum mismatch** — deterministic across runs (`expected 0x01, got 0x75`);
   read `BLE_Eth_PushTargetData` → `full_buf[244]` overflow → Fix 2.
3. **Tunnel CONNECT timeout with L3 healthy** — `NET_PING` 3/3 proved ARP+ICMP worked;
   `pktmon` on Windows proved the firmware's TCP SYNs arrived and were silently dropped;
   disabling the firewall turned the timeout into an immediate `status=0xE8` (RST) —
   Windows Defender Firewall was eating inbound :9000 despite the allow rule. Fixed by
   running the echo server under a scheduled task and re-verifying with the rule active.

## 6. Test-bench gotchas (for future sessions)

- **Windows echo server**: run it as a scheduled task (`schtasks /create /tn TCPEcho9000
  ... /ru SYSTEM`), plain hidden PowerShell processes keep dying. Logs to
  `C:/Users/admin/tcp_echo.log`. Local `Test-NetConnection` validates binding but not
  firewall reachability — check the server log for `CONNECT from 192.168.20.5`.
- **BLE advertising pauses**: after a disconnect the device may stop advertising for
  minutes. Poll with `BleakScanner.discover()` loops instead of a single
  `find_device_by_address()` timeout; keep a whole test in one session.
- **Don't interleave same-subnet links in tests**: the fedora box has two NICs on
  192.168.20.0/24 — always bind with `ping -I` / explicit interface.
- **macOS BLE permission**: bleak crashes (SIGABRT) unless the host app has Bluetooth
  TCC permission. Granting it to WorkBuddy makes BLE tests fully scriptable.
- **Control board power GPIO** hangs intermittently (3rd time today); rebooting the
  fedora host resets the control board. For flashing use
  `ctrl_board.py isp on` → `wchisp flash` → `isp off` when the target is on another machine.

## 7. TUN-1 RESOLVED: root cause was the test environment, not the device

The "rapid close/reopen data race" recorded earlier turned out to have a
prosaic root cause. Instrumented reproduction (device-side counters plus a
host-side TX/RX log) showed, at the moment of the stall:

- the conn-slot snapshot (`0x33/0x12`, added in d5b9ea1) had **two**
  ESTABLISHED connections: the still-open D2 tunnel on slot0 and the new
  D3 tunnel on slot1, with a consistent seq/ack pair and **zero data
  accepted** on the new connection;
- the echo-server log had **no** `CONNECT` line for the new connection
  during the whole 24 s stall window, and only logged it (and sent its
  banner) after the old connection was RST by the suite's recovery step.

Root cause chain:

1. `sequential_tunnels_x3` opened the D3 tunnel while the D2 tunnel was
   still open (the firmware correctly allocated slot1 / conn=1).
2. The reference echo server (`win_tcp_echo.ps1`) was **single-client**:
   after accepting a connection it echoes forever and never returns to
   `AcceptTcpClient()`, so the new connection sat in the accept backlog
   forever. No banner, no data, no device involvement at any point.
3. "Fresh open after clearing slots recovers" is explained by the same
   chain: clearing slots RSTs the old connection, the server's echo loop
   exits, and only then is the backloged connection accepted (with its
   banner arriving far too late for the host's receive timeout).

Fixes applied:

- **Test suite**: each sequential cycle now closes the previous tunnel
  before opening the next one, so the rapid close/reopen path is exercised
  deterministically; a missing banner is a FAIL (the WARN path is gone).
- **Echo server**: rewritten as multi-client (one runspace per accepted
  connection), so overlapping tunnels -- which the firmware supports with
  its two slots -- each get their banner, matching real SSH server
  behavior.
- **Firmware diagnostics** (kept, they were what localized the issue):
  `0x33/0x11` now carries TCP RX-path counters (no_match / half_local /
  half_remote / seq_mism / fwd_fail / fwd_ok / synack / synack_dup /
  rst_rx) and a new `0x33/0x12` sub-command snapshots both conn slots
  (state, ports, seq/ack numbers).

Also fixed along the way: the Windows NCM NIC's connection profile flips
to Public on re-enumeration, which silently drops inbound ICMPv4 echo
requests and breaks `NET_PING`. A persistent allow rule
(`BLE-ETH ICMPv4 echo-in`, profile any) is now installed on the target.

Result: full suite **23 PASS / 0 WARN / 0 FAIL**, stable across repeated
runs; the former cycle-0 stall no longer reproduces.

## 8. Open items

- **Throughput**: 2.4-3.9 KB/s round-trip is dominated by the DATA-ACK
  window and notification pacing; worth profiling if higher rates are
  needed.
- **`test_eth_bridge.py` SSH test**: gate it on `--port 22` or add a
  banner-prefix check for non-SSH echo targets.
- **Control board power GPIO reliability**: recurring failure
  (reboot/recovers) -- needs its own investigation in
  `Openterface_KM_control`.
## 9. Complete test suite

`tests/test_tunnel_full.py` (committed) runs 23 cases across 5 layers; final result:
**23 PASS (1 WARN = TUN-1 recovery), 0 FAIL**. PING/PONG boundary sweep includes
238/239 as a regression guard for Fix 1; echo sizes include 234/235/236 as a
regression guard for Fix 2.

```bash
python3 tests/test_tunnel_full.py --ble-addr <addr> --target 192.168.20.2 --port 9000
```

## 10. Reproducing the tests

```bash
# on the Mac (bleak venv with Bluetooth permission)
cd /tmp/bletest
# full official suite (BLE + PING + TCP tunnel)
/tmp/bleenv/bin/python tests/test_eth_bridge.py \
    --ble-addr 1CC44426-51DA-E45A-73C8-2B01006A2D1E \
    --target 192.168.20.2 --port 9000
# large echo sweep (1000/2000/4000 B, reads banner first, accumulates recv)
# see /tmp/ble_session.py, /tmp/tunnel_e2e.py, /tmp/net_ping.py patterns:
#   - wrap client._notification_handler BEFORE client.connect()
#   - client.recv_data(conn_id) may return partial reassembled data — loop until len satisfied
```
