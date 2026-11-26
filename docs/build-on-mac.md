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
array its own storage, so `mode 8` (NCM) with BLE no longer fits in the 64 KB RAM, and even the
no-BLE image lands within ~100 bytes of the stack:

```bash
# verified locally on 2026-11-26 (GCC 15.2, Apple Silicon), serial + NIC + ping OK
./build.sh --no-ble --make-arg USB_WORK_MODE=hid_ncm_bridge \
           --make-arg USB_WORK_MODE_RUNTIME_PROFILES=0 keymod-plus
```

Two flags matter:

| Flag | Why |
|---|---|
| `--no-ble` | BLE costs ~14 KB of RAM; without it mode 8 fits under GCC 15 |
| `USB_WORK_MODE_RUNTIME_PROFILES=0` | with `1` (the default) a stale mode record in flash wins over the compiled mode: the image enumerates but exposes the wrong function, e.g. no NIC |

Images built this way are for bring-up only. Ship builds must come from the 8.2 toolchain:
it is the only one that produces the fast-interrupt code the firmware was written for, and the
only one where the full BLE + NCM image fits.

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

## Troubleshooting

- `riscv-none-embed-gcc` not found: ensure the MRS toolchain `bin/` directory is on your `PATH`.
- `wchisp` timeout: check that the device is in ISP mode (VID 0x1A86 or 0x4348, PID 0x55E0) and udev rules are installed.
- Firmware doesn’t boot after flash: verify you released the BOOT pin and power-cycled the target after flashing. See [build-flash-cycle.md](build-flash-cycle.md).
