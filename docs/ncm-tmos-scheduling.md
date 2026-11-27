# How the NCM bridge and the BLE stack share the CPU

Status: **reference** (source analysis plus bench measurements on hardware)
Applies to: `USB_WORK_MODE=hid_ncm_bridge` (mode 8) with `ENABLE_BLE=1`
Analysed: 2026-11-27, firmware `04a26d7` by disassembling
`output/openterface-km-firmware-kvmgo.elf`; measurements extended after the
scheduling changes in this document.

This note answers two questions: when the radio is enabled, what delays the NCM
bridge, and why does the damage show up as packet loss instead of latency.

**Read the second half before acting on the first.** The loop structure below is
correct and worth knowing, but the loss model that was originally derived from it
turned out to be wrong when it was tested. §"Evidence" records what the
measurements actually say.

## The short answer

**`NCM_Bridge_Poll()` and `TMOS_SystemProcess()` never preempt each other.** They
are two ordinary functions called one after the other from the same `while(1)`
in the same thread. The only preemption in the system comes from interrupts, and
no interrupt runs either of them except two endpoint completions that call
`NCM_Bridge_Poll()` directly.

So the "preemption" is really **serialisation with a fixed order**:

- `TMOS_SystemProcess()` can stay for an unbounded time, because its two
  notification pumps are bounded by buffer state, not by a count.
- `NCM_Bridge_Poll()` owns a queue that is only **2 frames deep**.

That combination is a real scheduling hazard, and it was worth fixing on its own
terms. It is **not** the cause of the ICMP loss seen under saturation: with the
fix in place the loss did not move, and it is present whether or not a BLE link
is connected. The loss is what a 2-frame queue does when TCP is filling a
12 Mbit/s link.

## The loop

`Main_Circulation()` in `APP/peripheral_main.c` (itself placed in `.highcode`,
i.e. executed from RAM):

```c
while (1) {
    ...
    USB_DataRx_To_KMHandle();
    USB_SendFromQueues();
    ...
    NCM_Bridge_Poll();         /* bridge first, since the scheduling change */
    ...
    TMOS_SystemProcess();      /* BLE stack second, bounded by the pump budget */
    ...
}
```

Neither call can be interrupted by the other. There is no RTOS, no priority, no
time slicing. An iteration takes as long as the sum of the two. The order used to
be the other way round (`TMOS_SystemProcess()` first, at line 179); it was
swapped so the bridge is never handed only the leftovers of an iteration.

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

   Both were originally bounded only by buffer state, so the time spent here was
   a function of link conditions rather than of anything the NCM side could
   predict. The tunnel pump now stops after `BLE_ETH_TX_CHUNKS_PER_PASS` (4)
   notifications and re-arms itself one tick later; the USB pump is bounded by
   construction (a 512-byte ring, so at most three 244-byte notifications).

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
the same function the main loop may be inside. `NCM_Bridge_Poll()` now carries a
one-byte re-entry guard (`s_poll_busy`), so the loser of that race returns
immediately instead of running the body twice; see "Open issue" below.

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

That asymmetry is real and it does show up on the bench. Measured under full
saturation of each direction:

| Saturating direction | Probe direction | Loss | RTT |
|---|---|---|---|
| host → target | host → target | 42–58% | 1.5–11 ms |
| host → target | target → host | 0–2% | 111–225 ms |
| target → host | target → host | 0% | 111–225 ms |
| target → host | host → target | 0–2% | 114–225 ms |

Two things follow. First, only row 1 drops; everywhere else the same 12 Mbit/s
link merely queues and delays (~200 ms). Second, row 1 is the direction whose TX
has no interrupt assist — the one structural difference between the two paths —
so it is the plausible weak side, and that is where the device's own counters
point as well: during a host→target run the host reported `Drop` 0 / `Oerrs` 0
on its interface while the device's `host_frames` counter rose by 6538, i.e.
every frame the host sent was parsed by the bridge. The requests therefore
reached the bridge and died on the way out, at the target-facing queue.

Do not over-read this as "one line in the ISR would fix it". Calling the whole
`NCM_Bridge_Poll()` body from the EP7 completion was tried and made the loss
*worse* (76.4%, RTT 10–21 ms) because it moved the host-side path and both polls
into an ISR that fires every 64-byte chunk. A lighter assist — only the EP7 chunk
feed — was not tried. What can be said without further work is that the queue is
full and 2 entries deep, and that is the operating point the loss comes from.


## Slack arithmetic

The buffer absorbs 2 × 1514 B = **3028 B** per direction. At the measured
0.53 MiB/s that is **~5.5 ms of queueing before the tail is dropped** — which is
exactly the RTT reported in the saturated same-direction case (3–11 ms). The
arithmetic and the measurement agree, so the queue being full is not a hypothesis;
it is the observed operating point.

So the requirement is not "`NCM_Bridge_Poll()` must run every millisecond"; it is
"the loop must return to `NCM_Bridge_Poll()` at least every ~5 ms while the link
is saturated". `TMOS_SystemProcess()` used to run ahead of it in the same
iteration, so its residency came straight out of that budget. Once the budget is
exceeded, `queue_push()` increments `overflow` and drops the frame — silently, by
design.

Note what this implies: no amount of scheduling work removes the loss. TCP always
fills whatever bandwidth is available, so the queue stays full, and any packet
that arrives from a *different* flow (ICMP) while both slots are occupied is
dropped. Draining faster does not create room; it only lets TCP send faster.

## Evidence from the measurements

The first draft of this note explained the loss as "the radio steals CPU". That
explanation was built on this comparison, which is **invalid**:

| | "BLE off" | BLE on |
|---|---|---|
| ICMP under saturation | 0% loss, RTT 117–214 ms | 37.5–50% loss, RTT 2.7–7.8 ms |

The two runs did not share a control variable. The 117–214 ms figure came from a
run with the TCP load flowing in the *opposite* direction to the probe; with the
load in the same direction, this firmware drops packets whether the radio is on
or not. The contrast was measuring queueing-versus-dropping, not radio-on-versus-
radio-off.

What the controlled runs show (mode 8, `kvmgo`, 55 probes at 5/s, 8 MiB TCP):

| Firmware | BLE link | Loss | RTT | TCP rate |
|---|---|---|---|---|
| pre-change | not connected | 41.8% / 47.3% / 56.4% / 54.5% | 1.5–10.7 ms | 0.52–0.56 MiB/s |
| with the changes below | not connected | 54.5% / 56.4% / 54.5% / 56.4% | 2.8–11.0 ms | 0.53 MiB/s |
| with the changes below | connected, idle | 52.7% / 58.2% | 3.3–15.1 ms | 0.49–0.52 MiB/s |

Three conclusions:

1. **The radio is not a factor.** Connected and disconnected overlap completely.
2. **The scheduling changes are not a factor either.** The pre-change firmware
   spans the same range; the spread is run-to-run variance, not a regression.
   Four runs per build were needed to see that — the first two baseline runs
   (41.8%, 47.3%) looked like a 10-point improvement that two more runs erased.
3. **The dropper is the device, not the host.** During a host→target run the
   macOS interface reported `Opkts` +6491 with `Drop` 0 and `Oerrs` 0, and the
   device's `host_frames` counter rose by 6538 — every frame the host handed over
   was parsed by the bridge. So the ICMP requests reached the bridge and died
   afterwards, at the target-facing queue. The host-facing queue's overflow
   counter rose by 2 over the same run, which is consistent with the table above:
   that direction queues, it does not drop.


## Duplicate replies: the window is real, the mechanism is now impossible

Earlier bench runs (before the changes below) reported up to 361 duplicate ICMP
replies, +85 in a second run, in the target→host saturated case only.

The suspect was the re-entry of `NCM_Bridge_Poll()` from the EP4 IN completion
ISR. `host_tx_feed()` used to arm the endpoint *before* advancing state:

```c
if (USBD_ENDPx_DataUp(ENDP4, &s_host_ntb_tx[s_host_ntb_tx_off], chunk) != USB_SUCCESS)
    return;
s_host_ntb_tx_off = ...;   /* state updated after the transfer is armed */
```

The window between those two statements is as wide as the PMA copy, because
`USBD_ENDPx_DataUp()` only sets `USBD_Endp4_Busy` **after** `USB_SIL_Write()`
returns (`usb_endp.c`, the `ENDP4` branch). A second entry in that window finds
the endpoint not busy and the old offset, so it queues the chunk that is already
in the buffer. A repeat in the middle of an NTB shifts every following byte; a
repeat at a boundary puts a whole NTB on the wire twice, which the host NCM
driver delivers as a second copy of the same frame.

### The overlap is not hypothetical

A new counter, `poll_skipped`, is incremented every time the guard refuses a
re-entry. On the bench, four consecutive 8 MiB target→host transfers produced:

| Counter | After run 1 | After run 5 | Delta over 4 runs |
|---|---|---|---|
| `poll_skipped` | 5 789 | 22 814 | 17 025 (≈ 4 250 per 14 s run) |
| `dup_blocks` | 0 | 0 | 0 |
| `t2h` frames | 6 639 | 24 611 | 17 972 |

So the EP4 completion lands inside a running poll body roughly **300 times a
second** under load. Every one of those is an occasion on which the old code
would have re-armed the chunk in flight. This is the strongest statement the
bench can make about the historical duplicates: the overlapping context was not
rare, it was the normal case.

### What actually closes it

Two independent changes, either of which is sufficient:

1. **The re-entry guard is claimed atomically.** `s_poll_busy` was tested and
   set with a plain load followed by a plain store, which the EP4 ISR can split:
   the ISR then also reads 0, runs the whole body, and the interrupted caller
   resumes and runs it again. Interrupts are now masked across the test-and-set
   only (`ncm_irq_save()` / `ncm_irq_restore()`), not across the body.
2. **The offset advances before the arm and is rolled back on refusal.** A
   second entry can no longer see the byte range it just queued, so correctness
   no longer depends on the guard.

The second change is also what makes `dup_blocks` meaningful: it counts attempts
to arm an offset that is already in flight for the same NTB instance, and stays
0 when the ordering holds. It is a tripwire for a regression, not a repair.

### Verdict, and what it is not

Four 8 MiB target→host transfers with 150 ICMP probes each in the same window:
**0 duplicate replies**, 0.0–1.3% loss, 0.55 MiB/s — identical throughput to the
build before the change, so the hardening costs nothing. `dup_blocks` stayed 0
throughout.

Treat the duplicate issue as **closed for the EP4 path**. Do not read this as
"the 361 duplicates were explained": the guard landed before the counters
existed, so nobody ever watched `dup_blocks` on a build that could still
duplicate. What is established is that the suspect window was live and frequent,
and that it is now impossible by construction rather than by luck.

If duplicates reappear, the counters to read are, in this order: `dup_blocks`
(non-zero means the ordering was broken again), then `tog_bad` — the
target-facing RX re-arm in `target_ep_resync()` (now only reached from the TX
watchdog) resets the expected data toggle to DATA0, and a peer sitting on the
opposite toggle has its next packet ACKed and discarded. That is a **hole**,
not a duplicate: the reassembler only resyncs on the `NCMH` signature and
`NCM_ParseNtb()` checks three of them (NTB, NDP, and the descriptor bounds), so
a shifted stream is rejected rather than delivered. The worst case is one lost
NTB — one Ethernet frame — and TCP retransmits it. `unproven` rising at the same
time means the peer has gone quiet at every address we know, not that a frame
was duplicated.


## What was tried, and what it did

| Change | Purpose | Result |
|---|---|---|
| Call `NCM_Bridge_Poll()` **before** `TMOS_SystemProcess()` | take the BLE residency out of the bridge's iteration budget | no measurable change; kept (it removes a dependency that should not exist) |
| Bound the tunnel pump to `BLE_ETH_TX_CHUNKS_PER_PASS` = 4, re-arm with `tmos_start_task(..., 1)` | cap `TMOS_SystemProcess()` residency instead of letting it run to buffer exhaustion | not exercised by these tests (the pump only runs with an active BLE link and tunnel traffic); kept as a latency bound |
| Re-entry guard (`s_poll_busy`) in `NCM_Bridge_Poll()` | close the duplicate-NTB window and make ISR use safe | duplicates not reproduced either way; no throughput cost (0.55 MiB/s target→host before and after) |
| Claim the guard atomically (`ncm_irq_save()` around the test-and-set) | a plain load/store pair can be split by the EP4 ISR, which then runs the body too | no measurable cost; `poll_skipped` now counts the refusals (≈4 250 per 8 MiB run) |
| Advance `s_host_ntb_tx_off` **before** arming EP4, roll back on refusal | make a duplicate arm impossible even if the guard is ever broken again | `dup_blocks` stayed 0 across 4 runs; wire showed 0 duplicate replies |
| *Rejected:* call `NCM_Bridge_Poll()` from the EP7 IN completion (mirroring EP4) | give the target-facing TX the same interrupt assist as the host-facing one | **made it worse** — 76.4% loss, RTT 10–21 ms, same 0.56 MiB/s. It put the whole poll body, including the host-side path and the two polls, into an ISR that fires every 64-byte chunk. Reverted. |
| *Not tried:* `NCM_BRIDGE_QUEUE_DEPTH` 2 → 4 | the only change that can actually reduce same-direction loss | costs **+3028 B RAM** (89.17% → ~93.8%) on the current images |

The last row is the honest answer to "how do we fix the loss": on a saturated
link the only lever is more buffer, and that buffer does not exist in this RAM
budget. Everything else in the table either removes a hazard or changes nothing.


## The silence watchdog was firing on idle

The target-side silence watchdog assumed "a live Windows always emits something
within seconds (NCSI probes, discovery, ARP)", so 10 s with no EP5 OUT interrupt
was taken as proof of a dead pipe, and the recovery is destructive: reset both
data toggles, clear the reply queue, send a link DOWN/UP notification so NDIS
restarts the interface.

That premise is false. A connected, healthy target simply has nothing to say
sometimes, and the numbers, taken on an otherwise idle bench:

| Before the fix | Counters |
|---|---|
| 60 s of idling, no traffic from either side | `silence_wd` 2 → 3, `tog_bad` 0 → 1, `set_if5` 2 → 2 |
| 90 s more idling | `silence_wd` still 3, `tog_bad` still 1 |
| `ping` before and after | 0% loss, 1.9–2.5 ms |

Read it as: the watchdog fired because the link was quiet, not because it was
broken. Each fire cost exactly one 64-byte packet — `tog_bad` counts EP5 OUT
interrupts whose data toggle did not match, which the controller has already
ACKed, so the payload is gone and the host never retransmits it. And
`set_if5` did not move, i.e. the host never re-issued `SET_INTERFACE`: the
toggle reset was unilateral, which is exactly what creates the one-packet hole.
Whether the hole costs anything depends on where it lands: a fire only happens
while the reassembler is empty, so it normally eats the first packet of a fresh
NTB and that NTB is dropped.

The fix keeps the recovery but makes it earn the evidence: after 10 s of
silence the bridge now queues **one broadcast ARP request for the target's own
address** (`target_probe_peer()`) and only resets the endpoints if nothing at
all comes back within 1.5 s. A live IPv4 stack always answers ARP; a paused
pipe or a desynced toggle cannot. Measured after the change, with the same
bench:

| After the fix | Counters |
|---|---|
| 90 s of idling | `probes` 2 → 9, `silence_wd` 0, `tog_bad` 0, `set_if5` 2 |
| 8 MiB target→host + 150 probes | 0.55 MiB/s, 0% loss, 0 duplicates, `tog_bad` 0, `silence_wd` 0 |

So an idle link now costs a 42-byte ARP request every ~10 s instead of a lost
chunk plus a link bounce, and the "quiet but alive" case can no longer be
mistaken for a fault.

### The probed address is learned, not assumed

`target_host_ip` is what the bridge *expects* the target to use, not what the
target *does* use. A machine whose IPv4 was set by hand on the far side answers
nothing for it, so the first version of this ladder went unanswered and then
bounced a perfectly healthy link every 12 s.

Two changes close that:

- Every frame the target sends teaches the bridge the address it speaks from —
  the ARP sender field or the IPv4 source, whichever the EtherType provides.
  A peer cannot hide it, and it is read before any handler runs. DHCP DISCOVERs
  (source `0.0.0.0`) are ignored, as is anything with a broadcast source.
- An unanswered probe for `target_host_ip` is not a verdict. The ladder asks the
  learned address next, and only then draws a conclusion. Measured with the
  target moved to **192.168.20.77 by hand** while `target_host_ip` stayed `.2`:

  | Counter | Δ over ~6 min at the "wrong" address |
  |---|---|
  | `seen_ip` | followed to 192.168.20.77 |
  | `probes` | +8 over 58 s of idling, i.e. two per cycle: `.2` (unanswered) then `.77` (answered) |
  | `unproven` | 0 — the second probe was always answered |
  | `tog_bad` / `set_if5` | 0 / 2 — no toggle reset, no link bounce |
  | `probe_interval` | stayed 10 s, so no back-off was entered |

  The old code would have fired about five times over the same window, each fire
  worth one `tog_bad`. It stayed at 0.

The learned address is not only a probe target: proxy-ARP and the Host→Target
unicast filter match it too (see `ncm-bridge.md` §"Proxy-ARP and forwarding
follow the address the peer actually uses"). Without that, the watchdog stopped
torturing the link but the Host still could not reach a hand-addressed target —
`ping .77` timed out and the ARP for `.77` went unanswered. With it: `ping` 0%
loss, `ssh` to `.77` works, and both transfer directions run at their usual rate
(0.55 / 0.52 MiB/s, 0 duplicates).

### When nothing answers, the bridge stops escalating

If neither address answers, "dead pipe" and "peer living at an address we have
never seen" produce exactly the same evidence, and nothing here can tell them
apart. So the watchdog no longer resets in that case: it counts `unproven` and
doubles the silence interval (10 s → 20 → 40 → 80, capped at 120 s), returning
to 10 s as soon as the peer sends anything at all.

That is a deliberate trade. The reset was measured to cost a packet every time
and has never been observed to fix anything — `set_if5` does not move, so the
host does not even re-issue `SET_INTERFACE`, and the toggle reset is unilateral.
The failure modes a reset could plausibly fix are signalled elsewhere: a host
that stops reading leaves EP7 busy, which the TX watchdog owns, and a desynced
toggle heals on the peer's very next packet, which also counts as an answer
here. The price is that a pipe which is genuinely dead *and* silent needs a
replug, because nothing on this side can distinguish it from an idle one.

Caveat on interpretation: the old `silence_wd` byte (it read 3 in the session
that produced the duplicate-frame work) is **not** evidence that the target pipe
was broken — it was this false positive, three times. That byte now reports
`probe_interval` instead, because the path it counted no longer resets.


## The CI artifact drops far more small frames than a local build

Same source, same flags (`-Os -flto=full -fuse-linker-plugin -ffat-lto-objects`),
different compiler: the CI runner uses GCC 8.2.0 (MounRiver), the bench build
here uses GCC 15.2.0 (xpack). Under a saturated target→host transfer the two
behave very differently, and this was found only by flashing the CI artifact
and running the same script against it.

Test: 8 MiB TCP target→host, 100 ICMP probes at 10/s from the Host to the
target address, 0 duplicate replies in every run.

| Build | `ping` loss (7 / 6 runs) | `g_host_tx_queue.overflow` Δ per run | throughput |
|---|---|---|---|
| local, GCC 15.2 | 0, 0, 0, 0, 1, 1 % | +4 | 0.55 MiB/s |
| CI, GCC 8.2 + LTO | 71, 74, 76, 76, 77, 79, 81 % | +1 585 | 0.53–0.54 MiB/s |

Where the loss happens, measured rather than assumed: the target's own ICMP
counters moved by exactly +100 received and +100 replies sent in a run where the
Host received 29. So the requests crossed the link, the target answered every
one, and the replies — 98-byte frames — were dropped inside the device on the
way out. The `hq_overflow` delta above says where: the target→host queue is two
frames deep and the transfer keeps it full, so a small frame arriving in a full
queue is dropped whole.

Throughput, retransmission behaviour, duplicates and link stability are
unaffected; this is a loss-rate difference, not a correctness one. It matters
because the shipped firmware *is* the CI artifact: the "0–1% ICMP loss under
target→host saturation" figure earlier in this document describes a locally
built binary, not the one users flash. If you are re-measuring loss, record
which compiler produced the image.

The cause is not identified. Both builds run the same poll loop and the same
2-frame queue, and the drain sits within a few percent of the arrival rate
(§Slack arithmetic), which makes the drop rate sensitive to per-frame timing —
but a 400× difference is more than "a few microseconds". Next step if this
matters: disassemble `NCM_Bridge_Poll()`/`host_tx_feed()` from both ELFs and
compare the hot path, or bisect by building locally with GCC 8.2.

## How to verify

1. Idle link, no load: `ping` both ways must stay at 0% loss, 1–2 ms (any loss
   here is a different problem, see `ble-eth-ssh-usage.md` §Common Failures).
2. Saturate one direction with 8 MiB of TCP and probe **in the same direction**:
   expect 40–60% loss and RTT equal to the 2-frame drain time (~5 ms). This is the
   baseline behaviour, not a fault.
3. Repeat with the probe in the **opposite** direction: expect 0–2% loss and
   RTT 110–230 ms. If this instead shows loss, something other than queue depth
   is wrong and the counters below will say which side.
4. If a change claims to reduce loss, it has to move item 2 below ~40%, and it
   has to do so across at least four runs — the run-to-run spread on this metric
   is 15 points wide, wide enough to make a two-run comparison lie.

The `0x33/0x10` diagnostics frame exposes `host_frames`, `hq_overflow`,
`poll_calls`, `tx_sent` and `zlp_sent` (58-byte reply payload — full, nothing
else fits), which is enough to separate "arrived at the bridge" from "dropped on
the way out". The target-facing queue's overflow counter, the re-entry skips,
the duplicate re-arms, the toggle-mismatch count and the probe count are
exported by `0x33/0x13` instead, at payload bytes 20–31 (see
`GetBridgeDiagnostics()` for the layout).

Reading `0x13` over BLE has two traps worth knowing before blaming the firmware
for silence: the response can take longer than 1.5 s while the BLE-Eth pump is
busy, and a reconnect often lands on the stale duplicate `CBPeripheral` that
negotiates **MTU 60** and then delivers no notification at all. Retry until the
log shows MTU 247 (see `ble-eth-ssh-usage.md` §Common Failures); the 38-byte
payload does not survive the MTU-60 identity.
