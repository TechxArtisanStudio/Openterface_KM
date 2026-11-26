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
#include "include/hid_launcher.h"
#include <string.h>

/* Debug output macro */
#ifndef USB_DEBUG
#define USB_DEBUG(...)  printf(__VA_ARGS__)
#endif

/* Forward declarations for target-facing notification state */
volatile uint8_t g_target_ncm_notify_state = 1;  /* NCM_NOTIFY_DOWN */
volatile uint32_t g_target_ncm_notify_hold_until_ms = 0;
volatile uint16_t g_target_ncm_notify_tx_count = 0;
volatile uint32_t g_ncm_bus_reset_count = 0;
volatile uint32_t g_ncm_init_target_count = 0;

/*******************************************************************************
 * NCM Protocol Constants
 ******************************************************************************/
#define NCM_BRIDGE_MTU_FRAME_SIZE       1514u
#define NCM_BRIDGE_QUEUE_DEPTH          2u
#define NCM_BRIDGE_USBFS_PACKET_SIZE    64u
/* Host-facing (USBLIB) bulk packet size — PMA buffers are 64 bytes. */
#define NCM_BRIDGE_HOST_PACKET_SIZE     64u
#define NCM_BRIDGE_USBD_PACKET_SIZE     64u
#define NCM_BRIDGE_HOST_COMM_IF         0u
#define NCM_BRIDGE_LINK_SPEED_BPS       12000000u

/* Ethernet protocol types */
#define NCM_ETH_TYPE_IPV4               0x0800u
#define NCM_ETH_TYPE_IPV6               0x86DDu
#define NCM_ETH_TYPE_VLAN               0x8100u
/* IPv4 destination address = IP header + 16 (frame offset 30). */
#define NCM_IPV4_DST_OFFSET             16u
/* IPv6 destination address = IPv6 fixed header(8) + src(16) (frame offset 38). */
#define NCM_IPV6_DST_OFFSET             24u
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
 * Host-Side Diagnostics (memory instrumentation, no printf).
 * Counters are incremented at key points of the host-facing NCM data path
 * and read out over the serial API via CMD_ISP_DEBUG sub_cmd 0x10.
 ******************************************************************************/
typedef struct {
    volatile uint16_t ep3_rx_pkts;   /* EP3 OUT CTR packets received        */
    volatile uint16_t ntb_sig_ok;    /* packets starting with "NCMH"        */
    volatile uint16_t ntb_sig_bad;   /* bad signature / bad wBlockLength    */
    volatile uint16_t ntb_parsed;    /* completed NTBs handed to parser     */
    volatile uint16_t host_frames;   /* Ethernet frames delivered           */
    volatile uint16_t arp_req;       /* ARP requests seen                   */
    volatile uint16_t arp_match;     /* ARP requests for our device IP      */
    volatile uint16_t arp_reply;     /* ARP replies queued                  */
    volatile uint16_t icmp_echo;     /* ICMP echo replies queued            */
    volatile uint16_t dhcp_offer;    /* DHCP OFFERs queued                  */
    volatile uint16_t tx_sent;       /* NTBs completed on EP4               */
    volatile uint16_t zlp_sent;      /* terminating ZLPs sent               */
    volatile uint16_t rx_oflow;      /* packets dropped (oversize)          */
    volatile uint16_t poll_calls;    /* NCM_Bridge_Poll invocations (hi 8)  */
} NCM_HostDiag_t;

static NCM_HostDiag_t g_host_diag;

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

/* USBFS TX: NTB is built here and copied into the EP7 buffer by
 * USBFS_Endp_DataUp(); the controller may transfer after NCM_Bridge_Poll
 * returns, so a stack buffer would be unsafe. */
static uint8_t  s_target_ntb_tx[NCM_NTB_MAX_SIZE];
static uint16_t s_target_ntb_seq;
static uint8_t  s_target_zlp_pending;

/* USBFS loads exactly one MPS per DataUp() into the endpoint buffer; a
 * longer TX_LEN is emitted as a single oversized packet, which the host
 * reports as babble (EOVERFLOW) and quickly disables the port.  A queued
 * NTB is therefore handed to EP7 one 64-byte packet at a time. */
static uint16_t s_target_ntb_tx_len;   /* current NTB length, 0 = idle */
static uint16_t s_target_ntb_tx_off;   /* bytes already given to USBFS */

/* USBFS RX: EP5 OUT delivers one 64-byte packet per interrupt, so an NTB
 * arriving as several packets must be reassembled before parsing. */
static uint8_t  s_target_rx_buf[NCM_NTB_MAX_SIZE];
static uint16_t s_target_rx_len;
static uint16_t s_target_rx_need;

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
    uint16_t src_len;
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

    /* Ethernet frames shorter than 60 bytes must be zero-padded on the wire;
     * the datagram length advertised in the NDP includes that padding. */
    if (frame_len < 60u) {
        frame_len = 60u;
    }

    /* Calculate padded frame length (4-byte alignment required by NCM) */
    padded_frame_len = (frame_len + 3) & ~3;
    src_len = frame_lens[0];

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

    /* Fill NTB Header according to CDC NCM spec (NTB-16 format):
     * - bytes 0-3: dwSignature ('NCMH' = 0x484D434E)
     * - bytes 4-5: wHeaderLength (12)
     * - bytes 6-7: wSequence
     * - bytes 8-9: wBlockLength
     * - bytes 10-11: wNdpIndex
     */
    ntb_buf[0] = 'N';   /* dwSignature = 'NCMH' */
    ntb_buf[1] = 'C';
    ntb_buf[2] = 'M';
    ntb_buf[3] = 'H';
    put_u16(&ntb_buf[4], 12);           /* wHeaderLength = 12 */
    put_u16(&ntb_buf[6], sequence);     /* wSequence */
    put_u16(&ntb_buf[8], ntb_len);      /* wBlockLength */
    put_u16(&ntb_buf[10], ndp_offset);  /* wNdpIndex */

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
    memcpy(&ntb_buf[data_offset], frame, src_len);

    /* Zero-pad the frame to 60 bytes if needed, then to 4-byte alignment */
    if (padded_frame_len > src_len) {
        memset(&ntb_buf[data_offset + src_len], 0, padded_frame_len - src_len);
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

    if (!ntb_buf || ntb_len < 12) {
        return 0;
    }

    /* Parse NTB header according to CDC NCM spec (section 3.2.1):
     * - bytes 0-3: dwSignature ('NCMH' = 0x484D434E)
     * - bytes 4-5: wHeaderLength (typically 12)
     * - bytes 6-7: wSequence
     * - bytes 8-9: wBlockLength
     * - bytes 10-11: wNdpIndex
     */
    /* Verify signature 'NCMH' */
    if (ntb_buf[0] != 0x4E || ntb_buf[1] != 0x43 ||
        ntb_buf[2] != 0x4D || ntb_buf[3] != 0x48) {
        USB_DEBUG("NCM: Invalid NTB signature\r\n");
        return 0;
    }

    block_len = ntb_buf[8] | ((uint16_t)ntb_buf[9] << 8);
    ndp_index = ntb_buf[10] | ((uint16_t)ntb_buf[11] << 8);

    if (block_len > ntb_len) {
        USB_DEBUG("NCM: NTB block_len %d > ntb_len %d\r\n", block_len, ntb_len);
        return 0;
    }
    if (ndp_index >= ntb_len || ndp_index < 12) {
        USB_DEBUG("NCM: NDP index %d out of range\r\n", ndp_index);
        return 0;
    }

    /* Verify NDP signature 'NCM0' */
    if (ntb_buf[ndp_index] != 0x4E || ntb_buf[ndp_index + 1] != 0x43 ||
        ntb_buf[ndp_index + 2] != 0x4D || ntb_buf[ndp_index + 3] != 0x30) {
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

/* L2 bridge: peer MACs learned from the first frames on each side, and a
 * queue push that rewrites the Ethernet header for the destination link.
 * The two NCM ports share an IP subnet but use different device MACs, so a
 * transparent bridge has to swap the src/dst MAC on every forwarded frame. */
static uint8_t s_host_peer_mac[6];
static uint8_t s_target_peer_mac[6];
static uint8_t s_bridge_enabled = 1u;
volatile uint16_t g_bridge_host_to_target;
volatile uint16_t g_bridge_target_to_host;
volatile uint16_t g_bridge_arp_proxy;
/* Target-RX instrumentation (readable via diag sub 0x13): every frame handed
 * to handle_target_frame, and how many of them were discovery forwards. */
volatile uint16_t g_target_diag_frames;
volatile uint16_t g_target_diag_discovery;

static uint8_t queue_push_bridge(NCM_FrameQueue_t *queue, const uint8_t *data,
                                 uint16_t len, const uint8_t *dst_mac,
                                 const uint8_t *src_mac)
{
    uint32_t irq_state = ncm_irq_save();
    if (queue->count >= NCM_BRIDGE_QUEUE_DEPTH || len > NCM_BRIDGE_MTU_FRAME_SIZE) {
        queue->overflow++;
        ncm_irq_restore(irq_state);
        return 0;
    }
    NCM_Frame_t *f = &queue->frames[queue->wr];
    memcpy(f->data, data, len);
    memcpy(&f->data[0], dst_mac, 6);
    memcpy(&f->data[6], src_mac, 6);
    f->len = len;
    queue->wr = (queue->wr + 1u) % NCM_BRIDGE_QUEUE_DEPTH;
    queue->count++;
    ncm_irq_restore(irq_state);
    return 1;
}

/*******************************************************************************
 * Discovery frame forwarding
 *
 * Windows Network discovery relies on broadcast/multicast traffic:
 *   NetBIOS name service 255.255.255.255 / UDP 137-138
 *   LLMNR                224.0.0.252
 *   WS-Discovery / SSDP  239.255.255.250 / UDP 3702,1900
 *   IPv6 equivalent      ff02::c
 * The unicast-only L2 bridge never saw these frames, so the Host with the
 * file share was invisible in "Network" even though http://192.168.20.1:8080
 * worked. Forward broadcast/multicast frames at L2 with proper multicast MAC
 * translation; unicast forwarding behaviour stays unchanged.
 ******************************************************************************/
static uint8_t bridge_ipv4_is_broadcast(const uint8_t *dst)
{
    if (dst[0] == 0xFFu && dst[1] == 0xFFu && dst[2] == 0xFFu && dst[3] == 0xFFu) {
        return 1u;
    }
    /* Directed broadcast for the shared /24 subnet. */
    if (dst[0] == ncm_host_ip[0] && dst[1] == ncm_host_ip[1] &&
        dst[2] == ncm_host_ip[2] && dst[3] == 0xFFu) {
        return 1u;
    }
    return 0u;
}

static uint8_t bridge_frame_is_discovery(const uint8_t *frame, uint16_t len,
                                         uint8_t dst_mac[6])
{
    uint16_t eth_type;
    const uint8_t *dst4;

    if (frame == NULL || len < (uint16_t)(NCM_ETH_HEADER_LEN + 20u)) {
        return 0u;
    }

    eth_type = (uint16_t)(frame[12] << 8) | frame[13];

    if (eth_type == NCM_ETH_TYPE_IPV4) {
        /* Frame must hold the full IP header before the dst address is read. */
        if (len < (uint16_t)(NCM_ETH_HEADER_LEN + NCM_IPV4_DST_OFFSET + 4u)) {
            return 0u;
        }
        dst4 = &frame[NCM_ETH_HEADER_LEN + NCM_IPV4_DST_OFFSET];
        if (bridge_ipv4_is_broadcast(dst4) || (dst4[0] & 0xF0u) == 0xE0u) {
            if (bridge_ipv4_is_broadcast(dst4)) {
                memset(dst_mac, 0xFF, 6);
            } else {
                /* IPv4 multicast MAC: 01:00:5e + low 23 bits of the group. */
                dst_mac[0] = 0x01u; dst_mac[1] = 0x00u; dst_mac[2] = 0x5Eu;
                dst_mac[3] = dst4[1] & 0x7Fu;
                dst_mac[4] = dst4[2];
                dst_mac[5] = dst4[3];
            }
            return 1u;
        }
        return 0u;
    }

    if (eth_type == NCM_ETH_TYPE_IPV6) {
        /* Fixed IPv6 header is 40 bytes; the destination address starts at
         * frame offset 14 + 24 = 38. */
        const uint8_t *dst6;
        if (len < (uint16_t)(NCM_ETH_HEADER_LEN + NCM_IPV6_DST_OFFSET + 16u)) {
            return 0u;
        }
        dst6 = &frame[NCM_ETH_HEADER_LEN + NCM_IPV6_DST_OFFSET];
        if (dst6[0] != 0xFFu) {
            return 0u;
        }
        /* IPv6 multicast MAC: 33:33 + low 32 bits of the group address. */
        dst_mac[0] = 0x33u; dst_mac[1] = 0x33u;
        dst_mac[2] = dst6[12]; dst_mac[3] = dst6[13];
        dst_mac[4] = dst6[14]; dst_mac[5] = dst6[15];
        return 1u;
    }

    return 0u;
}

static uint8_t bridge_forward_discovery(const uint8_t *frame, uint16_t len,
                                        NCM_FrameQueue_t *queue,
                                        const uint8_t *src_mac)
{
    uint8_t dst_mac[6];

    if (!s_bridge_enabled || !bridge_frame_is_discovery(frame, len, dst_mac)) {
        return 0u;
    }

    /* Never echo a frame that came from one of our own NCM ports. */
    if (memcmp(&frame[6], NCM_DEV_MAC, 6) == 0 ||
        memcmp(&frame[6], NCM_TARGET_DEV_MAC, 6) == 0) {
        return 0u;
    }

    return queue_push_bridge(queue, frame, len, dst_mac, src_mac);
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
        uint8_t is_dev_ip;

        g_host_diag.arp_req++;
        is_dev_ip = (frame[38] == ncm_dev_ip[0] && frame[39] == ncm_dev_ip[1] &&
                     frame[40] == ncm_dev_ip[2] && frame[41] == ncm_dev_ip[3]);

        if (!is_dev_ip) {
            /* Proxy-ARP for the target PC: tell the Host we own its IP so its
             * HTTP packets are sent to us for bridging. */
            if (s_bridge_enabled && memcmp(&frame[38], ncm_target_host_ip, 4) == 0) {
                uint8_t reply[42];
                memcpy(&reply[0], &frame[6], 6);
                memcpy(&reply[6], NCM_DEV_MAC, 6);
                reply[12] = 0x08; reply[13] = 0x06;
                reply[14] = 0x00; reply[15] = 0x01;
                reply[16] = 0x08; reply[17] = 0x00;
                reply[18] = 6; reply[19] = 4;
                reply[20] = 0x00; reply[21] = 0x02;
                memcpy(&reply[22], NCM_DEV_MAC, 6);
                memcpy(&reply[28], ncm_target_host_ip, 4);
                memcpy(&reply[32], &frame[22], 6);
                memcpy(&reply[38], &frame[28], 4);
                queue_push(&g_host_tx_queue, reply, sizeof(reply));
                g_bridge_arp_proxy++;
                return 1;
            }
            return 0;
        }
        g_host_diag.arp_match++;

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
        g_host_diag.arp_reply++;
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
/*******************************************************************************
 * Captive-portal DNS hijack
 *
 * The bridge intercepts every DNS query (UDP port 53) that crosses the
 * link — regardless of which DNS server the peer configured — and answers
 * it locally with the portal address. The peer OS then fails its own
 * connectivity probe (msftconnecttest / captive.apple.com / generate_204),
 * flags the network as a captive portal and pops up its built-in sign-in
 * window pointing at the file manager. UDP only; the HTTP 302 hop stays on
 * the Host (portal_redirect.py). Configurable via ECM_IPConfig portal
 * fields (0x33 sub-commands 0x14/0x15); portal_ip 0.0.0.0 follows host_ip.
 ******************************************************************************/
#define NCM_PORTAL_MAX_REPLY  512u

static volatile uint16_t g_portal_hijacked;

static uint8_t handle_dns_hijack(const uint8_t *data, uint16_t len,
                                 NCM_FrameQueue_t *queue,
                                 const uint8_t *dev_mac)
{
    static uint8_t reply[NCM_PORTAL_MAX_REPLY];
    uint8_t enabled, mode, portal_ip[4], host_ip[4];
    const uint8_t *answer_ip;
    uint16_t eth_type, ihl, udp_off, dns_off, src_port, dst_port;
    uint16_t dns_flags, qdcount, question_off, question_len;
    uint16_t udp_len, ip_total, off;
    uint32_t sum;
    uint16_t chk;
    uint8_t i;

    if (!s_bridge_enabled || len < (uint16_t)(NCM_ETH_HEADER_LEN + 20u + 8u + 12u)) {
        return 0;
    }
    eth_type = get_be16(&data[12]);
    if (eth_type != NCM_ETH_TYPE_IPV4 || data[14] != 0x45u) {
        return 0;
    }
    if (data[NCM_ETH_HEADER_LEN + 9] != 17u) {          /* UDP only */
        return 0;
    }

    udp_off = NCM_ETH_HEADER_LEN + 20u;
    src_port = get_be16(&data[udp_off]);
    dst_port = get_be16(&data[udp_off + 2]);
    if (dst_port != 53u || src_port == 53u) {           /* queries only */
        return 0;
    }

    dns_off = udp_off + 8u;
    if (len < (uint16_t)(dns_off + 12u)) {
        return 0;
    }
    dns_flags = get_be16(&data[dns_off + 2]);
    if (dns_flags & 0x8000u) {                          /* QR=1: response */
        return 0;
    }
    qdcount = get_be16(&data[dns_off + 4]);
    if (qdcount != 1u) {
        return 0;
    }

    /* Parse the single question: labels until the root, plus type+class. */
    question_off = dns_off + 12u;
    {
        uint16_t p = question_off;
        uint8_t label_len;
        do {
            if (p >= len) {
                return 0;
            }
            label_len = data[p];
            p = (uint16_t)(p + 1u + label_len);
        } while (label_len != 0u);
        question_len = (uint16_t)(p + 4u - question_off);
        if (p > len || question_len > (uint16_t)(NCM_PORTAL_MAX_REPLY - 60u)) {
            return 0;
        }
    }

    ECM_IPConfig_GetPortal(&enabled, &mode, portal_ip);
    if (!enabled || mode != 1u) {
        return 0;
    }
    ECM_IPConfig_GetHostIP(host_ip);
    answer_ip = (portal_ip[0] | portal_ip[1] | portal_ip[2] | portal_ip[3])
                ? portal_ip : host_ip;

    /* Build the reply: swap L2/L3/UDP addresses, keep the transaction ID. */
    off = 0;
    memcpy(&reply[0], &data[6], 6);                     /* dst = querier MAC */
    memcpy(&reply[6], dev_mac, 6);                      /* src = our MAC */
    put_be16(&reply[12], NCM_ETH_TYPE_IPV4);
    off = NCM_ETH_HEADER_LEN;

    reply[off] = 0x45u;                                 /* v4, IHL 5 */
    reply[off + 1] = 0x00u;                             /* DSCP/ECN */
    ip_total = (uint16_t)(20u + 8u + 12u + question_len + 16u);
    put_be16(&reply[off + 2], ip_total);
    put_be16(&reply[off + 4], get_be16(&data[NCM_ETH_HEADER_LEN + 4])); /* id */
    put_be16(&reply[off + 6], 0x0000u);                 /* no fragmentation */
    reply[off + 8] = 0x80u;                             /* TTL */
    reply[off + 9] = 17u;                               /* UDP */
    reply[off + 10] = 0x00u;
    reply[off + 11] = 0x00u;                            /* IP checksum later */
    memcpy(&reply[off + 12], &data[NCM_ETH_HEADER_LEN + 16], 4); /* src = DNS server */
    memcpy(&reply[off + 16], &data[NCM_ETH_HEADER_LEN + 12], 4); /* dst = querier */
    off += 20u;

    put_be16(&reply[off], 53u);                         /* src port */
    put_be16(&reply[off + 2], src_port);                /* dst = querier port */
    udp_len = (uint16_t)(8u + 12u + question_len + 16u);
    put_be16(&reply[off + 4], udp_len);
    put_be16(&reply[off + 6], 0x0000u);                 /* UDP checksum: none */
    off += 8u;

    put_be16(&reply[off], get_be16(&data[dns_off]));    /* transaction ID */
    put_be16(&reply[off + 2],
             (uint16_t)(0x8400u | (dns_flags & 0x0100u) | 0x0080u)); /* QR|AA|RD|RA */
    put_be16(&reply[off + 4], 1u);                      /* QDCOUNT */
    put_be16(&reply[off + 6], 1u);                      /* ANCOUNT */
    put_be16(&reply[off + 8], 0u);                      /* NSCOUNT */
    put_be16(&reply[off + 10], 0u);                     /* ARCOUNT (drop OPT) */
    off += 12u;

    memcpy(&reply[off], &data[question_off], question_len);
    off = (uint16_t)(off + question_len);

    put_be16(&reply[off], 0xC00Cu);                     /* ptr to question */
    put_be16(&reply[off + 2], 1u);                      /* type A */
    put_be16(&reply[off + 4], 1u);                      /* class IN */
    put_be16(&reply[off + 6], 10u);                     /* TTL hi */
    put_be16(&reply[off + 8], 0u);                      /* TTL lo */
    put_be16(&reply[off + 10], 4u);                     /* RDLENGTH */
    memcpy(&reply[off + 12], answer_ip, 4);             /* RDATA */
    off = (uint16_t)(off + 16u);

    /* IPv4 header checksum */
    sum = 0;
    for (i = 0; i < 10u; i++) {
        sum += (uint16_t)(reply[NCM_ETH_HEADER_LEN + i * 2u] << 8) |
               reply[NCM_ETH_HEADER_LEN + i * 2u + 1u];
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16u);
    }
    put_be16(&reply[NCM_ETH_HEADER_LEN + 10], (uint16_t)(~sum) & 0xFFFFu);

    if (!queue_push_bridge(queue, reply, off, &reply[0], dev_mac)) {
        return 0;
    }
    g_portal_hijacked++;
    USB_DEBUG("NCM: DNS hijacked -> portal\r\n");
    return 1;
}

static uint8_t handle_host_frame(const uint8_t *data, uint16_t len)
{
    uint16_t eth_type;

    if (len < NCM_ETH_HEADER_LEN) {
        return 0;
    }
    g_host_diag.host_frames++;
    if (s_bridge_enabled) memcpy(s_host_peer_mac, &data[6], 6);

    eth_type = (uint16_t)(data[12] << 8) | data[13];

    /* Discovery broadcasts/multicasts (wsdd, NetBIOS, LLMNR) are not
     * addressed to a unicast peer IP, so the regular bridge check below
     * cannot see them. Forward them before the local protocol handlers. */
    if (bridge_forward_discovery(data, len, &g_target_tx_queue,
                                 NCM_TARGET_DEV_MAC)) {
        g_bridge_host_to_target++;
        return 1;
    }

    /* Handle ARP */
    if (eth_type == 0x0806) {
        return handle_arp_frame(data, len);
    }

    /* Handle IPv4 */
    if (eth_type == NCM_ETH_TYPE_IPV4) {
        /* Check for ICMP */
        if (handle_icmp_packet(data, len)) {
            g_host_diag.icmp_echo++;
            return 1;
        }
        /* Check for DHCP */
        if (handle_dhcp_packet(data, len)) {
            g_host_diag.dhcp_offer++;
            return 1;
        }
        /* Captive portal: answer DNS queries before they cross the link */
        if (handle_dns_hijack(data, len, &g_host_tx_queue,
                              NCM_DEV_MAC)) {
            return 1;
        }
        /* L2 bridge: Host->Target frames (HTTP replies for the browser). */
        if (s_bridge_enabled && len >= NCM_ETH_HEADER_LEN + NCM_IPV4_MIN_HEADER_LEN) {
            const uint8_t *dst_ip = &data[NCM_ETH_HEADER_LEN + 16u];
            if (memcmp(dst_ip, ncm_target_host_ip, 4) == 0) {
                queue_push_bridge(&g_target_tx_queue, data, len,
                                  s_target_peer_mac, NCM_TARGET_DEV_MAC);
                g_bridge_host_to_target++;
                return 1;
            }
        }
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
/* Static NTB buffers and stream state for the host-facing (USBD) NCM path. */
static uint8_t s_host_ntb_tx[NCM_NTB_MAX_SIZE];
static uint16_t s_host_ntb_tx_len;   /* current host NTB length, 0 = idle */
static uint16_t s_host_ntb_tx_off;   /* bytes already handed to the PMA */
static uint8_t  s_host_ntb_zlp_pending;
static uint8_t  s_host_rx_buf[NCM_NTB_MAX_SIZE];
static uint16_t s_host_rx_len;
static uint16_t s_host_rx_need;

void NCM_Bridge_Init(void)
{
    memset(&g_host_tx_queue, 0, sizeof(g_host_tx_queue));
    memset(&g_host_rx_frame, 0, sizeof(g_host_rx_frame));
    g_host_tx_offset = 0;
    g_host_tx_gen = 0;
    g_host_zlp_pending = 0;
    g_ncm_ntb_sequence = 0;

    /* Reset the host-facing (USBLIB) NTB stream and reassembly state. */
    s_host_ntb_tx_len = 0;
    s_host_ntb_tx_off = 0;
    s_host_ntb_zlp_pending = 0;
    s_host_rx_len = 0;
    s_host_rx_need = 0;

    /* Start link notification sequence: DOWN -> UP -> SPEED -> BEACON */
    g_host_notify_state = NCM_NOTIFY_DOWN;
    g_host_notify_last_send_ms = 0;
    g_host_notify_hold_until_ms = 0;

    /* Start target-facing link notification sequence */
    g_target_ncm_notify_state = NCM_NOTIFY_DOWN;
    g_target_ncm_notify_hold_until_ms = 0;

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

/* Serialize host-side NCM diagnostics into out[] (little-endian, versioned).
 * Pure memory readout: no printf.  Returns bytes written, 0 on bad args.
 *
 * Layout (v1, 56 bytes):
 *  [0]      version (0x01)
 *  [1]      data_if_active      (g_host_ncm_data_if_active)
 *  [2..4]   EPnR[EP3..EP5] low  (raw endpoint status registers)
 *  [5]      ntb_sequence (low 8 bits)
 *  [6..8]   host TX queue count / rd / wr
 *  [9..10]  host TX queue overflow (u16)
 *  [11..12] ntb_tx_len   (u16)
 *  [13..14] ntb_tx_off   (u16)
 *  [15]     ntb_zlp_pending
 *  [16..17] rx_len  (u16)
 *  [18..19] rx_need (u16)
 *  [20..23] device IP (host link, e.g. 192.168.20.3)
 *  [24..27] host IP   (host link, e.g. 192.168.20.1)
 *  [28..55] counters u16: ep3_rx_pkts, ntb_sig_ok, ntb_sig_bad, ntb_parsed,
 *           host_frames, arp_req, arp_match, arp_reply, icmp_echo,
 *           dhcp_offer, tx_sent, zlp_sent, rx_oflow, poll_calls
 */
uint16_t NCM_Bridge_GetHostDiagnostics(uint8_t *out, uint16_t max_len)
{
    uint16_t i = 0;

#define DIAG_PUT8(v)  do { if (i < max_len) { out[i] = (uint8_t)(v); } i++; } while (0)
#define DIAG_PUT16(v) do { if ((i + 1) < max_len) {                         \
                              out[i]     = (uint8_t)((v) & 0xFF);           \
                              out[i + 1] = (uint8_t)(((v) >> 8) & 0xFF);    \
                          }                                                 \
                          i += 2; } while (0)

    DIAG_PUT8(0x01);                                  /* version        */
    DIAG_PUT8(g_host_ncm_data_if_active);
    DIAG_PUT8(_GetENDPOINT(ENDP3) & 0xFF);            /* EP3 OUT status */
    DIAG_PUT8(_GetENDPOINT(ENDP4) & 0xFF);            /* EP4 IN  status */
    DIAG_PUT8(_GetENDPOINT(ENDP5) & 0xFF);            /* EP5 IN  status */
    DIAG_PUT8(g_ncm_ntb_sequence);
    DIAG_PUT8(g_host_tx_queue.count);
    DIAG_PUT8(g_host_tx_queue.rd);
    DIAG_PUT8(g_host_tx_queue.wr);
    DIAG_PUT16(g_host_tx_queue.overflow);
    DIAG_PUT16(s_host_ntb_tx_len);
    DIAG_PUT16(s_host_ntb_tx_off);
    DIAG_PUT8(s_host_ntb_zlp_pending);
    DIAG_PUT16(s_host_rx_len);
    DIAG_PUT16(s_host_rx_need);
    DIAG_PUT8(ncm_dev_ip[0]); DIAG_PUT8(ncm_dev_ip[1]);
    DIAG_PUT8(ncm_dev_ip[2]); DIAG_PUT8(ncm_dev_ip[3]);
    DIAG_PUT8(ncm_host_ip[0]); DIAG_PUT8(ncm_host_ip[1]);
    DIAG_PUT8(ncm_host_ip[2]); DIAG_PUT8(ncm_host_ip[3]);

    DIAG_PUT16(g_host_diag.ep3_rx_pkts);
    DIAG_PUT16(g_host_diag.ntb_sig_ok);
    DIAG_PUT16(g_host_diag.ntb_sig_bad);
    DIAG_PUT16(g_host_diag.ntb_parsed);
    DIAG_PUT16(g_host_diag.host_frames);
    DIAG_PUT16(g_host_diag.arp_req);
    DIAG_PUT16(g_host_diag.arp_match);
    DIAG_PUT16(g_host_diag.arp_reply);
    DIAG_PUT16(g_host_diag.icmp_echo);
    DIAG_PUT16(g_host_diag.dhcp_offer);
    DIAG_PUT16(g_host_diag.tx_sent);
    DIAG_PUT16(g_host_diag.zlp_sent);
    DIAG_PUT16(g_host_diag.rx_oflow);
    DIAG_PUT16(g_host_diag.poll_calls);
    /* L2 bridge counters (proxy ARP + forwarded frames each direction). */
    DIAG_PUT16(g_bridge_arp_proxy);
    DIAG_PUT16(g_bridge_host_to_target);
    DIAG_PUT16(g_bridge_target_to_host);
    DIAG_PUT16(g_portal_hijacked);

    /* Append the BLE-Eth tunnel debug block (28 bytes). */
    {
        uint16_t dbg_len = 0;
        uint8_t dbg[28];
        BLE_Eth_GetDebugStats(dbg, sizeof(dbg), &dbg_len);
        for (uint16_t k = 0; k < dbg_len; k++) {
            DIAG_PUT8(dbg[k]);
        }
    }

#undef DIAG_PUT8
#undef DIAG_PUT16

    return (i > max_len) ? max_len : i;
}

/* Compact target-RX diagnostics (15 bytes, fits the 58B response limit).
 * Layout (v1):
 *  [0]      version (0x01)
 *  [1]      s_bridge_enabled
 *  [2..3]   EP5 OUT status low (target RX endpoint)
 *  [4..5]   target RX in-flight len (u16)
 *  [6..7]   target RX expected NTB len (u16)
 *  [8..9]   frames delivered to handle_target_frame (u16)
 *  [10..11] discovery forwards (u16)
 *  [12..13] bridge target->host forwards (u16)
 *  [14]     target TX queue count
 */
uint16_t NCM_Bridge_GetBridgeDiagnostics(uint8_t *out, uint16_t max_len)
{
    uint16_t i = 0;

#define BD_PUT8(v)  do { if (i < max_len) { out[i] = (uint8_t)(v); } i++; } while (0)
#define BD_PUT16(v) do { if ((i + 1) < max_len) {                        \
                             out[i]     = (uint8_t)((v) & 0xFF);         \
                             out[i + 1] = (uint8_t)(((v) >> 8) & 0xFF);  \
                         }                                               \
                         i += 2; } while (0)

    BD_PUT8(0x01);
    BD_PUT8(s_bridge_enabled);
    BD_PUT8(_GetENDPOINT(ENDP5) & 0xFF);
    BD_PUT16(s_target_rx_len);
    BD_PUT16(s_target_rx_need);
    BD_PUT16(g_target_diag_frames);
    BD_PUT16(g_target_diag_discovery);
    BD_PUT16(g_bridge_target_to_host);
    BD_PUT8(g_target_tx_queue.count);
    BD_PUT16(g_portal_hijacked);          /* captive-portal DNS answers */
    BD_PUT8(0x00u);                       /* reserved */

#undef BD_PUT8
#undef BD_PUT16

    return (i > max_len) ? max_len : i;
}

/* One bulk packet per CTR: feed the next 64-byte chunk of the pending NTB
 * into the EP4 IN PMA buffer.  An NTB whose length is an exact multiple of
 * the packet size is terminated with a ZLP. */
static void host_tx_feed(void)
{
    if (USBD_Endp4_Busy || s_host_ntb_tx_len == 0) {
        return;
    }

    if (s_host_ntb_tx_off < s_host_ntb_tx_len) {
        uint16_t remain = (uint16_t)(s_host_ntb_tx_len - s_host_ntb_tx_off);
        uint16_t chunk = (remain > NCM_BRIDGE_HOST_PACKET_SIZE)
                             ? NCM_BRIDGE_HOST_PACKET_SIZE : remain;
        extern uint8_t USBD_ENDPx_DataUp(uint8_t endp, uint8_t *pbuf, uint16_t len);
        if (USBD_ENDPx_DataUp(ENDP4, &s_host_ntb_tx[s_host_ntb_tx_off], chunk) != USB_SUCCESS) {
            return;
        }
        s_host_ntb_tx_off = (uint16_t)(s_host_ntb_tx_off + chunk);
        if (s_host_ntb_tx_off >= s_host_ntb_tx_len &&
            (s_host_ntb_tx_len % NCM_BRIDGE_HOST_PACKET_SIZE) == 0) {
            s_host_ntb_zlp_pending = 1;
        }
        return;
    }

    /* Whole NTB handed off: send a terminating ZLP when the length is an
     * exact multiple of the packet size, otherwise the transfer is done. */
    if (s_host_ntb_zlp_pending) {
        extern uint8_t USBD_ENDPx_DataUp(uint8_t endp, uint8_t *pbuf, uint16_t len);
        if (USBD_ENDPx_DataUp(ENDP4, s_host_ntb_tx, 0) == USB_SUCCESS) {
            s_host_ntb_zlp_pending = 0;
            s_host_ntb_tx_len = 0;
            g_host_diag.zlp_sent++;
            g_host_diag.tx_sent++;
        }
    } else {
        /* Non-multiple length: the last chunk already ended the transfer. */
        s_host_ntb_tx_len = 0;
        g_host_diag.tx_sent++;
    }
}

/* Reassemble one 64-byte bulk OUT packet into the pending NTB; when the NTB
 * announced by its header (NCH.. + wBlockLength) is complete, parse it. */
void NCM_Bridge_HostRxPacket(const uint8_t *data, uint16_t len)
{
    g_host_diag.ep3_rx_pkts++;

    if (s_host_rx_need == 0) {
        if (data == NULL || len < NCM_NTB_HEADER_SIZE) {
            g_host_diag.ntb_sig_bad++;
            return;
        }
        if (data[0] != 'N' || data[1] != 'C' || data[2] != 'M' || data[3] != 'H') {
            g_host_diag.ntb_sig_bad++;
            return;
        }
        uint16_t block_len = (uint16_t)(data[8] | ((uint16_t)data[9] << 8));
        if (block_len < NCM_NTB_HEADER_SIZE || block_len > sizeof(s_host_rx_buf)) {
            g_host_diag.ntb_sig_bad++;
            return;
        }
        g_host_diag.ntb_sig_ok++;
        s_host_rx_need = block_len;
        s_host_rx_len = 0;
    }

    if (len > (uint16_t)(sizeof(s_host_rx_buf) - s_host_rx_len)) {
        g_host_diag.rx_oflow++;
        s_host_rx_len = 0;
        s_host_rx_need = 0;
        return;
    }

    memcpy(&s_host_rx_buf[s_host_rx_len], data, len);
    s_host_rx_len = (uint16_t)(s_host_rx_len + len);

    if (s_host_rx_len >= s_host_rx_need) {
        g_host_diag.ntb_parsed++;
        NCM_ParseNtb(s_host_rx_buf, s_host_rx_need, handle_host_frame);
        s_host_rx_len = 0;
        s_host_rx_need = 0;
    }
}

void NCM_Bridge_Poll(void)
{
    HIDLauncher_Poll();
    g_host_diag.poll_calls++;
    poll_host_notification();
    NCM_Bridge_Target_NotificationPoll();

    /* Host-facing TX path (USBD controller).  PMA holds one 64-byte bulk
     * packet at a time, so NTBs stream out in chunks. */
    if (s_host_ntb_tx_len != 0) {
        host_tx_feed();
    }
    else if (!USBD_Endp4_Busy && g_host_tx_queue.count > 0) {
        /* Build NTB from one frame (single-frame NTBs for Phase 1) */
        NCM_Frame_t *frame = &g_host_tx_queue.frames[g_host_tx_queue.rd];
        uint8_t popped = 0;

        if (frame->len > 0) {
            const uint8_t *frame_ptrs[1] = { frame->data };
            uint16_t frame_lens[1] = { frame->len };
            uint16_t ntb_len = NCM_BuildNtb(s_host_ntb_tx, sizeof(s_host_ntb_tx),
                                             frame_ptrs, frame_lens,
                                             1, g_ncm_ntb_sequence);
            g_ncm_ntb_sequence++;

            if (ntb_len > 0) {
                s_host_ntb_tx_len = ntb_len;
                s_host_ntb_tx_off = 0;
                popped = 1;
            }
        } else {
            /* Zero-length frame: nothing to send, just drop it. */
            popped = 1;
        }

        if (popped) {
            uint32_t csr = ncm_irq_save();
            g_host_tx_queue.count--;
            g_host_tx_queue.rd = (g_host_tx_queue.rd + 1) % NCM_BRIDGE_QUEUE_DEPTH;
            ncm_irq_restore(csr);
            host_tx_feed();
        }
    }

    /* Target-facing TX path (USBFS controller) */
    if (!g_target_ncm_data_if_active || USBFS_Endp_Busy[DEF_UEP7] != 0) {
        return;
    }

    /* A bulk IN transfer whose length is an exact multiple of the endpoint
     * packet size must be terminated with a ZLP, otherwise the host keeps
     * waiting for more data and the NTB never reaches the driver. */
    if (s_target_zlp_pending) {
        if (USBFS_Endp_DataUp(DEF_UEP7, s_target_ntb_tx, 0, DEF_UEP_CPY_LOAD) == READY) {
            s_target_zlp_pending = 0;
        }
        return;
    }

    if (s_target_ntb_tx_len == 0 && g_target_tx_queue.count > 0) {
        NCM_Frame_t *frame = &g_target_tx_queue.frames[g_target_tx_queue.rd];
        if (frame->len > 0) {
            const uint8_t *frame_ptrs[1] = { frame->data };
            uint16_t frame_lens[1] = { frame->len };
            uint16_t ntb_len = NCM_BuildNtb(s_target_ntb_tx, sizeof(s_target_ntb_tx),
                                            frame_ptrs, frame_lens,
                                            1, s_target_ntb_seq++);
            if (ntb_len > 0) {
                s_target_ntb_tx_len = ntb_len;
                s_target_ntb_tx_off = 0;
                uint32_t csr = ncm_irq_save();
                g_target_tx_queue.count--;
                g_target_tx_queue.rd = (g_target_tx_queue.rd + 1) % NCM_BRIDGE_QUEUE_DEPTH;
                ncm_irq_restore(csr);
            }
        }
    }

    if (s_target_ntb_tx_len == 0) {
        return;
    }

    {
        uint16_t remaining = (uint16_t)(s_target_ntb_tx_len - s_target_ntb_tx_off);
        uint16_t chunk = (remaining > NCM_BRIDGE_USBFS_PACKET_SIZE)
                             ? NCM_BRIDGE_USBFS_PACKET_SIZE
                             : remaining;

        /* Copy load is required: USBFS DMA reads asynchronously and the
         * EP7 TX half of the endpoint buffer is the only safe location. */
        if (USBFS_Endp_DataUp(DEF_UEP7, &s_target_ntb_tx[s_target_ntb_tx_off],
                              chunk, DEF_UEP_CPY_LOAD) == READY) {
            s_target_ntb_tx_off = (uint16_t)(s_target_ntb_tx_off + chunk);
            if (s_target_ntb_tx_off >= s_target_ntb_tx_len) {
                if ((s_target_ntb_tx_len % NCM_BRIDGE_USBFS_PACKET_SIZE) == 0) {
                    s_target_zlp_pending = 1;
                }
                s_target_ntb_tx_len = 0;
                s_target_ntb_tx_off = 0;
                g_ncm_target_tx_count++;
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
volatile uint8_t g_target_ncm_data_if_active = 0;
volatile uint16_t g_ncm_target_tx_count = 0;

static uint32_t g_target_ncm_notify_last_send_ms = 0;

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

    if (g_target_ncm_notify_state == 0) {
        return;
    }

    if ((int32_t)(now - g_target_ncm_notify_hold_until_ms) < 0) {
        return;
    }

    /* Space notification packets out so the host's interrupt URB can retire. */
    if (now - g_target_ncm_notify_last_send_ms < 50) {
        return;
    }

    /* USBFS EP6 IN busy check */
    if (USBFS_Endp_Busy[DEF_UEP6] != 0) {
        return;
    }

    len = make_target_notification(g_target_ncm_notify_state, NCM_BRIDGE_TARGET_COMM_IF, packet);
    if (len > 0 &&
        USBFS_Endp_DataUp(DEF_UEP6, packet, len, DEF_UEP_CPY_LOAD) == READY) {
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

void NCM_Bridge_Target_SetDataAlt(uint8_t alt)
{
    /* A partially received NTB or queued TX belongs to the previous alt. */
    s_target_rx_len = 0;
    s_target_rx_need = 0;
    s_target_zlp_pending = 0;
    s_target_ntb_tx_len = 0;
    s_target_ntb_tx_off = 0;

    /* Stay open in both alts: hosts toggle alt 0/1 quickly and ARP/DHCP
     * must still flow during that window. */
    g_target_ncm_data_if_active = 1;

    if (alt == 1) {
        NCM_Bridge_Target_RearmLinkNotify();
    }
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
            /* Proxy-ARP for the Host: tell the Target we own 192.168.20.1 so
             * browser traffic is sent to us for bridging. With the captive
             * portal active also claim the portal answer address (the DNS
             * server handed out via DHCP), so the target can reach it. */
            uint8_t proxy_ip_match = 0;
            {
                uint8_t p_enabled, p_mode, p_ip[4];
                ECM_IPConfig_GetPortal(&p_enabled, &p_mode, p_ip);
                if (p_enabled && p_mode == 1u) {
                    const uint8_t *eff = (p_ip[0] | p_ip[1] | p_ip[2] | p_ip[3])
                                         ? p_ip : ncm_host_ip;
                    proxy_ip_match = (memcmp(&frame[38], eff, 4) == 0);
                }
            }
            if (s_bridge_enabled && (memcmp(&frame[38], ncm_host_ip, 4) == 0 ||
                                     proxy_ip_match)) {
                uint8_t reply[42];
                memcpy(&reply[0], &frame[6], 6);
                memcpy(&reply[6], NCM_TARGET_DEV_MAC, 6);
                reply[12] = 0x08; reply[13] = 0x06;
                reply[14] = 0x00; reply[15] = 0x01;
                reply[16] = 0x08; reply[17] = 0x00;
                reply[18] = 6; reply[19] = 4;
                reply[20] = 0x00; reply[21] = 0x02;
                memcpy(&reply[22], NCM_TARGET_DEV_MAC, 6);
                memcpy(&reply[28], ncm_host_ip, 4);
                memcpy(&reply[32], &frame[22], 6);
                memcpy(&reply[38], &frame[28], 4);
                queue_push(&g_target_tx_queue, reply, sizeof(reply));
                g_bridge_arp_proxy++;
                return 1;
            }
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

    /* DNS server: when the captive portal is active, hand out the portal
     * answer address so the target's DNS queries cross the bridge and get
     * hijacked (a "public" DNS would never resolve: nothing ARPs for it). */
    reply[off++] = NCM_DHCP_OPT_DNS;
    reply[off++] = 0x04;
    {
        uint8_t p_enabled, p_mode, p_ip[4];
        ECM_IPConfig_GetPortal(&p_enabled, &p_mode, p_ip);
        if (p_enabled && p_mode == 1u) {
            if (p_ip[0] | p_ip[1] | p_ip[2] | p_ip[3]) {
                memcpy(&reply[off], p_ip, 4);
            } else {
                memcpy(&reply[off], ncm_host_ip, 4);
            }
        } else {
            uint8_t public_dns[4] = { 8, 8, 8, 8 };
            memcpy(&reply[off], public_dns, 4);
        }
    }
    off += 4;

    /* Router: with the portal active, announce the Host so NCSI can run its
     * connectivity probe through this interface. */
    {
        uint8_t p_enabled, p_mode, p_ip[4];
        ECM_IPConfig_GetPortal(&p_enabled, &p_mode, p_ip);
        if (p_enabled && p_mode == 1u) {
            reply[off++] = NCM_DHCP_OPT_ROUTER;
            reply[off++] = 0x04;
            memcpy(&reply[off], ncm_host_ip, 4);
            off += 4;
        }
    }

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
 * Target-side ICMP Handler
 ******************************************************************************/
static uint8_t handle_target_icmp_packet(const uint8_t *frame, uint16_t frame_len)
{
    static uint8_t reply[NCM_BRIDGE_MTU_FRAME_SIZE];
    const uint8_t *ip;
    const uint8_t *icmp;
    uint16_t eth_offset;
    uint16_t eth_type;
    uint16_t ip_header_len;
    uint16_t ip_total_len;
    uint16_t icmp_len;
    uint16_t off;
    uint16_t ip_start;
    uint16_t icmp_start;
    uint32_t sum;
    uint16_t chk;

    if (frame == NULL || frame_len < NCM_ETH_HEADER_LEN) {
        return 0;
    }

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

    ip = &frame[eth_offset];
    if ((ip[0] >> 4) != 4u) {
        return 0;
    }

    ip_header_len = (uint16_t)((ip[0] & 0x0Fu) * 4u);
    if (ip_header_len < NCM_IPV4_MIN_HEADER_LEN ||
        frame_len < (uint16_t)(eth_offset + ip_header_len)) {
        return 0;
    }

    ip_total_len = get_be16(&ip[2]);
    if (ip_total_len < (uint16_t)(ip_header_len + 8u) ||
        frame_len < (uint16_t)(eth_offset + ip_total_len)) {
        return 0;
    }

    if (ip[9] != NCM_IPV4_PROTO_ICMP) {
        return 0;
    }

    if (memcmp(&ip[16], ncm_target_dev_ip, 4) != 0) {
        return 0;
    }

    icmp = &ip[ip_header_len];
    icmp_len = (uint16_t)(ip_total_len - ip_header_len);
    if (icmp_len < 8u || icmp[0] != NCM_ICMP_ECHO_REQUEST) {
        return 0;
    }

    if ((uint16_t)(14u + ip_header_len + icmp_len) > sizeof(reply)) {
        return 0;
    }

    off = 0;
    /* Ethernet: swap MACs */
    memcpy(&reply[off], &frame[6], 6);
    off += 6;
    memcpy(&reply[off], &frame[0], 6);
    off += 6;
    reply[off++] = 0x08;
    reply[off++] = 0x00;

    /* IPv4 header with swapped addresses */
    ip_start = off;
    memcpy(&reply[off], ip, ip_header_len);
    memcpy(&reply[off + 12], &ip[16], 4);
    memcpy(&reply[off + 16], &ip[12], 4);
    reply[off + 10] = 0;
    reply[off + 11] = 0;
    off += ip_header_len;

    /* ICMP echo reply */
    icmp_start = off;
    memcpy(&reply[off], icmp, icmp_len);
    reply[off] = NCM_ICMP_ECHO_REPLY;
    reply[off + 2] = 0;
    reply[off + 3] = 0;
    off += icmp_len;

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

    put_be16(&reply[ip_start + 2], (uint16_t)(off - ip_start));
    sum = 0;
    for (uint16_t i = 0; i < ip_header_len; i += 2) {
        sum += (uint16_t)(reply[ip_start + i] << 8) | reply[ip_start + i + 1];
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    chk = (~sum) & 0xFFFF;
    put_be16(&reply[ip_start + 10], chk);

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
    g_target_diag_frames++;
    if (s_bridge_enabled) memcpy(s_target_peer_mac, &data[6], 6);

    eth_type = (uint16_t)(data[12] << 8) | data[13];

    /* Windows sends WS-Discovery probes to 239.255.255.250 (and ff02::c)
     * and NetBIOS name queries to the broadcast address. These must reach
     * the Host before it can appear in the Windows Network view. */
    if (bridge_forward_discovery(data, len, &g_host_tx_queue,
                                 NCM_DEV_MAC)) {
        g_bridge_target_to_host++;
        g_target_diag_discovery++;
        return 1;
    }

    /* Handle ARP */
    if (eth_type == 0x0806) {
        return handle_target_arp_frame(data, len);
    }

    /* Handle IPv4 */
    if (eth_type == NCM_ETH_TYPE_IPV4) {
        /* Feed the frame to an active BLE-Eth tunnel first: target TCP
         * responses and ICMP echo replies terminate here, everything else
         * (DHCP, pings for the device itself) falls through to the local
         * handlers below. */
        if (BLE_Eth_HandleTargetFrame(data, len) && g_ble_tx_event_cb) {
            g_ble_tx_event_cb();
        }
        if (handle_target_dhcp_packet(data, len)) {
            return 1;
        }
        if (handle_target_icmp_packet(data, len)) {
            return 1;
        }
        /* Captive portal: answer DNS queries before they cross the link */
        if (handle_dns_hijack(data, len, &g_target_tx_queue,
                              NCM_TARGET_DEV_MAC)) {
            return 1;
        }
        /* L2 bridge: Target->Host frames (browser HTTP requests) to Host. */
        if (s_bridge_enabled && len >= NCM_ETH_HEADER_LEN + NCM_IPV4_MIN_HEADER_LEN) {
            const uint8_t *dst_ip = &data[NCM_ETH_HEADER_LEN + 16u];
            if (memcmp(dst_ip, ncm_host_ip, 4) == 0) {
                queue_push_bridge(&g_host_tx_queue, data, len,
                                  s_host_peer_mac, NCM_DEV_MAC);
                g_bridge_target_to_host++;
                return 1;
            }
        }
    }

    /* Frame not handled */
    return 0;
}

/**
 * Handle data received from the target PC via EP5 OUT.
 *
 * USBFS delivers one bulk packet per interrupt (64 bytes for every packet
 * except the last one of a transfer), so an NTB has to be reassembled from
 * consecutive packets before it can be parsed. The NTB header carries
 * wBlockLength, which is used as the expected size.
 */
void NCM_Bridge_TargetRxDone(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return;
    }

    if (s_target_rx_need == 0) {
        /* Start of a new transfer: the first packet holds the NTB header. */
        if (len < NCM_NTB_HEADER_SIZE) {
            return;
        }
        if (data[0] != 'N' || data[1] != 'C' || data[2] != 'M' || data[3] != 'H') {
            return;
        }
        uint16_t block_len = (uint16_t)(data[8] | ((uint16_t)data[9] << 8));
        if (block_len < NCM_NTB_HEADER_SIZE || block_len > sizeof(s_target_rx_buf)) {
            return;
        }
        s_target_rx_need = block_len;
        s_target_rx_len = 0;
    }

    if (len > (uint16_t)(sizeof(s_target_rx_buf) - s_target_rx_len)) {
        s_target_rx_len = 0;
        s_target_rx_need = 0;
        return;
    }

    memcpy(&s_target_rx_buf[s_target_rx_len], data, len);
    s_target_rx_len = (uint16_t)(s_target_rx_len + len);

    if (s_target_rx_len >= s_target_rx_need) {
        NCM_ParseNtb(s_target_rx_buf, s_target_rx_need, handle_target_frame);
        s_target_rx_len = 0;
        s_target_rx_need = 0;
    }
}

/**
 * Initialize target-side NCM state.
 */
void NCM_Bridge_InitTarget(void)
{
    USB_DEBUG("NCM: InitTarget called\r\n");
    memset(&g_target_tx_queue, 0, sizeof(g_target_tx_queue));
    s_target_ntb_seq = 0;
    s_target_zlp_pending = 0;
    s_target_ntb_tx_len = 0;
    s_target_ntb_tx_off = 0;
    s_target_rx_len = 0;
    s_target_rx_need = 0;

    /* Start link notification sequence: DOWN -> UP -> SPEED -> BEACON */
    g_target_ncm_notify_state = NCM_NOTIFY_DOWN;
    g_target_ncm_notify_hold_until_ms = 0;
    g_target_ncm_notify_tx_count = 0;
    g_target_ncm_notify_last_send_ms = 0;
    g_target_ncm_data_if_active = 0;
    g_ncm_target_tx_count = 0;

    /* Load target-side IP config */
    ECM_IPConfig_GetTargetHostIP(ncm_target_host_ip);
    ECM_IPConfig_GetTargetDevIP(ncm_target_dev_ip);
    ECM_IPConfig_GetSubnetMask(ncm_subnet_mask);
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
