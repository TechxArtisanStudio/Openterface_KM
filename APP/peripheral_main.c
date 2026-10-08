/*******************************************************************************/
/* Header Files */
#include "ch32v20x_usbfs_device.h"
#include "usbd_composite_km.h"
#include "iwdg_auto.h"
#include "UART.h"
#include "debug.h"
#include "usb_lib.h"
#ifdef ENABLE_BLE
#include "config.h"
#include "HAL.h"
#include "gattprofile.h"
#include "peripheral.h"
#include "include/ble_eth_protocol.h"
#endif
#include "include/keyboard_handler.h"
#include "include/mouse_handler.h"
#include "sd_switch.h"
#include "RGB.h"
#include "DS18B20.h"
#include "CH32_TEMP.h"
#include "km_ring.h"
#include "Version_selection.h"
#include "include/uac_handler.h"
#include "include/acm_bridge.h"
#include "include/ecm_bridge.h"
#include "include/usb_mode_config.h"
#include "include/default_target_config.h"
#include "include/ecm_ip_config.h"
#include "include/pwm_led.h"

/*********************************************************************
 * GLOBAL TYPEDEFS
 */
#ifdef ENABLE_BLE
__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];
#endif

#if(defined(BLE_MAC)) && (BLE_MAC == TRUE)
const uint8_t MacAddr[6] = {0x84, 0xC2, 0xE4, 0x03, 0x02, 0x02};
#endif

/*********************************************************************
 * @fn      Main_Circulation
 *
 * @brief   Main loop
 *
 * @return  none
 */
__attribute__((section(".highcode")))
__attribute__((noinline))
void Main_Circulation(void)
{
    uint8_t selector_prev_state;
    // SD card switching status reading
    if (version == 1) {
        selector_prev_state = GPIO_ReadInputDataBit(GPIOB, GPIO_Pin_8);
    }
    else if (version == 2) {
        selector_prev_state = GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_4);
    }
    uint32_t t0 = systick_ms;
    uint32_t led_last = 0;
    uint16_t last_tx_count = 0;
    uint8_t led_state = 0;     /* 0=off, 1=on */
    uint32_t last_tx_time = 0; /* timestamp of last TX activity */
    uint32_t last_ep6_count = 0; /* track EP6 counter changes */
    uint32_t last_dhcp_reset = 0; /* periodically reset DHCP LED mode */
    CH9329_Cmd_GetInfo_Reply(00);


    while(1)
    {
        /* USB Endpoint Busy Timeout Recovery
         * Check if any USB endpoint is stuck in busy state and recover.
         * This prevents permanent endpoint lockup if transfer completion
         * interrupt is lost due to EMI or other issues.
         */
        USBFS_CheckEndpointBusyTimeout();

        /* Simple LED for DHCP debug: ON = g_dhcp_led_mode active, OFF = idle */
        uint32_t now = systick_ms;
        uint16_t tx_now = g_ecm_tx_count;
        if (tx_now != last_tx_count) {
            last_tx_time = now;
            last_tx_count = tx_now;
        }

        /* Poll EP6 OUT interrupt diagnostic flag */
        uint8_t ep6_fired = g_usbfs_ep6_int_flag;
        uint8_t ep6_tog = g_usbfs_ep6_tog_ok;
        uint16_t ep6_len = g_usbfs_ep6_rx_len;
        if (ep6_fired) {
            g_usbfs_ep6_int_flag = 0;  /* clear for next cycle */
        }

        /* Poll EP5 OUT debug counters */
        extern volatile uint32_t g_ep5_out_call_count;
        extern volatile uint32_t g_ep5_out_total_bytes;
        static uint32_t last_ep5_count = 0;
        if (g_ep5_out_call_count != last_ep5_count) {
            last_ep5_count = g_ep5_out_call_count;
#ifdef ENABLE_BLE
            /* Blink LED when EP5 receives data */
            extern void HalLedBlink(uint8_t leds, uint8_t numBlinks, uint8_t percent, uint16_t period);
            HalLedBlink(1, 1, 50, 100);
#endif
        }

        /* Auto-reset DHCP diagnostic every 3 seconds */
        if (now - last_dhcp_reset >= 3000) {
            g_dhcp_led_mode = 0;
            last_dhcp_reset = now;
        }

        //rgb_state
        RGB_Update();
        USB_ModeConfig_Poll();
        // BLE connection LED overrides when active
#if defined(ENABLE_BLE) && version == 1
        if (g_ble_led_state) {
            static uint8_t last_ble_led = 0;
            if (g_ble_led_state != last_ble_led) {
                last_ble_led = g_ble_led_state;
                switch (g_ble_led_state) {
                    case 1: RGB_SetBreathMode(0.03f); break;            // advertising → blue pulse
                    case 2: RGB_SetColorSolid(RGB_GREEN, 1.0f); break;  // connected → solid green
                    case 3: RGB_SetFlashMode(300, RGB_RED); break;      // disconnected → red flash
                    case 4: RGB_SetColorSolid(RGB_RED, 1.0f); break;    // error → solid red
                }
            }
        }
        // KVM-Go: always check PA14 via SD card switch logic
        Update_SDcard();
#else
        Update_SDcard();
#endif
        //Enable Bluetooth broadcasting
#ifdef ENABLE_BLE
        if (version == 1 || version == 3) {
            TMOS_SystemProcess();
        }
#endif
        //Instruction code receiving protocol
        USB_DataRx_To_KMHandle();
        //Keyboard and mouse control code sending loop
        USB_SendFromQueues();
        /* PC7 LED debug for DHCP:
         * 55 = handler entered, 60 = MAC match, 61 = MAC unknown,
         * 62 = bad cookie, 63 = chaddr mismatch, 11/12 = msg parsed/queued
         * otherwise: ON if ecm_polled, OFF if idle
         * On KeyMod (version==3), BLE status overrides when active. */
        uint8_t ecm_polled = 0;
        if (USB_ModeConfig_ActiveModeBridgeAcm()) {
            ACM_Bridge_Poll();
        }
        else if (USB_ModeConfig_ActiveModeBridgeEcm()) {
            ECM_Bridge_Poll();
            ecm_polled = 1;
        }
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
        else if (USB_ModeConfig_ActiveModeHasHostCdcEcm() &&
                 USB_ModeConfig_ActiveModeHasHostCdcAcm()) {
            ECM_Bridge_Poll();
            ecm_polled = 1;
        }
#endif
        /* Also poll ECM bridge when BLE-Eth has active tunnels or ARP-pending connections */
#ifdef ENABLE_BLE
        else if (BLE_Eth_HasActiveOrPendingConnection()) {
            ECM_Bridge_Poll();
            ecm_polled = 1;
        }
#endif

        uint8_t ble_overrides_pc7 = 0;
#if defined(ENABLE_BLE) && version == 3
        if (g_ble_led_state) {
            ble_overrides_pc7 = 1;
            switch (g_ble_led_state) {
                case 1: PWM_LED_SetBrightness((systick_ms / 1000) % 2 ? 100 : 0); break;  // advertising slow blink
                case 2: PWM_LED_SetBrightness(100); break;                               // connected solid
                case 3: PWM_LED_SetBrightness((systick_ms / 500) % 2 ? 100 : 0); break;  // disconnected fast blink
                case 4: PWM_LED_SetBrightness((systick_ms / 80) % 2 ? 100 : 0); break;   // error rapid blink
            }
        }
#endif
        if (!ble_overrides_pc7) {
            if (g_dhcp_led_mode == 60 || g_dhcp_led_mode == 55 || g_dhcp_led_mode == 11 ||
                g_dhcp_led_mode == 12) {
                PWM_LED_SetBrightness(100);  /* solid ON = good path */
            } else if (g_dhcp_led_mode == 61 || g_dhcp_led_mode == 62 || g_dhcp_led_mode == 63) {
                PWM_LED_SetBrightness(0);    /* OFF = error */
            } else if (g_ecm_tx_state == 2) {
                PWM_LED_SetBrightness(100);  /* TX success */
            } else if (g_ecm_tx_state == 4) {
                PWM_LED_SetBrightness(0);    /* TX failed */
            } else if (ecm_polled || g_dhcp_led_mode != 0) {
                PWM_LED_SetBrightness(100);
            } else {
                PWM_LED_SetBrightness(0);
            }
        }


        //Different devices have different control pin
        //version == 1
        if (version == 1) {
        if(systick_ms - t0 > 20000)
        {
            SD_Switch_StateMachine();
        }
        else {
        GPIO_WriteBit(GPIOA, GPIO_Pin_1, Bit_RESET);
        }
        }
        //version == 2
        else
        {SD_Switch_StateMachine();}
        //watchdog
        IWDG_Auto_Handler();
    }
}

int main(void) {

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();
    Delay_Init();
    USB_ModeConfig_Init();
    DefaultTargetConfig_Init();
#if USB_WORK_MODE_COMPILES_BRIDGE_ECM
    ECM_IPConfig_Init();
#endif
    //usbd_init
    USART_Printf_Init(115200);
    Set_USBConfig();
    USB_Init();
    USB_Interrupts_Config();
#ifdef ENABLE_BLE
    WCHBLE_Init();
#endif
    if (version == 1)
    {
        DS18B20_Init();
    }
    //usbfs_init
    USBFS_RCC_Init();
    USBFS_Device_Init(ENABLE);
    USB_Sleep_Wakeup_CFG();
#ifdef USB_BRINGUP_DIAG
    while (1)
    {
    }
#endif
#ifdef ENABLE_UAC
    if (USB_ModeConfig_ActiveModeHasTargetUac()) {
        UAC_Init();
    }
#endif
    Keyboard_Init();
    Mouse_Init();
    //ble_init(Must be placed after USB initialization)
#ifdef ENABLE_BLE
    HAL_Init();
    GAPRole_PeripheralInit();
    Peripheral_Init();
#endif
    //km_ring
    Queue_Init();
    //SDcontrol_init
    SD_Switch_Init();
    //temp_init
    ADC_Function_Init();
    //rgb_init
    RGB_Init();

    /* Initialize PC7 LED - must be after RGB_Init since RGB configures GPIOC/AFIO */
    PWM_LED_Init();

    /* Distinctive startup blink to verify firmware version is running */
#if version == 3
    /* KeyMod: 5 blinks in pattern: 2 fast, pause, 3 fast */
    for (int i = 0; i < 2; i++) {  /* 2 fast blinks */
        PWM_LED_SetBrightness(i % 2 ? 100 : 0);
        Delay_Ms(100);
    }
    PWM_LED_SetBrightness(0);
    Delay_Ms(400);  /* pause */
    for (int i = 0; i < 6; i++) {  /* 3 fast blinks */
        PWM_LED_SetBrightness(i % 2 ? 100 : 0);
        Delay_Ms(100);
    }
#elif version == 1
    /* KVM-Go: RGB three-color flash */
    RGB_FlashVersionVerify();
#else
    (void)0;
#endif

    Delay_Ms(1000);

    IWDG_Auto_Init(IWDG_Prescaler_32, 4000);
    Main_Circulation();
}
