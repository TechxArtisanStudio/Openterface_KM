#ifndef NCM_BRIDGE_H
#define NCM_BRIDGE_H

#include <stdint.h>
#include "usb_work_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- NCM Protocol Constants --- */
#define NCM_NTB_MAX_SIZE          2048    /* Max NTB size - must match Linux cdc_ncm minimum (2048 for FS) */
#define NCM_NDP_MAX_DATAGRAMS     4       /* Max frames per NTB */
#define NCM_NTB_HEADER_SIZE       12      /* NTB header size (NTB-16 format) */
#define NCM_NDP_HEADER_SIZE       8       /* NDP header (signature + length + next) */
#define NCM_NDP_ENTRY_SIZE        4       /* Index + length per datagram entry */

/* NCM NDP signature */
#define NCM_NDP_SIGNATURE_16      0x304D434E  /* 'NCM0' in little-endian */

/* --- Bridge functions (only compiled when bridge NCM is active) ---------- */
#if USB_WORK_MODE_COMPILES_BRIDGE_NCM
void NCM_Bridge_Init(void);
void NCM_Bridge_InitTarget(void);      /* Target-facing USBFS (re-)enumeration only */
void NCM_Bridge_UpdateIPConfig(void);  /* Update IP config immediately after modification */
void NCM_Bridge_HostLinkReset(void);   /* Sync SW TX busy flags after control-transfer endpoint resets */
void NCM_Bridge_HostRxDone(const uint8_t *data, uint16_t len);
void NCM_Bridge_TargetRxDone(const uint8_t *data, uint16_t len);
void NCM_Bridge_Poll(void);
void NCM_Bridge_SetBleTxCallback(void (*cb)(void));
uint8_t NCM_Bridge_SendRawFrame(const uint8_t *data, uint16_t len);
void NCM_Bridge_Target_RearmLinkNotify(void);
void NCM_Bridge_Target_SetDataAlt(uint8_t alt);  /* Reset RX state on SET_INTERFACE */
void NCM_Bridge_Target_NotificationPoll(void);  /* USBFS-side link notification poll */

/* 0x33/0x10 diagnostics: host-side NCM bridge memory readout (no printf).
 * Consumed by the CMD_ISP_DEBUG dispatcher in usbd_compostie_km.c. */
uint16_t NCM_Bridge_GetHostDiagnostics(uint8_t *out, uint16_t max_len);

/* 0x33/0x13 diagnostics: compact target-RX / bridge readout (20 bytes). */
uint16_t NCM_Bridge_GetBridgeDiagnostics(uint8_t *out, uint16_t max_len);

/* NTB builder/parser functions (exposed for testing) */
uint16_t NCM_BuildNtb(uint8_t *ntb_buf, uint16_t max_size,
                       const uint8_t **frames, const uint16_t *frame_lens,
                       uint8_t frame_count, uint16_t sequence);
uint8_t NCM_ParseNtb(const uint8_t *ntb_buf, uint16_t ntb_len,
                     uint8_t (*frame_callback)(const uint8_t *frame, uint16_t len));
#else
#define NCM_Bridge_Init()                  ((void)0)
#define NCM_Bridge_InitTarget()            ((void)0)
#define NCM_Bridge_UpdateIPConfig()        ((void)0)
#define NCM_Bridge_HostLinkReset()         ((void)0)
#define NCM_Bridge_HostRxDone(data, len)   ((void)0)
#define NCM_Bridge_TargetRxDone(data, len) ((void)0)
#define NCM_Bridge_Poll()                  ((void)0)
#define NCM_Bridge_SetBleTxCallback(cb)    ((void)0)
#define NCM_Bridge_SendRawFrame(data, len) ((uint8_t)0)
#define NCM_Bridge_Target_RearmLinkNotify() ((void)0)
#define NCM_Bridge_Target_SetDataAlt(alt)   ((void)0)
#define NCM_Bridge_Target_NotificationPoll() ((void)0)
#define NCM_Bridge_GetHostDiagnostics(out, max) ((uint16_t)0)
#define NCM_Bridge_GetBridgeDiagnostics(out, max) ((uint16_t)0)
#endif

/* --- Diagnostic variables (always defined) ------------------------------- */
extern volatile uint8_t g_host_ncm_data_if_active;
extern volatile uint16_t g_ncm_tx_count;
extern volatile uint8_t g_ncm_garp_sent;
extern volatile uint8_t g_ncm_ntb_sequence;

/* Target-side NCM link notification state machine */
extern volatile uint8_t  g_target_ncm_notify_state;
extern volatile uint16_t g_target_ncm_notify_tx_count;
extern volatile uint8_t  g_target_ncm_data_if_active;

/* Target-side NCM TX counters */
extern volatile uint16_t g_ncm_target_tx_count;

#ifdef __cplusplus
}
#endif

#endif /* NCM_BRIDGE_H */
