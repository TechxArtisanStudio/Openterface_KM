# Firmware DHCP server debugging guide

> **Superseded.** See §4 and §10 of [ncm-bridge.md](ncm-bridge.md) for the current DHCP behaviour and verification steps.

## Problem description

### Symptoms
- A bot device connects to the Openterface Target port over Ethernet
- The bot device is configured for DHCP (automatic IP configuration)
- The bot device cannot obtain an IP address and logs "no lease"
- Manually configuring the static IP 192.168.11.2 makes communication work

### Expected behaviour
The firmware should implement a DHCP server that automatically assigns an IP
address to the Target device:
1. The bot device sends DHCP Discover
2. The Openterface firmware replies with DHCP Offer (IP: 192.168.11.2)
3. The bot device sends DHCP Request
4. The Openterface firmware replies with DHCP ACK

### Actual behaviour
- The bot device sends DHCP Discover
- The firmware does not reply with a DHCP Offer
- The bot device times out

## Code analysis

### Key files

#### 1. APP/ecm_bridge.c
- **handle_target_dhcp_packet()** (lines 705-1100)
  - handles DHCP Discover/Request from the Target PC
  - assigns the IP 192.168.11.2
  - builds the DHCP Offer/ACK response

- **handle_target_frame()** (lines 1763-1780)
  - handles Ethernet frames received from the Target side
  - calls handle_target_dhcp_packet() to handle DHCP requests

- **ECM_Bridge_TargetRxDone()** (lines 1754-1761)
  - Target-side receive-complete callback
  - passes the data to handle_target_frame()

#### 2. APP/include/usb_work_mode.h
- **USB_WORK_MODE_COMPILES_TARGET_CDC_ECM** (line 56)
  - controls whether the Target CDC ECM function is compiled
  - currently set to: `(USB_WORK_MODE_HAS_TARGET_CDC_ECM || USB_WORK_MODE_RUNTIME_PROFILES)`
  - for USB_WORK_MODE=6 (HID_ACM_ECM) this macro should be true

#### 3. APP/include/ecm_ip_config.h
- **ECM_IP_CONFIG_DEFAULT_TARGET_HOST_IP** (line 32)
  - default: { 192, 168, 11, 2 }
  - this is the IP address assigned to the Target PC

### Data flow

```
Bot device (DHCP Discover)
    ↓
Target port (physical Ethernet)
    ↓
USBFS device stack (EP6 OUT)
    ↓
ECM_Bridge_TargetRxDone()
    ↓
handle_target_frame()
    ↓
handle_target_dhcp_packet()
    ↓
build the DHCP Offer
    ↓
ECM_Bridge_SendRawFrame() or similar
    ↓
USBFS device stack (EP7 IN)
    ↓
Target port
    ↓
Bot device
```

## Debugging steps

### Step 1: verify the build configuration

Check whether the firmware actually compiled the Target CDC ECM function:

```bash
# check the USB_WORK_MODE used at build time
grep -n "USB_WORK_MODE" build/makefile

# check whether USB_WORK_MODE_COMPILES_TARGET_CDC_ECM is defined
grep -n "USB_WORK_MODE_COMPILES_TARGET_CDC_ECM" APP/include/usb_work_mode.h

# check the value actually compiled in
# add debug code in APP/ecm_bridge.c:
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
    USB_DEBUG("Target CDC ECM: ENABLED\r\n");
#else
    USB_DEBUG("Target CDC ECM: DISABLED\r\n");
#endif
```

**Expected output**: "Target CDC ECM: ENABLED"

### Step 2: verify data reception

Check whether the Target side receives the DHCP request correctly:

```c
// add to ECM_Bridge_TargetRxDone() in APP/ecm_bridge.c:
void ECM_Bridge_TargetRxDone(const uint8_t *data, uint16_t len)
{
    USB_DEBUG("Target RX: len=%d\r\n", len);
    if (len >= 14) {
        USB_DEBUG("  Dst MAC: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                  data[0], data[1], data[2], data[3], data[4], data[5]);
        USB_DEBUG("  Src MAC: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                  data[6], data[7], data[8], data[9], data[10], data[11]);
        USB_DEBUG("  EthType: 0x%02x%02x\r\n", data[12], data[13]);
    }
    
    // ... existing code
}
```

**Expected output**: the DHCP request packets should appear (EthType: 0x0800, protocol: UDP, port: 67)

### Step 3: verify the DHCP handler

Check whether handle_target_dhcp_packet() is called:

```c
// change handle_target_frame() in APP/ecm_bridge.c:
static uint8_t handle_target_frame(const uint8_t *frame, uint16_t frame_len)
{
    USB_DEBUG("Target frame: len=%d\r\n", frame_len);
    
    /* Handle DHCP requests from Target PC first */
    if (handle_target_dhcp_packet(frame, frame_len)) {
        USB_DEBUG("DHCP response queued\r\n");
        return 1;
    }
    
    // ... existing code
}
```

**Expected output**: "DHCP response queued" should appear

### Step 4: verify DHCP request parsing

Check whether the DHCP request is parsed correctly:

```c
// add to handle_target_dhcp_packet() in APP/ecm_bridge.c:
static uint8_t handle_target_dhcp_packet(const uint8_t *frame, uint16_t frame_len)
{
    USB_DEBUG("DHCP handler entered, frame_len=%d\r\n", frame_len);
    
    // after parsing the Ethernet header:
    USB_DEBUG("  EthType: 0x%04X\r\n", eth_type);
    
    // after parsing the IP header:
    USB_DEBUG("  IP proto: %d\r\n", ip[9]);
    
    // after parsing the UDP header:
    USB_DEBUG("  UDP dst port: %d\r\n", get_be16(&udp[2]));
    
    // after parsing the DHCP message type:
    USB_DEBUG("  DHCP msg type: %d\r\n", dhcp_msg_type);
    
    // ... existing code
}
```

**Expected output**:
- EthType: 0x0800
- IP proto: 17 (UDP)
- UDP dst port: 67
- DHCP msg type: 1 (Discover) or 3 (Request)

### Step 5: verify DHCP response construction

Check whether the DHCP response is built correctly:

```c
// add to the response construction part of handle_target_dhcp_packet():
USB_DEBUG("Building DHCP response:\r\n");
USB_DEBUG("  yiaddr: %d.%d.%d.%d\r\n", 
          reply[yiaddr_offset], reply[yiaddr_offset+1], 
          reply[yiaddr_offset+2], reply[yiaddr_offset+3]);
USB_DEBUG("  dst IP: %d.%d.%d.%d\r\n",
          reply[dst_ip_offset], reply[dst_ip_offset+1],
          reply[dst_ip_offset+2], reply[dst_ip_offset+3]);
```

**Expected output**:
- yiaddr: 192.168.11.2
- dst IP: 255.255.255.255 (Discover) or 192.168.11.2 (Request)

### Step 6: verify response transmission

Check whether the DHCP response is sent correctly:

```c
// add before handle_target_dhcp_packet() returns:
USB_DEBUG("DHCP response ready, len=%d\r\n", reply_len);

// add to ECM_Bridge_SendRawFrame() or similar:
void ECM_Bridge_SendRawFrame(const uint8_t *data, uint16_t len)
{
    USB_DEBUG("Sending raw frame: len=%d\r\n", len);
    // ... existing code
}
```

**Expected output**: "Sending raw frame" with the correct length should appear

### Step 7: verify the USBFS endpoint configuration

Check whether the USBFS endpoints on the Target side are configured correctly:

```c
// add to USBFS_Device_Endp_Init() in APP/ch32v20x_usbfs_device.c:
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    USB_DEBUG("USBFS endpoints:\r\n");
    USB_DEBUG("  EP3: ECM data OUT (RX)\r\n");
    USB_DEBUG("  EP4: ECMdata IN (TX)\r\n");
    USB_DEBUG("  EP6: ACMdata OUT (RX)\r\n");
    USB_DEBUG("  EP7: ACMdata IN (TX)\r\n");
#endif
```

**Expected output**: the endpoint configuration is correct

### Step 8: verify the MAC address

Check the MAC address configuration on the Target side:

```c
// check in APP/ecm_bridge.c:
USB_DEBUG("Target device MAC: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
          ECM_TARGET_DEV_MAC[0], ECM_TARGET_DEV_MAC[1], ECM_TARGET_DEV_MAC[2],
          ECM_TARGET_DEV_MAC[3], ECM_TARGET_DEV_MAC[4], ECM_TARGET_DEV_MAC[5]);
```

**Expected output**: 02:00:00:00:03:00 (or whatever MAC is configured)

## Possible causes

### Cause 1: Target CDC ECM not compiled
- **Symptom**: USB_DEBUG prints "Target CDC ECM: DISABLED"
- **Fix**: check the USB_WORK_MODE configuration and make sure it is 6 (HID_ACM_ECM)

### Cause 2: data never reaches the Target side
- **Symptom**: ECM_Bridge_TargetRxDone() is never called
- **Fix**: check the USBFS endpoint configuration and make sure EP6 OUT is configured correctly

### Cause 3: the DHCP request is not parsed correctly
- **Symptom**: handle_target_dhcp_packet() returns 0
- **Fix**: check the Ethernet frame format and make sure it conforms to the DHCP specification

### Cause 4: the DHCP response is not built correctly
- **Symptom**: the DHCP response data is wrong
- **Fix**: check the DHCP response construction logic and make sure the fields are correct

### Cause 5: the DHCP response is not sent
- **Symptom**: the response is built successfully but never sent
- **Fix**: check ECM_Bridge_SendRawFrame() or similar

### Cause 6: wrong USBFS endpoint configuration
- **Symptom**: the endpoints are not initialised correctly
- **Fix**: check the endpoint configuration in USBFS_Device_Endp_Init()

### Cause 7: MAC address mismatch
- **Symptom**: DHCP requests are filtered out
- **Fix**: check the MAC address filtering logic

## Fix approaches

### Approach A: enable debug logging

Add detailed debug logging in APP/ecm_bridge.c:

```c
#define USB_DEBUG_ENABLED 1

#if USB_DEBUG_ENABLED
#define USB_DEBUG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define USB_DEBUG(fmt, ...) ((void)0)
#endif
```

Then add USB_DEBUG() calls at the key points.

### Approach B: inspect the packet flow

Capture packets on the Target side with a USB analyser or a logic analyser:
1. check whether the bot device sent a DHCP Discover
2. check whether the Openterface device received the packet
3. check whether the Openterface device sent a DHCP Offer
4. check whether the bot device received the DHCP Offer

### Approach C: test a DHCP response manually

Temporarily modify the code to force a DHCP response:

```c
// add test code in ECM_Bridge_Init():
void ECM_Bridge_Init(void)
{
    // ... existing code
    
    // test: send a fake DHCP Offer
    uint8_t test_dhcp_offer[300];
    // ... build the test DHCP Offer
    ECM_Bridge_SendRawFrame(test_dhcp_offer, sizeof(test_dhcp_offer));
}
```

### Approach D: check the firmware version

Make sure the latest firmware is in use:

```bash
# check the firmware version
python3 check_firmware_version.py

# rebuild the firmware if needed
cd build
make clean
make USB_WORK_MODE=6
```

## Verification steps

After fixing, verify that the DHCP server works:

1. **Rebuild the firmware**
   ```bash
   cd build
   make clean
   make USB_WORK_MODE=6
   ```

2. **Flash the firmware**
   ```bash
   # use the appropriate flashing tool
   ```

3. **Restart the bot device's networking**
   ```bash
   # restart networking on the bot device
   sudo systemctl restart NetworkManager
   ```

4. **Check the bot device's logs**
   ```bash
   journalctl -u NetworkManager --since "1 minute ago" | grep dhcp
   ```

5. **Verify the IP address**
   ```bash
   ip addr show enp0s20f0u4u1i4
   # should show inet 192.168.11.2/24
   ```

6. **Test connectivity**
   ```bash
   ping 192.168.11.3  # the Openterface device
   ping 192.168.11.1  # the host
   ```

## References

- RFC 2131: Dynamic Host Configuration Protocol
- RFC 2132: DHCP Options and BOOTP Vendor Extensions
- CH32V20x USBFS device controller user manual
- CDC ECM class specification

## Appendix

### A. DHCP message types

| Type | Value | Description |
|------|-----|------|
| DHCP Discover | 1 | client discovers a DHCP server |
| DHCP Offer | 2 | server responds to Discover |
| DHCP Request | 3 | client requests an IP |
| DHCP Decline | 4 | client declines an IP |
| DHCP ACK | 5 | server confirms the Request |
| DHCP NAK | 6 | server rejects the Request |
| DHCP Release | 7 | client releases the IP |
| DHCP Inform | 8 | client requests configuration parameters |

### B. DHCP options

| Option | Value | Description |
|------|-----|------|
| Subnet Mask | 1 | subnet mask |
| Router | 3 | default gateway |
| DNS Server | 6 | DNS server |
| Domain Name | 15 | domain name |
| Requested IP | 50 | requested IP address |
| Lease Time | 51 | IP lease duration |
| Message Type | 53 | DHCP message type |
| Server ID | 54 | DHCP server identifier |
| End | 255 | end of options |

### C. Network topology

```
┌─────────────┐
│   Host PC   │
│ 192.168.11.1│
└──────┬──────┘
       │ USB CDC ECM (en13)
       │
┌──────┴──────┐
│ Openterface │
│   Device    │
│ 192.168.11.3│
└──────┬──────┘
       │ Target Port (Ethernet)
       │
┌──────┴──────┐
│  Bot Device │
│ 192.168.11.2│ (expected)
└─────────────┘
```

### D. Key variables

| Variable | Location | Value | Description |
|------|------|-----|------|
| ecm_host_ip | ecm_bridge.c | 192.168.11.1 | host IP |
| ecm_dev_ip | ecm_bridge.c | 192.168.11.3 | device IP |
| ecm_target_host_ip | ecm_bridge.c | 192.168.11.2 | Target PC IP |
| ecm_target_dev_ip | ecm_bridge.c | 192.168.11.5 | device IP on the Target side |
| ECM_TARGET_DEV_MAC | ecm_bridge.c | 02:00:00:00:03:00 | Target device MAC |
| ECM_TARGET_HOST_MAC | ecm_bridge.c | 02:00:00:00:00:20 | Target PC MAC |

---

**Document version**: 1.0  
**Created**: 2026-10-20  
**Last updated**: 2026-10-20  
**Author**: Claude Code
