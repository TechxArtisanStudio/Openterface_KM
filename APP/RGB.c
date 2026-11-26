/**
 * @file RGB.c
 * @brief RGB LED driver using TIM3 PWM (PC6=R, PC8=G, PC9=B)
 *
 * Hardware mapping (TIM3 Full-Remap to GPIOC):
 *   - TIM3_CH1 -> PC6  (Blue,   active-low)
 *   - TIM3_CH2 -> PC7  (PWM debug LED, handled in pwm_led.c)
 *   - TIM3_CH3 -> PC8  (Green,  active-low)
 *   - TIM3_CH4 -> PC9  (Red,    active-low)
 *
 * Timing:
 *   - TIM2 provides a 1 ms systick (systick_ms) used by all non-blocking effects.
 *   - TIM3 runs at 1 MHz (96 MHz timer clock / 96), ARR=999 -> 1 kHz PWM, 0~999 duty.
 *     Because the RGB LED is common-anode (active-low), a larger duty value
 *     means lower brightness: pwm = (1.0 - intensity) * 999.
 *
 * Supported modes:
 *   RGB_MODE_SOLID   - solid color, set once and hold
 *   RGB_MODE_BREATH  - sinusoidal RGB breathing (120 deg phase shift per channel)
 *   RGB_MODE_FLASH   - monochrome on/off flashing at configurable interval
 *   RGB_MODE_STARTUP - power-on red/green/blue sequence, highest priority
 *
 * Additional:
 *   - Update_SDcard() reflects the SD card channel state on the RGB LED:
 *       HOST   (sd_card_channel_state == 1) -> GREEN when PA14 is LOW
 *       TARGET (sd_card_channel_state == 0) -> RED   when PA14 is LOW
 *       PA14 HIGH turns the LED OFF in both states.
 *   - PA14 is an active-low enable input (input pull-up).
 *   - PC13 is a feedback output (not PWM), reset LOW at init.
 */

#include "wch_irq.h"
#include "RGB.h"
#include "sd_switch.h"
#include <math.h>

/* Millisecond counter, incremented every 1 ms by TIM2 IRQ */
volatile uint32_t systick_ms = 0;

/* RGB internal state */
static RGB_Mode_t rgb_mode = RGB_MODE_SOLID;   /* current display mode */
static float rgb_r = 0, rgb_g = 0, rgb_b = 0;  /* last-set RGB values (0.0~1.0) */
static float breath_phase = 0;                  /* current phase for breathing effect */
static float breath_speed = 0.01f;              /* phase increment per tick */

/* Monochrome flashing state */
static uint16_t flash_interval = 500;           /* on/off interval in ms */
static RGB_Color_t flash_color = RGB_RED;       /* color used while flashing */
static uint32_t flash_last_ms = 0;              /* timestamp of last toggle */
static uint8_t flash_state = 0;                 /* 0=off, 1=on */

/* ====================== Startup Sequence ====================== */
/* Power-on three-color flash: Red -> Green -> Blue -> OFF */
typedef struct {
    RGB_Color_t color;
    uint16_t duration_ms;
} RGB_FlashStep_t;

static RGB_FlashStep_t startup_steps[] = {
    {RGB_RED,    2000},   /* red   for 2 s */
    {RGB_GREEN,  2000},   /* green for 2 s */
    {RGB_BLUE,   2000},   /* blue  for 2 s */
    {RGB_OFF,     500}    /* off   for 0.5 s, then hand over to normal mode */
};

static uint8_t startup_index = 0;       /* current step index */
static uint32_t startup_last_ms = 0;    /* timestamp when current step started */
static uint8_t startup_active = 0;      /* 1 while startup sequence is running */


/* ====================== TIM2: 1 ms Systick ====================== */
/*
 * HCLK = 96 MHz, PCLK1 = HCLK/2, so the TIM2 clock is 96 MHz.
 * Prescaler 96 -> 1 MHz tick; ARR = 999 -> 1 kHz update = 1 ms period.
 * Used only as a timebase for RGB effects; does not drive any GPIO.
 */
void RGB_TIM_Init(void)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);

    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure = {0};
    TIM_TimeBaseStructure.TIM_Period = 999;          /* 1000 ticks per period */
    TIM_TimeBaseStructure.TIM_Prescaler = SystemCoreClock / 1000000 - 1;
    TIM_TimeBaseStructure.TIM_ClockDivision = 0;
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM2, &TIM_TimeBaseStructure);

    TIM_ITConfig(TIM2, TIM_IT_Update, ENABLE);
    TIM_Cmd(TIM2, ENABLE);

    NVIC_InitTypeDef NVIC_InitStructure = {0};
    NVIC_InitStructure.NVIC_IRQChannel = TIM2_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
}

/* TIM2 interrupt: increment the millisecond counter */
void TIM2_IRQHandler(void) WCH_IRQ_FAST;
void TIM2_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM2, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
        systick_ms++;
    }
}


/* ====================== RGB_Init ====================== */
/*
 * Initialize all RGB-related peripherals:
 *   1. TIM3 CH1/3/4 PWM on PC6/PC8/PC9  (R/G/B, active-low)
 *   2. TIM2 as 1 ms systick
 *   3. PA14 as input pull-up (SD enable / active-low)
 *   4. PC13 as push-pull output (feedback), driven LOW
 */
void RGB_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    TIM_OCInitTypeDef TIM_OCInitStructure = {0};

    /* --- Step 1: TIM3 PWM GPIO (PC6=CH1/B, PC8=CH3/G, PC9=CH4/R) --- */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO, ENABLE);
    GPIO_PinRemapConfig(GPIO_FullRemap_TIM3, ENABLE);   /* TIM3 -> PC6/7/8/9 */

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6 | GPIO_Pin_8 | GPIO_Pin_9;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    /* --- Step 2: TIM3 PWM timebase --- */
    /*
     * TIM3 clock = PCLK1 x2 = 96 MHz, prescaler 96 -> 1 MHz,
     * ARR = 999 -> 1 kHz PWM frequency, duty range 0~999.
     */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure = {0};
    TIM_TimeBaseStructure.TIM_Period = 999;
    TIM_TimeBaseStructure.TIM_Prescaler = SystemCoreClock / 1000000 - 1;
    TIM_TimeBaseStructure.TIM_ClockDivision = 0;
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &TIM_TimeBaseStructure);

    /* PWM1 mode: output high while CNT < CCR; active-low LED inverts visually */
    TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM1;
    TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
    TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OCInitStructure.TIM_Pulse = 0;                /* start with all channels off (duty=0 -> LED off) */

    TIM_OC1Init(TIM3, &TIM_OCInitStructure);          /* CH1 -> PC6 -> Blue  */
    TIM_OC3Init(TIM3, &TIM_OCInitStructure);          /* CH3 -> PC8 -> Green */
    TIM_OC4Init(TIM3, &TIM_OCInitStructure);          /* CH4 -> PC9 -> Red   */

    TIM_OC1PreloadConfig(TIM3, TIM_OCPreload_Enable); /* shadow register for glitch-free updates */
    TIM_OC3PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_OC4PreloadConfig(TIM3, TIM_OCPreload_Enable);

    TIM_ARRPreloadConfig(TIM3, ENABLE);
    TIM_Cmd(TIM3, ENABLE);

    /* --- Step 3: TIM2 systick --- */
    /* TIM2 is now initialized early in main() via Systick_Init() */
    /* Only call RGB_TIM_Init() for KVM-Go (version 1) which has RGB LEDs */
#if version == 1
    RGB_TIM_Init();
#endif

    /* --- Step 4: PA14 as input pull-up (SD LED enable, active-low) --- */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_14;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* --- Step 5: PC13 as push-pull output (feedback pin), default LOW --- */
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_13;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    GPIO_ResetBits(GPIOC, GPIO_Pin_13);
}


/* ====================== RGB_SetColor ====================== */
/*
 * Set the RGB LED to an arbitrary color.
 * @param r, g, b  intensity per channel, clamped to [0.0, 1.0].
 *
 * The LED is common-anode (active-low), so the PWM compare value is
 * inverted: pwm = (1.0 - intensity) * 999.
 *   intensity 1.0 -> pwm 0   -> FET fully on  -> LED ON
 *   intensity 0.0 -> pwm 999 -> FET fully off -> LED OFF
 *
 * Channel mapping (TIM3 after full-remap):
 *   CH1 (CCR1) -> PC6 -> Blue
 *   CH3 (CCR3) -> PC8 -> Green
 *   CH4 (CCR4) -> PC9 -> Red
 */
void RGB_SetColor(float r, float g, float b)
{
    /* Clamp to valid range */
    if (r > 1) r = 1; if (g > 1) g = 1; if (b > 1) b = 1;
    if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;

    /* Invert for active-low output */
    uint16_t pwm_r = (uint16_t)((1.0f - r) * 999);
    uint16_t pwm_g = (uint16_t)((1.0f - g) * 999);
    uint16_t pwm_b = (uint16_t)((1.0f - b) * 999);

    TIM_SetCompare1(TIM3, pwm_b);    /* CH1 -> Blue  */
    TIM_SetCompare3(TIM3, pwm_g);    /* CH3 -> Green */
    TIM_SetCompare4(TIM3, pwm_r);    /* CH4 -> Red   */

    rgb_r = r; rgb_g = g; rgb_b = b; /* remember current color */
}


/* ====================== RGB_SetColorSolid ====================== */
/*
 * Display a solid (always-on) color from the RGB_Color_t enum.
 * @param color       one of RGB_RED / GREEN / BLUE / YELLOW / CYAN / MAGENTA / WHITE / RGB_OFF
 * @param brightness  multiplier in [0.0, 1.0]; 1.0 = full brightness
 *
 * Sets rgb_mode to RGB_MODE_SOLID so RGB_Update() will not override it.
 */
void RGB_SetColorSolid(RGB_Color_t color, float brightness)
{
    rgb_mode = RGB_MODE_SOLID;
    float r = 0, g = 0, b = 0;
    switch (color)
    {
        case RGB_RED:     r = 1;             break;
        case RGB_GREEN:         g = 1;       break;
        case RGB_BLUE:                b = 1; break;
        case RGB_YELLOW:  r = 1; g = 1;       break;
        case RGB_CYAN:          g = 1; b = 1; break;
        case RGB_MAGENTA: r = 1;       b = 1; break;
        case RGB_WHITE:   r = 1; g = 1; b = 1; break;
        case RGB_OFF:     r = 0; g = 0; b = 0; break;
    }
    RGB_SetColor(r * brightness, g * brightness, b * brightness);
}


/* ====================== RGB_SetBreathMode ====================== */
/*
 * Switch to sinusoidal RGB breathing mode.
 * @param speed  phase increment per 25 ms tick; larger = faster color cycle.
 *
 * The three channels are 120 degrees out of phase to produce a smooth
 * hue-rotating effect: phaseR = x, phaseG = x + 2*pi/3, phaseB = x + 4*pi/3.
 */
void RGB_SetBreathMode(float speed)
{
    rgb_mode = RGB_MODE_BREATH;
    breath_speed = speed;
}


/* ====================== RGB_SetFlashMode ====================== */
/*
 * Switch to monochrome on/off flashing mode.
 * @param interval_ms  half-period in ms (time between on and off transitions)
 * @param color        the color shown during the ON phase
 */
void RGB_SetFlashMode(uint16_t interval_ms, RGB_Color_t color)
{
    rgb_mode = RGB_MODE_FLASH;
    flash_interval = interval_ms;
    flash_color = color;
    flash_last_ms = systick_ms;
    flash_state = 0;   /* start in OFF phase */
}


/* ====================== RGB_FlashStartupSequence ====================== */
/*
 * Begin the power-on startup sequence (Red -> Green -> Blue -> OFF).
 * This mode has the highest priority in RGB_Update(); no other effect
 * will run until the sequence completes.
 */
void RGB_FlashStartupSequence(void)
{
    rgb_mode = RGB_MODE_STARTUP;
    startup_active = 1;
    startup_index = 0;
    startup_last_ms = systick_ms;
    RGB_SetColorSolid(startup_steps[0].color, 1.0f);
}


/* ====================== RGB_Update ====================== */
/*
 * Non-blocking periodic refresh. Call from the main loop on every iteration.
 *
 * Priority:
 *   1. Startup sequence (if active) — runs to completion, then falls through.
 *   2. Current rgb_mode (SOLID / BREATH / FLASH).
 *
 * SOLID mode does nothing here (color is set directly by RGB_SetColorSolid).
 * BREATH mode updates every 25 ms with a sine-wave color cycle.
 * FLASH mode toggles on/off every flash_interval ms.
 */
void RGB_Update(void)
{
    static uint32_t last_ms = 0;
    uint32_t now = systick_ms;

    /* --- Startup sequence (highest priority) --- */
    if (startup_active)
    {
        if (now - startup_last_ms >= startup_steps[startup_index].duration_ms)
        {
            startup_index++;
            if (startup_index >= sizeof(startup_steps) / sizeof(startup_steps[0]))
            {
                /* Sequence finished; hand control over to the current rgb_mode */
                startup_active = 0;
            }
            else
            {
                RGB_SetColorSolid(startup_steps[startup_index].color, 1.0f);
                startup_last_ms = now;
            }
        }
        return;   /* do not process other modes while startup is active */
    }

    /* --- Normal mode processing --- */
    switch (rgb_mode)
    {
        case RGB_MODE_SOLID:
            /* Nothing to do — color was set directly by RGB_SetColorSolid() */
            break;

        case RGB_MODE_BREATH:
            /* Update every 25 ms for a smooth ~40 Hz animation rate */
            if (now - last_ms >= 25)
            {
                last_ms = now;
                breath_phase += breath_speed;
                if (breath_phase > 6.283185f) breath_phase -= 6.283185f;   /* wrap at 2*pi */

                /*
                 * Three sine waves, 120 degrees apart, mapped to 0.0~1.0:
                 *   R = sin(phase)         scaled to [0, 1]
                 *   G = sin(phase + 2*pi/3) scaled to [0, 1]
                 *   B = sin(phase + 4*pi/3) scaled to [0, 1]
                 */
                float r = (sinf(breath_phase)          + 1) * 0.5f;
                float g = (sinf(breath_phase + 2.094395f) + 1) * 0.5f;   /* +2*pi/3 */
                float b = (sinf(breath_phase + 4.18879f)  + 1) * 0.5f;   /* +4*pi/3 */

                RGB_SetColor(r, g, b);
            }
            break;

        case RGB_MODE_FLASH:
            /* Toggle on/off every flash_interval ms */
            if (now - flash_last_ms >= flash_interval)
            {
                flash_last_ms = now;
                flash_state = !flash_state;
                if (flash_state)
                    RGB_SetColorSolid(flash_color, 1.0f);
                else
                    RGB_SetColorSolid(RGB_OFF, 0);
            }
            break;

        case RGB_MODE_STARTUP:
            /* Handled above via startup_active flag */
            break;
    }
}


/* ====================== RGB_FlashVersionVerify ====================== */
/*
 * Firmware version verification flash sequence.
 * Called once at startup (after RGB_Init) to visually confirm the firmware
 * is running. The pattern differs by hardware version:
 *
 *   version == 1 (KVM-Go):    red 300ms -> green 300ms -> blue 200ms
 *   version == 3 (KeyMod):    handled separately via PC7 PWM LED in main()
 *   version == 2 (Mini KVM):  no flash (silent)
 *
 * This function is blocking (uses Delay_Ms). Call it before entering the
 * main loop.
 */
void RGB_FlashVersionVerify(void)
{
#if version == 1
    /* KVM-Go: RGB three-color flash */
    RGB_SetColor(1, 0, 0); Delay_Ms(300);   /* red   for 300ms */
    RGB_SetColor(0, 1, 0); Delay_Ms(300);   /* green for 300ms */
    RGB_SetColor(0, 0, 1); Delay_Ms(300);   /* blue  for 200ms */
#else
    /* version 2 and others: no flash */
    (void)0;
#endif
}


/* ====================== Update_SDcard ====================== */
/*
 * Reflect the current SD card channel state on the RGB LED.
 * Called from the main loop on every iteration.
 *
 * Behavior:
 *   sd_card_channel_state == 1 (HOST_SD):
 *     PA14 LOW  -> GREEN LED  (SD connected to HOST side)
 *     PA14 HIGH -> LED OFF
 *
 *   sd_card_channel_state == 0 (TARGET_SD):
 *     PA14 LOW  -> RED LED    (SD connected to TARGET side)
 *     PA14 HIGH -> LED OFF
 *
 * PA14 acts as an active-low enable: when HIGH the LED is forced off
 * regardless of the channel state.
 *
 * Note: This function overrides any color previously set by RGB_SetColorSolid,
 * because it is called every loop iteration after RGB_Update().
 */
void Update_SDcard(void)
{
    if (sd_card_channel_state == 1)
    {
        /* HOST_SD: GREEN when PA14 is LOW, OFF when PA14 is HIGH */
        if (GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_14))
            RGB_SetColorSolid(RGB_OFF, 1.0f);
        else
            RGB_SetColorSolid(RGB_GREEN, 1.0f);
    }
    else if (sd_card_channel_state == 0)
    {
        /* TARGET_SD: RED when PA14 is LOW, OFF when PA14 is HIGH */
        if (GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_14))
            RGB_SetColorSolid(RGB_OFF, 1.0f);
        else
            RGB_SetColorSolid(RGB_RED, 1.0f);
    }
}
