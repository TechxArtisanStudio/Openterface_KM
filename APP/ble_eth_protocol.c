/********************************** (C) COPYRIGHT *******************************
 * File Name          : ble_eth_protocol.c
 * Author             : Openterface Team
 * Version            : V1.0.0
 * Date               : 2026/05/21
 * Description        : BLE-to-Ethernet tunneling protocol implementation.
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

#include "include/ble_eth_protocol.h"
#include "include/default_target_config.h"
#include "include/ecm_ip_config.h"
#include "include/ecm_bridge.h"
#include <string.h>
#include "debug.h"
#include "include/pwm_led.h"

#if USB_WORK_MODE_BRIDGE_ECM

/* LED diagnostic patterns for BLE-Eth tunnel debugging */
#define LED_DIAG_CONNECT_RX      90   /* CONNECT received from BLE */
#define LED_DIAG_SYN_SENT        91   /* TCP SYN sent to HOST */
#define LED_DIAG_SYN_ACK_RX      92   /* TCP SYN-ACK received from TARGET */
#define LED_DIAG_TIMEOUT         93   /* CONNECT timeout */

/* ======================== Internal State ================================= */

static ble_eth_parser_t     g_parser;
static ble_eth_conn_t       g_connections[BLE_ETH_MAX_CONNECTIONS];
static ble_eth_event_cb_t   g_event_cb;

/* TX queue: holds pre-built frames ready for BLE notification */
#define BLE_ETH_TX_QUEUE_DEPTH  8U
#define BLE_ETH_TX_CHUNK_SIZE   255U  /* fits in ble notify (uint8_t len param, max 255) */
/* Reserve a small number of TX queue slots for control/ACK frames so
 * large DATA_PUSH bursts cannot completely block host->device progress.
 */
#define BLE_ETH_TX_QUEUE_CTRL_RESERVE  1U

#define BLE_ETH_SYN_RETRY_TICKS 10U
#define BLE_ETH_SYN_MAX_RETRIES 5U

typedef struct {
    uint8_t  data[BLE_ETH_TX_CHUNK_SIZE];
    uint16_t len;
} ble_eth_tx_chunk_t;

typedef struct {
    ble_eth_tx_chunk_t chunks[BLE_ETH_TX_QUEUE_DEPTH];
    volatile uint8_t   rd;
    volatile uint8_t   wr;
    volatile uint8_t   count;
} ble_eth_tx_queue_t;

static ble_eth_tx_queue_t  g_tx_queue;
/* When the head TX chunk has been partially consumed (because the BLE notify
 * length is smaller than a full framed packet), we must not insert new frames
 * ahead of it. Doing so would interleave bytes from different protocol frames
 * and corrupt the stream seen by the host parser.
 */
static bool g_tx_head_partial;

static inline void txq_lock(void)
{
    __disable_irq();
}

static inline void txq_unlock(void)
{
    __enable_irq();
}

/* Reassembly buffer (one at a time, since BLE is single-connection) */
static ble_eth_reassembly_t g_reassembly;
static ble_eth_reliability_stats_t g_reliability_stats;

typedef struct {
    bool    valid;
    uint8_t conn_id;
    uint8_t seq;
    uint8_t expected_seq;
} ble_eth_last_data_ack_t;

static ble_eth_last_data_ack_t g_last_data_ack;

static uint8_t compute_rx_window(void)
{
    /* Advertise a conservative in-flight fragment window based on:
     *  - reassembly buffer remaining (receiver-side storage)
     *  - TX queue headroom (so ACK frames themselves don't overflow the notify queue)
     *
     * Notes:
     *  - Protocol defines rx_window as 1..15.
     *  - Firmware reassembly buffer fits ~6 full-size (246B) fragments.
     */
    const uint16_t max_frag_data = 246U; /* BLE_ETH_MAX_PAYLOAD_LEN - BLE_ETH_FRAG_HEADER_LEN */
    const uint8_t  max_frags_fit = (uint8_t)(sizeof(g_reassembly.data) / max_frag_data); /* ~= 6 */

    uint8_t window_by_buf = max_frags_fit;
    if (window_by_buf < 1U) window_by_buf = 1U;

    if (g_reassembly.in_progress) {
        uint16_t remaining_bytes = (uint16_t)(sizeof(g_reassembly.data) - g_reassembly.received_len);
        uint8_t remaining_by_buf = (uint8_t)(remaining_bytes / max_frag_data);
        if (remaining_by_buf < 1U) remaining_by_buf = 1U;

        uint8_t remaining_by_msg = 1U;
        if (g_reassembly.total_frags > g_reassembly.next_seq) {
            remaining_by_msg = (uint8_t)(g_reassembly.total_frags - g_reassembly.next_seq);
            if (remaining_by_msg < 1U) remaining_by_msg = 1U;
        }

        window_by_buf = remaining_by_buf;
        if (remaining_by_msg < window_by_buf) window_by_buf = remaining_by_msg;
    }

    /* Ensure we have room to enqueue ACK frames into the notify TX queue.
     * Keep at least one slot reserved for non-ACK traffic.
     */
    uint8_t occupancy = g_tx_queue.count;
    uint8_t headroom = 0U;
    if (occupancy < BLE_ETH_TX_QUEUE_DEPTH) {
        headroom = (uint8_t)(BLE_ETH_TX_QUEUE_DEPTH - occupancy);
    }
    if (headroom > 1U) {
        headroom = (uint8_t)(headroom - 1U);
    } else {
        headroom = 1U;
    }

    uint8_t window = window_by_buf;
    if (headroom < window) window = headroom;
    if (window < 1U) window = 1U;
    if (window > 15U) window = 15U;
    return window;
}

/* Callback for sending complete Ethernet frames to ECM bridge.
   Set by peripheral.c via BLE_Eth_SetEcmtxCb(). */
typedef void (*ble_eth_ecm_tx_cb_t)(const uint8_t *data, uint16_t len);
static ble_eth_ecm_tx_cb_t g_ecm_tx_cb;

/* Firmware-side peer identity on the target CDC ECM link. */
static const uint8_t g_dev_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x03, 0x00 };
static const uint8_t g_host_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t g_dev_ip[4] = { 192, 168, 11, 5 };  /* Target-side firmware IP */

/* Learned host MAC from incoming frames (overrides hardcoded g_host_mac) */
static uint8_t g_host_mac_learned[6];
static bool g_host_mac_valid = false;
static uint16_t g_next_local_port = 50000U;
static uint32_t g_next_initial_seq = 0x12345678U;
static bool g_tcp_alloc_seeded = false;
extern volatile uint32_t systick_ms;

void BLE_Eth_SetEcmtxCb(ble_eth_ecm_tx_cb_t cb)
{
    g_ecm_tx_cb = cb;
}

void BLE_Eth_LearnHostMac(const uint8_t *mac)
{
    if (mac && !g_host_mac_valid) {
        memcpy(g_host_mac_learned, mac, 6);
        g_host_mac_valid = true;
    }
}

/* ======================== Helpers ======================================== */

static uint8_t calc_checksum(const uint8_t *data, uint16_t len)
{
    uint8_t sum = 0;
    for (uint16_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

static uint16_t alloc_local_port(void)
{
    if (!g_tcp_alloc_seeded) {
        uint16_t offset = (uint16_t)(systick_ms % 9000U);
        g_next_local_port = (uint16_t)(50000U + offset);
        g_next_initial_seq ^= ((uint32_t)systick_ms << 8);
        g_tcp_alloc_seeded = true;
    }

    uint16_t port = g_next_local_port;
    g_next_local_port = (uint16_t)(g_next_local_port + 17U);
    if (g_next_local_port > 60999U) {
        g_next_local_port = 50000U;
    }
    return port;
}

static uint32_t alloc_initial_seq(void)
{
    uint32_t seq = g_next_initial_seq;
    g_next_initial_seq += 0x01010101U;
    return seq;
}

static uint16_t build_frame(uint8_t *buf, uint8_t addr, uint8_t cmd,
                            const uint8_t *payload, uint16_t plen)
{
    uint16_t off = 0;

    buf[off++] = BLE_ETH_FRAME_HEAD1;
    buf[off++] = BLE_ETH_FRAME_HEAD2;
    buf[off++] = addr;
    buf[off++] = cmd;
    buf[off++] = (uint8_t)plen;

    if (payload && plen > 0) {
        memcpy(&buf[off], payload, plen);
        off += plen;
    }

    buf[off++] = calc_checksum(buf, off);
    return off;
}

static void normalize_frame_checksum(uint8_t *data, uint16_t len)
{
    if (!data || len < BLE_ETH_FRAME_MIN_LEN) return;
    if (data[0] != BLE_ETH_FRAME_HEAD1 || data[1] != BLE_ETH_FRAME_HEAD2) return;

    uint16_t expected_len = (uint16_t)data[4] + BLE_ETH_FRAME_MIN_LEN;
    if (expected_len != len) return;

    data[len - 1] = calc_checksum(data, (uint16_t)(len - 1));
}

static void tx_queue_clear(void)
{
    txq_lock();
    g_tx_queue.rd = 0;
    g_tx_queue.wr = 0;
    g_tx_queue.count = 0;
    g_tx_head_partial = false;
    txq_unlock();
}

static bool tx_queue_push(const uint8_t *data, uint16_t len)
{
    if (len == 0) return true;

    txq_lock();

    /* Enqueue must be all-or-nothing to avoid truncating framed payloads. */
    uint16_t needed_chunks = (uint16_t)((len + BLE_ETH_TX_CHUNK_SIZE - 1U) / BLE_ETH_TX_CHUNK_SIZE);
    if ((uint16_t)(g_tx_queue.count + needed_chunks) > BLE_ETH_TX_QUEUE_DEPTH) {
        g_reliability_stats.tx_queue_full_drop++;
        txq_unlock();
        return false;
    }

    uint16_t offset = 0;
    while (offset < len) {
        uint16_t copy_len = (uint16_t)(len - offset);
        if (copy_len > BLE_ETH_TX_CHUNK_SIZE) copy_len = BLE_ETH_TX_CHUNK_SIZE;

        memcpy(g_tx_queue.chunks[g_tx_queue.wr].data, data + offset, copy_len);
        normalize_frame_checksum(g_tx_queue.chunks[g_tx_queue.wr].data, copy_len);
        g_tx_queue.chunks[g_tx_queue.wr].len = copy_len;

        g_tx_queue.wr++;
        if (g_tx_queue.wr >= BLE_ETH_TX_QUEUE_DEPTH) g_tx_queue.wr = 0;
        g_tx_queue.count++;

        offset = (uint16_t)(offset + copy_len);
    }

    txq_unlock();

    return true;
}

static bool tx_queue_push_front(const uint8_t *data, uint16_t len)
{
    if (len == 0) return true;
    if (len > BLE_ETH_TX_CHUNK_SIZE) {
        /* Should not happen under protocol limits; fall back to normal push. */
        return tx_queue_push(data, len);
    }

    txq_lock();

    if (g_tx_queue.count >= BLE_ETH_TX_QUEUE_DEPTH) {
        g_reliability_stats.tx_queue_full_drop++;
        txq_unlock();
        return false;
    }

    if (g_tx_head_partial) {
        /* We are mid-frame: do not preempt, enqueue at tail instead.
         * This preserves frame byte ordering (no interleaving).
         */
        memcpy(g_tx_queue.chunks[g_tx_queue.wr].data, data, len);
        normalize_frame_checksum(g_tx_queue.chunks[g_tx_queue.wr].data, len);
        g_tx_queue.chunks[g_tx_queue.wr].len = len;
        g_tx_queue.wr++;
        if (g_tx_queue.wr >= BLE_ETH_TX_QUEUE_DEPTH) g_tx_queue.wr = 0;
        g_tx_queue.count++;
    } else {
        /* Insert one chunk before current rd (priority). */
        if (g_tx_queue.rd == 0) {
            g_tx_queue.rd = (uint8_t)(BLE_ETH_TX_QUEUE_DEPTH - 1U);
        } else {
            g_tx_queue.rd--;
        }

        memcpy(g_tx_queue.chunks[g_tx_queue.rd].data, data, len);
        normalize_frame_checksum(g_tx_queue.chunks[g_tx_queue.rd].data, len);
        g_tx_queue.chunks[g_tx_queue.rd].len = len;
        g_tx_queue.count++;
    }

    txq_unlock();
    return true;
}

static void reassembly_clear(void)
{
    g_reassembly.total_len = 0;
    g_reassembly.received_len = 0;
    g_reassembly.conn_id = BLE_ETH_INVALID_CONN_ID;
    g_reassembly.total_frags = 0;
    g_reassembly.next_seq = 0;
    g_reassembly.in_progress = false;
}

static void evt_frame_rx(const ble_eth_frame_t *frame)
{
    if (g_event_cb) {
        ble_eth_evt_t evt;
        evt.type = BLE_ETH_EVT_FRAME_RX;
        memcpy(&evt.info.frame, frame, sizeof(ble_eth_frame_t));
        g_event_cb(&evt);
    }
}

static void evt_data_ready(uint8_t conn_id, const uint8_t *data, uint16_t len)
{
    if (g_event_cb) {
        ble_eth_evt_t evt;
        evt.type = BLE_ETH_EVT_DATA_READY;
        evt.info.data.conn_id = conn_id;
        evt.info.data.data = (uint8_t *)data;
        evt.info.data.len = len;
        g_event_cb(&evt);
    }
}

static void evt_conn_opened(uint8_t conn_id, uint8_t status)
{
    if (g_event_cb) {
        ble_eth_evt_t evt;
        evt.type = BLE_ETH_EVT_CONN_OPENED;
        evt.info.conn.conn_id = conn_id;
        evt.info.conn.status = status;
        g_event_cb(&evt);
    }
}

static void evt_conn_closed(uint8_t conn_id, uint8_t status)
{
    if (g_event_cb) {
        ble_eth_evt_t evt;
        evt.type = BLE_ETH_EVT_CONN_CLOSED;
        evt.info.conn.conn_id = conn_id;
        evt.info.conn.status = status;
        g_event_cb(&evt);
    }
}

/* ======================== Frame Dispatch ================================ */

static uint8_t send_tcp_frame(uint8_t conn_id, uint16_t tcp_flags,
                              const uint8_t *payload, uint16_t payload_len);
static uint8_t send_tcp_syn(uint8_t conn_id);
static void send_arp_request(uint8_t conn_id);
static void queue_connect_response(uint8_t addr, uint8_t conn_id, uint8_t status);

static void queue_data_ack(uint8_t addr, uint8_t status, uint8_t conn_id, uint8_t seq, uint8_t expected_seq)
{
    uint8_t rsp[BLE_ETH_DATA_ACK_WINDOW_PAYLOAD_LEN] = {
        status,
        conn_id,
        seq,
        expected_seq,
        compute_rx_window(),
    };
    uint8_t buf[16];
    uint16_t len = build_frame(buf, addr, BLE_ETH_CMD_DATA_RESP, rsp, BLE_ETH_DATA_ACK_WINDOW_PAYLOAD_LEN);
    if (tx_queue_push_front(buf, len)) {
        g_reliability_stats.tx_ack_sent++;
    }
}

static void dispatch_frame(const ble_eth_frame_t *frame)
{
    uint8_t addr = frame->addr;
    uint8_t cmd = frame->cmd;

    /* Check if this is a response from device (cmd >= 0x80) -
     * these are normally handled by the host side, but on device
     * we may receive them for loopback testing. For now, handle
     * only host->device commands. */

    switch (cmd) {
    case BLE_ETH_CMD_CONNECT: {
        /* Parse connect payload */
        if (frame->payload_len < BLE_ETH_CONNECT_PAYLOAD_LEN) {
            uint8_t status = BLE_ETH_STATUS_PARAM_ERR;
            queue_connect_response(addr, BLE_ETH_INVALID_CONN_ID, status);
            evt_conn_opened(BLE_ETH_INVALID_CONN_ID, status);
            return;
        }

        const uint8_t *ip = frame->payload;
        uint16_t port = ((uint16_t)frame->payload[4] << 8) | frame->payload[5];

        uint8_t conn_id = BLE_Eth_AllocConnection(ip, port);
        uint8_t status = (conn_id != BLE_ETH_INVALID_CONN_ID)
                         ? BLE_ETH_STATUS_SUCCESS
                         : BLE_ETH_STATUS_CONN_FULL;

        if (status != BLE_ETH_STATUS_SUCCESS) {
            queue_connect_response(addr, BLE_ETH_INVALID_CONN_ID, status);
            evt_conn_opened(BLE_ETH_INVALID_CONN_ID, status);
            return;
        }

        /* Send TCP SYN immediately, but only report CONNECT success after
         * the target answers SYN+ACK. This keeps the local proxy from sending
         * SSH bytes before the firmware TCP state is established. */
        if (!g_host_mac_valid) {
            memcpy(g_host_mac_learned, g_host_mac, 6);
            g_host_mac_valid = true;
        }
        ble_eth_conn_t *c = &g_connections[conn_id];
        c->peer_addr = addr;
        c->syn_retries_left = BLE_ETH_SYN_MAX_RETRIES;
        /* Start in ARP_PENDING so we discover the target PC's actual MAC
         * before sending the SYN. The SYN will be sent after the ARP reply
         * arrives (in BLE_Eth_HandleArpReply) or via SYN retry timer. */
        c->tcp_state = BLE_ETH_TCP_ARP_PENDING;
        c->arp_timeout_ticks = BLE_ETH_SYN_RETRY_TICKS;

        PWM_LED_SetBrightness(LED_DIAG_SYN_SENT);  // LED pattern 91: ARP request sent
        send_arp_request(conn_id);
        break;
    }

    case BLE_ETH_CMD_DISCONNECT: {
        if (frame->payload_len < BLE_ETH_DISCONNECT_PAYLOAD_LEN) {
            uint8_t buf[16];
            uint16_t len = build_frame(buf, addr, BLE_ETH_CMD_DISCONN_RESP,
                                       NULL, 0);
            tx_queue_push_front(buf, len);
            return;
        }

        uint8_t conn_id = frame->payload[0];
        if (conn_id < BLE_ETH_MAX_CONNECTIONS && g_connections[conn_id].active) {
            ble_eth_conn_t *c = &g_connections[conn_id];
            if (c->tcp_state == BLE_ETH_TCP_ESTABLISHED || c->tcp_state == BLE_ETH_TCP_SYN_SENT) {
                send_tcp_frame(conn_id, 0x14, NULL, 0);  /* RST+ACK */
            }
        }
        BLE_Eth_FreeConnection(conn_id);

        uint8_t rsp[2] = { conn_id, BLE_ETH_STATUS_SUCCESS };
        uint8_t buf[16];
        uint16_t len = build_frame(buf, addr, BLE_ETH_CMD_DISCONN_RESP, rsp, 2);
        tx_queue_push_front(buf, len);
        evt_conn_closed(conn_id, BLE_ETH_STATUS_SUCCESS);
        break;
    }

    case BLE_ETH_CMD_DATA: {
        /* Parse fragment header */
        if (frame->payload_len < BLE_ETH_FRAG_HEADER_LEN) {
            uint8_t buf[16];
            uint16_t len = build_frame(buf, addr, BLE_ETH_CMD_ERROR, NULL, 0);
            tx_queue_push_front(buf, len);
            return;
        }

        uint8_t frag_flags = frame->payload[0];
        uint8_t seq        = frame->payload[1];
        uint8_t conn_id    = frame->payload[2];
        const uint8_t *frag_data = &frame->payload[3];
        uint16_t frag_data_len = frame->payload_len - BLE_ETH_FRAG_HEADER_LEN;
        uint8_t total_frags = frag_flags & BLE_ETH_FRAG_COUNT_MASK;
        g_reliability_stats.frag_rx_total++;

        if (total_frags == 0) total_frags = 1;

        /* Start new reassembly on first fragment */
        if (frag_flags & BLE_ETH_FRAG_FIRST) {
            reassembly_clear();
            g_reassembly.conn_id = conn_id;
            g_reassembly.total_frags = total_frags;
            g_reassembly.next_seq = 0;
            g_reassembly.in_progress = true;
            g_reliability_stats.frag_reassembly_start++;
            g_last_data_ack.valid = false;

            /* Pre-allocate buffer size estimate */
            g_reassembly.total_len = 0; /* will be set as we go */
        }

        if (!g_reassembly.in_progress) {
            if (g_last_data_ack.valid
                && g_last_data_ack.conn_id == conn_id
                && g_last_data_ack.seq == seq) {
                /* Replay ACK for lost-ACK retransmission of final fragment. */
                g_reliability_stats.frag_rx_duplicate++;
                queue_data_ack(addr, BLE_ETH_STATUS_SUCCESS, conn_id, seq, g_last_data_ack.expected_seq);
                return;
            }
            queue_data_ack(addr, BLE_ETH_STATUS_FRAME_ERR, conn_id, seq, g_reassembly.next_seq);
            return; /* no reassembly in progress, drop */
        }

        if (seq != g_reassembly.next_seq) {
            if (seq < g_reassembly.next_seq) {
                /* Duplicate retransmission (ACK likely lost): ACK current progress. */
                g_reliability_stats.frag_rx_duplicate++;
                queue_data_ack(addr, BLE_ETH_STATUS_SUCCESS, conn_id, seq, g_reassembly.next_seq);
                return;
            }

            /* Future out-of-order fragment, abort reassembly. */
            g_reliability_stats.frag_rx_out_of_order++;
            g_reliability_stats.frag_reassembly_abort++;
            queue_data_ack(addr, BLE_ETH_STATUS_FRAME_ERR, conn_id, seq, g_reassembly.next_seq);
            g_last_data_ack.valid = false;
            reassembly_clear();
            return;
        }

        /* Copy fragment data */
        if (g_reassembly.received_len + frag_data_len <= sizeof(g_reassembly.data)) {
            memcpy(&g_reassembly.data[g_reassembly.received_len],
                   frag_data, frag_data_len);
            g_reassembly.received_len += frag_data_len;
            g_reassembly.next_seq++;
            g_reliability_stats.frag_reassembly_bytes += frag_data_len;
            queue_data_ack(addr, BLE_ETH_STATUS_SUCCESS, conn_id, seq, g_reassembly.next_seq);
            g_last_data_ack.valid = true;
            g_last_data_ack.conn_id = conn_id;
            g_last_data_ack.seq = seq;
            g_last_data_ack.expected_seq = g_reassembly.next_seq;
        } else {
            g_reliability_stats.frag_reassembly_abort++;
            queue_data_ack(addr, BLE_ETH_STATUS_PARAM_ERR, conn_id, seq, g_reassembly.next_seq);
            g_last_data_ack.valid = false;
            reassembly_clear();
            return;
        }

        /* Check if this is the last fragment */
        if (!(frag_flags & BLE_ETH_FRAG_MORE)) {
            /* Reassembly complete */
            g_reassembly.in_progress = false;
            g_reliability_stats.frag_reassembly_complete++;
            evt_data_ready(conn_id, g_reassembly.data, g_reassembly.received_len);
            reassembly_clear();
        }
        break;
    }

    case BLE_ETH_CMD_INFO: {
        uint8_t rsp[BLE_ETH_INFO_PAYLOAD_LEN];
        memset(rsp, 0, sizeof(rsp));
        rsp[0] = BLE_ETH_PROTO_VERSION;
        rsp[1] = BLE_ETH_MAX_CONNECTIONS;
        rsp[2] = 0x00;  /* high byte of max single-frag payload */
        rsp[3] = BLE_ETH_MAX_PAYLOAD_LEN - BLE_ETH_FRAG_HEADER_LEN;  /* low byte */
        rsp[4] = BLE_ETH_FEAT_TCP_TUNNEL | BLE_ETH_FEAT_SSH_OPT | BLE_ETH_FEAT_DATA_ACK_META | BLE_ETH_FEAT_DATA_ACK_WINDOW;

        uint8_t buf[24];
        uint16_t len = build_frame(buf, addr, BLE_ETH_CMD_INFO_RESP, rsp, BLE_ETH_INFO_PAYLOAD_LEN);
        tx_queue_push_front(buf, len);
        break;
    }

    case BLE_ETH_CMD_PING: {
        /* Echo payload back as PONG — tests BLE data path without network */
        uint8_t buf[8 + BLE_ETH_MAX_PAYLOAD_LEN];
        uint16_t len = build_frame(buf, addr, BLE_ETH_CMD_PONG,
                                   frame->payload, frame->payload_len);
        tx_queue_push_front(buf, len);
        break;
    }

    case BLE_ETH_CMD_SET_DEFAULT: {
        /* Set default target IP/port
         * Payload: [IP 4 bytes][Port 2 bytes] = 6 bytes
         */
        if (frame->payload_len < BLE_ETH_CONNECT_PAYLOAD_LEN) {
            uint8_t rsp[1] = { BLE_ETH_STATUS_PARAM_ERR };
            uint8_t buf[16];
            uint16_t len = build_frame(buf, addr, BLE_ETH_CMD_SET_DEFAULT_RESP, rsp, 1);
            tx_queue_push_front(buf, len);
            break;
        }

        const uint8_t *ip = frame->payload;
        uint16_t port = ((uint16_t)frame->payload[4] << 8) | frame->payload[5];

        if (DefaultTargetConfig_Set(ip, port)) {
            uint8_t rsp[1] = { BLE_ETH_STATUS_SUCCESS };
            uint8_t buf[16];
            uint16_t len = build_frame(buf, addr, BLE_ETH_CMD_SET_DEFAULT_RESP, rsp, 1);
            tx_queue_push_front(buf, len);
        } else {
            uint8_t rsp[1] = { BLE_ETH_STATUS_CONN_FULL };
            uint8_t buf[16];
            uint16_t len = build_frame(buf, addr, BLE_ETH_CMD_SET_DEFAULT_RESP, rsp, 1);
            tx_queue_push_front(buf, len);
        }
        break;
    }

    case BLE_ETH_CMD_GET_DEFAULT: {
        /* Get default target IP/port
         * Response: [IP 4 bytes][Port 2 bytes][Enabled 1 byte] = 7 bytes
         */
        uint8_t rsp[7];
        DefaultTargetConfig_GetIP(rsp);
        uint16_t port = DefaultTargetConfig_GetPort();
        rsp[4] = (port >> 8) & 0xFF;
        rsp[5] = port & 0xFF;
        rsp[6] = DefaultTargetConfig_IsEnabled() ? 1 : 0;

        uint8_t buf[16];
        uint16_t len = build_frame(buf, addr, BLE_ETH_CMD_GET_DEFAULT_RESP, rsp, 7);
        tx_queue_push_front(buf, len);
        break;
    }

    case BLE_ETH_CMD_SET_ECM_IP: {
        /* Set ECM IP configuration
         * Payload: [sub_cmd 1][IP data...]
         *   sub_cmd 0x00: Set All - [host_ip 4][dev_ip 4][subnet 4][target_host 4][target_dev 4] = 21 bytes
         *   sub_cmd 0x01: Set Host Side - [host_ip 4][dev_ip 4][subnet 4] = 13 bytes
         *   sub_cmd 0x02: Set Target Side - [target_host 4][target_dev 4] = 9 bytes
         *   sub_cmd 0x03: Reset to Default - 1 byte
         * Response: [status 1 byte]
         */
        if (frame->payload_len < 1) {
            uint8_t rsp[1] = { BLE_ETH_STATUS_PARAM_ERR };
            uint8_t buf[16];
            uint16_t len = build_frame(buf, addr, BLE_ETH_CMD_SET_ECM_IP_RESP, rsp, 1);
            tx_queue_push_front(buf, len);
            break;
        }

        uint8_t sub_cmd = frame->payload[0];
        uint8_t status = BLE_ETH_STATUS_SUCCESS;

        switch (sub_cmd) {
        case 0x00: /* Set All */
            if (frame->payload_len != 21) {
                status = BLE_ETH_STATUS_PARAM_ERR;
            } else {
                ECM_IPConfig_t config = {
                    .record_version = ECM_IP_CONFIG_VERSION,
                    .host_ip = { frame->payload[1], frame->payload[2], frame->payload[3], frame->payload[4] },
                    .dev_ip = { frame->payload[5], frame->payload[6], frame->payload[7], frame->payload[8] },
                    .subnet_mask = { frame->payload[9], frame->payload[10], frame->payload[11], frame->payload[12] },
                    .target_host_ip = { frame->payload[13], frame->payload[14], frame->payload[15], frame->payload[16] },
                    .target_dev_ip = { frame->payload[17], frame->payload[18], frame->payload[19], frame->payload[20] },
                    .reserved = { 0, 0 },
                    .padding = 0
                };

                if (!ECM_IPConfig_Validate(&config)) {
                    status = BLE_ETH_STATUS_PARAM_ERR;
                } else {
                    uint8_t result = ECM_IPConfig_Set(&config);
                    status = (result == ECM_IP_CONFIG_OK) ? BLE_ETH_STATUS_SUCCESS : BLE_ETH_STATUS_INTERNAL_ERR;
                }
            }
            break;

        case 0x01: /* Set Host Side */
            if (frame->payload_len != 13) {
                status = BLE_ETH_STATUS_PARAM_ERR;
            } else {
                ECM_IPConfig_t temp_config;
                ECM_IPConfig_GetCurrent(&temp_config);
                memcpy(temp_config.host_ip, &frame->payload[1], 4);
                memcpy(temp_config.dev_ip, &frame->payload[5], 4);
                memcpy(temp_config.subnet_mask, &frame->payload[9], 4);

                if (!ECM_IPConfig_Validate(&temp_config)) {
                    status = BLE_ETH_STATUS_PARAM_ERR;
                } else {
                    uint8_t result = ECM_IPConfig_Set(&temp_config);
                    status = (result == ECM_IP_CONFIG_OK) ? BLE_ETH_STATUS_SUCCESS : BLE_ETH_STATUS_INTERNAL_ERR;
                }
            }
            break;

        case 0x02: /* Set Target Side */
            if (frame->payload_len != 9) {
                status = BLE_ETH_STATUS_PARAM_ERR;
            } else {
                ECM_IPConfig_t temp_config;
                ECM_IPConfig_GetCurrent(&temp_config);
                memcpy(temp_config.target_host_ip, &frame->payload[1], 4);
                memcpy(temp_config.target_dev_ip, &frame->payload[5], 4);

                if (!ECM_IPConfig_Validate(&temp_config)) {
                    status = BLE_ETH_STATUS_PARAM_ERR;
                } else {
                    uint8_t result = ECM_IPConfig_Set(&temp_config);
                    status = (result == ECM_IP_CONFIG_OK) ? BLE_ETH_STATUS_SUCCESS : BLE_ETH_STATUS_INTERNAL_ERR;
                }
            }
            break;

        case 0x03: /* Reset to Default */
            {
                uint8_t result = ECM_IPConfig_Reset();
                status = (result == ECM_IP_CONFIG_OK) ? BLE_ETH_STATUS_SUCCESS : BLE_ETH_STATUS_INTERNAL_ERR;
            }
            break;

        default:
            status = BLE_ETH_STATUS_PARAM_ERR;
            break;
        }

        uint8_t rsp[1] = { status };
        uint8_t buf[16];
        uint16_t len = build_frame(buf, addr, BLE_ETH_CMD_SET_ECM_IP_RESP, rsp, 1);
        tx_queue_push_front(buf, len);
        break;
    }

    case BLE_ETH_CMD_GET_ECM_IP: {
        /* Get ECM IP configuration
         * Response: [status 1][host_ip 4][dev_ip 4][subnet 4][target_host 4][target_dev 4] = 21 bytes
         */
        uint8_t rsp[21];
        ECM_IPConfig_t config;
        ECM_IPConfig_Get(&config);

        rsp[0] = BLE_ETH_STATUS_SUCCESS;
        memcpy(&rsp[1], config.host_ip, 4);
        memcpy(&rsp[5], config.dev_ip, 4);
        memcpy(&rsp[9], config.subnet_mask, 4);
        memcpy(&rsp[13], config.target_host_ip, 4);
        memcpy(&rsp[17], config.target_dev_ip, 4);

        uint8_t buf[32];
        uint16_t len = build_frame(buf, addr, BLE_ETH_CMD_GET_ECM_IP_RESP, rsp, 21);
        tx_queue_push_front(buf, len);
        break;
    }

    default:
        /* Unknown command - send error */
        {
            g_reliability_stats.frame_rx_cmd_unknown++;
            uint8_t buf[16];
            uint16_t len = build_frame(buf, addr, BLE_ETH_CMD_ERROR, NULL, 0);
            tx_queue_push_front(buf, len);
        }
        break;
    }
}

/* ======================== Parser ========================================= */

void BLE_Eth_Parse(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        switch (g_parser.state) {
        case BLE_ETH_PARSER_WAIT_HEAD1:
            if (byte == BLE_ETH_FRAME_HEAD1) {
                g_parser.frame.payload_len = 0;
                g_parser.checksum = byte;
                g_parser.rx_index = 1;
                g_parser.state = BLE_ETH_PARSER_WAIT_HEAD2;
            }
            break;

        case BLE_ETH_PARSER_WAIT_HEAD2:
            if (byte == BLE_ETH_FRAME_HEAD2) {
                g_parser.checksum += byte;
                g_parser.rx_index++;
                g_parser.state = BLE_ETH_PARSER_WAIT_ADDR;
            } else {
                /* Not head2, restart search */
                g_parser.state = BLE_ETH_PARSER_WAIT_HEAD1;
                if (byte == BLE_ETH_FRAME_HEAD1) {
                    g_parser.checksum = byte;
                    g_parser.rx_index = 1;
                    g_parser.state = BLE_ETH_PARSER_WAIT_HEAD2;
                }
            }
            break;

        case BLE_ETH_PARSER_WAIT_ADDR:
            g_parser.frame.addr = byte;
            g_parser.checksum += byte;
            g_parser.rx_index++;
            g_parser.state = BLE_ETH_PARSER_WAIT_CMD;
            break;

        case BLE_ETH_PARSER_WAIT_CMD:
            g_parser.frame.cmd = byte;
            g_parser.checksum += byte;
            g_parser.rx_index++;
            g_parser.state = BLE_ETH_PARSER_WAIT_LEN;
            break;

        case BLE_ETH_PARSER_WAIT_LEN:
            g_parser.frame.payload_len = byte;
            g_parser.checksum += byte;
            g_parser.rx_index++;
            if (byte == 0) {
                /* No payload, go straight to checksum */
                g_parser.state = BLE_ETH_PARSER_WAIT_CSUM;
            } else if (byte > BLE_ETH_MAX_PAYLOAD_LEN) {
                /* Payload too long, skip this frame */
                g_reliability_stats.frame_rx_len_err++;
                g_parser.state = BLE_ETH_PARSER_WAIT_HEAD1;
            } else {
                g_parser.rx_index = 0;
                g_parser.state = BLE_ETH_PARSER_WAIT_PAYLOAD;
            }
            break;

        case BLE_ETH_PARSER_WAIT_PAYLOAD:
            g_parser.frame.payload[g_parser.rx_index] = byte;
            g_parser.checksum += byte;
            g_parser.rx_index++;
            if (g_parser.rx_index >= g_parser.frame.payload_len) {
                g_parser.state = BLE_ETH_PARSER_WAIT_CSUM;
            }
            break;

        case BLE_ETH_PARSER_WAIT_CSUM:
            if (byte == g_parser.checksum) {
                /* Valid frame, dispatch */
                g_reliability_stats.frame_rx_total++;
                dispatch_frame(&g_parser.frame);
            } else {
                g_reliability_stats.frame_rx_checksum_err++;
            }
            /* Either way, restart */
            g_parser.state = BLE_ETH_PARSER_WAIT_HEAD1;
            break;
        }
    }
}

/* ======================== Public API ===================================== */

static void queue_connect_response(uint8_t addr, uint8_t conn_id, uint8_t status)
{
    uint8_t rsp[2] = { conn_id, status };
    uint8_t buf[16];
    uint16_t len = build_frame(buf, addr, BLE_ETH_CMD_CONNECT_RESP, rsp, 2);
    tx_queue_push_front(buf, len);
}

void BLE_Eth_Init(ble_eth_event_cb_t event_cb)
{
    g_event_cb = event_cb;
    memset(&g_parser, 0, sizeof(g_parser));
    g_parser.state = BLE_ETH_PARSER_WAIT_HEAD1;
    memset(g_connections, 0, sizeof(g_connections));
    g_tcp_alloc_seeded = false;
    tx_queue_clear();
    memset(&g_reliability_stats, 0, sizeof(g_reliability_stats));
    memset(&g_last_data_ack, 0, sizeof(g_last_data_ack));
    reassembly_clear();
}

void BLE_Eth_GetReliabilityStats(ble_eth_reliability_stats_t *out_stats)
{
    if (!out_stats) return;
    *out_stats = g_reliability_stats;
}

void BLE_Eth_ResetReliabilityStats(void)
{
    memset(&g_reliability_stats, 0, sizeof(g_reliability_stats));
}

uint16_t BLE_Eth_BuildConnect(uint8_t *buf, const uint8_t remote_ip[4], uint16_t remote_port)
{
    uint8_t payload[BLE_ETH_CONNECT_PAYLOAD_LEN];
    memcpy(payload, remote_ip, 4);
    payload[4] = (remote_port >> 8) & 0xFF;
    payload[5] = remote_port & 0xFF;
    return build_frame(buf, 0x00, BLE_ETH_CMD_CONNECT, payload, BLE_ETH_CONNECT_PAYLOAD_LEN);
}

uint16_t BLE_Eth_BuildDisconnect(uint8_t *buf, uint8_t conn_id)
{
    return build_frame(buf, 0x00, BLE_ETH_CMD_DISCONNECT, &conn_id, BLE_ETH_DISCONNECT_PAYLOAD_LEN);
}

uint16_t BLE_Eth_BuildData(uint8_t *buf, uint8_t conn_id, const uint8_t *data, uint16_t len)
{
    if (len > BLE_ETH_MAX_PAYLOAD_LEN - BLE_ETH_FRAG_HEADER_LEN) {
        return 0; /* Needs fragmentation, use PushTargetData instead */
    }

    uint8_t payload[BLE_ETH_MAX_PAYLOAD_LEN];
    payload[0] = BLE_ETH_FRAG_FIRST;  /* FIRST, no MORE, count=1 */
    payload[1] = 0x00;                 /* seq 0 */
    payload[2] = conn_id;

    if (data && len > 0) {
        memcpy(&payload[3], data, len);
    }

    return build_frame(buf, 0x00, BLE_ETH_CMD_DATA, payload,
                       BLE_ETH_FRAG_HEADER_LEN + len);
}

uint16_t BLE_Eth_BuildInfoRequest(uint8_t *buf)
{
    return build_frame(buf, 0x00, BLE_ETH_CMD_INFO, NULL, 0);
}

uint16_t BLE_Eth_BuildResponse(uint8_t *buf, uint8_t cmd, uint8_t addr,
                               uint8_t status, const uint8_t *payload, uint8_t plen)
{
    uint8_t rsp[1 + 252];
    rsp[0] = status;
    if (payload && plen > 0) {
        memcpy(&rsp[1], payload, plen);
    }
    return build_frame(buf, addr, cmd, rsp, plen + 1);
}

const ble_eth_conn_t *BLE_Eth_GetConnection(uint8_t conn_id)
{
    if (conn_id >= BLE_ETH_MAX_CONNECTIONS) return NULL;
    if (!g_connections[conn_id].active) return NULL;
    return &g_connections[conn_id];
}

uint8_t BLE_Eth_AllocConnection(const uint8_t remote_ip[4], uint16_t remote_port)
{
    for (uint8_t i = 0; i < BLE_ETH_MAX_CONNECTIONS; i++) {
        if (!g_connections[i].active) {
            g_connections[i].active = true;
            g_connections[i].conn_id = i;
            memcpy(g_connections[i].remote_ip, remote_ip, 4);
            g_connections[i].remote_port = remote_port;
            g_connections[i].tcp_state = BLE_ETH_TCP_CLOSED;
            g_connections[i].local_seq = alloc_initial_seq();
            g_connections[i].local_ack = 0;
            g_connections[i].remote_seq = 0;
            g_connections[i].remote_ack = 0;
            g_connections[i].local_port = alloc_local_port();
            memset(g_connections[i].target_mac, 0, 6);
            g_connections[i].arp_timeout_ticks = 10;  /* 10 second timeout */
            g_connections[i].peer_addr = 0;
            return i;
        }
    }
    return BLE_ETH_INVALID_CONN_ID;
}

void BLE_Eth_FreeConnection(uint8_t conn_id)
{
    if (conn_id >= BLE_ETH_MAX_CONNECTIONS) return;
    memset(&g_connections[conn_id], 0, sizeof(ble_eth_conn_t));
}

/* Handle an incoming ARP reply and resolve pending connections. */
bool BLE_Eth_HandleArpReply(const uint8_t sender_ip[4], const uint8_t sender_mac[6])
{
    for (uint8_t i = 0; i < BLE_ETH_MAX_CONNECTIONS; i++) {
        ble_eth_conn_t *c = &g_connections[i];
        if (c->active && c->tcp_state == BLE_ETH_TCP_ARP_PENDING &&
            memcmp(c->remote_ip, sender_ip, 4) == 0) {
            memcpy(c->target_mac, sender_mac, 6);
            c->tcp_state = BLE_ETH_TCP_SYN_SENT;
            c->syn_seq = c->local_seq;
            c->syn_retries_left = BLE_ETH_SYN_MAX_RETRIES;
            c->arp_timeout_ticks = BLE_ETH_SYN_RETRY_TICKS;
            if (!send_tcp_syn(i)) {
                c->tcp_state = BLE_ETH_TCP_CLOSED;
                c->active = false;
                queue_connect_response(c->peer_addr, BLE_ETH_INVALID_CONN_ID, BLE_ETH_STATUS_NO_ROUTE);
            }
            return true;
        }
    }
    return false;
}

/* Check if any BLE-Eth connections exist (active or ARP-pending). */
bool BLE_Eth_HasActiveOrPendingConnection(void)
{
    for (uint8_t i = 0; i < BLE_ETH_MAX_CONNECTIONS; i++) {
        if (g_connections[i].active) return true;
    }
    return false;
}

/* Periodic tick handler for ARP/SYN timeout processing. */
bool BLE_Eth_PeriodicTick(void)
{
    bool tx_queued = false;

    for (uint8_t i = 0; i < BLE_ETH_MAX_CONNECTIONS; i++) {
        ble_eth_conn_t *c = &g_connections[i];
        if (c->active && c->tcp_state == BLE_ETH_TCP_ARP_PENDING) {
            if (c->arp_timeout_ticks > 0) {
                c->arp_timeout_ticks--;
                if (c->arp_timeout_ticks == 0) {
                    if (c->syn_retries_left > 0) {
                        c->syn_retries_left--;
                        c->arp_timeout_ticks = BLE_ETH_SYN_RETRY_TICKS;
                        send_arp_request(i);
                    } else {
                        c->tcp_state = BLE_ETH_TCP_CLOSED;
                        c->active = false;
                        queue_connect_response(c->peer_addr, BLE_ETH_INVALID_CONN_ID, BLE_ETH_STATUS_TIMEOUT);
                        tx_queued = true;
                        evt_conn_opened(BLE_ETH_INVALID_CONN_ID, BLE_ETH_STATUS_TIMEOUT);
                    }
                }
            }
        }
        else if (c->active && c->tcp_state == BLE_ETH_TCP_SYN_SENT) {
            if (c->arp_timeout_ticks > 0) {
                c->arp_timeout_ticks--;
                if (c->arp_timeout_ticks == 0) {
                    if (c->syn_retries_left > 0) {
                        c->syn_retries_left--;
                        c->arp_timeout_ticks = BLE_ETH_SYN_RETRY_TICKS;
                        if (!send_tcp_syn(i)) {
                            c->tcp_state = BLE_ETH_TCP_CLOSED;
                            c->active = false;
                            queue_connect_response(c->peer_addr, BLE_ETH_INVALID_CONN_ID, BLE_ETH_STATUS_NO_ROUTE);
                            tx_queued = true;
                            evt_conn_opened(BLE_ETH_INVALID_CONN_ID, BLE_ETH_STATUS_NO_ROUTE);
                        }
                    } else {
                        c->tcp_state = BLE_ETH_TCP_CLOSED;
                        c->active = false;
                        queue_connect_response(c->peer_addr, BLE_ETH_INVALID_CONN_ID, BLE_ETH_STATUS_TIMEOUT);
                        tx_queued = true;
                        evt_conn_opened(BLE_ETH_INVALID_CONN_ID, BLE_ETH_STATUS_TIMEOUT);
                    }
                }
            }
        }
    }

    return tx_queued;
}

bool BLE_Eth_PushTargetData(uint8_t conn_id, const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) return false;

    /* Calculate fragmentation */
    uint16_t max_frag_data = BLE_ETH_MAX_PAYLOAD_LEN - BLE_ETH_FRAG_HEADER_LEN;
    uint8_t total_frags = (uint8_t)((len + max_frag_data - 1U) / max_frag_data);
    if (total_frags == 0) total_frags = 1;
    if (total_frags > 6U) return false; /* Too large (fits one full-size TCP segment) */

    /* Enqueue fragments atomically. If we enqueue only a prefix of a TCP segment
     * but still ACK it at the TCP layer, the byte stream becomes corrupted.
     */
    txq_lock();
    uint8_t depth_for_data = BLE_ETH_TX_QUEUE_DEPTH;
    if (depth_for_data > BLE_ETH_TX_QUEUE_CTRL_RESERVE) {
        depth_for_data = (uint8_t)(depth_for_data - BLE_ETH_TX_QUEUE_CTRL_RESERVE);
    }
    if ((uint16_t)(g_tx_queue.count + total_frags) > depth_for_data) {
        g_reliability_stats.tx_queue_full_drop++;
        txq_unlock();
        return false;
    }

    uint16_t offset = 0;
    for (uint8_t seq = 0; seq < total_frags; seq++) {
        uint16_t chunk_len = (uint16_t)(len - offset);
        if (chunk_len > max_frag_data) chunk_len = max_frag_data;

        uint8_t frag_header[3];
        frag_header[0] = (uint8_t)(BLE_ETH_FRAG_COUNT_MASK & total_frags);  /* count */
        if (seq == 0) frag_header[0] |= BLE_ETH_FRAG_FIRST;
        if (seq < (uint8_t)(total_frags - 1U)) frag_header[0] |= BLE_ETH_FRAG_MORE;
        frag_header[1] = seq;
        frag_header[2] = conn_id;

        /* Build complete frame: header + frag_header + data + checksum */
        uint16_t total_payload = (uint16_t)(BLE_ETH_FRAG_HEADER_LEN + chunk_len);
        uint8_t full_buf[BLE_ETH_TX_CHUNK_SIZE];
        uint16_t off = 0;

        full_buf[off++] = BLE_ETH_FRAME_HEAD1;
        full_buf[off++] = BLE_ETH_FRAME_HEAD2;
        full_buf[off++] = 0x00;  /* addr */
        full_buf[off++] = BLE_ETH_CMD_DATA_PUSH;
        full_buf[off++] = (uint8_t)total_payload;
        memcpy(&full_buf[off], frag_header, 3);
        off += 3;
        memcpy(&full_buf[off], &data[offset], chunk_len);
        off += chunk_len;
        full_buf[off++] = calc_checksum(full_buf, off);

        memcpy(g_tx_queue.chunks[g_tx_queue.wr].data, full_buf, off);
        normalize_frame_checksum(g_tx_queue.chunks[g_tx_queue.wr].data, off);
        g_tx_queue.chunks[g_tx_queue.wr].len = off;

        g_tx_queue.wr++;
        if (g_tx_queue.wr >= BLE_ETH_TX_QUEUE_DEPTH) g_tx_queue.wr = 0;
        g_tx_queue.count++;

        offset = (uint16_t)(offset + chunk_len);
    }

    txq_unlock();
    return true;
}

uint16_t BLE_Eth_GetNextTxChunk(uint8_t *buf, uint16_t max_len)
{
    if (max_len == 0) return 0;

    txq_lock();

    if (g_tx_queue.count == 0) {
        txq_unlock();
        return 0;
    }

    ble_eth_tx_chunk_t *chunk = &g_tx_queue.chunks[g_tx_queue.rd];
    uint16_t len = chunk->len;
    if (len > max_len) len = max_len;
    memcpy(buf, chunk->data, len);

    txq_unlock();

    return len;
}

void BLE_Eth_ConsumeTxChunk(uint16_t len)
{
    txq_lock();

    while (len > 0 && g_tx_queue.count > 0) {
        ble_eth_tx_chunk_t *chunk = &g_tx_queue.chunks[g_tx_queue.rd];

        if (len < chunk->len) {
            memmove(chunk->data, chunk->data + len, chunk->len - len);
            chunk->len = (uint16_t)(chunk->len - len);
            g_tx_head_partial = true;
            txq_unlock();
            return;
        }

        len = (uint16_t)(len - chunk->len);

        g_tx_queue.rd++;
        if (g_tx_queue.rd >= BLE_ETH_TX_QUEUE_DEPTH) g_tx_queue.rd = 0;
        g_tx_queue.count--;
        g_tx_head_partial = false;
    }

    txq_unlock();
}

/* ======================== ECM TCP frame generation ======================= */

static uint16_t ecm_get_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t ecm_get_be32(const uint8_t *p)
{
    return (uint32_t)(p[0]) << 24 | (uint32_t)(p[1]) << 16 |
           (uint32_t)(p[2]) << 8  | (uint32_t)(p[3]);
}

static void ecm_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}

static void ecm_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

static uint16_t ip_checksum(const uint8_t *hdr, uint16_t len)
{
    uint32_t sum = 0;
    for (uint16_t i = 0; i < len; i += 2) {
        sum += (uint16_t)(hdr[i] << 8) | hdr[i + 1];
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (~sum) & 0xFFFF;
}

/* TCP checksum: covers pseudo-header + TCP header + payload */
static uint16_t tcp_checksum(const uint8_t *tcp_hdr, uint16_t tcp_len,
                             const uint8_t *src_ip, const uint8_t *dst_ip)
{
    uint32_t sum = 0;

    /* Pseudo-header: src IP + dst IP + zero + protocol + TCP length */
    sum += (uint16_t)(src_ip[0] << 8) | src_ip[1];
    sum += (uint16_t)(src_ip[2] << 8) | src_ip[3];
    sum += (uint16_t)(dst_ip[0] << 8) | dst_ip[1];
    sum += (uint16_t)(dst_ip[2] << 8) | dst_ip[3];
    sum += 0x0006;  /* zero + protocol (TCP=6) */
    sum += tcp_len;  /* TCP length in network byte order */

    /* TCP header + payload */
    for (uint16_t i = 0; i < tcp_len; i += 2) {
        uint16_t word;
        if (i + 1 < tcp_len) {
            word = (uint16_t)(tcp_hdr[i] << 8) | tcp_hdr[i + 1];
        } else {
            word = (uint16_t)(tcp_hdr[i] << 8);  /* pad last odd byte */
        }
        sum += word;
    }

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (~sum) & 0xFFFF;
}

/* Build Ethernet + IPv4 + TCP frame and send via ECM callback.
   Returns 1 on success, 0 if no callback registered. */
static uint8_t send_tcp_frame(uint8_t conn_id, uint16_t tcp_flags,
                              const uint8_t *payload, uint16_t payload_len)
{
    if (!g_ecm_tx_cb) return 0;

    ble_eth_conn_t *c = &g_connections[conn_id];
    if (!c->active) return 0;

    /* Build frame: 14 Eth + 20 IPv4 + 20 TCP + payload */
    uint16_t total = 14 + 20 + 20 + payload_len;
    if (total > 1514) return 0;

    uint8_t buf[1514];
    uint16_t off = 0;

    /* Ethernet header */
    memcpy(&buf[off], c->target_mac, 6); off += 6;  /* dst = resolved target MAC */
    memcpy(&buf[off], g_dev_mac, 6);     off += 6;  /* src = device */
    ecm_put_be16(&buf[off], 0x0800); off += 2;   /* EtherType = IPv4 */

    /* IPv4 header */
    uint16_t ip_total = 20 + 20 + payload_len;
    buf[off + 0] = 0x45;              /* v4, IHL=5 */
    buf[off + 1] = 0x00;              /* DSCP */
    ecm_put_be16(&buf[off + 2], ip_total);
    ecm_put_be16(&buf[off + 4], 0x0001); /* identification */
    ecm_put_be16(&buf[off + 6], 0x4000); /* DF */
    buf[off + 8] = 64;                /* TTL */
    buf[off + 9] = 6;                 /* protocol = TCP */
    buf[off + 10] = 0; buf[off + 11] = 0; /* checksum placeholder */
    memcpy(&buf[off + 12], g_dev_ip, 4);    /* src IP */
    memcpy(&buf[off + 16], c->remote_ip, 4); /* dst IP */
    uint16_t ip_chk = ip_checksum(&buf[off], 20);
    ecm_put_be16(&buf[off + 10], ip_chk);
    off += 20;

    /* TCP header */
    ecm_put_be16(&buf[off + 0], c->local_port);   /* src port */
    ecm_put_be16(&buf[off + 2], c->remote_port);  /* dst port */
    ecm_put_be32(&buf[off + 4], c->local_seq);    /* seq */
    ecm_put_be32(&buf[off + 8], c->local_ack);    /* ack */
    buf[off + 12] = 0x50;             /* data offset = 5 (20 bytes) */
    buf[off + 13] = tcp_flags;        /* flags */
    ecm_put_be16(&buf[off + 14], 0xFFFF); /* window */
    ecm_put_be16(&buf[off + 16], 0);      /* checksum placeholder */
    buf[off + 18] = 0; buf[off + 19] = 0; /* urgent pointer */

    if (payload && payload_len > 0) {
        memcpy(&buf[off + 20], payload, payload_len);
    }

    /* TCP checksum: pseudo-header + TCP header + payload */
    uint16_t tcp_len = 20 + payload_len;
    uint16_t tcp_chk = tcp_checksum(&buf[off], tcp_len, g_dev_ip, c->remote_ip);
    ecm_put_be16(&buf[off + 16], tcp_chk);
    off += 20;

    /* Payload */
    if (payload && payload_len > 0) {
        off += payload_len;
    }

    /* Update TCP sequence numbers */
    if (tcp_flags & 0x02) c->local_seq++;  /* SYN increments seq */
    if (tcp_flags & 0x01) c->local_seq++;  /* FIN increments seq */
    if (payload_len > 0) c->local_seq += payload_len;

    g_ecm_tx_cb(buf, total);
    return 1;
}

static uint8_t send_tcp_syn(uint8_t conn_id)
{
    if (conn_id >= BLE_ETH_MAX_CONNECTIONS) return 0;

    ble_eth_conn_t *c = &g_connections[conn_id];
    uint32_t saved_next_seq = c->local_seq;

    c->local_seq = c->syn_seq;
    uint8_t ok = send_tcp_frame(conn_id, 0x02, NULL, 0);  /* SYN */
    c->local_seq = ok ? (c->syn_seq + 1U) : saved_next_seq;
    return ok;
}

/* Build and send an ARP request for a connection's target IP.
   Sent via ECM bridge so the Mac can forward it to the physical network. */
static void send_arp_request(uint8_t conn_id)
{
    if (!g_ecm_tx_cb) return;

    ble_eth_conn_t *c = &g_connections[conn_id];
    if (!c->active) return;

    uint8_t buf[42];

    /* Ethernet header: always broadcast for ARP requests so the target PC
     * actually receives the frame (its NIC would drop unicast to Mac's MAC). */
    memset(&buf[0], 0xFF, 6);                      /* dst = broadcast */
    memcpy(&buf[6], g_dev_mac, 6);      /* src MAC = device (target-side) */
    buf[12] = 0x08; buf[13] = 0x06;     /* EtherType = ARP */

    /* ARP request */
    buf[14 + 0] = 0x00; buf[14 + 1] = 0x01;  /* hardware type = Ethernet */
    buf[14 + 2] = 0x08; buf[14 + 3] = 0x00;  /* protocol type = IPv4 */
    buf[14 + 4] = 6;  buf[14 + 5] = 4;       /* hlen=6, plen=4 */
    buf[14 + 6] = 0x00; buf[14 + 7] = 0x01;  /* op = ARP request */
    memcpy(&buf[14 + 8], g_dev_mac, 6);       /* sender MAC */
    memcpy(&buf[14 + 14], g_dev_ip, 4);       /* sender IP = firmware target-side */
    memset(&buf[14 + 18], 0x00, 6);           /* target MAC = unknown (zeros) */
    memcpy(&buf[14 + 24], c->remote_ip, 4);   /* target IP = connection target */

    g_ecm_tx_cb(buf, 42);
}

/* Send TCP data through tunnel. Wraps in Ethernet/IPv4/TCP.
 * Payloads larger than the TCP MSS are split into multiple segments:
 * a single oversized frame would exceed the 1514-byte Ethernet MTU and
 * be dropped silently by send_tcp_frame(), stalling the tunnel. */
#define BLE_ETH_TCP_MSS  1460U  /* 1514 - 14 Eth - 20 IPv4 - 20 TCP */

void BLE_Eth_SendDataToEth(uint8_t conn_id, const uint8_t *data, uint16_t len)
{
    if (conn_id >= BLE_ETH_MAX_CONNECTIONS) return;
    ble_eth_conn_t *c = &g_connections[conn_id];
    if (!c->active || c->tcp_state != BLE_ETH_TCP_ESTABLISHED) return;

    uint16_t off = 0U;
    while (off < len) {
        uint16_t seg_len = (uint16_t)(len - off);
        if (seg_len > BLE_ETH_TCP_MSS) seg_len = BLE_ETH_TCP_MSS;
        send_tcp_frame(conn_id, 0x18, data + off, seg_len);  /* PSH+ACK */
        off = (uint16_t)(off + seg_len);
    }
}

/* ======================== Target frame handler =========================== */

static uint16_t frame_get_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

bool BLE_Eth_HandleTargetFrame(const uint8_t *frame, uint16_t len)
{
    if (!frame || len < 14u) return false;

    uint16_t off = 0;

    /* Parse Ethernet header (handle VLAN tags) */
    uint16_t eth_type = frame_get_be16(&frame[12]);
    off = 14u;
    while ((eth_type == 0x8100u || eth_type == 0x88A8u) && off + 4u <= len) {
        eth_type = frame_get_be16(&frame[off + 2]);
        off += 4;
    }

    if (eth_type != 0x0800u || off + 20u > len) return false;  /* not IPv4 */

    const uint8_t *ip = &frame[off];
    if ((ip[0] >> 4) != 4u) return false;

    uint16_t ip_header_len = (uint16_t)((ip[0] & 0x0Fu) * 4u);
    if (ip_header_len < 20u || off + ip_header_len + 20u > len) return false;
    if (ip[9] != 6u) return false;  /* not TCP */

    const uint8_t *tcp = &frame[off + ip_header_len];
    uint16_t tcp_segment_len = frame_get_be16(&ip[2]) - ip_header_len;
    uint16_t tcp_header_len = (uint16_t)((tcp[12] >> 4) * 4u);
    if (tcp_header_len < 20u || tcp_header_len > tcp_segment_len) return false;

    uint8_t tcp_flags = tcp[13];
    uint32_t remote_seq = ecm_get_be32(&tcp[4]);
    uint32_t remote_ack = ecm_get_be32(&tcp[8]);
    uint16_t tcp_payload_len = tcp_segment_len - tcp_header_len;

    /* Extract source/dest IP and ports from the frame */
    uint8_t src_ip[4];
    memcpy(src_ip, &ip[12], 4);
    uint8_t dst_ip[4];
    memcpy(dst_ip, &ip[16], 4);
    uint16_t src_port = frame_get_be16(&tcp[0]);
    uint16_t dst_port = frame_get_be16(&tcp[2]);

    /* Match against active connections using full 4-tuple:
     * remote_ip:remote_port == src_ip:src_port  AND
     * local_port == dst_port */
    int8_t conn_id = -1;
    for (uint8_t i = 0; i < BLE_ETH_MAX_CONNECTIONS; i++) {
        if (g_connections[i].active &&
            memcmp(g_connections[i].remote_ip, src_ip, 4) == 0 &&
            g_connections[i].remote_port == src_port &&
            g_connections[i].local_port == dst_port) {
            conn_id = (int8_t)i;
            break;
        }
    }

    if (conn_id < 0) return false;

    ble_eth_conn_t *c = &g_connections[(uint8_t)conn_id];

    /* Handle SYN-ACK: complete TCP handshake */
    if ((tcp_flags & 0x12) == 0x12) {  /* SYN+ACK */
        PWM_LED_SetBrightness(LED_DIAG_SYN_ACK_RX);  // LED pattern 92: SYN-ACK received
        c->tcp_state = BLE_ETH_TCP_ESTABLISHED;
        c->arp_timeout_ticks = 0;
        c->syn_retries_left = 0;
        c->remote_seq = remote_seq;
        c->remote_ack = remote_ack;
        c->local_ack = remote_seq + 1;  /* ACK the SYN */
        send_tcp_frame((uint8_t)conn_id, 0x10, NULL, 0);  /* ACK */
        queue_connect_response(c->peer_addr, (uint8_t)conn_id, BLE_ETH_STATUS_SUCCESS);
        evt_conn_opened((uint8_t)conn_id, BLE_ETH_STATUS_SUCCESS);
        return true;
    }

    /* Handle RST: connection refused */
    if (tcp_flags & 0x04) {
        uint8_t was_connecting = (c->tcp_state == BLE_ETH_TCP_SYN_SENT) ? 1u : 0u;
        c->tcp_state = BLE_ETH_TCP_CLOSED;
        c->active = false;
        if (was_connecting) {
            queue_connect_response(c->peer_addr, BLE_ETH_INVALID_CONN_ID, BLE_ETH_STATUS_NO_ROUTE);
            return true;
        }
        return false;
    }

    /* Update ack number from target's response */
    if (remote_ack > c->remote_ack) {
        c->remote_ack = remote_ack;
    }

    /* Handle pure ACKs from target */
    if (tcp_payload_len == 0) return false;



    /* Only forward in-order TCP payload. Any silent drop while advancing ACK
     * will corrupt the byte stream (e.g. SSH MAC errors). Apply backpressure by
     * withholding ACK advancement when the BLE TX queue is full.
     */
    uint32_t expected_seq = c->local_ack;
    if (remote_seq != expected_seq) {
        /* Duplicate or out-of-order segment: don't forward; just ACK what we've
         * already accepted.
         */
        send_tcp_frame((uint8_t)conn_id, 0x10, NULL, 0);
        return true;
    }

    /* Push TCP payload back to BLE host */
    const uint8_t *tcp_payload = &tcp[tcp_header_len];
    bool pushed = BLE_Eth_PushTargetData((uint8_t)conn_id, tcp_payload, tcp_payload_len);
    if (pushed) {
        c->local_ack = remote_seq + tcp_payload_len;
    }
    send_tcp_frame((uint8_t)conn_id, 0x10, NULL, 0);  /* ACK received data (or dup-ACK for backpressure) */
    return pushed;
}

/* ======================== Public API ===================================== */

void BLE_Eth_ResetParser(void)
{
    g_parser.state = BLE_ETH_PARSER_WAIT_HEAD1;
    g_parser.rx_index = 0;
    g_parser.checksum = 0;
    reassembly_clear();
}

bool BLE_Eth_HasActiveTunnel(void)
{
    for (uint8_t i = 0; i < BLE_ETH_MAX_CONNECTIONS; i++) {
        if (g_connections[i].active &&
            g_connections[i].tcp_state == BLE_ETH_TCP_ESTABLISHED) {
            return true;
        }
    }
    return false;
}

bool BLE_Eth_AutoConnect(void)
{
    if (!DefaultTargetConfig_IsEnabled()) {
        return false;
    }

    uint8_t ip[4];
    DefaultTargetConfig_GetIP(ip);
    uint16_t port = DefaultTargetConfig_GetPort();


    uint8_t conn_id = BLE_Eth_AllocConnection(ip, port);
    if (conn_id == BLE_ETH_INVALID_CONN_ID) {
        return false;
    }

    if (!g_host_mac_valid) {
        memcpy(g_host_mac_learned, g_host_mac, 6);
        g_host_mac_valid = true;
    }

    ble_eth_conn_t *c = &g_connections[conn_id];
    c->peer_addr = 0;
    c->syn_retries_left = BLE_ETH_SYN_MAX_RETRIES;
    /* Start in ARP_PENDING so we discover the target PC's actual MAC
     * before sending the SYN. The SYN will be sent after the ARP reply
     * arrives (in BLE_Eth_HandleArpReply) or via SYN retry timer. */
    c->tcp_state = BLE_ETH_TCP_ARP_PENDING;
    c->arp_timeout_ticks = BLE_ETH_SYN_RETRY_TICKS;

    send_arp_request(conn_id);

    return true;
}

#endif /* USB_WORK_MODE_BRIDGE_ECM */
