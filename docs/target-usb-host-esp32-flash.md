# Target USB Host Mode - ESP32 Firmware Flash via USB CDC

## Overview

This document describes the plan to enable ESP32 firmware flashing through the KeyMod's Target USB port by switching the CH32V208 USB OTG controller to Host mode.

## Background

The KeyMod device uses the WCH CH32V208 MCU which features a USB OTG controller capable of operating in both Device and Host modes. Currently, the Target USB port (PB6 D+, PB7 D-) operates exclusively in Device mode, emulating a USB HID keyboard/mouse to the target machine.

The goal is to enable runtime switching to Host mode, allowing the CH32V208 to enumerate and communicate with an ESP32 connected to the Target USB port via its native USB interface (USB CDC).

## Hardware Architecture

### CH32V208 USB Controllers

| Controller | Pins | Current Usage | Capability |
|------------|------|---------------|------------|
| USBFS (USBHD) | PB6 (D-), PB7 (D+) | Target port - Device mode | Device + Host (OTG) |
| USBD (USBLIB) | Internal PHY | Host port - Device mode | Device only |

### Target USB Port (USBFS)

- **D+**: PB7
- **D-**: PB6
- **Mode**: USB OTG Full Speed (12 Mbps)
- **Host Mode Register**: `USBFS_UC_HOST_MODE` (bit 7 of `R8_USB_CTRL`)

### ESP32 USB Interface

ESP32-S2/S3/C3/C6 models with native USB support:
- **VID:PID**: `303A:0002` (ESP32-S3 CDC) or custom
- **Interface**: USB CDC-ACM (virtual serial port)
- **Download Mode**: Triggered via GPIO (EN/IO0) or USB command

## Technical Feasibility

### Hardware Support Analysis

The CH32V208 USB OTG controller provides complete support for both Device and Host modes:

| Feature | Register | Status |
|---------|----------|--------|
| Host/Device Mode Switch | `USBFS_UC_HOST_MODE` (0x80) | Supported |
| Host Control | `R8_UHOST_CTRL` | Full register set |
| Host Endpoint Config | `R32_UH_EP_MOD` | TX/RX enable |
| Host Transfer Control | `R8_UH_TX_CTRL`, `R8_UH_RX_CTRL` | Auto-toggle, RES |
| Device Detection | `USBFS_UH_PORT_EN`, `USBFS_UIF_DETECT` | Interrupt-driven |
| SOF Generation | `USBFS_UH_SOF_EN`, `USBFS_UIF_HST_SOF` | 1ms timer |
| OTG Control | `R32_USB_OTG_CR` | Session/VBUS/ID |

### Mode Switching Mechanism

```c
// Switch to Host Mode
USBFS->USB_CTRL |= USBFS_UC_HOST_MODE;

// Configure Host registers
USBFS->UHOST_CTRL = USBFS_UH_PORT_EN;
USBFS->UH_EP_MOD = USBFS_UH_EP_TX_EN | USBFS_UH_EP_RX_EN;

// Switch to Device Mode
USBFS->USB_CTRL &= ~USBFS_UC_HOST_MODE;

// Configure Device registers
USBFS->UDEV_CTRL = USBFS_UD_PORT_EN;
USBFS->UEP4_1_MOD = USBFS_UEP1_TX_EN | USBFS_UEP4_TX_EN;
```

### Runtime Switching Feasibility

| Aspect | Feasibility | Notes |
|--------|-------------|-------|
| Register Switching | Yes | Single bit in USB_CTRL |
| Interrupt Reconfiguration | Yes | Different ISR for Host/Device |
| Endpoint Buffer Reuse | Yes | Same RAM, different mapping |
| Bus Reset | Yes | Required for device enumeration |
| VBUS Generation | TBD | May need external 5V source |
| Hot Switch | No | Requires USB re-enumeration (~100ms) |

## Implementation Plan

### Phase 1: USB OTG Mode Switch Framework

**Duration**: 1 week

**Tasks**:
1. Create USB OTG state machine (Device / Host / Switching)
2. Implement mode switch function with proper register save/restore
3. Add USB Host mode initialization (SOF, port enable, detection)
4. Implement Host mode interrupt handler
5. Add device detection and enumeration trigger

**Key Files**:
- `APP/include/usb_otg_mode.h` - New header for OTG mode definitions
- `APP/usb_otg_mode.c` - New implementation file
- `APP/ch32v20x_usbfs_device.c` - Extend with Host mode support

**Deliverables**:
- Ability to switch between Device and Host modes at runtime
- Host mode enumeration of USB Device Descriptor
- Clean state transitions with proper cleanup

### Phase 2: USB Host CDC-ACM Class Driver

**Duration**: 1 week

**Tasks**:
1. Implement USB Host CDC-ACM device recognition
2. Parse CDC functional descriptors
3. Implement `SET_LINE_CODING` and `GET_LINE_CODING` requests
4. Implement bulk IN/OUT transfer functions
5. Add data receive callback mechanism

**Key Files**:
- `APP/include/usb_host_cdc.h` - CDC Host class header
- `APP/usb_host_cdc.c` - CDC Host class implementation

**Deliverables**:
- Successful enumeration of ESP32 USB CDC device
- Bidirectional bulk data transfer capability
- Line coding configuration (baud rate, data bits, etc.)

### Phase 3: ESP32 Flash Protocol (esptool)

**Duration**: 1 week

**Tasks**:
1. Implement SLIP frame encoding/decoding
2. Implement SYNC command for initial handshake
3. Implement FLASH_BEGIN, FLASH_DATA, FLASH_END commands
4. Implement checksum calculation
5. Add flash size/baud rate configuration
6. Implement response parsing and error handling

**Key Files**:
- `APP/include/esp32_flash.h` - ESP32 flash protocol header
- `APP/esp32_flash.c` - Flash protocol implementation

**Reference**: [esptool.py protocol documentation](https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/serial-protocol.html)

**Deliverables**:
- Complete esptool protocol implementation
- Flash read/write capability via USB CDC
- Error recovery and retry mechanism

### Phase 4: CH9329 Command Integration

**Duration**: 0.5 week

**Tasks**:
1. Add `CMD_ESP32_FLASH` (0x1B) command definition
2. Implement command parser for flash operations
3. Add data buffer management for flash payload
4. Implement response mechanism (progress/status)
5. Add abort/reset command handling

**Key Files**:
- `APP/usbd_compostie_km.c` - Extend CH9329 command handler
- `APP/include/usbd_composite_km.h` - Add command definitions

**Command Structure**:
```
Request:  57 AB <addr> 1B <len> <sub_cmd> <data...> <checksum>
Response: 57 AB <addr> 9B <len> <status> <data...> <checksum>

Sub-commands:
  0x00 - Enter flash mode (switch to Host, enumerate ESP32)
  0x01 - Exit flash mode (switch back to Device)
  0x02 - Send SLIP frame to ESP32
  0x03 - Get flash status/progress
  0x04 - Abort flash operation
```

**Deliverables**:
- Host PC can initiate ESP32 flash via CH9329 protocol
- Real-time progress reporting
- Clean error handling

### Phase 5: Host-Side Tool Integration

**Duration**: 0.5 week

**Tasks**:
1. Create Python wrapper for CH9329 flash commands
2. Modify esptool.py to use CH9329 transport (or create adapter)
3. Test end-to-end flash workflow
4. Add documentation for host-side usage

**Key Files**:
- `tools/esp32_flash_ch9329.py` - Host-side flash tool

**Deliverables**:
- Working host-side tool for ESP32 flash via KeyMod
- Usage documentation
- Example scripts

## Technical Details

### USB Host Mode Register Configuration

```c
void USB_Host_ModeInit(void)
{
    // 1. Switch to Host mode
    USBFS->USB_CTRL |= USBFS_UC_HOST_MODE;
    
    // 2. Configure Host control
    USBFS->UHOST_CTRL = USBFS_UH_PORT_EN;  // Enable port
    
    // 3. Configure endpoints
    USBFS->UH_EP_MOD = USBFS_UH_EP_TX_EN | USBFS_UH_EP_RX_EN;
    
    // 4. Set DMA addresses
    USBFS->UH_TX_DMA = (uint32_t)Host_Tx_Buf;
    USBFS->UH_RX_DMA = (uint32_t)Host_Rx_Buf;
    
    // 5. Enable Host interrupts
    USBFS->USB_INT_EN = USBFS_UIE_TRANSFER | USBFS_UIE_DETECT | USBFS_UIE_HST_SOF;
    
    // 6. Enable SOF generation
    USBFS->UH_SETUP = USBFS_UH_SOF_EN;
}
```

### USB Host Interrupt Handler

```c
void USBFS_IRQHandler(void)
{
    uint8_t int_fg = USBFS->USB_INT_FG;
    
    if (int_fg & USBFS_UIF_DETECT) {
        // Device attach/detach detection
        USB_Host_DetectDevice();
        USBFS->USB_INT_FG = USBFS_UIF_DETECT;
    }
    
    if (int_fg & USBFS_UIF_TRANSFER) {
        // Transfer completion
        USB_Host_TransferComplete();
        USBFS->USB_INT_FG = USBFS_UIF_TRANSFER;
    }
    
    if (int_fg & USBFS_UIF_HST_SOF) {
        // SOF timer interrupt
        USB_Host_SOFHandler();
        USBFS->USB_INT_FG = USBFS_UIF_HST_SOF;
    }
}
```

### ESP32 USB CDC Enumeration

```c
void USB_Host_EnumESP32(void)
{
    // 1. Reset device
    USBFS->UHOST_CTRL |= USBFS_UH_BUS_RESET;
    Delay_Ms(100);
    USBFS->UHOST_CTRL &= ~USBFS_UH_BUS_RESET;
    Delay_Ms(100);
    
    // 2. Get Device Descriptor
    USB_Host_GetDescriptor(USB_DESCR_TYP_DEVICE, 0, 18);
    
    // 3. Set Address
    USB_Host_SetAddress(1);
    
    // 4. Get Configuration Descriptor
    USB_Host_GetDescriptor(USB_DESCR_TYP_CONFIG, 0, 9);
    USB_Host_GetDescriptor(USB_DESCR_TYP_CONFIG, 0, total_len);
    
    // 5. Parse CDC descriptors
    // Find CDC Communication Interface and Data Interface
    // Find CDC functional descriptors (Header, Call Management, ACM, Union)
    // Find endpoint descriptors (Notification, Bulk IN, Bulk OUT)
    
    // 6. Set Configuration
    USB_Host_SetConfiguration(1);
    
    // 7. Set Line Coding (115200, 8N1)
    CDC_SetLineCoding(115200, 0, 0, 8);
}
```

### esptool SLIP Protocol

```c
// SLIP special characters
#define SLIP_END      0xC0
#define SLIP_ESC      0xDB
#define SLIP_ESC_END  0xDC
#define SLIP_ESC_ESC  0xDD

// SLIP encode
int SLIP_Encode(uint8_t *in, int in_len, uint8_t *out, int out_max)
{
    int out_len = 0;
    out[out_len++] = SLIP_END;
    
    for (int i = 0; i < in_len; i++) {
        if (out_len >= out_max - 2) return -1;
        
        if (in[i] == SLIP_END) {
            out[out_len++] = SLIP_ESC;
            out[out_len++] = SLIP_ESC_END;
        } else if (in[i] == SLIP_ESC) {
            out[out_len++] = SLIP_ESC;
            out[out_len++] = SLIP_ESC_ESC;
        } else {
            out[out_len++] = in[i];
        }
    }
    
    out[out_len++] = SLIP_END;
    return out_len;
}

// ESP32 flash command structure
typedef struct {
    uint8_t direction;  // 0x00 = request, 0x01 = response
    uint8_t command;    // Command code
    uint16_t size;      // Data size
    uint32_t checksum;  // Checksum
    uint8_t data[];     // Variable length data
} esp_command_t;
```

## Data Flow

```
┌─────────────────────────────────────────────────────────────────────┐
│                          PC / Mac (Host)                            │
│                                                                     │
│  esptool.py → Modified transport → CH9329 frames                   │
└───────────────────────────┬─────────────────────────────────────────┘
                            │ Host USB (USBD CDC ACM)
                            ↓
┌─────────────────────────────────────────────────────────────────────┐
│                        KeyMod (CH32V208)                            │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                    Firmware State Machine                    │  │
│  │                                                              │  │
│  │  [Device HID Mode] ──cmd──> [Switching] ──> [Host CDC Mode] │  │
│  │        ↑                                    │               │  │
│  │        └──────────────done──────────────────┘               │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  Host USB RX → CH9329 Parse → esptool SLIP → USB Host TX          │
│  Host USB TX ← CH9329 Resp  ← esptool Resp ← USB Host RX          │
└───────────────────────────┬─────────────────────────────────────────┘
                            │ Target USB (PB6/PB7) - Host Mode
                            ↓
┌─────────────────────────────────────────────────────────────────────┐
│                          ESP32 (S2/S3/C3)                           │
│                                                                     │
│  USB CDC Device (GPIO19 D-, GPIO20 D+)                             │
│         ↓                                                           │
│  UART→USB Bridge (ROM bootloader or firmware)                      │
│         ↓                                                           │
│  Flash Programming                                                  │
└─────────────────────────────────────────────────────────────────────┘
```

## Risks and Mitigations

| Risk | Impact | Probability | Mitigation |
|------|--------|-------------|------------|
| VBUS not provided by hardware | High | Medium | Add external 5V source or USB power switch |
| USB Host stack complexity | Medium | High | Reference WCH USB Host examples, use existing TinyUSB if possible |
| ESP32 USB not in download mode | Medium | Medium | Require manual BOOT pin activation initially |
| Timing issues with SOF | Low | Low | Use hardware SOF timer, test extensively |
| Memory constraints | Medium | Low | Optimize buffers, verify RAM usage |
| HID interruption during flash | Low | High | Document as expected behavior, add user notification |

## Testing Plan

### Phase 1 Testing
- [ ] Mode switch from Device to Host
- [ ] Mode switch from Host to Device
- [ ] Multiple switch cycles (stability)
- [ ] Interrupt handling in both modes

### Phase 2 Testing
- [ ] Enumerate standard USB CDC device
- [ ] Enumerate ESP32-S3 USB CDC
- [ ] Bulk IN/OUT data transfer
- [ ] Line coding configuration

### Phase 3 Testing
- [ ] SLIP encode/decode
- [ ] SYNC command with ESP32
- [ ] FLASH_BEGIN command
- [ ] FLASH_DATA command (small packet)
- [ ] FLASH_DATA command (full firmware)
- [ ] FLASH_END command
- [ ] Checksum verification
- [ ] Error recovery

### Phase 4 Testing
- [ ] CMD_ESP32_FLASH command parsing
- [ ] End-to-end flash via CH9329
- [ ] Progress reporting
- [ ] Abort handling

### Phase 5 Testing
- [ ] Host tool integration
- [ ] Full firmware flash (1MB+)
- [ ] Multiple ESP32 models
- [ ] Stress testing (100+ flash cycles)

## Timeline

| Phase | Duration | Dependencies |
|-------|----------|--------------|
| Phase 1: USB OTG Framework | 1 week | None |
| Phase 2: USB Host CDC | 1 week | Phase 1 |
| Phase 3: esptool Protocol | 1 week | Phase 2 |
| Phase 4: CH9329 Integration | 0.5 week | Phase 3 |
| Phase 5: Host Tool | 0.5 week | Phase 4 |
| **Total** | **4 weeks** | |

## Conclusion

The proposed implementation is technically feasible based on the CH32V208's USB OTG controller capabilities. The key enabling feature is the `USBFS_UC_HOST_MODE` register bit that allows runtime switching between Device and Host modes.

The main challenges are:
1. Implementing a complete USB Host stack within the firmware
2. Handling the ESP32 USB CDC protocol correctly
3. Integrating with the existing CH9329 command infrastructure

With a 4-week timeline and proper testing at each phase, this feature can be successfully implemented to enable ESP32 firmware flashing through the KeyMod's Target USB port.

## References

- [CH32V208 Datasheet](https://www.wch-ic.com/products/CH32V208.html)
- [CH32V208 USB Application Note](https://www.wch-ic.com/downloads/CH32V208EVT_ZIP.html)
- [esptool.py Serial Protocol](https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/serial-protocol.html)
- [USB CDC-ACM Class Specification](https://www.usb.org/document-library/class-definitions-communication-devices-12)
- [ESP32-S3 USB Peripheral](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_device.html)
