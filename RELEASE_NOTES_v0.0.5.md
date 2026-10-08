# Openterface KVM Firmware v0.0.5 Release Notes

**Release Date:** September 2026  
**Base Version:** v0.0.4 → v0.0.5  
**Commits:** 89 files changed, 14,457 insertions(+), 416 deletions(-)

---

## 🎯 Highlights

This release delivers **major stability improvements**, **BLE advertising enhancements**, **USB HID latency fixes**, and **comprehensive testing infrastructure**. Key achievements include:

- **P0 stability fixes** resolving critical watchdog resets and mailbox logic issues
- **BLE advertising parameter command (0x19)** with TX power control and frequency selection
- **USB HID latency resolution** by moving parsing out of interrupt context
- **Industrial-grade firmware optimization plan** documented for production readiness
- **New testing tools**: serial fuzz testing, flash automation, and BLE test mode enhancements

---

## 🔧 Major Features

### BLE Advertising & Configuration
| Feature | Description |
|---------|-------------|
| **BLE Broadcast Parameter Command (0x19)** | New command specification and HTML test tool for configuring BLE advertising parameters |
| **TX Power Control** | User-specified power setting in BLE advertising command with validation |
| **Frequency Band Selection** | Enhanced test mode supporting frequency selection for continuous TX |
| **BLE Name Configuration** | Board-specific default BLE names (replacing hardcoded "KVM-GO") with compile-time guards and error return mechanism |
| **Automatic System Reset** | Auto-reset after BLE name change to ensure proper application |

### USB HID & Performance
| Improvement | Impact |
|-------------|--------|
| **USB HID Latency Fix** | Moved CH9329 parsing out of interrupt context → eliminated blocking in ISR |
| **printf Removal** | Removed all blocking `printf`/`PRINT`/`iprintf` from hot paths and interrupt contexts |
| **Queue Depth Increase** | Relative mouse queue deepened from 28 → 64 slots |
| **Absolute Mouse Model** | Refactored to single-slot newest-wins mailbox for lower latency |
| **Zero-copy Receive Buffer** | Consume-offset receive buffer eliminating `memmove` per parsed frame |

### Stability & Reliability (P0 Fixes)
- **Watchdog Reset Prevention** — Removed delay loops causing watchdog resets
- **Critical Section Correctness** — Fixed MIE bit management in mailbox logic; added nesting-safe critical-section macros
- **Queue Thread Safety** — All `Queue_Push_*`, `Queue_Pop_*`, `Queue_Init` now protected with critical sections
- **Response Queue** — Single-consumer gating and head-advance protection
- **CH9329 Framing Buffer** — Protected with critical sections
- **USB Suspend/Resume** — Fixed USBFS STOP mode handling, register restoration on bus reset, D+ pull-up timing
- **Endpoint Recovery** — USB endpoint busy timeout recovery mechanism implemented

---

## 🧪 Testing & Tooling

| Tool | Purpose |
|------|---------|
| **Serial Fuzz Test (`serial_fuzz_test.py`)** | 620-line comprehensive fuzz testing framework for serial/CH9329 communication |
| **Flash Automation (`flash.sh`, `flash_clean.ini`)** | Complete flash cycle automation with clean configuration |
| **Build Script Enhancements** | `build_flash_cycle.sh` supports `USB_WORK_MODE` override for KVM-Go/MiniKVM variants |
| **BLE Test Mode** | LL Test API integration with ultra-short advertising interval, power control, continuous TX |
| **TCP/Serial Commands** | Firmware version check and USB mode query tools (`check_firmware_version.py`, `query_usb_mode.py`) |

---

## 🐛 Bug Fixes

### BLE & Communication
- Fixed CH9329 command response failures and USB endpoint handling
- Corrected BLE advertising channel mapping and frequency band mapping
- Fixed `CMD_SYSTEM_RESET` BLE routing bug
- Added missing BLE security command definitions
- Fixed BLE name display issues and build script problems

### USB & HID
- Fixed EP3 IN endpoint enabling in `USBD_SetConfiguration` for CH9329 responses
- Improved EP3 response queue drain logic
- Fixed `USBD_IsCh9329ModeCommandFrame` to accept all CH9329 commands
- Added `USB_WORK_MODE_HAS_HOST_CDC_ACM` compilation condition for KVM-Go 01 command support
- Fixed ECM bridge broken by 16-byte wMaxPacketSize (reverted packet size increase)

### Hardware & Peripherals
- Fixed SD switch initialization and state handling
- Corrected RGB blue flash duration in version verification sequence
- Updated KVM-Go startup color sequence for better visibility
- Fixed `Update_SDcard` color logic for better state handling
- Restored `fSuspendEnabled = TRUE` to fix device enumeration

---

## 📚 Documentation

- **Industrial-Grade Optimization Plan** — Comprehensive USB firmware optimization roadmap
- **HID Queue Reliability Redesign Spec** — English translation with implementation checklist
- **CH9329 Communication Protocol** — Detailed frame format, command map, status codes
- **Build & Flash Guides** — Updated documentation with output directory changes
- **Qt Firmware Update Guide** — Updated screenshots (Control Chip Firmware replacing WCH ISP Flash)
- **USB Error -71 Fix Documentation** — Added troubleshooting guide

---

## 🏗️ Infrastructure

### CI/CD
- **Build Artifact Retention** — 7-day retention policy added to prevent storage quota exhaustion
- **Node.js 24 Compatibility** — GitHub Actions upgraded to Node 24 compatible versions
- **Branch Specification Fix** — Corrected build workflow branch format

### Code Quality
- Obsolete test scripts removed (CH9329 device communication)
- Critical-section macros refactored for nesting safety
- Dead declarations removed (`USBD_SendFromRespQueue`)

---

## 📦 Affected Files Summary

### Core Application (`APP/`)
- `peripheral_main.c` — Main loop, initialization
- `keyboard_handler.c`, `mouse_handler.c` — HID processing
- `usbd_composite_km.c`, `usbd_desc.c` — USB composite device
- `UART/`, `USBLIB/` — Communication stack

### Hardware Abstraction (`HAL/`)
- LED, KEY, RTC, SLEEP, RGB implementations
- Linker script updates

### BLE Stack (`Profile/`, `LIB/`)
- GATT profiles, device info service, custom BLE USB service
- BLE task scheduler, ROM interface

### Tools & Scripts
- `build.sh`, `build_flash_cycle.sh`, `flash.sh`, `flash_clean.ini`
- `serial_fuzz_test.py`, `check_firmware_*.py`, `query_usb_mode.py`
- `kvmgo_tool.py` — Device management utility

---

## ⚠️ Breaking Changes

- **BLE Default Name**: Now uses board-specific `BLE_DEVICE_NAME` macro instead of hardcoded "KVM-GO"
- **USB Work Mode**: `USB_WORK_MODE_HAS_HOST_CDC_ACM` now required for KVM-Go 01 command
- **Queue Architecture**: Relative mouse queue depth changed (28 → 64), absolute mouse now single-slot mailbox

---

## 🔄 Upgrade Notes

1. **Re-flash Required**: Full flash recommended due to BLE name and USB mode changes
2. **Board Configuration**: Ensure correct `BLE_DEVICE_NAME` and `USB_WORK_MODE` for your hardware variant
3. **Test BLE Advertising**: Verify new 0x19 command with provided HTML test tool
4. **Monitor Stability**: Watch for watchdog resets — the P0 fixes should eliminate them

---

## 🙏 Acknowledgments

Thanks to all contributors for stability fixes, performance optimizations, and testing infrastructure. Special recognition for the P0 stability fix plan and comprehensive serial fuzz testing framework.

---

**Full Changelog:** `git log v0.0.4..v0.0.5 --oneline`  
**Protocol Documentation:** `docs/ch9329-communication-protocol.md`  
**Optimization Plan:** `docs/USB_FIRMWARE_INDUSTRIAL_GRADE_OPTIMIZATION_PLAN.md`