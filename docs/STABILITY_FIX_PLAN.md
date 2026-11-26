# Firmware stability fix plan

- Branch: `fix/stability-p0-p1`
- Baseline: `main` @ `19ef5c8`
- Applies to: KVM-GO (mode 7), KeyMod Plus (mode 7), KeyMod Mini (mode 4)
- Status: Phase 1 implemented (all four product modes build; hardware verification pending, see §5)

---

## 1. Goals and scope

This round only fixes problems **that cause data corruption, lock-ups, protocol
cross-talk or unrecoverable states**. It does not change the external protocol byte
definitions of BLE-Eth / CH9329 and does no unrelated refactoring.

Goals:

1. Tunnel data no longer suffers "frame splicing" or silent corruption under normal load and interrupt concurrency.
2. BLE-Eth frames are no longer consumed a second time by the legacy CH9329/USB HID path.
3. The TCP transmit path no longer uses a 1.5 KB stack frame, and no longer advances the sequence number on failure.
4. After a BLE disconnect or a target-side re-enumeration, tunnel state is predictable and recoverable.
5. Configuration writes no longer erase/write flash for nothing.

Out of scope this round (see Phase 2 in §6): TCP retransmission/RTO, BLE-Eth ACK
end-to-end back-pressure, re-enumeration watchdog, flash load semantic validation,
linker-script reserved sections.

---

## 2. Problem overview

| ID | Level | Problem | Location | Consequence | Phase |
|----|------|------|------|------|-------|
| P0-1 | P0 | The ECM queues are read and written concurrently by the USBFS/USBD interrupts and the main loop with no critical section; the `queue_peek` pointer spans several 32B transfers, and is overwritten by the flush when the queue fills | `APP/ecm_bridge.c` `queue_push/peek/pop/clear`, `poll_*_tx`, `ECM_Bridge_*RxDone` | spliced frames on the wire, rd/wr/count races, tunnel permanently stuck after the peer drops packets | 1 |
| P0-2 | P0 | BLE-Eth frames are also written to `RingMemBLE`, sent to EP2 (HID mouse) by `USBSendData()` and parsed by `CH9329_RxBuffer_Add()` | `APP/peripheral.c` `ble_usb_ServiceEvt` | tunnel bytes treated as HID reports/serial noise; CH9329 replies with the 0xC0 error frame | 1 |
| P0-3 | P0 | `send_tcp_frame()` uses a 1514B stack buffer; `g_ecm_tx_cb` failure still returns success and advances `local_seq`; the transmit layer has no retransmission | `APP/ble_eth_protocol.c` `send_tcp_frame`, `BLE_Eth_SendDataToEth` | stack overflow in ISR context; sequence permanently skewed after a queue drop | 1 |
| P1-1 | P1 | A BLE disconnect does not clean up BLE-Eth (parser/reassembly/tx_queue/connections) | `APP/peripheral.c` `Peripheral_LinkTerminated` | half frames and stale notifications survive across connections; state is wrong after reconnecting | 1 |
| P1-2 | P1 | notify failure retries immediately without checking the connection handle | `APP/peripheral.c` `SBP_PROCESS_BLEETH_TX_EVT` | the main loop spins after a disconnect | 1 |
| P1-3 | P1 | A target-side re-enumeration does not reset BLE-Eth connections or notify the client | `APP/ecm_bridge.c` `poll_target_reenum` | the client still believes ESTABLISHED and keeps sending the old seq | 1 |
| P1-4 | P1 | `BLE_Eth_HandleTargetFrame` does not validate the IP total length | `APP/ble_eth_protocol.c` | unsigned underflow, out-of-bounds read from a bogus length | 1 |
| P1-5 | P1 | The re-enumeration state machine has no timeout, success criterion or retry; the enumeration result is not verified after attach | `APP/ecm_bridge.c` `poll_*_reenum` | after one failure a replug is needed; target HID lost for about 1.1 s | 2 |
| P1-6 | P1 | Host-side `ECM_Bridge_Init()` clears `g_host_to_target` while the tunnel seq has already advanced | `APP/ecm_bridge.c` `ECM_Bridge_Init` | silent segment loss on USB reset or wake from sleep | 1 (mitigation) / 2 (proper fix) |
| P2-1 | P2 | Every `ECM_IPConfig_Set()` erases and writes without comparing whether anything changed; no semantic validation on load | `APP/ecm_ip_config.c` | pointless wear; an illegal record can misconfigure the network | 1 (compare) / 2 (validate) |
| P2-2 | P2 | `txq_lock/unlock` unconditionally call `__enable_irq()` inside an interrupt | `APP/ble_eth_protocol.c` | corrupts the caller's interrupt state | 1 |
| P2-3 | P2 | 0x22/0x23 are distinguished by payload shape, which is fragile | `APP/peripheral.c` | future extensions easily regress | 2 |
| P2-4 | P2 | The linker script does not explicitly reserve the 0x6B000/0x6C000 config pages | `HAL/Link_440k.ld` | code growth may overwrite the configuration | 2 |

---

## 3. Phase 1 detailed design

### 3.1 P0-1 ECM queue concurrency protection

**Root cause**

- `ECM_Bridge_TargetRxDone()` / `ECM_Bridge_HostRxDone()` are called from the USB interrupts and end up in `queue_push()` / `queue_clear()`.
- The main loop's `poll_host_tx()` / `poll_target_tx()` perform `queue_peek()` / `queue_pop()`.
- `ECM_FrameQueue_t`'s `rd/wr/count` are not atomic and have no critical section.
- The pointer returned by `queue_peek()` is used across several 32-byte `USBD_ENDPx_DataUp()` calls; when the queue fills, `queue_push()` does `rd = wr; count = 0`, overwriting the same slot and producing "old frame first half + new frame second half".

**Fix**

1. Add a save/restore critical section based on WCH CSR `0x800` bit3 (do **not** use `__get_MSTATUS/__set_MSTATUS`, see `docs/HID_QUEUE_RELIABILITY_FIX.md`):

```c
static inline uint32_t ecm_irq_save(void)
{
    uint32_t csr;
    __asm volatile ("csrr %0, 0x800" : "=r"(csr));
    __disable_irq();
    return (csr >> 3) & 0x1u;
}

static inline void ecm_irq_restore(uint32_t state)
{
    if (state) __enable_irq();
}
```

2. Add `volatile uint8_t in_flight` and `volatile uint16_t gen` to `ECM_FrameQueue_t`.
3. `queue_push()`:
   - return 0 immediately on an out-of-range length.
   - when the queue is full and `in_flight` is set, **drop the new frame and return 0**, never overwriting the head frame currently being transmitted.
   - when the queue is full with nothing in flight, keep the existing flush behaviour and `gen++`.
4. `queue_clear()`: take the lock, clear `in_flight`, `gen++`.
5. Add `queue_tx_load()` / `queue_tx_complete()`:
   - `queue_tx_load()` copies the next segment (≤32B) of the current head frame, or the ZLP marker, into a local chunk inside the critical section, and sets `in_flight = 1`.
   - `queue_tx_complete()` pops only after checking that `gen` and the head pointer are unchanged; if a `queue_clear()` happened in between it abandons the pop.
   - `poll_*_tx()` no longer holds a pointer into the queue for long; the USB write uses the local chunk.
6. `ECM_Bridge_SendRawFrame()` returns the `queue_push()` result, for use by P0-3.

**Acceptance**

- Under heavy load the `overflow` counter may rise, but captures no longer show Ethernet frames with bad length/checksum.
- Tunnel + 100 Hz HID + USB reset stress for 1 hour without lock-up.

### 3.2 P0-2 Isolate the BLE-Eth and CH9329 paths

**Root cause**

`ble_usb_ServiceEvt()` only recognises CH9329 command bytes; 0x10/0x11/0x12/0x1F/0x1E/0x20/0x21 fall into the else branch, where, besides calling `BLE_Eth_Parse()`, the data is also written to `RingMemBLE`, sent to EP2 by `USBSendData()`, and fed to the CH9329 parser by `USB_DataRx_To_KMHandle()`. Both protocols use the same checksum algorithm, so CH9329 answers the unknown command with `STATUS_ERR_CMD`.

**Fix**

1. Add `Peripheral_IsBleEthCommandByte(cmd)`: recognises 0x10–0x12, 0x1E, 0x1F, 0x20, 0x21.
2. `ble_usb_ServiceEvt()` becomes three branches:
   - BLE-Eth (including the 0x22/0x23 ECM IP shapes) → call only `BLE_Eth_Parse()` and trigger the TX event, **without writing RingMemBLE or feeding CH9329**.
   - CH9329 commands → unchanged.
   - other non-protocol data → keep the existing raw RingMemBLE pass-through.
3. CH9329 frames on 0x22/0x23 (BLE security / set name) are still judged false by `Peripheral_IsBleEthEcmIpFrame()` and so enter the CH9329 branch.

**Acceptance**

- When sending 0x11 DATA, no HID report appears on the target-side EP2.
- When sending 0x10/0x11/0x12/0x1F, no 0xC0 error frame appears in the serial/USB response queue.
- CH9329 keyboard/mouse, BLE renaming and BLE pairing queries all regress clean.

### 3.3 P0-3 TCP transmit stack usage and failure semantics

**Fix**

1. Turn `send_tcp_frame()`'s 1514-byte buffer into a module-level static. That function runs both in the TMOS/BLE task (SYN/DATA) and from the USBFS interrupt (ACKs for target data), so the whole "build frame + enqueue" sequence is serialised with `txq_lock()` (CSR 0x800 bit3 save/restore) to prevent a nested send from rewriting the shared buffer.
2. `ble_eth_ecm_tx_cb_t` and `BLE_Eth_SetEcmtxCb()` now return `uint8_t`; `ECM_Bridge_SendRawFrame()` returns the enqueue result.
3. Advance `local_seq` only when the enqueue succeeds (one rule for SYN/FIN/data); on failure return 0.
4. `BLE_Eth_SendDataToEth()` stops the remaining segments on failure and increments the `eth_tx_fail` counter.
5. `send_tcp_syn()` keeps its existing failure-rollback logic.

**Acceptance**

- In `-fstack-usage`, `send_tcp_frame` stack usage drops from ~1.5 KB to < 100 B.
- When the queue is deliberately saturated, the sequence no longer advances early; `eth_tx_fail` is observable.

> Note: this round does **not** implement TCP retransmission. Losing a segment is
> still an error path, but it has gone from "silent + skewed sequence" to
> "observable + consistent sequence". Real end-to-end guarantees are Phase 2.

### 3.4 P1-1 / P1-2 BLE link lifecycle

1. Add `BLE_Eth_OnLinkLost()`:
   - clear the parser, fragment reassembly and TX queue;
   - set every connection to CLOSED and clear `active`;
   - reset the last-ACK state.
2. `Peripheral_LinkTerminated()` calls `BLE_Eth_OnLinkLost()` and `tmos_stop_task(SBP_PROCESS_BLEETH_TX_EVT)`.
3. `SBP_PROCESS_BLEETH_TX_EVT`:
   - validate `connHandle != GAP_CONNHANDLE_INIT` before notify;
   - on failure, retry after a delay with `tmos_start_task(..., N)`, and after three consecutive failures stop this round and keep the queue.

### 3.5 P1-3 Tunnel handling after a target-side re-enumeration

1. Add `BLE_Eth_ResetAllTunnels(uint8_t status)`:
   - send a `BLE_ETH_CMD_CONN_CLOSED (0xD2)` notification to every active connection (payload: conn_id + status);
   - clear the connection state.
2. **Placement adjustment**: call `BLE_Eth_ResetAllTunnels()` in `schedule_target_reenum()` rather than in the detach stage. The reason is that queued frames are only detached after 500 ms, so a RST enqueued at detach time has no chance to be sent, and the attach stage clears `g_host_to_target`. Closing the tunnel at schedule time lets the RST travel to the target over the still-live ECM link, while `CONN_CLOSED` has ample time to reach the BLE client.
3. A host-side re-enumeration (`ECM_Bridge_Init`) does not close tunnels; see 3.8.

### 3.6 P1-4 IP length validation

Add the following in `BLE_Eth_HandleTargetFrame()`:

```c
uint16_t ip_total = frame_get_be16(&ip[2]);
if (ip_total < ip_header_len) return false;
if ((uint32_t)off + ip_total > len) return false;
```

Then compute the TCP segment length as `ip_total - ip_header_len`, avoiding an unsigned underflow.

### 3.7 P2-1 / P2-2 Detail fixes

1. After parameter validation, `ECM_IPConfig_Set()` compares against the RAM cache; if identical it returns `ECM_IP_CONFIG_OK` directly, with no erase/write and no re-enumeration.
2. `txq_lock/txq_unlock` switch to the same CSR 0x800 bit3 save/restore as `ecm_irq_save/restore`.

### 3.8 P1-6 Silent segment loss on a host-side re-enumeration (Phase 1 mitigation)

`ECM_Bridge_Init()` is triggered by a USBD reset on the Mac side. That path does
not affect the target-side ECM link, so while an active or pending tunnel exists it
**no longer clears `g_host_to_target`** (that queue carries both tunnel frames and
bridge frames); only when there is no tunnel are the old session frames flushed.
This prevents tunnel frames from being silently dropped while `local_seq` has
already advanced. The complete solution (distinguishing the queue's origin) is
Phase 2.

---

## 4. Files touched

| File | Change |
|------|------|
| `APP/ecm_bridge.c` | queue critical sections, in_flight/gen, TX chunk copy, `SendRawFrame` return value, close tunnels on target re-enumeration, keep the tunnel queue on host re-enumeration |
| `APP/include/ecm_bridge.h` | `ECM_Bridge_SendRawFrame` return type |
| `APP/ble_eth_protocol.c` | static TX buffer, callback return value, seq advancement rule, IP length validation, `BLE_Eth_OnLinkLost`, `BLE_Eth_ResetAllTunnels`, txq lock |
| `APP/include/ble_eth_protocol.h` | new API declarations, callback signature, statistics fields |
| `APP/peripheral.c` | BLE-Eth command allow-list, disconnect cleanup, notify retry back-off, TX callback return value |
| `APP/ecm_ip_config.c` | short-circuit when the configuration is unchanged |

### 4.1 Implementation and build verification (2026-11-02)

All Phase 1 changes are on the `fix/stability-p0-p1` branch; all four product modes
build with no compiler warnings:

| Mode | FLASH | RAM |
|------|-------|-----|
| kvmgo (mode 7) | 219968 B / 48.82% | 51064 B / 77.92% |
| minikvm (no BLE) | 49924 B / 11.08% | 37704 B / 57.53% |
| keymod-plus (mode 7) | 217864 B / 48.35% | 51048 B / 77.89% |
| keymod-mini (mode 4) | 217784 B / 48.34% | 51048 B / 77.89% |

The static transmit buffer moved about 1.5 KB of stack usage into BSS; RAM headroom
is about 22%, so the headroom must be re-checked before Phase 2 adds retransmission
buffers.

---

## 5. Test plan

### 5.1 Build and static checks

1. Build `kvmgo` (mode 7) and `keymod-mini` (mode 4) with the MounRiver toolchain:

```bash
RISC_COMPILER_PATH="/Users/txa/MRS_Toolchain_MAC_V240/Toolchain/RISC-V Embedded GCC/bin" ./build.sh kvmgo
RISC_COMPILER_PATH="/Users/txa/MRS_Toolchain_MAC_V240/Toolchain/RISC-V Embedded GCC/bin" ./build.sh keymod-mini
```

2. Check that no new warnings appear; in `output/*.map`, `__stack_size` is still 2048 and the `_ebss` headroom is at least 12 KB.
3. Generate `-fstack-usage` and confirm the worst-case stack along the `send_tcp_frame`, `BLE_Eth_Parse`, `ECM_Bridge_TargetRxDone` call chains is < 1.5 KB.
4. Run `cppcheck --enable=warning,performance` over `APP/*.c` and confirm no new findings.

### 5.2 Protocol isolation regression (no extra hardware needed)

1. Send 0x01/0x02/0x05 over BLE: keyboard and mouse behave normally.
2. Send the single-byte 0x22 0x01–0x03 over BLE: BLE pairing queries work.
3. Send 0x23 + name text over BLE: renaming + reboot work.
4. Send 0x10/0x11/0x12/0x1F over BLE-Eth: capture the target-side EP2 and confirm no HID reports; no 0xC0 error frames on the serial port.
5. `tools/ble_ecm_ip.py get` / `set` 50 times in a row: the configuration is correct and the device does not reboot.

### 5.3 Data integrity and concurrency

1. BLE-Eth `scp` of 10 MB + `sha256sum`: 20 consecutive runs all match.
2. Send 100 Hz HID reports during tunnel transfer: data still matches, no keyboard/mouse stutter.
3. Trigger a target-side IP change (re-enumeration) during tunnel transfer: the client receives `CONN_CLOSED` within 1 s and data is intact after reconnecting.
4. Mac sleep/wake and USB reset 20 times each during tunnel transfer: the connection recovers, no permanent lock-up.
5. Deliberately saturate the ECM queue (reduce `ECM_BRIDGE_QUEUE_DEPTH` or inject heavy traffic): confirm no more spliced frames, and `eth_tx_fail` grows.

### 5.4 Link and re-enumeration

1. BLE connect for 1 s / disconnect for 1 s, 100 times: no residual parser, no idle TX events, usable immediately after reconnecting.
2. 200 × `SET_ECM_IP` (alternating identical/different): `reenum_count` from `GET 0x05` matches the command count, `detach_dp_level == 0`.
3. Cut power or reset during the re-enumeration detach window: the state machine restarts from 0 after the reboot.
4. Change the target-side address 20 times in a row during a tunnel: `CONN_CLOSED` arrives every time, the device does not wedge.

### 5.5 Long-run stability

1. 72 h tunnel + periodic HID + an IP query every hour: no IWDG reset (4 s), no steady decline in the memory watermark.
2. Record `g_target_to_host.overflow`, `g_host_to_target.overflow`, `tx_queue_full_drop`, `frag_reassembly_abort`, `eth_tx_fail`; none keeps growing afterwards.

---

## 6. Phase 2 (later)

| Item | Description |
|----|------|
| TCP retransmission/RTO | add a 2-segment transmit cache for the device→target direction, retransmitting on 3 dup-ACKs or a 300 ms RTO |
| BLE-Eth ACK back-pressure | delay the last fragment's ACK until the ECM enqueue succeeds; on failure have the client retransmit the whole TCP segment |
| Re-enumeration watchdog | after attach, wait for `g_*_ecm_data_if_active`, retry 1–2 times on timeout and report |
| Host-side re-enumeration | evaluate whether to keep tunnels across a USBD reset; close and notify the client if needed |
| Flash semantic validation | run `validate_config()` after `ECM_IPConfig_Init()` loads, falling back to defaults on failure |
| Linker script | add `ASSERT`/reserved sections for the 0x6B000/0x6C000 config pages |
| Protocol identification | give BLE-Eth its own command byte or addr marker and drop the 0x22/0x23 shape discrimination |
| Diagnostics | extend `GET 0x05` with queue overflow, transmit failure and retransmission counters |

---

## 7. Acceptance criteria (Phase 1)

- [ ] All four build targets compile, with FLASH/RAM within limits.
- [ ] The protocol isolation regression passes in full.
- [ ] 20 × 10 MB transfers produce matching sha256.
- [ ] No anomalous residue after a 100× BLE disconnect storm.
- [ ] The client receives a close notification after a target-side re-enumeration.
- [ ] `send_tcp_frame` stack usage < 100 B.
- [ ] 72 h long run with no watchdog reset.

---

## 8. Risks, compatibility and rollback

**Compatibility**

- No change to BLE-Eth / CH9329 command bytes or payload formats.
- The `BLE_Eth_SetEcmtxCb()` signature change only affects the internal wiring in `peripheral.c` and `ecm_bridge.c`.
- The `ECM_Bridge_SendRawFrame()` return-type change is backward compatible for existing callers (code that ignores the return value still compiles).

**Risks**

- The critical sections briefly disable interrupts (frame building + a 1.5 KB memcpy, a few tens of microseconds); confirm that USB/BLE lose no events.
- `queue_push()` dropping the new frame when the queue is full (with a frame in flight) may surface packet loss earlier under heavy load, but it cannot splice frames.
- `BLE_Eth_OnLinkLost()` actively closes all tunnels and relies on the client reconnecting.

**Rollback**

- Each problem is committed separately and can be reverted per commit.
- If the critical sections cause USB anomalies, revert P0-1 first and evaluate separately.

---

## 9. Suggested commit split

1. `fix(ecm): serialize frame queues between USB ISRs and main loop`
2. `fix(ble): route BLE-Eth frames away from legacy CH9329/USB path`
3. `fix(ble): avoid 1.5KB stack frame and only advance TCP seq on enqueue success`
4. `fix(ble): reset BLE-Eth state on link loss and back off notify retries`
5. `fix(ecm): close BLE-Eth tunnels before target-side re-enumeration`
6. `fix(ble): validate IPv4 total length before parsing TCP`
7. `fix(config): skip flash write when ECM IP configuration is unchanged`
8. `fix(ble): restore interrupt state in tx queue lock`
