# Openterface KM Build & Flash Scripts

This directory contains build and flash scripts for compiling and programming the Openterface KM firmware.

## Available Scripts

### Build Scripts

| Script | Environment | Description |
|--------|-------------|-------------|
| `build.ps1` | Windows PowerShell | **Recommended for Windows** |
| `build-win.sh` | Git Bash / MSYS2 / WSL | For command-line users |
| `build.sh` | Linux / macOS | Original script |

### Flash Scripts

| Script | Environment | Description |
|--------|-------------|-------------|
| `tools/flash.ps1` | Windows PowerShell | **Recommended for Windows** |
| `tools/flash-win.sh` | Git Bash / MSYS2 / WSL | For command-line users |
| `tools/flash.sh` | Linux / macOS | Original script |

### Combined Scripts

| Script | Environment | Description |
|--------|-------------|-------------|
| `build-and-flash.ps1` | Windows PowerShell | Build + Flash in one command |

## Prerequisites

### 1. Mount River Studio (Recommended)
If you have Mount River Studio installed, the scripts will automatically detect the toolchain at:
```
D:\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC\bin
```

### 2. Flash Tool (choose one)

#### Option A: wchisp (Recommended)
Rust-based WCH ISP tool, auto-detects chip type.

```powershell
# Install via cargo
cargo install wchisp --git https://github.com/ch32-rs/wchisp
```

#### Option B: WCHISPTool_CMD (Official)
Official WCH command-line ISP tool. Download from [WCH official site](http://www.wch.cn/downloads/WCHISPTool_download.html).

## Quick Start

### Build Firmware

**PowerShell:**
```powershell
.\build.ps1 kvmgo          # Build KVMGo
.\build.ps1 all            # Build all versions
.\build.ps1 -NoBle kvmgo   # Build without Bluetooth
```

**Git Bash:**
```bash
./build-win.sh kvmgo
./build-win.sh all
./build-win.sh --no-ble kvmgo
```

### Flash Firmware

**PowerShell:**
```powershell
.\tools\flash.ps1 kvmgo              # Flash KVMGo
.\tools\flash.ps1 -Firmware custom.bin  # Flash custom firmware
```

**Git Bash:**
```bash
./tools/flash-win.sh kvmgo
./tools/flash-win.sh --firmware custom.bin
```

### Build + Flash (One Command)

```powershell
.\build-and-flash.ps1 kvmgo          # Build and flash KVMGo
.\build-and-flash.ps1 -NoBle minikvm # Build without BLE and flash
```

## Build Targets

| Target | Description | BOARD_VERSION | BLE |
|--------|-------------|---------------|-----|
| `kvmgo` | KVMGo | 1 | Enabled |
| `minikvm` | Mini-KVM v1.2 | 2 | Disabled |
| `keymod-plus` | KeyMod Plus | 3 | Enabled |
| `keymod-mini` | KeyMod Mini | 3 | Enabled |
| `all` | Build all versions | - | - |

## Flash Procedure

### Enter BOOT Mode

Before flashing, the device must be in BOOT/ISP mode:

1. **Method 1**: Hold BOOT button while plugging in USB
2. **Method 2**: Pull BOOT pin high during reset

**Verify BOOT mode in Windows:**
```powershell
Get-PnpDevice -Class USB | Where-Object { $_.InstanceId -match '55E0' }
```

You should see:
- **Vendor ID**: 0x4348 (or 0x1A86)
- **Product ID**: 0x55E0

### Flash

```powershell
# Auto-detect flash tool
.\tools\flash.ps1 kvmgo

# Force use wchisp
.\tools\flash.ps1 --tool wchisp kvmgo

# Flash custom firmware
.\tools\flash.ps1 --firmware C:\path\to\firmware.bin
```

## Output Files

Built firmware files are placed in the `output/` directory:

```
output/
├── openterface-km-firmware-kvmgo.bin    # Raw binary (for wchisp)
├── openterface-km-firmware-kvmgo.hex    # Intel HEX (for WCHISPTool_CMD)
├── openterface-km-firmware-kvmgo.elf    # ELF with debug symbols
└── openterface-km-firmware-kvmgo.map    # Memory map
```

## Troubleshooting

### Build Issues

**Compiler not found:**
```powershell
# Specify compiler path manually
.\build.ps1 -CompilerPath "D:\MounRiver_Studio2\...\bin" kvmgo
```

**Bash not found:**
The script automatically searches for Git Bash at common locations. If not found:
```powershell
# Install Git for Windows
winget install Git.Git
```

### Flash Issues

**Device not detected:**
1. Check USB connection
2. Ensure device is in BOOT mode (hold BOOT button while plugging in)
3. Try different USB port or cable

**No flash tool found:**
```powershell
# Install wchisp via cargo
cargo install wchisp --git https://github.com/ch32-rs/wchisp

# Or set environment variable to point to WCHISPTool_CMD
$env:WCHISPTOOL_CMD = "C:\path\to\WCHISPTool_CMD.exe"
```

## Advanced Usage

### Custom Build Flags

```bash
cd build

# Build with custom BLE name
make all BOARD_VERSION=1 BLE_NAME="My-KVM" ENABLE_BLE=1

# Build without BLE
make all ENABLE_BLE=0

# Build with external oscillator
make all ENABLE_OSCILLATOR=1

# Build with USB work mode
make all USB_WORK_MODE=hid_ecm_bridge
```

### Flash Options

```powershell
# Don't reset device after flash
.\tools\flash.ps1 --no-reset kvmgo

# Dry run (print commands without executing)
.\tools\flash.ps1 --dry-run kvmgo
```

## Environment Variables

| Variable | Description |
|----------|-------------|
| `RISC_COMPILER_PATH` | RISC-V compiler path |
| `WCHISP_BIN` | wchisp executable path |
| `WCHISPTOOL_CMD` | WCHISPTool_CMD executable path |