# CDC NCM Descriptor Debugging Notes

## Status: RESOLVED ✅

The CDC NCM descriptor issue has been successfully resolved as of Nov 19, 2026.

## Problem Summary

The CDC NCM driver failed to bind with "NCM or ECM functional descriptors missing" despite descriptors being present in lsusb output.

## Root Cause

**Incorrect NCM Functional Descriptor subtype!**

The firmware was using subtype `0x0D` (Ethernet Networking) instead of `0x1A` (USB_CDC_NCM_TYPE) for the NCM Functional Descriptor.

### Linux Kernel Requirements

The Linux `cdc_ncm` driver (`drivers/net/usb/cdc_ncm.c`) searches for:
- **NCM Functional Descriptor**: subtype `0x1A` (USB_CDC_NCM_TYPE)
- **ECM Functional Descriptor**: subtype `0x0F` (USB_CDC_ECM_TYPE)

Using subtype `0x0D` caused the driver to fail with:
```
cdc_ncm 1-6:1.4: NCM or ECM functional descriptors missing
cdc_ncm 1-6:1.4: bind() failure
```

## Solution

### 1. Fix NCM Functional Descriptor Subtype

Changed in `APP/usbd_desc.c`:

```c
// WRONG - subtype 0x0D (Ethernet Networking)
0x08, 0x24, 0x0D, 0x00, 0x00, 0x05, 0x10, 0x00

// CORRECT - subtype 0x1A (USB_CDC_NCM_TYPE)
0x08, 0x24, 0x1A, 0x00, 0x00, 0x05, 0x10, 0x00
```

### 2. Implement GET_NTB_PARAMETERS Request

Added handler in `APP/ch32v20x_usbfs_device.c`:

```c
#define USBFS_CDC_NCM_REQ_GET_NTB_PARAMS  0x80u

case USBFS_CDC_NCM_REQ_GET_NTB_PARAMS:
    if (USB_ModeConfig_ActiveModeBridgeNcm() &&
        ( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_INTERF ) &&
        ( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == USBFS_CDC_NCM_IF_COMM ) )
    {
        memcpy( USBFS_EP0_Buf, USBFS_CDC_NCM_NtbParams, sizeof( USBFS_CDC_NCM_NtbParams ) );
        len = USBFS_SetupReqLen;
    }
    break;
```

NTB Parameters structure (28 bytes per CDC NCM 1.0 spec Table 6.3):
```c
static uint8_t USBFS_CDC_NCM_NtbParams[28] = {
    0x1C, 0x00,  /* wLength: 28 bytes */
    0x01, 0x00,  /* bmNtbFormatsSupported: 16-bit NTB only */
    0x00, 0x05, 0x00, 0x00,  /* dwNtbInMaxSize: 1280 bytes */
    0x04, 0x00,  /* wNdpInDivisor: 4 */
    0x00, 0x00,  /* wNdpInPayloadRemainder: 0 */
    0x10, 0x00,  /* wNdpInMaxSize: 16 bytes */
    0x00, 0x05, 0x00, 0x00,  /* dwNtbOutMaxSize: 1280 bytes */
    0x04, 0x00,  /* wNdpOutDivisor: 4 */
    0x00, 0x00,  /* wNdpOutPayloadRemainder: 0 */
    0x10, 0x00,  /* wNdpOutMaxSize: 16 bytes */
};
```

### 3. Add MAC Address String Descriptor

The kernel requires a MAC address string descriptor. Added:

1. **String descriptor** in `APP/usbd_desc.c`:
```c
const uint8_t MyNcmMacInfo[ ] =
{
    0x1A, 0x03,  // bLength, bDescriptorType
    '0', 0, '2', 0, '0', 0, '0', 0, '0', 0, '0', 0,
    '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '1', 0
};
```

2. **Ethernet Networking descriptor** updated to reference string index 6:
```c
0x0D, 0x24, 0x0F, 0x06,  // iMACAddress=6 (NCM MAC string)
```

3. **String descriptor handler** in `APP/ch32v20x_usbfs_device.c`:
```c
case 0x06:
    if( USB_ModeConfig_ActiveModeBridgeNcm() )
    {
        pUSBFS_Descr = MyNcmMacInfo;
        len = 0x1A;
    }
    break;
```

4. **GET_NET_ADDRESS request** handler:
```c
case USBFS_CDC_NCM_REQ_GET_NET_ADDR:
    memcpy( USBFS_EP0_Buf, USBFS_CDC_NCM_MacAddr, sizeof( USBFS_CDC_NCM_MacAddr ) );
    len = USBFS_SetupReqLen;
    break;
```

## Final Descriptor Structure

```c
/* IAD: CDC NCM (IF4=comm + IF5=data) */
0x08, 0x0B, 0x04, 0x02, 0x02, 0x0D, 0x00, 0x00,

/* Interface Descriptor: CDC NCM Communication (IF4) */
0x09, 0x04, 0x04, 0x00, 0x01, 0x02, 0x0D, 0x00, 0x00,

/* CDC NCM Functional Descriptors */
0x05, 0x24, 0x00, 0x10, 0x01,                           // Header, CDC 1.10
0x05, 0x24, 0x06, 0x04, 0x05,                           // Union: control IF4, data IF5
0x0D, 0x24, 0x0F, 0x06,                                 // Ethernet Networking, iMACAddress=6
0x00, 0x00, 0x00, 0x00,                                 // bmEthernetStatistics
0xEA, 0x05,                                             // wMaxSegmentSize: 1514
0x00, 0x00,                                             // wNumberMCFilters: none
0x00,                                                   // bNumberPowerFilters

/* NCM Functional Descriptor: 8 bytes */
0x08, 0x24, 0x1A,                                       // NCM Functional (subtype 0x1A!)
0x00,                                                   // bmNetworkCapabilities
0x00, 0x05,                                             // wNtbInMaxSize: 1280 bytes
0x10, 0x00,                                             // wNdpInMaxSize: 16 bytes

/* Endpoint Descriptor: CDC NCM notification EP6 IN */
0x07, 0x05, 0x86, 0x03, 0x10, 0x00, 0x10,

/* Interface Descriptor: CDC NCM Data (IF5 alt 0, idle) */
0x09, 0x04, 0x05, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,

/* Interface Descriptor: CDC NCM Data (IF5 alt 1, active) */
0x09, 0x04, 0x05, 0x01, 0x02, 0x0A, 0x00, 0x00, 0x00,

/* Endpoint Descriptor: CDC NCM data OUT EP5 */
0x07, 0x05, 0x05, 0x02, 0x40, 0x00, 0x00,

/* Endpoint Descriptor: CDC NCM data IN EP7 */
0x07, 0x05, 0x87, 0x02, 0x40, 0x00, 0x00,
```

## Verification Results (Nov 19, 2026)

After applying all fixes:

```
[  282.790633] cdc_ncm 1-6:1.4: MAC-Address: 02:00:00:00:00:01
[  282.790656] cdc_ncm 1-6:1.4: dwNtbInMaxSize=1280 is too small. Using 2048
[  282.790678] cdc_ncm 1-6:1.4: setting rx_max = 2048
[  282.795123] cdc_ncm 1-6:1.4: setting tx_max = 16384
[  282.795456] cdc_ncm 1-6:1.4 usb0: register 'cdc_ncm' at usb-0000:00:15.0-6, CDC NCM (NO ZLP), 02:00:00:00:00:01
[  282.795789] cdc_ncm 1-6:1.4 enp0s21f0u6i4: renamed from usb0
```

**✅ Linux device recognition:**
- cdc_ncm driver bound to interfaces 4 and 5
- Network interface created: `enp0s21f0u6i4`
- MAC address: `02:00:00:00:00:01`
- NTB parameters accepted
- Interface state: UP (NO-CARRIER until data path tested)

## Key Learnings

1. **NCM Functional Descriptor subtype must be 0x1A**, not 0x0D
   - 0x0D = Ethernet Networking Functional Descriptor
   - 0x1A = NCM Functional Descriptor (USB_CDC_NCM_TYPE)

2. **GET_NTB_PARAMETERS (0x80) is mandatory** - kernel requires this during bind

3. **MAC address must be provided** either via:
   - String descriptor (iMACAddress field in Ethernet Networking descriptor)
   - GET_NET_ADDRESS request (0x81)

4. **NTB Parameters structure is 28 bytes** per CDC NCM 1.0 spec Table 6.3

## Build and Flash

```bash
# Set compiler path
export PATH="/home/bbot/tools/MRS-linux-x64/resources/app/resources/linux/components/WCH/Toolchain/RISC-V Embedded GCC/bin:$PATH"

# Rebuild firmware with NCM mode
./build.sh --make-arg USB_WORK_MODE=hid_ncm_bridge keymod-plus

# Flash firmware via ISP
python3 tools/ctrl_board.py flash output/openterface-km-firmware-keymod-plus.bin
```

## Code References

- NCM descriptor block: `APP/usbd_desc.c:USBFS_RuntimeNcmBlock`
- NCM class requests: `APP/ch32v20x_usbfs_device.c:USBFS_CDC_NCM_REQ_*`
- NTB parameters: `APP/ch32v20x_usbfs_device.c:USBFS_CDC_NCM_NtbParams`
- MAC string descriptor: `APP/usbd_desc.c:MyNcmMacInfo`
