# ECM Bridge DHCP Implementation

> **Superseded.** The ECM bridge was replaced by CDC-NCM; see [ncm-bridge.md](ncm-bridge.md). The DHCP/ARP description below still matches the NCM path.

## Overview

The ECM bridge (Mode 4) provides network connectivity between the host PC and target device through two separate CDC-ECM interfaces:

- **Host side** (USBD controller, PID 0xFE0C): Exposes CDC ECM to host PC (e.g., Mac)
- **Target side** (USBFS controller, PID 0xFE00): Exposes CDC ECM to target device (e.g., Windows PC)

Each side implements a minimal DHCP server that assigns IP addresses on the 192.168.11.0/24 subnet, enabling the two computers to communicate as if connected to the same Ethernet segment.

## Architecture

```
Host PC (Mac)                    Target PC (Windows)
    |                                |
    | USB (USBD, PID 0xFE0C)         | USB (USBFS, PID 0xFE00)
    v                                v
┌─────────────────────────────────────────────────┐
│  CH32V208 MCU                                    │
│  ├─ Host CDC ECM (en24)                          │
│  │  ├─ MAC: 02:00:00:00:00:02 (descriptor)       │
│  │  ├─ Firmware MAC: 02:00:00:00:00:32           │
│  │  ├─ Firmware IP: 192.168.11.3                 │
│  │  └─ Offers: 192.168.11.1 to Host PC           │
│  │                                               │
│  ├─ Target CDC ECM (en20)                        │
│  │  ├─ MAC: 02:00:00:00:00:30 (descriptor)       │
│  │  ├─ Firmware MAC: 02:00:00:00:03:00           │
│  │  ├─ Firmware IP: 192.168.11.5                 │
│  │  └─ Offers: 192.168.11.2 to Target PC         │
│  │                                               │
│  └─ Ethernet Bridge (L2 forwarding)              │
│     ├─ ARP/DHCP handled per-side                 │
│     └─ Other frames forwarded unchanged          │
└─────────────────────────────────────────────────┘
```

## IP Address Scheme

| Entity | IP Address | MAC Address | Notes |
|--------|-----------|-------------|-------|
| Host PC | 192.168.11.1 | 02:00:00:00:00:02 | Assigned by host-side DHCP |
| Target PC | 192.168.11.2 | 02:00:00:00:00:30 | Assigned by target-side DHCP |
| Host-side firmware | 192.168.11.3 | 02:00:00:00:00:32 | Firmware's identity on host link |
| Target-side firmware | 192.168.11.5 | 02:00:00:00:03:00 | Firmware's identity on target link |
| Subnet mask | 255.255.255.0 | - | Same for both sides |

## DHCP Flow

### Host Side (Mac connects to Host USB port)

1. Mac's CDC ECM interface comes up, sends DHCP Discover (broadcast)
2. Firmware receives frame via USBD EP6 OUT
3. `ECM_Bridge_HostRxDone()` reassembles frame, passes to `handle_host_control_frame()`
4. Frame is IPv4+UDP to port 67 → routed to `handle_dhcp_packet()`
5. `handle_dhcp_packet()` validates:
   - Source MAC must be 02:00:00:00:00:02 (Mac's descriptor MAC)
   - DHCP magic cookie must be present
   - Message type must be DISCOVER or REQUEST
6. Firmware builds DHCP OFFER/ACK:
   - Source: firmware MAC 02:00:00:00:00:32, IP 192.168.11.3
   - Offered IP: 192.168.11.1 (to Mac)
   - Options: subnet mask, router (192.168.11.3), DNS (192.168.11.3), lease time (3600s)
7. Reply queued to `g_target_to_host`, sent via USBD EP3 IN
8. Mac receives DHCP OFFER/ACK, configures interface with 192.168.11.1

### Target Side (Windows PC connects to Target USB port)

1. Target PC's CDC ECM interface comes up, sends DHCP Discover (broadcast)
2. Firmware receives frame via USBFS EP6 OUT
3. `ECM_Bridge_TargetRxDone()` reassembles frame, passes to `handle_target_frame()`
4. Frame is IPv4+UDP to port 67 → routed to `handle_target_dhcp_packet()`
5. `handle_target_dhcp_packet()` validates:
   - DHCP magic cookie must be present
   - Message type must be DISCOVER or REQUEST
   - No MAC filtering (accepts any client MAC)
6. Firmware builds DHCP OFFER/ACK:
   - Source: firmware MAC 02:00:00:00:03:00, IP 192.168.11.5
   - Offered IP: 192.168.11.2 (to Target PC)
   - Options: subnet mask, router (192.168.11.5), DNS (192.168.11.5), lease time (3600s)
7. Reply queued to `g_host_to_target`, sent via USBFS EP7 IN
8. Target PC receives DHCP OFFER/ACK, configures interface with 192.168.11.2

## ARP Handling

### Gratuitous ARP (GARP)

When each CDC ECM data interface is activated (SET_INTERFACE alt=1), the firmware sends a gratuitous ARP to populate the connected PC's ARP cache:

- **Host side**: `poll_host_tx()` checks `g_host_ecm_data_if_active && !g_host_garp_sent`, queues GARP with firmware MAC 02:00:00:00:00:32 and IP 192.168.11.3 via USBD EP3
- **Target side**: `poll_target_tx()` checks `g_target_ecm_data_if_active && !g_target_garp_sent`, queues GARP with firmware MAC 02:00:00:00:03:00 and IP 192.168.11.5 via USBFS EP7

### ARP Request/Reply

When a PC needs to resolve an IP address:

1. PC sends ARP request (broadcast or unicast)
2. Firmware receives frame, checks if target IP matches firmware's IP on that side:
   - Host side: `handle_arp_frame()` checks for 192.168.11.3
   - Target side: `handle_target_arp_frame()` checks for 192.168.11.5
3. If match, firmware sends ARP reply with its MAC address
4. PC caches the firmware's MAC for that IP
5. Future packets to that IP use the firmware's MAC as destination

**Note**: The firmware uses different MAC addresses than the descriptor MACs. This is intentional to avoid conflicts (e.g., Mac OS ignores ARP replies that match its own interface MAC).

## Frame Forwarding

After DHCP and ARP are handled, other Ethernet frames are forwarded unchanged between the two sides:

- Frames from Host PC → `handle_host_control_frame()` → if not DHCP/ARP, queued to `g_host_to_target` → sent via USBFS EP7 to Target PC
- Frames from Target PC → `handle_target_frame()` → if not DHCP/ARP, queued to `g_target_to_host` → sent via USBD EP3 to Host PC

This creates a transparent L2 bridge. The Host PC and Target PC can ping each other, establish TCP connections, and use any IP-based protocol as if directly connected via Ethernet.

## Testing

To verify the bridge is working:

1. Connect Host PC to the Host USB port (USBD controller)
2. Connect Target PC to the Target USB port (USBFS controller)
3. Both PCs should get DHCP:
   - Host PC: 192.168.11.1
   - Target PC: 192.168.11.2
4. From Host PC: `ping 192.168.11.2` should reach Target PC
5. From Target PC: `ping 192.168.11.1` should reach Host PC
6. SSH, file transfer, and other IP services should work bidirectionally

## Implementation Details

### Key Files

- `APP/ecm_bridge.c`: Main bridge logic, DHCP handlers, ARP handlers, frame forwarding
- `APP/include/ecm_bridge.h`: Public API and volatile flags
- `APP/USBLIB/CONFIG/usb_prop.c`: USBD (host-side) SET_INTERFACE handler
- `APP/ch32v20x_usbfs_device.c`: USBFS (target-side) SET_INTERFACE handler

### Key Functions

- `handle_dhcp_packet()`: Host-side DHCP handler
- `handle_target_dhcp_packet()`: Target-side DHCP handler
- `handle_arp_frame()`: Host-side ARP handler
- `handle_target_arp_frame()`: Target-side ARP handler
- `poll_host_tx()`: Sends queued frames to Host PC via USBD EP3, includes GARP logic
- `poll_target_tx()`: Sends queued frames to Target PC via USBFS EP7, includes GARP logic

### Volatile Flags

- `g_host_ecm_data_if_active`: Set when USBD CDC ECM data interface is activated
- `g_target_ecm_data_if_active`: Set when USBFS CDC ECM data interface is activated
- `g_host_garp_sent`: Tracks whether GARP has been sent on host side
- `g_target_garp_sent`: Tracks whether GARP has been sent on target side

These flags are split between host and target sides to prevent state confusion when both interfaces are active simultaneously.
