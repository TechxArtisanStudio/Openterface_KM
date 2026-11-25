/********************************** (C) COPYRIGHT *******************************
 * File Name          : hid_launcher.c
 * Description        : Types a Host-provided URL into the target PC via the
 *                      USBFS boot-keyboard interface (Win+R -> URL -> Enter).
 *                      No page content is stored in firmware.
 *******************************************************************************/
#include "include/hid_launcher.h"
#include "include/usb_mode_config.h"

#if USB_WORK_MODE_COMPILES_BRIDGE_NCM

#include "include/keyboard_handler.h"
#include "include/km_ring.h"
#include <string.h>

extern volatile uint32_t systick_ms;

#define HID_STEP_MS       40u
#define HID_RUN_DELAY_MS  500u

typedef enum {
    ST_IDLE = 0,
    ST_WIN_DOWN,
    ST_WIN_UP,
    ST_R_DOWN,
    ST_R_UP,
    ST_WAIT_RUN,
    ST_CHAR_DOWN,
    ST_CHAR_UP,
    ST_ENTER_DOWN,
    ST_ENTER_UP,
    ST_DONE,
} launch_state_t;

static uint8_t  s_url[HID_URL_MAX];
static uint8_t  s_url_len;
static uint8_t  s_enabled;
static launch_state_t s_state;
static uint8_t  s_char_idx;
static uint32_t s_next_ms;
static uint8_t  s_triggers;

static uint8_t ascii_to_hid(uint8_t ch)
{
    if (ch >= 'a' && ch <= 'z') return (uint8_t)(0x04u + (ch - 'a'));
    if (ch >= 'A' && ch <= 'Z') return (uint8_t)(0x04u + (ch - 'A'));
    if (ch >= '1' && ch <= '9') return (uint8_t)(0x1Eu + (ch - '1'));
    if (ch == '0') return 0x27u;
    switch (ch) {
    case ' ': return 0x2Cu;
    case '.': return 0x37u;
    case '/': return 0x38u;
    case ':': return 0x33u;
    case '-': return 0x2Du;
    case '_': return 0x2Du;
    case '?': return 0x38u;
    case '=': return 0x2Eu;
    default:  return 0x00u;
    }
}

static bool needs_shift(uint8_t ch)
{
    return (ch == ':' || ch == '_' || ch == '?' ||
            (ch >= 'A' && ch <= 'Z'));
}

static void send_report(uint8_t mod, uint8_t key)
{
    uint8_t rpt[8] = { mod, 0, key, 0, 0, 0, 0, 0 };
    Keyboard_SendDataToUSB(rpt);
}

static void go(launch_state_t next, uint16_t delay_ms)
{
    s_state = next;
    s_next_ms = (uint32_t)(systick_ms + delay_ms);
}

void HIDLauncher_Init(void)
{
    s_url_len = 0;
    s_enabled = 0;
    s_state = ST_IDLE;
    s_char_idx = 0;
    s_next_ms = 0;
    s_triggers = 0;
}

uint8_t HIDLauncher_SetUrl(const uint8_t *url, uint8_t len)
{
    if (!url || !len || len > HID_URL_MAX) return 0xE5u;
    memcpy(s_url, url, len);
    s_url_len = len;
    return 0;
}

uint8_t HIDLauncher_SetEnabled(uint8_t enabled)
{
    if (enabled && s_url_len) {
        s_enabled = 1;
        s_char_idx = 0;
        s_state = ST_WIN_DOWN;
        s_next_ms = systick_ms;
    } else {
        s_enabled = 0;
        s_state = ST_IDLE;
    }
    return 0;
}

void HIDLauncher_GetStatus(uint8_t out[4])
{
    out[0] = s_enabled;
    out[1] = s_url_len;
    out[2] = (uint8_t)s_state;
    out[3] = s_triggers;
}

void HIDLauncher_Poll(void)
{
    if (s_state == ST_IDLE || s_state == ST_DONE) return;
    if ((int32_t)(systick_ms - s_next_ms) < 0) return;

    switch (s_state) {
    case ST_WIN_DOWN:
        send_report(MOD_LGUI, 0);
        go(ST_WIN_UP, HID_STEP_MS);
        break;
    case ST_WIN_UP:
        send_report(0, 0);
        go(ST_R_DOWN, HID_STEP_MS);
        break;
    case ST_R_DOWN:
        send_report(0, 0x15);  /* R */
        go(ST_R_UP, HID_STEP_MS);
        break;
    case ST_R_UP:
        send_report(0, 0);
        go(ST_WAIT_RUN, HID_RUN_DELAY_MS);
        break;
    case ST_WAIT_RUN:
        s_char_idx = 0;
        go(ST_CHAR_DOWN, HID_STEP_MS);
        break;
    case ST_CHAR_DOWN: {
        if (s_char_idx >= s_url_len) { go(ST_ENTER_DOWN, HID_STEP_MS); break; }
        uint8_t ch = s_url[s_char_idx];
        uint8_t key = ascii_to_hid(ch);
        if (key) send_report(needs_shift(ch) ? MOD_LSHIFT : 0, key);
        go(ST_CHAR_UP, HID_STEP_MS);
        break;
    }
    case ST_CHAR_UP:
        send_report(0, 0);
        s_char_idx++;
        go(ST_CHAR_DOWN, HID_STEP_MS);
        break;
    case ST_ENTER_DOWN:
        send_report(0, 0x28);  /* Enter */
        go(ST_ENTER_UP, HID_STEP_MS);
        break;
    case ST_ENTER_UP:
        send_report(0, 0);
        s_triggers++;
        go(ST_DONE, 0);
        break;
    default:
        s_state = ST_IDLE;
        break;
    }
}

#endif /* USB_WORK_MODE_COMPILES_BRIDGE_NCM */
