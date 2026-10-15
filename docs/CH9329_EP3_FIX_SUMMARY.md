# CH9329 EP3 Enable Fix Summary

## Problem Description

The CH9329 USB-to-serial protocol commands sent via the USB serial port (CDC ACM) were not receiving responses. Specifically, the Get Info command (0x01) would be sent but no response would be received from the device.

## Root Cause

The EP3 IN endpoint (used for sending CH9329 responses) was stuck in a disabled state:

1. During USB reset (`USBD_Reset()`), EP3 IN is set to `EP_TX_DIS` (disabled)
2. The original code only enabled EP3 after receiving a `CDC_SET_LINE_CODING` control request
3. If the host doesn't send `CDC_SET_LINE_CODING` (or sends it after other operations), EP3 remains disabled
4. With EP3 disabled, `USBD_ENDPx_DataUp()` cannot send any data, causing CH9329 responses to be silently dropped

## The Fix

### Fix 1: Enable EP3 in USBD_SetConfiguration() (ESSENTIAL)

**File:** `APP/USBLIB/CONFIG/usb_prop.c`

```c
void USBD_SetConfiguration(void)
{
  DEVICE_INFO *pInfo = &Device_Info;
  if (pInfo->Current_Configuration != 0)
  {
    bDeviceState = CONFIGURED;
    
    // NEW: Enable EP3 IN immediately when USB is configured
    if (USB_ModeConfig_ActiveModeHasHostCdcAcm())
    {
      _ClearDTOG_TX(ENDP3);
      SetEPTxStatus(ENDP3, EP_TX_NAK);
      USB_DEBUG("USBD_SetConfiguration: EP3 enabled to NAK\n");
    }
  }
}
```

**Why this works:**
- `USBD_SetConfiguration()` is called when the host sets the device configuration (standard USB enumeration step)
- This happens before any `CDC_SET_LINE_CODING` requests
- EP3 is now enabled as soon as USB is fully configured
- `EP_TX_NAK` state means the endpoint is ready to send data when the host requests it

### Fix 2: EP3 Status Check in USB_SendFromQueues() (DEFENSIVE)

**File:** `APP/km_ring.c`

```c
// In USB_SendFromQueues(), before sending response queue data:
uint16_t ep3_status = _GetEPTxStatus(ENDP3);
if (ep3_status == EP_TX_DIS) {
  USB_DEBUG("EP3 is disabled, enabling\n");
  if (USB_ModeConfig_ActiveModeHasHostCdcAcm()) {
    _ClearDTOG_TX(ENDP3);
    SetEPTxStatus(ENDP3, EP_TX_NAK);
    USB_DEBUG("EP3 enabled to NAK\n");
  }
}
```

**Why this is included:**
- Provides a safety net in case EP3 gets disabled for any reason during operation
- Not strictly necessary with Fix 1 working correctly, but adds robustness
- Helps diagnose if EP3 is being unexpectedly disabled

### Fix 3: Wait for USB Enumeration (RECOMMENDED)

**File:** `APP/peripheral_main.c`

```c
/* Wait for USB to be fully enumerated before sending startup response */
USB_DEBUG("Waiting for USB enumeration...\n");
for (int i = 0; i < 50; i++) {  /* Wait up to 5 seconds */
  if (bDeviceState == CONFIGURED) {
    USB_DEBUG("USB configured after %d ms\n", i * 100);
    break;
  }
  Delay_Ms(100);
}
```

**Why this is included:**
- The startup code calls `CH9329_Cmd_GetInfo_Reply(00)` to send an initial response
- Without waiting, this response could be sent before USB is fully configured
- Ensures the startup response is actually delivered to the host

## Test Results

### Before Fix
- CH9329 commands sent but no responses received
- EP3 IN endpoint remained in `EP_TX_DIS` state
- Device appeared non-functional for CH9329 protocol

### After Fix
- ✓ Get Info (0x01) command works correctly
- ✓ Response received: `57ab008108010100060000000093`
- ✓ Checksum validation passes
- ✓ Serial port works at 9600 baud
- ✓ pyusb direct EP3 access works at any baud rate

### Test Command
```python
# Send Get Info command
cmd = bytearray([0x57, 0xAB, 0x00, 0x01, 0x00, 0x03])
dev.write(0x05, cmd, timeout=1000)

# Read response from EP3 IN
data = dev.read(0x83, 64, timeout=2000)
# Result: 57ab008108010100060000000093 (14 bytes)
```

## Fix Necessity Analysis

| Fix | Necessity | Reason |
|-----|-----------|--------|
| Fix 1: EP3 enable in USBD_SetConfiguration | **ESSENTIAL** | Core fix - without this, EP3 remains disabled |
| Fix 2: EP3 status check in USB_SendFromQueues | DEFENSIVE | Safety net, rarely triggered with Fix 1 |
| Fix 3: USB enumeration wait | RECOMMENDED | Ensures startup response is delivered |

## Files Modified

1. `APP/USBLIB/CONFIG/usb_prop.c` - USBD_SetConfiguration()
2. `APP/km_ring.c` - USB_SendFromQueues()
3. `APP/peripheral_main.c` - main()

## Remaining Issues

Two additional issues were discovered during testing (see NEW_ISSUES.md):

1. **Serial port multi-baud rate problem** - Serial port only works at 9600 baud, not at other rates
2. **Large response packet fragmentation** - Commands with responses > 16 bytes (e.g., Get Para Cfg) fail silently

These are separate from the EP3 enable issue and require additional fixes.

---

*Document created: 2026-09-14*  
*Based on: COMPREHENSIVE_TEST_REPORT.md*  
*Fix verified with: test_comprehensive.py*
