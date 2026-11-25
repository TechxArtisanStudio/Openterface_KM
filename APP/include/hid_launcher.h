#ifndef __HID_LAUNCHER_H
#define __HID_LAUNCHER_H

#include <stdint.h>

/* HID browser launcher: types Win+R, a Host-provided URL, Enter into the
 * target PC's boot-keyboard interface. The page itself is served by the
 * Host web service over the NCM L2 bridge; no HTML lives in firmware. */
#define HID_URL_MAX 96u

void HIDLauncher_Init(void);
void HIDLauncher_Poll(void);
/* Configure the URL and arm the launcher (empty url disarms). */
uint8_t HIDLauncher_SetUrl(const uint8_t *url, uint8_t len);
/* Arm/disarm the typing sequence without changing the URL. */
uint8_t HIDLauncher_SetEnabled(uint8_t enabled);
void HIDLauncher_GetStatus(uint8_t out[4]);  /* [enabled, url_len, state, trig] */

#endif
