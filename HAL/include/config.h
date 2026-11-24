/********************************** (C) COPYRIGHT *******************************
 * File Name          : CONFIG.h
 * Author             : WCH
 * Version            : V1.2
 * Date               : 2022/01/18
 * Description        : Configuration description and default value, 
 *                      it is recommended to modify the current value in the 
 *                      pre-processing of the engineering configuration
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/******************************************************************************/
#ifndef __CONFIG_H
#define __CONFIG_H

#define	ID_CH32V208							0x0208

#define CHIP_ID								ID_CH32V208

#ifdef WCHBLE_ROM
#include "wchble_rom.h"
#else
#include "wchble.h"
#endif

#include "ch32v20x.h"

/*********************************************************************
 【MAC】
 BLE_MAC                                    - Whether to customize Bluetooth MAC address (Default: FALSE - use chip MAC address), need to modify MAC address in main.c

 【SLEEP】
 HAL_SLEEP                                  - Whether to enable sleep function (Default: FALSE)
 WAKE_UP_MAX_TIME_US                        - Early wake time, i.e., the time required for system clock stabilization
                                                                                                                                        Pause mode    - 45
                                                                                                                                       Idle mode    - 5
 
 【TEMPERATION】
 TEM_SAMPLE                                 - Whether to enable temperature change calibration function, single calibration takes less than 10ms (Default: TRUE)
 
 【CALIBRATION】
 BLE_CALIBRATION_ENABLE                     - Whether to enable periodic calibration function, single calibration takes less than 10ms (Default: TRUE)
 BLE_CALIBRATION_PERIOD                     - Period of periodic calibration in ms (Default: 120000)
 
 【SNV】
 BLE_SNV                                    - Whether to enable SNV function for storing pairing information (Default: TRUE)
 BLE_SNV_ADDR                               - SNV information storage address, use end of data flash (Default: 0x77E00)
 BLE_SNV_NUM                                - Number of SNV information storage sectors equals the number of storable pairings (Default: 3)
                                            - If SNVNum parameter is configured, need to modify the flash size erased in Lib_Write_Flash function, size is SNVBlock*SNVNum

 【RTC】
 CLK_OSC32K                                 - RTC clock selection, must use external 32K if master role included (0 external (32768Hz), default: 1 internal (32000Hz), 2 internal (32768Hz))

 【MEMORY】
 BLE_MEMHEAP_SIZE                           - RAM size used by Bluetooth protocol stack, not less than 6K (Default: (1024*6))

 【DATA】
 BLE_BUFF_MAX_LEN                           - Maximum packet length per connection (Default: 27 (ATT_MTU=23), value range [27~251])
 BLE_BUFF_NUM                               - Number of packets cached by controller (Default: 5)
 BLE_TX_NUM_EVENT                           - Maximum number of data packets per connection event (Default: 1)
 BLE_TX_POWER                               - Transmit power (Default: LL_TX_POWEER_0_DBM (0dBm))
 
 【MULTICONN】
 PERIPHERAL_MAX_CONNECTION                  - Maximum number of simultaneous peripheral roles (Default: 1)
 CENTRAL_MAX_CONNECTION                     - Maximum number of simultaneous master roles (Default: 3)

 **********************************************************************/

/*********************************************************************
 * Default configuration values
 */
#ifndef BLE_MAC
#define BLE_MAC                             FALSE
#endif
#ifndef HAL_SLEEP
#define HAL_SLEEP                           FALSE
#endif
#ifndef WAKE_UP_MAX_TIME_US
#define WAKE_UP_MAX_TIME_US                 2400
#endif
#ifndef HAL_KEY
#define HAL_KEY                             FALSE
#endif
#ifndef HAL_LED
#define HAL_LED                             FALSE
#endif
#ifndef TEM_SAMPLE
#define TEM_SAMPLE                          TRUE
#endif
#ifndef BLE_CALIBRATION_ENABLE
#define BLE_CALIBRATION_ENABLE              TRUE
#endif
#ifndef BLE_CALIBRATION_PERIOD
#define BLE_CALIBRATION_PERIOD              120000
#endif
#ifndef BLE_SNV
#define BLE_SNV                             TRUE
#endif
#ifndef BLE_SNV_ADDR
#define BLE_SNV_ADDR                        0x08077C00
#endif
#ifndef BLE_SNV_NUM
#define BLE_SNV_NUM                         3
#endif
#ifndef CLK_OSC32K
#define CLK_OSC32K                          1   // Do not modify this item here, must modify in project configuration preprocessor if master role is included
#endif
#ifndef BLE_MEMHEAP_SIZE
#define BLE_MEMHEAP_SIZE                    (1024*10)
#endif
#ifndef BLE_BUFF_MAX_LEN            
#define BLE_BUFF_MAX_LEN                    251
#endif
#ifndef BLE_BUFF_NUM
#define BLE_BUFF_NUM                        8
#endif
#ifndef BLE_TX_NUM_EVENT
#define BLE_TX_NUM_EVENT                    4
#endif
#ifndef BLE_TX_POWER
#define BLE_TX_POWER                        LL_TX_POWEER_0_DBM
#endif
#ifndef PERIPHERAL_MAX_CONNECTION
#define PERIPHERAL_MAX_CONNECTION           1
#endif
#ifndef CENTRAL_MAX_CONNECTION
#define CENTRAL_MAX_CONNECTION              3
#endif

extern uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];
extern const uint8_t MacAddr[6];

#endif

