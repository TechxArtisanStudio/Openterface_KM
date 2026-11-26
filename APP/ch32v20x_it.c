/********************************** (C) COPYRIGHT *******************************
 * File Name          : ch32v20x_it.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2022/06/16
 * Description        : Main Interrupt Service Routines.
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/*********************************************************************
 * INCLUDES
 */
#include "wch_irq.h"
#include "ch32v20x_it.h"
#include "include/Version_selection.h"
#ifdef ENABLE_BLE
#include "config.h"
#endif

/*********************************************************************
 * LOCAL FUNCTIONS
 */
void NMI_Handler(void) WCH_IRQ_FAST;
void HardFault_Handler(void) WCH_IRQ_FAST;
#ifdef ENABLE_BLE
void BB_IRQHandler(void) WCH_IRQ_FAST;
#endif

/*********************************************************************
 * @fn      NMI_Handler
 *
 * @brief   This function handles NMI exception.
 *
 * @return  None
 */
void NMI_Handler(void)
{
}

/*********************************************************************
 * @fn      HardFault_Handler
 *
 * @brief   This function handles Hard Fault exception.
 *
 * @return  None
 */
void HardFault_Handler(void)
{
    NVIC_SystemReset();
    while(1)
    {
    }
}

#ifdef ENABLE_BLE
void BB_IRQHandler(void)
{
    BB_IRQLibHandler();
}
#endif /* ENABLE_BLE */
