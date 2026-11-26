# How the NCM bridge and the BLE stack share the CPU

Status: **reference** (source analysis; the loss model is not yet verified by an
instrumented build)
Applies to: `USB_WORK_MODE=hid_ncm_bridge` (mode 8) with `ENABLE_BLE=1`
Analysed: 2026-11-27, firmware commit `04a26d7`, by disassembling
`output/openterface-km-firmware-kvmgo.elf`

This note exists to answer one question: when the radio is enabled, what delays
the NCM bridge, and why does the damage show up as packet loss instead of
latency? It is written against `feature/cdc-ncm`, mode 8 (`hid_ncm_bridge`) with
`ENABLE_BLE=1`.

## The short answer

**`NCM_Bridge_Poll()` and `TMOS_SystemProcess()` never preempt each other.** They
are two ordinary functions called one after the other from the same `while(1)`
in the same thread. The only preemption in the system comes from interrupts, and
no interrupt runs either of them except one case: the EP4 IN completion ISR
calls `NCM_Bridge_Poll()` directly.

So the "preemption" is really **serialisation with a fixed order and no slack**:

- `TMOS_SystemProcess()` runs **first** and can stay for an unbounded time.
- `NCM_Bridge_Poll()` runs **last** and owns a queue that is only **2 frames
  deep**.

That combination is the whole story.

## The loop

`Main_Circulation()` in `APP/peripheral_main.c` (itself placed in `.highcode`,
i.e. executed from RAM):

```c
while (1) {
    ...
    TMOS_SystemProcess();      /* line 179 - BLE stack, runs to completion   */
    USB_DataRx_To_KMHandle();
    USB_SendFromQueues();
    ...
    NCM_Bridge_Poll();         /* line 201 - NCM bridge, whatever is left    */
    ...
}
```

Neither call can be interrupted by the other. There is no RTOS, no priority, no
time slicing. An iteration takes as long as the sum of the two.

## What `TMOS_SystemProcess()` actually does

The function lives in the WCH BLE library, but it is linked into `.highcode` and
can be read out of the ELF (`output/openterface-km-firmware-kvmgo.elf`,
`TMOS_SystemProcess` at `0x00000e76`). Two facts matter:

1. **It does not sleep.** The exit path is

   ```
   1032: lw  ra,12(sp)
   1034: lw  s0,8(sp)
   1036: lw  s1,4(sp)
   1038: addi sp,sp,16
   103a: ret
   ```

   There is no `wfi`, no idle loop, no tick wait. It processes whatever is due
   and returns. It is not a scheduler tick that can be assumed to be cheap: its
   cost is the sum of all work that happens to be pending.

2. **It runs application code inline.** Every due TMOS event is dispatched
   inside this call, including the two loops in `APP/peripheral.c`:

   - `SBP_PROCESS_USBDATA_EVT` — `while (RingMemUSB.CurrentLen)` pumping up to
     512 bytes in 244-byte notifications, breaking only when
     `app_usb_notify()` fails.
   - `SBP_PROCESS_BLEETH_TX_EVT` — `while ((len = BLE_Eth_GetNextTxChunk(...)))`,
     pumping the tunnel TX queue until the controller runs out of buffers.

   Both are bounded by buffer state rather than by a fixed count, so the time
   spent here is a function of link conditions, not of anything the NCM side can
   predict.

It also executes deferred link-layer work: at `0x1050` the disassembly calls
`ll_multi_states_process` when the library has multi-state work pending. Part of
the radio's bookkeeping is therefore *not* in the interrupt — it is in this main
context call, competing with the bridge directly.

Relevant timing constants:

| Constant | Value | Meaning |
|---|---|---|
| TMOS tick | 625 µs | unit of `tmos_start_task` |
| `DEFAULT_DESIRED_*_CONN_INTERVAL` | 12 → **15 ms** | one connection event per 15 ms |
| `DEFAULT_DESIRED_SLAVE_LATENCY` | **0** | the peripheral must answer *every* event |

With slave latency 0 the stack cannot skip an event, so the 15 ms cadence is a
hard floor on how often this work arrives.

## Interrupts

| Source | Handler | What it touches |
|---|---|---|
| Radio / link layer | `BB_IRQHandler` → `BB_IRQLibHandler`, `LLE_IRQHandler` (RAM stub) | BLE controller state |
| USB device (host side) | `USBFS_IRQHandler` (`APP/ch32v20x_usbfs_device.c`) | EP3/EP4/EP5/EP6/EP7 |
| 1 ms tick / RGB | `TIM2_IRQHandler` | `systick_ms`, LED |

Priority is programmed in exactly one place: `Systick_Init()` puts TIM2 at
preemption priority 1. The USB device interrupt is only `NVIC_EnableIRQ`d
(`ch32v20x_usbfs_device.c:725`) and the radio interrupt is enabled inside the
BLE library, so both keep the reset priority of 0. `NVIC_PriorityGroup_1` gives
one bit of preemption, which means:

- USB and radio are at the same level and therefore **cannot preempt each
  other** — a long radio ISR delays the USB ISR, and the USB FIFO has no flow
  control to protect it.
- TIM2 (the 1 ms tick) is the only source that can be preempted by both.

For the NCM/BLE question, the practical consequence is the first bullet: the two
sources that matter most cannot nest, so their ISR times add.

### The one interrupt that calls into the bridge

`EP4_IN_Callback()` (`APP/USBLIB/CONFIG/usb_endp.c`) runs in the USB ISR and
calls `NCM_Bridge_Poll()`:

```c
USBD_Endp4_Busy = 0;
NCM_Bridge_Poll();   /* from interrupt context */
```

So on the host-facing TX path the ISR *does* preempt the main loop and re-enters
the same function the main loop may be inside. `NCM_Bridge_Poll()` has no
re-entry guard. See "Open issue" below.

### Interrupt load

NCM moves data in 64-byte USB packets (`NCM_BRIDGE_USBFS_PACKET_SIZE`,
`NCM_BRIDGE_HOST_PACKET_SIZE`). A full 1514-byte Ethernet frame becomes an NTB of
roughly 1530 bytes, i.e. **24 packets and 24 interrupts per frame**. At the
measured 0.5 MiB/s that is ~350 frames/s, or **~8 000 interrupts per second**.

The main context is therefore chopped into slices of roughly 120 µs. Any
`TMOS_SystemProcess()` call is stretched by every one of those interrupts, which
is why the *wall-clock* residency of the BLE stack is much larger than the work
it performs.

## Why the two directions behave differently

Both directions buffer through a 2-entry frame queue
(`NCM_BRIDGE_QUEUE_DEPTH == 2`), but they are drained by different code paths.

| Direction | Producer (context) | Consumer (context) | ISR help on the drain |
|---|---|---|---|
| Host → target (Mac → Windows) | `NCM_Bridge_HostRxPacket` — USBFS ISR, EP3 OUT | `NCM_Bridge_Poll` → `USBFS_Endp_DataUp(DEF_UEP7)` — **main loop only** | **none** |
| Target → host (Windows → Mac) | `NCM_Bridge_TargetRxDone` — USBFS ISR, EP5 OUT | `NCM_Bridge_Poll` builds the NTB (main), `host_tx_feed()` feeds chunks — **also called from EP4 IN ISR** | yes, partially |

`DEF_UEP7` is fed from exactly two places in a mode 8 build, both inside
`NCM_Bridge_Poll` (`ncm_bridge.c:2173` and `:2210`). Nothing else can advance the
host→target stream. That direction is therefore *fully* exposed to any main-loop stall,
while target→host keeps making progress from interrupt context.

This predicts exactly the asymmetry that was measured: **37.5–50% loss in the
fully-exposed direction, 10% in the partially-assisted one.**

## Slack arithmetic

The buffer absorbs 2 × 1514 B = **3028 B** per direction. At the measured
388 kB/s (0.37 MiB/s) that is **~7.8 ms of stall before the tail is dropped**.

So the requirement is not "`NCM_Bridge_Poll()` must run every millisecond"; it is
"the loop must return to `NCM_Bridge_Poll()` at least every ~8 ms while the link
is saturated". `TMOS_SystemProcess()` runs ahead of it in the same iteration, so
its residency comes straight out of that budget. Once the budget is exceeded,
`queue_push()` increments `overflow` and drops the frame — silently, by design.

## Evidence from the measurements

The counter-intuitive part of the bench data is that enabling the radio makes
latency *better* and loss *worse*:

| | BLE off | BLE on |
|---|---|---|
| ICMP host → target under saturation | 0% loss, RTT 117–214 ms | 37.5–50% loss, RTT 2.7–7.8 ms |
| ICMP target → host under saturation | 0% loss, no duplicates | 10% loss, up to 361 duplicates |

That inversion is the signature of **tail drop moving upstream**. With the radio
off the bridge drains fast enough, nothing is dropped in the device, and the
backlog builds in the Windows stack — high delay, no loss. With the radio on the
bridge drains slower, the 2-deep queue overflows, and frames die *before* they
ever reach Windows — so the delay never has a chance to build, and the loss
appears instead.

A drop-in-the-device explanation and a "the radio steals CPU" explanation are the
same explanation here; the RTT/loss inversion is what distinguishes it from a
link or driver problem.

## Open issue: duplicate replies

Target → host saturation with the radio on produced up to 361 duplicate ICMP
replies (+85 in a second run). Zero in every BLE-off run and zero on an idle
link, so it is load-dependent and radio-dependent.

The loss model above does not explain duplicates. The standing suspect is the
re-entry described earlier: `NCM_Bridge_Poll()` is called from both the EP4 IN
ISR and the main loop with no guard, and `host_tx_feed()` arms the endpoint
*before* it advances state:

```c
if (USBD_ENDPx_DataUp(ENDP4, &s_host_ntb_tx[s_host_ntb_tx_off], chunk) != USB_SUCCESS)
    return;
s_host_ntb_tx_off = ...;   /* state updated after the transfer is armed */
```

If the completion ISR lands in that window the ISR sees the old offset and arms a
chunk that was already sent. What that does on the wire depends on which chunk it
hits — a repeated chunk in the middle shifts every following byte and corrupts
the NTB, while a repeat at a boundary can put a whole NTB on the wire twice,
which a host NCM driver delivers as a second copy of the same Ethernet frame.
The corrupt-NTB variant would show up as loss and the duplicate-NTB variant as a
duplicated reply, so both observed symptoms are consistent with one defect.

The window itself is small — a handful of instructions against a ~43 µs packet
time — which is consistent with the effect needing saturation and interrupt
pressure to appear at all. Still not proven; the guard-plus-retest experiment
below is what would settle it.


## Fix options

Ordered by cost. None is implemented yet.

| Change | Effect | Cost |
|---|---|---|
| Call `NCM_Bridge_Poll()` **before** `TMOS_SystemProcess()` | removes the BLE residency from the NCM latency budget entirely | one line, no RAM |
| Bound the two pump loops (max N chunks per event, re-arm with `tmos_start_task(..., 1)`) | caps `TMOS_SystemProcess()` residency instead of letting it run to buffer exhaustion | ~10 lines, no RAM |
| Re-entry guard in `NCM_Bridge_Poll()` | removes the duplicate-NTB hazard and makes ISR use safe | 3 lines, no RAM |
| Raise `NCM_BRIDGE_QUEUE_DEPTH` from 2 to 4 | doubles the stall budget to ~15 ms | **+3028 B RAM** (89% → 95%), likely too expensive |

The first three are structurally correct and free in RAM; the fourth is a
band-aid and probably unaffordable on the current images.

## How to verify a fix

1. BLE connected, radio idle, no tunnel traffic: confirm baseline throughput is
   unchanged (0.50–0.56 / 0.39–0.40 MiB/s).
2. Saturate host → target and ping from the Mac: loss should fall well below
   37.5%, and RTT should rise toward the BLE-off figure rather than the loss
   disappearing into nothing.
3. Saturate target → host and count replies: duplicates should be 0.
4. Repeat with the tunnel active (SSH over BLE) to confirm the bound on the pump
   loops did not cost throughput.

The `0x33/0x10` diagnostics frame already exposes `poll_calls` and
`g_host_tx_queue.overflow`, so steps 2 and 3 can be checked from counters
instead of inferring from `ping` output.
