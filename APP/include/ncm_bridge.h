#ifndef NCM_BRIDGE_H
#define NCM_BRIDGE_H

#include <stdint.h>
#include "usb_work_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- NCM Protocol Constants --- */
#define NCM_NTB_MAX_SIZE          1280    /* Max NTB size (matches descriptor) */
#define NCM_NDP_MAX_DATAGRAMS     4       /* Max frames per NTB */
#define NCM_NTB_HEADER_SIZE       14      /* NTB header size */
#define NCM_NDP_HEADER_SIZE       8       /* NDP header (signature + length + next) */
#define NCM_NDP_ENTRY_SIZE        4       /* Index + length per datagram entry */

/* NCM NDP signature */
#define NCM_NDP_SIGNATURE_16      0x304D434E  /* 'NCM0' in little-endian */

/* --- Bridge functions (only compiled when bridge NCM is active) ---------- */
#if USB_WORK_MODE_COMPILES_BRIDGE_NCM
void NCM_Bridge_Init(void);
void NCM_Bridge_InitTarget(void);      /* Target-facing USBFS (re-)enumeration only */
void NCM_Bridge_UpdateIPConfig(void);  /* Update IP config immediately after modification */
void NCM_Bridge_HostRxDone(const uint8_t *data, uint16_t len);
void NCM_Bridge_TargetRxDone(const uint8_t *data, uint16_t len);
void NCM_Bridge_Poll(void);
void NCM_Bridge_SetBleTxCallback(void (*cb)(void));
uint8_t NCM_Bridge_SendRawFrame(const uint8_t *data, uint16_t len);
void NCM_Bridge_Target_RearmLinkNotify(void);

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
#define NCM_Bridge_HostRxDone(data, len)   ((void)0)
#define NCM_Bridge_TargetRxDone(data, len) ((void)0)
#define NCM_Bridge_Poll()                  ((void)0)
#define NCM_Bridge_SetBleTxCallback(cb)    ((void)0)
#define NCM_Bridge_SendRawFrame(data, len) ((uint8_t)0)
#define NCM_Bridge_Target_RearmLinkNotify() ((void)0)
#endif

/* --- Diagnostic variables (always defined) ------------------------------- */
extern volatile uint8_t g_host_ncm_data_if_active;
extern volatile uint16_t g_ncm_tx_count;
extern volatile uint8_t g_ncm_garp_sent;
extern volatile uint8_t g_ncm_ntb_sequence;

#ifdef __cplusplus
}
#endif

#endif /* NCM_BRIDGE_H */
