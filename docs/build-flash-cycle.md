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

### 2. Enter ISP mode and flash

```bash
PORT="/dev/ttyACM2"   # control board — adjust to match your setup

# Power off target
printf '\x57\xAB\x00\x1A\x01\x00\x1D' > "$PORT"
sleep 0.5

# Set BOOT pin HIGH (enter ISP mode on next boot)
printf '\x57\xAB\x00\x19\x01\x01\x1D' > "$PORT"
sleep 0.5

# Power on target (boots into ISP bootloader)
printf '\x57\xAB\x00\x1A\x01\x01\x1E' > "$PORT"
sleep 3

# Flash via USB (device shows up as VID:0x1A86 or 0x4348, PID:0x55E0)
~/.cargo/bin/wchisp flash openterface-km-firmware.bin

# Release BOOT pin AND power cycle to boot firmware
printf '\x57\xAB\x00\x19\x01\x00\x1C' > "$PORT"   # release BOOT
sleep 0.5
printf '\x57\xAB\x00\x1A\x01\x00\x1D' > "$PORT"   # power off
sleep 1
printf '\x57\xAB\x00\x1A\x01\x01\x1E' > "$PORT"   # power on (BOOT LOW)
sleep 5
```

**CRITICAL**: After flashing, you MUST release the BOOT pin AND power cycle. If the BOOT pin stays HIGH, the device will boot back into ISP mode and the firmware will never run. The wchisp auto-reset is not reliable — always do a manual power cycle with BOOT LOW.

### 3. Verify firmware is running

```bash
lsusb | grep -i "openterface"
# Should show: 1a86:fe00 Openterface Target (and 1a86:fe0c Openterface Host)
```

## Identifying the Control Port

The control board (which manages BOOT/power pins) appears as one of the `/dev/ttyACM*` devices with VID:PID `1a86:fe0c`. **The port number varies across reboots/replugs** — always verify before sending commands.

### Quick identification

Send a power-off command to each port and check if the Target disappears:

```bash
for port in /dev/ttyACM*; do
  echo "=== Testing $port ==="
  printf '\x57\xAB\x00\x1A\x01\x00\x1D' > "$port" 2>/dev/null
  sleep 1
  lsusb | grep "1a86:fe00" || echo "Target disappeared — $port is the control board"
done
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
