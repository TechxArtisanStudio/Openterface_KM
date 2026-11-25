# Change note: pipelined multi-message sends + download TCP flow control + idempotent retransmits
**Date**: 2026-11-25
**Branch**: `feature/cdc-ncm` (Fedora repo is authoritative)
**Commits**: `e1444c0` (perf), `563a66a` (docs)
**Flashed firmware**: `output/openterface-km-firmware-keymod-plus.bin` (225,748 B, wchisp verify OK)
**Build command**:
```bash
./build.sh \
  --compiler "/home/bbot/tools/MRS-linux-x64/resources/app/resources/linux/components/WCH/Toolchain/RISC-V Embedded GCC/bin" \
  --make-arg USB_WORK_MODE=hid_ncm_bridge \
  --make-arg USB_WORK_MODE_RUNTIME_PROFILES=0 \
  keymod-plus
```

---

## 1. Background and motivation

Two leftovers from the windowed host sender round (test report §10):

1. **Extra RTT at 2000 B**: payloads above `MAX_MESSAGE_LEN` (1476 B) were split at the boundary into two messages sent serially, each awaiting the ACKs of all its fragments — one full round trip paid for nothing (2000 B = 1476 + 524).
2. **Download stalls**: the device advertised a fixed `0xFFFF` TCP receive window, so the target could push far more data than the device can buffer (TX queue depth 16, 14 usable chunks ≈ 3.4 KB). Queue full → PushTargetData fails → dup-ACK → the target sits on its RTO timer (Windows ≥300 ms) and the download is paced by retransmissions.

During verification, bidirectional saturation also exposed a previously hidden **data corruption bug** (4000/6000 B echoes always failed), which is fixed here as well.

---

## 2. Change list

| File | Change |
|---|---|
| `tools/ble_eth_tunnel.py` | (1) `send_data()` splits payloads > 1476 B into reassembly-sized groups but keeps them in ONE windowed pass across group boundaries; (2) the windowed sender is group-aware — a failed group restarts from seq 0 with the RETRANS marker (≤3 attempts) instead of degrading to unacked legacy mode; (3) every retransmitted frame sets `FRAG_RETRANS` and refreshes the checksum; (4) `_await_pending_data_ack` gains a strict mode (raises so the caller can restart the group) |
| `APP/ble_eth_protocol.c` | (1) CMD_DATA handles `FRAG_RETRANS` (three-state idempotency); (2) TCP ACKs advertise the free BLE TX queue space dynamically + PumpTick sends a window-update ACK; (3) new diagnostic counter `g_dbg_tcp_win_update` (0x11 stats offset 55-56) |
| `APP/include/ble_eth_protocol.h` | new `BLE_ETH_FRAG_RETRANS 0x20U` (previously unused bit 5) |
| `build/makefile` | `BLE_BUFF_NUM ?= 8` → `12` |
| `tests/test_tunnel_full.py` | echo sizes 4000 B and 6000 B added; the 0x11 stats formatter prints the TCP counter tail |

---

## 3. Protocol change: FRAG_RETRANS (0x20)

Fragment header `frag_flags` bits in use: bit7 `FRAG_MORE (0x80)`, bit6 `FRAG_FIRST (0x40)`, bits 3-0 fragment count. **Bit 5 (0x20) was free** and now means "host retransmission marker".

- Host→device direction only (upload DATA fragments); device→host download fragments have no ACK mechanism and never set it.
- **Compatibility**: an old host never sets the bit → the device takes the existing path unchanged; a new host against old firmware sets a bit that the device ignores (FIRST/MORE/COUNT parsing is unaffected) — only the idempotency protection is lost, no corruption.

Three-state device logic on FIRST|RETRANS:

| Device state | Condition | Action |
|---|---|---|
| Message still reassembling | `reassembly.in_progress` | do not restart; dup-ACK current progress (`expected_seq = next_seq`); the host's cumulative ACK handling converges on its own |
| Previous message completed | `!in_progress` && `g_last_data_ack.valid` && conn matches && `expected_seq == total_frags` | **replay the final ACK, no reassembly, no forwarding** (prevents a second forward of the payload) |
| Previous attempt aborted mid-message | `!in_progress` && `g_last_data_ack.valid == false` | accept a fresh start (the only recovery path) |

Why the count guard matters: a retransmission can only be a duplicate of the last completed message if the fragment counts match. A mismatch (e.g. the 1048 B tail group, count=5, following 1476 B groups with count=6) must never replay — it falls through to the fresh-start path, which can only remove the "swallow real data" failure mode, never introduce it.

Three-state decision flow:

```
        upload DATA fragment, frag_flags has FIRST|RETRANS (host restarts a message)
                                |
                                v
              +---------------------------------+
              |  reassembly.in_progress ?       |
              +-----+---------------------+-----+
                 yes|                     |no
                    v                     v
     [1] report progress:      +---------------------------------+
       dup-ACK                 |  g_last_data_ack.valid ?        |
       expected_seq=next_seq   |  && conn matches                |
       (the host converges     |  && expected_seq == count ?     |
        via cumulative ACKs;   +-----+---------------------+-----+
        in-flight fragments       yes|                     |no
        keep flowing normally)       v                     v
                    [2] replay the final ACK          [3] fresh start
                      no reassembly, no forwarding     reassembly_clear()
                      (payload already delivered       + normal reassembly;
                       once -- prevents a duplicate)   the only recovery after
                                                       a mid-message abort
```

Trigger scenarios: [1] fragments all arrived but an ACK was lost; [2] the message was completed and forwarded but its final ACK never reached the host; [3] a dropped write caused an out-of-order abort (the RETRANS branch must fall through to a fresh start when `valid==false`, otherwise the tunnel cannot recover after an abort).

---

## 4. Key bugs fixed (the full corruption chain behind the 4000/6000 B echo failures)

### 4.1 Root cause: BLE write drops

Every session showed exactly one DATA ACK timeout — not random packet loss but the **device-side controller cache overflowing and dropping write-without-response frames**: the `BLE_BUFF_NUM=8` packet pool is insufficient under bidirectional saturation (4 TX notifies per event + concurrent RX writes). Fix: `BLE_BUFF_NUM 8→12` (buffers are carved out of the BLE heap; RAM headroom unchanged, bss identical).

### 4.2 Corruption path A: phantom reassembly (fixed)

When a write is lost, the host retransmits:

```
fragment seq0 (FIRST) write dropped -> device never saw it -> host ACK timeout -> retransmit seq0 (FIRST)
-> device idle: the old logic unconditionally reassembly_clear() + starts a new reassembly
-> retransmitted seq1..5 accepted in order -> the "phantom message" completes
-> payload forwarded to TCP a second time -> echo server receives duplicate data
-> the echoed stream is shifted from offset 1476 onward
```

The measured corruption matches the theory exactly (4000 B: `diffs=738@1476` — precisely 3 retransmitted fragments; 6000 B: `diffs=246@1476`). Fix: the FRAG_RETRANS three states (§3).

### 4.3 Corruption path B: FRAME_ERR loop → legacy fallback (fixed)

When a middle fragment write is dropped: the device receives an out-of-order fragment → aborts reassembly + FRAME_ERR (expected_seq = the missing index) → the host retransmits **the same fragment** (never the one the device wants) → FRAME_ERR loop → retries exhausted → the host falls back to **unacked legacy mode** → subsequent fragments are silently lost.

Fix: the host restarts **the whole group from seq 0** (marked RETRANS). The device's `g_last_data_ack.valid=false` after an abort → the fresh-start path accepts it → the remaining fragments then arrive in order and the semantics converge. A 30 ms backoff before the restart lets the device drain.

### 4.4 Tuning lesson: the TCP window floor

The first version floored the window at MSS (1460 B): with 8-14 chunks free, the target saw a ≥MSS window and sent a full segment (1460 B = 7 chunks), the push failed → dup-ACK → RTO stall. Changed to **200 B per free chunk**: 200 × 7 = 1400 B, so any mix of ≤MSS segments fits without a push failure (the 200 B/chunk scale leaves margin for per-segment fragment rounding). Floor at one chunk (244 B): the device never emits unsolicited window updates (pure-ACK probes go unanswered), so advertising 0 could deadlock the flow; with ≥1 chunk always advertised the target keeps exactly one segment-sized, enqueueable segment in flight, and every received segment refreshes the window via its ACK.

---

## 5. Verification

### 5.1 Full suite (`tests/test_tunnel_full.py`, run from the Mac bleenv)

**25 PASS / 0 WARN / 0 FAIL, two consecutive runs** (23 → 25 after adding 4000 B/6000 B).

### 5.2 Throughput comparison

| echo size | windowed host (§10) | this round (pipelined + flow control) | notes |
|---|---|---|---|
| 1000 B | 14.4–15.6 KB/s | 13.0–14.3 KB/s | within run-to-run variance |
| 1460 B | 11.3–17.5 KB/s | **19.0–19.1 KB/s** | +15-60% |
| 2000 B | 10.4 KB/s (extra RTT) | **10.1–10.4 KB/s** | pipelined straight through, no boundary RTT |
| 4000 B | corrupted FAIL | **8.1–8.9 KB/s** | new case: 3-segment download + 3 pipelined upload groups |
| 6000 B | corrupted FAIL | **6.9–7.4 KB/s** | new case: 5 segments + 5 groups |

### 5.3 Stress regression

`repro4000.py`, six consecutive 4000 B echoes: all `diffs=0`, retransmit/timeout counters at the normal one-per-session level, every event took a safe recovery path. After flashing, ICMP NET_PING works (rtt 256 ms — first-packet ARP delay after the new Windows firewall rule).

---

## 6. Known limitations and follow-ups

- **The download direction remains notify-rate bound** in absolute terms (connection interval 15 ms × BLE_TX_NUM_EVENT=4 × 244 B ≈ 65 KB/s ceiling; measured 7-19 KB/s combined round trip). This round removed the dup-ACK/RTO stalls, not the bandwidth ceiling.
- The host restart path assumes **the oldest unacked message is the last completed one** (window blocking guarantees this). Supporting concurrent fragmented uploads on multiple connections would need a message ID (the global reassembly buffer only holds one message anyway).
- Extreme edge case where **every** fragment write of a message is dropped while the previous message's ACK is still valid: the device would replay the stale ACK and swallow the message. Probability is vanishingly low (12+ consecutive write drops); the protocol has no message ID to distinguish this, so the risk is accepted.
- TX queue depth 16 cannot be increased: RAM usage is 58.4/64 KB (bss ends 0x2000e874, stack at 0x2000f800) — no headroom.
- The Fedora commit chain (`cc51e4c…563a66a`) is not pushed to origin; the Mac worktree is a content mirror only (HEAD still `4c11581` + uncommitted changes) — compare md5s before syncing (this round the stale Mac makefile briefly regressed the build config).

---

## 7. Operation record (reproduction path)

1. Edit locally on the Mac: `~/projects/Openterface/Openterface_KM` (+ `/tmp/bletest` as the test-execution copy).
2. Sync: scp to Fedora `/home/bbot/projects/Openterface/Openterface_KM` (compare md5s first).
3. Build: the command in the header (Fedora, MRS 8.2.0 toolchain, LTO).
4. Flash: `sudo reboot` → wait ~60 s → `python3 tools/ctrl_board.py isp on` → `wchisp flash output/openterface-km-firmware-keymod-plus.bin` → `isp off` (ISP entry is only reliable on the first attempt after a reboot).
5. Test: on the Mac in `/tmp/bletest`: `/tmp/bleenv/bin/python -u tests/test_tunnel_full.py --ble-addr 1CC44426-51DA-E45A-73C8-2B01006A2D1E --target 192.168.20.2 --port 9000`.
6. Environment: echo server is Windows 192.168.20.2:9000 (multi-client PowerShell runspace version, schtasks TCPEcho9000); BLE device KeyMod-NCM.
