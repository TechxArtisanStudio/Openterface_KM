# Build and Flash Guide

This guide explains how to build and flash Openterface KM firmware on macOS/Linux.

## Table of Contents

- [Prerequisites](#prerequisites)
- [Building Firmware](#building-firmware)
- [Flashing Firmware](#flashing-firmware)
- [Troubleshooting](#troubleshooting)

## Prerequisites

### 1. WCH RISC-V Toolchain

The firmware uses WCH's custom RISC-V extension `xw`, which requires the WCH-customized GCC toolchain.

**Install WCH Toolchain:**

```bash
# Create installation directory
mkdir -p ~/opt
cd ~/opt

# Download WCH toolchain (macOS)
curl -L -o wch-toolchain.tar.bz2 \
  "https://github.com/Community-PIO-CH32V/toolchain-riscv-mac/releases/download/8.2.0/toolchain-riscv-mac.tar.bz2"

# Extract
tar -xjf wch-toolchain.tar.bz2
mv toolchain-riscv-mac wch-toolchain

# Add to PATH (add to ~/.zshrc or ~/.bashrc)
export PATH="$HOME/opt/wch-toolchain/bin:$PATH"
```

**Verify installation:**

```bash
riscv-none-embed-gcc --version
# Should show: riscv-none-embed-gcc (GCC) 8.2.0
```

### 2. Flash Tool (choose one)

#### Option A: wchisp (Recommended)

Rust-based WCH ISP tool, auto-detects chip type, no configuration needed.

```bash
# Install via Homebrew
brew install wchisp

# Or install via cargo
cargo install wchisp --git https://github.com/ch32-rs/wchisp

# Verify
wchisp --version
```

**Install location:** `~/.local/bin/wchisp` or `~/.cargo/bin/wchisp`

#### Option B: WCHISPTool_CMD (Official)

Official WCH command-line ISP tool (V3.70). Requires configuration file.

**Download:**
- Visit [WCH official site](http://www.wch.cn/downloads/WCHISPTool_download.html)
- Download macOS version
- Extract to `~/project/WCHISPTool_CMD/`

**Remove quarantine (macOS):**

```bash
xattr -cr ~/project/WCHISPTool_CMD/macOS/bin/
```

**Add to PATH:**

```bash
export PATH="$HOME/project/WCHISPTool_CMD/macOS/bin:$PATH"
```

### 3. Environment Setup

Add these to your `~/.zshrc` or `~/.bashrc`:

```bash
# WCH RISC-V Toolchain
export PATH="$HOME/opt/wch-toolchain/bin:$PATH"

# WCH ISP Tool (if using WCHISPTool_CMD)
export PATH="$HOME/project/WCHISPTool_CMD/macOS/bin:$PATH"

# wchisp (if installed to ~/.local/bin)
export PATH="$HOME/.local/bin:$PATH"
```

Reload your shell:

```bash
source ~/.zshrc
```

## Building Firmware

### Quick Build

```bash
cd ~/project/Openterface_KM
./build.sh kvmgo
```

### Build Targets

| Target | Description | BOARD_VERSION |
|--------|-------------|---------------|
| `kvmgo` | KVMGo device | 1 |
| `minikvm` | Mini-KVM v1.2 | 2 |
| `keymod-plus` | KeyMod Plus (external 32KHz oscillator) | 3 |
| `keymod-mini` | KeyMod Mini (internal RC oscillator) | 3 |

**Examples:**

```bash
# Build KVMGo firmware
./build.sh kvmgo

# Build Mini-KVM firmware
./build.sh minikvm

# Build KeyMod Plus firmware
./build.sh keymod-plus

# Build all targets
./build.sh all
```

### Build Options

```bash
# Disable BLE (saves ~14KB RAM)
./build.sh --no-ble kvmgo

# Specify custom compiler path
./build.sh --compiler /path/to/compiler/bin kvmgo
```

### Build Output

Firmware files are generated in the `output/` directory:

```
output/
├── openterface-km-firmware-kvmgo.bin  # Raw binary (for wchisp)
├── openterface-km-firmware-kvmgo.hex  # Intel HEX (for WCHISPTool_CMD)
├── openterface-km-firmware-kvmgo.elf  # ELF with debug symbols
└── openterface-km-firmware-kvmgo.map  # Memory map
```

### Advanced Build Flags

For advanced builds, use `make` directly in the `build/` directory:

```bash
cd build

# Build with custom BLE name
make all BOARD_VERSION=1 BLE_NAME="My-KVM" ENABLE_BLE=1

# Build without BLE
make all ENABLE_BLE=0

# Build with external oscillator
make all ENABLE_OSCILLATOR=1
```

**Key build flags:**

| Flag | Default | Description |
|------|---------|-------------|
| `BOARD_VERSION` | 1 | Board version (1=KVMGo, 2=MiniKVM, 3=KeyMod) |
| `ENABLE_BLE` | 1 | Enable Bluetooth (0=disable) |
| `ENABLE_UAC` | 1 | Enable USB Audio Class (0=disable) |
| `ENABLE_OSCILLATOR` | 0 | External 32KHz crystal (1=enable) |
| `BLE_NAME` | KVM-GO | Bluetooth device name |

## Flashing Firmware

### Enter BOOT Mode

Before flashing, the device must be in BOOT/ISP mode:

1. **Method 1**: Hold BOOT button while plugging in USB
2. **Method 2**: Pull BOOT pin high during reset

**Verify BOOT mode in macOS:**

```bash
system_profiler SPUSBDataType | grep -A 10 -i "55e0"
```

You should see:
- **Vendor ID**: 0x4348 (or 0x1A86)
- **Product ID**: 0x55E0

### Quick Flash

```bash
cd ~/project/Openterface_KM
./tools/flash.sh kvmgo
```

The script will:
1. Detect available flash tool (wchisp or WCHISPTool_CMD)
2. Wait for device in BOOT mode (max 15 seconds)
3. Flash firmware from `output/` directory
4. Reset device after flashing

### Dev Kit Control Board Flash (Linux)

For the Openterface Dev Kit (which includes a control board that manages BOOT/power pins), use the `ctrl_board.py` tool for fully automated flashing:

```bash
# From project root
cd /home/bbot/projects/Openterface/Openterface_KM

# Full flash cycle (build + flash + verify)
python3 tools/ctrl_board.py flash build/openterface-km-firmware-keymod-plus-beta.bin

# Step-by-step
python3 tools/ctrl_board.py isp on      # Enter ISP mode (BOOT HIGH + power cycle)
~/.cargo/bin/wchisp flash firmware.bin  # Flash
python3 tools/ctrl_board.py isp off     # Exit ISP mode (BOOT LOW + power cycle)
python3 tools/ctrl_board.py verify      # Verify via USB
python3 tools/verify_firmware.py        # Verify via CMD_GET_INFO
```

The control board handles all BOOT pin and power sequencing automatically via USB serial commands (460800 baud).

This helper is Linux-only: it discovers the port with `udevadm`, checks ISP/run state with `lsusb`, and calls `/home/bbot/.cargo/bin/wchisp`. On macOS, see [build-on-mac.md](build-on-mac.md#flashing-with-the-dev-kit-control-board-on-macos) for the same sequence driven by hand.

### Flash Options

```bash
# Force use wchisp
./tools/flash.sh --tool wchisp kvmgo

# Force use WCHISPTool_CMD
./tools/flash.sh --tool wchisp_cmd kvmgo

# Flash custom firmware file
./tools/flash.sh --firmware /path/to/firmware.bin

# Don't reset device after flash
./tools/flash.sh --no-reset kvmgo

# Dry run (print commands without executing)
./tools/flash.sh --dry-run kvmgo
```

### Manual Flash with wchisp

```bash
# Probe for device
wchisp probe

# Flash firmware
wchisp flash output/openterface-km-firmware-kvmgo.bin

# Get chip info
wchisp info
```

### Manual Flash with WCHISPTool_CMD

```bash
# Flash with config file
WCHISPTool_CMD \
  -c tools/flash_clean.ini \
  -o program \
  -r \
  -f output/openterface-km-firmware-kvmgo.hex
```

**WCHISPTool_CMD options:**
- `-c <config>`: Configuration file (e.g., `tools/flash_clean.ini`)
- `-o program`: Operation type
- `-r`: Remove code protection before flashing
- `-f <file>`: Firmware file (.hex format)

**Common error codes:**
- **Code 6**: Device not found or chip type mismatch in config
- **Code 12**: Download failed (try `-r` flag to remove code protection first)

## Troubleshooting

### Build Issues

**Compiler not found:**

```bash
which riscv-none-embed-gcc
# If not found, check PATH includes WCH toolchain
echo $PATH | grep wch-toolchain
```

**Error: `-march=rv32imacxw: unsupported ISA substring 'xw'`:**

You're using the wrong toolchain. Standard xPack GCC doesn't support WCH's `xw` extension. Install the WCH-customized toolchain as described in [Prerequisites](#prerequisites).

**Stale build artifacts:**

```bash
cd build
make clean
cd ..
./build.sh kvmgo
```

### Flash Issues

**Device not detected:**

1. Check USB connection:
   ```bash
   system_profiler SPUSBDataType | grep -A 10 -i "wch\|4348\|55e0"
   ```

2. Ensure device is in BOOT mode:
   - Hold BOOT button while plugging in
   - Or pull BOOT pin high during reset

3. Try different USB port or cable

**wchisp: "No WCH ISP USB device found":**

Device is not in BOOT mode or USB driver issue. Try:
```bash
# Probe again
wchisp probe

# Check permissions (macOS may require sudo for first run)
sudo wchisp flash output/openterface-km-firmware-kvmgo.bin
```

**WCHISPTool_CMD: Code 6 "Fail to find any valid isp device":**

Possible causes:
- Device not in BOOT mode
- Chip type mismatch in config file

**Fix:** Edit `tools/flash_clean.ini` and update `MCUName` to match your actual chip:
```ini
[Public]
MCUName=CH32V208GBU  # Change to your chip model
```

**WCHISPTool_CMD: Code 12 "Fail to download":**

Code protection is enabled. Remove it first:
```bash
WCHISPTool_CMD -c tools/flash_clean.ini -r
```

Then flash again.

**Device exits BOOT mode after each flash attempt:**

This is normal behavior. The device resets after ISP operations. Re-enter BOOT mode before each attempt.

### Verification

After successful flash, the device should enumerate as a USB HID device:

```bash
# Check for Openterface device
system_profiler SPUSBDataType | grep -i openterface

# Or check all USB devices
ioreg -p IOUSB -l | grep -i "openterface\|kvm"
```

The RGB LED should show the startup sequence animation, indicating the firmware is running.

## Complete Workflow Example

```bash
# 1. Setup environment (one-time)
export PATH="$HOME/opt/wch-toolchain/bin:$PATH"
export PATH="$HOME/.local/bin:$PATH"

# 2. Build firmware
cd ~/project/Openterface_KM
./build.sh kvmgo

# 3. Enter BOOT mode on device (hold BOOT button + plug in USB)

# 4. Verify device is in BOOT mode
system_profiler SPUSBDataType | grep -A 5 -i "55e0"

# 5. Flash firmware
./tools/flash.sh kvmgo

# 6. Verify device is running
system_profiler SPUSBDataType | grep -i openterface
```

## Additional Resources

- [WCH CH32V20x Datasheet](http://www.wch.cn/downloads/CH32V208_Datasheet.html)
- [wchisp GitHub](https://github.com/ch32-rs/wchisp)
- [Community PIO CH32V](https://github.com/Community-PIO-CH32V)
- [Openterface KM Documentation](../README.md)
