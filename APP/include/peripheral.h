/********************************** (C) COPYRIGHT *******************************
 * File Name          : peripheral.h
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2018/12/11
 * Description        :
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

#ifndef PERIPHERAL_H
#define PERIPHERAL_H
#include "RingMem.h"

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************************
 * INCLUDES
 */

/*********************************************************************
 * CONSTANTS
 */

// Peripheral Task Events
#define SBP_START_DEVICE_EVT    0x0001
#define SBP_PERIODIC_EVT        0x0002
#define SBP_READ_RSSI_EVT       0x0004
#define SBP_PARAM_UPDATE_EVT    0x0008
#define SBP_PHY_UPDATE_EVT      0x0010
#define SBP_PROCESS_USBDATA_EVT 0x0020
#define SBP_PROCESS_BLEDATA_EVT 0x0040
#define SBP_PROCESS_BLEETH_TX_EVT 0x0080
#define SBP_SYSTEM_RESET_EVT    0x0100
#define SBP_PAIR_LED_EVT        0x0200
#define SBP_SECURITY_REQ_EVT    0x0400

/*********************************************************************
 * MACROS
 */
typedef struct
{
    uint16_t connHandle; // Connection handle of current connection
    uint16_t connInterval;
    uint16_t connSlaveLatency;
    uint16_t connTimeout;
} peripheralConnItem_t;

#ifdef ENABLE_BLE
extern RingMemParm_t RingMemUSB;
extern uint8_t RingMemUSBBuf[512];
extern RingMemParm_t RingMemBLE;
extern uint8_t RingMemBLEBuf[512];
extern uint8_t Peripheral_TaskID;
extern uint8_t g_ble_led_state;  // BLE connection LED status
/*********************************************************************
 * FUNCTIONS
 */

/*
 * Task Initialization for the BLE Application
 */
extern void Peripheral_Init(void);

/*
 * Task Event Processor for the BLE Application
 */
extern uint16_t Peripheral_ProcessEvent(uint8_t task_id, uint16_t events);
uint8_t app_usb_notify(uint8_t *SendBuf, uint8_t l);

/*
 * Check if a CH9329 command byte should be handled locally (not forwarded to USB host).
 * Returns 1 if the command is a CH9329 command that the device handles directly.
 */
uint8_t Peripheral_IsCh9329CommandByte(uint8_t cmd);

/* Where CH9329_SendResponse puts a response. */
#define BLE_CH9329_RESP_USB   0   /* USB EP3 queue (default, USB serial path) */
#define BLE_CH9329_RESP_BLE   1   /* FFF1 notification */
#define BLE_CH9329_RESP_NONE  2   /* dropped: HID reports over BLE get no ACK */

/*
 * Select where CH9329 responses go.
 * Must be set before dispatching a CH9329 command received from BLE.
 */
void Peripheral_SetBleCh9329Response(uint8_t mode);

/*
 * Read the current response routing mode.
 * Called by CH9329_SendResponse to decide where to send the response.
 */
uint8_t Peripheral_GetBleCh9329Response(void);

/*
 * Handle BLE security commands (pairing, bonding, etc.)
 * Called from CH9329 command dispatcher for CMD_BLE_SECURITY (0x22)
 */
void Peripheral_SecurityCommand(uint8_t addr, uint8_t cmd_code, uint8_t *pdata, uint8_t data_len);

/*
 * Handle BLE advertising parameter commands (get/set channel map, etc.)
 * Called from CH9329 command dispatcher for CMD_BLE_ADV_PARAM (0x19)
 */
void Peripheral_BleAdvParamCommand(uint8_t addr, uint8_t cmd_code, uint8_t *pdata, uint8_t data_len);

/*
 * Get the current BLE connection handle
 * Returns GAP_CONNHANDLE_INIT (0xFFFF) if not connected
 */
uint16_t Peripheral_GetConnHandle(void);

/*
 * Erase every bond, clear the PIN override and restore the default bond slot.
 * Shared by the 0x22 0x02 command and the 5 s button hold.
 */
void Peripheral_FactoryResetSecurity(void);

/*
 * Schedule a system reset after a delay to allow BLE notifications
 * to be transmitted.
 */
void Peripheral_ScheduleSystemReset(uint16_t delay_ms);

/*
 * Update BLE advertising data and scan response with current device name.
 * Called after device name is changed to ensure new name appears in advertisements.
 * Stops advertising, updates data, and restarts advertising.
 */
void Peripheral_UpdateBLEAdvertisingData(void);

#endif /* ENABLE_BLE */

/*********************************************************************
*********************************************************************/

#ifdef __cplusplus
}
#endif

#endif
