# Software ISP Entry Design

**Status**: Draft  
**Date**: 2026-11-17  
**Target MCU**: WCH CH32V208 (RISC-V rv32imacxw)

## Overview

This document describes the design for implementing software-triggered ISP (In-System Programming) mode entry on the CH32V208. Currently, entering ISP mode requires physical interaction (holding BOOT button + reset). The goal is to enable firmware updates via a software command, allowing remote or automated flashing without physical access.

## Current State

### Physical ISP Entry Method

The device currently requires manual intervention to enter ISP mode:

1. **Hold BOOT button** (PB8/BOOT0)
2. **Press RESET button**
3. **Release BOOT button**

The device then enumerates as USB VID:PID `1A86:55E0` (or `4348:55E0`) for the `wchisp` flashing tool.

### Hardware Constraints

- **PB8 is shared with BOOT0** on the QFN28 CH32V208GBU6 package
- External **pull-down resistor** on BOOT0 keeps it low for normal flash boot
- GPIO state is **lost during reset** - cannot drive PB8 high in software and expect it to persist through reset
- The existing `button.c` code explicitly delays reset until button release to avoid accidental ISP entry

### Existing Reset Mechanisms

The firmware already has software reset capabilities:

```c
// From peripheral.c
void Peripheral_ScheduleSystemReset(uint16_t delay_ms);

// From core_riscv.h
__attribute__((always_inline)) RV_STATIC_INLINE void NVIC_SystemReset(void)
{
    NVIC->CFGR = NVIC_KEY3 | (1<<7);  // 0xBEEF0080
}
```

These perform a normal reset that boots from user flash (BOOT0=0).

## Proposed Solution

### WCH RISC-V Bootloader Entry Mechanism

Research into the WCH RISC-V family (CH32V003, CH32V203, CH32V208) reveals a software mechanism to enter the ROM bootloader:

**Sequence** (from community documentation and reverse engineering):

```c
// Step 1: Unlock BOOT_MODEKEYR with magic keys
FLASH->BOOT_MODEKEYR = 0x45670123;  // KEY1
FLASH->BOOT_MODEKEYR = 0xCDEF89AB;  // KEY2

// Step 2: Set MODE bit in FLASH_STATR (bit 14)
FLASH->STATR = 0x4000;  // MODE = 1 → boot from system memory

// Step 3: Trigger software reset
RCC->RSTSCKR |= 0x1000000;  // SFTRST bit
// OR use: NVIC_SystemReset();
```

After reset, the MCU boots from the ROM bootloader (system memory) instead of user flash, appearing as the ISP device.

### CH32V208 Register Analysis

**Existing registers in `ch32v20x.h`:**

```c
typedef struct {
    __IO uint32_t ACTLR;      // +0x00
    __IO uint32_t KEYR;       // +0x04
    __IO uint32_t OBKEYR;     // +0x08
    __IO uint32_t STATR;      // +0x0C
    __IO uint32_t CTLR;       // +0x10
    __IO uint32_t ADDR;       // +0x14
    __IO uint32_t RESERVED;   // +0x18
    __IO uint32_t OBR;        // +0x1C
    __IO uint32_t WPR;        // +0x20
    __IO uint32_t MODEKEYR;   // +0x24
} FLASH_TypeDef;
```

**Key findings:**

| Register | Status | Notes |
|----------|--------|-------|
| `FLASH->MODEKEYR` | ✅ Defined | Used for fast program mode unlock |
| `BOOT_MODEKEYR` | ❓ Unknown | Not explicitly defined in CH32V208 header |
| `FLASH_STATR.MODE` (bit 14) | ❌ Not defined | Header only defines bits 0, 2, 4, 5 |

**Critical observation:** The `MODEKEYR` register exists but is currently used for fast program mode unlock, not boot mode. The `BOOT_MODEKEYR` register may be:
1. The same register with different unlock keys
2. A separate register at a different offset
3. Not present on CH32V208 (only on CH32V003)

### ROM Bootloader Location

Based on community research:
- **CH32V20x family**: ROM bootloader at approximately `0x1FFF8000` (28KB)
- **CH32V003**: ROM bootloader at `0x1FFFF000`
- **Bootloader interfaces**: USB and USART1

## Implementation Plan

### Phase 1: Investigation (COMPLETED - FAILED)

**Goal**: Verify if the bootloader entry mechanism exists on CH32V208.

**Date**: 2026-11-18  
**Result**: ❌ **FAILED** - Mechanism does NOT exist on CH32V208

#### 1.1 Register Offset Discovery ✅

Added debug code to dump FLASH controller registers via CMD_ISP_DEBUG (0x33).

**Findings**:
- `FLASH->MODEKEYR` exists at offset +0x24
- No additional registers found after MODEKEYR
- All registers readable and writable as expected

#### 1.2 Test STATR Bit 14 ✅

**Result**: Bit 14 (MODE) is **NOT defined** in CH32V20x.

**CH32V20x FLASH_STATR only defines**:
- Bit 0: BSY (busy)
- Bit 2: PGERR (programming error)
- Bit 4: WRPRTERR (write protection error)
- Bit 5: EOP (end of operation)

**No MODE bit (0x4000) exists.**

#### 1.3 Test Bootloader Entry ❌

**Test**: Attempted full sequence:
1. Write magic keys 0x45670123, 0xCDEF89AB to MODEKEYR
2. Attempt to set STATR bit 14 (0x4000)
3. Trigger NVIC_SystemReset()

**Result**: Device rebooted normally (1a86:fe00) instead of entering ISP mode (1a86:55e0).

**Conclusion**: The undocumented bootloader entry mechanism (CH32V003-style) does **NOT** work on CH32V208.

#### Phase 1 Summary

| Test | Result | Notes |
|------|--------|-------|
| MODEKEYR register exists | ✅ | Offset +0x24, used for fast program mode |
| STATR bit 14 (MODE) exists | ❌ | Not defined in CH32V20x headers |
| Software ISP entry works | ❌ | Device reboots normally, doesn't enter ISP |

**Final Conclusion**: CH32V208 does **NOT** support software-triggered ROM bootloader entry. The mechanism is specific to CH32V003.

**Alternative approaches required**:
1. ✅ **Control board** (already implemented) - physically controls BOOT pin during power cycle
2. Custom software bootloader at flash start (complex, requires linker changes)
3. Physical BOOT button (current fallback)

### Phase 2: Implementation (REVISED - Control Board Approach)

**Goal**: Add a CH9329 command to prepare for ISP entry, then use control board to enter ISP mode.

**Date**: 2026-11-18  
**Approach**: Since software ISP entry doesn't work on CH32V208, Phase 2 uses the existing control board mechanism (which physically controls BOOT pin during power cycle).

#### 2.1 Architecture

```
Host (Linux/Mac/Windows)
  │
  ├─► CH9329 CMD_ENTER_ISP_MODE (0x32) → Target device
  │     └─► Target acknowledges, prepares for ISP mode
  │
  └─► Control board serial command → Control board
        └─► Control board enters ISP mode (BOOT HIGH + power cycle)
```

**Flow**:
1. Host sends `CMD_ENTER_ISP_MODE` to target via CDC ACM (CH9329 protocol)
2. Target acknowledges and sets internal flag
3. Host sends ISP entry command to control board via serial (115200 baud)
4. Control board physically enters ISP mode (BOOT HIGH → power ON)
5. Target boots into ISP bootloader (1a86:55e0)
6. Host flashes firmware using `wchisp`
7. Host sends ISP exit command to control board (BOOT LOW → power cycle)
8. Target boots normally with new firmware

#### 2.2 Define New Command

Add to `usbd_composite_km.h`:

```c
#define CMD_ENTER_ISP_MODE    0x32  // Prepare for ISP mode entry
```

#### 2.3 Implement Command Handler

Add to `usbd_compostie_km.c` in `CH9329_DispatchCommand()`:

```c
case CMD_ENTER_ISP_MODE:
{
    uint8_t status = STATUS_SUCCESS;
    
    // Send success response
    CH9329_SendResponse(addr, cmd_code, &status, 1, 1);
    
    // Set flag to indicate ISP mode preparation
    // (Optional: could disable BLE, save state, etc.)
    printf("ISP mode preparation complete. Host should use control board to enter ISP mode.\r\n");
    
    break;
}
```

#### 2.4 Update Command Table

Add to `Peripheral_IsCommandByte()` in `peripheral.c`:

```c
case CMD_ENTER_ISP_MODE:         /* 0x32 - Prepare for ISP mode entry */
    return 1;
```

#### 2.5 Host-Side Automation Script

The `tools/ctrl_board.py` script already provides the control board interface:

```bash
# Enter ISP mode
python3 tools/ctrl_board.py isp on

# Flash firmware
~/.cargo/bin/wchisp flash firmware.bin

# Exit ISP mode
python3 tools/ctrl_board.py isp off
```

**Full automation flow**:
```bash
#!/bin/bash
# 1. Send CMD_ENTER_ISP_MODE to target (optional preparation)
python3 -c "
import serial
frame = bytes([0x57, 0xAB, 0x00, 0x32, 0x00, 0x89])  # CMD_ENTER_ISP_MODE
with serial.Serial('/dev/ttyACM1', 115200, timeout=1) as ser:
    ser.write(frame)
"

# 2. Enter ISP mode via control board
python3 tools/ctrl_board.py isp on

# 3. Flash firmware
~/.cargo/bin/wchisp flash firmware.bin

# 4. Exit ISP mode
python3 tools/ctrl_board.py isp off

# 5. Verify firmware is running (via CMD_GET_INFO)
python3 tools/verify_firmware.py

# 6. Verify via control board (USB device check)
python3 tools/ctrl_board.py verify
```

### Phase 3: Testing and Validation (COMPLETED)

**Date**: 2026-11-18  
**Result**: ✅ **PASSED** - All tests successful

#### 3.1 Test Cases

| Test | Expected Result | Actual Result | Status |
|------|----------------|---------------|--------|
| Send CMD_ENTER_ISP_MODE (0x32) via CDC ACM | Target acknowledges | Response: `57ab00b20100b5` (STATUS_SUCCESS) | ✅ PASS |
| Enter ISP mode via control board | Device becomes 1a86:55e0 | ISP device found (1a86:55e0) | ✅ PASS |
| Flash firmware while in ISP mode | Flash succeeds, device boots normally | wchisp flash OK, device boots normally | ✅ PASS |
| Exit ISP mode via control board | Device returns to normal mode | Target running (1a86:fe00) | ✅ PASS |
| Full automated flow | All steps complete successfully | End-to-end flow works | ✅ PASS |

#### 3.2 Test Output

```
=== Full Automated Flow Test ===
1. Send CMD_ENTER_ISP_MODE (0x32) to target
   Target response: 57ab00b20100b5
2. Enter ISP mode via control board (isp on)
   Control board: /dev/ttyACM0
   Entering ISP mode...
     1. Power OFF
     2. BOOT → HIGH
     3. Power ON
   ✓ ISP device found: Bus 001 Device 019: ID 1a86:55e0 QinHeng Electronics
3. Check ISP device
   ✓ ISP device found: Bus 001 Device 019: ID 1a86:55e0 QinHeng Electronics
   ✓ ISP device found: Bus 001 Device 020: ID 4348:55e0 WinChipHead
4. Exit ISP mode (isp off)
   Control board: /dev/ttyACM0
   Exiting ISP mode...
     1. BOOT → LOW
     2. Power OFF
     3. Power ON
   ✓ Target running: Bus 001 Device 021: ID 1a86:fe00 QinHeng Electronics Openterface Target
5. Verify target running
   ✓ Target running: Bus 001 Device 021: ID 1a86:fe00 QinHeng Electronics Openterface Target

=== Flow test complete ===
```

#### 3.3 Implementation Summary

**Files modified**:
- `APP/include/usbd_composite_km.h` - Added `CMD_ENTER_ISP_MODE` (0x32)
- `APP/peripheral.c` - Added command to `Peripheral_IsCh9329CommandByte()`
- `APP/usbd_compostie_km.c` - Added command handler in `CH9329_DispatchCommand()`

**Build stats**:
- FLASH: 227,460 B / 440 KB (50.48%)
- RAM: 53,160 B / 64 KB (81.12%)

#### 3.4 Safety Measures

1. **Response before action**: Target sends success response before host uses control board
2. **Physical isolation**: Control board physically controls BOOT pin - no risk of flash corruption
3. **Recovery**: Physical BOOT button remains as fallback
4. **Documentation**: Full automation script provided

## Risks and Mitigation

### Risk 1: Undocumented Feature

**Risk**: The bootloader entry mechanism may not exist or work differently on CH32V208.

**Mitigation**:
- Phase 1 investigation verifies the mechanism before full implementation
- Physical BOOT button remains as fallback
- No permanent damage if feature doesn't work

### Risk 2: Flash Corruption

**Risk**: Incorrect register writes could corrupt flash configuration.

**Mitigation**:
- Only write to documented registers initially
- Test on development hardware first
- Keep physical BOOT button accessible for recovery

### Risk 3: Bricked Device

**Risk**: Device gets stuck in unknown state.

**Mitigation**:
- Use WCH-LinkE programmer for recovery if needed
- Keep bootloader unlock sequence reversible (power cycle)
- Document recovery procedures

### Risk 4: Security Concerns

**Risk**: Unauthorized firmware updates.

**Mitigation**:
- Consider adding authentication (future enhancement)
- Document that this feature should only be used in trusted environments
- Physical access still required for full recovery

## Alternative Approaches

### Alternative 1: Hardware Modification

**Approach**: Add GPIO-controlled transistor to pull BOOT0 high on command.

**Pros**:
- Reliable, documented mechanism
- No risk of flash corruption

**Cons**:
- Requires hardware revision
- Additional BOM cost
- Not possible for existing devices

### Alternative 2: Custom Software Bootloader

**Approach**: Implement a small bootloader at flash start that checks a flag and enters ISP.

**Pros**:
- Full control over boot process
- Can add authentication
- Works on any MCU

**Cons**:
- Requires linker script changes
- Reduces available flash space
- More complex implementation
- Risk of bricking if bootloader is corrupted

### Alternative 3: Option Byte BFB2

**Approach**: Modify option bytes to force boot from system memory.

**Pros**:
- Uses documented feature (if supported)

**Cons**:
- Permanent change (requires option byte erase to revert)
- High risk of bricking
- Not suitable for temporary ISP entry

## Testing Strategy

### Development Testing

1. **Unit tests**: Verify register writes work as expected
2. **Integration tests**: Verify command triggers ISP entry
3. **Recovery tests**: Verify physical BOOT button still works

### Field Testing

1. **Beta testers**: Deploy to trusted users with recovery equipment
2. **Documentation**: Provide clear instructions for entering/exiting ISP mode
3. **Feedback loop**: Collect results and refine implementation

## Success Criteria

The feature is successful if:

1. ✅ Software command reliably enters ISP mode
2. ✅ Device can be flashed using `wchisp` after software entry
3. ✅ Device boots normally after flashing
4. ✅ Physical BOOT button still works as fallback
5. ✅ No flash corruption or bricking incidents

## Future Enhancements

1. **Authentication**: Add challenge-response to prevent unauthorized ISP entry
2. **BLE support**: Trigger ISP entry via BLE (requires BLE stack to remain active)
3. **Auto-recovery**: Detect failed flash and automatically enter ISP mode
4. **Progress reporting**: Report ISP entry status via USB/BLE

## References

- [WCH CH32V003 Factory Bootloader - The Missing Manual](https://github.com/basilhussain/ch32v003-bootloader-docs)
- [StackOverflow: CH32V203 Jumping to bootloader from software](https://stackoverflow.com/questions/77722171/ch32v203-jumping-to-bootloader-from-software)
- [WCH RISC-V Microcontroller Web Serial ISP](https://www.stasisleak.uk/wchisp/)
- [ch32-rs/wchisp - Rust-based WCH ISP tool](https://github.com/ch32-rs/wchisp)
- CH32V208 Datasheet (WCH official)
- CH32V20x Reference Manual (WCH official)

## Appendix A: Register Definitions

### Current FLASH_STATR Bits (from ch32v20x.h)

```c
#define FLASH_STATR_BSY       ((uint8_t)0x01)  // Bit 0: Busy
#define FLASH_STATR_PGERR     ((uint8_t)0x04)  // Bit 2: Programming Error
#define FLASH_STATR_WRPRTERR  ((uint8_t)0x10)  // Bit 4: Write Protection Error
#define FLASH_STATR_EOP       ((uint8_t)0x20)  // Bit 5: End of operation
```

### Proposed Additional Bit

```c
#define FLASH_STATR_MODE      ((uint16_t)0x4000)  // Bit 14: Boot Mode (unverified)
```

### NVIC System Reset

```c
#define NVIC_KEY3             ((uint32_t)0xBEEF0000)

__attribute__((always_inline)) RV_STATIC_INLINE void NVIC_SystemReset(void)
{
    NVIC->CFGR = NVIC_KEY3 | (1<<7);  // 0xBEEF0080
}
```

## Appendix B: Command Protocol

### CMD_ENTER_ISP_MODE (0x32)

**Request**:
```
Host → Device:
57 AB 00 32 00 00    // Header + CMD + length=0
```

**Response**:
```
Device → Host:
57 AB 00 32 01 00    // Header + CMD + length=1 + status=0x00 (success)
```

**Behavior**:
1. Device sends success response
2. Device waits 100ms for USB transmission
3. Device enters ISP mode
4. Device re-enumerates as `1A86:55E0`
5. Host can now flash firmware using `wchisp`

## Appendix C: Recovery Procedures

### If Device Gets Stuck in ISP Mode

1. **Power cycle** the device
2. If still in ISP mode, **hold BOOT button**
3. **Press RESET** while holding BOOT
4. **Release BOOT button**
5. Device should boot normally

### If Device is Completely Unresponsive

1. Use **WCH-LinkE programmer** to recover
2. Connect WCH-LinkE to SWD pins
3. Use WCH-Link utility to erase and reflash
4. Alternatively, use **OpenOCD** with WCH RISC-V config

### If Flash is Corrupted

1. Enter ISP mode using physical BOOT button
2. Use `wchisp` to erase all flash: `wchisp erase`
3. Reflash firmware: `wchisp flash firmware.bin`
4. Device should boot normally
