#include "km_ring.h"
#include "ch32v20x_usbfs_device.h"
#include "usb_lib.h"
#include "usb_desc.h"
#include "usb_prop.h"
#include "usb_mode_config.h"

#include <string.h>
#include <stdarg.h>


/* Global queues */
KB_Queue_t kb_queue;
MS_Rel_Queue_t ms_rel_queue;
MS_Abs_Queue_t ms_abs_queue;
Resp_Queue_t resp_queue;
Media_Queue_t media_queue;

/* Extern variables */
extern uint8_t USBD_Endp3_Busy;

/* Initialize queues */
void Queue_Init(void) {
    kb_queue.head = kb_queue.tail = 0;
    ms_rel_queue.head = ms_rel_queue.tail = 0;
    ms_abs_queue.head = ms_abs_queue.tail = 0;
    resp_queue.head = resp_queue.tail = 0;
    media_queue.head = media_queue.tail = 0;
}
//Keyboard message queue
void Queue_Push_Keyboard(uint8_t *data) {
    int next = (kb_queue.tail + 1) % KB_QUEUE_SIZE;
    if (next == kb_queue.head) kb_queue.head = (kb_queue.head + 1) % KB_QUEUE_SIZE;
    memcpy(kb_queue.buf[kb_queue.tail].data, data, sizeof(kb_queue.buf[kb_queue.tail].data));
    kb_queue.tail = next;
}
//MouseRel message queue
void Queue_Push_MouseRel(uint8_t *data) {
    int next = (ms_rel_queue.tail + 1) % MS_REL_QUEUE_SIZE;
    if (next == ms_rel_queue.head) ms_rel_queue.head = (ms_rel_queue.head + 1) % MS_REL_QUEUE_SIZE;
    memcpy(ms_rel_queue.buf[ms_rel_queue.tail].data, data, sizeof(ms_rel_queue.buf[ms_rel_queue.tail].data));
    ms_rel_queue.tail = next;
}
//MouseAbs message queue
void Queue_Push_MouseAbs(uint8_t *data) {
    int next = (ms_abs_queue.tail + 1) % MS_ABS_QUEUE_SIZE;
    if (next == ms_abs_queue.head) ms_abs_queue.head = (ms_abs_queue.head + 1) % MS_ABS_QUEUE_SIZE;
    memcpy(ms_abs_queue.buf[ms_abs_queue.tail].data, data, sizeof(ms_abs_queue.buf[ms_abs_queue.tail].data));
    ms_abs_queue.tail = next;
}
//Response message queue (Message length varies)
void Queue_Push_Response(uint8_t *data, uint8_t len) {
    int next = (resp_queue.tail + 1) % RESP_QUEUE_SIZE;
    if (next == resp_queue.head) resp_queue.head = (resp_queue.head + 1) % RESP_QUEUE_SIZE;
    memcpy(resp_queue.buf[resp_queue.tail].data, data, len);
    resp_queue.buf[resp_queue.tail].len = len;
    resp_queue.tail = next;
}
//Media key message queue
void Queue_Push_Media(uint8_t *data) {
    int next = (media_queue.tail + 1) % MEDIA_QUEUE_SIZE;
    if (next == media_queue.head) media_queue.head = (media_queue.head + 1) % MEDIA_QUEUE_SIZE;
    memcpy(media_queue.buf[media_queue.tail].data, data, sizeof(media_queue.buf[media_queue.tail].data));
    media_queue.tail = next;
}

/* Pop functions */
bool Queue_Pop_Keyboard(KB_Report_t *out) {
    if (kb_queue.head == kb_queue.tail) return false;
    memcpy(out->data, kb_queue.buf[kb_queue.head].data, sizeof(out->data));
    kb_queue.head = (kb_queue.head + 1) % KB_QUEUE_SIZE;
    return true;
}

bool Queue_Pop_MouseRel(MS_Rel_Report_t *out) {
    if (ms_rel_queue.head == ms_rel_queue.tail) return false;
    memcpy(out->data, ms_rel_queue.buf[ms_rel_queue.head].data, sizeof(out->data));
    ms_rel_queue.head = (ms_rel_queue.head + 1) % MS_REL_QUEUE_SIZE;
    return true;
}

bool Queue_Pop_MouseAbs(MS_Abs_Report_t *out) {
    if (ms_abs_queue.head == ms_abs_queue.tail) return false;
    memcpy(out->data, ms_abs_queue.buf[ms_abs_queue.head].data, sizeof(out->data));
    ms_abs_queue.head = (ms_abs_queue.head + 1) % MS_ABS_QUEUE_SIZE;
    return true;
}

bool Queue_Pop_Response(Resp_Report_t *out) {
    if (resp_queue.head == resp_queue.tail) return false;
    memcpy(out->data, resp_queue.buf[resp_queue.head].data, resp_queue.buf[resp_queue.head].len);
    out->len = resp_queue.buf[resp_queue.head].len;
    resp_queue.head = (resp_queue.head + 1) % RESP_QUEUE_SIZE;
    return true;
}

bool Queue_Pop_Media(Media_Report_t *out) {
    if (media_queue.head == media_queue.tail) return false;
    memcpy(out->data, media_queue.buf[media_queue.head].data, sizeof(out->data));
    media_queue.head = (media_queue.head + 1) % MEDIA_QUEUE_SIZE;
    return true;
}

/* Try to send all queued packets until the endpoint is busy or queue is empty */
void USB_SendFromQueues(void) {
    /* The target side exposes four independent HID interfaces, each with its
     * own endpoint and no Report IDs: EP1 keyboard, EP2 relative mouse,
     * EP3 absolute mouse/stylus, EP4 media keys. Reports must be sent to
     * their own endpoint verbatim — prefixing a Report ID breaks the host
     * parser because the descriptors declare none. */

    while (kb_queue.head != kb_queue.tail && USBFS_Endp_Busy[DEF_UEP1] == 0) {
        KB_Report_t *pkt = &kb_queue.buf[kb_queue.head];
        if (USBFS_Endp_DataUp(DEF_UEP1, pkt->data, sizeof(pkt->data), DEF_UEP_CPY_LOAD) == READY)
            kb_queue.head = (kb_queue.head + 1) % KB_QUEUE_SIZE;
        else break;
    }

    while (ms_rel_queue.head != ms_rel_queue.tail && USBFS_Endp_Busy[DEF_UEP2] == 0) {
        MS_Rel_Report_t *pkt = &ms_rel_queue.buf[ms_rel_queue.head];
        if (USBFS_Endp_DataUp(DEF_UEP2, pkt->data, sizeof(pkt->data), DEF_UEP_CPY_LOAD) == READY)
            ms_rel_queue.head = (ms_rel_queue.head + 1) % MS_REL_QUEUE_SIZE;
        else break;
    }

    while (ms_abs_queue.head != ms_abs_queue.tail && USBFS_Endp_Busy[DEF_UEP3] == 0) {
        MS_Abs_Report_t *pkt = &ms_abs_queue.buf[ms_abs_queue.head];
        if (USBFS_Endp_DataUp(DEF_UEP3, pkt->data, sizeof(pkt->data), DEF_UEP_CPY_LOAD) == READY)
            ms_abs_queue.head = (ms_abs_queue.head + 1) % MS_ABS_QUEUE_SIZE;
        else break;
    }

    while (media_queue.head != media_queue.tail && USBFS_Endp_Busy[DEF_UEP4] == 0) {
        Media_Report_t *pkt = &media_queue.buf[media_queue.head];
        /* Report ID 0x01 = ACPI: 2 bytes (ID + 1 data)
         * Report ID 0x02 = Consumer: 4 bytes (ID + 3 data) */
        uint16_t send_len = (pkt->data[0] == 0x01) ? 2 : 4;
        if (USBFS_Endp_DataUp(DEF_UEP4, pkt->data, send_len, DEF_UEP_CPY_LOAD) == READY)
            media_queue.head = (media_queue.head + 1) % MEDIA_QUEUE_SIZE;
        else break;
    }

    /* Response queue drain — send CH9329 responses via EP3 (ACM bulk IN).
     *
     * In all modes with host CDC ACM (including ACM+ECM combined mode),
     * responses must be sent via EP3. The ECM bridge has its own separate
     * response handling for TCP-based CH9329 commands, but USB serial
     * commands still need this path.
     *
     * IMPORTANT: CH9329 responses go through the USBD library (USB 1.1 FS
     * controller at 0x40005C00), NOT the USBFS library (USB 2.0 OTG at
     * 0x50000000). The USBD library handles CDC ACM, while USBFS handles
     * HID endpoints. Both controllers share the same physical USB port
     * via a multiplexer.
     */
    if (USB_ModeConfig_ActiveModeHasHostCdcAcm() ||
        (!USB_ModeConfig_ActiveModeBridgeEcm() && !USB_ModeConfig_ActiveModeHasHostCdcEcm())) {

        /* Check if EP3 IN endpoint is enabled */
        uint16_t ep3_status = _GetEPTxStatus(ENDP3);
        if (ep3_status == EP_TX_DIS) {
            /* EP3 not enabled, try to enable it */
            if (USB_ModeConfig_ActiveModeHasHostCdcAcm()) {
                _ClearDTOG_TX(ENDP3);
                SetEPTxStatus(ENDP3, EP_TX_NAK);
            }
            /* Don't return - continue to process the queue after enabling */
        }

        while (resp_queue.head != resp_queue.tail) {
            Resp_Report_t *pkt = &resp_queue.buf[resp_queue.head];

            /* Check if endpoint is busy */
            if (USBD_Endp3_Busy) {
                break;  /* endpoint busy, retry next pass */
            }

            int result = USBD_ENDPx_DataUp(ENDP3, pkt->data, pkt->len);
            if (result == 0) {
                resp_queue.head = (resp_queue.head + 1) % RESP_QUEUE_SIZE;
            }
            else break;  /* endpoint busy, retry next pass */
        }
    }
}

/* ====== Debug ring buffer (disabled — EP2 no longer available for HID) ====== */

/* Debug ring buffer is disabled because EP2 is now used for CDC endpoints.
 * To re-enable debug output, either:
 *   1. Add a vendor-defined Report ID (e.g., 0xFF) to the combined HID report descriptor
 *      and send debug reports via EP1 with that Report ID.
 *   2. Use a separate debug interface/endpoint.
 * For now, USB_DEBUG() and USB_DEBUG_Poll() are no-ops.
 */

static struct {
    char buf[USB_DEBUG_RING_SIZE];
    volatile uint16_t head;
    volatile uint16_t tail;
} g_debug_ring;

/* Marker byte in data[1] distinguishes debug reports from real media keys.
 * Format: {0x02, 0xFE, char, char} — consumer HID report, 2 chars per packet. */
#define USB_DEBUG_MARKER  0xFE

/* Called from USB_DEBUG() — adds string to ring buffer (caller holds lock). */
static void debug_ring_push_str(const char *s) {
    (void)s;  // Unused - debug disabled
}

#define USB_DEBUG_MOUSE_MARKER  0xFF

void USB_DEBUG(const char *fmt, ...) {
    (void)fmt;  // Unused - debug disabled
    // va_list ap; ... original implementation disabled
}

void USB_DEBUG_Poll(void) {
    // Debug output disabled - EP2 no longer available for HID
    // Original implementation sent via EP2 using mouse report format
}
