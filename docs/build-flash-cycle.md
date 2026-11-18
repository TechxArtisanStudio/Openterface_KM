# Auto Build & Flash Cycle

## Overview

Firmware can be **built and flashed entirely on Linux** using the MounRiver Studio (MRS) Linux toolchain and `wchisp`. No Windows machine is required.

A Windows-based build flow (via SSH to a remote Windows machine) is also documented as an alternative.

## Prerequisites

### Toolchain (Linux)

Install the MRS Linux toolchain:

```bash
mkdir -p ~/tools
tar -xf ~/Downloads/MRS_Toolchain_Linux_X64_V300.tar.xz -C ~/tools
export PATH="$HOME/tools/MRS-linux-x64/resources/app/resources/linux/components/WCH/Toolchain/RISC-V Embedded GCC/bin:$PATH"
```

Verify:

```bash
riscv-none-embed-gcc --version
# riscv-none-embed-gcc (xPack GNU RISC-V Embedded GCC, 64-bit) 8.2.0
```

### Flash Tool

```bash
cargo install wchisp --git https://github.com/ch32-rs/wchisp
```

Install udev rules so `wchisp` can access the ISP USB device without root:

```bash
sudo tee /etc/udev/rules.d/50-wchisp.rules << 'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="1a86", ATTR{idProduct}=="55e0", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="4348", ATTR{idProduct}=="55e0", MODE="0666"
EOF
sudo udevadm control --reload-rules && sudo udevadm trigger
```

### Device Connection

- Target device connected via USB
- Control board (CH32 that manages BOOT/power pins) connected — appears as `/dev/ttyACM*`

## Local Build & Flash (Recommended)

### 1. Build

```bash
cd build
make clean
make PRODUCT=keymod-plus   # or kvmgo | minikvm | keymod-plus-beta | keymod-mini
riscv-none-embed-objcopy -O binary openterface-km-firmware.elf openterface-km-firmware.bin
```

## Local Build & Flash (Recommended)

### 1. Build

```bash
cd build
make clean
make PRODUCT=keymod-plus   # or kvmgo | minikvm | keymod-plus-beta | keymod-mini
riscv-none-embed-objcopy -O binary openterface-km-firmware.elf openterface-km-firmware.bin
```

### 2. Enter ISP mode and flash (NEW: use ctrl_board.py tool)

The `tools/ctrl_board.py` tool provides a complete CLI for the control board, auto-detecting the correct serial port.

```bash
# From project root
cd /home/bbot/projects/Openterface/Openterface_KM

# Full flash cycle (build + flash + verify)
python3 tools/ctrl_board.py flash build/openterface-km-firmware-keymod-plus-beta.bin
```

**Or step-by-step:**

```bash
# 1. Check USB device status
python3 tools/ctrl_board.py status

# 2. Enter ISP mode (BOOT HIGH + power cycle)
python3 tools/ctrl_board.py isp on

# 3. Flash firmware
~/.cargo/bin/wchisp flash build/openterface-km-firmware-keymod-plus-beta.bin

# 4. Exit ISP mode (BOOT LOW + power cycle)
python3 tools/ctrl_board.py isp off

# 5. Verify firmware is running
python3 tools/ctrl_board.py verify
```

**Other useful commands:**
```bash
python3 tools/ctrl_board.py power on      # Power on target
python3 tools/ctrl_board.py power off     # Power off target
python3 tools/ctrl_board.py boot high     # Set BOOT pin HIGH
python3 tools/ctrl_board.py boot low      # Set BOOT pin LOW
python3 tools/ctrl_board.py status        # Show USB devices and serial ports
```

### 3. Verify firmware is running (NEW: dedicated verification tool)

```bash
# Via control board (USB device check)
python3 tools/ctrl_board.py verify

# Via CDC ACM (CMD_GET_INFO - more detailed)
python3 tools/verify_firmware.py
```

Both tools confirm the firmware is running correctly. The `verify_firmware.py` tool provides additional detail (board version, USB connection status, LED status).

## Identifying the Control Port

The control board (which manages BOOT/power pins) appears as one of the `/dev/ttyACM*` devices with VID:PID `1a86:fe0c`. **The port number varies across reboots/replugs.**

**Good news:** The `tools/ctrl_board.py` tool automatically detects the control board by its `USB_Serial` model name. You don't need to manually identify the port unless debugging.

```bash
# Tool auto-detects the control board
python3 tools/ctrl_board.py status
# Control board: /dev/ttyACM0
```

### Manual identification (for debugging)

If the tool can't find the control board, identify it manually:

```bash
# Method 1: Identify by model name (recommended)
for dev in /dev/ttyACM*; do
  model=$(udevadm info -q property -n "$dev" 2>/dev/null | grep ID_MODEL= | cut -d= -f2)
  echo "$dev -> $model"
done
# Control board shows as "USB_Serial", NOT "Openterface_Host"

# Method 2: Use the tool with --port override
python3 tools/ctrl_board.py --port /dev/ttyACM1 status
```

### USB topology

The Openterface dev kit exposes **5 USB endpoints**:

| Device | VID:PID | Description |
|--------|---------|-------------|
| Control board | 1a86:fe0c | CH32 USB Serial — manages the target's BOOT and power pins |
| USB Serial #1 | 1a86:fe0c | Additional serial interface |
| USB Serial #2 | 1a86:fe0c | Additional serial interface |
| Target (normal mode) | 1a86:fe00 | "Openterface Target" — the CH32V208GBU6 running firmware |
| Target (host side) | 1a86:fe0c | "Openterface Host" — second CDC-ACM interface from target |
| Target (ISP mode) | 1a86:55e0 or 4348:55e0 | WCH ISP bootloader — appears only when BOOT pin is held HIGH during power-on |

To identify ports by device number:

```bash
for dev in /dev/ttyACM*; do
  devpath=$(udevadm info -q path -n $dev 2>/dev/null)
  devnum=$(cat /sys$devpath/../../../devnum 2>/dev/null)
  echo "$dev -> devnum=$devnum"
done
lsusb -t   # match devnum to Bus/Port/Dev
```

## Alternative: Windows Remote Build

For cases where the local toolchain is not available, firmware can be built on a Windows machine:

### Prerequisites

- SSH access: `admin@192.168.100.63`
- MSYS2 installed at `C:\msys64`
- MounRiver Studio toolchain (riscv-none-embed-gcc) available in MSYS2 PATH

### Steps

```bash
# 1. Zip and upload source
zip -r /tmp/openterface-km-build.zip \
  SRC/ Profile/ HAL/ LIB/ APP/ \
  tools/build_firmware.sh tools/wchisp_c.c HAL/Link.ld \
  -x "*/.DS_Store" -x "__MACOSX/*"
scp /tmp/openterface-km-build.zip admin@192.168.100.63:/tmp/

# 2. Build on Windows
ssh admin@192.168.100.63 'powershell -Command "Expand-Archive -Path C:\tmp\openterface-km-build.zip -DestinationPath C:\tmp\openterface-km-build -Force"'
ssh admin@192.168.100.63 'C:\msys64\usr\bin\bash.exe -l -c "cd /c/tmp/openterface-km-build && bash tools/build_firmware.sh"'

# 3. Fetch binary
scp admin@192.168.100.63:/tmp/openterface-km-build/obj/openterface-km-firmware.bin /tmp/

# 4. Flash locally (same as step 2 above)
```

## Device Connection

### USB Topology

The Openterface dev kit exposes **5 USB endpoints** on the host:

| Device | VID:PID | Description |
|--------|---------|-------------|
| Control board | 1a86:fe0c | CH32 USB Serial — manages the target's BOOT and power pins. Appears as `/dev/ttyACM*` |
| USB Serial #1 | 1a86:fe0c | Additional serial interface |
| USB Serial #2 | 1a86:fe0c | Additional serial interface |
| Target (normal mode) | 1a86:fe00 | "Openterface Target" — the CH32V208GBU6 running firmware |
| Target (host side) | 1a86:fe0c | "Openterface Host" — second CDC-ACM interface from target |
| Target (ISP mode) | 1a86:55e0 or 4348:55e0 | WCH ISP bootloader — appears only when BOOT pin is held HIGH during power-on |

Multiple devices share VID:PID `1a86:fe0c`. The control board is the one that responds to power control commands. See [Identifying the Control Port](#identifying-the-control-port) above.

### Flash Control Protocol

The target CH32V208GBU6's **BOOT pin** and **power pin** are controlled by the control CH32 via USB serial commands. No manual button-pressing is needed.

**Serial Port Settings:**
- **Baud Rate**: 115200 (NOT 9600)
- **Data Bits**: 8
- **Stop Bits**: 1
- **Parity**: None

#### Command Format

```
57 AB 00 <cmd> <len> <data> <checksum>
```

| Control | Direction | Frame | Checksum |
|---------|-----------|-------|----------|
| Flash (BOOT) pin | Pull HIGH | `57 AB 00 19 01 01 1D` | 0x1D |
| Flash (BOOT) pin | Pull LOW | `57 AB 00 19 01 00 1C` | 0x1C |
| Power pin | Pull HIGH | `57 AB 00 1A 01 01 1E` | 0x1E |
| Power pin | Pull LOW | `57 AB 00 1A 01 00 1D` | 0x1D |

#### Response Format

The control board responds to valid commands:

```
57 AB 00 <cmd+0x80> <len> <data> <checksum>
```

Example: sending `57AB001A01001D` (power off) returns `57ab009a01009d`.

### CDC ACM CH9329 Commands (Target Firmware)

The target firmware also accepts CH9329 commands over CDC ACM (`/dev/ttyACM1` — Openterface Host) for firmware-level operations.

| Command | Code | Description |
|---------|------|-------------|
| CMD_GET_INFO | 0x01 | Query device info (version, USB state, LEDs) |
| CMD_ENTER_ISP_MODE | 0x32 | Prepare for ISP entry (host uses control board) |
| CMD_ISP_DEBUG | 0x33 | Phase 1 ISP diagnostics (sub-cmds 0x00-0x04) |

**Frame format:** `57 AB <addr> <cmd> <len> <data> <checksum>`

**Example: CMD_GET_INFO (verify firmware running)**
```bash
python3 tools/verify_firmware.py
# ✓ Firmware is running
#   Board version: 3
#   USB connected: Yes
#   LED status: 0x00
#   Flags: 0x06
```

**Example: CMD_ENTER_ISP_MODE (prepare for ISP)**
```bash
# Frame: 57 AB 00 32 00 34
python3 -c "
import serial
with serial.Serial('/dev/ttyACM1', 115200, timeout=1) as ser:
    ser.write(bytes.fromhex('57AB00320034'))
"
# Response: 57ab00b20100b5 (STATUS_SUCCESS)
```

## Known Working Firmware

- `BLE_USB.hex` — known-good reference firmware (LED + BLE both work)
- `obj/openterface-km-firmware.bin` — built from this repo

## PC7 External LED Configuration (CONFIRMED WORKING)

### Hardware
- LED on **PC7 (GPIOC Pin 7)**, active-low (LOW = ON, HIGH = OFF)
- TIM3_CH2 with full remap: PC6=CH1, PC7=CH2, PC8=CH3, PC9=CH4

### Initialization Order (in `main()`)
1. `RGB_Init()` — initializes TIM3 (PSC=71, ARR=999, PWM1 mode, High polarity) and configures PC6/PC8/PC9
2. `PWM_LED_Init()` — adds TIM3_CH2 (PC7) without reinitializing the time base

**PWM_LED_Init() must be called AFTER RGB_Init()** because RGB_Init sets up the TIM3 FullRemap and time base. If PWM_LED_Init runs first, RGB_Init will overwrite its TIM3 config.

### Working PWM_LED_Init Implementation
```c
void PWM_LED_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    TIM_OCInitTypeDef TIM_OCInitStructure;

    /* Clocks (RGB_Init already enabled these) */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO, ENABLE);

    /* Full remap: TIM3_CH2 -> PC7 (same as RGB_Init) */
    GPIO_PinRemapConfig(GPIO_FullRemap_TIM3, ENABLE);

    /* PC7: alternate function push-pull for TIM3_CH2 */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    /* TIM3 CH2: PWM mode 1, polarity HIGH, 100% brightness */
    TIM_OCInitStructure.TIM_OCMode      = TIM_OCMode_PWM1;
    TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
    TIM_OCInitStructure.TIM_Pulse       = 0;            /* 100% brightness (LED ON) */
    TIM_OCInitStructure.TIM_OCPolarity  = TIM_OCPolarity_High;
    TIM_OC2Init(TIM3, &TIM_OCInitStructure);

    TIM_OC2PreloadConfig(TIM3, TIM_OCPreload_Enable);
    g_brightness = 100;
}
```

### Brightness Control
With PWM1 + High polarity on an active-low LED:
- `CCR = 0`: counter always >= CCR -> pin LOW -> LED fully ON
- `CCR = 1000`: counter always < CCR -> pin HIGH -> LED fully OFF
- `CCR = (1000 - brightness * 1000 / 100)`: maps 0-100% brightness

### Build Script Config
- `CLK_OSC32K=1` (internal 32K RC oscillator for BLE)
- `ENABLE_BLE` defined
- `version=1` (kvm go mode)
- Links against `libwchble.a` in `LIB/`
- Toolchain (Linux): `~/tools/MRS-linux-x64/resources/app/resources/linux/components/WCH/Toolchain/RISC-V Embedded GCC/bin/riscv-none-embed-gcc`
- Toolchain (Windows): `C:/MounRiver/.../riscv-none-embed-gcc`
- Flags: `-march=rv32imacxw -mabi=ilp32 -Os`

## Verified Flash Parameters (Tested 2026-11-18)

The following parameters were verified on actual hardware:

### Control Board Serial Port

| Parameter | Value | Notes |
|-----------|-------|-------|
| **Baud Rate** | **115200** | NOT 9600 — 9600 produces no response |
| **Port** | `/dev/ttyACM0` | Shows as "USB Serial" (VID:PID 1a86:fe0c) |
| **Model** | `USB_Serial` | Use `udevadm info -n /dev/ttyACM0` to identify |

**Identification:**
```bash
# Control board shows as "USB_Serial", NOT "Openterface_Host"
for dev in /dev/ttyACM*; do
  udevadm info -q property -n "$dev" | grep ID_MODEL
done
# Output:
# ID_MODEL=USB_Serial          <-- CONTROL BOARD (use this)
# ID_MODEL=Openterface_Host    <-- Target CDC-ACM (NOT this)
```

### Verified Flash Sequence (NEW: use tools)

**Recommended: Use `ctrl_board.py` tool (auto-detects port, handles timing)**

```bash
# Full flash cycle (one command)
python3 tools/ctrl_board.py flash build/openterface-km-firmware-keymod-plus-beta.bin

# Or step-by-step
python3 tools/ctrl_board.py isp on                    # Enter ISP mode
~/.cargo/bin/wchisp flash firmware.bin                # Flash
python3 tools/ctrl_board.py isp off                   # Exit ISP mode
python3 tools/ctrl_board.py verify                    # Verify via USB
python3 tools/verify_firmware.py                      # Verify via CMD_GET_INFO
```

**Manual method (for reference/debugging):**

```bash
PORT="/dev/ttyACM0"   # Control board (USB_Serial)
BAUD=115200

# Helper function to send command at 115200
send_cmd() {
  python3 -c "
import serial, sys
with serial.Serial('$PORT', $BAUD, timeout=1) as ser:
    ser.write(bytes.fromhex('$1'))
"
}

# 1. Power OFF target
send_cmd 57AB001A01001D
sleep 1

# 2. BOOT pin HIGH (enter ISP on next boot)
send_cmd 57AB001901011D
sleep 1

# 3. Power ON target (boots into ISP bootloader)
send_cmd 57AB001A01011E
sleep 3

# 4. Verify ISP mode (should show 4348:55e0 or 1a86:55e0)
lsusb | grep 55e0

# 5. Flash firmware
~/.cargo/bin/wchisp flash openterface-km-firmware-keymod-plus-beta.bin

# 6. BOOT pin LOW
send_cmd 57AB001901001C
sleep 1

# 7. Wait for auto-reset to APP
sleep 5

# 8. Verify (should show 1a86:fe00 Openterface Target)
lsusb | grep "1a86:fe00"
```

### Timing Notes

- **Sleep after power OFF**: 1s (allows clean shutdown)
- **Sleep after BOOT HIGH**: 1s (allows pin to stabilize before power-on)
- **Sleep after power ON**: 3s (allows USB enumeration in ISP mode)
- **Sleep after BOOT LOW**: 1s (allows pin release before reset)
- **Sleep after flash**: 5s (allows wchisp reset + firmware boot)

### Critical Differences from Original Doc

| Original Doc | Actual Verified |
|--------------|-----------------|
| Baud 9600 | **Baud 115200** |
| PORT=/dev/ttyACM2 | **PORT=/dev/ttyACM0** (varies, must identify) |
| sleep 0.5 | **sleep 1-3s** (longer delays needed) |
| printf to port | **Python serial** (reliable binary write) |

## LED Debug Codes (PC7)

The PC7 external LED signals firmware state so you can diagnose without USB debug traffic:

| Pattern | Timing | Meaning |
|---------|--------|---------|
| **OFF** | First 3s after boot | Boot phase — firmware initializing |
| **Slow blink** | 1s ON / 1s OFF | ECM data interface activated (SET_INTERFACE received), idle — waiting for data |
| **Fast blink** | 100ms ON / 100ms OFF | TX activity on EP7 — data is flowing |
| **Double flash** | 2×150ms flashes, 1.2s cycle | Error: >10s elapsed but host never activated data interface (SET_INTERFACE never received) |

### Interpreting patterns

- **Double flash** → check USB descriptor, SET_INTERFACE handler, or whether Mac driver loaded
- **Slow blink but no fast blink on ping** → EP7 TX path not triggering; check `poll_host_tx()` queue logic
- **Fast blink on ping** → data leaving EP7; if ping still fails, problem is on Mac side (routing, ARP cache)
