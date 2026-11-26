# macOS / Linux Build Notes

## Current Validated Workflow (2026-11-17)

Firmware can now be **built and flashed entirely on Linux** using the MounRiver Studio (MRS) Linux toolchain. No Windows machine is required.

### Quick Start

```bash
# 1. Install the MRS Linux toolchain
mkdir -p ~/tools
tar -xf ~/Downloads/MRS_Toolchain_Linux_X64_V300.tar.xz -C ~/tools

# 2. Add the toolchain to PATH
export PATH="$HOME/tools/MRS-linux-x64/resources/app/resources/linux/components/WCH/Toolchain/RISC-V Embedded GCC/bin:$PATH"

# 3. Verify
riscv-none-embed-gcc --version
# riscv-none-embed-gcc (xPack GNU RISC-V Embedded GCC, 64-bit) 8.2.0

# 4. Build
cd build
make clean
make PRODUCT=keymod-plus

# 5. Generate binary
riscv-none-embed-objcopy -O binary openterface-km-firmware.elf openterface-km-firmware.bin

# 6. Flash (see build-flash-cycle.md for ISP mode entry)
~/.cargo/bin/wchisp flash openterface-km-firmware.bin
```

### Toolchain Details

| Property | Value |
|----------|-------|
| Package | `MRS_Toolchain_Linux_X64_V300.tar.xz` |
| Install path | `~/tools/MRS-linux-x64/` |
| GCC path | `~/tools/MRS-linux-x64/resources/app/resources/linux/components/WCH/Toolchain/RISC-V Embedded GCC/bin/` |
| Compiler | `riscv-none-embed-gcc` (xPack GNU RISC-V Embedded GCC, 64-bit) 8.2.0 |
| Supported arch | `rv32imacxw` (WCH custom `xw` extension) |

### Build Products

```bash
make PRODUCT=kvmgo          # KVMGo (board v1)
make PRODUCT=minikvm        # Mini-KVM v1.2 (board v2)
make PRODUCT=keymod-plus    # KeyMod Plus — external 32KHz oscillator (board v3)
make PRODUCT=keymod-plus-beta  # KeyMod Plus Beta — internal RC oscillator, same USB features as Plus (board v3)
make PRODUCT=keymod-mini    # KeyMod Mini — internal RC oscillator, no CDC-ACM (board v3)
```

## Toolchain Notes

- The MRS Linux toolchain is the **same xPack GCC 8.2.0** bundled with MounRiver Studio on Windows. It supports the WCH custom `xw` RISC-V extension required by the CH32V208.
- The CI uses `openwch/risc-none-embed-gcc` 8.2.0-3.1.1 (same GCC 8.2.0 base).
- GCC 12 and newer can build an image that **links, flashes, verifies and is completely dead**. Read the next section before using one.

### Building with GCC 12+ (Apple Silicon, Homebrew, plain xPack)

Two things break, and neither is reported as an error.

**1. `interrupt("WCH-Interrupt-fast")` is silently downgraded.** Upstream GCC does not know the
WCH string. It prints

```
warning: argument to 'interrupt' attribute is not '"user"', '"supervisor"', or '"machine"' [-Wattributes]
```

and then compiles the handler as an ordinary function: no register save, and it returns with
`ret` instead of `mret`. The first interrupt (SysTick, about 1 ms after reset) therefore jumps
into garbage. The device never enumerates — no NIC, no CDC — and the symptom looks like a
descriptor bug or a power problem, which is exactly how it was misdiagnosed.

`APP/include/wch_irq.h` fixes this: it expands to the WCH fast form under GCC < 12 and to the
standard `interrupt` form otherwise. Use `WCH_IRQ_FAST` for any new handler; do not write the
attribute literal.

**2. `.bss` grows by roughly 20 KB.** GCC 15 gives each inlined copy of a block-scope `static`
array its own storage, so a `mode 8` (NCM) + BLE image used to overflow the 64 KB RAM. It links
now, at 58 956 B of RAM (89.96%) — the endpoint-buffer fix in
[`usb-endpoint-map-mode8.md`](usb-endpoint-map-mode8.md#23-buffer-layout) reclaimed 3968 B,
which is what the 20 KB inflation was eating:

```bash
# links locally on 2026-11-27 (GCC 15.2, Apple Silicon); runtime not re-verified from this
# toolchain with BLE on — flash only for bring-up
./build.sh --make-arg USB_WORK_MODE=hid_ncm_bridge \
           --make-arg USB_WORK_MODE_RUNTIME_PROFILES=0 keymod-plus
```

Two flags matter:

| Flag | Why |
|---|---|
| `USB_WORK_MODE_RUNTIME_PROFILES=0` | with `1` (the default) a stale mode record in flash wins over the compiled mode: the image enumerates but exposes the wrong function, e.g. no NIC |
| `--no-ble` | drops ~14 KB and leaves a lot more slack; use it when you only need to test the USB function, not the radio |

Images built this way are for bring-up only. Ship builds must still come from the 8.2 toolchain:
it is the only one that produces the fast-interrupt code the firmware was written for, and the
only one whose RAM figures are meaningful.

## Flash Tool

Install `wchisp` for flashing:

```bash
cargo install wchisp --git https://github.com/ch32-rs/wchisp
```

On Linux, install udev rules so `wchisp` can access the ISP USB device without root:

```bash
sudo tee /etc/udev/rules.d/50-wchisp.rules << ‘EOF’
SUBSYSTEM=="usb", ATTR{idVendor}=="1a86", ATTR{idProduct}=="55e0", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="4348", ATTR{idProduct}=="55e0", MODE="0666"
EOF
sudo udevadm control --reload-rules && sudo udevadm trigger
```

### Flashing with the Dev Kit control board on macOS

`tools/ctrl_board.py` drives the control board, but it looks the port up with
`udevadm`, confirms ISP/run state with `lsusb`, and calls a hardcoded
`/home/bbot/.cargo/bin/wchisp`. None of those exist on macOS, so it fails before
sending its first frame. `wchisp` itself is fine here — there are no udev rules to
install — so only the BOOT/power sequencing has to be reproduced by hand.

The control board accepts the same 7-byte frames over its own USB CDC port
(`/dev/cu.usbmodem*`, 460800 baud):

| Step | Frame | Meaning |
|---|---|---|
| power off | `57 AB 00 1A 01 00 1D` | PWR low |
| BOOT high | `57 AB 00 19 01 01 1D` | BOOT pin high (ISP) |
| power on | `57 AB 00 1A 01 01 1E` | PWR high |
| BOOT low | `57 AB 00 19 01 00 1C` | release BOOT (run mode) |

A full cycle is: power off, BOOT high, power on, wait ~3 s, `wchisp flash <bin>`,
then BOOT low, power off, power on, wait ~5 s. Each frame is a plain seven-byte
write:

```python
import serial, time
with serial.Serial("/dev/cu.usbmodem211101", 460800, timeout=1) as s:
    s.reset_input_buffer()
    s.write(bytes.fromhex("57AB001A01001D"))   # power off
    time.sleep(0.4)
```

There is no `lsusb` to confirm the state, so use `system_profiler SPUSBDataType`
instead: in ISP mode the CH32V208 shows up as `1a86:55e0` (or `4348:55e0`), and
after the final power-on the running image appears as USB product
"Openterface Host". Flashing erases the BLE bond — see
[ble-eth-ssh-usage.md](ble-eth-ssh-usage.md#common-failures) before reconnecting
over BLE.

## Troubleshooting

- `riscv-none-embed-gcc` not found: ensure the MRS toolchain `bin/` directory is on your `PATH`.
- `wchisp` timeout: check that the device is in ISP mode (VID 0x1A86 or 0x4348, PID 0x55E0) and udev rules are installed.
- Firmware doesn’t boot after flash: verify you released the BOOT pin and power-cycled the target after flashing. See [build-flash-cycle.md](build-flash-cycle.md).
- `RAM: 64 KB / 64 KB 100.00%` in the build report: this is a reporting artifact, not an overflow. `.stack` is placed at `ORIGIN + LENGTH - 2048`, so the linker counts the whole end of the region as used. Read the gap instead — with mode 8 + BLE on GCC 15, `.bss` ends at `0x2000DE50` and `.stack` starts at `0x2000F800`, i.e. 6576 B free. A real overflow fails the link.
- Local size figures do not line up with CI: the same `PRODUCT=kvmgo USB_WORK_MODE=hid_ncm_bridge` configuration is 58 956 B (89.96%) under GCC 15 and 58 440 B (89.17%) under the CI 8.2 toolchain. Compare against the same toolchain only.
- `wchisp` reports a size larger than the `.bin` (e.g. 232 448 for a 231 568 B image): it pads the image to the flash block size before writing. Not an error.
