/********************************** (C) COPYRIGHT *******************************
 * File Name          : ncm_bridge.c
 * Description        : CDC-NCM (Network Control Model) bridge implementation.
 *                      Provides USB networking with NTB (Network Transfer Block)
 *                      framing for native Windows 10/11 support via usbncm.sys.
 *********************************************************************************
 * This implementation starts with single-frame NTBs for simplicity.
 * Multi-frame NTB batching can be added later for performance optimization.
 *******************************************************************************/

#include "include/ncm_bridge.h"
#include "include/Version_selection.h"
#include "include/usb_mode_config.h"
#include "include/ecm_ip_config.h"

#if USB_WORK_MODE_COMPILES_BRIDGE_NCM

#include "ch32v20x_usbfs_device.h"
#include "usb_lib.h"
#include "usb_conf.h"

/* External systick_ms from RGB.c */
extern volatile uint32_t systick_ms;

#include "usb_desc.h"
#include "usb_prop.h"
#include "usb_pwr.h"
#include "include/usbd_composite_km.h"
#include "include/km_ring.h"
#include "include/ble_eth_protocol.h"
#include <string.h>

/* Debug output macro */
#ifndef USB_DEBUG
#define USB_DEBUG(...)
#endif

/*******************************************************************************
 * NCM Protocol Constants
 ******************************************************************************/
#define NCM_BRIDGE_MTU_FRAME_SIZE       1514u
#define NCM_BRIDGE_QUEUE_DEPTH          2u
#define NCM_BRIDGE_USBFS_PACKET_SIZE    64u
#define NCM_BRIDGE_USBD_PACKET_SIZE     64u
#define NCM_BRIDGE_HOST_COMM_IF         0u
#define NCM_BRIDGE_LINK_SPEED_BPS       12000000u

/* Ethernet protocol types */
#define NCM_ETH_TYPE_IPV4               0x0800u
#define NCM_ETH_TYPE_VLAN               0x8100u
#define NCM_ETH_TYPE_QINQ               0x88A8u
#define NCM_IPV4_PROTO_TCP              0x06u
#define NCM_IPV4_PROTO_UDP              0x11u
#define NCM_IPV4_PROTO_ICMP             0x01u

/* ICMP constants */
#define NCM_ICMP_ECHO_REPLY             0u
#define NCM_ICMP_ECHO_REQUEST           8u
#define NCM_ETH_HEADER_LEN              14u
#define NCM_IPV4_HEADER_LEN_OFFSET      16u
#define NCM_IPV4_MIN_HEADER_LEN         20u

/* DHCP constants */
#define NCM_DHCP_CLIENT_PORT            68u
#define NCM_DHCP_SERVER_PORT            67u
#define NCM_DHCP_MAGIC_COOKIE           0x63825363u
#define NCM_DHCP_DISCOVER               1u
#define NCM_DHCP_OFFER                  2u
#define NCM_DHCP_REQUEST                3u
#define NCM_DHCP_ACK                    5u
#define NCM_DHCP_OPT_SUBNET             1u
#define NCM_DHCP_OPT_ROUTER             3u
#define NCM_DHCP_OPT_DNS                6u
#define NCM_DHCP_OPT_REQUESTED_IP       50u
#define NCM_DHCP_OPT_LEASE_TIME         51u
#define NCM_DHCP_OPT_MSG_TYPE           53u
#define NCM_DHCP_OPT_SERVER_ID          54u
#define NCM_DHCP_OPT_END                255u

/* CH9329 frame constants */
#define NCM_CH9329_FRAME_HEAD1          0x57
#define NCM_CH9329_FRAME_HEAD2          0xAB
#define NCM_CH9329_MIN_FRAME_LEN        6u

/* ARP constants */
#define NCM_ARP_FRAME_LEN               42u

/*******************************************************************************
 * Diagnostic Variables
 ******************************************************************************/
volatile uint8_t g_host_ncm_data_if_active = 0;
volatile uint16_t g_ncm_tx_count = 0;
volatile uint8_t g_ncm_garp_sent = 0;
volatile uint8_t g_ncm_ntb_sequence = 0;

/* USB endpoint busy flags (external) */
extern uint8_t ECM_USBD_INT_BUSY;
extern uint8_t ECM_USBD_BULK_IN_BUSY;
extern uint8_t USBD_Endp4_Busy;

/*******************************************************************************
 * Runtime Network Parameters
 ******************************************************************************/
static uint8_t ncm_host_ip[4];
static uint8_t ncm_dev_ip[4];
static uint8_t ncm_subnet_mask[4];

/* Firmware-side peer identity on the CDC NCM link */
static const uint8_t NCM_DEV_MAC[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x32 };

/*******************************************************************************
 * Frame Queue Structures
 ******************************************************************************/
typedef struct {
    uint8_t data[NCM_BRIDGE_MTU_FRAME_SIZE];
    uint16_t len;
} NCM_Frame_t;

typedef struct {
    NCM_Frame_t frames[NCM_BRIDGE_QUEUE_DEPTH];
    volatile uint8_t rd;
    volatile uint8_t wr;
    volatile uint8_t count;
    volatile uint8_t in_flight;
    volatile uint16_t gen;
    volatile uint32_t overflow;
} NCM_FrameQueue_t;

typedef struct {
    uint8_t data[NCM_BRIDGE_USBD_PACKET_SIZE];
    uint16_t len;
    uint16_t frame_len;
    uint16_t gen;
    uint8_t zlp;
    const NCM_Frame_t *head;
} NCM_TxChunk_t;

/* Critical section helpers */
static inline uint32_t ncm_irq_save(void)
{
    uint32_t csr;
    __asm volatile ("csrr %0, 0x800" : "=r"(csr));
    __disable_irq();
    return (csr >> 3) & 0x1u;
}

static inline void ncm_irq_restore(uint32_t state)
{
    if (state) {
        __enable_irq();
    }
}

typedef struct {
    uint8_t data[NCM_BRIDGE_MTU_FRAME_SIZE];
    uint16_t len;
    uint16_t expected_len;
    uint8_t drop;
    volatile uint32_t overflow;
} NCM_RxFrame_t;

/* Link notification states */
#define NCM_NOTIFY_DOWN     1u
#define NCM_NOTIFY_UP       2u
#define NCM_NOTIFY_SPEED    3u
#define NCM_NOTIFY_BEACON   4u

#define NCM_LINK_DOWN_HOLD_MS   800u
#define NCM_LINK_UP_BEACON_MS   1000u

/*******************************************************************************
 * Global State Variables
 ******************************************************************************/
static NCM_FrameQueue_t g_host_tx_queue;
static NCM_RxFrame_t g_host_rx_frame;
static uint16_t g_host_tx_offset;
static uint16_t g_host_tx_gen;
static uint8_t g_host_zlp_pending;
static uint8_t g_host_notify_state;
static uint32_t g_host_notify_last_send_ms;
static uint32_t g_host_notify_hold_until_ms;

/* Callback for BLE TX events */
static void (*g_ble_tx_event_cb)(void);

/*******************************************************************************
 * Helper Functions
 ******************************************************************************/
static uint16_t get_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static void put_be16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)(value & 0xFF);
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

/*******************************************************************************
 * NTB Builder - Single Frame (Phase 1: Simplest implementation)
 ******************************************************************************/

/**
 * Build an NTB containing a single Ethernet frame.
 *
 * NTB Structure:
 * [NTB Header (14 bytes)][NDP (16 bytes for 1 frame)][Frame data][Padding]
 *
 * @param ntb_buf     Output buffer for NTB
 * @param max_size    Maximum NTB size
 * @param frames      Array of frame pointers (single frame in Phase 1)
 * @param frame_lens  Array of frame lengths
 * @param frame_count Number of frames (1 in Phase 1)
 * @param sequence    NTB sequence number
 * @return            Total NTB length, or 0 on error
 */
uint16_t NCM_BuildNtb(uint8_t *ntb_buf, uint16_t max_size,
                       const uint8_t **frames, const uint16_t *frame_lens,
                       uint8_t frame_count, uint16_t sequence)
{
    uint16_t ntb_len;
    uint16_t ndp_offset;
    uint16_t data_offset;
    uint16_t padded_frame_len;
    uint16_t frame_len;
    const uint8_t *frame;

    if (!ntb_buf || !frames || !frame_lens || frame_count == 0) {
        return 0;
    }

    /* Phase 1: Only handle single frame */
    frame = frames[0];
    frame_len = frame_lens[0];

    if (!frame || frame_len == 0) {
        return 0;
    }

    /* Calculate padded frame length (4-byte alignment required by NCM) */
    padded_frame_len = (frame_len + 3) & ~3;

    /* Calculate total NTB length */
    ntb_len = NCM_NTB_HEADER_SIZE + NCM_NDP_HEADER_SIZE +
              ((frame_count + 1) * NCM_NDP_ENTRY_SIZE) +  /* entries + terminator */
              padded_frame_len;

    if (ntb_len > max_size) {
        return 0;  /* Frame too large */
    }

    /* Offsets */
    ndp_offset = NCM_NTB_HEADER_SIZE;
    data_offset = ndp_offset + NCM_NDP_HEADER_SIZE + ((frame_count + 1) * NCM_NDP_ENTRY_SIZE);

    /* Fill NTB Header */
    put_u16(&ntb_buf[0], ntb_len);      /* wBlockLength */
    put_u16(&ntb_buf[2], sequence);     /* wSequence */
    put_u16(&ntb_buf[4], ndp_offset);   /* wNdpIndex */
    put_u16(&ntb_buf[6], 0);            /* wNdpIndexAlt = 0 */
    ntb_buf[8] = 0; ntb_buf[9] = 0;     /* Reserved */
    ntb_buf[10] = 0; ntb_buf[11] = 0;
    ntb_buf[12] = 0; ntb_buf[13] = 0;

    /* Fill NDP (Network Datagram Pointer) */
    ntb_buf[ndp_offset + 0] = 'N';      /* dwSignature = 'NCM0' */
    ntb_buf[ndp_offset + 1] = 'C';
    ntb_buf[ndp_offset + 2] = 'M';
    ntb_buf[ndp_offset + 3] = '0';
    put_u16(&ntb_buf[ndp_offset + 4], NCM_NDP_HEADER_SIZE + ((frame_count + 1) * NCM_NDP_ENTRY_SIZE));  /* wLength */
    put_u16(&ntb_buf[ndp_offset + 6], 0);  /* wNextNdpIndex = 0 (last) */

    /* NDP Entry 0: datagram index and length */
    put_u16(&ntb_buf[ndp_offset + 8], data_offset);   /* wDatagramIndex[0] */
    put_u16(&ntb_buf[ndp_offset + 10], frame_len);    /* wDatagramLength[0] */

    /* NDP Terminator */
    put_u16(&ntb_buf[ndp_offset + 12], 0);  /* wDatagramIndex[1] = 0 */
    put_u16(&ntb_buf[ndp_offset + 14], 0);  /* wDatagramLength[1] = 0 */

    /* Copy frame data */
    memcpy(&ntb_buf[data_offset], frame, frame_len);

    /* Zero-pad to 4-byte boundary */
    if (padded_frame_len > frame_len) {
        memset(&ntb_buf[data_offset + frame_len], 0, padded_frame_len - frame_len);
    }

    return ntb_len;
}

/*******************************************************************************
 * NTB Parser
 ******************************************************************************/

/**
 * Parse an NTB and extract Ethernet frames.
 *
 * @param ntb_buf        Input NTB buffer
 * @param ntb_len        Length of NTB
 * @param frame_callback Callback for each extracted frame
 * @return               Number of frames extracted, or 0 on error
 */
uint8_t NCM_ParseNtb(const uint8_t *ntb_buf, uint16_t ntb_len,
                     uint8_t (*frame_callback)(const uint8_t *frame, uint16_t len))
{
    uint16_t block_len, ndp_index;
    uint8_t frame_count = 0;
    uint16_t entry_offset;

    if (!ntb_buf || ntb_len < NCM_NTB_HEADER_SIZE) {
        return 0;
    }

    /* Parse NTB header */
    block_len = ntb_buf[0] | ((uint16_t)ntb_buf[1] << 8);
    ndp_index = ntb_buf[4] | ((uint16_t)ntb_buf[5] << 8);

    if (block_len > ntb_len) {
        USB_DEBUG("NCM: NTB block_len %d > ntb_len %d\r\n", block_len, ntb_len);
        return 0;
    }
    if (ndp_index >= ntb_len || ndp_index < NCM_NTB_HEADER_SIZE) {
        USB_DEBUG("NCM: NDP index %d out of range\r\n", ndp_index);
        return 0;
    }

    /* Verify NDP signature 'NCM0' */
    if (ntb_buf[ndp_index] != 'N' || ntb_buf[ndp_index + 1] != 'C' ||
        ntb_buf[ndp_index + 2] != 'M' || ntb_buf[ndp_index + 3] != '0') {
        USB_DEBUG("NCM: Invalid NDP signature\r\n");
        return 0;
    }

    /* Parse NDP entries */
    entry_offset = ndp_index + NCM_NDP_HEADER_SIZE;

    while (entry_offset + 4 <= ntb_len) {
        uint16_t dg_index = ntb_buf[entry_offset] | ((uint16_t)ntb_buf[entry_offset + 1] << 8);
        uint16_t dg_length = ntb_buf[entry_offset + 2] | ((uint16_t)ntb_buf[entry_offset + 3] << 8);

        entry_offset += NCM_NDP_ENTRY_SIZE;

        /* Terminator */
        if (dg_index == 0 && dg_length == 0) {
            break;
        }

        /* Validate datagram bounds */
        if (dg_index + dg_length > ntb_len) {
            USB_DEBUG("NCM: Datagram out of bounds\r\n");
            continue;
        }

        /* Process frame via callback */
        if (frame_callback) {
            frame_callback(&ntb_buf[dg_index], dg_length);
        }
        frame_count++;
    }

    return frame_count;
}

/*******************************************************************************
 * Frame Queue Operations
 ******************************************************************************/
static uint8_t queue_push(NCM_FrameQueue_t *queue, const uint8_t *data, uint16_t len)
{
    uint32_t irq_state = ncm_irq_save();

    if (queue->count >= NCM_BRIDGE_QUEUE_DEPTH) {
        queue->overflow++;
        ncm_irq_restore(irq_state);
        return 0;
    }

    memcpy(queue->frames[queue->wr].data, data, len);
    queue->frames[queue->wr].len = len;
    queue->wr = (queue->wr + 1) % NCM_BRIDGE_QUEUE_DEPTH;
    queue->count++;

    ncm_irq_restore(irq_state);
    return 1;
}

static void queue_clear(NCM_FrameQueue_t *queue)
{
    uint32_t irq_state = ncm_irq_save();
    queue->rd = 0;
    queue->wr = 0;
    queue->count = 0;
    queue->gen++;
    ncm_irq_restore(irq_state);
}

/*******************************************************************************
 * ARP Handler
 ******************************************************************************/
static uint8_t handle_arp_frame(const uint8_t *frame, uint16_t frame_len)
{
    uint16_t eth_type;
    uint16_t arp_op;

    if (frame_len < 28) {
        return 0;
    }

    eth_type = (uint16_t)(frame[12] << 8) | frame[13];
    if (eth_type != 0x0806) {
        return 0;
    }

    arp_op = (uint16_t)(frame[20] << 8) | frame[21];

    /* Handle ARP requests for our device IP */
    if (arp_op == 1) {  /* ARP request */
        uint8_t is_dev_ip = (frame[38] == ncm_dev_ip[0] && frame[39] == ncm_dev_ip[1] &&
                             frame[40] == ncm_dev_ip[2] && frame[41] == ncm_dev_ip[3]);

        if (!is_dev_ip) {
            return 0;
        }

        USB_DEBUG("NCM: ARP req matched, queuing reply\r\n");

        /* Build ARP reply */
        uint8_t reply[42];
        memcpy(&reply[0], &frame[6], 6);   /* dst MAC = requestor MAC */
        memcpy(&reply[6], NCM_DEV_MAC, 6); /* src MAC = our MAC */
        reply[12] = 0x08; reply[13] = 0x06; /* EtherType = ARP */
        reply[14] = 0x00; reply[15] = 0x01; /* hardware type = Ethernet */
        reply[16] = 0x08; reply[17] = 0x00; /* protocol type = IPv4 */
        reply[18] = 6;  reply[19] = 4;     /* MAC len=6, IP len=4 */
        reply[20] = 0x00; reply[21] = 0x02; /* op = ARP reply */
        memcpy(&reply[22], NCM_DEV_MAC, 6);  /* sender MAC */
        memcpy(&reply[28], ncm_dev_ip, 4);   /* sender IP */
        memcpy(&reply[32], &frame[22], 6);   /* target MAC (from request) */
        memcpy(&reply[38], &frame[28], 4);   /* target IP = requestor IP */

        queue_push(&g_host_tx_queue, reply, sizeof(reply));
        return 1;
    }

    /* Handle ARP replies - pass to BLE-Eth for connection resolution */
    if (arp_op == 2) {  /* ARP reply */
        uint8_t sender_ip[4], sender_mac[6];
        memcpy(sender_mac, &frame[22], 6);
        memcpy(sender_ip, &frame[28], 4);
        if (BLE_Eth_HandleArpReply(sender_ip, sender_mac)) {
            USB_DEBUG("NCM: ARP reply processed\r\n");
            if (g_ble_tx_event_cb) {
                g_ble_tx_event_cb();
            }
        }
    }

    return 0;
}

/*******************************************************************************
 * ICMP Handler
 ******************************************************************************/
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

    if (frame == NULL || frame_len < NCM_ETH_HEADER_LEN) {
        return 0;
    }

    /* Parse Ethernet header */
    eth_offset = NCM_ETH_HEADER_LEN;
    eth_type = get_be16(&frame[12]);
    while ((eth_type == NCM_ETH_TYPE_VLAN || eth_type == NCM_ETH_TYPE_QINQ) &&
           frame_len >= (uint16_t)(eth_offset + 4u)) {
        eth_type = get_be16(&frame[eth_offset + 2u]);
        eth_offset = (uint16_t)(eth_offset + 4u);
    }

    if (eth_type != NCM_ETH_TYPE_IPV4 ||
        frame_len < (uint16_t)(eth_offset + NCM_IPV4_MIN_HEADER_LEN)) {
        return 0;
    }

    /* Parse IPv4 header */
    ip = &frame[eth_offset];
    if ((ip[0] >> 4) != 4u) {
        return 0;
    }

    ip_header_len = (uint16_t)((ip[0] & 0x0Fu) * 4u);
    if (ip_header_len < NCM_IPV4_MIN_HEADER_LEN) {
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
    if (ip[9] != NCM_IPV4_PROTO_ICMP) {
        return 0;
    }

    /* Check if destination IP is our IP */
    const uint8_t *dst_ip = &ip[16];
    if (memcmp(dst_ip, ncm_dev_ip, 4) != 0 &&
        memcmp(dst_ip, ncm_host_ip, 4) != 0) {
        return 0;
    }

    /* Parse ICMP header */
    icmp = &ip[ip_header_len];
    icmp_len = (uint16_t)(ip_total_len - ip_header_len);
    icmp_type = icmp[0];

    /* Only handle Echo Request (type 8) */
    if (icmp_type != NCM_ICMP_ECHO_REQUEST) {
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
    memcpy(&reply[off], ip, ip_header_len);
    memcpy(&reply[off + 12], &ip[16], 4);  /* src = original dst IP */
    memcpy(&reply[off + 16], &ip[12], 4);  /* dst = original src IP */
    reply[off + 9] = NCM_IPV4_PROTO_ICMP;
    reply[off + 10] = 0;
    reply[off + 11] = 0;
    off += ip_header_len;

    /* ICMP header */
    icmp_start = off;
    memcpy(&reply[off], icmp, icmp_len);
    reply[off] = NCM_ICMP_ECHO_REPLY;
    reply[off + 2] = 0;
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

    USB_DEBUG("NCM: ICMP Echo Reply queued, len=%d\r\n", off);
    queue_clear(&g_host_tx_queue);
    queue_push(&g_host_tx_queue, reply, off);
    return 1;
}

/*******************************************************************************
 * Host Frame Handler (RX from Windows host)
 ******************************************************************************/
static uint8_t handle_host_frame(const uint8_t *data, uint16_t len)
{
    uint16_t eth_type;

    if (len < NCM_ETH_HEADER_LEN) {
        return 0;
    }

    eth_type = (uint16_t)(data[12] << 8) | data[13];

    /* Handle ARP */
    if (eth_type == 0x0806) {
        return handle_arp_frame(data, len);
    }

    /* Handle IPv4 */
    if (eth_type == NCM_ETH_TYPE_IPV4) {
        /* Check for ICMP */
        if (handle_icmp_packet(data, len)) {
            return 1;
        }
        /* TODO: Handle DHCP, TCP, UDP */
    }

    /* Frame not handled - could forward to target or BLE */
    return 0;
}

/*******************************************************************************
 * Notification Endpoint Handling
 ******************************************************************************/
static uint8_t make_notification(uint8_t state, uint8_t interface, uint8_t *packet)
{
    packet[0] = 0xA1;

    if (state == NCM_NOTIFY_DOWN) {
        packet[1] = 0x00;
        put_u16(&packet[2], 0);
        put_u16(&packet[4], interface);
        put_u16(&packet[6], 0);
        return 8;
    }

    if (state == NCM_NOTIFY_UP || state == NCM_NOTIFY_BEACON) {
        packet[1] = 0x00;
        put_u16(&packet[2], 1);
        put_u16(&packet[4], interface);
        put_u16(&packet[6], 0);
        return 8;
    }

    if (state == NCM_NOTIFY_SPEED) {
        packet[1] = 0x2A;
        put_u16(&packet[2], 0);
        put_u16(&packet[4], interface);
        put_u16(&packet[6], 8);
        put_u32(&packet[8], NCM_BRIDGE_LINK_SPEED_BPS);
        put_u32(&packet[12], NCM_BRIDGE_LINK_SPEED_BPS);
        return 16;
    }

    return 0;
}

static void poll_host_notification(void)
{
    uint8_t packet[16];
    uint8_t len;

    if (g_host_notify_state == 0 || ECM_USBD_INT_BUSY != 0) {
        return;
    }

    uint32_t now = systick_ms;

    if ((int32_t)(now - g_host_notify_hold_until_ms) < 0) {
        return;
    }

    if (now - g_host_notify_last_send_ms < 50) {
        return;
    }

    len = make_notification(g_host_notify_state, NCM_BRIDGE_HOST_COMM_IF, packet);
    /* TODO: Send via USBD notification endpoint */
    if (len > 0) {
        g_host_notify_last_send_ms = now;
        if (g_host_notify_state == 1) {
            g_host_notify_state = 2;
            g_host_notify_hold_until_ms = now + NCM_LINK_DOWN_HOLD_MS;
        } else if (g_host_notify_state >= 3) {
            g_host_notify_state = 0;
            g_ncm_garp_sent = 0;
        } else {
            g_host_notify_state++;
        }
    }
}

/*******************************************************************************
 * Public API
 ******************************************************************************/
void NCM_Bridge_Init(void)
{
    memset(&g_host_tx_queue, 0, sizeof(g_host_tx_queue));
    memset(&g_host_rx_frame, 0, sizeof(g_host_rx_frame));
    g_host_tx_offset = 0;
    g_host_tx_gen = 0;
    g_host_zlp_pending = 0;
    g_host_notify_state = 0;
    g_host_notify_last_send_ms = 0;
    g_host_notify_hold_until_ms = 0;
    g_ncm_ntb_sequence = 0;

    /* Load IP config */
    ECM_IPConfig_GetHostIP(ncm_host_ip);
    ECM_IPConfig_GetDevIP(ncm_dev_ip);
    ECM_IPConfig_GetSubnetMask(ncm_subnet_mask);

    USB_DEBUG("NCM: Bridge initialized\r\n");
}

void NCM_Bridge_SetBleTxCallback(void (*cb)(void))
{
    g_ble_tx_event_cb = cb;
}

void NCM_Bridge_HostRxDone(const uint8_t *data, uint16_t len)
{
    /* Parse NTB and process frames */
    NCM_ParseNtb(data, len, handle_host_frame);
}

void NCM_Bridge_Poll(void)
{
    poll_host_notification();

    /* TX path: if EP4 IN is free and we have queued frames, build and send an NTB */
    if (!USBD_Endp4_Busy && g_host_tx_queue.count > 0) {
        /* Build NTB from one frame (single-frame NTBs for Phase 1) */
        uint8_t ntb_buf[NCM_NTB_MAX_SIZE];
        const uint8_t *frame_ptrs[1];
        uint16_t frame_lens[1];
        uint8_t frame_count = 0;

        /* Peek the head frame */
        NCM_Frame_t *frame = &g_host_tx_queue.frames[g_host_tx_queue.rd];
        if (frame->len > 0) {
            frame_ptrs[0] = frame->data;
            frame_lens[0] = frame->len;
            frame_count = 1;
        }

        if (frame_count > 0) {
            uint16_t ntb_len = NCM_BuildNtb(ntb_buf, sizeof(ntb_buf),
                                             frame_ptrs, frame_lens,
                                             frame_count, g_ncm_ntb_sequence);
            g_ncm_ntb_sequence++;

            /* Send NTB via EP4 IN */
            extern uint8_t USBD_ENDPx_DataUp(uint8_t endp, uint8_t *pbuf, uint16_t len);
            if (USBD_ENDPx_DataUp(ENDP4, ntb_buf, ntb_len) == USB_SUCCESS) {
                USBD_Endp4_Busy = 1;
                /* Pop the frame from queue */
                uint32_t csr = ncm_irq_save();
                g_host_tx_queue.count--;
                g_host_tx_queue.rd = (g_host_tx_queue.rd + 1) % NCM_BRIDGE_QUEUE_DEPTH;
                ncm_irq_restore(csr);
            }
        }
    }
}

void NCM_Bridge_UpdateIPConfig(void)
{
    ECM_IPConfig_GetHostIP(ncm_host_ip);
    ECM_IPConfig_GetDevIP(ncm_dev_ip);
    ECM_IPConfig_GetSubnetMask(ncm_subnet_mask);
    USB_DEBUG("NCM: IP config updated\r\n");
}

/* Stub implementations for unused functions */
void NCM_Bridge_InitTarget(void) { }
void NCM_Bridge_TargetRxDone(const uint8_t *data, uint16_t len) { (void)data; (void)len; }
void NCM_Bridge_Target_RearmLinkNotify(void) { }
uint8_t NCM_Bridge_SendRawFrame(const uint8_t *data, uint16_t len)
{
    (void)data; (void)len;
    return 0;
}

#endif /* USB_WORK_MODE_COMPILES_BRIDGE_NCM */
