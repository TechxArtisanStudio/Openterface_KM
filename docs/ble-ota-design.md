# KeyMod / KVM-GO BLE OTA update design

- Date: 2026-11-11
- Baseline: `feature/ble-pairing-security` @ `bf30ece`
- Applies to: `ENABLE_BLE` boards such as KeyMod (`BOARD_VERSION=3`) and KVM-GO (`BOARD_VERSION=1`)
- Status: **design stage, not implemented**. Milestones in §12, all not started
- Depends on: the encryption admission from the BLE pairing security design ([ble-pairing-design.md](ble-pairing-design.md)) landing first; OTA should start on a new branch `feature/ble-ota` after it
- Decision history: 2026-11-11 completed the feasibility check (measured flash/RAM/library size), rejected both "BLE stack in the bootloader" and "migrate to the ROM library", and chose **app-side reception + compressed staging + a minimal BLE-free bootloader**

---

## 1. Conclusion

**It can be done.** Recommended route: while the firmware runs normally, receive a compressed image over BLE into a staging area; after a reboot a minimal bootloader **with no BLE stack** verifies, decompresses, overwrites the app region and jumps to it. Every stage is power-loss tolerant, with USB ISP as the fallback recovery path.

No BLE library migration, no RAM layout change, no BLE in the bootloader. The cost is introducing compression (mandatory, see the space arithmetic in §3) and a new boot project.

## 2. Current facts (each verified)

| Item | Value | Evidence |
|---|---|---|
| Total flash | **480K** = 448K code region (0x00000–0x6FFFF) + 32K data region (0x70000–0x77FFF) | flash driver `ValidAddrEnd = FLASH_BASE + 0x78000`, [ch32v20x_flash.c:66](file:///Users/txa/project/Openterface_KM/SRC/Peripheral/src/ch32v20x_flash.c#L66) |
| Current partition | 440K app + 3×4K config pages (BLENAMECFG 0x6D000 / DEFTCFG 0x6E000 / USBMODECFG 0x6F000) | [HAL/Link.ld:29-33](file:///Users/txa/project/Openterface_KM/HAL/Link.ld#L29-L33) |
| Current firmware | 219,292 B ≈ **214.2K** (built 2026-11-10) | `build/openterface-km-firmware.bin` |
| Of which the BLE stack | **198.1K** (202,812 B of .text from `libwchble.a`, the flash variant linked into the app image) | summed section by section from the map file |
| Application's own code | ≈ 16K | 214.2K − 198.1K |
| Bond storage (SNV) | data region **0x08077C00**, 3×256B blocks, `BLE_SNV=TRUE` | [config.h:103](file:///Users/txa/project/Openterface_KM/HAL/include/config.h#L103), [MCU.c:110](file:///Users/txa/project/Openterface_KM/HAL/MCU.c#L110) |
| RAM | data+bss 50,924 B of 64K, free heap gap ≈ 14.6K | [internal-memory-usage.md](internal-memory-usage.md) |
| Flash write API | 4K page erase + 256B page fast program, SNV already uses it | `Lib_Write_Flash`, [MCU.c:64-72](file:///Users/txa/project/Openterface_KM/HAL/MCU.c#L64-L72) |
| Vector table relocation | startup writes `mtvec` (mode 3) from the linker symbol `_vector_base`, so **changing the link offset relocates it automatically** with no extra code | [startup_ch32v20x_D8W.S:266-269](file:///Users/txa/project/Openterface_KM/SRC/Startup/startup_ch32v20x_D8W.S#L266-L269) |
| BLE throughput ceiling | MTU 60, 1 packet per connection event ≈ 36.5 kbit/s ≈ 4.5 KB/s | measured on the bench (see the memory/tunnel documents) |
| DFU admission foundation | FFF2 already requires `GATT_PERMIT_ENCRYPT_WRITE` (a pairing-design result) | [ble_usb_service.c:138](file:///Users/txa/project/Openterface_KM/Profile/ble_usb_service.c#L138) |
| ROM BLE library | the repo ships `wchble_rom.hex`, occupying 0x40000–0x6CFFF (187.3K including config-page initial values) + **16K RAM** (0x20000000–0x20003FFF); the current build **does not use it** | [wchble_rom.h:1-15](file:///Users/txa/project/Openterface_KM/LIB/wchble_rom.h#L1-L15), measured from the hex addresses |

**Key inference: the BLE stack is 198K, so any "run BLE in the bootloader" route inflates the bootloader to ~200K** — that is what decides the route.

## 3. Route comparison

| Route | Description | Rejection / adoption rationale |
|---|---|---|
| **A. App reception + compressed staging + minimal boot (adopted)** | the app receives the image with the existing BLE stack → staging area; the bootloader has no BLE and only verifies/decompresses/copies/jumps | bootloader ≈ 6K; no change to the BLE library or RAM; every stage is power-loss recoverable; ~30 s transfer when compressed |
| B. Migrate to the ROM library + true A/B dual region | the stack is fixed at 0x40000, the app shrinks to ~20K, boot and app share the stack | cleanest long term, but: ① migration requires re-verifying all pairing/SNV behaviour; ② the ROM library reserves 16K RAM, leaving 48K for the app while it currently uses 50.9K — **not enough RAM**, so `MEM_BUF` etc. would have to be cut; ③ far more work and risk than A. Keep as a long-term evolution |
| C. BLE stack in the bootloader writing the app region directly | the bootloader links a second copy of `libwchble.a` and overwrites the app once the image arrives | bootloader ≈ 206K + app 214K + config 12K = 432K, leaving only ~16K of app growth headroom; no staging and no rollback, and the device is unusable during an interrupted transfer; the bootloader is 30× the size of A's. Rejected |

Space arithmetic for A (why compression is mandatory): uncompressed needs app 214K + staging 214K + boot 8K + config 12K = 448K, which **exactly fills the region with zero headroom**; compressed, staging needs only ~130K and still leaves a 40K spare area.

## 4. Flash partition (route A)

The 448K code region is redivided; the config pages and the data region are **untouched**:

| Region | Address | Size | Purpose |
|---|---|---|---|
| `BOOT` | 0x00000–0x01FFF | 8K | bootloader, no BLE, never written at runtime |
| `APP` | 0x02000–0x39FFF | 224K | application image (currently 214.2K, ~10K headroom) |
| `STAGING` | 0x3A000–0x61FFF | 160K | compressed image staging |
| `META` | 0x62000–0x62FFF | 4K | update state log (§8) |
| `SPARE` | 0x63000–0x6CFFF | 40K | spare, can later be given to APP or STAGING |
| `BLENAMECFG` | 0x6D000 | 4K | unchanged |
| `DEFTCFG` | 0x6E000 | 4K | unchanged |
| `USBMODECFG` | 0x6F000 | 4K | unchanged |
| data region | 0x70000–0x77FFF | 32K | SNV bond @0x77C00, **no OTA path may touch it** |

Constraints:
- Compressed image ≤ 160K (enforced by the packaging tool). heatshrink typically achieves 50–65% on RISC-V firmware, so 214K → 110–140K, leaving headroom; M0 confirms by measurement.
- 10K of app headroom is tight, a known trade-off; when needed, give SPARE to APP (two linker-script constants, boot logic unchanged).
- 8K bootloader budget: startup + flash driver + heatshrink decoder (~1K) + CRC32 + meta logic + LED ≈ 5–6K. If M4 adds signature verification it needs 16K (borrowed from SPARE).

## 5. Image format and compression

### 5.1 OTA package (the .ota file)

```
[ 64B header ][ heatshrink compressed data ][ optional 64B Ed25519 signature (M4) ]
```

Header (little-endian):

| Offset | Field | Notes |
|---|---|---|
| 0 | magic `KMOT` | 4B |
| 4 | hdr_ver | 1B, currently 1 |
| 5 | hw_id | 1B, `BOARD_VERSION` (KeyMod=3 / KVM-GO=1), mismatch is rejected |
| 6 | flags | 2B, bit0 = signature present |
| 8 | fw_version | u32, **monotonically increasing**, the basis for rollback prevention |
| 12 | raw_size | u32, decompressed size (= app image length) |
| 16 | comp_size | u32, compressed data length |
| 20 | raw_crc32 | u32, CRC32 over the decompressed image |
| 24 | comp_crc32 | u32, CRC32 over the compressed data |
| 28 | hdr_crc32 | u32, CRC32 over the first 28 header bytes |
| 32–63 | reserved | zero |

The app image needs no special trailer: all verification CRCs live in the OTA header and META, so a raw bin flashed over USB is unaffected (in §7 the bootloader passes straight through a factory state with no META record).

### 5.2 Compression algorithm

Choose **heatshrink** (an LZ77 variant aimed at embedded): the decoder is ~1K of code with a configurable window and lookahead (using a 2^10 window + 2^4 lookahead, ~1.3K RAM), and encoding runs only in the host tool. Alternative LZ4 (faster decode, slightly worse ratio, more RAM); M0 measures both on the current bin and then decides. zlib is out: decoder size and RAM are an order of magnitude larger.

## 6. App-side changes

1. **Link offset**: the app linker script becomes `FLASH ORIGIN=0x02000 LENGTH=224K`, RAM layout unchanged. The vector table needs no manual relocation — startup already writes `mtvec` from a linker symbol (verified in §2).
2. **DFU GATT characteristics** (under the existing ble_usb service, reusing the pairing design's encrypted admission):
   - `FFF3` DFU_CTRL: write + notify, permissions `GATT_PERMIT_READ | GATT_PERMIT_WRITE | GATT_PERMIT_ENCRYPT_WRITE` (same as FFF2)
   - `FFF4` DFU_DATA: write without response, permissions `GATT_PERMIT_ENCRYPT_WRITE`
   - No new service, no advertising change; an unpaired device cannot even write the characteristic, so admission sits at the protocol stack (the same principle as the pairing design).
3. **DFU mode**: after START, suspend HID/CH9329/ECM/ACM/UAC forwarding (the device does no KVM work while updating) and keep only the BLE connection and TMOS scheduling; notify progress every few pages. Flash programming briefly stalls the bus, and suspending the other paths avoids packet loss and connection-event timeouts.
4. **Writing the staging area**: call `FLASH_ProgramPage_Fast` per 256B page (the same API family as SNV, already verified), erasing in 4K pages. The write address is **hard-limited to the STAGING range** and rejected otherwise — this is the only barrier against accidentally writing the bootloader, config pages or SNV, and it is mandatory.
5. **RAM budget**: a 256B block buffer + 256B page buffer + state machine/header ≈ 1.5K, well within the 14.6K free heap gap.
6. **Version reporting**: the INFO command returns the existing firmware version (devinfo/devinfoservice wording) plus the OTA fw_version.

## 7. Bootloader design

No BLE, no USB, no TMOS; a bare-metal loop that does four things: read META → verify → (if needed) apply the image → jump.

```
power on
 ├─ read the latest valid META record
 ├─ state == PENDING?
 │    ├─ verify STAGING comp_crc32 ── fail → set FAIL, go to "no usable image"
 │    ├─ erase the APP region (4K/page)
 │    ├─ heatshrink-decompress STAGING → APP
 │    ├─ verify APP raw_crc32 ── fail → stay PENDING, retry on the next power-up
 │    └─ write META state=OK (with the new fw_version/raw_crc32)
 ├─ verify APP: if META has an OK record → verify against the recorded raw_crc32;
 │              if META is empty (factory USB-flashed state) → trust and pass through
 ├─ APP valid → disable interrupts, jump to the APP reset vector (mtvec is set again by the app's own startup)
 └─ APP invalid and no PENDING → LED error blink, stay in the bootloader, wait for USB ISP recovery
```

Design points:
- **The bootloader never updates itself**: neither the DFU write path nor the boot copy path writes 0x00000–0x01FFF. A corrupt bootloader can only be recovered over USB ISP, and the boot logic is simple enough to get right in one pass.
- In the PENDING state every power-up **re-verifies STAGING before doing anything**, never trusting the previous in-progress state, so an interrupted copy is safe any number of times.
- A full CRC32 over 214K takes about 10–20 ms at 144MHz (table-driven); it runs on every boot, so the startup delay is negligible.
- The apply stage (erase 224K + write 224K + decompress) is estimated at 2–5 s, to be measured in M0; the LED stays on or blinks slowly to signal "updating, do not remove power".
- The recovery path is exactly as today: `build/wchisp` USB ISP flashing. The device not answering BLE while in the bootloader is **deliberate** — the recovery channel is USB, not a second BLE bootloader.

## 8. META log (4K page)

A record is 32B, appended in order (flash can be reprogrammed repeatedly without erasing until the bits are exhausted); the page is erased and rewritten with the latest record only when full:

| Field | Notes |
|---|---|
| magic / seq | seq increases monotonically; the bootloader takes the record with the highest seq and a valid CRC |
| state | IDLE / PENDING / OK / FAIL |
| fw_version / comp_size / raw_size / raw_crc32 | carry the image information in PENDING/OK |
| rec_crc32 | checksum for this record |

- **Rollback prevention**: on START the app checks `new fw_version ≥ the latest OK fw_version in META`, and the bootloader checks again before applying. The version number only ever increases, guaranteed by the release process.
- No redundant double copy of the latest record is needed: append-only writing preserves history naturally, and a power loss at any moment loses at most the last unfinished record, so the bootloader falls back to the previous valid one.

## 9. GATT DFU protocol

CTRL (FFF3) uses 1B command words with responses/events over notify; DATA (FFF4) is a write-no-response _bulk_ channel, physically separate from the HID command channel (FFF2).

| Command | Payload | Behaviour |
|---|---|---|
| 0x01 INFO | — | notify: boot_ver, app_ver, hw_id, max_image(160K), META state |
| 0x02 START | 64B header | verify hw_id / rollback / comp_size≤160K / hdr_crc → erase STAGING → ACK or ERR |
| 0x03 DATA | FFF4: [seq u16][crc16][payload] | write in order; payload defaults to 244B (MTU 247), negotiated to ~55B at this device's MTU 60; on CRC error or out-of-order → ERR + notify the expected seq, and the host retransmits from that point |
| 0x04 VERIFY | — | full comp_crc32 → ACK/ERR |
| 0x05 APPLY | — | write META PENDING → notify OK → reset after a 100 ms delay |
| 0x06 ABORT | — | set META to IDLE, leave DFU mode, resume normal forwarding |

Notify events: progress (every 4K: bytes written / total) and error codes (HW_MISMATCH / ROLLBACK / TOO_LARGE / CRC / FLASH / STATE).

Transfer time estimate: at the measured ceiling of 4.5 KB/s, 130K compressed ≈ **30 s**; once MTU negotiates to 247 it can be several times faster (depending on the phone and connection parameters, outside what this design promises).

After the update the host reconnects and reads INFO to confirm fw_version changed, which is the end-to-end success criterion.

## 10. Security

| Layer | Measure |
|---|---|
| Admission | the DFU characteristics require a bonded, encrypted link (inherited from the pairing design, PIN as root of trust); an unpaired device cannot even send START |
| Integrity | CRC16 per block + full CRC32 over the compressed data + full CRC32 over the decompressed data, three levels |
| Rollback prevention | fw_version is monotonic, checked by both the app and the bootloader |
| Write protection | the DFU write path is hard-limited to an address range; the boot region is unwritable at runtime; SNV and the config pages are outside every OTA path |
| Signature (M4, optional) | Ed25519 (monocypher, ~5K of code), signed on the host and verified by the bootloader before applying; not in the baseline, because the threat model is mainly credential-less access by passers-by (already blocked by pairing) and accidental corruption (already blocked by CRC), while forging a targeted image requires physical access or a broken pairing — a residual risk, consistent with §2 of the pairing design |

## 11. Power-loss tolerance analysis

| Moment of power loss | State after power-up | Result |
|---|---|---|
| during download (any progress) | META=IDLE, APP intact | boots normally, host retransmits |
| after VERIFY, before writing PENDING | same as above | same as above |
| PENDING written, at any moment of erase/copy | bootloader sees PENDING, STAGING intact | applies again, safe any number of times |
| copy finished, before writing OK | bootloader verifies the APP CRC, passes → writes OK | normal |
| the new firmware itself has a bug that boots but misbehaves | no automatic rollback (baseline) | recover over USB ISP; optional rollback in M4 |
| boot region corrupted | no runtime path writes the bootloader | physically excluded; USB ISP as the fallback |

The only "device unusable" window is the 2–5 s apply stage (the bootloader copying); a power loss never bricks it, it only means starting over.

## 12. Milestones and checklists

### M0 Bench feasibility (before any code change)
- [ ] measure 4K erase + 256B fast program timing (total time to erase 224K / write 224K)
- [ ] compare compression ratio and decode speed of heatshrink vs LZ4 on the current 214K bin, then fix the algorithm
- [ ] minimal experiment: a hello firmware linked at 0x2000, bootloader jump to verify automatic mtvec relocation
- [ ] confirm SNV (0x77C00) and the config pages survive the experimental erase/write (read the bond record back)

### M1 Partition and bootloader
- [ ] change the app linker script to 0x2000/224K; create the boot project and linker script (8K)
- [ ] add a boot target and a boot+app combined image to `build.sh` (wchisp flashes the whole chip in one go)
- [ ] boot state machine (the no-PENDING path) + LED state definitions
- [ ] bench: boots normally after a USB flash, full regression of existing features (HID/ECM/pairing)

### M2 DFU end to end
- [ ] FFF3/FFF4 characteristics + encryption permissions + DFU mode (suspend forwarding)
- [ ] STAGING writes + hard address limits + META log
- [ ] `tools/ota_pack.py` (compress + header + size check), `tools/ota_upload.py` (bleak, connecting by full CoreBluetooth address to avoid keymod-test's name-substring false-match trap)
- [ ] bench: first successful BLE OTA, version confirmed through INFO

### M3 Robustness
- [ ] power-loss injection matrix: random cuts during download/erase/copy/META write, × 3 each
- [ ] rollback prevention, hw_id mismatch, oversize, ABORT, resume from a break point
- [ ] post-update integrity check of bond / config pages / USB mode
- [ ] record the measured transfer time and progress notifications

### M4 Optional hardening (not done by default, decide separately)
- [ ] Ed25519 signature (bootloader grows to 16K)
- [ ] rollback: compress the old app back into STAGING before applying, so a new firmware that fails to confirm can be reverted

## 13. How testing runs

Keep the bench division of labour: I provide the command batches and on-chip counter/read-back commands for each flash round, and the user performs the flashing and host-side/sudo operations. The M3 power-loss injection needs switchable power or manual USB unplugging coordinated with a countdown script; settle the method during M0.

## 14. Risks and open questions

1. **Only 10K of app headroom**: the firmware keeps growing (pairing, later the tunnel). The 40K SPARE is a buffer, but repartitioning changes boot constants, so earlier is better — at M1 APP could simply be set to 240K / STAGING 144K (requiring a ratio ≤67%; M0 data decides).
2. **Worst-case compression ratio**: if the measured result exceeds 160K the packaging tool refuses it; the remedy is trimming the app or enlarging STAGING, and there is no "cannot compress, therefore bricked" path.
3. **Flash programming stalls vs BLE connection events**: SNV writes have proven feasible, but DFU writes continuously and densely, so M2 must watch the disconnect rate; if needed, explicitly yield to TMOS between pages.
4. **The pairing branch is not merged**: DFU permissions depend on encryption admission, so the branch order cannot be reversed.
5. **ROM library migration (route B) is a long-term direction**: re-evaluate if the app grows sharply or true A/B is needed; the 48K RAM ceiling is then a hard constraint (requiring cuts such as `MEM_BUF` 10K → 8K).
6. **KeyCmd iOS/Android integration**: this design delivers the Python toolchain first; app integration follows §9 and is out of scope here.

## 15. Decision record

- 2026-11-11: measurement confirmed a 198K BLE stack, 480K flash (448K+32K), SNV at 0x77C00, the ROM library unused with 16K RAM reserved. On that basis routes B and C were rejected and route A adopted; compression became mandatory; signing and rollback were demoted to optional M4.
