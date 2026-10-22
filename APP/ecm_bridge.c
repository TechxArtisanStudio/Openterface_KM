#include "include/ecm_bridge.h"
#include "include/Version_selection.h"
#include "include/usb_mode_config.h"
#include "include/ecm_ip_config.h"

#if USB_WORK_MODE_COMPILES_BRIDGE_ECM

#include "ch32v20x_usbfs_device.h"
#include "usb_lib.h"
#include "usb_conf.h"
#include "usb_desc.h"
#include "usb_prop.h"
#include "usb_pwr.h"
#include "include/usbd_composite_km.h"
#include "include/km_ring.h"
#include "include/ble_eth_protocol.h"
#include <string.h>

#define ECM_BRIDGE_MTU_FRAME_SIZE       1514u
#define ECM_BRIDGE_QUEUE_DEPTH          2u
#define ECM_BRIDGE_USBFS_PACKET_SIZE    64u
/* ECM bridge uses 64-byte bulk packets on both USBD and USBFS sides.
   Note: DEF_USBD_MAX_PACK_SIZE is 16 (legacy ACM), but the runtime ECM
   descriptor advertises 64-byte max packet for EP5/EP3 bulk endpoints. */
#define ECM_BRIDGE_USBD_PACKET_SIZE     64u
#define ECM_BRIDGE_TARGET_COMM_IF       4u
/* In combined ACM+ECM mode with USBFS (HID + ACM + ECM):
 *   IF0-IF1: HID (keyboard + mouse)
 *   IF2-IF3: CDC ACM
 *   IF4-IF5: CDC ECM
 * So host-side ECM comm interface is IF4.
 * For USBLIB stack (no HID), it would be IF2. */
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
#define ECM_BRIDGE_HOST_COMM_IF         4u
#else
#define ECM_BRIDGE_HOST_COMM_IF         0u
#endif
#define ECM_BRIDGE_LINK_SPEED_BPS       12000000u
#define ECM_ETH_TYPE_IPV4               0x0800u
#define ECM_ETH_TYPE_VLAN               0x8100u
#define ECM_ETH_TYPE_QINQ               0x88A8u
#define ECM_IPV4_PROTO_TCP              0x06u
#define ECM_IPV4_PROTO_UDP              0x11u
#define ECM_IPV4_PROTO_ICMP             0x01u

/* ICMP constants */
#define ECM_ICMP_ECHO_REPLY             0u
#define ECM_ICMP_ECHO_REQUEST           8u
#define ECM_ETH_HEADER_LEN              14u
#define ECM_IPV4_HEADER_LEN_OFFSET      16u  /* IP total_length offset within IP header */
#define ECM_IPV4_MIN_HEADER_LEN         20u
#define ECM_TCP_MIN_HEADER_LEN          20u
#define ECM_CH9329_MIN_FRAME_LEN        6u
#define ECM_ARP_FRAME_LEN               42u  /* 14 eth + 28 arp */

/* DHCP constants */
#define ECM_DHCP_CLIENT_PORT            68u
#define ECM_DHCP_SERVER_PORT            67u
#define ECM_DHCP_MAGIC_COOKIE           0x63825363u

/* DHCP message types */
#define ECM_DHCP_DISCOVER               1u
#define ECM_DHCP_OFFER                  2u
#define ECM_DHCP_REQUEST                3u
#define ECM_DHCP_ACK                    5u

/* DHCP option codes */
#define ECM_DHCP_OPT_SUBNET             1u
#define ECM_DHCP_OPT_ROUTER             3u
#define ECM_DHCP_OPT_DNS                6u
#define ECM_DHCP_OPT_REQUESTED_IP       50u
#define ECM_DHCP_OPT_LEASE_TIME         51u
#define ECM_DHCP_OPT_MSG_TYPE           53u
#define ECM_DHCP_OPT_SERVER_ID          54u
#define ECM_DHCP_OPT_END                255u

/* Runtime network parameters (populated from ECM_IPConfig module) */
static uint8_t ecm_host_ip[4];
static uint8_t ecm_dev_ip[4];
static uint8_t ecm_subnet_mask[4];
static uint8_t ecm_target_host_ip[4];
static uint8_t ecm_target_dev_ip[4];

/* Firmware-side peer identity on the CDC ECM link. The descriptor MAC
   02:00:00:00:00:02 becomes macOS's interface MAC, so ARP replies must use a
   distinct peer MAC or macOS ignores them as self-MAC replies. */
static const uint8_t ECM_DEV_MAC[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x32 };

/* Firmware-side peer identity on the Target CDC ECM link (USBFS side). */
static const uint8_t ECM_TARGET_DEV_MAC[6] = { 0x02, 0x00, 0x00, 0x00, 0x03, 0x00 };

typedef struct {
    uint8_t data[ECM_BRIDGE_MTU_FRAME_SIZE];
    uint16_t len;
} ECM_Frame_t;

typedef struct {
    ECM_Frame_t frames[ECM_BRIDGE_QUEUE_DEPTH];
    volatile uint8_t rd;
    volatile uint8_t wr;
    volatile uint8_t count;
    volatile uint32_t overflow;
} ECM_FrameQueue_t;

typedef struct {
    uint8_t data[ECM_BRIDGE_MTU_FRAME_SIZE];
    uint16_t len;
    uint16_t expected_len;
    uint8_t drop;
    volatile uint32_t overflow;
} ECM_RxFrame_t;

typedef uint8_t (*ECM_FrameHandler_t)(const uint8_t *data, uint16_t len);

/* Forward declaration */
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
static uint8_t handle_target_frame(const uint8_t *data, uint16_t len);
static uint8_t handle_target_dhcp_packet(const uint8_t *frame, uint16_t frame_len);
static uint8_t handle_target_icmp_packet(const uint8_t *frame, uint16_t frame_len);
#endif
static uint8_t handle_dhcp_packet(const uint8_t *frame, uint16_t frame_len);
static uint8_t handle_icmp_packet(const uint8_t *frame, uint16_t frame_len);
static uint8_t queue_push(ECM_FrameQueue_t *queue, const uint8_t *data, uint16_t len);
static void queue_clear(ECM_FrameQueue_t *queue);
static void put_be16(uint8_t *data, uint16_t value);
static void put_u16(uint8_t *data, uint16_t value);

static ECM_FrameQueue_t g_target_to_host;
static ECM_RxFrame_t g_host_rx_frame;
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
static ECM_FrameQueue_t g_host_to_target;
static ECM_RxFrame_t g_target_rx_frame;
static uint16_t g_target_tx_offset;
static uint8_t g_target_zlp_pending;
static uint8_t g_target_notify_state;
#endif
static uint16_t g_host_tx_offset;
static uint8_t g_host_zlp_pending;
static uint8_t g_host_notify_state;

/* Diagnostic variables moved to after #endif (always defined, see bottom of file) */

extern uint8_t ECM_USBD_INT_BUSY;
extern uint8_t ECM_USBD_BULK_IN_BUSY;

/* Callback invoked when target frame data is pushed to BLE.
   Set by peripheral.c to trigger BLE TX event. */
static void (*g_ble_tx_event_cb)(void);

void ECM_Bridge_SetBleTxCallback(void (*cb)(void))
{
    g_ble_tx_event_cb = cb;
}

static uint16_t get_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static void put_be16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)(value & 0xFF);
}

static uint8_t ch9329_frame_valid(const uint8_t *data, uint16_t len, uint16_t *frame_len)
{
    uint16_t total_len;
    uint16_t index;
    uint8_t checksum = 0;

    if (data == 0 || len < ECM_CH9329_MIN_FRAME_LEN) {
        return 0;
    }
    if (data[0] != CH9329_FRAME_HEAD1 || data[1] != CH9329_FRAME_HEAD2) {
        return 0;
    }

    total_len = (uint16_t)(ECM_CH9329_MIN_FRAME_LEN + data[4]);
    if (len < total_len) {
        return 0;
    }

    for (index = 0; index < (uint16_t)(total_len - 1u); index++) {
        checksum = (uint8_t)(checksum + data[index]);
    }
    if (checksum != data[total_len - 1u]) {
        return 0;
    }

    if (frame_len != 0) {
        *frame_len = total_len;
    }
    return 1;
}

static uint8_t ch9329_payload_valid(const uint8_t *payload, uint16_t payload_len)
{
    uint16_t offset = 0;
    uint16_t frame_len;

    if (payload_len < ECM_CH9329_MIN_FRAME_LEN) {
        return 0;
    }

    while (offset < payload_len) {
        if (!ch9329_frame_valid(&payload[offset], (uint16_t)(payload_len - offset), &frame_len)) {
            return 0;
        }
        offset = (uint16_t)(offset + frame_len);
    }

    return 1;
}

static uint8_t queue_push(ECM_FrameQueue_t *queue, const uint8_t *data, uint16_t len);

/* ARP handler: respond to ARP requests for the device IP so the Mac
   can resolve our MAC address and route packets back to us. */
static uint8_t handle_arp_frame(const uint8_t *frame, uint16_t frame_len)
{
    if (frame_len < 28) {
        return 0;
    }

    uint16_t eth_type = (uint16_t)(frame[12] << 8) | frame[13];
    if (eth_type != 0x0806) {
        g_ecm_led_override = 5;  /* not ARP */
        return 0;
    }
    g_ecm_led_override = 2;  /* ARP detected */

    uint16_t arp_op = (uint16_t)(frame[20] << 8) | frame[21];

    /* Handle ARP requests for our device IPs (both host-side and target-side) */
    if (arp_op == 1) {  /* ARP request */
        uint8_t is_dev_ip = (frame[38] == ecm_dev_ip[0] && frame[39] == ecm_dev_ip[1] &&
                             frame[40] == ecm_dev_ip[2] && frame[41] == ecm_dev_ip[3]);
        uint8_t is_target_dev_ip = (frame[38] == ecm_target_dev_ip[0] && frame[39] == ecm_target_dev_ip[1] &&
                                    frame[40] == ecm_target_dev_ip[2] && frame[41] == ecm_target_dev_ip[3]);

        if (!is_dev_ip && !is_target_dev_ip) {
            g_ecm_led_override = 4;  /* wrong ip */
            USB_DEBUG("ARP req but wrong IP %d.%d.%d.%d\r\n",
                       frame[38], frame[39], frame[40], frame[41]);
            return 0;
        }

        g_ecm_led_override = 3;  /* ARP reply queued */
        USB_DEBUG("ARP req matched, queuing reply\r\n");

        /* Build ARP reply: sender=us, target=requestor */
        uint8_t reply[42];
        const uint8_t *sender_mac = is_target_dev_ip ? ECM_TARGET_DEV_MAC : ECM_DEV_MAC;
        const uint8_t *sender_ip = is_target_dev_ip ? ecm_target_dev_ip : ecm_dev_ip;

        memcpy(&reply[0], &frame[6], 6);   /* dst MAC = requestor MAC */
        memcpy(&reply[6], sender_mac, 6);  /* src MAC = our MAC */
        reply[12] = 0x08; reply[13] = 0x06; /* EtherType = ARP */
        reply[14] = 0x00; reply[15] = 0x01; /* hardware type = Ethernet */
        reply[16] = 0x08; reply[17] = 0x00; /* protocol type = IPv4 */
        reply[18] = 6;  reply[19] = 4;     /* MAC len=6, IP len=4 */
        reply[20] = 0x00; reply[21] = 0x02; /* op = ARP reply */
        memcpy(&reply[22], sender_mac, 6);  /* sender MAC */
        memcpy(&reply[28], sender_ip, 4);   /* sender IP */
        memcpy(&reply[32], &frame[22], 6);   /* target MAC (from request) */
        memcpy(&reply[38], &frame[28], 4);   /* target IP = requestor IP */

        queue_push(&g_target_to_host, reply, sizeof(reply));
        return 1;
    }

    /* Handle ARP replies - pass to BLE-Eth for connection resolution */
    if (arp_op == 2) {  /* ARP reply */
        uint8_t sender_ip[4], sender_mac[6];
        memcpy(sender_mac, &frame[22], 6);
        memcpy(sender_ip, &frame[28], 4);
        if (BLE_Eth_HandleArpReply(sender_ip, sender_mac)) {
            USB_DEBUG("ARP reply processed\r\n");
        }
        /* Don't consume the frame - let it also go to Mac's queue */
    }

    return 0;
}

/* Target-side ARP handler: respond to ARP requests using target-side MAC/IP */
static uint8_t handle_target_arp_frame(const uint8_t *frame, uint16_t frame_len)
{
    if (frame_len < 28) {
        return 0;
    }

    uint16_t eth_type = (uint16_t)(frame[12] << 8) | frame[13];
    if (eth_type != 0x0806) {
        return 0;
    }

    uint16_t arp_op = (uint16_t)(frame[20] << 8) | frame[21];

    /* Handle ARP requests for our target device IP */
    if (arp_op == 1) {  /* ARP request */
        if (!(frame[38] == ecm_target_dev_ip[0] && frame[39] == ecm_target_dev_ip[1] &&
              frame[40] == ecm_target_dev_ip[2] && frame[41] == ecm_target_dev_ip[3])) {
            USB_DEBUG("Target ARP req but wrong IP %d.%d.%d.%d\r\n",
                       frame[38], frame[39], frame[40], frame[41]);
            return 0;
        }

        USB_DEBUG("Target ARP req matched, queuing reply\r\n");

        /* Build ARP reply: sender=us, target=requestor */
        uint8_t reply[42];
        memcpy(&reply[0], &frame[6], 6);   /* dst MAC = requestor MAC */
        memcpy(&reply[6], ECM_TARGET_DEV_MAC, 6); /* src MAC = our target MAC */
        reply[12] = 0x08; reply[13] = 0x06; /* EtherType = ARP */
        reply[14] = 0x00; reply[15] = 0x01; /* hardware type = Ethernet */
        reply[16] = 0x08; reply[17] = 0x00; /* protocol type = IPv4 */
        reply[18] = 6;  reply[19] = 4;     /* MAC len=6, IP len=4 */
        reply[20] = 0x00; reply[21] = 0x02; /* op = ARP reply */
        memcpy(&reply[22], ECM_TARGET_DEV_MAC, 6);  /* sender MAC */
        memcpy(&reply[28], ecm_target_dev_ip, 4);   /* sender IP */
        memcpy(&reply[32], &frame[22], 6);   /* target MAC (from request) */
        memcpy(&reply[38], &frame[28], 4);   /* target IP = requestor IP */

        queue_push(&g_host_to_target, reply, sizeof(reply));
        return 1;
    }

    /* Handle ARP replies from target side - pass to BLE-Eth for connection resolution */
    if (arp_op == 2) {  /* ARP reply */
        uint8_t sender_ip[4], sender_mac[6];
        memcpy(sender_mac, &frame[22], 6);
        memcpy(sender_ip, &frame[28], 4);
        USB_DEBUG("ARP reply from %d.%d.%d.%d MAC=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                  sender_ip[0], sender_ip[1], sender_ip[2], sender_ip[3],
                  sender_mac[0], sender_mac[1], sender_mac[2],
                  sender_mac[3], sender_mac[4], sender_mac[5]);
        if (BLE_Eth_HandleArpReply(sender_ip, sender_mac)) {
            USB_DEBUG("ARP reply from target processed\r\n");
        }
    }

    return 0;
}

/* ICMP handler: respond to ICMP Echo Request (ping) from the Mac */
static uint8_t handle_icmp_packet(const uint8_t *frame, uint16_t frame_len)
{
    uint16_t eth_offset;
    uint16_t eth_type;
    const uint8_t *ip;
    uint16_t ip_header_len;
    uint16_t ip_total_len;
    const uint8_t *icmp;
    uint16_t icmp_len;
    uint8_t icmp_type;
    static uint8_t reply[1514];
    uint16_t off;
    uint16_t ip_start;
    uint16_t icmp_start;
    uint32_t sum;
    uint16_t chk;

    if (frame == NULL || frame_len < ECM_ETH_HEADER_LEN) {
        return 0;
    }

    /* Parse Ethernet header */
    eth_offset = ECM_ETH_HEADER_LEN;
    eth_type = get_be16(&frame[12]);
    while ((eth_type == ECM_ETH_TYPE_VLAN || eth_type == ECM_ETH_TYPE_QINQ) &&
           frame_len >= (uint16_t)(eth_offset + 4u)) {
        eth_type = get_be16(&frame[eth_offset + 2u]);
        eth_offset = (uint16_t)(eth_offset + 4u);
    }

    if (eth_type != ECM_ETH_TYPE_IPV4 ||
        frame_len < (uint16_t)(eth_offset + ECM_IPV4_MIN_HEADER_LEN)) {
        return 0;
    }

    /* Parse IPv4 header */
    ip = &frame[eth_offset];
    if ((ip[0] >> 4) != 4u) {
        return 0;
    }

    ip_header_len = (uint16_t)((ip[0] & 0x0Fu) * 4u);
    if (ip_header_len < ECM_IPV4_MIN_HEADER_LEN) {
        return 0;
    }
    if (frame_len < (uint16_t)(eth_offset + ip_header_len)) {
        return 0;
    }

    ip_total_len = get_be16(&ip[2]);
    if (ip_total_len < (uint16_t)(ip_header_len + 8u) ||
        frame_len < (uint16_t)(eth_offset + ip_total_len)) {
        return 0;
    }

    /* Check if ICMP */
    if (ip[9] != ECM_IPV4_PROTO_ICMP) {
        return 0;
    }

    /* Check if destination IP is one of our configured IPs */
    const uint8_t *dst_ip = &ip[16];
    uint8_t is_our_ip = 0;

    /* Check against all configured IP addresses */
    if (memcmp(dst_ip, ecm_host_ip, 4) == 0 ||
        memcmp(dst_ip, ecm_dev_ip, 4) == 0 ||
        memcmp(dst_ip, ecm_target_host_ip, 4) == 0 ||
        memcmp(dst_ip, ecm_target_dev_ip, 4) == 0) {
        is_our_ip = 1;
    }

    if (!is_our_ip) {
        USB_DEBUG("ICMP: dst IP %d.%d.%d.%d not ours, ignoring\r\n",
                  dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);
        return 0;
    }

    /* Parse ICMP header */
    icmp = &ip[ip_header_len];
    icmp_len = (uint16_t)(ip_total_len - ip_header_len);
    icmp_type = icmp[0];

    /* Only handle Echo Request (type 8) */
    if (icmp_type != ECM_ICMP_ECHO_REQUEST) {
        return 0;
    }

    /* Build ICMP Echo Reply */
    memset(reply, 0, sizeof(reply));
    off = 0;

    /* Ethernet header - swap src/dst MAC */
    memcpy(&reply[off], &frame[6], 6);  /* dst = original src */
    off += 6;
    memcpy(&reply[off], &frame[0], 6);  /* src = original dst */
    off += 6;
    reply[off++] = 0x08;
    reply[off++] = 0x00;  /* EtherType = IPv4 */

    /* IPv4 header */
    ip_start = off;
    memcpy(&reply[off], ip, ip_header_len);  /* Copy IP header */
    /* Swap src/dst IP */
    memcpy(&reply[off + 12], &ip[16], 4);  /* src = original dst IP */
    memcpy(&reply[off + 16], &ip[12], 4);  /* dst = original src IP */
    reply[off + 9] = ECM_IPV4_PROTO_ICMP;  /* protocol = ICMP */
    reply[off + 10] = 0;  /* checksum placeholder */
    reply[off + 11] = 0;
    off += ip_header_len;

    /* ICMP header */
    icmp_start = off;
    memcpy(&reply[off], icmp, icmp_len);  /* Copy ICMP data */
    reply[off] = ECM_ICMP_ECHO_REPLY;  /* type = Echo Reply (0) */
    reply[off + 2] = 0;  /* checksum placeholder */
    reply[off + 3] = 0;
    off += icmp_len;

    /* Calculate ICMP checksum */
    sum = 0;
    for (uint16_t i = 0; i < icmp_len; i += 2) {
        if (i + 1 < icmp_len) {
            sum += (uint16_t)(reply[icmp_start + i] << 8) | reply[icmp_start + i + 1];
        } else {
            sum += (uint16_t)reply[icmp_start + i] << 8;
        }
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    chk = (~sum) & 0xFFFF;
    put_be16(&reply[icmp_start + 2], chk);

    /* Update IP total length */
    uint16_t ip_total = (uint16_t)(off - ip_start);
    put_be16(&reply[ip_start + 2], ip_total);

    /* Calculate IPv4 checksum */
    sum = 0;
    for (uint16_t i = 0; i < ip_header_len; i += 2) {
        sum += (uint16_t)(reply[ip_start + i] << 8) | reply[ip_start + i + 1];
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    chk = (~sum) & 0xFFFF;
    put_be16(&reply[ip_start + 10], chk);

    USB_DEBUG("ICMP Echo Reply queued, len=%d\r\n", off);
    queue_clear(&g_target_to_host);
    queue_push(&g_target_to_host, reply, off);
    return 1;
}

/* DHCP handler: respond to DHCP Discover/Request from the Mac so it
   auto-configures with IP 192.168.11.1 */
static uint8_t handle_dhcp_packet(const uint8_t *frame, uint16_t frame_len)
{
    uint16_t eth_offset;
    uint16_t eth_type;
    const uint8_t *ip;
    const uint8_t *udp;
    uint16_t ip_total_len;
    uint16_t ip_header_len;
    uint16_t udp_len;
    uint8_t dhcp_msg_type;
    uint8_t requested_ip[4];
    uint8_t has_requested_ip = 0;
    static uint8_t reply[576];  /* DHCP response buffer (min DHCP: 14+20+8+240=282, with options ~350) */
    uint16_t off;
    uint16_t dhcp_off;
    uint16_t udp_payload_len;
    uint32_t sum;
    uint16_t chk;

    /* Immediate diagnostic: handle_dhcp_packet was called.
     * Only upgrade g_dhcp_led_mode, never downgrade, so the deepest
     * failure point seen during a time window is what the LED shows.
     * Main loop auto-resets to 0 periodically.
     *
     * Mode numbering: success modes (20-22) > failure modes (10-19) > entry (1-9)
     * so success naturally overrides failure in the upgrade logic.
     *
     * LED: solid ON (brightness 100) when this function is entered.
     * Main loop will show LED ON if DHCP processing is happening.
     */
    g_dhcp_led_mode = 55;  /* unique value: DHCP handler entered */

    if (frame == NULL || frame_len < ECM_ETH_HEADER_LEN) {
        if (g_dhcp_led_mode < 10) g_dhcp_led_mode = 10;  /* frame too short */
        USB_DEBUG("DHCP: frame too short\r\n");
        return 0;
    }

    /* Parse Ethernet header */
    eth_offset = ECM_ETH_HEADER_LEN;
    eth_type = get_be16(&frame[12]);
    while ((eth_type == ECM_ETH_TYPE_VLAN || eth_type == ECM_ETH_TYPE_QINQ) &&
           frame_len >= (uint16_t)(eth_offset + 4u)) {
        eth_type = get_be16(&frame[eth_offset + 2u]);
        eth_offset = (uint16_t)(eth_offset + 4u);
    }

    if (eth_type != ECM_ETH_TYPE_IPV4 ||
        frame_len < (uint16_t)(eth_offset + ECM_IPV4_MIN_HEADER_LEN)) {
        if (g_dhcp_led_mode < 11) g_dhcp_led_mode = 11;  /* not IPv4 */
        USB_DEBUG("DHCP: not IPv4, type=0x%04X\r\n", eth_type);
        return 0;
    }

    /* Parse IPv4 header */
    ip = &frame[eth_offset];
    if ((ip[0] >> 4) != 4u) {
        if (g_dhcp_led_mode < 12) g_dhcp_led_mode = 12;  /* not IPv4 version */
        USB_DEBUG("DHCP: not IPv4 (version=%d)\r\n", ip[0] >> 4);
        return 0;
    }

    ip_header_len = (uint16_t)((ip[0] & 0x0Fu) * 4u);
    if (ip_header_len < ECM_IPV4_MIN_HEADER_LEN) {
        if (g_dhcp_led_mode < 13) g_dhcp_led_mode = 13;  /* IHL < 5 (invalid) */
        USB_DEBUG("DHCP: IP header IHL too small, ihl=%d\r\n", ip_header_len);
        return 0;
    }
    if (frame_len < (uint16_t)(eth_offset + ip_header_len)) {
        if (g_dhcp_led_mode < 14) g_dhcp_led_mode = 14;  /* frame truncated */
        USB_DEBUG("DHCP: frame truncated for IP header, need=%d have=%d\r\n",
                  eth_offset + ip_header_len, frame_len);
        return 0;
    }

    ip_total_len = get_be16(&ip[2]);
    if (ip_total_len < (uint16_t)(ip_header_len + 8u) ||
        frame_len < (uint16_t)(eth_offset + ip_total_len)) {
        if (g_dhcp_led_mode < 14) g_dhcp_led_mode = 14;  /* IP total len */
        USB_DEBUG("DHCP: IP total len invalid\r\n");
        return 0;
    }

    /* Check if UDP */
    if (ip[9] != ECM_IPV4_PROTO_UDP) {
        if (g_dhcp_led_mode < 22) g_dhcp_led_mode = 22;  /* IPv4 but not UDP (4 flashes) */
        USB_DEBUG("DHCP: not UDP (proto=%d)\r\n", ip[9]);
        return 0;
    }

    /* Parse UDP header */
    udp = &ip[ip_header_len];
    udp_len = get_be16(&udp[4]);
    if (udp_len < 8u || udp_len > (uint16_t)(ip_total_len - ip_header_len)) {
        if (g_dhcp_led_mode < 16) g_dhcp_led_mode = 16;  /* UDP len issue */
        USB_DEBUG("DHCP: UDP len invalid\r\n");
        return 0;
    }

    /* Check if destination port is 67 (DHCP server) */
    if (get_be16(&udp[2]) != ECM_DHCP_SERVER_PORT) {
        /* Valid IPv4+UDP but not DHCP — mode 21 (3 flashes) */
        if (g_dhcp_led_mode < 21) g_dhcp_led_mode = 21;
        USB_DEBUG("DHCP: dst port=%d (not 67)\r\n", get_be16(&udp[2]));
        return 0;
    }

    /* SUCCESS: DHCP packet hit port 67 */
    if (g_dhcp_led_mode < 20) g_dhcp_led_mode = 20;

    /* Parse DHCP message */
    const uint8_t *dhcp = &udp[8];

    /* Only handle requests from the single remaining interface (en24, MAC 02:00:00:00:00:02) */
    const uint8_t *src_mac = &frame[6];
    if (!(src_mac[0] == 0x02 && src_mac[1] == 0x00 && src_mac[2] == 0x00 &&
          src_mac[3] == 0x00 && src_mac[4] == 0x00 && src_mac[5] == 0x02)) {
        g_dhcp_led_mode = 62;  /* unknown MAC */
        return 0;
    }
    g_dhcp_led_mode = 61;  /* Host interface (en24) */

    if (udp_len < 8u + 240u) {  /* min DHCP: 8 UDP + 240 DHCP */
        USB_DEBUG("DHCP: payload too short\r\n");
        return 0;
    }

    /* Check magic cookie */
    uint32_t cookie = ((uint32_t)dhcp[236] << 24) | ((uint32_t)dhcp[237] << 16) |
                      ((uint32_t)dhcp[238] << 8) | dhcp[239];
    if (cookie != ECM_DHCP_MAGIC_COOKIE) {
        USB_DEBUG("DHCP: bad cookie=0x%08X\r\n", cookie);
        return 0;
    }

    /* Verify chaddr (client MAC in DHCP payload) matches the source MAC */
    const uint8_t *chaddr = &dhcp[28];
    if (!(chaddr[0] == 0x02 && chaddr[1] == 0x00 && chaddr[2] == 0x00 &&
          chaddr[3] == 0x00 && chaddr[4] == 0x00 && chaddr[5] == 0x02)) {
        return 0;  /* chaddr mismatch */
    }

    /* Parse DHCP options to find message type and requested IP */
    uint16_t opt_off = 240;
    dhcp_msg_type = 0;
    while (opt_off < udp_len - 8u) {
        uint8_t opt_code = dhcp[opt_off];
        if (opt_code == ECM_DHCP_OPT_END) {
            break;
        }
        if (opt_off + 1 >= udp_len - 8u) {
            break;
        }
        uint8_t opt_len = dhcp[opt_off + 1];
        if (opt_off + 2 + opt_len > udp_len - 8u) {
            break;
        }

        if (opt_code == ECM_DHCP_OPT_MSG_TYPE && opt_len == 1) {
            dhcp_msg_type = dhcp[opt_off + 2];
        } else if (opt_code == ECM_DHCP_OPT_REQUESTED_IP && opt_len == 4) {
            memcpy(requested_ip, &dhcp[opt_off + 2], 4);
            has_requested_ip = 1;
        }

        opt_off = (uint16_t)(opt_off + 2 + opt_len);
    }

    if (dhcp_msg_type != ECM_DHCP_DISCOVER && dhcp_msg_type != ECM_DHCP_REQUEST) {
        USB_DEBUG("DHCP: unknown msg type=%d\r\n", dhcp_msg_type);
        return 0;
    }

    g_dhcp_led_mode = 11;  /* DHCP msg type parsed - pause 1s, on 3s */

    /* Log xid for debugging */
    uint32_t xid = ((uint32_t)dhcp[4] << 24) | ((uint32_t)dhcp[5] << 16) |
                   ((uint32_t)dhcp[6] << 8) | dhcp[7];
    USB_DEBUG("DHCP type=%d xid=0x%08lX\r\n", dhcp_msg_type, (unsigned long)xid);

    /* Build DHCP response (ACK) */
    memset(reply, 0, sizeof(reply));
    off = 0;

    /* Ethernet header - DHCP DISCOVER/OFFER must broadcast */
    if (dhcp_msg_type == ECM_DHCP_DISCOVER) {
        memset(&reply[off], 0xFF, 6);  /* dst = broadcast for OFFER */
    } else {
        memcpy(&reply[off], &frame[6], 6);  /* dst = original src for ACK */
    }
    off += 6;
    memcpy(&reply[off], ECM_DEV_MAC, 6); /* src = firmware device MAC */
    off += 6;
    reply[off++] = 0x08;
    reply[off++] = 0x00;  /* EtherType = IPv4 */

    /* IPv4 header (20 bytes) */
    uint16_t ip_start = off;
    memset(&reply[off], 0, 20);
    reply[off + 0] = 0x45;  /* version=4, IHL=5 */
    reply[off + 1] = 0x00;  /* DSCP */
    reply[off + 4] = 0x00;  /* identification */
    reply[off + 5] = 0x01;
    reply[off + 6] = 0x40;  /* DF */
    reply[off + 7] = 0x00;  /* fragment offset */
    reply[off + 8] = 64;    /* TTL */
    reply[off + 9] = ECM_IPV4_PROTO_UDP;  /* protocol = UDP */
    /* src IP = firmware device IP */
    memcpy(&reply[off + 12], ecm_dev_ip, 4);
    /* dst IP = host IP (broadcast for Discover, unicast for Request) */
    if (dhcp_msg_type == ECM_DHCP_DISCOVER) {
        memset(&reply[off + 16], 0xFF, 4);  /* broadcast */
    } else {
        memcpy(&reply[off + 16], ecm_host_ip, 4);
    }
    off += 20;

    /* UDP header (8 bytes) */
    uint16_t udp_start = off;
    put_be16(&reply[off + 0], ECM_DHCP_SERVER_PORT);  /* src port */
    put_be16(&reply[off + 2], ECM_DHCP_CLIENT_PORT);  /* dst port */
    /* UDP length and checksum filled later */
    off += 8;

    /* DHCP packet */
    dhcp_off = off;
    reply[off++] = 0x02;  /* op = BOOTREPLY */
    reply[off++] = 0x01;  /* htype = Ethernet */
    reply[off++] = 0x06;  /* hlen = 6 */
    reply[off++] = 0x00;  /* hops */
    /* xid (copy from request) */
    memcpy(&reply[off], &dhcp[4], 4);
    off += 4;
    /* secs (2 bytes) */
    memset(&reply[off], 0, 2);
    off += 2;
    /* flags (2 bytes) - copy from request (preserves broadcast flag) */
    memcpy(&reply[off], &dhcp[10], 2);
    off += 2;
    /* ciaddr (4 bytes) */
    memset(&reply[off], 0, 4);
    off += 4;
    /* yiaddr (4 bytes) - IP offered to client */
    memcpy(&reply[off], ecm_host_ip, 4);
    off += 4;
    /* siaddr (4 bytes) - server IP */
    memcpy(&reply[off], ecm_dev_ip, 4);
    off += 4;
    /* giaddr (4 bytes) */
    memset(&reply[off], 0, 4);
    off += 4;
    /* chaddr (copy from request) */
    memcpy(&reply[off], &dhcp[28], 16);
    off += 16;
    /* sname (64 bytes) */
    memset(&reply[off], 0, 64);
    off += 64;
    /* file (128 bytes) */
    memset(&reply[off], 0, 128);
    off += 128;
    /* magic cookie */
    reply[off++] = 0x63;
    reply[off++] = 0x82;
    reply[off++] = 0x53;
    reply[off++] = 0x63;

    /* DHCP options */
    /* Message type: OFFER for DISCOVER, ACK for REQUEST (RFC 2131) */
    {
        uint8_t reply_type = (dhcp_msg_type == ECM_DHCP_DISCOVER)
            ? ECM_DHCP_OFFER : ECM_DHCP_ACK;
        reply[off++] = ECM_DHCP_OPT_MSG_TYPE;  // Option 53: DHCP Message Type
        reply[off++] = 0x01;
        reply[off++] = reply_type;
    }

    /* Server identifier */
    reply[off++] = ECM_DHCP_OPT_SERVER_ID;
    reply[off++] = 0x04;
    memcpy(&reply[off], ecm_dev_ip, 4);
    off += 4;

    /* Lease time: 1 hour */
    reply[off++] = ECM_DHCP_OPT_LEASE_TIME;
    reply[off++] = 0x04;
    reply[off++] = 0x00;
    reply[off++] = 0x00;
    reply[off++] = 0x0E;
    reply[off++] = 0x10;  /* 3600 seconds */

    /* Subnet mask */
    reply[off++] = ECM_DHCP_OPT_SUBNET;
    reply[off++] = 0x04;
    memcpy(&reply[off], ecm_subnet_mask, 4);  /* Same subnet mask for both */
    off += 4;

    /* DNS server - use public DNS to avoid OS trying to use firmware as DNS */
    reply[off++] = ECM_DHCP_OPT_DNS;
    reply[off++] = 0x04;
    uint8_t public_dns[4] = { 8, 8, 8, 8 };  /* Google DNS */
    memcpy(&reply[off], public_dns, 4);
    off += 4;

    /* Router (default gateway) - use firmware device IP */
    reply[off++] = ECM_DHCP_OPT_ROUTER;
    reply[off++] = 0x04;
    memcpy(&reply[off], ecm_dev_ip, 4);
    off += 4;

    /* End option */
    reply[off++] = ECM_DHCP_OPT_END;

    /* Pad DHCP message to minimum 300 bytes (RFC 2131 requirement) */
    /* dhcp_off is the offset where DHCP message starts (after Ethernet+IP+UDP headers) */
    while ((off - dhcp_off) < 300) {
        reply[off++] = 0x00;
    }

    /* Fill UDP length */
    udp_payload_len = (uint16_t)(off - udp_start);
    put_be16(&reply[udp_start + 4], udp_payload_len);

    /* Calculate UDP checksum */
    sum = 0;
    /* Pseudo-header */
    sum += (uint16_t)(reply[ip_start + 12] << 8) | reply[ip_start + 13];
    sum += (uint16_t)(reply[ip_start + 14] << 8) | reply[ip_start + 15];
    sum += (uint16_t)(reply[ip_start + 16] << 8) | reply[ip_start + 17];
    sum += (uint16_t)(reply[ip_start + 18] << 8) | reply[ip_start + 19];
    sum += ECM_IPV4_PROTO_UDP;
    sum += udp_payload_len;
    /* UDP header + data */
    for (uint16_t i = 0; i < udp_payload_len; i += 2) {
        uint16_t idx = udp_start + i;
        if (idx >= sizeof(reply)) break;
        if (i + 1 < udp_payload_len && idx + 1 < sizeof(reply)) {
            sum += (uint16_t)(reply[idx] << 8) | reply[idx + 1];
        } else {
            sum += (uint16_t)reply[idx] << 8;
        }
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    chk = (~sum) & 0xFFFF;
    if (chk == 0) chk = 0xFFFF;
    put_be16(&reply[udp_start + 6], chk);

    /* Fill IPv4 total length */
    uint16_t ip_total = (uint16_t)(off - ip_start);
    reply[ip_start + 2] = (ip_total >> 8) & 0xFF;
    reply[ip_start + 3] = ip_total & 0xFF;

    /* Calculate IPv4 checksum */
    sum = 0;
    for (int i = 0; i < 20; i += 2) {
        sum += (uint16_t)(reply[ip_start + i] << 8) | reply[ip_start + i + 1];
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    chk = (~sum) & 0xFFFF;
    reply[ip_start + 10] = (chk >> 8) & 0xFF;
    reply[ip_start + 11] = chk & 0xFF;

    USB_DEBUG("DHCP ACK queued, len=%d\r\n", off);
    g_dhcp_led_mode = 12;  /* DHCP ACK queued - pause 1s, on 5s */
    /* Flush any stale frames before queuing DHCP reply to ensure
       the reply is sent immediately without being blocked by old frames. */
    queue_clear(&g_target_to_host);
    queue_push(&g_target_to_host, reply, off);
    return 1;
}

#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
/* Target-side ICMP handler: respond to ICMP Echo Request (ping) from Target PC */
static uint8_t handle_target_icmp_packet(const uint8_t *frame, uint16_t frame_len)
{
    uint16_t eth_offset;
    uint16_t eth_type;
    const uint8_t *ip;
    uint16_t ip_header_len;
    uint16_t ip_total_len;
    const uint8_t *icmp;
    uint16_t icmp_len;
    uint8_t icmp_type;
    static uint8_t reply[1514];
    uint16_t off;
    uint16_t ip_start;
    uint16_t icmp_start;
    uint32_t sum;
    uint16_t chk;

    if (frame == NULL || frame_len < ECM_ETH_HEADER_LEN) {
        return 0;
    }

    /* Parse Ethernet header */
    eth_offset = ECM_ETH_HEADER_LEN;
    eth_type = get_be16(&frame[12]);
    while ((eth_type == ECM_ETH_TYPE_VLAN || eth_type == ECM_ETH_TYPE_QINQ) &&
           frame_len >= (uint16_t)(eth_offset + 4u)) {
        eth_type = get_be16(&frame[eth_offset + 2u]);
        eth_offset = (uint16_t)(eth_offset + 4u);
    }

    if (eth_type != ECM_ETH_TYPE_IPV4 ||
        frame_len < (uint16_t)(eth_offset + ECM_IPV4_MIN_HEADER_LEN)) {
        return 0;
    }

    /* Parse IPv4 header */
    ip = &frame[eth_offset];
    if ((ip[0] >> 4) != 4u) {
        return 0;
    }

    ip_header_len = (uint16_t)((ip[0] & 0x0Fu) * 4u);
    if (ip_header_len < ECM_IPV4_MIN_HEADER_LEN) {
        return 0;
    }
    if (frame_len < (uint16_t)(eth_offset + ip_header_len)) {
        return 0;
    }

    ip_total_len = get_be16(&ip[2]);
    if (ip_total_len < (uint16_t)(ip_header_len + 8u) ||
        frame_len < (uint16_t)(eth_offset + ip_total_len)) {
        return 0;
    }

    /* Check if ICMP */
    if (ip[9] != ECM_IPV4_PROTO_ICMP) {
        return 0;
    }

    /* Check if destination IP is one of our configured IPs */
    const uint8_t *dst_ip = &ip[16];
    uint8_t is_our_ip = 0;

    /* Check against all configured IP addresses */
    if (memcmp(dst_ip, ecm_host_ip, 4) == 0 ||
        memcmp(dst_ip, ecm_dev_ip, 4) == 0 ||
        memcmp(dst_ip, ecm_target_host_ip, 4) == 0 ||
        memcmp(dst_ip, ecm_target_dev_ip, 4) == 0) {
        is_our_ip = 1;
    }

    if (!is_our_ip) {
        USB_DEBUG("ICMP: dst IP %d.%d.%d.%d not ours, ignoring\r\n",
                  dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);
        return 0;
    }

    /* Parse ICMP header */
    icmp = &ip[ip_header_len];
    icmp_len = (uint16_t)(ip_total_len - ip_header_len);
    icmp_type = icmp[0];

    /* Only handle Echo Request (type 8) */
    if (icmp_type != ECM_ICMP_ECHO_REQUEST) {
        return 0;
    }

    /* Build ICMP Echo Reply */
    memset(reply, 0, sizeof(reply));
    off = 0;

    /* Ethernet header - swap src/dst MAC */
    memcpy(&reply[off], &frame[6], 6);  /* dst = original src */
    off += 6;
    memcpy(&reply[off], &frame[0], 6);  /* src = original dst */
    off += 6;
    reply[off++] = 0x08;
    reply[off++] = 0x00;  /* EtherType = IPv4 */

    /* IPv4 header */
    ip_start = off;
    memcpy(&reply[off], ip, ip_header_len);  /* Copy IP header */
    /* Swap src/dst IP */
    memcpy(&reply[off + 12], &ip[16], 4);  /* src = original dst IP */
    memcpy(&reply[off + 16], &ip[12], 4);  /* dst = original src IP */
    reply[off + 9] = ECM_IPV4_PROTO_ICMP;  /* protocol = ICMP */
    reply[off + 10] = 0;  /* checksum placeholder */
    reply[off + 11] = 0;
    off += ip_header_len;

    /* ICMP header */
    icmp_start = off;
    memcpy(&reply[off], icmp, icmp_len);  /* Copy ICMP data */
    reply[off] = ECM_ICMP_ECHO_REPLY;  /* type = Echo Reply (0) */
    reply[off + 2] = 0;  /* checksum placeholder */
    reply[off + 3] = 0;
    off += icmp_len;

    /* Calculate ICMP checksum */
    sum = 0;
    for (uint16_t i = 0; i < icmp_len; i += 2) {
        if (i + 1 < icmp_len) {
            sum += (uint16_t)(reply[icmp_start + i] << 8) | reply[icmp_start + i + 1];
        } else {
            sum += (uint16_t)reply[icmp_start + i] << 8;
        }
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    chk = (~sum) & 0xFFFF;
    put_be16(&reply[icmp_start + 2], chk);

    /* Update IP total length */
    uint16_t ip_total = (uint16_t)(off - ip_start);
    put_be16(&reply[ip_start + 2], ip_total);

    /* Calculate IPv4 checksum */
    sum = 0;
    for (uint16_t i = 0; i < ip_header_len; i += 2) {
        sum += (uint16_t)(reply[ip_start + i] << 8) | reply[ip_start + i + 1];
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    chk = (~sum) & 0xFFFF;
    put_be16(&reply[ip_start + 10], chk);

    USB_DEBUG("Target ICMP Echo Reply queued, len=%d\r\n", off);
    queue_clear(&g_host_to_target);
    queue_push(&g_host_to_target, reply, off);
    return 1;
}

/* Target-side DHCP handler: respond to DHCP Discover/Request from Target PC
   so it auto-configures with IP 192.168.11.2 */
static uint8_t handle_target_dhcp_packet(const uint8_t *frame, uint16_t frame_len)
{
    uint16_t eth_offset;
    uint16_t eth_type;
    const uint8_t *ip;
    const uint8_t *udp;
    uint16_t ip_total_len;
    uint16_t ip_header_len;
    uint16_t udp_len;
    uint8_t dhcp_msg_type;
    uint8_t requested_ip[4];
    uint8_t has_requested_ip = 0;
    static uint8_t reply[576];  /* DHCP response buffer */
    uint16_t off;
    uint16_t ip_start;
    uint16_t udp_start;
    uint16_t dhcp_off;
    uint16_t udp_payload_len;
    uint32_t sum;
    uint16_t chk;

    /* Step 4: Verify DHCP request parsing */
    USB_DEBUG("Target DHCP handler entered, frame_len=%d\r\n", frame_len);

    if (frame == NULL || frame_len < ECM_ETH_HEADER_LEN) {
        USB_DEBUG("Target DHCP: frame too short\r\n");
        return 0;
    }

    /* Parse Ethernet header */
    eth_offset = ECM_ETH_HEADER_LEN;
    eth_type = get_be16(&frame[12]);
    while ((eth_type == ECM_ETH_TYPE_VLAN || eth_type == ECM_ETH_TYPE_QINQ) &&
           frame_len >= (uint16_t)(eth_offset + 4u)) {
        eth_type = get_be16(&frame[eth_offset + 2u]);
        eth_offset = (uint16_t)(eth_offset + 4u);
    }

    USB_DEBUG("  EthType: 0x%04X\r\n", eth_type);

    if (eth_type != ECM_ETH_TYPE_IPV4 ||
        frame_len < (uint16_t)(eth_offset + ECM_IPV4_MIN_HEADER_LEN)) {
        USB_DEBUG("Target DHCP: not IPv4, type=0x%04X\r\n", eth_type);
        return 0;
    }

    /* Parse IPv4 header */
    ip = &frame[eth_offset];
    if ((ip[0] >> 4) != 4u) {
        USB_DEBUG("Target DHCP: not IPv4 (version=%d)\r\n", ip[0] >> 4);
        return 0;
    }

    ip_header_len = (uint16_t)((ip[0] & 0x0Fu) * 4u);
    if (ip_header_len < ECM_IPV4_MIN_HEADER_LEN) {
        USB_DEBUG("Target DHCP: IP header IHL too small, ihl=%d\r\n", ip_header_len);
        return 0;
    }
    if (frame_len < (uint16_t)(eth_offset + ip_header_len)) {
        USB_DEBUG("Target DHCP: frame truncated for IP header\r\n");
        return 0;
    }

    ip_total_len = get_be16(&ip[2]);
    if (ip_total_len < (uint16_t)(ip_header_len + 8u) ||
        frame_len < (uint16_t)(eth_offset + ip_total_len)) {
        USB_DEBUG("Target DHCP: IP total len invalid\r\n");
        return 0;
    }

    /* Check if UDP */
    if (ip[9] != ECM_IPV4_PROTO_UDP) {
        USB_DEBUG("Target DHCP: not UDP (proto=%d)\r\n", ip[9]);
        return 0;
    }

    USB_DEBUG("  IP proto: %d (UDP)\r\n", ip[9]);

    /* Parse UDP header */
    udp = &ip[ip_header_len];
    udp_len = get_be16(&udp[4]);
    if (udp_len < 8u || udp_len > (uint16_t)(ip_total_len - ip_header_len)) {
        USB_DEBUG("Target DHCP: UDP len invalid\r\n");
        return 0;
    }

    /* Check if destination port is 67 (DHCP server) */
    uint16_t dst_port = get_be16(&udp[2]);
    USB_DEBUG("  UDP dst port: %d\r\n", dst_port);

    if (dst_port != ECM_DHCP_SERVER_PORT) {
        USB_DEBUG("Target DHCP: dst port=%d (not 67)\r\n", dst_port);
        return 0;
    }

    /* Parse DHCP message */
    const uint8_t *dhcp = &udp[8];

    /* No MAC filtering for target side - accept any client */

    if (udp_len < 8u + 240u) {  /* min DHCP: 8 UDP + 240 DHCP */
        USB_DEBUG("Target DHCP: payload too short\r\n");
        return 0;
    }

    /* Check magic cookie */
    uint32_t cookie = ((uint32_t)dhcp[236] << 24) | ((uint32_t)dhcp[237] << 16) |
                      ((uint32_t)dhcp[238] << 8) | dhcp[239];
    if (cookie != ECM_DHCP_MAGIC_COOKIE) {
        USB_DEBUG("Target DHCP: bad cookie=0x%08X\r\n", cookie);
        return 0;
    }

    /* Parse DHCP options to find message type and requested IP */
    uint16_t opt_off = 240;
    dhcp_msg_type = 0;
    while (opt_off < udp_len - 8u) {
        uint8_t opt_code = dhcp[opt_off];
        if (opt_code == ECM_DHCP_OPT_END) {
            break;
        }
        if (opt_off + 1 >= udp_len - 8u) {
            break;
        }
        uint8_t opt_len = dhcp[opt_off + 1];
        if (opt_off + 2 + opt_len > udp_len - 8u) {
            break;
        }

        if (opt_code == ECM_DHCP_OPT_MSG_TYPE && opt_len == 1) {
            dhcp_msg_type = dhcp[opt_off + 2];
        } else if (opt_code == ECM_DHCP_OPT_REQUESTED_IP && opt_len == 4) {
            memcpy(requested_ip, &dhcp[opt_off + 2], 4);
            has_requested_ip = 1;
        }

        opt_off = (uint16_t)(opt_off + 2 + opt_len);
    }

    if (dhcp_msg_type != ECM_DHCP_DISCOVER && dhcp_msg_type != ECM_DHCP_REQUEST) {
        USB_DEBUG("Target DHCP: unknown msg type=%d\r\n", dhcp_msg_type);
        return 0;
    }

    USB_DEBUG("  DHCP msg type: %d (%s)\r\n", dhcp_msg_type,
              dhcp_msg_type == ECM_DHCP_DISCOVER ? "Discover" : "Request");

    /* Log xid for debugging */
    uint32_t xid = ((uint32_t)dhcp[4] << 24) | ((uint32_t)dhcp[5] << 16) |
                   ((uint32_t)dhcp[6] << 8) | dhcp[7];
    USB_DEBUG("Target DHCP type=%d xid=0x%08lX\r\n", dhcp_msg_type, (unsigned long)xid);

    /* Step 5: Verify DHCP response construction */
    USB_DEBUG("Building DHCP response:\r\n");
    USB_DEBUG("  yiaddr: %d.%d.%d.%d\r\n",
              ecm_target_host_ip[0], ecm_target_host_ip[1],
              ecm_target_host_ip[2], ecm_target_host_ip[3]);
    USB_DEBUG("  dst IP: %s\r\n",
              dhcp_msg_type == ECM_DHCP_DISCOVER ? "255.255.255.255 (broadcast)" :
              "192.168.11.2 (unicast)");

    /* Build DHCP response (ACK) */
    memset(reply, 0, sizeof(reply));
    off = 0;

    /* Ethernet header - DHCP DISCOVER/OFFER must broadcast */
    if (dhcp_msg_type == ECM_DHCP_DISCOVER) {
        memset(&reply[off], 0xFF, 6);  /* dst = broadcast for OFFER */
    } else {
        memcpy(&reply[off], &frame[6], 6);  /* dst = original src for ACK */
    }
    off += 6;
    memcpy(&reply[off], ECM_TARGET_DEV_MAC, 6); /* src = firmware target device MAC */
    off += 6;
    reply[off++] = 0x08;
    reply[off++] = 0x00;  /* EtherType = IPv4 */

    /* IPv4 header (20 bytes) */
    ip_start = off;
    memset(&reply[off], 0, 20);
    reply[off + 0] = 0x45;  /* version=4, IHL=5 */
    reply[off + 1] = 0x00;  /* DSCP */
    reply[off + 4] = 0x00;  /* identification */
    reply[off + 5] = 0x01;
    reply[off + 6] = 0x40;  /* DF */
    reply[off + 7] = 0x00;  /* fragment offset */
    reply[off + 8] = 64;    /* TTL */
    reply[off + 9] = ECM_IPV4_PROTO_UDP;  /* protocol = UDP */
    /* src IP = firmware target device IP */
    memcpy(&reply[off + 12], ecm_target_dev_ip, 4);
    /* dst IP = target host IP (broadcast for Discover, unicast for Request) */
    if (dhcp_msg_type == ECM_DHCP_DISCOVER) {
        memset(&reply[off + 16], 0xFF, 4);  /* broadcast */
    } else {
        memcpy(&reply[off + 16], ecm_target_host_ip, 4);
    }
    off += 20;

    /* UDP header (8 bytes) */
    udp_start = off;
    put_be16(&reply[off + 0], ECM_DHCP_SERVER_PORT);  /* src port */
    put_be16(&reply[off + 2], ECM_DHCP_CLIENT_PORT);  /* dst port */
    /* UDP length and checksum filled later */
    off += 8;

    /* DHCP packet */
    dhcp_off = off;
    reply[off++] = 0x02;  /* op = BOOTREPLY */
    reply[off++] = 0x01;  /* htype = Ethernet */
    reply[off++] = 0x06;  /* hlen = 6 */
    reply[off++] = 0x00;  /* hops */
    /* xid (copy from request) */
    memcpy(&reply[off], &dhcp[4], 4);
    off += 4;
    /* secs (2 bytes) */
    memset(&reply[off], 0, 2);
    off += 2;
    /* flags (2 bytes) - copy from request (preserves broadcast flag) */
    memcpy(&reply[off], &dhcp[10], 2);
    off += 2;
    /* ciaddr (4 bytes) */
    memset(&reply[off], 0, 4);
    off += 4;
    /* yiaddr (4 bytes) - IP offered to client */
    memcpy(&reply[off], ecm_target_host_ip, 4);
    off += 4;
    /* siaddr (4 bytes) - server IP */
    memcpy(&reply[off], ecm_target_dev_ip, 4);
    off += 4;
    /* giaddr (4 bytes) */
    memset(&reply[off], 0, 4);
    off += 4;
    /* chaddr (copy from request) */
    memcpy(&reply[off], &dhcp[28], 16);
    off += 16;
    /* sname (64 bytes) */
    memset(&reply[off], 0, 64);
    off += 64;
    /* file (128 bytes) */
    memset(&reply[off], 0, 128);
    off += 128;
    /* magic cookie */
    reply[off++] = 0x63;
    reply[off++] = 0x82;
    reply[off++] = 0x53;
    reply[off++] = 0x63;

    /* DHCP options */
    /* Message type: OFFER for DISCOVER, ACK for REQUEST (RFC 2131) */
    {
        uint8_t reply_type = (dhcp_msg_type == ECM_DHCP_DISCOVER)
            ? ECM_DHCP_OFFER : ECM_DHCP_ACK;
        reply[off++] = ECM_DHCP_OPT_MSG_TYPE;  // Option 53: DHCP Message Type
        reply[off++] = 0x01;
        reply[off++] = reply_type;
    }

    /* Server identifier */
    reply[off++] = ECM_DHCP_OPT_SERVER_ID;
    reply[off++] = 0x04;
    memcpy(&reply[off], ecm_target_dev_ip, 4);
    off += 4;

    /* Lease time: 1 hour */
    reply[off++] = ECM_DHCP_OPT_LEASE_TIME;
    reply[off++] = 0x04;
    reply[off++] = 0x00;
    reply[off++] = 0x00;
    reply[off++] = 0x0E;
    reply[off++] = 0x10;  /* 3600 seconds */

    /* Subnet mask */
    reply[off++] = ECM_DHCP_OPT_SUBNET;
    reply[off++] = 0x04;
    memcpy(&reply[off], ecm_subnet_mask, 4);
    off += 4;

    /* DNS server - use public DNS to avoid OS trying to use firmware as DNS */
    reply[off++] = ECM_DHCP_OPT_DNS;
    reply[off++] = 0x04;
    uint8_t public_dns[4] = { 8, 8, 8, 8 };  /* Google DNS */
    memcpy(&reply[off], public_dns, 4);
    off += 4;

    /* Router (default gateway) - use firmware target device IP */
    reply[off++] = ECM_DHCP_OPT_ROUTER;
    reply[off++] = 0x04;
    memcpy(&reply[off], ecm_target_dev_ip, 4);
    off += 4;

    /* End option */
    reply[off++] = ECM_DHCP_OPT_END;

    /* Pad DHCP message to minimum 300 bytes (RFC 2131 requirement) */
    /* dhcp_off is the offset where DHCP message starts (after Ethernet+IP+UDP headers) */
    while ((off - dhcp_off) < 300) {
        reply[off++] = 0x00;
    }

    /* Fill UDP length */
    udp_payload_len = (uint16_t)(off - udp_start);
    put_be16(&reply[udp_start + 4], udp_payload_len);

    /* Calculate UDP checksum */
    sum = 0;
    /* Pseudo-header */
    sum += (uint16_t)(reply[ip_start + 12] << 8) | reply[ip_start + 13];
    sum += (uint16_t)(reply[ip_start + 14] << 8) | reply[ip_start + 15];
    sum += (uint16_t)(reply[ip_start + 16] << 8) | reply[ip_start + 17];
    sum += (uint16_t)(reply[ip_start + 18] << 8) | reply[ip_start + 19];
    sum += ECM_IPV4_PROTO_UDP;
    sum += udp_payload_len;
    /* UDP header + data */
    for (uint16_t i = 0; i < udp_payload_len; i += 2) {
        uint16_t idx = udp_start + i;
        if (idx >= sizeof(reply)) break;
        if (i + 1 < udp_payload_len && idx + 1 < sizeof(reply)) {
            sum += (uint16_t)(reply[idx] << 8) | reply[idx + 1];
        } else {
            sum += (uint16_t)reply[idx] << 8;
        }
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    chk = (~sum) & 0xFFFF;
    if (chk == 0) chk = 0xFFFF;
    put_be16(&reply[udp_start + 6], chk);

    /* Fill IPv4 total length */
    uint16_t ip_total = (uint16_t)(off - ip_start);
    reply[ip_start + 2] = (ip_total >> 8) & 0xFF;
    reply[ip_start + 3] = ip_total & 0xFF;

    /* Calculate IPv4 checksum */
    sum = 0;
    for (int i = 0; i < 20; i += 2) {
        sum += (uint16_t)(reply[ip_start + i] << 8) | reply[ip_start + i + 1];
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    chk = (~sum) & 0xFFFF;
    reply[ip_start + 10] = (chk >> 8) & 0xFF;
    reply[ip_start + 11] = chk & 0xFF;

    /* Step 6: Verify response sending */
    USB_DEBUG("DHCP response ready, len=%d\r\n", off);
    USB_DEBUG("  Response type: %s\r\n",
              (dhcp_msg_type == ECM_DHCP_DISCOVER) ? "OFFER" : "ACK");
    USB_DEBUG("  Server ID: %d.%d.%d.%d\r\n",
              ecm_target_dev_ip[0], ecm_target_dev_ip[1],
              ecm_target_dev_ip[2], ecm_target_dev_ip[3]);

    queue_clear(&g_host_to_target);
    uint8_t push_result = queue_push(&g_host_to_target, reply, off);
    USB_DEBUG("  Queue push: %s\r\n", push_result ? "OK" : "FAIL");

    return 1;
}
#endif


static uint8_t tcp_payload_from_ipv4_frame(const uint8_t *frame,
                                           uint16_t frame_len,
                                           const uint8_t **payload,
                                           uint16_t *payload_len)
{
    uint16_t eth_offset;
    uint16_t eth_type;
    const uint8_t *ip;
    const uint8_t *tcp;
    uint16_t ip_total_len;
    uint16_t ip_header_len;
    uint16_t tcp_segment_len;
    uint16_t tcp_header_len;
    uint16_t fragment;

    if (frame == 0 || payload == 0 || payload_len == 0 || frame_len < ECM_ETH_HEADER_LEN) {
        return 0;
    }

    eth_offset = ECM_ETH_HEADER_LEN;
    eth_type = get_be16(&frame[12]);
    while ((eth_type == ECM_ETH_TYPE_VLAN || eth_type == ECM_ETH_TYPE_QINQ) &&
           frame_len >= (uint16_t)(eth_offset + 4u)) {
        eth_type = get_be16(&frame[eth_offset + 2u]);
        eth_offset = (uint16_t)(eth_offset + 4u);
    }

    if (eth_type != ECM_ETH_TYPE_IPV4 ||
        frame_len < (uint16_t)(eth_offset + ECM_IPV4_MIN_HEADER_LEN)) {
        return 0;
    }

    ip = &frame[eth_offset];
    if ((ip[0] >> 4) != 4u) {
        return 0;
    }

    ip_header_len = (uint16_t)((ip[0] & 0x0Fu) * 4u);
    if (ip_header_len < ECM_IPV4_MIN_HEADER_LEN ||
        frame_len < (uint16_t)(eth_offset + ip_header_len)) {
        return 0;
    }

    ip_total_len = get_be16(&ip[2]);
    if (ip_total_len < (uint16_t)(ip_header_len + ECM_TCP_MIN_HEADER_LEN) ||
        frame_len < (uint16_t)(eth_offset + ip_total_len)) {
        return 0;
    }

    fragment = get_be16(&ip[6]);
    if ((fragment & 0x3FFFu) != 0u || ip[9] != ECM_IPV4_PROTO_TCP) {
        return 0;
    }

    tcp = &ip[ip_header_len];
    tcp_segment_len = (uint16_t)(ip_total_len - ip_header_len);
    tcp_header_len = (uint16_t)((tcp[12] >> 4) * 4u);
    if (tcp_header_len < ECM_TCP_MIN_HEADER_LEN || tcp_header_len > tcp_segment_len) {
        return 0;
    }

    *payload = &tcp[tcp_header_len];
    *payload_len = (uint16_t)(tcp_segment_len - tcp_header_len);
    return (*payload_len > 0u) ? 1u : 0u;
}

static uint8_t queue_push(ECM_FrameQueue_t *queue, const uint8_t *data, uint16_t len);

static uint8_t handle_host_control_frame(const uint8_t *frame, uint16_t frame_len)
{
    const uint8_t *payload;
    uint16_t payload_len;

    /* Log every frame's src MAC and ethertype for debugging */
    if (frame_len >= 14) {
        USB_DEBUG("HCF: src=%02x:%02x:%02x:%02x:%02x:%02x etype=0x%04X len=%d\r\n",
                  frame[6], frame[7], frame[8], frame[9], frame[10], frame[11],
                  get_be16(&frame[12]), frame_len);
    }

    /* Diagnostic: only set mode 9 if we haven't reached a deeper diagnostic yet */
    if (g_dhcp_led_mode == 0) {
        g_dhcp_led_mode = 9;  /* Fast blink = host control frame entered */
    }

    /* Check EtherType first to route to correct handler */
    if (frame_len < 14) {
        return 0;
    }
    uint16_t eth_type = get_be16(&frame[12]);

    /* Handle DHCP requests FIRST - before ARP, since DHCP is needed for IP config */
    if (eth_type == 0x0800) {  /* IPv4 */
        if (handle_dhcp_packet(frame, frame_len)) {
            return 1;  /* DHCP response queued, done */
        }
        /* Handle ICMP Echo Request (ping) */
        if (handle_icmp_packet(frame, frame_len)) {
            return 1;  /* ICMP reply queued, done */
        }
    }

    /* Handle ARP requests - the Mac needs to resolve our MAC address */
    if (handle_arp_frame(frame, frame_len)) {
        return 1;  /* ARP reply queued, done */
    }

    /* If ARP request is for target PC IP (target_host_ip), forward to target side */
    if (eth_type == 0x0806 && frame_len >= 42) {
        uint16_t arp_op = (uint16_t)(frame[20] << 8) | frame[21];
        if (arp_op == 1) {  /* ARP request */
            uint8_t target_ip[4];
            memcpy(target_ip, &frame[38], 4);

            /* Only forward ARP for target_host_ip (target PC), not target_dev_ip (device itself) */
            if (target_ip[0] == ecm_target_host_ip[0] && target_ip[1] == ecm_target_host_ip[1] &&
                target_ip[2] == ecm_target_host_ip[2] && target_ip[3] == ecm_target_host_ip[3]) {
                USB_DEBUG("ARP req for target PC IP %d.%d.%d.%d, forwarding\r\n",
                          target_ip[0], target_ip[1], target_ip[2], target_ip[3]);
                /* Forward to target side */
                queue_push(&g_host_to_target, frame, frame_len);
                return 1;
            }
        }
    }

    /* First, check if this frame is a TCP response for an active BLE-Eth tunnel.
       The Mac's TCP stack may respond (e.g. SYN+ACK) to frames injected by
       BLE-Eth, and these need to be routed back through the tunnel. */
    if (BLE_Eth_HandleTargetFrame(frame, frame_len) && g_ble_tx_event_cb) {
        g_ble_tx_event_cb();
        /* Don't drop the frame — let it also go to the Mac's queue below */
    }

    if (!tcp_payload_from_ipv4_frame(frame, frame_len, &payload, &payload_len)) {
        return 0;
    }
    if (!ch9329_payload_valid(payload, payload_len)) {
        return 0;
    }

    /* Set flag to indicate we're processing CH9329 commands via ECM bridge.
     * This prevents CH9329_RxBuffer_Add from pushing responses to the USB queue,
     * since we'll handle them here by wrapping in Ethernet frames. */
    USB_DEBUG("HCF: CH9329 frame valid, payload_len=%d\r\n", payload_len);
    USB_DEBUG("HCF: First 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
              payload[0], payload[1], payload[2], payload[3],
              payload[4], payload[5], payload[6], payload[7]);
    g_ecm_bridge_processing_ch9329 = 1;
    CH9329_RxBuffer_Add((uint8_t *)payload, payload_len);
    g_ecm_bridge_processing_ch9329 = 0;

    /* Pop the CH9329 response (generated synchronously by the parser) and
     * wrap it in Ethernet/IPv4/TCP so it comes back to the host through
     * the ECM bridge's bulk IN endpoint. */
    Resp_Report_t resp;
    if (Queue_Pop_Response(&resp)) {
        static uint8_t out_frame[ECM_BRIDGE_MTU_FRAME_SIZE];  /* static: avoid stack overflow */
        uint16_t off = 0;

        /* Ethernet header: src←dst, dst←src (reverse direction) */
        memcpy(&out_frame[off], &frame[6], 6);  /* dst = original src */
        off += 6;
        memcpy(&out_frame[off], &frame[0], 6);  /* src = original dst */
        off += 6;
        out_frame[off++] = 0x08;
        out_frame[off++] = 0x00;  /* EtherType = IPv4 */

        /* IPv4 header (20 bytes) */
        uint16_t total_ip_len = 20 + 20 + resp.len;
        memset(&out_frame[off], 0, 20);
        out_frame[off + 0] = 0x45;  /* version=4, IHL=5 */
        out_frame[off + 1] = 0x00;  /* DSCP */
        out_frame[off + 2] = (total_ip_len >> 8) & 0xFF;
        out_frame[off + 3] = total_ip_len & 0xFF;
        out_frame[off + 4] = 0x00;  /* identification */
        out_frame[off + 5] = 0x01;
        out_frame[off + 6] = 0x40;  /* DF */
        out_frame[off + 7] = 0x00;  /* fragment offset */
        out_frame[off + 8] = 64;    /* TTL */
        out_frame[off + 9] = 6;     /* protocol = TCP */
        /* src IP = original dst IP */
        memcpy(&out_frame[off + 12], &frame[0x1A], 4);
        /* dst IP = original src IP */
        memcpy(&out_frame[off + 16], &frame[0x1E], 4);

        /* IPv4 checksum */
        uint32_t ip_sum = 0;
        for (int i = 0; i < 20; i += 2) {
            ip_sum += (uint16_t)(out_frame[off + i] << 8) | out_frame[off + i + 1];
        }
        while (ip_sum >> 16) ip_sum = (ip_sum & 0xFFFF) + (ip_sum >> 16);
        uint16_t ip_chk = (~ip_sum) & 0xFFFF;
        out_frame[off + 10] = (ip_chk >> 8) & 0xFF;
        out_frame[off + 11] = ip_chk & 0xFF;
        off += 20;

        /* TCP header (20 bytes) */
        memset(&out_frame[off], 0, 20);
        /* src port = original dst port, dst port = original src port */
        memcpy(&out_frame[off + 0], &frame[0x22], 2);
        memcpy(&out_frame[off + 2], &frame[0x20], 2);
        out_frame[off + 4] = 0x00;
        out_frame[off + 5] = 0x00;
        out_frame[off + 6] = 0x00;
        out_frame[off + 7] = 0x01;  /* seq */
        out_frame[off + 8] = 0x00;
        out_frame[off + 9] = 0x00;
        out_frame[off + 10] = 0x00;
        out_frame[off + 11] = 0x00;  /* ack */
        out_frame[off + 12] = 0x50;  /* data offset = 5 (20 bytes) */
        out_frame[off + 13] = 0x02;  /* flags = SYN */
        out_frame[off + 14] = 0xFF;
        out_frame[off + 15] = 0xFF;  /* window */
        off += 20;

        /* TCP payload (CH9329 response) */
        memcpy(&out_frame[off], resp.data, resp.len);
        off += resp.len;

        /* Push to target→host queue so it comes back to the host */
        queue_push(&g_target_to_host, out_frame, off);
    }

    return 1;
}

static uint8_t queue_push(ECM_FrameQueue_t *queue, const uint8_t *data, uint16_t len)
{
    if (len == 0) {
        return 1;
    }
    if (queue->count >= ECM_BRIDGE_QUEUE_DEPTH) {
        queue->overflow++;
        /* Queue is full — likely previous TX failed. Flush old data
           and push the new frame so it gets a chance to be sent. */
        queue->rd = queue->wr;
        queue->count = 0;
    }

    memcpy(queue->frames[queue->wr].data, data, len);
    queue->frames[queue->wr].len = len;
    if (queue == &g_target_to_host && len >= 14) {
        USB_DEBUG("Q(T2H): wr=%d len=%d cnt=%d etype=0x%04X\r\n",
                  queue->wr, len, queue->count + 1, get_be16(&data[12]));
    } else {
        USB_DEBUG("Q: wr=%d len=%d cnt=%d\r\n", queue->wr, len, queue->count + 1);
    }
    queue->wr++;
    if (queue->wr >= ECM_BRIDGE_QUEUE_DEPTH) {
        queue->wr = 0;
    }
    queue->count++;
    return 1;
}

static void queue_clear(ECM_FrameQueue_t *queue)
{
    queue->rd = 0;
    queue->wr = 0;
    queue->count = 0;
    queue->overflow = 0;
}

static void rx_frame_clear(ECM_RxFrame_t *frame)
{
    frame->len = 0;
    frame->expected_len = 0;
    frame->drop = 0;
    frame->overflow = 0;
}

static ECM_Frame_t *queue_peek(ECM_FrameQueue_t *queue)
{
    if (queue->count == 0) {
        return 0;
    }
    return &queue->frames[queue->rd];
}

static void queue_pop(ECM_FrameQueue_t *queue)
{
    if (queue->count == 0) {
        return;
    }
    queue->rd++;
    if (queue->rd >= ECM_BRIDGE_QUEUE_DEPTH) {
        queue->rd = 0;
    }
    queue->count--;
}

static uint16_t eth_frame_length(const uint8_t *data, uint16_t accumulated)
{
    /* Once we have at least 14 bytes (Ethernet header), extract the total
       frame length. */
    if (accumulated < 14) {
        return 0;
    }
    uint16_t ether = get_be16(&data[12]);
    if (ether == 0x0806) {
        /* ARP over Ethernet: fixed 28-byte payload + 14-byte header = 42 */
        return 42;
    }
    if (ether == 0x0800 && accumulated >= 18) {
        /* IPv4: Total Length is at offset 16 (2 bytes into IPv4 header,
           which is 16 bytes into the Ethernet frame).
           Total Ethernet frame = 14 + IPv4 total length. */
        uint16_t ipv4_total = get_be16(&data[16]);
        return (uint16_t)(14 + ipv4_total);
    }
    if (ether == 0x86DD && accumulated >= 20) {
        /* IPv6 payload length is at bytes 4..5 of the IPv6 header. */
        uint16_t ipv6_payload = get_be16(&data[18]);
        return (uint16_t)(14 + 40 + ipv6_payload);
    }
    if (ether < 0x0600) {
        /* IEEE 802.3 length field = payload length after 14-byte header */
        return (uint16_t)(14 + ether);
    }
    /* Unknown EtherType — can't determine length from header */
    return 0;
}

static void rx_frame_append(ECM_RxFrame_t *frame,
                            ECM_FrameQueue_t *queue,  /* NULL = drop if handler returns 0 */
                            const uint8_t *data,
                            uint16_t len,
                            uint16_t max_packet,
                            ECM_FrameHandler_t frame_handler)
{
    /* Append the chunk first (if not dropping) so we can parse headers. */
    if (!frame->drop && len > 0) {
        if ((uint32_t)frame->len + len <= ECM_BRIDGE_MTU_FRAME_SIZE) {
            memcpy(&frame->data[frame->len], data, len);
            frame->len = (uint16_t)(frame->len + len);
        }
        else {
            frame->drop = 1;
            frame->overflow++;
        }
    }

    /* Try to determine expected frame length from accumulated Ethernet data.
       This lets us handle fragmented USB transfers (e.g., 16+16+10 byte
       chunks for a 42-byte ARP frame) instead of incorrectly treating each
       short chunk as a complete frame. */
    if (frame->expected_len == 0 && !frame->drop && frame->len > 0) {
        uint16_t elen = eth_frame_length(frame->data, frame->len);
        if (elen > 0) {
            frame->expected_len = elen;
        }
    }

    uint8_t end_of_frame = 0;
    if (frame->expected_len > 0) {
        /* We know the exact frame length from Ethernet/protocol header */
        if (frame->len >= frame->expected_len) {
            end_of_frame = 1;
        }
    } else {
        /* Fallback: short-packet detection. Only fire if we either have
           enough data to have parsed headers (>= 18 bytes for IPv4) or the
           packet is too small to contain a valid Ethernet header (< 14).
           This prevents prematurely firing on fragmented EtherType frames
           where the first chunk is < max_packet but more data follows. */
        if (len < max_packet) {
            if (frame->len < 14) {
                /* Tiny fragment — treat as end of frame (likely an error) */
                end_of_frame = 1;
            } else {
                /* We have >= 14 bytes accumulated. Check if this is a known
                   EtherType where we should wait for more data. */
                uint16_t ether = get_be16(&frame->data[12]);
                if (ether == 0x0800 && frame->len < 18) {
                    /* IPv4 but not enough for Total Length field yet */
                    end_of_frame = 0;
                } else if (ether >= 0x0600 && ether != 0x0806) {
                    end_of_frame = 1;
                } else {
                    end_of_frame = 1;
                }
            }
        }
    }

    if (end_of_frame) {
        if (!frame->drop && frame->len > 0) {
            if (frame_handler == 0 || !frame_handler(frame->data, frame->len)) {
                if (queue != NULL) {
                    (void)queue_push(queue, frame->data, frame->len);
                }
            }
        }
        frame->len = 0;
        frame->drop = 0;
        frame->expected_len = 0;
    }
}

static void put_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static uint8_t make_notification(uint8_t state, uint8_t interface, uint8_t *packet)
{
    packet[0] = 0xA1;
    if (state == 1) {
        packet[1] = 0x00;
        put_u16(&packet[2], 1);
        put_u16(&packet[4], interface);
        put_u16(&packet[6], 0);
        return 8;
    }

    packet[1] = 0x2A;
    put_u16(&packet[2], 0);
    put_u16(&packet[4], interface);
    put_u16(&packet[6], 8);
    put_u32(&packet[8], ECM_BRIDGE_LINK_SPEED_BPS);
    put_u32(&packet[12], ECM_BRIDGE_LINK_SPEED_BPS);
    return 16;
}

#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
static void poll_target_notification(void)
{
    uint8_t packet[16];
    uint8_t len;

    if (g_target_notify_state == 0) {
        return;
    }

    USB_DEBUG("ECM: notif state=%d busy=%d\r\n",
              g_target_notify_state, USBFS_Endp_Busy[DEF_UEP5]);

    if (USBFS_Endp_Busy[DEF_UEP5] != 0) {
        return;
    }

    len = make_notification(g_target_notify_state, ECM_BRIDGE_TARGET_COMM_IF, packet);
    if (USBFS_Endp_DataUp(DEF_UEP5, packet, len, DEF_UEP_CPY_LOAD) == READY) {
        USB_DEBUG("ECM: notif sent state=%d len=%d\r\n", g_target_notify_state, len);
        g_target_notify_state++;
        if (g_target_notify_state > 2) {
            g_target_notify_state = 0;
        }
    }
    else {
        USB_DEBUG("ECM: notif FAILED to send\r\n");
    }
}
#endif

static void poll_host_notification(void)
{
    uint8_t packet[16];
    uint8_t len;

    if (g_host_notify_state == 0 || ECM_USBD_INT_BUSY != 0) {
        return;
    }

    len = make_notification(g_host_notify_state, ECM_BRIDGE_HOST_COMM_IF, packet);
    if (USBD_ENDPx_DataUp(ECM_USBD_INT_ENDP, packet, len) == USB_SUCCESS) {
        g_host_notify_state++;
        if (g_host_notify_state > 2) {
            g_host_notify_state = 0;
        }
    }
}

#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
static void poll_target_tx(void)
{
    ECM_Frame_t *frame;
    uint16_t len;

    /* When the CDC ECM data interface first becomes active on Target side
       (Target PC sent SET_INTERFACE on USBFS side), queue a gratuitous ARP
       so the Target PC's ARP cache gets populated immediately. */
    if (g_target_ecm_data_if_active && !g_target_garp_sent) {
        USB_DEBUG("Target ECM: IF active, queuing GARP\r\n");
        uint8_t garp[42];
        memset(garp, 0xFF, 6);
        memcpy(&garp[6], ECM_TARGET_DEV_MAC, 6);
        garp[12] = 0x08; garp[13] = 0x06;
        garp[14] = 0x00; garp[15] = 0x01;
        garp[16] = 0x08; garp[17] = 0x00;
        garp[18] = 6;  garp[19] = 4;
        garp[20] = 0x00; garp[21] = 0x02;
        memcpy(&garp[22], ECM_TARGET_DEV_MAC, 6);
        memcpy(&garp[28], ecm_target_dev_ip, 4);
        memcpy(&garp[32], ECM_TARGET_DEV_MAC, 6);
        memcpy(&garp[38], ecm_target_dev_ip, 4);
        queue_push(&g_host_to_target, garp, sizeof(garp));
        g_target_garp_sent = 1;
    }

    /* Guard against EP7 being in use — poll_host_tx() also drives EP7.
     * Without this busy check, poll_target_tx() and poll_host_tx() can
     * overwrite each other's data or send out-of-order fragments. */
    if (USBFS_Endp_Busy[DEF_UEP7] != 0) {
        return;
    }

    frame = queue_peek(&g_host_to_target);
    if (frame == 0) {
        g_target_tx_offset = 0;
        g_target_zlp_pending = 0;
        return;
    }

    if (g_target_zlp_pending) {
        if (USBFS_Endp_DataUp(DEF_UEP7, frame->data, 0, DEF_UEP_CPY_LOAD) == READY) {
            g_target_zlp_pending = 0;
            g_target_tx_offset = 0;
            queue_pop(&g_host_to_target);
        }
        return;
    }

    len = (uint16_t)(frame->len - g_target_tx_offset);
    if (len > ECM_BRIDGE_USBFS_PACKET_SIZE) {
        len = ECM_BRIDGE_USBFS_PACKET_SIZE;
    }

    if (USBFS_Endp_DataUp(DEF_UEP7, &frame->data[g_target_tx_offset], len, DEF_UEP_CPY_LOAD) == READY) {
        g_target_tx_offset = (uint16_t)(g_target_tx_offset + len);
        if (g_target_tx_offset >= frame->len) {
            if (len == ECM_BRIDGE_USBFS_PACKET_SIZE) {
                g_target_zlp_pending = 1;
            }
            else {
                g_target_tx_offset = 0;
                queue_pop(&g_host_to_target);
            }
        }
    }
}
#endif

static void poll_host_tx(void)
{
    ECM_Frame_t *frame;
    uint16_t len;

    /* When the CDC ECM data interface first becomes active (Mac sent
       SET_INTERFACE on USBD side), queue a gratuitous ARP so the Mac's
       ARP cache gets populated immediately. */
    if (g_host_ecm_data_if_active && !g_host_garp_sent) {
        USB_DEBUG("ECM: IF active, queuing GARP\r\n");
        uint8_t garp[42];
        memset(garp, 0xFF, 6);
        memcpy(&garp[6], ECM_DEV_MAC, 6);
        garp[12] = 0x08; garp[13] = 0x06;
        garp[14] = 0x00; garp[15] = 0x01;
        garp[16] = 0x08; garp[17] = 0x00;
        garp[18] = 6;  garp[19] = 4;
        garp[20] = 0x00; garp[21] = 0x02;
        memcpy(&garp[22], ECM_DEV_MAC, 6);
        memcpy(&garp[28], ecm_dev_ip, 4);
        memcpy(&garp[32], ECM_DEV_MAC, 6);
        memcpy(&garp[38], ecm_dev_ip, 4);
        queue_push(&g_target_to_host, garp, sizeof(garp));
        g_host_garp_sent = 1;
        g_ecm_tx_state = 1;
    }

    frame = queue_peek(&g_target_to_host);
    if (frame == 0) {
        g_host_tx_offset = 0;
        g_host_zlp_pending = 0;
        return;
    }

    /* Only attempt TX after the Mac has activated the CDC ECM data interface
       via SET_INTERFACE(IF1, alt=1). Without this, USBD_ENDPx_DataUp returns
       SUCCESS but the endpoint never responds to host IN tokens. */
    if (!g_host_ecm_data_if_active) {
        return;
    }

    if (ECM_USBD_BULK_IN_BUSY != 0) {
        g_ecm_tx_state = 3;
        return;
    }

    static uint8_t tx_count = 0;
    if ((++tx_count & 0x0F) == 0) {
        USB_DEBUG("ECM TX len=%d busy=%d if_active=%d\r\n", frame->len,
                   ECM_USBD_BULK_IN_BUSY, g_host_ecm_data_if_active);
    }

    if (g_host_zlp_pending) {
        g_ecm_tx_state = 2;
        if (USBD_ENDPx_DataUp(ECM_USBD_BULK_IN_ENDP, frame->data, 0) == USB_SUCCESS) {
            g_ecm_tx_count++;
            g_ecm_tx_state = 2;  /* success */
            g_host_zlp_pending = 0;
            g_host_tx_offset = 0;
            queue_pop(&g_target_to_host);
        }
        else {
            g_ecm_tx_state = 4;  /* DataUp failed */
        }
        return;
    }

    len = (uint16_t)(frame->len - g_host_tx_offset);
    if (len > ECM_BRIDGE_USBD_PACKET_SIZE) {
        len = ECM_BRIDGE_USBD_PACKET_SIZE;
    }

    if (USBD_ENDPx_DataUp(ECM_USBD_BULK_IN_ENDP, &frame->data[g_host_tx_offset], len) == USB_SUCCESS) {
        g_ecm_tx_count++;
        g_ecm_tx_state = 2;  /* success */
        g_host_tx_offset = (uint16_t)(g_host_tx_offset + len);
        if (g_host_tx_offset >= frame->len) {
            if (len == ECM_BRIDGE_USBD_PACKET_SIZE) {
                g_host_zlp_pending = 1;
            }
            else {
                g_host_tx_offset = 0;
                queue_pop(&g_target_to_host);
            }
        }
    }
}

void ECM_Bridge_Init(void)
{
    /* Load network configuration from ECM_IPConfig module (populated by ECM_IPConfig_Init) */
    ECM_IPConfig_GetHostIP(ecm_host_ip);
    ECM_IPConfig_GetDevIP(ecm_dev_ip);
    ECM_IPConfig_GetSubnetMask(ecm_subnet_mask);
    ECM_IPConfig_GetTargetHostIP(ecm_target_host_ip);
    ECM_IPConfig_GetTargetDevIP(ecm_target_dev_ip);

    /* Step 1: Verify compilation configuration */
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
    USB_DEBUG("Target CDC ECM: ENABLED\r\n");
#else
    USB_DEBUG("Target CDC ECM: DISABLED\r\n");
#endif

#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
    queue_clear(&g_host_to_target);
#endif
    queue_clear(&g_target_to_host);
    rx_frame_clear(&g_host_rx_frame);
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
    rx_frame_clear(&g_target_rx_frame);
    g_target_tx_offset = 0;
    g_target_zlp_pending = 0;
    g_target_notify_state = 1;
#endif
    g_host_tx_offset = 0;
    g_host_zlp_pending = 0;
    g_host_notify_state = 1;
    g_host_garp_sent = 0;
    g_host_ecm_data_if_active = 0;
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
    g_target_garp_sent = 0;
    g_target_ecm_data_if_active = 0;
#endif

#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
    /* Only configure USBFS EP6/EP7 if the active mode actually uses
       target-side CDC ECM. With runtime profiles, the compiled code
       exists but the descriptor may not advertise these endpoints. */
    if (USB_ModeConfig_ActiveModeHasTargetCdcEcm()) {
        /* Force enable EP7 TX at the hardware level */
        USBFSD_UEP_MOD(3) |= USBFS_UEP7_TX_EN;

        /* Also configure EP6 RX to ACK so we can receive data from the Mac
           without SET_INTERFACE. Without this, EP6 stays in NAK state and
           the Mac's bulk OUT (ARP requests etc.) is silently dropped. */
        USBFSD->UEP6_RX_CTRL = USBFS_UEP_R_RES_ACK;

        USB_DEBUG("USBFS endpoints configured:\r\n");
        USB_DEBUG("  EP6: Target data OUT (RX)\r\n");
        USB_DEBUG("  EP7: Target data IN (TX)\r\n");
    }
#endif

    USB_DEBUG("ECM init done\r\n");
    USB_DEBUG("Network config:\r\n");
    USB_DEBUG("  Host IP: %d.%d.%d.%d\r\n", ecm_host_ip[0], ecm_host_ip[1], ecm_host_ip[2], ecm_host_ip[3]);
    USB_DEBUG("  Dev IP: %d.%d.%d.%d\r\n", ecm_dev_ip[0], ecm_dev_ip[1], ecm_dev_ip[2], ecm_dev_ip[3]);
    USB_DEBUG("  Target Host IP: %d.%d.%d.%d\r\n", ecm_target_host_ip[0], ecm_target_host_ip[1], ecm_target_host_ip[2], ecm_target_host_ip[3]);
    USB_DEBUG("  Target Dev IP: %d.%d.%d.%d\r\n", ecm_target_dev_ip[0], ecm_target_dev_ip[1], ecm_target_dev_ip[2], ecm_target_dev_ip[3]);
    USB_DEBUG("Target device MAC: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
              ECM_TARGET_DEV_MAC[0], ECM_TARGET_DEV_MAC[1], ECM_TARGET_DEV_MAC[2],
              ECM_TARGET_DEV_MAC[3], ECM_TARGET_DEV_MAC[4], ECM_TARGET_DEV_MAC[5]);
}

void ECM_Bridge_UpdateIPConfig(void)
{
    /* Update bridge layer IP variables from ECM_IPConfig module's RAM cache.
       This function is called after IP configuration is modified to make
       the changes take effect immediately without requiring USB reconnection. */

    /* Load network configuration from ECM_IPConfig module */
    ECM_IPConfig_GetHostIP(ecm_host_ip);
    ECM_IPConfig_GetDevIP(ecm_dev_ip);
    ECM_IPConfig_GetSubnetMask(ecm_subnet_mask);
    ECM_IPConfig_GetTargetHostIP(ecm_target_host_ip);
    ECM_IPConfig_GetTargetDevIP(ecm_target_dev_ip);

    /* Reset GARP sent flags so that GARP packets with new IPs will be
       sent when the interface is activated */
    g_host_garp_sent = 0;
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
    g_target_garp_sent = 0;
#endif

    /* Trigger link state change to force host to re-negotiate DHCP.
       Set notification state to trigger a link down/up sequence:
       - First notification (state 1): link down
       - Second notification (state 2): link up with speed change
       This causes the host to detect a link change and renew DHCP. */
    g_host_notify_state = 1;
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
    g_target_notify_state = 1;
#endif

    USB_DEBUG("ECM IP config updated, triggering link state change\r\n");
}

void ECM_Bridge_SendRawFrame(const uint8_t *data, uint16_t len)
{
    /* Queue raw Ethernet frame directly to host_to_target (target PC),
       bypassing CH9329 protocol handler. Used by BLE-Eth TCP frame injection. */
    USB_DEBUG("ECM send: len=%d\r\n", len);
    if (len > 0 && len <= 54) {
        USB_DEBUG("  First 16: ");
        for (int i = 0; i < 16 && i < len; i++) {
            USB_DEBUG("%02x ", data[i]);
        }
        USB_DEBUG("\r\n");
    }
    uint8_t ok = queue_push(&g_host_to_target, data, len);
    USB_DEBUG("  q: %s\r\n", ok ? "OK" : "FAIL");
}

void ECM_Bridge_HostRxDone(const uint8_t *data, uint16_t len)
{
    /* Host is sending data - interface is active (bypasses missed SET_INTERFACE) */
    if (!g_host_ecm_data_if_active) {
        /* First activation after reconnect — flush stale frames from previous
           cycle (e.g. queued DHCP replies that were never consumed). */
        queue_clear(&g_target_to_host);
        g_host_garp_sent = 0;
    }
    g_host_ecm_data_if_active = 1;

    /* LED: blink ON briefly when data arrives via EP6 */
    if (len > 0) g_ecm_led_override = 1;
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
    rx_frame_append(&g_host_rx_frame, &g_host_to_target, data, len,
                    ECM_BRIDGE_USBD_PACKET_SIZE, handle_host_control_frame);
#else
    rx_frame_append(&g_host_rx_frame, NULL, data, len,
                    ECM_BRIDGE_USBD_PACKET_SIZE, handle_host_control_frame);
#endif
}

#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
void ECM_Bridge_TargetRxDone(const uint8_t *data, uint16_t len)
{
    /* Step 2: Verify data reception */
    USB_DEBUG("Target RX: len=%d\r\n", len);
    if (len >= 14) {
        USB_DEBUG("  Dst MAC: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                  data[0], data[1], data[2], data[3], data[4], data[5]);
        USB_DEBUG("  Src MAC: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                  data[6], data[7], data[8], data[9], data[10], data[11]);
        USB_DEBUG("  EthType: 0x%02x%02x\r\n", data[12], data[13]);
    }

    /* Target is sending data - interface is active (bypasses missed SET_INTERFACE) */
    g_target_ecm_data_if_active = 1;

    rx_frame_append(&g_target_rx_frame, &g_target_to_host, data, len,
                    ECM_BRIDGE_USBFS_PACKET_SIZE, handle_target_frame);
}

static uint8_t handle_target_frame(const uint8_t *frame, uint16_t frame_len)
{
    /* Step 3: Verify DHCP processing function */
    USB_DEBUG("Target frame: len=%d\r\n", frame_len);

    /* Handle DHCP requests from Target PC first (DHCP before ARP) */
    if (handle_target_dhcp_packet(frame, frame_len)) {
        USB_DEBUG("DHCP response queued\r\n");
        return 1;  /* DHCP reply queued, don't queue original */
    }

    /* Handle ICMP Echo Request (ping) from Target PC */
    if (handle_target_icmp_packet(frame, frame_len)) {
        return 1;  /* ICMP reply queued, don't queue original */
    }

    /* Handle ARP requests from Target PC */
    if (handle_target_arp_frame(frame, frame_len)) {
        return 1;  /* ARP reply queued, don't queue original */
    }

    /* Handle ARP replies from Target PC - forward to host side */
    if (frame_len >= 28) {
        uint16_t eth_type = (uint16_t)(frame[12] << 8) | frame[13];
        if (eth_type == 0x0806) {  /* ARP */
            uint16_t arp_op = (uint16_t)(frame[20] << 8) | frame[21];
            if (arp_op == 2) {  /* ARP reply */
                USB_DEBUG("Target ARP reply, forwarding to host\r\n");
                queue_push(&g_target_to_host, frame, frame_len);
                return 1;
            }
        }
    }

    /* Try to match frame to a BLE-Eth tunnel and push TCP payload back via BLE */
    if (BLE_Eth_HandleTargetFrame(frame, frame_len) && g_ble_tx_event_cb) {
        g_ble_tx_event_cb();
    }
    /* Return 0 to forward the frame to host side (Mac) via g_target_to_host queue */
    return 0;
}
#endif

void ECM_Bridge_Poll(void)
{
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
    poll_target_notification();
#endif
    poll_host_notification();
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
    poll_target_tx();
#endif
    poll_host_tx();
}

#endif /* USB_WORK_MODE_COMPILES_BRIDGE_ECM */

/* Diagnostic/debug variables — always defined so other files can reference
 * them even when COMPILES_BRIDGE_ECM=0. When ECM is not compiled they stay
 * at their initial values (unused). */
volatile uint8_t g_host_ecm_data_if_active = 0;
volatile uint8_t g_target_ecm_data_if_active = 0;
volatile uint16_t g_ecm_tx_count = 0;
volatile uint8_t g_ecm_led_override = 0;
volatile uint8_t g_dhcp_led_mode = 0;
volatile uint8_t g_host_garp_sent = 0;
volatile uint8_t g_target_garp_sent = 0;
volatile uint8_t g_ecm_tx_state = 0;
/* Flag indicating ECM bridge is currently processing a CH9329 command via TCP frame.
 * Set to 1 when about to call CH9329_RxBuffer_Add, cleared after response is handled.
 * Used to prevent race conditions with USB queue processing of CH9329 responses. */
volatile uint8_t g_ecm_bridge_processing_ch9329 = 0;