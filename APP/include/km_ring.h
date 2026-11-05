#ifndef KM_RING_H
#define KM_RING_H

#include <stdint.h>
#include <stdbool.h>
#include "Version_selection.h"

/* Queue size definitions */
#define KB_QUEUE_SIZE      512    // Keyboard report queue size
#define MS_REL_QUEUE_SIZE  64     // Relative mouse report queue size (increased from 28 to handle bursts)
#define MS_ABS_QUEUE_SIZE  64     // Absolute mouse report queue size (increased from 28 to handle bursts)
#define RESP_QUEUE_SIZE    28     // Response packet queue size
#define MEDIA_QUEUE_SIZE   16     // Media key report queue size

/* Keyboard report structure */
typedef struct {
    uint8_t data[8];
} KB_Report_t;

/* Mouse relative movement report */
typedef struct {
    uint8_t data[4];
} MS_Rel_Report_t;

/* Mouse absolute movement report
 * Size is determined at compile time by EP3_USE_STYLUS:
 *   EP3_USE_STYLUS not defined  →  6 bytes  (abs mouse)
 *   EP3_USE_STYLUS defined      →  5 bytes  (stylus)
 */
#ifdef EP3_USE_STYLUS
  #define MS_ABS_DATA_LEN  5
#else
  #define MS_ABS_DATA_LEN  6
#endif

typedef struct {
    uint8_t data[MS_ABS_DATA_LEN];
} MS_Abs_Report_t;

/* Response packet structure */
typedef struct {
    uint8_t data[64];   // Maximum 64 bytes per response frame
    uint8_t len;        // Actual valid length
} Resp_Report_t;

/* Media key report structure
 * data[0] = Report ID (0x01=ACPI, 0x02=Consumer, 0x03=Apple function row)
 * data[1..3] = payload (1 byte for ACPI and Apple row, 3 bytes for Consumer)
 */
typedef struct {
    uint8_t data[4];
} Media_Report_t;

/* Media key queue structure */
typedef struct {
    Media_Report_t buf[MEDIA_QUEUE_SIZE];
    volatile int head;
    volatile int tail;
} Media_Queue_t;

/* Keyboard queue structure */
typedef struct {
    KB_Report_t buf[KB_QUEUE_SIZE];
    volatile int head;
    volatile int tail;
} KB_Queue_t;

/* Mouse relative queue structure */
typedef struct {
    MS_Rel_Report_t buf[MS_REL_QUEUE_SIZE];
    volatile int head;
    volatile int tail;
} MS_Rel_Queue_t;

/* Mouse absolute queue structure */
typedef struct {
    MS_Abs_Report_t buf[MS_ABS_QUEUE_SIZE];
    volatile int head;
    volatile int tail;
} MS_Abs_Queue_t;

/* Response packet queue structure */
typedef struct {
    Resp_Report_t buf[RESP_QUEUE_SIZE];
    volatile int head;
    volatile int tail;
} Resp_Queue_t;

/* Global queue declarations */
extern KB_Queue_t kb_queue;
extern MS_Rel_Queue_t ms_rel_queue;
extern MS_Abs_Queue_t ms_abs_queue;
extern Resp_Queue_t resp_queue;
extern Media_Queue_t media_queue;

/* Queue initialization */
void Queue_Init(void);

/* Push functions */
void Queue_Push_Keyboard(uint8_t *data);
void Queue_Push_MouseRel(uint8_t *data);
void Queue_Push_MouseAbs(uint8_t *data);
void Queue_Push_Response(uint8_t *data, uint8_t len);
void Queue_Push_Media(uint8_t *data);

/* Pop functions */
bool Queue_Pop_Keyboard(KB_Report_t *out);
bool Queue_Pop_MouseRel(MS_Rel_Report_t *out);
bool Queue_Pop_MouseAbs(MS_Abs_Report_t *out);
bool Queue_Pop_Response(Resp_Report_t *out);
bool Queue_Pop_Media(Media_Report_t *out);

/* Send functions */
void USB_SendFromQueues(void);        // Send keyboard/mouse reports via USBFS
void USBD_SendFromRespQueue(void);    // Send response packets via USBD (endpoint)

/* USB debug ring buffer — sends debug strings via USBFS EP2 (relative mouse endpoint).
 * Debug reports use mouse HID format with a 0xFF marker byte in data[0].
 * Monitor on macOS with a raw HID reader (e.g. pyhidapi). */
#define USB_DEBUG_RING_SIZE 256
void USB_DEBUG(const char *fmt, ...);
void USB_DEBUG_Poll(void);

#endif /* KM_RING_H */
