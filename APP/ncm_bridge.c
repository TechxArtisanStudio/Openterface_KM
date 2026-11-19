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
#define USB_DEBUG(...)  printf(__VA_ARGS__)
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
extern uint8_t USBD_Endp4_Busy;
extern uint8_t USBD_Endp5_Busy;

/*******************************************************************************
 * Runtime Network Parameters
 ******************************************************************************/
static uint8_t ncm_host_ip[4];
static uint8_t ncm_dev_ip[4];
static uint8_t ncm_subnet_mask[4];

/* Target-side network parameters */
static uint8_t ncm_target_host_ip[4];
static uint8_t ncm_target_dev_ip[4];

/* Firmware-side peer identity on the CDC NCM link */
static const uint8_t NCM_DEV_MAC[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x32 };

/* Target-side MAC (different from host-side to avoid self-MAC filtering) */
static const uint8_t NCM_TARGET_DEV_MAC[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x33 };

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
#define NCM_LINK_UP_DELAY_MS    100u   /* Delay between DOWN and UP notifications */
#define NCM_LINK_SPEED_DELAY_MS 50u    /* Delay between UP and SPEED notifications */
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

/* Target-side NCM state (forward declarations for use in NCM_Bridge_Poll) */
static NCM_FrameQueue_t g_target_tx_queue;
static NCM_RxFrame_t g_target_rx_frame;

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
 * DHCP Handler (Host-facing)
 ******************************************************************************/
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
    static uint8_t reply[576];
    uint16_t off;
    uint16_t dhcp_off;
    uint16_t udp_payload_len;
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

    /* Check if UDP */
    if (ip[9] != NCM_IPV4_PROTO_UDP) {
        return 0;
    }

    /* Parse UDP header */
    udp = &ip[ip_header_len];
    udp_len = get_be16(&udp[4]);
    if (udp_len < 8u || udp_len > (uint16_t)(ip_total_len - ip_header_len)) {
        return 0;
    }

    /* Check if destination port is 67 (DHCP server) */
    if (get_be16(&udp[2]) != NCM_DHCP_SERVER_PORT) {
        return 0;
    }

    /* Parse DHCP message */
    const uint8_t *dhcp = &udp[8];

    if (udp_len < 8u + 240u) {
        return 0;
    }

    /* Check magic cookie */
    uint32_t cookie = ((uint32_t)dhcp[236] << 24) | ((uint32_t)dhcp[237] << 16) |
                      ((uint32_t)dhcp[238] << 8) | dhcp[239];
    if (cookie != NCM_DHCP_MAGIC_COOKIE) {
        return 0;
    }

    /* Parse DHCP options to find message type and requested IP */
    uint16_t opt_off = 240;
    dhcp_msg_type = 0;
    while (opt_off < udp_len - 8u) {
        uint8_t opt_code = dhcp[opt_off];
        if (opt_code == NCM_DHCP_OPT_END) {
            break;
        }
        if (opt_off + 1 >= udp_len - 8u) {
            break;
        }
        uint8_t opt_len = dhcp[opt_off + 1];
        if (opt_off + 2 + opt_len > udp_len - 8u) {
            break;
        }

        if (opt_code == NCM_DHCP_OPT_MSG_TYPE && opt_len == 1) {
            dhcp_msg_type = dhcp[opt_off + 2];
        } else if (opt_code == NCM_DHCP_OPT_REQUESTED_IP && opt_len == 4) {
            memcpy(requested_ip, &dhcp[opt_off + 2], 4);
            has_requested_ip = 1;
        }

        opt_off = (uint16_t)(opt_off + 2 + opt_len);
    }

    if (dhcp_msg_type != NCM_DHCP_DISCOVER && dhcp_msg_type != NCM_DHCP_REQUEST) {
        return 0;
    }

    USB_DEBUG("NCM: DHCP type=%d\r\n", dhcp_msg_type);

    /* Build DHCP response (ACK) */
    memset(reply, 0, sizeof(reply));
    off = 0;

    /* Determine if broadcast reply */
    uint8_t ciaddr[4];
    uint8_t broadcast_reply;
    memcpy(ciaddr, &dhcp[12], 4);
    broadcast_reply = (dhcp_msg_type == NCM_DHCP_DISCOVER) ||
                      ((get_be16(&dhcp[10]) & 0x8000u) != 0);

    /* Ethernet header */
    if (broadcast_reply) {
        memset(&reply[off], 0xFF, 6);  /* dst = broadcast */
    } else {
        memcpy(&reply[off], &frame[6], 6);  /* dst = original src */
    }
    off += 6;
    memcpy(&reply[off], NCM_DEV_MAC, 6); /* src = firmware device MAC */
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
    reply[off + 9] = NCM_IPV4_PROTO_UDP;  /* protocol = UDP */
    /* src IP = firmware device IP */
    memcpy(&reply[off + 12], ncm_dev_ip, 4);
    /* dst IP */
    if (broadcast_reply) {
        memset(&reply[off + 16], 0xFF, 4);  /* broadcast */
    } else if ((ciaddr[0] | ciaddr[1] | ciaddr[2] | ciaddr[3]) != 0) {
        memcpy(&reply[off + 16], ciaddr, 4);
    } else {
        memcpy(&reply[off + 16], ncm_host_ip, 4);
    }
    off += 20;

    /* UDP header (8 bytes) */
    uint16_t udp_start = off;
    put_be16(&reply[off + 0], NCM_DHCP_SERVER_PORT);  /* src port */
    put_be16(&reply[off + 2], NCM_DHCP_CLIENT_PORT);  /* dst port */
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
    /* flags (2 bytes) - copy from request */
    memcpy(&reply[off], &dhcp[10], 2);
    off += 2;
    /* ciaddr (4 bytes) */
    memset(&reply[off], 0, 4);
    off += 4;
    /* yiaddr (4 bytes) - IP offered to client */
    memcpy(&reply[off], ncm_host_ip, 4);
    off += 4;
    /* siaddr (4 bytes) - server IP */
    memcpy(&reply[off], ncm_dev_ip, 4);
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
    /* Message type: OFFER for DISCOVER, ACK for REQUEST */
    {
        uint8_t reply_type = (dhcp_msg_type == NCM_DHCP_DISCOVER)
            ? NCM_DHCP_OFFER : NCM_DHCP_ACK;
        reply[off++] = NCM_DHCP_OPT_MSG_TYPE;
        reply[off++] = 0x01;
        reply[off++] = reply_type;
    }

    /* Server identifier */
    reply[off++] = NCM_DHCP_OPT_SERVER_ID;
    reply[off++] = 0x04;
    memcpy(&reply[off], ncm_dev_ip, 4);
    off += 4;

    /* Lease time: 1 hour */
    reply[off++] = NCM_DHCP_OPT_LEASE_TIME;
    reply[off++] = 0x04;
    reply[off++] = 0x00;
    reply[off++] = 0x00;
    reply[off++] = 0x0E;
    reply[off++] = 0x10;  /* 3600 seconds */

    /* Subnet mask */
    reply[off++] = NCM_DHCP_OPT_SUBNET;
    reply[off++] = 0x04;
    memcpy(&reply[off], ncm_subnet_mask, 4);
    off += 4;

    /* DNS server - use public DNS */
    reply[off++] = NCM_DHCP_OPT_DNS;
    reply[off++] = 0x04;
    uint8_t public_dns[4] = { 8, 8, 8, 8 };
    memcpy(&reply[off], public_dns, 4);
    off += 4;

    /* End option */
    reply[off++] = NCM_DHCP_OPT_END;

    /* Pad DHCP message to minimum 300 bytes */
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
    sum += NCM_IPV4_PROTO_UDP;
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

    USB_DEBUG("NCM: DHCP ACK queued, len=%d\r\n", off);
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
        /* Check for DHCP */
        if (handle_dhcp_packet(data, len)) {
            return 1;
        }
        /* TODO: Handle TCP, UDP */
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

    if (g_host_notify_state == 0 || NCM_USBD_INT_BUSY != 0) {
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
    if (len > 0 && USBD_ENDPx_DataUp(NCM_USBD_INT_ENDP, packet, len) == USB_SUCCESS) {
        NCM_USBD_INT_BUSY = 1;
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
    g_ncm_ntb_sequence = 0;

    /* Start link notification sequence: DOWN -> UP -> SPEED -> BEACON */
    g_host_notify_state = NCM_NOTIFY_DOWN;
    g_host_notify_last_send_ms = 0;
    g_host_notify_hold_until_ms = 0;

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

/* Debug counter for NCM_Bridge_Poll calls */
volatile uint32_t g_ncm_bridge_poll_count = 0;

void NCM_Bridge_Poll(void)
{
    g_ncm_bridge_poll_count++;
    poll_host_notification();
    NCM_Bridge_Target_NotificationPoll();

    /* Host-facing TX path (USBD controller) */
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

    /* Target-facing TX path (USBFS controller) */
    if (USBFS_Endp_Busy[DEF_UEP7] == 0 && g_target_tx_queue.count > 0) {
        /* Build NTB from one frame */
        uint8_t ntb_buf[NCM_NTB_MAX_SIZE];
        const uint8_t *frame_ptrs[1];
        uint16_t frame_lens[1];
        uint8_t frame_count = 0;

        NCM_Frame_t *frame = &g_target_tx_queue.frames[g_target_tx_queue.rd];
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

            /* Send NTB via EP7 IN */
            if (USBFS_Endp_DataUp(DEF_UEP7, ntb_buf, ntb_len, DEF_UEP_CPY_LOAD) == READY) {
                g_ncm_target_tx_count++;
                /* Pop the frame from queue */
                uint32_t csr = ncm_irq_save();
                g_target_tx_queue.count--;
                g_target_tx_queue.rd = (g_target_tx_queue.rd + 1) % NCM_BRIDGE_QUEUE_DEPTH;
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
    ECM_IPConfig_GetTargetHostIP(ncm_target_host_ip);
    ECM_IPConfig_GetTargetDevIP(ncm_target_dev_ip);
    USB_DEBUG("NCM: IP config updated\r\n");
}

/* Target-side NCM state variables */
volatile uint8_t g_target_ncm_notify_state = 0;
volatile uint32_t g_target_ncm_notify_hold_until_ms = 0;
volatile uint16_t g_target_ncm_notify_tx_count = 0;
volatile uint8_t g_target_ncm_data_if_active = 0;
volatile uint16_t g_ncm_target_tx_count = 0;

/* Debug counters for notification poll */
volatile uint32_t g_ncm_notify_poll_count = 0;
volatile uint32_t g_ncm_notify_state0_count = 0;
volatile uint32_t g_ncm_notify_hold_count = 0;
volatile uint32_t g_ncm_notify_busy_count = 0;
volatile uint32_t g_ncm_notify_noready_count = 0;

static uint8_t g_target_ncm_notify_last_send_ms = 0;

/* Target-side interface number for NCM communication (IF4 in our descriptor) */
#define NCM_BRIDGE_TARGET_COMM_IF    4u

/**
 * Build NCM notification packet (same format as host-side).
 */
static uint8_t make_target_notification(uint8_t state, uint8_t interface, uint8_t *packet)
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

/**
 * Poll target-side notification endpoint (EP6 IN) - sends NCM link notifications.
 */
void NCM_Bridge_Target_NotificationPoll(void)
{
    uint8_t packet[16];
    uint8_t len;
    uint32_t now = systick_ms;
    uint8_t result;

    g_ncm_notify_poll_count++;

    if (g_target_ncm_notify_state == 0) {
        g_ncm_notify_state0_count++;
        return;
    }

    if ((int32_t)(now - g_target_ncm_notify_hold_until_ms) < 0) {
        g_ncm_notify_hold_count++;
        return;
    }

    /* USBFS EP6 IN busy check */
    if (USBFS_Endp_Busy[DEF_UEP6] != 0) {
        g_ncm_notify_busy_count++;
        USB_DEBUG("NCM: EP6 busy=%d, skipping notification\r\n", USBFS_Endp_Busy[DEF_UEP6]);
        return;
    }

    len = make_target_notification(g_target_ncm_notify_state, NCM_BRIDGE_TARGET_COMM_IF, packet);
    USB_DEBUG("NCM: Sending notification state=%d len=%d\r\n", g_target_ncm_notify_state, len);
    if (len > 0) {
        result = USBFS_Endp_DataUp(DEF_UEP6, packet, len, DEF_UEP_CPY_LOAD);
        USB_DEBUG("NCM: Endp_DataUp result=%d (READY=%d)\r\n", result, READY);
        if (result == READY) {
            USB_DEBUG("NCM: Notification sent, state=%d\r\n", g_target_ncm_notify_state);
            g_target_ncm_notify_last_send_ms = now;
            g_target_ncm_notify_tx_count++;
            if (g_target_ncm_notify_state == NCM_NOTIFY_DOWN) {
                g_target_ncm_notify_state = NCM_NOTIFY_UP;
                g_target_ncm_notify_hold_until_ms = now + NCM_LINK_UP_DELAY_MS;
            } else if (g_target_ncm_notify_state == NCM_NOTIFY_UP) {
                g_target_ncm_notify_state = NCM_NOTIFY_SPEED;
                g_target_ncm_notify_hold_until_ms = now + NCM_LINK_SPEED_DELAY_MS;
            } else if (g_target_ncm_notify_state == NCM_NOTIFY_SPEED) {
                g_target_ncm_notify_state = NCM_NOTIFY_BEACON;
                g_target_ncm_notify_hold_until_ms = now + NCM_LINK_UP_BEACON_MS;
            } else if (g_target_ncm_notify_state == NCM_NOTIFY_BEACON) {
                g_target_ncm_notify_state = 0;  /* Done */
            }
        } else {
            g_ncm_notify_noready_count++;
            USB_DEBUG("NCM: Failed to send notification, result=%d\r\n", result);
        }
    }
}

/**
 * Re-arm target-side link notification (called on SET_INTERFACE(alt=1)).
 */
void NCM_Bridge_Target_RearmLinkNotify(void)
{
    g_target_ncm_notify_state = NCM_NOTIFY_UP;
    g_target_ncm_notify_hold_until_ms = systick_ms + 100;
}

/*******************************************************************************
 * Target-side ARP Handler
 ******************************************************************************/
static uint8_t handle_target_arp_frame(const uint8_t *frame, uint16_t frame_len)
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

    /* Handle ARP requests for our target device IP */
    if (arp_op == 1) {  /* ARP request */
        uint8_t is_dev_ip = (frame[38] == ncm_target_dev_ip[0] && frame[39] == ncm_target_dev_ip[1] &&
                             frame[40] == ncm_target_dev_ip[2] && frame[41] == ncm_target_dev_ip[3]);

        if (!is_dev_ip) {
            return 0;
        }

        USB_DEBUG("NCM: Target ARP req matched, queuing reply\r\n");

        /* Build ARP reply */
        uint8_t reply[42];
        memcpy(&reply[0], &frame[6], 6);   /* dst MAC = requestor MAC */
        memcpy(&reply[6], NCM_TARGET_DEV_MAC, 6); /* src MAC = our target MAC */
        reply[12] = 0x08; reply[13] = 0x06; /* EtherType = ARP */
        reply[14] = 0x00; reply[15] = 0x01; /* hardware type = Ethernet */
        reply[16] = 0x08; reply[17] = 0x00; /* protocol type = IPv4 */
        reply[18] = 6;  reply[19] = 4;     /* MAC len=6, IP len=4 */
        reply[20] = 0x00; reply[21] = 0x02; /* op = ARP reply */
        memcpy(&reply[22], NCM_TARGET_DEV_MAC, 6);  /* sender MAC */
        memcpy(&reply[28], ncm_target_dev_ip, 4);   /* sender IP */
        memcpy(&reply[32], &frame[22], 6);   /* target MAC (from request) */
        memcpy(&reply[38], &frame[28], 4);   /* target IP = requestor IP */

        queue_push(&g_target_tx_queue, reply, sizeof(reply));
        return 1;
    }

    /* Handle ARP replies - pass to BLE-Eth for connection resolution */
    if (arp_op == 2) {  /* ARP reply */
        uint8_t sender_ip[4], sender_mac[6];
        memcpy(sender_mac, &frame[22], 6);
        memcpy(sender_ip, &frame[28], 4);
        if (BLE_Eth_HandleArpReply(sender_ip, sender_mac)) {
            USB_DEBUG("NCM: Target ARP reply processed\r\n");
            if (g_ble_tx_event_cb) {
                g_ble_tx_event_cb();
            }
        }
    }

    return 0;
}

/*******************************************************************************
 * Target-side DHCP Handler
 ******************************************************************************/
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
    static uint8_t reply[576];
    uint16_t off;
    uint16_t dhcp_off;
    uint16_t udp_payload_len;
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

    /* Check if UDP */
    if (ip[9] != NCM_IPV4_PROTO_UDP) {
        return 0;
    }

    /* Parse UDP header */
    udp = &ip[ip_header_len];
    udp_len = get_be16(&udp[4]);
    if (udp_len < 8u || udp_len > (uint16_t)(ip_total_len - ip_header_len)) {
        return 0;
    }

    /* Check if destination port is 67 (DHCP server) */
    if (get_be16(&udp[2]) != NCM_DHCP_SERVER_PORT) {
        return 0;
    }

    /* Parse DHCP message */
    const uint8_t *dhcp = &udp[8];

    if (udp_len < 8u + 240u) {
        return 0;
    }

    /* Check magic cookie */
    uint32_t cookie = ((uint32_t)dhcp[236] << 24) | ((uint32_t)dhcp[237] << 16) |
                      ((uint32_t)dhcp[238] << 8) | dhcp[239];
    if (cookie != NCM_DHCP_MAGIC_COOKIE) {
        return 0;
    }

    /* Parse DHCP options to find message type */
    uint16_t opt_off = 240;
    dhcp_msg_type = 0;
    while (opt_off < udp_len - 8u) {
        uint8_t opt_code = dhcp[opt_off];
        if (opt_code == NCM_DHCP_OPT_END) {
            break;
        }
        if (opt_off + 1 >= udp_len - 8u) {
            break;
        }
        uint8_t opt_len = dhcp[opt_off + 1];
        if (opt_off + 2 + opt_len > udp_len - 8u) {
            break;
        }

        if (opt_code == NCM_DHCP_OPT_MSG_TYPE && opt_len == 1) {
            dhcp_msg_type = dhcp[opt_off + 2];
        }

        opt_off = (uint16_t)(opt_off + 2 + opt_len);
    }

    if (dhcp_msg_type != NCM_DHCP_DISCOVER && dhcp_msg_type != NCM_DHCP_REQUEST) {
        return 0;
    }

    USB_DEBUG("NCM: Target DHCP type=%d\r\n", dhcp_msg_type);

    /* Build DHCP response (ACK) */
    memset(reply, 0, sizeof(reply));
    off = 0;

    /* Determine if broadcast reply */
    uint8_t ciaddr[4];
    uint8_t broadcast_reply;
    memcpy(ciaddr, &dhcp[12], 4);
    broadcast_reply = (dhcp_msg_type == NCM_DHCP_DISCOVER) ||
                      ((get_be16(&dhcp[10]) & 0x8000u) != 0);

    /* Ethernet header */
    if (broadcast_reply) {
        memset(&reply[off], 0xFF, 6);  /* dst = broadcast */
    } else {
        memcpy(&reply[off], &frame[6], 6);  /* dst = original src */
    }
    off += 6;
    memcpy(&reply[off], NCM_TARGET_DEV_MAC, 6); /* src = firmware target MAC */
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
    reply[off + 9] = NCM_IPV4_PROTO_UDP;  /* protocol = UDP */
    /* src IP = firmware target device IP */
    memcpy(&reply[off + 12], ncm_target_dev_ip, 4);
    /* dst IP */
    if (broadcast_reply) {
        memset(&reply[off + 16], 0xFF, 4);  /* broadcast */
    } else if ((ciaddr[0] | ciaddr[1] | ciaddr[2] | ciaddr[3]) != 0) {
        memcpy(&reply[off + 16], ciaddr, 4);
    } else {
        memcpy(&reply[off + 16], ncm_target_host_ip, 4);
    }
    off += 20;

    /* UDP header (8 bytes) */
    uint16_t udp_start = off;
    put_be16(&reply[off + 0], NCM_DHCP_SERVER_PORT);  /* src port */
    put_be16(&reply[off + 2], NCM_DHCP_CLIENT_PORT);  /* dst port */
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
    /* flags (2 bytes) - copy from request */
    memcpy(&reply[off], &dhcp[10], 2);
    off += 2;
    /* ciaddr (4 bytes) */
    memset(&reply[off], 0, 4);
    off += 4;
    /* yiaddr (4 bytes) - IP offered to client (target host IP) */
    memcpy(&reply[off], ncm_target_host_ip, 4);
    off += 4;
    /* siaddr (4 bytes) - server IP */
    memcpy(&reply[off], ncm_target_dev_ip, 4);
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
    /* Message type: OFFER for DISCOVER, ACK for REQUEST */
    {
        uint8_t reply_type = (dhcp_msg_type == NCM_DHCP_DISCOVER)
            ? NCM_DHCP_OFFER : NCM_DHCP_ACK;
        reply[off++] = NCM_DHCP_OPT_MSG_TYPE;
        reply[off++] = 0x01;
        reply[off++] = reply_type;
    }

    /* Server identifier */
    reply[off++] = NCM_DHCP_OPT_SERVER_ID;
    reply[off++] = 0x04;
    memcpy(&reply[off], ncm_target_dev_ip, 4);
    off += 4;

    /* Lease time: 1 hour */
    reply[off++] = NCM_DHCP_OPT_LEASE_TIME;
    reply[off++] = 0x04;
    reply[off++] = 0x00;
    reply[off++] = 0x00;
    reply[off++] = 0x0E;
    reply[off++] = 0x10;  /* 3600 seconds */

    /* Subnet mask */
    reply[off++] = NCM_DHCP_OPT_SUBNET;
    reply[off++] = 0x04;
    memcpy(&reply[off], ncm_subnet_mask, 4);
    off += 4;

    /* DNS server - use public DNS */
    reply[off++] = NCM_DHCP_OPT_DNS;
    reply[off++] = 0x04;
    uint8_t public_dns[4] = { 8, 8, 8, 8 };
    memcpy(&reply[off], public_dns, 4);
    off += 4;

    /* End option */
    reply[off++] = NCM_DHCP_OPT_END;

    /* Pad DHCP message to minimum 300 bytes */
    while ((off - dhcp_off) < 300) {
        reply[off++] = 0x00;
    }

    /* Fill UDP length */
    udp_payload_len = (uint16_t)(off - udp_start);
    put_be16(&reply[udp_start + 4], udp_payload_len);

    /* Calculate UDP checksum */
    sum = 0;
    sum += (uint16_t)(reply[ip_start + 12] << 8) | reply[ip_start + 13];
    sum += (uint16_t)(reply[ip_start + 14] << 8) | reply[ip_start + 15];
    sum += (uint16_t)(reply[ip_start + 16] << 8) | reply[ip_start + 17];
    sum += (uint16_t)(reply[ip_start + 18] << 8) | reply[ip_start + 19];
    sum += NCM_IPV4_PROTO_UDP;
    sum += udp_payload_len;
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

    USB_DEBUG("NCM: Target DHCP ACK queued, len=%d\r\n", off);
    queue_clear(&g_target_tx_queue);
    queue_push(&g_target_tx_queue, reply, off);
    return 1;
}

/*******************************************************************************
 * Target-side Frame Handler (RX from Target PC)
 ******************************************************************************/
static uint8_t handle_target_frame(const uint8_t *data, uint16_t len)
{
    uint16_t eth_type;

    if (len < NCM_ETH_HEADER_LEN) {
        return 0;
    }

    eth_type = (uint16_t)(data[12] << 8) | data[13];

    /* Handle ARP */
    if (eth_type == 0x0806) {
        return handle_target_arp_frame(data, len);
    }

    /* Handle IPv4 */
    if (eth_type == NCM_ETH_TYPE_IPV4) {
        /* Check for DHCP */
        if (handle_target_dhcp_packet(data, len)) {
            return 1;
        }
        /* TODO: Handle ICMP, TCP, UDP */
    }

    /* Frame not handled */
    return 0;
}

/**
 * Handle data received from target via EP5 OUT (NTB data).
 */
void NCM_Bridge_TargetRxDone(const uint8_t *data, uint16_t len)
{
    g_target_ncm_data_if_active = 1;
    NCM_ParseNtb(data, len, handle_target_frame);
}

/**
 * Initialize target-side NCM state.
 */
void NCM_Bridge_InitTarget(void)
{
    USB_DEBUG("NCM: InitTarget called\r\n");
    memset(&g_target_tx_queue, 0, sizeof(g_target_tx_queue));
    memset(&g_target_rx_frame, 0, sizeof(g_target_rx_frame));

    /* Start link notification sequence: DOWN -> UP -> SPEED -> BEACON */
    g_target_ncm_notify_state = NCM_NOTIFY_DOWN;
    g_target_ncm_notify_hold_until_ms = 0;
    g_target_ncm_notify_tx_count = 0;
    g_target_ncm_data_if_active = 0;
    g_ncm_target_tx_count = 0;

    /* Load target-side IP config */
    ECM_IPConfig_GetTargetHostIP(ncm_target_host_ip);
    ECM_IPConfig_GetTargetDevIP(ncm_target_dev_ip);
    USB_DEBUG("NCM: Target IP=%d.%d.%d.%d\r\n",
              ncm_target_dev_ip[0], ncm_target_dev_ip[1],
              ncm_target_dev_ip[2], ncm_target_dev_ip[3]);
}

uint8_t NCM_Bridge_SendRawFrame(const uint8_t *data, uint16_t len)
{
    if (USBFS_Endp_Busy[DEF_UEP7] != 0 || g_target_tx_queue.count >= NCM_BRIDGE_QUEUE_DEPTH) {
        return 0;
    }

    NCM_Frame_t *frame = &g_target_tx_queue.frames[g_target_tx_queue.wr];
    if (len > sizeof(frame->data)) {
        len = sizeof(frame->data);
    }
    memcpy(frame->data, data, len);
    frame->len = len;

    uint32_t csr = ncm_irq_save();
    g_target_tx_queue.count++;
    g_target_tx_queue.wr = (g_target_tx_queue.wr + 1) % NCM_BRIDGE_QUEUE_DEPTH;
    ncm_irq_restore(csr);

    return 1;
}

#endif /* USB_WORK_MODE_COMPILES_BRIDGE_NCM */
