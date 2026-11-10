/*******************************************************************************
 * KeyMod factory-reset button: hold the BOOT button for 5 s.
 *
 * Sequence: press -> 20 ms debounce -> 5 s hold -> LED fast-blinks to ask for
 * release -> release (20 ms debounce) -> factory reset -> reboot 500 ms later.
 * Releasing before 5 s cancels silently. The reboot is deliberately delayed
 * until release: while the button is held BOOT0 is high, and resetting then
 * would enter the ROM ISP instead of the firmware.
 *******************************************************************************/

#include "include/button.h"

#if defined(ENABLE_BLE) && version == 3

#include "include/peripheral.h"
#include "ch32v20x_gpio.h"
#include "ch32v20x_rcc.h"

extern volatile uint32_t systick_ms;

enum {
    BTN_IDLE = 0,
    BTN_BOUNCE_DOWN,
    BTN_HOLD,
    BTN_PROMPT,
    BTN_BOUNCE_UP
};

static uint8_t s_state = BTN_IDLE;
static uint32_t s_mark;
/* Idle level must be observed once before a hold can start, so a pin that
 * reads high at boot (wrong net, missing pull-down) cannot fake a press. */
static uint8_t s_armed;

static uint8_t Button_IsDown(void)
{
    return GPIO_ReadInputDataBit(BUTTON_GPIO_PORT, BUTTON_GPIO_PIN) == Bit_SET;
}

void Button_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    gpio.GPIO_Pin = BUTTON_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(BUTTON_GPIO_PORT, &gpio);

    s_state = BTN_IDLE;
    s_mark = systick_ms;
    s_armed = Button_IsDown() ? 0u : 1u;
}

void Button_Poll(void)
{
    uint32_t now = systick_ms;

    switch (s_state) {
    case BTN_IDLE:
        if (!Button_IsDown()) {
            s_armed = 1;
        } else if (s_armed) {
            s_state = BTN_BOUNCE_DOWN;
            s_mark = now;
        }
        break;

    case BTN_BOUNCE_DOWN:
        if (!Button_IsDown()) {
            s_state = BTN_IDLE;
        } else if (now - s_mark >= BUTTON_DEBOUNCE_MS) {
            s_state = BTN_HOLD;
            s_mark = now;
        }
        break;

    case BTN_HOLD:
        if (!Button_IsDown()) {
            s_state = BTN_IDLE;  /* released early: cancel */
        } else if (now - s_mark >= BUTTON_HOLD_MS) {
            s_state = BTN_PROMPT;
            s_mark = now;
        }
        break;

    case BTN_PROMPT:
        if (!Button_IsDown()) {
            s_state = BTN_BOUNCE_UP;
            s_mark = now;
        }
        break;

    case BTN_BOUNCE_UP:
        if (Button_IsDown()) {
            s_state = BTN_PROMPT;
        } else if (now - s_mark >= BUTTON_DEBOUNCE_MS) {
            s_state = BTN_IDLE;
            Peripheral_FactoryResetSecurity();
            Peripheral_ScheduleSystemReset(500);
        }
        break;
    }
}

uint8_t Button_LedLevel(void)
{
    if (s_state != BTN_PROMPT && s_state != BTN_BOUNCE_UP) {
        return BUTTON_LED_NONE;
    }
    return ((systick_ms / 100u) & 1u) ? 1u : 0u;
}

#endif /* ENABLE_BLE && version == 3 */
