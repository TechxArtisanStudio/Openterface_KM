/********************************** (C) COPYRIGHT *******************************
 * File Name          : RTC.c
 * Author             : WCH
 * Version            : V1.2
 * Date               : 2022/01/18
 * Description        : RTC configuration and its initialization
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/******************************************************************************/
/* Header file contains */
#include "HAL.h"

/*********************************************************************
 * CONSTANTS
 */
#define RTC_INIT_TIME_HOUR      0
#define RTC_INIT_TIME_MINUTE    0
#define RTC_INIT_TIME_SECEND    0

/***************************************************
 * Global variables
 */
volatile uint32_t RTCTigFlag;

/*******************************************************************************
 * @fn      RTC_SetTignTime
 *
 * @brief   Configure RTC trigger time
 *
 * @param   time    - Trigger time.
 *
 * @return  None.
 */
void RTC_SetTignTime(uint32_t time)
{
    RTC_WaitForLastTask();
    RTC_SetAlarm(time);
    RTC_WaitForLastTask();
    RTCTigFlag = 0;
}

/*******************************************************************************
 * @fn      HAL_Time0Init
 *
 * @brief   System timer initialization
 *
 * @param   None.
 *
 * @return  None.
 */
void HAL_TimeInit(void)
{
    uint16_t temp=0;
    uint8_t state=0;
    uint8_t lsi_clock=0;
    bleClockConfig_t  conf={0};

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR|RCC_APB1Periph_BKP, ENABLE);
    PWR_BackupAccessCmd(ENABLE);
#if( CLK_OSC32K )
    RCC_LSICmd(ENABLE);
    RCC_LSEConfig(RCC_LSE_OFF);
    RCC_RTCCLKConfig(RCC_RTCCLKSource_LSI);
    lsi_clock = 1;
#else
    RCC_LSEConfig(RCC_LSE_ON);
    /* Wait for the low-speed crystal. This wait must be bounded: the dev kit
     * variant has no 32.768 kHz crystal populated, and an unbounded wait here
     * hangs the firmware before the main loop starts (no ECM/ARP servicing,
     * no watchdog init). Fall back to the internal RC instead. */
    while (RCC_GetFlagStatus(RCC_FLAG_LSERDY) == RESET && temp < 100)
    {
        temp++;
        Delay_Ms(10);
    }
    if(RCC_GetFlagStatus(RCC_FLAG_LSERDY) == RESET)
    {
        printf("LSE not ready, using LSI\n");
        RCC_LSEConfig(RCC_LSE_OFF);
        RCC_LSICmd(ENABLE);
        while(RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET);
        RCC_RTCCLKConfig(RCC_RTCCLKSource_LSI);
        lsi_clock = 1;
    }
    else
    {
        RCC_RTCCLKConfig(RCC_RTCCLKSource_LSE);
    }
#endif
    RCC_RTCCLKCmd(ENABLE);
    RTC_WaitForLastTask();
    RTC_WaitForLastTask();
    RTC_SetPrescaler(1);
    RTC_WaitForLastTask();
    RTC_SetCounter(0);
    RTC_WaitForLastTask();
    if( lsi_clock )
    {
        Lib_Calibration_LSI();
    }
    conf.ClockAccuracy = lsi_clock?1000:100;
    conf.ClockFrequency = CAB_LSIFQ/2;
    conf.ClockMaxCount = 0xFFFFFFFF;
    conf.getClockValue = RTC_GetCounter;
    state = TMOS_TimerInit( &conf );
    if(state)
    {
        PRINT("TMOS_TimerInit err %x\n",state);
    }
}


__attribute__((interrupt("WCH-Interrupt-fast")))
void RTCAlarm_IRQHandler(void)
{
    RTCTigFlag = 1;
    EXTI_ClearITPendingBit(EXTI_Line17);
    RTC_ClearITPendingBit(RTC_IT_ALR);
    RTC_WaitForLastTask();
}

/******************************** endfile @ time ******************************/
