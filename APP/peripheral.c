/********************************** (C) COPYRIGHT *******************************
 * File Name          : peripheral.C
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2018/12/10
 * Description        : Peripheral slave multi-connection application, initialize 
 *                      broadcast connection parameters, then broadcast, after connecting 
 *                      to the host, request to update connection parameters, 
 *                      and transmit data through custom services.
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/*********************************************************************
 * INCLUDES
 */
#include "include/Version_selection.h"
#ifdef ENABLE_BLE
#include <ch32v20x_usbfs_device.h>
#include "config.h"
#include "devinfoservice.h"
#include "gattprofile.h"
#include "peripheral.h"
#include "ble_usb_service.h"
#include "include/ble_eth_protocol.h"
#include "include/default_target_config.h"
#include "include/ble_name_config.h"
#include "usbd_composite_km.h"
#include "include/ecm_bridge.h"
#include "RGB.h"

/*********************************************************************
 * MACROS
 */

/*********************************************************************
 * CONSTANTS
 */

// How often to perform periodic event
#define SBP_PERIODIC_EVT_PERIOD              1600

// Keep BLE-Eth notifications below large ATT payload sizes that corrupt on some hosts.
#define BLE_ETH_SAFE_NOTIFY_LEN              180

// How often to perform read rssi event
#define SBP_READ_RSSI_EVT_PERIOD             3200

// Parameter update delay
#define SBP_PARAM_UPDATE_DELAY               6400

// PHY update delay
#define SBP_PHY_UPDATE_DELAY                 2400

// What is the advertising interval when device is discoverable (units of 625us, 80=50ms)
#define DEFAULT_ADVERTISING_INTERVAL         80

// Limited discoverable mode advertises for 30.72s, and then stops
// General discoverable mode advertises indefinitely
#define DEFAULT_DISCOVERABLE_MODE            GAP_ADTYPE_FLAGS_GENERAL

// Minimum connection interval (units of 1.25ms, 6=7.5ms)
#define DEFAULT_DESIRED_MIN_CONN_INTERVAL    10

// Maximum connection interval (units of 1.25ms, 100=125ms)
#define DEFAULT_DESIRED_MAX_CONN_INTERVAL   10

// Slave latency to use parameter update
#define DEFAULT_DESIRED_SLAVE_LATENCY        0

// Supervision timeout value (units of 10ms, 100=1s)
#define DEFAULT_DESIRED_CONN_TIMEOUT         100

// Company Identifier: WCH
#define WCH_COMPANY_ID                       0x07D7

/*********************************************************************
 * TYPEDEFS
 */

/*********************************************************************
 * GLOBAL VARIABLES
 */

/*********************************************************************
 * EXTERNAL VARIABLES
 */

/*********************************************************************
 * EXTERNAL FUNCTIONS
 */

/*********************************************************************
 * LOCAL VARIABLES
 */
uint8_t Peripheral_TaskID = INVALID_TASK_ID; // Task ID for internal task/event processing

#ifndef BLE_DEVICE_NAME
#define BLE_DEVICE_NAME "KVM-GO"
#endif

// GAP - SCAN RSP data (max size = 31 bytes)
static uint8_t scanRspData[31] = {0};

// Current TX power level (tracked at runtime)
static uint8_t g_current_tx_power = BLE_TX_POWER;

// Test mode flag (1 = test mode active, 0 = normal mode)
static uint8_t g_test_mode_active = 0;
static uint8_t scanRspDataLen = 0;

// GAP - Advertisement data (max size = 31 bytes, though this is
// best kept short to conserve power while advertising)
static uint8_t advertData[31] = {
    // Flags; this sets the device to use limited discoverable
    // mode (advertises for 30 seconds at a time) instead of general
    // discoverable mode (advertises indefinitely)
    0x02, // length of this data
    GAP_ADTYPE_FLAGS,
    DEFAULT_DISCOVERABLE_MODE | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED,

    // service UUID, to notify central devices what services are included
    // in this peripheral
    0x03,                  // length of this data
    GAP_ADTYPE_16BIT_MORE, // some of the UUID's, but not all
    LO_UINT16(SIMPLEPROFILE_SERV_UUID),
    HI_UINT16(SIMPLEPROFILE_SERV_UUID)
};
static uint8_t advertDataLen = 7; // Initial length without device name

// GAP GATT Attributes
static uint8_t attDeviceName[GAP_DEVICE_NAME_LEN] = {0};

// Connection item list
static peripheralConnItem_t peripheralConnList;

static uint16_t peripheralMTU = ATT_MTU_SIZE;

RingMemParm_t RingMemUSB;
uint8_t       RingMemUSBBuf[512];
RingMemParm_t RingMemBLE;
uint8_t       RingMemBLEBuf[512];

// BLE LED status — 0=default(SD card), 1=advertising, 2=connected, 3=disconnected, 4=error
uint8_t g_ble_led_state = 0;

// Flag to route CH9329 responses via BLE notification instead of USB EP3
static uint8_t g_ble_ch9329_response = 0;
/*********************************************************************
 * LOCAL FUNCTIONS
 */
static void Peripheral_ProcessTMOSMsg(tmos_event_hdr_t *pMsg);
static void peripheralStateNotificationCB(gapRole_States_t newState, gapRoleEvent_t *pEvent);
static void performPeriodicTask(void);
static void simpleProfileChangeCB(uint8_t paramID, uint8_t *pValue, uint16_t len);
static void peripheralParamUpdateCB(uint16_t connHandle, uint16_t connInterval,
                                    uint16_t connSlaveLatency, uint16_t connTimeout);
static void peripheralInitConnItem(peripheralConnItem_t *peripheralConnList);
static void peripheralRssiCB(uint16_t connHandle, int8_t rssi);
static void peripheralChar4Notify(uint8_t *pValue, uint16_t len);
void        ble_usb_ServiceEvt(uint16_t connection_handle, ble_usb_evt_t *p_evt);
int         RingMem_Protection(uint8_t enable);
static void ble_eth_event_handler(ble_eth_evt_t *evt);
static void ble_eth_tx_event_trigger(void);
static void ecm_bridge_tx_handler(const uint8_t *data, uint16_t len);

static uint8_t Peripheral_IsCh9329ModeCommandFrame(const uint8_t *buf, uint16_t len)
{
    uint16_t frame_len;
    uint8_t sum = 0;
    uint16_t index;

    if (buf == NULL || len < 6) {
        return 0;
    }
    if (buf[0] != CH9329_FRAME_HEAD1 || buf[1] != CH9329_FRAME_HEAD2 || buf[3] != CMD_USB_WORK_MODE) {
        return 0;
    }

    frame_len = (uint16_t)(6u + buf[4]);
    if (len < frame_len) {
        return 0;
    }

    for (index = 0; index < (uint16_t)(frame_len - 1u); index++) {
        sum = (uint8_t)(sum + buf[index]);
    }
    return (sum == buf[frame_len - 1u]) ? 1u : 0u;
}

/*********************************************************************
 * @fn      Peripheral_IsCh9329CommandByte
 *
 * @brief   Check if a command byte is a CH9329 command that should be
 *          handled locally by the device (not forwarded to USB host).
 *
 * @param   cmd - Command byte to check
 *
 * @return  1 if CH9329 command, 0 otherwise (BLE-Eth or unknown)
 */
uint8_t Peripheral_IsCh9329CommandByte(uint8_t cmd)
{
    switch (cmd)
    {
        case CMD_GET_INFO:             /* 0x01 - Query device status: board version,
                                        *        USB enumeration state, keyboard LED
                                        *        indicators, and firmware version. */

        case CMD_SEND_KB_GENERAL_DATA: /* 0x02 - Send a standard USB HID keyboard
                                        *        report (modifier keys + 6 keycodes). */

        case CMD_SEND_KB_MEDIA_DATA:   /* 0x03 - Send media / consumer control keys
                                        *        (play, volume, brightness) or ACPI
                                        *        power keys (sleep, shutdown, wake). */

        case CMD_SEND_MS_ABS_DATA:     /* 0x04 - Send absolute-position mouse / stylus
                                        *        report (X, Y coordinates in device
                                        *        units, buttons, wheel). */

        case CMD_SEND_MS_REL_DATA:     /* 0x05 - Send relative-motion mouse report
                                        *        (delta X, delta Y, buttons, wheel). */

        case CMD_GET_PARA_CFG:         /* 0x08 - Read back the device's CH9329
                                        *        parameter configuration block
                                        *        (fixed 44-byte compatibility reply). */

        case CMD_SD_SWITCH:            /* 0x17 - Switch or query the SD card routing
                                        *        path (Openterface extension). */

        case CMD_DS18B20_GET_TEMP:     /* 0x18 - Issue a temperature measurement
                                        *        command to the DS18B20 onewire
                                        *        sensor (Openterface extension). */

        case CMD_BLE_ADV_PARAM:        /* 0x19 - Control BLE advertising parameters
                                        *        such as channel map selection and
                                        *        continuous-TX test mode (extension). */

        case CMD_BLE_SECURITY:         /* 0x22 - Manage BLE security / pairing:
                                        *        open pairing window, erase bonds,
                                        *        or query bond count (extension). */

        case CMD_BLE_SET_NAME:         /* 0x23 - Set BLE device name (persisted to
                                        *        Flash) or reset to factory default
                                        *        "KVM-GO" (Openterface extension). */

        case CMD_SET_ECM_IP_CONFIG:    /* 0x26 - Set ECM network IP configuration
                                        *        (host IP, dev IP, subnet, target IPs).
                                        *        Persisted to Flash (Openterface extension). */

        case CMD_GET_ECM_IP_CONFIG:    /* 0x27 - Get ECM network IP configuration
                                        *        from RAM cache, no Flash access
                                        *        (Openterface extension). */

        case CMD_USB_WORK_MODE:        /* 0x30 - Query or change the active USB
                                        *        personality (HID+ACM, HID+UAC+ACM,
                                        *        HID+ACM-Bridge, HID+ECM-Bridge)
                                        *        and trigger a reboot if changed. */

        case CMD_SYSTEM_RESET:         /* 0x31 - Trigger a system reset / reboot.
                                        *        Optionally reset BLE name to factory
                                        *        default "KVM-GO" if payload is 0x00. */
            return 1;

        default:
            return 0;
    }
}

/*********************************************************************
 * @fn      Peripheral_SetBleCh9329Response
 *
 * @brief   Enable/disable BLE CH9329 response routing.
 *          When enabled, CH9329_SendResponse sends via BLE notification.
 *
 * @param   enable - 1 to enable, 0 to disable
 */
void Peripheral_SetBleCh9329Response(uint8_t enable)
{
    g_ble_ch9329_response = enable;
}

/*********************************************************************
 * @fn      Peripheral_GetBleCh9329Response
 *
 * @brief   Check if BLE CH9329 response routing is enabled.
 *
 * @return  1 if enabled, 0 if disabled
 */
uint8_t Peripheral_GetBleCh9329Response(void)
{
    return g_ble_ch9329_response;
}

/*********************************************************************
 * PROFILE CALLBACKS
 */

// GAP Role Callbacks
static gapRolesCBs_t Peripheral_PeripheralCBs = {
    peripheralStateNotificationCB, // Profile State Change Callbacks
    peripheralRssiCB,              // When a valid RSSI is read from controller (not used by application)
    peripheralParamUpdateCB
};

// Broadcast Callbacks
static gapRolesBroadcasterCBs_t Broadcaster_BroadcasterCBs = {
    NULL, // Not used in peripheral role
    NULL  // Receive scan request callback
};

// GAP Bond Manager Callbacks
static gapBondCBs_t Peripheral_BondMgrCBs = {
    NULL, // Passcode callback (not used by application)
    NULL  // Pairing / Bonding state Callback (not used by application)
};

// Simple GATT Profile Callbacks
static simpleProfileCBs_t Peripheral_SimpleProfileCBs = {
    simpleProfileChangeCB // Characteristic value change callback
};
/*********************************************************************
 * PUBLIC FUNCTIONS
 */

/*********************************************************************
 * @fn      Peripheral_Init
 *
 * @brief   Initialization function for the Peripheral App Task.
 *          This is called during initialization and should contain
 *          any application specific initialization (ie. hardware
 *          initialization/setup, table initialization, power up
 *          notificaiton ... ).
 *
 * @param   task_id - the ID assigned by TMOS.  This ID should be
 *                    used to send messages and set timers.
 *
 * @return  none
 */
void Peripheral_Init()
{
    Peripheral_TaskID = TMOS_ProcessEventRegister(Peripheral_ProcessEvent);

    // 【Compile-time guarantee】KVM-GO: true initialization; MiniKVM: empty macro expansion
    BLE_NameConfig_Init();

    // Setup the GAP Peripheral Role Profile
    {
        uint8_t  initial_advertising_enable = TRUE;
        uint16_t desired_min_interval = DEFAULT_DESIRED_MIN_CONN_INTERVAL;
        uint16_t desired_max_interval = DEFAULT_DESIRED_MAX_CONN_INTERVAL;
        uint8_t nameLen = strlen(g_ble_name.name);  // Use runtime state
        uint8_t deviceNameLen = sizeof(attDeviceName) - 1;

        if(nameLen > 20) nameLen = 20;
        if(deviceNameLen > 20) deviceNameLen = 20;

        tmos_memset(attDeviceName, 0, sizeof(attDeviceName));
        tmos_memcpy(attDeviceName, g_ble_name.name, deviceNameLen);

        // Add device name to advertisement data (not just scan response)
        advertData[advertDataLen++] = nameLen + 1;
        advertData[advertDataLen++] = GAP_ADTYPE_LOCAL_NAME_COMPLETE;
        tmos_memcpy(&advertData[advertDataLen], g_ble_name.name, nameLen);
        advertDataLen += nameLen;

        scanRspDataLen = 0;
        scanRspData[scanRspDataLen++] = nameLen + 1;
        scanRspData[scanRspDataLen++] = GAP_ADTYPE_LOCAL_NAME_COMPLETE;
        tmos_memcpy(&scanRspData[scanRspDataLen], g_ble_name.name, nameLen);
        scanRspDataLen += nameLen;
        
        scanRspData[scanRspDataLen++] = 0x05;
        scanRspData[scanRspDataLen++] = GAP_ADTYPE_SLAVE_CONN_INTERVAL_RANGE;
        scanRspData[scanRspDataLen++] = LO_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL);
        scanRspData[scanRspDataLen++] = HI_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL);
        scanRspData[scanRspDataLen++] = LO_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL);
        scanRspData[scanRspDataLen++] = HI_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL);
        
        scanRspData[scanRspDataLen++] = 0x02;
        scanRspData[scanRspDataLen++] = GAP_ADTYPE_POWER_LEVEL;
        scanRspData[scanRspDataLen++] = 0;

        // Set the GAP Role Parameters
        GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &initial_advertising_enable);
        GAPRole_SetParameter(GAPROLE_SCAN_RSP_DATA, scanRspDataLen, scanRspData);
        GAPRole_SetParameter(GAPROLE_ADVERT_DATA, advertDataLen, advertData);
        GAPRole_SetParameter(GAPROLE_MIN_CONN_INTERVAL, sizeof(uint16_t), &desired_min_interval);
        GAPRole_SetParameter(GAPROLE_MAX_CONN_INTERVAL, sizeof(uint16_t), &desired_max_interval);
    }

    {
        uint16_t advInt = DEFAULT_ADVERTISING_INTERVAL;

        // Set advertising interval
        GAP_SetParamValue(TGAP_DISC_ADV_INT_MIN, advInt);
        GAP_SetParamValue(TGAP_DISC_ADV_INT_MAX, advInt);

        // Enable scan req notify
        GAP_SetParamValue(TGAP_ADV_SCAN_REQ_NOTIFY, ENABLE);
    }

    // Setup the GAP Bond Manager
    {
        uint32_t passkey = 0; // passkey "000000"
        uint8_t  pairMode = GAPBOND_PAIRING_MODE_WAIT_FOR_REQ;
        uint8_t  mitm = TRUE;
        uint8_t  bonding = TRUE;
        uint8_t  ioCap = GAPBOND_IO_CAP_DISPLAY_ONLY;
        GAPBondMgr_SetParameter(GAPBOND_PERI_DEFAULT_PASSCODE, sizeof(uint32_t), &passkey);
        GAPBondMgr_SetParameter(GAPBOND_PERI_PAIRING_MODE, sizeof(uint8_t), &pairMode);
        GAPBondMgr_SetParameter(GAPBOND_PERI_MITM_PROTECTION, sizeof(uint8_t), &mitm);
        GAPBondMgr_SetParameter(GAPBOND_PERI_IO_CAPABILITIES, sizeof(uint8_t), &ioCap);
        GAPBondMgr_SetParameter(GAPBOND_PERI_BONDING_ENABLED, sizeof(uint8_t), &bonding);
    }

    // Initialize GATT attributes
    GGS_AddService(GATT_ALL_SERVICES);           // GAP
    GATTServApp_AddService(GATT_ALL_SERVICES);   // GATT attributes
    DevInfo_AddService();                        // Device Information Service
    SimpleProfile_AddService(GATT_ALL_SERVICES); // Simple GATT Profile
    ble_usb_add_service(ble_usb_ServiceEvt);

    // Set the GAP Characteristics
    // Pass actual name length (not buffer size) to avoid trailing zeros
    GGS_SetParameter(GGS_DEVICE_NAME_ATT, strlen(g_ble_name.name) + 1, (void *)g_ble_name.name);

    // Setup the SimpleProfile Characteristic Values
    {
        uint8_t charValue1[SIMPLEPROFILE_CHAR1_LEN] = {1};
        uint8_t charValue2[SIMPLEPROFILE_CHAR2_LEN] = {2};
        // uint8_t charValue3[SIMPLEPROFILE_CHAR3_LEN] = {3};
        // uint8_t charValue4[SIMPLEPROFILE_CHAR4_LEN] = {4};
        // uint8_t charValue5[SIMPLEPROFILE_CHAR5_LEN] = {1, 2, 3, 4, 5};

        SimpleProfile_SetParameter(SIMPLEPROFILE_CHAR1, SIMPLEPROFILE_CHAR1_LEN, charValue1);
        SimpleProfile_SetParameter(SIMPLEPROFILE_CHAR2, SIMPLEPROFILE_CHAR2_LEN, charValue2);
        // SimpleProfile_SetParameter(SIMPLEPROFILE_CHAR3, SIMPLEPROFILE_CHAR3_LEN, charValue3);
        // SimpleProfile_SetParameter(SIMPLEPROFILE_CHAR4, SIMPLEPROFILE_CHAR4_LEN, charValue4);
        // SimpleProfile_SetParameter(SIMPLEPROFILE_CHAR5, SIMPLEPROFILE_CHAR5_LEN, charValue5);
    }

    // Init Connection Item
    peripheralInitConnItem(&peripheralConnList);

    // Register callback with SimpleGATTprofile
    SimpleProfile_RegisterAppCBs(&Peripheral_SimpleProfileCBs);

    // Register receive scan request callback
    GAPRole_BroadcasterSetCB(&Broadcaster_BroadcasterCBs);

    // Setup a delayed profile startup
    tmos_set_event(Peripheral_TaskID, SBP_START_DEVICE_EVT);

    RingMemInit(&RingMemUSB, RingMemUSBBuf, 512, RingMem_Protection);
    RingMemInit(&RingMemBLE, RingMemBLEBuf, 512, RingMem_Protection);

    /* Initialize BLE-Eth tunneling protocol */
    BLE_Eth_Init(ble_eth_event_handler);

    /* Hook ECM bridge target frames into BLE-Eth reverse path */
    ECM_Bridge_SetBleTxCallback(ble_eth_tx_event_trigger);

    /* Route BLE-Eth TCP frames through ECM bridge */
    BLE_Eth_SetEcmtxCb(ecm_bridge_tx_handler);
}

/* Send Ethernet frame from BLE-Eth to ECM bridge (host side) */
static void ecm_bridge_tx_handler(const uint8_t *data, uint16_t len)
{
    ECM_Bridge_SendRawFrame(data, len);
}

/* Trigger BLE-Eth TX event when target frame data is pushed via BLE */
static void ble_eth_tx_event_trigger(void)
{
    tmos_set_event(Peripheral_TaskID, SBP_PROCESS_BLEETH_TX_EVT);
}

/*********************************************************************
 * @fn      peripheralInitConnItem
 *
 * @brief   Init Connection Item
 *
 * @param   peripheralConnList -
 *
 * @return  NULL
 */
static void peripheralInitConnItem(peripheralConnItem_t *peripheralConnList)
{
    peripheralConnList->connHandle = GAP_CONNHANDLE_INIT;
    peripheralConnList->connInterval = 0;
    peripheralConnList->connSlaveLatency = 0;
    peripheralConnList->connTimeout = 0;
}

/*********************************************************************
 * @fn      Peripheral_GetConnHandle
 *
 * @brief   Get the current BLE connection handle
 *
 * @return  Connection handle, or GAP_CONNHANDLE_INIT if not connected
 */
uint16_t Peripheral_GetConnHandle(void)
{
    return peripheralConnList.connHandle;
}

/*********************************************************************
 * @fn      Peripheral_ProcessEvent
 *
 * @brief   Peripheral Application Task event processor.  This function
 *          is called to process all events for the task.  Events
 *          include timers, messages and any other user defined events.
 *
 * @param   task_id - The TMOS assigned task ID.
 * @param   events - events to process.  This is a bit map and can
 *                   contain more than one event.
 *
 * @return  events not processed
 */
uint16_t Peripheral_ProcessEvent(uint8_t task_id, uint16_t events)
{
    //  VOID task_id; // TMOS required parameter that isn't used in this function

    if(events & SYS_EVENT_MSG)
    {
        uint8_t *pMsg;

        if((pMsg = tmos_msg_receive(Peripheral_TaskID)) != NULL)
        {
            Peripheral_ProcessTMOSMsg((tmos_event_hdr_t *)pMsg);
            // Release the TMOS message
            tmos_msg_deallocate(pMsg);
        }
        // return unprocessed events
        return (events ^ SYS_EVENT_MSG);
    }

    if(events & SBP_START_DEVICE_EVT)
    {
        // Start the Device
        GAPRole_PeripheralStartDevice(Peripheral_TaskID, &Peripheral_BondMgrCBs, &Peripheral_PeripheralCBs);
        return (events ^ SBP_START_DEVICE_EVT);
    }

    if(events & SBP_PERIODIC_EVT)
    {
        // Restart timer
        if(SBP_PERIODIC_EVT_PERIOD)
        {
            tmos_start_task(Peripheral_TaskID, SBP_PERIODIC_EVT, SBP_PERIODIC_EVT_PERIOD);
        }
        // Perform periodic application task
        performPeriodicTask();
        return (events ^ SBP_PERIODIC_EVT);
    }

    if(events & SBP_PARAM_UPDATE_EVT)
    {
        // Send connect param update request
        GAPRole_PeripheralConnParamUpdateReq(peripheralConnList.connHandle,
                                             DEFAULT_DESIRED_MIN_CONN_INTERVAL,
                                             DEFAULT_DESIRED_MAX_CONN_INTERVAL,
                                             DEFAULT_DESIRED_SLAVE_LATENCY,
                                             DEFAULT_DESIRED_CONN_TIMEOUT,
                                             Peripheral_TaskID);

        return (events ^ SBP_PARAM_UPDATE_EVT);
    }

    if(events & SBP_PHY_UPDATE_EVT)
    {
        // start phy update

        return (events ^ SBP_PHY_UPDATE_EVT);
    }

    if(events & SBP_READ_RSSI_EVT)
    {
        GAPRole_ReadRssiCmd(peripheralConnList.connHandle);
        tmos_start_task(Peripheral_TaskID, SBP_READ_RSSI_EVT, SBP_READ_RSSI_EVT_PERIOD);
        return (events ^ SBP_READ_RSSI_EVT);
    }

    if(events & SBP_PROCESS_USBDATA_EVT)
    {
        while(RingMemUSB.CurrentLen)
        {
            uint8_t buf[peripheralMTU - 3];
            if(RingMemUSB.CurrentLen > peripheralMTU - 3)
            {
                RingMemCopy(&RingMemUSB, buf, peripheralMTU - 3);
                if(app_usb_notify(buf, peripheralMTU - 3))
                {
                    tmos_start_task(Peripheral_TaskID, SBP_PROCESS_USBDATA_EVT, 80);
                    break;
                }
                RingMemDelete(&RingMemUSB, peripheralMTU - 3);
            }
            else
            {
                RingMemCopy(&RingMemUSB, buf, RingMemUSB.CurrentLen);
                if(app_usb_notify(buf, RingMemUSB.CurrentLen))
                {
                    tmos_start_task(Peripheral_TaskID, SBP_PROCESS_USBDATA_EVT, 80);
                    break;
                }
                RingMemDelete(&RingMemUSB, RingMemUSB.CurrentLen);
            }
        }
        return (events ^ SBP_PROCESS_USBDATA_EVT);
    }

    if(events & SBP_PROCESS_BLEDATA_EVT)
    {
        if(RingMemBLE.CurrentLen)
        {
            USBSendData();
            tmos_start_task(Peripheral_TaskID, SBP_PROCESS_BLEDATA_EVT, 5);
        }
        return (events ^ SBP_PROCESS_BLEDATA_EVT);
    }

    if(events & SBP_PROCESS_BLEETH_TX_EVT)
    {
        /* Send pending BLE-Eth frames over BLE notification */
        uint8_t tx_buf[256];
        uint16_t max_notify_len = (peripheralMTU > 3) ? (uint16_t)(peripheralMTU - 3) : 0;
        uint16_t len;

        if (max_notify_len > BLE_ETH_SAFE_NOTIFY_LEN) {
            max_notify_len = BLE_ETH_SAFE_NOTIFY_LEN;
        }

        while ((len = BLE_Eth_GetNextTxChunk(tx_buf, max_notify_len)) > 0) {
            if (app_usb_notify(tx_buf, (uint8_t)len) != SUCCESS) {
                /* BLE notify failed, re-schedule for retry */
                tmos_set_event(Peripheral_TaskID, SBP_PROCESS_BLEETH_TX_EVT);
                break;
            }
            BLE_Eth_ConsumeTxChunk(len);
        }
        return (events ^ SBP_PROCESS_BLEETH_TX_EVT);
    }

    if(events & SBP_SYSTEM_RESET_EVT)
    {
        /* System reset after BLE notification has been sent */
        NVIC_SystemReset();
        return (events ^ SBP_SYSTEM_RESET_EVT);
    }

    // Discard unknown events
    return 0;
}

/*********************************************************************
 * @fn      Peripheral_ProcessGAPMsg
 *
 * @brief   Process an incoming task message.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
static void Peripheral_ProcessGAPMsg(gapRoleEvent_t *pEvent)
{
    switch(pEvent->gap.opcode)
    {
        case GAP_SCAN_REQUEST_EVENT:
        {
            break;
        }

        case GAP_PHY_UPDATE_EVENT:
        {
            break;
        }

        default:
            break;
    }
}

/*********************************************************************
 * @fn      Peripheral_ProcessTMOSMsg
 *
 * @brief   Process an incoming task message.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
static void Peripheral_ProcessTMOSMsg(tmos_event_hdr_t *pMsg)
{
    switch(pMsg->event)
    {
        case GAP_MSG_EVENT:
        {
            Peripheral_ProcessGAPMsg((gapRoleEvent_t *)pMsg);
            break;
        }

        case GATT_MSG_EVENT:
        {
            gattMsgEvent_t *pMsgEvent;

            pMsgEvent = (gattMsgEvent_t *)pMsg;
            if(pMsgEvent->method == ATT_MTU_UPDATED_EVENT)
            {
                peripheralMTU = pMsgEvent->msg.exchangeMTUReq.clientRxMTU;
            }
            break;
        }

        default:
            break;
    }
}

/*********************************************************************
 * @fn      Peripheral_LinkEstablished
 *
 * @brief   Process link established.
 *
 * @param   pEvent - event to process
 *
 * @return  none
 */
static void Peripheral_LinkEstablished(gapRoleEvent_t *pEvent)
{
    gapEstLinkReqEvent_t *event = (gapEstLinkReqEvent_t *)pEvent;

    // See if already connected
    if(peripheralConnList.connHandle != GAP_CONNHANDLE_INIT)
    {
        GAPRole_TerminateLink(pEvent->linkCmpl.connectionHandle);
    }
    else
    {
        peripheralConnList.connHandle = event->connectionHandle;
        peripheralConnList.connInterval = event->connInterval;
        peripheralConnList.connSlaveLatency = event->connLatency;
        peripheralConnList.connTimeout = event->connTimeout;
        peripheralMTU =  ATT_MTU_SIZE;

        // Set timer for periodic event
        tmos_start_task(Peripheral_TaskID, SBP_PERIODIC_EVT, SBP_PERIODIC_EVT_PERIOD);

        // Set timer for param update event
        tmos_start_task(Peripheral_TaskID, SBP_PARAM_UPDATE_EVT, SBP_PARAM_UPDATE_DELAY);

        // Start read rssi
        tmos_start_task(Peripheral_TaskID, SBP_READ_RSSI_EVT, SBP_READ_RSSI_EVT_PERIOD);


        // Auto-connect to default target if configured
        if (DefaultTargetConfig_IsEnabled()) {
            BLE_Eth_AutoConnect();
        }
    }
}

/*********************************************************************
 * @fn      Peripheral_LinkTerminated
 *
 * @brief   Process link terminated.
 *
 * @param   pEvent - event to process
 *
 * @return  none
 */
static void Peripheral_LinkTerminated(gapRoleEvent_t *pEvent)
{
    gapTerminateLinkEvent_t *event = (gapTerminateLinkEvent_t *)pEvent;

    if(event->connectionHandle == peripheralConnList.connHandle)
    {
        peripheralConnList.connHandle = GAP_CONNHANDLE_INIT;
        peripheralConnList.connInterval = 0;
        peripheralConnList.connSlaveLatency = 0;
        peripheralConnList.connTimeout = 0;
        tmos_stop_task(Peripheral_TaskID, SBP_PERIODIC_EVT);
        tmos_stop_task(Peripheral_TaskID, SBP_READ_RSSI_EVT);

        // Restart advertising
        {
            uint8_t advertising_enable = TRUE;
            GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &advertising_enable);
        }
    }
    else
    {
    }
}

/*********************************************************************
 * @fn      peripheralRssiCB
 *
 * @brief   RSSI callback.
 *
 * @param   connHandle - connection handle
 * @param   rssi - RSSI
 *
 * @return  none
 */
static void peripheralRssiCB(uint16_t connHandle, int8_t rssi)
{
    (void)connHandle;
    (void)rssi;
}

/*********************************************************************
 * @fn      peripheralParamUpdateCB
 *
 * @brief   Parameter update complete callback
 *
 * @param   connHandle - connect handle
 *          connInterval - connect interval
 *          connSlaveLatency - connect slave latency
 *          connTimeout - connect timeout
 *
 * @return  none
 */
static void peripheralParamUpdateCB(uint16_t connHandle, uint16_t connInterval,
                                    uint16_t connSlaveLatency, uint16_t connTimeout)
{
    if(connHandle == peripheralConnList.connHandle)
    {
        peripheralConnList.connInterval = connInterval;
        peripheralConnList.connSlaveLatency = connSlaveLatency;
        peripheralConnList.connTimeout = connTimeout;

    }
    else
    {
    }
}

/*********************************************************************
 * @fn      peripheralStateNotificationCB
 *
 * @brief   Notification from the profile of a state change.
 *
 * @param   newState - new state
 *
 * @return  none
 */
static void peripheralStateNotificationCB(gapRole_States_t newState, gapRoleEvent_t *pEvent)
{
    switch(newState & GAPROLE_STATE_ADV_MASK)
    {
        case GAPROLE_STARTED:
            g_ble_led_state = 0;
            break;

        case GAPROLE_ADVERTISING:
            if(pEvent->gap.opcode == GAP_LINK_TERMINATED_EVENT)
            {
                Peripheral_LinkTerminated(pEvent);
                g_ble_led_state = 3;  // disconnected → red
            }
            else if(pEvent->gap.opcode == GAP_MAKE_DISCOVERABLE_DONE_EVENT)
            {
                g_ble_led_state = 1;  // advertising → blue
            }
            break;

        case GAPROLE_CONNECTED:
            if(pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT)
            {
                Peripheral_LinkEstablished(pEvent);
                g_ble_led_state = 2;  // connected → green
            }
            break;

        case GAPROLE_CONNECTED_ADV:
            if(pEvent->gap.opcode == GAP_MAKE_DISCOVERABLE_DONE_EVENT)
            {
            }
            break;

        case GAPROLE_WAITING:
            if(pEvent->gap.opcode == GAP_END_DISCOVERABLE_DONE_EVENT)
            {
            }
            else if(pEvent->gap.opcode == GAP_LINK_TERMINATED_EVENT)
            {
                Peripheral_LinkTerminated(pEvent);
                g_ble_led_state = 3;  // disconnected → red
            }
            else if(pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT)
            {
                if(pEvent->gap.hdr.status != SUCCESS)
                {
                }
                else
                {
                }
            }
            else
            {
            }
            break;

        case GAPROLE_ERROR:
            g_ble_led_state = 4;  // error → solid red
            break;

        default:
            break;
    }
}

/*********************************************************************
 * @fn      performPeriodicTask
 *
 * @brief   Perform a periodic application task. This function gets
 *          called every five seconds as a result of the SBP_PERIODIC_EVT
 *          TMOS event. In this example, the value of the third
 *          characteristic in the SimpleGATTProfile service is retrieved
 *          from the profile, and then copied into the value of the
 *          the fourth characteristic.
 *
 * @param   none
 *
 * @return  none
 */
static void performPeriodicTask(void)
{
    /* Process BLE-Eth ARP timeouts */
    if (BLE_Eth_PeriodicTick()) {
        tmos_set_event(Peripheral_TaskID, SBP_PROCESS_BLEETH_TX_EVT);
    }
}

/*********************************************************************
 * @fn      peripheralChar4Notify
 *
 * @brief   Prepare and send simpleProfileChar4 notification
 *
 * @param   pValue - data to notify
 *          len - length of data
 *
 * @return  none
 */
static void peripheralChar4Notify(uint8_t *pValue, uint16_t len)
{
    attHandleValueNoti_t noti;
    if(len > (peripheralMTU - 3))
    {
        return;
    }
    noti.len = len;
    noti.pValue = GATT_bm_alloc(peripheralConnList.connHandle, ATT_HANDLE_VALUE_NOTI, noti.len, NULL, 0);
    if(noti.pValue)
    {
        tmos_memcpy(noti.pValue, pValue, noti.len);
        if(simpleProfile_Notify(peripheralConnList.connHandle, &noti) != SUCCESS)
        {
            GATT_bm_free((gattMsg_t *)&noti, ATT_HANDLE_VALUE_NOTI);
        }
    }
}

/*********************************************************************
 * @fn      simpleProfileChangeCB
 *
 * @brief   Callback from SimpleBLEProfile indicating a value change
 *
 * @param   paramID - parameter ID of the value that was changed.
 *          pValue - pointer to data that was changed
 *          len - length of data
 *
 * @return  none
 */
static void simpleProfileChangeCB(uint8_t paramID, uint8_t *pValue, uint16_t len)
{
    switch(paramID)
    {
        case SIMPLEPROFILE_CHAR1:
        {
            uint8_t newValue[SIMPLEPROFILE_CHAR1_LEN];
            tmos_memcpy(newValue, pValue, len);
            break;
        }

        // case SIMPLEPROFILE_CHAR3:
        // {
        //     uint8_t newValue[SIMPLEPROFILE_CHAR3_LEN];
        //     tmos_memcpy(newValue, pValue, len);
        //     PRINT("profile ChangeCB CHAR3..\n");
        //     break;
        // }

        default:
            // should not reach here!
            break;
    }
}

/*********************************************************************
 * @fn      ble_usb_ServiceEvt
 *
 * @brief   ble usb service callback handler
 *
 * @return  none
 */
void ble_usb_ServiceEvt(uint16_t connection_handle, ble_usb_evt_t *p_evt)
{
    switch(p_evt->type)
    {
        case BLE_USB_EVT_TX_NOTI_DISABLED:
            break;
        case BLE_USB_EVT_TX_NOTI_ENABLED:
            break;
        case BLE_USB_EVT_BLE_DATA_RECIEVED:

            /* Check if this is a CH9329 command that should be handled locally.
             * CH9329 commands (0x01-0x08, 0x17, 0x18, 0x30) are processed by the
             * device and responses are sent back via BLE notification.
             * BLE-Eth commands (0x10-0x12, 0x1F) are handled by BLE_Eth_Parse. */
            if (p_evt->data.length >= 6 &&
                p_evt->data.p_data[0] == CH9329_FRAME_HEAD1 &&
                p_evt->data.p_data[1] == CH9329_FRAME_HEAD2 &&
                Peripheral_IsCh9329CommandByte(p_evt->data.p_data[3]))
            {
                /* CH9329 command from BLE — route response via BLE notification */
                Peripheral_SetBleCh9329Response(1);
                CH9329_DataParser((uint8_t *)p_evt->data.p_data, p_evt->data.length);
                Peripheral_SetBleCh9329Response(0);
            }
            else
            {
                /* Not a CH9329 command — try BLE-Eth protocol parser */
                BLE_Eth_Parse(p_evt->data.p_data, p_evt->data.length);
                tmos_set_event(Peripheral_TaskID, SBP_PROCESS_BLEETH_TX_EVT);

                /* Also feed into legacy CH9329 path for backward compatibility.
                 * The CH9329 parser is stateful and will only match its own commands. */
                if(Peripheral_IsCh9329ModeCommandFrame(p_evt->data.p_data, p_evt->data.length))
                {
                    CH9329_RxBuffer_Add((uint8_t *)p_evt->data.p_data, p_evt->data.length);
                }
                else if(RingMemWrite(&RingMemBLE, (uint8_t *)p_evt->data.p_data, p_evt->data.length) != SUCCESS)
                {
                }
                else
                {
                    tmos_set_event(Peripheral_TaskID, SBP_PROCESS_BLEDATA_EVT);
                }
            }

            break;
        default:
            break;
    }
}

/*********************************************************************
 * @fn      ble_eth_event_handler
 *
 * @brief   BLE-Eth protocol event callback. Handles tunnel events from
 *          the BLE-Eth protocol layer and bridges data to/from USB.
 *
 * @return  none
 */
static void ble_eth_event_handler(ble_eth_evt_t *evt)
{
    switch (evt->type) {
    case BLE_ETH_EVT_DATA_READY:
        /* Reassembled TCP data from BLE host -> wrap in Ethernet/IPv4/TCP -> ECM */
        BLE_Eth_SendDataToEth(evt->info.data.conn_id,
                              evt->info.data.data, evt->info.data.len);
        break;

    case BLE_ETH_EVT_CONN_OPENED:
        break;

    case BLE_ETH_EVT_CONN_CLOSED:
        break;

    case BLE_ETH_EVT_FRAME_RX:
        /* Raw frame received (for debugging or advanced handling) */
        break;

    case BLE_ETH_EVT_ERROR:
        break;
    }
}

#if USB_WORK_MODE_BRIDGE_ECM
/*********************************************************************
 * @fn      BLE_Eth_PushFromBridge
 *
 * @brief   Push data from the ECM bridge (target side) back to BLE host.
 *          This is the reverse path: target TCP data -> BLE notification.
 *          Called by ECM_Bridge_TargetRxDone or USB data handlers.
 *
 * @param   conn_id  Connection ID (must match an active tunnel).
 * @param   data     TCP payload data from target.
 * @param   len      Data length.
 *
 * @return  none
 */
void BLE_Eth_PushFromBridge(uint8_t conn_id, const uint8_t *data, uint16_t len)
{
    if (BLE_Eth_PushTargetData(conn_id, data, len)) {
        tmos_set_event(Peripheral_TaskID, SBP_PROCESS_BLEETH_TX_EVT);
    }
}
#endif /* USB_WORK_MODE_BRIDGE_ECM */

/*********************************************************************
 * @fn      Peripheral_ScheduleSystemReset
 *
 * @brief   Schedule a system reset after a delay to allow BLE notifications
 *          to be transmitted.
 *
 * @param   delay_ms - Delay in milliseconds before reset (min 100ms)
 *
 * @return  none
 */
void Peripheral_ScheduleSystemReset(uint16_t delay_ms)
{
    /* TMOS timer tick is 625us, convert ms to ticks */
    tmosTimer ticks = (delay_ms * 1000) / 625;
    if (ticks < 160) ticks = 160;  // Minimum 100ms
    (void)tmos_start_task(Peripheral_TaskID, SBP_SYSTEM_RESET_EVT, ticks);
}

/*********************************************************************
 * @fn      app_usb_notify
 *
 * @brief   app usb service callback handler
 *
 * @return  Success or Failure
 */
uint8_t app_usb_notify(uint8_t *SendBuf, uint8_t l)
{
    uint8_t result = FAILURE;

    static attHandleValueNoti_t noti;

    if (l > (peripheralMTU - 3)) {
        return bleInvalidMtuSize;
    }
    noti.len = l;
    noti.pValue = GATT_bm_alloc(peripheralConnList.connHandle, ATT_HANDLE_VALUE_NOTI, noti.len, NULL, 0);
    if(noti.pValue != NULL)
    {
        tmos_memcpy(noti.pValue, SendBuf, noti.len);
        result = ble_usb_notify(peripheralConnList.connHandle, &noti, 0);
        if(result != SUCCESS)
        {
            GATT_bm_free((gattMsg_t *)&noti, ATT_HANDLE_VALUE_NOTI);
        }
    }
    return result;
}

/*********************************************************************
 * @fn      RingMem_Protection
 *
 * @brief   Protect RingMem
 *
 * @return  Success or Failure
 */
int RingMem_Protection(uint8_t enable)
{
    if(enable)
    {
        __disable_irq();
    }
    else
    {
        __enable_irq();
    }
}

/*********************************************************************
 * @fn      Peripheral_SecurityCommand
 *
 * @brief   Handle BLE security commands (pairing, bonding, etc.)
 *          Called from CH9329 command dispatcher for CMD_BLE_SECURITY (0x22)
 *
 * @param   addr     - Device address
 * @param   cmd_code - Command code (0x22)
 * @param   pdata    - Pointer to command data (sub-command byte)
 * @param   data_len - Length of command data
 *
 * @return  none
 */
void Peripheral_SecurityCommand(uint8_t addr, uint8_t cmd_code, uint8_t *pdata, uint8_t data_len)
{
    uint8_t response[8];
    uint8_t status = STATUS_SUCCESS;

    if (data_len < 1) {
        status = STATUS_ERR_PARAM;
        CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
        return;
    }

    uint8_t sub_cmd = pdata[0];

    switch (sub_cmd) {
        case 0x01:  // Open pairing window
            // TODO: Implement pairing window opening
            response[0] = STATUS_SUCCESS;
            response[1] = 0x00;  // Remaining seconds (u16 LE)
            response[2] = 0x00;
            CH9329_SendResponse(addr, cmd_code, response, 3, 1);
            break;

        case 0x02:  // Erase all bonds
            // TODO: Implement bond erasure
            response[0] = STATUS_SUCCESS;
            CH9329_SendResponse(addr, cmd_code, response, 1, 1);
            break;

        case 0x03:  // Bond count
            // TODO: Implement bond count query
            response[0] = STATUS_SUCCESS;
            response[1] = 0x00;  // Number of stored bonds
            CH9329_SendResponse(addr, cmd_code, response, 2, 1);
            break;

        default:
            status = STATUS_ERR_PARAM;
            CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
            break;
    }
}

/*********************************************************************
 * @fn      Peripheral_BleAdvParamCommand
 *
 * @brief   Handle BLE advertising parameter commands
 *          Called from CH9329 command dispatcher for CMD_BLE_ADV_PARAM (0x19)
 *
 * @param   addr     - Device address
 * @param   cmd_code - Command code (0x19)
 * @param   pdata    - Pointer to command data
 * @param   data_len - Length of command data
 *
 * @return  none
 *
 * Command format:
 *   pdata[0] = sub-command: 0x00=get, 0x01=set
 *   pdata[1] = param type:
 *              0x01 = channel map (bit0=CH37, bit1=CH38, bit2=CH39)
 *              0x02 = TX power (LL_TX_POWER_xxx values)
 *   pdata[2] = value (set only)
 *
 * Response format:
 *   Success: [param_type, current_value]
 *   Error:   [error_code]
 */
void Peripheral_BleAdvParamCommand(uint8_t addr, uint8_t cmd_code, uint8_t *pdata, uint8_t data_len)
{
    uint8_t response[4];
    uint8_t status = STATUS_SUCCESS;

    if (data_len < 2) {
        status = STATUS_ERR_PARAM;
        CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
        return;
    }

    uint8_t sub_cmd = pdata[0];
    uint8_t param_type = pdata[1];

    switch (param_type) {
        case 0x01:  // Channel map control
        {
            switch (sub_cmd) {
                case 0x00:  // Get current channel map
                {
                    uint8_t channel_map = 0;
                    GAPRole_GetParameter(GAPROLE_ADV_CHANNEL_MAP, &channel_map);
                    response[0] = param_type;      // param type
                    response[1] = channel_map & 0x07;  // current value (3 bits)
                    CH9329_SendResponse(addr, cmd_code, response, 2, 1);
                    break;
                }

                case 0x01:  // Set channel map
                {
                    if (data_len < 3) {
                        status = STATUS_ERR_PARAM;
                        CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
                        return;
                    }

                    // Check if already connected - cannot change adv params while connected
                    if (Peripheral_GetConnHandle() != GAP_CONNHANDLE_INIT) {
                        status = STATUS_ERR_FRAME;  // Device is connected, cannot change adv params
                        CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
                        return;
                    }

                    uint8_t new_channel_map = pdata[2] & 0x07;  // Only use lower 3 bits
                    // Ensure at least one channel is enabled
                    if (new_channel_map == 0) {
                        new_channel_map = 0x07;  // Default to all channels
                    }

                    // Stop advertising first
                    uint8_t adv_enable = FALSE;
                    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &adv_enable);

                    // Set new channel map (BLE stack handles timing internally)
                    GAPRole_SetParameter(GAPROLE_ADV_CHANNEL_MAP, sizeof(uint8_t), &new_channel_map);

                    // Restart advertising
                    adv_enable = TRUE;
                    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &adv_enable);

                    response[0] = param_type;      // param type
                    response[1] = new_channel_map; // new value
                    CH9329_SendResponse(addr, cmd_code, response, 2, 1);
                    break;
                }

                default:
                    status = STATUS_ERR_PARAM;
                    CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
                    break;
            }
            break;
        }

        case 0x02:  // TX power control
        {
            switch (sub_cmd) {
                case 0x00:  // Get current TX power
                {
                    // Return the current TX power value (tracked at runtime)
                    response[0] = param_type;      // param type
                    response[1] = g_current_tx_power;   // current value
                    CH9329_SendResponse(addr, cmd_code, response, 2, 1);
                    break;
                }

                case 0x01:  // Set TX power
                {
                    if (data_len < 3) {
                        status = STATUS_ERR_PARAM;
                        CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
                        return;
                    }

                    // Check if already connected - cannot change TX power while connected
                    if (Peripheral_GetConnHandle() != GAP_CONNHANDLE_INIT) {
                        status = STATUS_ERR_FRAME;  // Device is connected, cannot change TX power
                        CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
                        return;
                    }

                    uint8_t new_power = pdata[2];

                    // Validate power value - must be one of the defined levels
                    uint8_t valid = 0;
                    switch (new_power) {
                        case LL_TX_POWEER_MINUS_18_DBM:  // 0x01
                        case LL_TX_POWEER_MINUS_10_DBM:  // 0x03
                        case LL_TX_POWEER_MINUS_5_DBM:   // 0x05
                        case LL_TX_POWEER_MINUS_3_DBM:   // 0x07
                        case LL_TX_POWEER_0_DBM:         // 0x09
                        case LL_TX_POWEER_1_DBM:         // 0x0B
                        case LL_TX_POWEER_2_DBM:         // 0x0D
                        case LL_TX_POWEER_3_DBM:         // 0x11
                        case LL_TX_POWEER_4_DBM:         // 0x15
                        case LL_TX_POWEER_5_DBM:         // 0x1B
                        case LL_TX_POWEER_6_DBM:         // 0x25
                        case LL_TX_POWEER_7_DBM:         // 0x3F
                            valid = 1;
                            break;
                        default:
                            valid = 0;
                            break;
                    }

                    if (!valid) {
                        status = STATUS_ERR_PARAM;
                        CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
                        return;
                    }

                    // Set new TX power level
                    LL_SetTxPowerLevel(new_power);
                    g_current_tx_power = new_power;  // Track the current value

                    response[0] = param_type;      // param type
                    response[1] = new_power;       // new value
                    CH9329_SendResponse(addr, cmd_code, response, 2, 1);
                    break;
                }

                default:
                    status = STATUS_ERR_PARAM;
                    CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
                    break;
            }
            break;
        }

        case 0x03:  // BLE Test Mode (using LL Test API)
        {
            switch (sub_cmd) {
                case 0x00:  // Start test mode - continuous TX at selected frequency
                {
                    bStatus_t ret;
                    uint8_t tx_channel;
                    uint8_t tx_power;

                    // Check if already connected
                    if (Peripheral_GetConnHandle() != GAP_CONNHANDLE_INIT) {
                        status = STATUS_ERR_FRAME;
                        CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
                        return;
                    }

                    // Check frequency band and power parameters (pdata[2], pdata[3]) - required for start
                    if (data_len < 4) {
                        status = STATUS_ERR_PARAM;
                        CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
                        return;
                    }

                    // Map frequency band to TX channel
                    // Test channel formula: freq = 2402 + 2 * channel (MHz)
                    // 0 = Low (CH37, 2402 MHz) -> test channel 0
                    // 1 = Medium (2440 MHz) -> test channel 19
                    // 2 = High (CH39, 2480 MHz) -> test channel 39
                    switch (pdata[2]) {
                        case 0:
                            tx_channel = 0;   // 2402 MHz
                            break;
                        case 1:
                            tx_channel = 19;  // 2440 MHz
                            break;
                        case 2:
                            tx_channel = 39;  // 2480 MHz
                            break;
                        default:
                            status = STATUS_ERR_PARAM;
                            CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
                            return;
                    }

                    // Validate and set TX power (pdata[3])
                    // Power value must be one of the defined levels
                    tx_power = pdata[3];
                    switch (tx_power) {
                        case LL_TX_POWEER_MINUS_18_DBM:  // 0x01 = -18 dBm
                        case LL_TX_POWEER_MINUS_10_DBM:  // 0x03 = -10 dBm
                        case LL_TX_POWEER_MINUS_5_DBM:   // 0x05 = -5 dBm
                        case LL_TX_POWEER_MINUS_3_DBM:   // 0x07 = -3 dBm
                        case LL_TX_POWEER_0_DBM:         // 0x09 = 0 dBm
                        case LL_TX_POWEER_1_DBM:         // 0x0B = +1 dBm
                        case LL_TX_POWEER_2_DBM:         // 0x0D = +2 dBm
                        case LL_TX_POWEER_3_DBM:         // 0x11 = +3 dBm
                        case LL_TX_POWEER_4_DBM:         // 0x15 = +4 dBm
                        case LL_TX_POWEER_5_DBM:         // 0x1B = +5 dBm
                        case LL_TX_POWEER_6_DBM:         // 0x25 = +6 dBm
                        case LL_TX_POWEER_7_DBM:         // 0x3F = +7 dBm
                            break;  // Valid power level
                        default:
                            status = STATUS_ERR_PARAM;
                            CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
                            return;
                    }

                    // Step 1: Stop advertising and wait for it to fully stop
                    uint8_t adv_enable = FALSE;
                    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &adv_enable);

                    // Step 2: Set TX power (user specified)
                    LL_SetTxPowerLevel(tx_power);
                    g_current_tx_power = tx_power;

                    // Step 3: Start transmitter test with optimized parameters
                    // Using BLE 5.x test command (opcode 0x2034)
                    // Parameters: TX_Channel, Test_Data_Length, Packet_Payload, PHY
                    uint8_t test_params[4];
                    test_params[0] = tx_channel;  // TX_Channel: 0/1/2
                    test_params[1] = 255;         // Test_Data_Length: 255 bytes (max for BLE 5.x)
                    test_params[2] = 4;           // Packet_Payload: 4 = 11111111 (continuous carrier)
                    test_params[3] = 1;           // PHY: 1 = LE 1M PHY

                    ret = API_LE_TransmitterTestCmd(test_params, 0x2034);

                    if (ret != 0) {
                        // BLE 5.x command failed, try BLE 4.x command (opcode 0x201E)
                        // For BLE 4.x, max length is 37 bytes
                        uint8_t test_params_v4[3];
                        test_params_v4[0] = tx_channel;  // TX_Channel: 0/1/2
                        test_params_v4[1] = 37;          // Test_Data_Length: 37 bytes (max for BLE 4.x)
                        test_params_v4[2] = 4;           // Packet_Payload: 4 = 11111111 (continuous carrier)
                        ret = API_LE_TransmitterTestCmd(test_params_v4, 0x201E);
                    }

                    // Set test mode flag
                    g_test_mode_active = 1;

                    response[0] = param_type;
                    response[1] = (ret == 0) ? 0x01 : 0x02;  // 0x01=success, 0x02=test started with fallback
                    response[2] = ret;   // Return status code
                    response[3] = tx_power;  // Return actual TX power used
                    CH9329_SendResponse(addr, cmd_code, response, 4, 1);
                    break;
                }

                case 0x01:  // Stop test mode
                {
                    bStatus_t ret;

                    // Step 1: End transmitter test
                    ret = API_LE_TestEndCmd();

                    // Step 2: Restore default TX power
                    LL_SetTxPowerLevel(BLE_TX_POWER);
                    g_current_tx_power = BLE_TX_POWER;

                    // Step 3: Restart advertising
                    uint8_t adv_enable = TRUE;
                    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &adv_enable);

                    // Step 4: Clear test mode flag
                    g_test_mode_active = 0;

                    response[0] = param_type;
                    response[1] = 0x00;  // test stopped
                    response[2] = ret;   // Return status code
                    CH9329_SendResponse(addr, cmd_code, response, 3, 1);
                    break;
                }

                default:
                    status = STATUS_ERR_PARAM;
                    CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
                    break;
            }
            break;
        }

        default:
            status = STATUS_ERR_PARAM;
            CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
            break;
    }
}

/*********************************************************************
 * @fn      Peripheral_UpdateBLEAdvertisingData
 *
 * @brief   Update BLE advertising data and scan response with current device name
 *
 * @details This function should be called after the device name is changed
 *          to ensure the new name appears in BLE advertisements.
 *          It stops advertising, updates the data, and restarts advertising.
 *
 * @param   none
 *
 * @return  none
 */
void Peripheral_UpdateBLEAdvertisingData(void)
{
    uint8_t advertising_enable = FALSE;
    uint8_t nameLen;
    uint8_t deviceNameLen;

    // Stop advertising first
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &advertising_enable);

    // Calculate name length
    nameLen = strlen(g_ble_name.name);
    deviceNameLen = sizeof(attDeviceName) - 1;

    if(nameLen > 20) nameLen = 20;
    if(deviceNameLen > 20) deviceNameLen = 20;

    // Update GAP device name attribute
    // Pass actual name length (not buffer size) to avoid trailing zeros
    GGS_SetParameter(GGS_DEVICE_NAME_ATT, strlen(g_ble_name.name) + 1, (void *)g_ble_name.name);

    // Rebuild advertising data with new name
    advertDataLen = 7; // Reset to initial length (flags + UUID)
    advertData[advertDataLen++] = nameLen + 1;
    advertData[advertDataLen++] = GAP_ADTYPE_LOCAL_NAME_COMPLETE;
    tmos_memcpy(&advertData[advertDataLen], g_ble_name.name, nameLen);
    advertDataLen += nameLen;

    // Rebuild scan response data with new name
    scanRspDataLen = 0;
    scanRspData[scanRspDataLen++] = nameLen + 1;
    scanRspData[scanRspDataLen++] = GAP_ADTYPE_LOCAL_NAME_COMPLETE;
    tmos_memcpy(&scanRspData[scanRspDataLen], g_ble_name.name, nameLen);
    scanRspDataLen += nameLen;

    // Add connection interval range
    scanRspData[scanRspDataLen++] = 0x05;
    scanRspData[scanRspDataLen++] = GAP_ADTYPE_SLAVE_CONN_INTERVAL_RANGE;
    scanRspData[scanRspDataLen++] = LO_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL);
    scanRspData[scanRspDataLen++] = HI_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL);
    scanRspData[scanRspDataLen++] = LO_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL);
    scanRspData[scanRspDataLen++] = HI_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL);

    // Add power level
    scanRspData[scanRspDataLen++] = 0x02;
    scanRspData[scanRspDataLen++] = GAP_ADTYPE_POWER_LEVEL;
    scanRspData[scanRspDataLen++] = 0;

    // Update GAP parameters
    GAPRole_SetParameter(GAPROLE_SCAN_RSP_DATA, scanRspDataLen, scanRspData);
    GAPRole_SetParameter(GAPROLE_ADVERT_DATA, advertDataLen, advertData);

    // Restart advertising
    advertising_enable = TRUE;
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &advertising_enable);
}

#endif /* ENABLE_BLE */

/*********************************************************************
*********************************************************************/
