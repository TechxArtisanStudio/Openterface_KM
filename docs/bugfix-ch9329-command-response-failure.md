# CH9329 command response failure analysis report

**Date**: 2026-09-11  
**Firmware build**: openterface-km-firmware-kvmgo  
**Problem**: the device does not respond to CH9329 commands (Get Info 0x01 and so on)

## Problem description

After flashing the firmware the device enumerates correctly as a USB CDC ACM
device, but does not respond to any CH9329 command. Mouse and keyboard data work
normally, which shows the USB connection itself is fine.

## Troubleshooting process

### First attempt (failed)

**Hypothesis**: endpoint state toggle conflict

I assumed the toggle behaviour of `SetEPTxStatus` was leaving the endpoint state
inconsistent, and changed the following code:

1. **usb_endp.c** - use a direct register write instead of `SetEPTxStatus`
   ```c
   // before
   SetEPTxStatus(ENDP3, EP_TX_VALID);
   
   // after
   USBFSD->UEP3_TX_CTRL = (USBFSD->UEP3_TX_CTRL & ~USBFS_UEP_T_RES_MASK) | USBFS_UEP_T_RES_ACK;
   ```

2. **usb_prop.c** - enable EP3 immediately in HID_ACM mode
   ```c
   if (USB_ModeConfig_ActiveModeHasHostCdcAcm() && !USB_ModeConfig_ActiveModeBridgeAcm())
   {
       SetEPTxStatus(ENDP3, EP_TX_NAK);
   }
   ```

3. **km_ring.c** - add a double busy-flag check
   ```c
   if (USBFS_Endp_Busy[DEF_UEP3] == 0 && 
       USBD_ENDPx_DataUp(ENDP3, pkt->data, pkt->len) == 0)
   ```

**Result**: the device still did not respond to CH9329 commands

### Second attempt (failed)

**Hypothesis**: a conditional-compilation error

I assumed the conditional compilation around `EP5_OUT_Callback` did not cover
HID_ACM mode, and added a `USB_WORK_MODE_HAS_HOST_CDC_ACM` condition.

**Result**: the device still did not respond

### Root cause analysis

The user pointed out that the merge commit `bf0a4b5` worked. Comparing the two
revealed these differences:

**Key differences**:

1. **USB Endpoint Busy Timeout Recovery mechanism**
   - the current code (commit c9f1cb8) added an endpoint busy-timeout recovery mechanism
   - the merge commit version has no such mechanism
   - that mechanism can interfere with the endpoint state under some conditions

2. **When EP3 is initialised**
   - merge commit version: EP3 is initialised to `EP_TX_DIS` in USBD_Reset
   - and only enabled as `EP_TX_NAK` when SET_LINE_CODING is handled
   - modified version: EP3 is enabled directly in USBD_Reset

3. **Data flow path**
   - in HID_ACM mode `EP5_OUT_Callback` takes the `#else` branch
   - the data goes into `UART2_Tx_Buf`
   - it is processed through `USB_DataRx_To_KMHandle()`
   - which calls `CH9329_RxBuffer_Add()` to parse the command
   - the response is sent through EP3

## Root cause

**The USB Endpoint Busy Timeout Recovery mechanism interfered with normal endpoint
state management**

That mechanism added the following in `ch32v20x_usbfs_device.c`:
- `USBFS_Endp_Busy_StartTime[]` - records when an endpoint became busy
- `USBFS_CheckEndpointBusyTimeout()` - checks for and recovers timed-out endpoints

`USBFS_CheckEndpointBusyTimeout()` is called from the main loop and can clear a
busy flag at the wrong moment, leaving the endpoint state inconsistent.

## Solution

Revert to the merge commit `bf0a4b5` version and remove the USB Endpoint Busy
Timeout Recovery mechanism:

1. restore `usb_endp.c` to the original version
2. restore `usb_prop.c` to the original version
3. remove the code related to `USBFS_CheckEndpointBusyTimeout()`

## Verification result

After reverting to the merge commit version, the device responds to CH9329
commands normally.

## Lessons learned

1. **Do not over-optimise**: the existing endpoint state management was already stable enough; it did not need an extra timeout-recovery mechanism

2. **Understand the existing code**: before changing anything, fully understand how the existing code and its data flow work

3. **Change incrementally**: change one aspect at a time rather than several places at once

4. **Refer to a working version**: when something breaks, first find a known-good version to compare against

## Related files

- `APP/USBLIB/CONFIG/usb_endp.c` - endpoint handling
- `APP/USBLIB/CONFIG/usb_prop.c` - USB property configuration
- `APP/ch32v20x_usbfs_device.c` - USB device driver
- `APP/usbd_compostie_km.c` - CH9329 command handling
- `APP/km_ring.c` - keyboard/mouse data queue management

## Commit information

- **Problem introduced**: `c9f1cb8` - feat: implement USB endpoint busy timeout recovery mechanism
- **Working version**: `bf0a4b5` - Merge branch 'fix/p0-stability-issues'
