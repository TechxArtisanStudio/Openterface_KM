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

| Test                                                      | Result                                                                                                                                       |
| --------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| Protocol frame build/parse (CONNECT/INFO/DISCONNECT/PING) | PASS ×4                                                                                                                                      |
| Checksum validation & parser resync                       | PASS ×2                                                                                                                                      |
| BLE connect + INFO exchange                               | PASS                                                                                                                                         |
| PING/PONG 5 B / 247 B / 249 B                             | PASS ×3                                                                                                                                      |
| TCP tunnel CONNECT to 192.168.20.2:9000                   | PASS                                                                                                                                         |
| ECM bridge diagnostics                                    | PASS                                                                                                                                         |
| SSH banner exchange                                       | FAIL — environment artifact: the suite expects an SSH server banner on the target port; port 9000 runs the echo server. Not a tunnel defect. |

### Custom end-to-end tests

| Test                                               | Result                         |
| -------------------------------------------------- | ------------------------------ |
| NET_PING (ICMP through target NCM) to 192.168.20.2 | 3/3, ~1 ms                     |
| Tunnel open (conn_id=0) + server banner            | PASS                           |
| Echo round-trip 25/10/200 B                        | PASS, byte-exact               |
| Echo round-trip 1000/2000/4000 B (fragmented)      | PASS, byte-exact, 2.4–3.3 KB/s |

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

## 9. Throughput experiment: adaptive idle ACK release (2026-11-25)

The delayed-ACK experiment (TX chunk 244 B, dedicated ACK queue, held  
success ACKs to merge per-message ACKs) shipped with two defects that  
cancelled most of its benefit:

1. **Unit bug**: `peripheral.c` passed `TMOS_GetSystemClock()` (625 us  
   ticks) to `BLE_Eth_PumpTick()` as milliseconds -- the 100 ms hold was  
   really 62.5 ms, and a naive tick conversion would wrap every ~72 min.
2. **Stop-and-wait penalty**: a held ACK waited the full hold while  
   reassembly was in progress. The host library is effectively stop-and-wait  
   for fragmented sends (window 1-2), so the next fragment cannot arrive  
   before the ACK is seen -- every fragment paid the full hold  
   (~140 ms/fragment measured). Holding made large transfers *slower*.

Fix (dd8932b): `PumpTick` releases the hold after 2 polls (~20 ms) with no  
new fragment, while pipelined hosts (fragments arriving per connection  
event) keep merging into a single ACK per message. The tick unit is  
converted with a 64-bit intermediate.

Measured with the full suite (same run also exercises TUN-1 regression):

| echo size | before    | hold fix only | idle release |
| --------- | --------- | ------------- | ------------ |
| 470 B     | 5.1 KB/s  | 4.3 KB/s      | 6.7-7.6 KB/s |
| 1000 B    | 3.4 KB/s  | 2.8 KB/s      | 5.9-6.5 KB/s |
| 1460 B    | 3.8 KB/s  | 3.1 KB/s      | 6.3-6.6 KB/s |
| 2000 B    | 3.8 KB/s  | 3.3 KB/s      | 6.1-7.0 KB/s |
| 232-236 B | ~7.5 KB/s | ~7.5 KB/s     | ~7.5 KB/s    |

Suite: 23 PASS / 0 WARN / 0 FAIL across repeated runs.

## 10. Windowed host sender (2026-11-25, follow-up)

The idle-ACK-release firmware still capped at ~6-7 KB/s because the HOST  
library was the bottleneck: fragmented sends ran stop-and-wait (window 1-2),  
so every fragment paid a round trip no matter what the device did. The host  
library now:

- adopts the device-advertised `rx_window` (from ACK frames) as its in-flight  
  cap when the device announces the window feature -- the device already  
  sizes that window from its reassembly buffer and ACK-queue headroom;
- applies device features from the INFO response at connect time (the  
  `apply_device_features` hook existed but was never called, so the first  
  send per connection silently ran fire-and-forget in legacy mode);
- raises `MAX_MESSAGE_LEN` to 1476 B (6 x 246) so a 1460 B message is one  
  message instead of 1440 B + a 20 B leftover paying an extra ACK round trip.

Device side had to catch up in two places (ca881f1): `compute_rx_window`  
reports true capacity using the observed fragment size (9 fragments of the  
host's 180 B chunks fit the 1664 B buffer, not the worst-case 6) and no  
longer clamps to the current message's remaining fragment count -- that  
clamp leaked into windowed hosts as their in-flight cap. The idle-release  
detector now uses a monotonic push counter (merges overwrite the queued ACK  
without growing the queue, so the queue length cannot signal progress) with  
a 4-poll (~40 ms) threshold covering the host's write pacing.

Measured with the full suite, 23 PASS / 0 WARN / 0 FAIL across repeated runs:

| echo size | stop-and-wait | idle release only | windowed host      |
| --------- | ------------- | ----------------- | ------------------ |
| 470 B     | 5.1 KB/s      | 6.7-7.6 KB/s      | 8.9-10.1 KB/s      |
| 1000 B    | 3.4 KB/s      | 5.9-6.5 KB/s      | **14.4-15.6 KB/s** |
| 1460 B    | 3.8 KB/s      | 6.3-6.6 KB/s      | **11.3-17.5 KB/s** |
| 2000 B    | 3.8 KB/s      | 6.1-7.0 KB/s      | **10.4 KB/s**      |

Remaining ceiling: 2000 B still splits at MAX_MESSAGE_LEN into two messages  
(one extra ACK round trip), and the echo download direction is notification  
pacing bound (~7 x 244 B frames per connection-event stream).

## 11. Pipelined multi-message sends + download flow control (2026-11-25)

Two follow-ups to §10: payloads above `MAX_MESSAGE_LEN` (1476 B) no longer pay
an extra round trip at the split boundary, and the download direction (target
-> BLE host) got real TCP flow control. This exposed and fixed a reliability
hole that bidirectional saturation had been hiding.

### What was done

**Host (`tools/ble_eth_tunnel.py`)**

- `send_data()` splits payloads > 1476 B into reassembly-sized *groups* but
  keeps them in ONE windowed pass (window shared across group boundaries), so
  the second message starts as soon as window credit frees up instead of after
  a full ACK round trip.
- The windowed sender is now group-aware: a group whose ACK fails (dropped BLE
  write or lost ACK) is **restarted from seq 0 with a `FRAG_RETRANS` marker**
  (up to 3 attempts) instead of degrading the tunnel to unacked legacy mode.
- Retransmitted fragments (timeout retries and restarts) set `FRAG_RETRANS`
  (protocol bit 0x20, previously unused) and refresh the frame checksum.

**Device (`APP/ble_eth_protocol.c`)**

- `FRAG_RETRANS` handling: a retransmitted FIRST fragment is now idempotent —
  if the message is still being reassembled, progress is reported (cumulative
  ACK); if the last completed message matches (count-guarded), its final ACK is
  replayed and the payload is *not* forwarded again; after a mid-message abort
  the fresh start is accepted. Previously a retransmitted FIRST fragment while
  idle started a "phantom reassembly" that forwarded the payload a second time
  (corruption exactly at offset 1476).
- Download TCP flow control: ACKs advertise the free BLE TX queue space
  (200 B per chunk, floored at one chunk) instead of a fixed 0xFFFF, so the
  target paces segments to the BLE drain rate instead of flooding 14-deep queue
  into dup-ACK + RTO retransmit stalls. `BLE_Eth_PumpTick` sends a pure
  window-update ACK once a full segment fits again, so a throttled target is
  not paced by its own 300 ms RTO timer.
- `BLE_BUFF_NUM` 8 -> 12 (`build/makefile`): with both directions saturated the
  8-packet controller cache dropped write-without-response frames — the
  single DATA-ACK timeout per session seen on every large echo.

### Results (full suite, 25 PASS / 0 WARN / 0 FAIL, two consecutive runs)

| echo size | windowed host (§10) | pipelined + flow control |
| --------- | ------------------- | ------------------------ |
| 1000 B    | 14.4-15.6 KB/s      | 13.0-14.3 KB/s           |
| 1460 B    | 11.3-17.5 KB/s      | **19.0-19.1 KB/s**       |
| 2000 B    | 10.4 KB/s           | 10.1-10.4 KB/s (no extra RTT) |
| 4000 B    | n/a (corrupted)     | **8.1-8.9 KB/s**         |
| 6000 B    | n/a (corrupted)     | **6.9-7.4 KB/s**         |

4000 B and 6000 B are new suite cases exercising multi-segment downloads and
multi-message pipelined uploads; they corrupted before the reliability fixes
and now pass repeatedly. The download direction remains notify-rate bound in
absolute terms, but no longer stalls on dup-ACK/RTO backpressure.


