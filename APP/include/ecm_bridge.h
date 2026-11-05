#ifndef ECM_BRIDGE_H
#define ECM_BRIDGE_H

#include <stdint.h>
#include "usb_work_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Bridge functions (only compiled when bridge ECM is active) ---------- */
#if USB_WORK_MODE_COMPILES_BRIDGE_ECM
void ECM_Bridge_Init(void);
void ECM_Bridge_InitTarget(void);      /* Target-facing USBFS (re-)enumeration only */
void ECM_Bridge_UpdateIPConfig(void);  /* Update IP config immediately after modification */
void ECM_Bridge_HostRxDone(const uint8_t *data, uint16_t len);
void ECM_Bridge_TargetRxDone(const uint8_t *data, uint16_t len);
void ECM_Bridge_Poll(void);
void ECM_Bridge_SetBleTxCallback(void (*cb)(void));
uint8_t ECM_Bridge_SendRawFrame(const uint8_t *data, uint16_t len);
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
void ECM_Bridge_Target_RearmLinkNotify(void);
#else
#define ECM_Bridge_Target_RearmLinkNotify() ((void)0)
#endif
#else
#define ECM_Bridge_Init()                  ((void)0)
#define ECM_Bridge_InitTarget()            ((void)0)
#define ECM_Bridge_UpdateIPConfig()        ((void)0)
#define ECM_Bridge_HostRxDone(data, len)   ((void)0)
#define ECM_Bridge_TargetRxDone(data, len) ((void)0)
#define ECM_Bridge_Poll()                  ((void)0)
#define ECM_Bridge_SetBleTxCallback(cb)    ((void)0)
#define ECM_Bridge_SendRawFrame(data, len) ((uint8_t)0)
#define ECM_Bridge_Target_RearmLinkNotify() ((void)0)
#endif

/* --- Diagnostic / debug variables (always defined) -----------------------
 * These variables are defined in ecm_bridge.c (outside #if guard) or in
 * ch32v20x_usbfs_device.c.  Always declare them so other translation units
 * can reference them without needing #ifdef guards.                          */

/* Set when USBD CDC ECM data interface (IF1, alt=1) is activated. */
extern volatile uint8_t g_host_ecm_data_if_active;
/* Set when USBFS CDC ECM data interface (IF5, alt=1) is activated. */
extern volatile uint8_t g_target_ecm_data_if_active;
/* Incremented on each successful TX. */
extern volatile uint16_t g_ecm_tx_count;
/* Reset in USBD SET_INTERFACE handler so GARP is re-queued on every activation. */
extern volatile uint8_t g_host_garp_sent;
/* Reset in USBFS SET_INTERFACE handler so GARP is re-queued on every activation. */
extern volatile uint8_t g_target_garp_sent;
/* Diagnostic: 0=idle, 1=GARP queued, 2=DataUp called & SUCCESS,
 *             3=EP3 busy, 4=DataUp FAILED */
extern volatile uint8_t g_ecm_tx_state;
/* LED override: 0=normal, 1=LED solid ON, 2=LED solid OFF.
   Set by HostRxDone (1 on data received) and handle_arp_frame (1 on ARP detected). */
extern volatile uint8_t g_ecm_led_override;
/* DHCP diagnostic LED (highest priority, separate from g_ecm_led_override).
   0=no DHCP activity, 9=host_control entered, 10=port 67 hit,
   11=msg type parsed, 12=ACK queued. */
extern volatile uint8_t g_dhcp_led_mode;
/* Flag indicating ECM bridge is currently processing a CH9329 command via TCP frame.
 * Set to 1 when about to call CH9329_RxBuffer_Add, cleared after response is handled.
 * Used to prevent race conditions with USB queue processing of CH9329 responses. */
extern volatile uint8_t g_ecm_bridge_processing_ch9329;

/* Target-side re-enumeration / DHCP diagnostics. These make it possible to
 * tell why a target IP change did not take effect without a replug:
 *   - g_target_detach_dp_level: D+ level sampled at the end of the soft
 *     detach window (0 = line low, target saw a disconnect; 1 = still high,
 *     the pull-up is not software controlled on this board).
 *   - g_target_bus_reset_count: USBFS bus resets, i.e. how often the target
 *     host actually re-enumerated us.
 *   - g_target_dhcp_count / _nak_count: DHCP replies from the target scope. */
extern volatile uint8_t  g_target_detach_dp_level;
extern volatile uint16_t g_target_reenum_count;
extern volatile uint16_t g_target_bus_reset_count;
extern volatile uint16_t g_target_dhcp_count;
extern volatile uint16_t g_target_dhcp_nak_count;

/* EP6 OUT diagnostic — defined in ch32v20x_usbfs_device.c */
extern volatile uint8_t  g_usbfs_ep6_int_flag;  /* 1 = EP6 OUT interrupt fired */
extern volatile uint8_t  g_usbfs_ep6_tog_ok;    /* last TOG_OK result */
extern volatile uint16_t g_usbfs_ep6_rx_len;    /* last RX length */
extern volatile uint32_t g_usbfs_ep6_rx_count;  /* total EP6 OUT interrupts (any TOG) */
extern volatile uint16_t g_usbfs_ep7_in_count;  /* total EP7 IN completions */
extern volatile uint16_t g_usbfs_ep5_in_count;  /* total EP5 IN (notification) completions */

/* Target-side CDC ECM link notification state machine.
 *   g_target_notify_state: 0=idle, 1=link down pending, 2=link up pending,
 *                          3=speed change pending. A host that is not polling
 *   the notification endpoint latches the pending packet, so a non-zero state
 *   with g_target_notify_tx_count stuck means the host never read it.
 *   g_target_notify_tx_count: notifications handed to EP5. */
extern volatile uint8_t  g_target_notify_state;
extern volatile uint16_t g_target_notify_tx_count;

#ifdef __cplusplus
}
#endif

#endif /* ECM_BRIDGE_H */
