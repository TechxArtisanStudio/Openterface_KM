/********************************** (C) COPYRIGHT *******************************
 * File Name          : ble_eth_protocol.h
 * Author             : Openterface Team
 * Version            : V1.0.0
 * Date               : 2026/05/21
 * Description        : BLE-to-Ethernet tunneling protocol based on CH9329 framing.
 *                      Supports TCP tunneling over BLE with fragmentation for
 *                      SSH and control traffic. Crypto (SSE) handled by host app.
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

#ifndef __BLE_ETH_PROTOCOL_H
#define __BLE_ETH_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include "usb_work_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== Frame Format ===================================
 *
 *  +------+------+-------+------+--------+----------+------+
 *  | 57   | AB   | Addr  | Cmd  | PayLen | Payload  | CSum |
 *  | 1    | 1    | 1     | 1    | 1      | 0..252   | 1    |
 *  +------+------+-------+------+--------+----------+------+
 *
 *  Checksum: 8-bit additive sum of bytes [0..4+PayLen-1]
 *  Response command: request cmd | 0x80 (success) or | 0xC0 (error)
 */

#define BLE_ETH_FRAME_HEAD1       0x57U
#define BLE_ETH_FRAME_HEAD2       0xABU
#define BLE_ETH_FRAME_MIN_LEN     6U    /* head1+head2+addr+cmd+len+csum */
#define BLE_ETH_FRAME_HEAD_LEN    5U    /* bytes before payload */
#define BLE_ETH_MAX_PAYLOAD_LEN   249U  /* max payload in one 255-byte notification */

/* ======================== Status Codes =================================== */

#define BLE_ETH_STATUS_SUCCESS      0x00U
#define BLE_ETH_STATUS_TIMEOUT      0xE1U
#define BLE_ETH_STATUS_HEADER_ERR   0xE2U
#define BLE_ETH_STATUS_BAD_CMD      0xE3U
#define BLE_ETH_STATUS_CHECKSUM_ERR 0xE4U
#define BLE_ETH_STATUS_PARAM_ERR    0xE5U
#define BLE_ETH_STATUS_FRAME_ERR    0xE6U
#define BLE_ETH_STATUS_CONN_FULL    0xE7U
#define BLE_ETH_STATUS_NO_ROUTE     0xE8U

/* ======================== Commands =======================================
 *
 *  Request commands (host -> device)
 *  Response commands (device -> host): cmd | 0x80 (ok) or | 0xC0 (err)
 */

#define BLE_ETH_CMD_CONNECT       0x10U  /* Establish TCP tunnel */
#define BLE_ETH_CMD_DATA          0x11U  /* Send tunneled data */
#define BLE_ETH_CMD_DISCONNECT    0x12U  /* Close TCP tunnel */
#define BLE_ETH_CMD_INFO          0x1FU  /* Device info / capabilities */
#define BLE_ETH_CMD_PING          0x1EU  /* Echo/ping test (no network) */
#define BLE_ETH_CMD_SET_DEFAULT   0x20U  /* Set default target IP/port */
#define BLE_ETH_CMD_GET_DEFAULT   0x21U  /* Get default target IP/port */

/* Response / push commands */
#define BLE_ETH_CMD_CONNECT_RESP  0x90U  /* Connect response (0x10 | 0x80) */
#define BLE_ETH_CMD_DATA_RESP     0x91U  /* Data acknowledged */
#define BLE_ETH_CMD_DATA_PUSH     0x91U  /* Incoming data push (same as DATA_RESP) */
#define BLE_ETH_CMD_DISCONN_RESP  0x92U  /* Disconnect confirmed */
#define BLE_ETH_CMD_CONN_CLOSED   0xD2U  /* Remote-side closed (0x12 | 0xC0) */
#define BLE_ETH_CMD_INFO_RESP     0x9FU  /* Info response */
#define BLE_ETH_CMD_PONG          0x9EU  /* Ping response (echo) */
#define BLE_ETH_CMD_SET_DEFAULT_RESP 0xA0U  /* Set default response */
#define BLE_ETH_CMD_GET_DEFAULT_RESP 0xA1U  /* Get default response */
#define BLE_ETH_CMD_ERROR         0xD0U  /* Generic error (cmd | 0xC0) */

/* ======================== Default Target Configuration =================== */

/* Compile-time fallback for default target IP/port when no flash config exists.
 * These are used for auto-connect on BLE link-up.
 */
#ifndef BLE_ETH_DEFAULT_TARGET_IP
#define BLE_ETH_DEFAULT_TARGET_IP       { 192, 168, 11, 2 }
#endif

#ifndef BLE_ETH_DEFAULT_TARGET_PORT
#define BLE_ETH_DEFAULT_TARGET_PORT     22U
#endif

/* ======================== Connection Management ========================== */

#define BLE_ETH_MAX_CONNECTIONS   2U     /* Max simultaneous tunnels */
#define BLE_ETH_INVALID_CONN_ID   0xFFU

/* ======================== Fragmentation ==================================
 *
 *  Payload layout for BLE_ETH_CMD_DATA (request and push):
 *
 *    +------+------+------+-------+
 *    | Frag | Seq  | ConnID| Data  | ...
 *    | 1    | 1    | 1     | 0..N  |
 *    +------+------+------+-------+
 *
 *  Frag byte (bit field):
 *    bit 7    : MORE_FRAGMENTS (1 = more fragments follow)
 *    bit 6    : FIRST_FRAGMENT (1 = this is the first fragment)
 *    bits 5-4 : reserved
 *    bits 3-0 : total fragment count (1..15)
 *
 *  Seq byte: fragment sequence number (0-based)
 *
 *  Single unfragmented message: FIRST_FRAGMENT set, MORE_FRAGMENTS clear,
 *  total_count = 1, seq = 0.
 *
 *  Max reassembled payload: 15 fragments x ~247 bytes data = ~3.7 KB
 */

#define BLE_ETH_FRAG_MORE        0x80U  /* bit 7: more fragments follow */
#define BLE_ETH_FRAG_FIRST       0x40U  /* bit 6: this is the first fragment */
#define BLE_ETH_FRAG_COUNT_MASK  0x0FU  /* bits 3-0: total fragment count */

#define BLE_ETH_FRAG_HEADER_LEN  3U     /* frag_flags + seq + conn_id */

/* ======================== Connect Payload ================================
 *
 *  BLE_ETH_CMD_CONNECT request payload:
 *    [0..3] : destination IP address (4 bytes, big-endian)
 *    [4..5] : destination port (2 bytes, big-endian)
 *
 *  BLE_ETH_CMD_CONNECT response payload (success):
 *    [0] : connection ID (0..BLE_ETH_MAX_CONNECTIONS-1)
 *    [1] : status (BLE_ETH_STATUS_SUCCESS or error)
 */

#define BLE_ETH_CONNECT_PAYLOAD_LEN  6U

/* ======================== Disconnect Payload =============================
 *
 *  BLE_ETH_CMD_DISCONNECT request payload:
 *    [0] : connection ID
 *
 *  BLE_ETH_CMD_DISCONNECT response payload:
 *    [0] : connection ID
 *    [1] : status
 */

#define BLE_ETH_DISCONNECT_PAYLOAD_LEN  1U

/* ======================== Data Push Payload ==============================
 *
 *  Fragment header (3 bytes) followed by tunneled TCP data.
 *  The actual TCP data length = payload_len - BLE_ETH_FRAG_HEADER_LEN.
 */

/* ======================== Data ACK Payload ===============================
 *
 *  BLE_ETH_CMD_DATA_RESP payload supports two formats:
 *
 *  Legacy format (1 byte):
 *    [0] : status
 *
 *  Metadata format (4 bytes):
 *    [0] : status
 *    [1] : conn_id
 *    [2] : seq (received fragment sequence)
 *    [3] : expected_seq (next sequence expected by receiver)
 *
 *  Metadata+window format (5 bytes):
 *    [0] : status
 *    [1] : conn_id
 *    [2] : seq (received fragment sequence)
 *    [3] : expected_seq (next sequence expected by receiver)
 *    [4] : rx_window (recommended sender in-flight fragment window, 1..15)
 */

#define BLE_ETH_DATA_ACK_PAYLOAD_LEN          1U
#define BLE_ETH_DATA_ACK_META_PAYLOAD_LEN     4U
#define BLE_ETH_DATA_ACK_WINDOW_PAYLOAD_LEN   5U

/* ======================== Info Payload ===================================
 *
 *  BLE_ETH_CMD_INFO request: no payload (len=0)
 *
 *  BLE_ETH_CMD_INFO response payload (8 bytes):
 *    [0]   : protocol version (0x01)
 *    [1]   : max connections
 *    [2..3]: max single-fragment payload (big-endian)
 *    [4]   : supported features bitmask
 *             bit 0: TCP tunneling
 *             bit 1: SSH-optimized mode
 *             bit 2: DATA ACK metadata ([status, conn_id, seq, expected_seq])
 *             bit 3: DATA ACK receiver window ([status, conn_id, seq, expected_seq, rx_window])
 *    [5..7]: reserved
 */

#define BLE_ETH_INFO_PAYLOAD_LEN   8U
#define BLE_ETH_PROTO_VERSION      0x01U
#define BLE_ETH_FEAT_TCP_TUNNEL    0x01U
#define BLE_ETH_FEAT_SSH_OPT       0x02U
#define BLE_ETH_FEAT_DATA_ACK_META 0x04U
#define BLE_ETH_FEAT_DATA_ACK_WINDOW 0x08U

/* ======================== Parser State =================================== */

typedef enum {
    BLE_ETH_PARSER_WAIT_HEAD1 = 0,
    BLE_ETH_PARSER_WAIT_HEAD2,
    BLE_ETH_PARSER_WAIT_ADDR,
    BLE_ETH_PARSER_WAIT_CMD,
    BLE_ETH_PARSER_WAIT_LEN,
    BLE_ETH_PARSER_WAIT_PAYLOAD,
    BLE_ETH_PARSER_WAIT_CSUM,
} ble_eth_parser_state_t;

/* ======================== Frame Structure ================================ */

typedef struct {
    uint8_t  addr;                        /* address / channel */
    uint8_t  cmd;                         /* command byte */
    uint8_t  payload[BLE_ETH_MAX_PAYLOAD_LEN];
    uint16_t payload_len;
} ble_eth_frame_t;

/* ======================== Connection Info ================================ */

/* TCP connection state machine for ECM bridge */
typedef enum {
    BLE_ETH_TCP_CLOSED = 0,
    BLE_ETH_TCP_ARP_PENDING,    /* Waiting for ARP resolution */
    BLE_ETH_TCP_SYN_SENT,
    BLE_ETH_TCP_ESTABLISHED,
} ble_eth_tcp_state_t;

typedef struct {
    uint8_t  conn_id;
    uint8_t  remote_ip[4];
    uint16_t remote_port;
    bool     active;
    /* TCP state for ECM frame generation */
    ble_eth_tcp_state_t tcp_state;
    uint32_t local_seq;           /* next seq to send to target */
    uint32_t syn_seq;             /* initial SYN sequence for retransmit */
    uint32_t local_ack;           /* next ack expected from target */
    uint32_t remote_seq;          /* next seq from target */
    uint32_t remote_ack;          /* ack we expect target to send */
    uint16_t local_port;          /* source port for ECM frames */
    uint8_t  target_mac[6];       /* Resolved target MAC via ARP */
    uint16_t arp_timeout_ticks;   /* ARP resolution countdown */
    uint8_t  syn_retries_left;    /* TCP SYN retries before connect timeout */
    uint8_t  peer_addr;           /* BLE protocol address from CONNECT */
} ble_eth_conn_t;

/* ======================== Parser Context ================================= */

typedef struct {
    ble_eth_parser_state_t state;
    ble_eth_frame_t        frame;
    uint16_t               rx_index;     /* bytes received into frame */
    uint8_t                checksum;     /* running checksum */
} ble_eth_parser_t;

/* ======================== Reassembly Context ============================= */

typedef struct {
    uint8_t  data[1536];                 /* max reassembled buffer (~6 x 247) */
    uint16_t total_len;
    uint16_t received_len;
    uint8_t  conn_id;
    uint8_t  total_frags;
    uint8_t  next_seq;
    bool     in_progress;
} ble_eth_reassembly_t;

/* ======================== Reliability Hooks ============================== */

typedef struct {
    uint32_t frag_rx_total;
    uint32_t frag_rx_duplicate;
    uint32_t frag_rx_out_of_order;
    uint32_t frag_reassembly_start;
    uint32_t frag_reassembly_complete;
    uint32_t frag_reassembly_abort;
    uint32_t frag_reassembly_bytes;
    uint32_t tx_ack_sent;
    uint32_t tx_queue_full_drop;

    /* Frame-level integrity / parser diagnostics */
    uint32_t frame_rx_total;
    uint32_t frame_rx_checksum_err;
    uint32_t frame_rx_len_err;
    uint32_t frame_rx_cmd_unknown;
} ble_eth_reliability_stats_t;

/* ======================== Event Types ==================================== */

typedef enum {
    BLE_ETH_EVT_FRAME_RX = 0,            /* Complete frame received */
    BLE_ETH_EVT_DATA_READY,              /* Reassembled data available */
    BLE_ETH_EVT_CONN_OPENED,             /* New tunnel established */
    BLE_ETH_EVT_CONN_CLOSED,             /* Tunnel closed */
    BLE_ETH_EVT_ERROR,                   /* Protocol error */
} ble_eth_evt_type_t;

typedef struct {
    ble_eth_evt_type_t type;
    union {
        ble_eth_frame_t  frame;
        struct {
            uint8_t  conn_id;
            uint8_t  status;
        } conn;
        struct {
            uint8_t  conn_id;
            uint8_t  status;
        } error;
        struct {
            uint8_t   conn_id;
            uint8_t  *data;
            uint16_t  len;
        } data;
    } info;
} ble_eth_evt_t;

/* ======================== Callback Type ================================== */

typedef void (*ble_eth_event_cb_t)(ble_eth_evt_t *evt);

/* ======================== API ============================================ */

#if USB_WORK_MODE_BRIDGE_ECM

/**
 * @brief Initialize the BLE-Eth protocol parser and connection table.
 * @param event_cb  Callback for protocol events (may be NULL).
 */
void BLE_Eth_Init(ble_eth_event_cb_t event_cb);

/**
 * @brief Feed received BLE bytes into the frame parser.
 *        Call repeatedly or with bulk data; the parser maintains state.
 * @param data  Received byte buffer.
 * @param len   Number of bytes in buffer.
 */
void BLE_Eth_Parse(const uint8_t *data, uint16_t len);

/**
 * @brief Build a CONNECT request frame into buffer.
 * @param buf         Output buffer (must hold at least 12 bytes).
 * @param remote_ip   4-byte destination IP (big-endian).
 * @param remote_port Destination port (big-endian).
 * @return Frame length in bytes.
 */
uint16_t BLE_Eth_BuildConnect(uint8_t *buf, const uint8_t remote_ip[4], uint16_t remote_port);

/**
 * @brief Build a DISCONNECT request frame into buffer.
 * @param buf      Output buffer (must hold at least 8 bytes).
 * @param conn_id  Connection ID to close.
 * @return Frame length in bytes.
 */
uint16_t BLE_Eth_BuildDisconnect(uint8_t *buf, uint8_t conn_id);

/**
 * @brief Build a DATA frame (single, no fragmentation) into buffer.
 * @param buf      Output buffer.
 * @param conn_id  Connection ID.
 * @param data     Payload to tunnel.
 * @param len      Payload length (must fit in single frame).
 * @return Frame length in bytes, 0 if too large.
 */
uint16_t BLE_Eth_BuildData(uint8_t *buf, uint8_t conn_id, const uint8_t *data, uint16_t len);

/**
 * @brief Build an INFO request frame.
 * @param buf  Output buffer.
 * @return Frame length in bytes.
 */
uint16_t BLE_Eth_BuildInfoRequest(uint8_t *buf);

/**
 * @brief Build a response frame from status and optional payload.
 * @param buf      Output buffer.
 * @param cmd      Response command byte (e.g., BLE_ETH_CMD_CONNECT_RESP).
 * @param addr     Address field (copied from request).
 * @param status   Status byte.
 * @param payload  Additional payload (may be NULL).
 * @param plen     Payload length.
 * @return Frame length in bytes.
 */
uint16_t BLE_Eth_BuildResponse(uint8_t *buf, uint8_t cmd, uint8_t addr,
                               uint8_t status, const uint8_t *payload, uint8_t plen);

/**
 * @brief Get connection info by ID.
 * @param conn_id  Connection ID.
 * @return Pointer to connection info, or NULL if invalid.
 */
const ble_eth_conn_t *BLE_Eth_GetConnection(uint8_t conn_id);

/**
 * @brief Allocate a new connection slot.
 * @param remote_ip   Remote IP address.
 * @param remote_port Remote port.
 * @return Connection ID, or BLE_ETH_INVALID_CONN_ID if full.
 */
uint8_t BLE_Eth_AllocConnection(const uint8_t remote_ip[4], uint16_t remote_port);

/**
 * @brief Free a connection slot.
 * @param conn_id  Connection ID to release.
 */
void BLE_Eth_FreeConnection(uint8_t conn_id);

/**
 * @brief Process incoming tunneled data from the target (Ethernet/USB side).
 *        This fragments and queues data for BLE notification.
 * @param conn_id  Connection ID.
 * @param data     TCP payload data.
 * @param len      Data length.
 * @return true if data was accepted for sending.
 */
bool BLE_Eth_PushTargetData(uint8_t conn_id, const uint8_t *data, uint16_t len);

/**
 * @brief Get the next BLE notification chunk to send.
 *        Call repeatedly until it returns 0.
 * @param buf      Output buffer.
 * @param max_len  Maximum bytes that fit in the current BLE notification.
 * @return Number of bytes to send, 0 if nothing pending.
 */
uint16_t BLE_Eth_GetNextTxChunk(uint8_t *buf, uint16_t max_len);

/**
 * @brief Consume bytes returned by BLE_Eth_GetNextTxChunk after notify success.
 * @param len Number of bytes accepted by the BLE stack.
 */
void BLE_Eth_ConsumeTxChunk(uint16_t len);

/**
 * @brief Reset parser state (e.g., after connection loss).
 */
void BLE_Eth_ResetParser(void);

/**
 * @brief Set the ECM TX callback for sending Ethernet frames to the host.
 *        Called internally during initialization.
 */
void BLE_Eth_SetEcmtxCb(void (*cb)(const uint8_t *data, uint16_t len));

/**
 * @brief Send TCP data through an active tunnel, wrapped in Ethernet/IPv4/TCP.
 *        Only valid after TCP handshake is complete (ESTABLISHED state).
 */
void BLE_Eth_SendDataToEth(uint8_t conn_id, const uint8_t *data, uint16_t len);

/**
 * @brief Push data from bridge (target side) back to BLE host.
 *        Schedules data for BLE notification via the peripheral task.
 * @param conn_id  Connection ID (must match active tunnel).
 * @param data     TCP payload from target.
 * @param len      Data length.
 */
void BLE_Eth_PushFromBridge(uint8_t conn_id, const uint8_t *data, uint16_t len);

/**
 * @brief Parse an incoming Ethernet frame from the target (ECM bridge)
 *        and push TCP payload back to the BLE host through the matching tunnel.
 *        Matches frame source IP:port against active connection remote IP:port.
 * @param frame   Complete Ethernet frame (including Eth, IPv4, TCP headers).
 * @param len     Frame length in bytes.
 * @return true if a matching tunnel was found and data was pushed.
 */
bool BLE_Eth_HandleTargetFrame(const uint8_t *frame, uint16_t len);

/**
 * @brief Check if any BLE-Eth tunnels are currently active.
 */
bool BLE_Eth_HasActiveTunnel(void);

/**
 * @brief Attempt to auto-connect to the configured default target.
 *        Called from peripheral.c when BLE connection is established
 *        and ECM bridge is ready. Reads IP/port from flash config.
 * @return true if auto-connect initiated successfully
 */
bool BLE_Eth_AutoConnect(void);

/**
 * @brief Handle an incoming ARP reply and resolve pending connections.
 *        Called from ECM bridge when an ARP reply is detected.
 * @param sender_ip   IP address of the ARP reply sender (4 bytes).
 * @param sender_mac  MAC address of the ARP reply sender (6 bytes).
 * @return true if a pending connection was resolved.
 */
bool BLE_Eth_HandleArpReply(const uint8_t sender_ip[4], const uint8_t sender_mac[6]);

/**
 * @brief Check if any BLE-Eth connections exist (active or ARP-pending).
 *        Used to determine whether to poll ECM bridge during ARP resolution.
 */
bool BLE_Eth_HasActiveOrPendingConnection(void);

/**
 * @brief Learn host MAC from an incoming frame.
 *        Call from ECM bridge when receiving a frame from the host side.
 */
void BLE_Eth_LearnHostMac(const uint8_t *mac);

/**
 * @brief Periodic tick handler for ARP timeout processing.
 *        Call from periodic task (e.g., every 1 second).
 * @return true if a BLE response was queued and TX should be scheduled.
 */
bool BLE_Eth_PeriodicTick(void);

/**
 * @brief Read a snapshot of protocol reliability counters.
 */
void BLE_Eth_GetReliabilityStats(ble_eth_reliability_stats_t *out_stats);

/**
 * @brief Reset protocol reliability counters.
 */
void BLE_Eth_ResetReliabilityStats(void);

#else /* !USB_WORK_MODE_BRIDGE_ECM — stub out all BLE-Eth API as no-ops */

#define BLE_Eth_Init(cb)                            ((void)0)
#define BLE_Eth_Parse(data, len)                    ((void)0)
#define BLE_Eth_BuildConnect(buf, ip, port)         (0U)
#define BLE_Eth_BuildDisconnect(buf, id)            (0U)
#define BLE_Eth_BuildData(buf, id, data, len)       (0U)
#define BLE_Eth_BuildInfoRequest(buf)               (0U)
#define BLE_Eth_BuildResponse(buf, cmd, a, s, p, l) (0U)
#define BLE_Eth_GetConnection(id)                   ((const ble_eth_conn_t *)0)
#define BLE_Eth_AllocConnection(ip, port)           (BLE_ETH_INVALID_CONN_ID)
#define BLE_Eth_FreeConnection(id)                  ((void)0)
#define BLE_Eth_PushTargetData(id, data, len)       (false)
#define BLE_Eth_GetNextTxChunk(buf, max)            (0U)
#define BLE_Eth_ConsumeTxChunk(len)                 ((void)0)
#define BLE_Eth_ResetParser()                       ((void)0)
#define BLE_Eth_SetEcmtxCb(cb)                      ((void)0)
#define BLE_Eth_SendDataToEth(id, data, len)        ((void)0)
#define BLE_Eth_HandleTargetFrame(frame, len)       (false)
#define BLE_Eth_HasActiveTunnel()                   (false)
#define BLE_Eth_AutoConnect()                       (false)
#define BLE_Eth_HandleArpReply(ip, mac)             (false)
#define BLE_Eth_HasActiveOrPendingConnection()      (false)
#define BLE_Eth_LearnHostMac(mac)                   ((void)0)
#define BLE_Eth_PeriodicTick()                      (false)
#define BLE_Eth_GetReliabilityStats(out)            ((void)0)
#define BLE_Eth_ResetReliabilityStats()             ((void)0)

#endif /* USB_WORK_MODE_BRIDGE_ECM */

#ifdef __cplusplus
}
#endif

#endif /* __BLE_ETH_PROTOCOL_H */
