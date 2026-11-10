/*******************************************************************************
 * KeyMod factory-reset button.
 *
 * The BOOT button reaches the MCU on PB8. On the QFN28 CH32V208GBU6 that pad is
 * shared with BOOT0, so the pin is read as a floating input: an internal
 * pull-up would fight the external pull-down that keeps the chip booting from
 * flash.
 *******************************************************************************/

#ifndef BUTTON_H
#define BUTTON_H

#include "ch32v20x.h"
#include "Version_selection.h"

#if defined(ENABLE_BLE) && version == 3

#define BUTTON_GPIO_PORT    GPIOB
#define BUTTON_GPIO_PIN     GPIO_Pin_8
#define BUTTON_HOLD_MS      5000u
#define BUTTON_DEBOUNCE_MS  20u

/* Button_LedLevel() value meaning "the button does not own the LED". */
#define BUTTON_LED_NONE     0xFFu

void Button_Init(void);

/* Non-blocking; call once per main-loop iteration. */
void Button_Poll(void);

/* 0/1 = LED brightness the button wants while prompting, BUTTON_LED_NONE
 * when the normal LED logic should run. */
uint8_t Button_LedLevel(void);

#endif /* ENABLE_BLE && version == 3 */

#endif
