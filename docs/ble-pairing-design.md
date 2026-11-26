# KeyMod / KVM-GO BLE pairing security design

- Branch: `feature/ble-pairing-security`
- Baseline: `main` @ `1ee6c00`
- Applies to: `ENABLE_BLE` boards such as KeyMod (`BOARD_VERSION=3`); the current KVM-GO release has no Bluetooth
- Status: **M1 + M2 + long-press button implemented** (see §12); M3 is app-side work only; P1/P1b/P3 in §14 still need bench verification
- Decision history: 2026-11-03 settled the PIN model and the "no pairing window" principle; 2026-11-09 the limit went from 4 to **1** (user: "one phone is safer"), and the same day it was decided the limit becomes **a configurable item stored in flash**, which turns single-binding from a hard constraint into the default setting; 2026-11-10 the initial PIN was fixed at **666666 across all devices** (user request) and implementation started

---

## 1. Current state: the pairing path is an empty shell

The bonding parameters are nominally on, but in practice any passer-by's phone can operate the device. Pin the facts down one by one.

| Item | Current state | Location |
|---|---|---|
| Data-path admission | FFF2 value attribute permission is `GATT_PERMIT_WRITE`, writable while unpaired/unencrypted → full control of HID, every CH9329 command, the BLE-Eth tunnel | [ble_usb_service.c:135-140](file:///Users/txa/project/Openterface_KM/Profile/ble_usb_service.c#L135-L140) |
| Subscription admission | FFF1 CCCD permission is `GATT_PERMIT_READ \| GATT_PERMIT_WRITE`, subscribable without encryption | [ble_usb_service.c:121-126](file:///Users/txa/project/Openterface_KM/Profile/ble_usb_service.c#L121-L126) |
| passkey | hard-coded `0` ("000000"), the same value on every device in the field | [peripheral.c:471-483](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L471-L483) (after implementation: default `666666`, changeable in flash, see §4) |
| Bond callbacks | Both members of `gapBondCBs_t` are NULL; the application has no idea whether pairing succeeded | [peripheral.c:380-384](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L380-L384) (after implementation: `pairStateCB` is wired up, see §10) |
| `0x22` security commands | 0x01/0x02/0x03 are all TODO stubs returning fake success; 0x03 always reports 0 bonds | [peripheral.c:1294-1334](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L1294-L1334) (after implementation: 0x02/0x03/0x04/0x05/0x06, see §8) |
| Behaviour when bonds are full | The application never sets `GAPBOND_ERASE_AUTO` → the ROM default is **on**: pairing device N+1 silently wipes every existing bond | [wchble_rom.h:1122](file:///Users/txa/project/Openterface_KM/LIB/wchble_rom.h#L1122) |
| Legacy dead code | `SimpleProfile_AddService` is called, but the whole registration body is commented out, so it is an empty function. `SIMPLEPROFILE_SERV_UUID`(0xFFF0) in the advertisement happens to match the `ble_usb` service UUID, which is misleading naming. **Not an attack surface**, cleanup only | [peripheral.c:489](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L489), [gattprofile.c:294-313](file:///Users/txa/project/Openterface_KM/Profile/gattprofile.c#L294-L313) |

Conclusion: writing FFF2 over BLE = injecting arbitrary keyboard input into the target machine, with no credentials required. That ranks ahead of the R1 risk recorded in the tunnel design document — [ble-ip-tunnel-design.md:309](file:///Users/txa/project/Openterface_KM/docs/ble-ip-tunnel-design.md#L309) says "the IP entry point needs a bonded session", but the entry point itself was still bare at that time.

## 2. Threat model

| Attacker | Today | After this design |
|---|---|---|
| Any random phone that scans nearby | writes FFF2 directly, full control | stopped at the protocol stack, write rejected |
| Someone who can operate BLE pairing (no PIN) | pairs with "000000", gains persistent access | pairing fails |
| Someone who knows the initial PIN 666666 | pairs with "000000" | **can still pair** (while the slot is empty and the PIN is unchanged), see the eviction trade-off in §6 |
| Someone who can plug in USB | full control | full control — USB is the recovery path, no admission control |

The main threat is the first and second row (passers-by without a PIN), and this design solves it. The third row is equivalent to a public credential while the PIN is unchanged, and is an acceptable residual risk (eliminated by changing the PIN after the first pairing); §6 gives the v2 remedy.

## 3. Design principles

1. **PIN = root of trust**. The initial 6-digit PIN is `666666`, changeable to a private PIN; `MITM=TRUE` + `GAPBOND_IO_CAP_DISPLAY_ONLY`. The device has no screen and no keys, so the PIN is only entered from the phone side; it is never echoed when set, and can only be read back explicitly via USB or an encrypted BLE link using `0x22 0x07` (since 2026-11-12, see §8) — and the read-back is open only to the currently bound phone, so an unencrypted link cannot get it.
2. **Admission is enforced at the protocol stack layer**, not by hiding the advertisement and not by application-layer checks. Application-layer checks are only defence in depth (because ROM behaviour cannot be read, see P1).
3. **Minimal UI**: no pairing window, no short press, no multi-colour LED. If the binding slot is empty it can pair at any time; if occupied, it refuses.
4. **Security parameters are changed at different authorisation levels**: the binding limit goes over USB only (it decides the access model); changing the PIN may go over USB or an encrypted BLE link (the current phone itself); an authenticated BLE link can also unbind itself.
5. **Failures are recoverable**: after clearing the override, 666666 always works (recovery-code semantics); the USB path is always available.

## 4. Security model: initial PIN 666666

### 4.1 Effective PIN

```
effective_pin = has_pin_override ? override_pin : BLE_PAIR_DEFAULT_PIN (= 666666)
```

The initial PIN is `666666` on every device (`BLE_PAIR_DEFAULT_PIN`, a compile-time constant, takes no flash), installed at startup with `GAPBondMgr_SetParameter(GAPBOND_PERI_DEFAULT_PASSCODE /*0x407*/, 4, &effective_pin)`. No label-printing process is needed.

**The cost of that choice must be stated plainly**: 666666 is a public constant, so anyone who knows this design can pair an empty-slot device. The out-of-box security level therefore depends on "the slot is occupied" — after the first phone binds, a new phone must wait for `0x22 0x02` to unbind. **Secure use requires changing to a private PIN with `0x22 0x04` immediately after the first pairing**, and the app should guide that step after the first successful connection. If you insist on possession semantics ("only someone holding the hardware can use it"), revert to a unique per-board PIN (the original scheme: `CRC32(MAC) % 1000000` printed on a label).

Keeping the passcode callback NULL is a deliberate choice: with `DISPLAY_ONLY` + a preset passcode the stack needs no application involvement, but that behaviour has not been measured (P3).

### 4.2 The PIN is changeable, 666666 is the recovery code

Add a runtime override (stored in flash, §9). Reset clears the override → 666666 works again. The new PIN is only delivered over USB or an encrypted BLE link, and the firmware never echoes it.

## 5. Bond policy: single binding by default, configurable limit

### 5.1 The predicate

There is exactly one capacity predicate in the system, refreshed in three places (after boot, after every successful pairing, after the limit is persisted):

```c
GAPBondMgr_GetParameter(GAPBOND_BOND_COUNT /*0x415*/, &cnt);
uint8_t mode = (cnt >= g_pair_cfg.max_bonds) ? GAPBOND_PAIRING_MODE_NO_PAIRING /*0x00*/
                                             : GAPBOND_PAIRING_MODE_WAIT_FOR_REQ /*0x01*/;
GAPBondMgr_SetParameter(GAPBOND_PERI_PAIRING_MODE /*0x400*/, 1, &mode);
```

Companion parameters: `GAPBOND_ERASE_AUTO(0x41E) = 0` **must be turned off explicitly** (default on = silently wipe old bonds, see §1); keep the default `GAPBOND_BOND_AUTO(0x418) = 1` so bonds survive power loss.

### 5.2 The limit is configurable

| Item | Value |
|---|---|
| Default | **1** (one phone, exclusive) |
| Configurable range | 1..4, clamped by the firmware. The upper bound of 4 is justified: `GATT_MAX_NUM_CONN = 4` ([wchble.h:647](file:///Users/txa/project/Openterface_KM/LIB/wchble.h#L647)), and the CCCD array is sized 4 as well |
| Storage | flash record (§9), retained across power loss |

The ROM stack's own bond capacity is not published (P2). A default of 1 means **that unknown no longer blocks release** — the stack cannot possibly support fewer than one bond. Only when a user deliberately raises it to 4 does that multi-phone test matrix become necessary.

### 5.3 Exact semantics of the limit: gate new pairings only, never evict existing ones

Lowering the limit **performs no cleanup at all**. Limit lowered from 4 to 1 with 3 devices already bound: those 3 reconnect by starting encryption directly with their stored LTK, which does not go through the pairing path and is unaffected by `NO_PAIRING` (premise P1b needs measurement); only new pairings are refused.

If the user wants "the limit is the current access list", they go through an explicit `0x22 0x02` full erase and re-bind. **The firmware never performs implicit over-limit cleanup** — it is hard to troubleshoot and violates principle 3.

### 5.4 Old problems that single binding removes

- The whole idea of "add phones 2-4 without a gesture" disappears.
- The "all 4 lose their bonds while full → new phone locked out" edge case disappears: the slot is either empty or occupied, a two-state machine.
- The regret of per-device revocation disappears: there is one slot, and `GAPBOND_ERASE_ALLBONDS` is revocation. `ERASE_SINGLEBOND(0x417)` is unnecessary (it could never enumerate anyway; the public API gives a count, not a list).

## 6. The cost of single binding: handover is explicit, and there is an eviction window

This is the only trade-off that needs a decision here, so state it clearly.

### 6.1 Switching phones (main path, no USB needed)

Disconnecting **no longer frees the slot** — a bond is persistent, disconnecting only drops the link. So there must be an explicit handover:

1. On the old phone, tap "unbind this phone" in the app → it sends `0x22 0x02` **over its own encrypted link**;
2. The firmware erases the single bond (that phone's own), freeing the slot;
3. The old phone's user goes to the system Bluetooth settings and "forgets this device" (skipping this on iOS makes the next pairing misbehave, so the app's prompt must include it);
4. The new phone pairs with the current PIN (666666 from the factory).

Security argument: an authenticated phone erasing **the single bond that belongs to itself** creates no new attack surface — it can walk away at any time. So `0x22 0x02` is relaxed from "USB only" to "USB or encrypted BLE". That relaxation is a consequence of single binding; it cannot be allowed in a multi-slot model (otherwise any device could DoS all of them).

### 6.2 Residual weakness: a PIN holder can evict the incumbent

The combined weakness of single binding plus a known initial PIN: once the slot is empty (incumbent unbound, or device reset over USB), **anyone holding the PIN can pair immediately and take exclusive control**. With 4 slots an attacker at worst occupies one, leaving the incumbent unaffected; with 1 slot it is exclusive eviction.

So "one phone is safer" holds for keeping out passers-by without the PIN (the main threat — it holds), but it hands a third party who has the PIN an exclusive right.

**v2 remedy (not in this version)**: after pairing, the app negotiates a per-phone ownership key generated locally on the device over the encrypted link and stores it in the app/keychain; after that, a new phone can only become "owner" through a signed handover from the incumbent. Without that, treat the PIN as a seal.

## 7. Data-path admission (the load-bearing part)

Two layers, neither optional. This is the part of the design that actually stops attackers.

| Layer | Change | Location |
|---|---|---|
| ① GATT permissions | FFF2 value: `GATT_PERMIT_WRITE` → `GATT_PERMIT_WRITE \| GATT_PERMIT_ENCRYPT_WRITE`(0x80); add `GATT_PERMIT_ENCRYPT_WRITE` to the CCCD | [ble_usb_service.c:135-140](file:///Users/txa/project/Openterface_KM/Profile/ble_usb_service.c#L135-L140), [:121-126](file:///Users/txa/project/Openterface_KM/Profile/ble_usb_service.c#L121-L126), bit definitions at [wchble.h:604](file:///Users/txa/project/Openterface_KM/LIB/wchble.h#L604) |
| ② Application fallback | First line of the `BLE_USB_EVT_BLE_DATA_RECIEVED` branch in `ble_usb_ServiceEvt`: `if(!linkDB_State(connection_handle, LINK_ENCRYPTED)) return;` | [peripheral.c:1107](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L1099-L1158), API at [wchble_rom.h:3120](file:///Users/txa/project/Openterface_KM/LIB/wchble_rom.h#L3120), `LINK_ENCRYPTED=0x10` at [:270](file:///Users/txa/project/Openterface_KM/LIB/wchble_rom.h#L270) |

**Why both**: enforcement of ① is done by the ATT layer inside the ROM, and we cannot read the source to confirm it really checks the encryption bit (P1). ② is a gate we control for certain. ① alone may be empty; ② alone means a library call on every write — so keep both.

Expected error code for an unencrypted write: `ATT_ERR_INSUFFICIENT_ENCRYPT` = **0x0F** ([wchble.h:565](file:///Users/txa/project/Openterface_KM/LIB/wchble.h#L565)). Note it is not 0x08 `INSUFFICIENT_AUTHOR` — that one is for signed write.

The USB side (ACM/CH9329 serial path) gets **no** admission control: plugging the cable is itself proof of possession, and it must remain the recovery path for when "BLE has locked itself out".

### 7.1 Request encryption immediately after connecting (mouse latency fix, 2026-11-10)

After M2 landed, users reported a 1–2 s mouse delay. The root cause was not the report path (the MS_REL queue holds 64 entries with drop-oldest, the main loop drains it every iteration, the USB EP busy-wait cap is 20 ms, BLE sends 1 packet per connection event ≈ 12.5 ms — none of that can stack up to seconds), but **upstream of the admission gate**:

1. The firmware never initiated the security procedure, so the link stayed plaintext until the host first touched a protected attribute;
2. On first connect, the app's CCCD/FFF2 writes were rejected by ATT with 0x0F;
3. iOS **does not retry** a failed CCCD write; the app itself re-subscribes only once after 0.5 s, and `sendTouchData` drops everything until `connectionState` reaches `.connected`.

So every (re)connect had a 0.5–2 s dead window for the pointer, exactly the delay users perceived. The fix is to have the peripheral initiate encryption once the link is up (Slave Security Request):

- In `Peripheral_LinkEstablished`, `tmos_start_task(Peripheral_TaskID, SBP_SECURITY_REQ_EVT, 4)` (4 ticks ≈ 2.5 ms, waiting for the connection event to settle), [peripheral.c:947](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L947);
- The event handler calls `GAPBondMgr_PeriSecurityReq(handle)`, only when connected and not yet `LINK_ENCRYPTED`; a bound link encrypts within one connection event, a first-time link pops the PIN dialog immediately, [peripheral.c:825-835](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L825-L835);
- `Peripheral_LinkTerminated` calls `tmos_stop_task` so a stale event cannot fire on the next link, [peripheral.c:979](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L979).

Side effect: encryption moves from "the host's first write" to "as soon as the link is up", which turns **P1b (whether NO_PAIRING blocks encryption on an already-bound link) from a marginal premise into a must-test** — with the slot full, an old phone reconnecting must still encrypt successfully, and T4 covers that path.

Dead-code cleanup: remove the `SimpleProfile_AddService` call and the corresponding comment block in `gattprofile.c`. It is hygiene, not security (§1 already notes the registration body is commented out).

### 7.2 Latency during continuous dragging: HID report ACKs + link budget (2026-11-11)

§7.1 removed the dead window **at connection time**. The user re-checked the iOS client: the start no longer stalls, but **during continuous dragging** the pointer still trails the finger by 1–2 s, and falls further behind the longer you drag. That is a steady-state problem, unrelated to pairing admission (`18b3b8c` added only a permission bit and one `linkDB_State` call to the hot path), caused by three things stacking:

1. **Every HID report is ACKed**: `ble_usb_ServiceEvt` turns on `g_ble_ch9329_response` for all CH9329 commands arriving over BLE ([peripheral.c:1269](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L1269)), and the mouse/keyboard cases set `resp_mode=1` unconditionally ([usbd_compostie_km.c:286](file:///Users/txa/project/Openterface_KM/APP/usbd_compostie_km.c#L286)) → every movement = an 11 B uplink plus a 7 B downlink notification.
2. **The outbound budget is 1 packet per connection event**: `BLE_TX_NUM_EVENT=1`, and the controller's 5 buffers are **shared between TX and RX** ([wchble.h:87-93](file:///Users/txa/project/Openterface_KM/LIB/wchble.h#L87-L93)); the firmware asks for 12.5 ms and iOS refuses (15 ms floor for app-class accessories, often landing at 30 ms in practice) → 33–66 packets/s, and the ACKs consume the whole budget.
3. **The app sends one packet per touch sample**: no coalescing, no `canSendWriteWithoutResponse` check ([BLEManager.swift:1056](file:///Users/txa/project/Openterface_KeyCmd_iOS/KeyCmd/Managers/BLEManager.swift#L1056)), 60–120 packets/s exceeds what the link supplies → backlog in the phone's send queue, drained only after the finger lifts.

Firmware fixes (implemented 2026-11-11):

- **B — do not ACK HID reports**: add `BLE_CH9329_RESP_NONE` so HID commands (0x02/0x03/0x04/0x05) arriving over BLE produce no response at all; query commands (0x01/0x08/0x19/0x22/0x23/0x26/0x27/0x30…) still answer over BLE, so the security panel, renaming and ECM reads are unaffected ([peripheral.h:81-96](file:///Users/txa/project/Openterface_KM/APP/include/peripheral.h#L81-L96), [usbd_compostie_km.c:119-134](file:///Users/txa/project/Openterface_KM/APP/usbd_compostie_km.c#L119-L134)).
- **C — raise the link ceiling**: request range 10→12 (15 ms, [peripheral.c:66-72](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L66-L72)); `BLE_TX_NUM_EVENT=4`; also `BLE_BUFF_NUM 5→8` (four TX buffers would squeeze the shared pool down to a single RX) and `BLE_MEMHEAP_SIZE 6144→8192` ([makefile:297-312](file:///Users/txa/project/Openterface_KM/build/makefile#L297-L312)). RAM usage 53160/65536 = 81%.
- Compatibility cost: third-party clients that rely on HID ACKs to detect errors will no longer see a response (our app does not depend on it; `keymod-test` needs a separate check).

**Not done**: the app still sends per sample, with no coalescing and no `peripheralIsReady` gating. If the bench still shows lag, the next step is ~40 Hz accumulation of dx/dy plus `canSendWriteWithoutResponse` gating on the client.

## 8. The `0x22 CMD_BLE_SECURITY` command set

The command byte is defined in [usbd_composite_km.h:51](file:///Users/txa/project/Openterface_KM/APP/include/usbd_composite_km.h#L51) and dispatched in [usbd_compostie_km.c:303](file:///Users/txa/project/Openterface_KM/APP/usbd_compostie_km.c#L303). The current implementation at [peripheral.c:1294-1334](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L1294-L1334) is replaced wholesale.

| Sub-command | Meaning | Payload/response | Link requirement |
|---|---|---|---|
| `0x01` | the old "open a pairing window" — **deprecated** | returns `STATUS_ERR_PARAM` | — this model has no pairing-window concept |
| `0x02` | erase all bonds + clear the PIN override + limit back to 1 (= factory reset of security parameters) | responds `[status]` | USB **or** encrypted BLE (§6.1 handover path) |
| `0x03` | bond status | responds `[status, count, max_bonds]`, count from `GAPBOND_BOND_COUNT`, max from the RAM cache | USB or encrypted BLE |
| `0x04` | **new**: set the PIN override | 6 ASCII digits, validated 0..999999 | USB or encrypted BLE, **never echoes the PIN** |
| `0x05` | **new**: set the binding limit | 1 byte, clamped 1..4 | **USB only** (principle 4) |
| `0x06` | **new**: read the binding limit | responds `[status, max_bonds]` | USB or encrypted BLE |
| `0x07` | **new** (2026-11-12): read the effective PIN | responds `[status]` + 6 ASCII digits (the override or `666666`) | USB or encrypted BLE; older firmware returns `E2` |

Note the disambiguation rule against the BLE-Eth ECM IP commands: `0x22 [0x03]` is reserved for the CH9329 bond query, while shapes such as `0x22 [0x04]+9/13/21B` go to BLE-Eth ([peripheral.c:201-234](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L201-L234)). The new single-byte sub-commands 0x04/0x05/0x06 must be re-checked against `Peripheral_IsBleEthEcmIpFrame`'s shape predicate — 0x04 is already taken by the ECM "reset to defaults", so the PIN sub-command's predicate must discriminate by length (the PIN is 6 ASCII bytes, not 9/13/21). This is a concrete implementation trap, covered by test matrix T11.

## 9. Flash record

Pages up to `0x0806B000..0x0806F000` are already in use ([ble_name_config.c:16-19](file:///Users/txa/project/Openterface_KM/APP/ble_name_config.c#L16-L19), [ecm_ip_config.h:17-26](file:///Users/txa/project/Openterface_KM/APP/include/ecm_ip_config.h#L17-L26)). **The next free page, `0x0806A000`**, holds this record.

Merge the PIN override and max_bonds into one record; do not open two pages:

```c
#define BLE_PAIR_MAGIC     0x50454C42u  /* "BLEP" */
#define BLE_PAIR_VERSION   1u
#define BLE_PAIR_FLASH_ADDR 0x0806A000u
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  max_bonds;          /* 1..4, default 1 */
    uint8_t  has_pin_override;
    uint8_t  rsvd;
    uint32_t pin_override;       /* 0..999999 */
    uint16_t seq;                /* write sequence, so a stale record cannot win */
    uint32_t crc;                /* FNV-1a, same pattern as ble_name_config */
} BLE_PairConfig_t;
```

Add `APP/ble_pair_config.c` + `APP/include/ble_pair_config.h`, with an interface in the style of `usb_mode_config`: `BLE_PairConfig_Init/Get/SetMax/GetEffectivePin/SetPinOverride/ClearPinOverride`. An invalid or corrupt record → fall back to defaults `{max=1, no override}` and self-heal on the next write. Rewrite the whole page; this page is written very rarely (changing the PIN or the limit is a manual action), so no wear levelling is needed.

## 10. LED and advertising

**LED (PC7, single-colour PWM, no RGB)**: the existing `g_ble_led_state` 0-4 semantics are unchanged ([peripheral_main.c:184-211](file:///Users/txa/project/Openterface_KM/APP/peripheral_main.c#L184-L211), definitions at [peripheral.c:155](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L155)). pairStateCB adds two states that last about 3 s and then fall back to the connected state:

- `5` = pairing succeeded: two short flashes
- `6` = pairing failed / wrong PIN: one rapid flash

No colours, no gesture modes.

**Advertising capacity field**: the design considered adding `[len, 0xFF, vid, occupied, count]` to scanRsp so the N+1st phone knows the slot is full before connecting. Today `advertData` is at most 7+1+1+20=29 B (name limit 20), and `scanRspData` already uses 18+name ([peripheral.c:429-450](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L429-L450)); another 5 B would squeeze out the conn-interval field or truncate the name.

**Not in v1.** After connecting, the app queries `0x22 0x03` first and then decides whether to say "occupied / please unbind" — with single binding that is a two-state message, costing one extra round trip. Add it once there is real demand from bare clients (not our app).

## 11. Factory reset (hold the BOOT key for 5 s)

**The BOOT key on KeyMod can be sampled while the firmware is running** (verified by the user on 2026-11-10), wired to **PB8**. The implementation is [button.c](file:///Users/txa/project/Openterface_KM/APP/button.c) + [button.h](file:///Users/txa/project/Openterface_KM/APP/include/button.h#L18), and the main loop calls `Button_Poll()` every iteration ([peripheral_main.c:83](file:///Users/txa/project/Openterface_KM/APP/peripheral_main.c#L83)).

Timing:

1. Press → 20 ms debounce, timing starts;
2. Hold for **5 s** ([button.c:81](file:///Users/txa/project/Openterface_KM/APP/button.c#L81)) → enter pending-confirmation, PC7 flashes **at 100 ms**, prompting release;
3. Release → after 20 ms debounce run `Peripheral_FactoryResetSecurity()` (erase all bonds → clear the PIN override → limit back to 1), then `Peripheral_ScheduleSystemReset(500)` to reboot ([button.c:99](file:///Users/txa/project/Openterface_KM/APP/button.c#L99));
4. Release before 5 s → silently cancelled, no state change.

**Why reboot only after release**: while held, BOOT0 is high, and calling `NVIC_SystemReset()` at that moment makes the chip enter the ROM ISP after reset instead of running the firmware. Moving the reset to after release (BOOT0 already low) is safe. The rapid flash therefore carries two meanings at once: "please release" and "release will execute".

**Electrical constraint**: on the QFN28 CH32V208GBU6, PB8 is co-packaged with BOOT0, and the datasheet notes that the pin's input function is restricted after co-packaging, so configure it as a **floating input and never enable the internal pull-up** — a pull-up would pull BOOT0 high and send the next power-up into the ISP. The key idles low (external pull-down) and goes high when pressed, consistent with "hold BOOT to enter ISP" during flashing. If the schematic puts the key on a pin other than PB8, only the port/pin macros in [button.h:18](file:///Users/txa/project/Openterface_KM/APP/include/button.h#L18) need changing.

**Power-up protection**: if init reads a high level, require seeing a low level once before timing may start ([button.c:52](file:///Users/txa/project/Openterface_KM/APP/button.c#L52)), so a miswired net or a missing pull-down does not leave it repeatedly entering pending-confirmation.

**Bench self-check**: hold for 5 s, the LED should start flashing rapidly; after release the device reboots, USB `0x22 0x03` returns `count=0, max=1`, and the old phone must "forget this device" in system Bluetooth before it can pair again with 666666. If the LED never flashes while held, that pin is not reading the key — check the schematic net first.

USB `0x22 0x02` ([peripheral.c](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L453)) and encrypted-BLE `0x22 0x02` (§6.1 handover path) still exist and share the same `Peripheral_FactoryResetSecurity()` as the button. With single binding, that function is also the fallback for the day-to-day phone-switching flow.



## 12. Implementation plan

Change list (numbering follows §1-§11, status as of 2026-11-10):

| # | File | Change | Milestone | Status |
|---|---|---|---|---|
| 1 | `Profile/ble_usb_service.c` | add `GATT_PERMIT_ENCRYPT_WRITE` to FFF2 + CCCD permissions | M1 | implemented |
| 2 | `APP/peripheral.c` | add the `linkDB_State(LINK_ENCRYPTED)` fallback in `ble_usb_ServiceEvt` | M1 | implemented |
| 3 | `APP/peripheral.c` | remove the `SimpleProfile_AddService` call (empty function) | M1 | implemented |
| 4 | `APP/peripheral.c` | `0x22 0x03` returns the real count + max | M1 | implemented |
| 5 | `APP/peripheral.c` | Bond Manager block: passkey from `effective_pin`, explicit `ERASE_AUTO=0`, capacity predicate | M2 | implemented |
| 6 | `APP/peripheral.c` | implement `pairStateCB` (LED 5/6, bond refresh, mode switch); passcodeCB stays NULL | M2 | implemented |
| 7 | `APP/ble_pair_config.c/.h` (new) | flash record + `BLE_PairConfig_Init` hooked into `Peripheral_Init` | M2 | implemented |
| 8 | `APP/peripheral.c` | implement `0x22 0x02`, add `0x04/0x05/0x06` | M2 | implemented |
| 9 | `APP/button.c` + `APP/include/button.h` (new) | 5 s long press → rapid LED → factory reset on release + reboot; PB8 floating input | M3 | implemented |
| 10 | `keymod-test` | pairing guide + PIN field, wire up `0x22 0x03/0x04/0x05/0x06/0x02`, "unbind this phone" button + iOS "forget this device" prompt, limit UI | M2/M3 | not started |
| 11 | `APP/peripheral.c` | proactive `GAPBondMgr_PeriSecurityReq` 4 ticks after connect + stop the timer on disconnect (removes the app's 0.5 s first-connect dead window, §7.1) | M1/M2 | implemented |
| 12 | `APP/peripheral.c`, `APP/usbd_compostie_km.c`, `peripheral.h` | HID reports (0x02/0x03/0x04/0x05) arriving over BLE send no ACK (`BLE_CH9329_RESP_NONE`, §7.2) | M4 | implemented |
| 13 | `build/makefile`, `APP/peripheral.c` | connection interval 12.5→15 ms, `BLE_TX_NUM_EVENT=4`, `BLE_BUFF_NUM=8`, 8 KB heap (§7.2) | M4 | implemented |

Implementation note: a new file must be registered in `build/APP/subdir.mk` (three lists: `C_SRCS`, `C_DEPS`, `OBJS`); that file is a static manifest generated by MounRiver, not a glob. The `bluetooth` record page `0x0806A000` sits inside the 440 KB code region, the same risk as the ECM IP config page: the image must stay under 426 KB (currently ~220 KB).

**M1 — close the door** (1-4): unusable while unpaired, though a bond can still be freeloaded with 666666. Small change, independently verifiable on the bench. **Risk**: older app versions fail to connect directly, so it must ship in step with the app; the USB path remains as fallback.
**M2 — real PIN + single binding** (5-8): the security model holds, initial PIN 666666, changeable.
**M3 — experience** (9, 10): button reset implemented (§11), limit UI, app pairing guide.

One-sentence priority: the load-bearing part is the two layers in §7; three lines of code demote "any passer-by" to "someone who has the initial PIN". If you do only one thing, do M1.

## 13. Test matrix

Bench: KeyMod + 2 iOS + 2 Android + USB serial (sudo). Each case maps to §7/§5/§6/§8/§9.

| # | Case | Expected |
|---|---|---|
| T1 | unencrypted write to FFF2 / write to CCCD | ATT error **0x0F** INSUFFICIENT_ENCRYPT; the `linkDB_State` fallback also triggers |
| T2 | pair with 666666 | succeeds, bond persisted, LED state 5, `0x22 0x03` returns count=1, max=1 |
| T3 | pair with a wrong PIN | fails and disconnects, count unchanged, LED state 6, retry possible |
| T4 | restart after pairing | the bound phone reconnects with encryption and no prompt (verifies P1b; must also be checked with the limit lowered to 1 while full) |
| T5 | second phone pairs while occupied | **fails** (`NO_PAIRING` takes effect) |
| T6 | app unbind → system "forget device" → new phone pairs with 666666 | no USB needed at any point (§6.1 main path holds) |
| T7 | USB `0x22 0x02` | bonds cleared, PIN override cleared, limit back to 1, 666666 usable |
| T8 | `0x22 0x05` sent over BLE | rejected; succeeds over USB |
| T9 | `0x22 0x05` clamping | write 0 → 1; write 255 → 4; retained across power cycle |
| T10 | limit 4, three devices bound, then lowered to 1 | all three existing devices still reconnect; new pairing refused (§5.3) |
| T11 | `0x22 0x04` (6 B PIN) vs a BLE-Eth ECM frame | disambiguated correctly (PIN frame has `buf[4]=7`, not matching 1/9/13/21), does not accidentally trigger the ECM defaults reset |
| T12 | corrupt flash record (write garbage to `0x0806A000`) | falls back to defaults, self-heals on the next write, no hard fault |
| T13 | passkey entry on both iOS and Android | dialog appears (verifies P3: stack behaviour with passcodeCB=NULL) |
| T14 | regression | the BLE-Eth tunnel, all CH9329 commands, `0x19` and renaming behave unchanged on an encrypted link |
| T15 | (only needed if the user raises the limit to 4) pair 6 devices in a row with `ERASE_AUTO=0` | no silent wipe; the stack's real capacity is predictable (P2) |
| T16 | change the PIN to 123456 with `0x22 0x04`, then restart | retained across power loss; existing bond unaffected, new pairing requires 123456 |
| T17 | hold the BOOT key 5 s then release | LED flashes rapidly at the 5th second; reboots on release, `0x22 0x03` shows count=0/max=1; the old phone can pair again with 666666 after "forget device" |
| T18 | hold the BOOT key 3 s then release | nothing happens: no flash, no reboot (early release cancels) |
| T19 | bound phone reconnects and immediately drags the mouse | the first FFF1 subscribe succeeds (no 0.5 s retry); the pointer tracks from the first frame; on a first pairing the PIN dialog pops at the moment of connection |
| T20 | drag continuously for 10 s (iOS, ProMotion device) | lag does not accumulate; pointer-to-finger distance stays within 150 ms; no more 7 B ACKs flooding FFF1 at the movement rate |
| T21 | send 0x01/0x22 0x03/0x23/0x26 queries over BLE | answers as before (security panel, renaming, ECM reads all work); confirms B only suppresses HID ACKs |
| T22 | residual lag even after B | record the negotiated connInterval (temporarily print `peripheralConnList.connInterval` on the peripheral); if it is still 30 ms, move to app-side coalescing/flow control |

## 14. Premises to verify

If P1 or P1b does not hold, M1's mechanism must be redone — this should be verified on the bench as step 0, not discovered after the code is written.

| ID | Premise | If it does not hold | When to verify |
|---|---|---|---|
| P1 | the ROM ATT layer really checks the encryption bit of the attribute permission | ① is a no-op, everything rests on ② plus an explicit reject returning an error code | first thing in M1, T1 |
| P1b | `GAPBOND_PAIRING_MODE_NO_PAIRING` does not block the encryption command on an already-bound link | old phones cannot connect when full → downgrade to "when full rely on the advertising field + app refusal, firmware keeps WAIT_FOR_REQ" | M2, T4/T5/T19 (promoted to must-test once §7.1 requests encryption immediately on connect) |
| P2 | ROM bond capacity ≥ the user-configurable limit | clamp the limit down to the measured value | only when someone raises it above 1, T15 |
| P3 | with `DISPLAY_ONLY` + preset passcode + passcodeCB=NULL the phone still shows the PIN dialog | register passcodeCB for display only | first day of M2, T13 |
| P4 | the key distribution list includes the required keys (default `0x77`, `GAPBOND_PERI_KEY_DIST_LIST=0x406`) sufficient for reconnect encryption | adjust the distribution mask | M2, T4 |
| T-HW | ~~is there a key on the KeyMod board that can be sampled via GPIO~~ | **resolved (2026-11-10)**: the key can be sampled, wired to PB8; implement the 5 s long press as a floating input (§11) | closed |

## 15. Risks and known gaps

| Risk | Handling |
|---|---|
| Older apps break against the new firmware | ship M1 in step with the app; the USB path stays permanently available |
| A PIN holder can evict the incumbent (§6.2) | accepted in v1, treat the PIN as a seal; v2 adds a per-phone ownership key at the app layer |
| A lost phone cannot be revoked individually | with single binding, that means the §6.1/§11 reset: erase everything, which is revocation with no residue |
| iOS keeps a stale bond record at the system layer | the app's unbind guide must include the "forget this device" step (T6) |
| `0x22` keeps sharing bytes with BLE-Eth commands | every new sub-command discriminates by length shape, guarded by T11; long term the security commands deserve their own command byte |
| BLE throughput ceiling (MTU 60, about 36.5 kbit/s) | unrelated to this design, but PIN validation and command round trips share that link, so do not stack another handshake layer on it |
