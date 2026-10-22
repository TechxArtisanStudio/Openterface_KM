#include "ch32v20x_usbfs_device.h"
#include "usbd_composite_km.h"
#include "UART.h"
#include "usb_lib.h"
#include "usb_desc.h"
#include "usb_prop.h"
#include "usb_pwr.h"
#include "peripheral.h"
#include "include/keyboard_handler.h"
#include "include/mouse_handler.h"
#include "sd_switch.h"
#include "CH32_TEMP.h"
#include "km_ring.h"
#include "Version_selection.h"
#include "include/usb_mode_config.h"
#include "include/ble_name_config.h"
#include "include/ecm_ip_config.h"
#include "include/ecm_bridge.h"
#include "include/acm_bridge.h"

#if USB_WORK_MODE_COMPILES_BRIDGE_ACM || USB_WORK_MODE_COMPILES_BRIDGE_ECM || USB_WORK_MODE_HAS_HOST_ACM_AND_ECM || USB_WORK_MODE_HAS_HOST_CDC_ACM
/* External function from usb_endp.c - validates CH9329 command frames */
extern uint8_t USBD_IsCh9329ModeCommandFrame(const uint8_t *buf, uint16_t len);
#endif

/* External variable from ch32v20x_usbfs_device.c - needs to be reset after STOP mode wakeup */
extern volatile uint8_t USBFS_DevSleepStatus;
/*******************************************************************************/
/* Global Variable Definition */

/*********************************************************************
 * @fn      USB_Sleep_Wakeup_CFG
 *
 * @brief   Configure USB wake up mode
 *
 * @return  none
 */
void USB_Sleep_Wakeup_CFG( void )
{
    EXTI_InitTypeDef EXTI_InitStructure = { 0 };

    EXTI_InitStructure.EXTI_Line = EXTI_Line20;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Event;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init( &EXTI_InitStructure );
}

/*********************************************************************
 * @fn      MCU_Sleep_Wakeup_Operate
 *
 * @brief   Perform sleep operation
 *
 * @return  none
 */
void MCU_Sleep_Wakeup_Operate( void )
{
    __disable_irq();
    EXTI_ClearFlag( EXTI_Line12 | EXTI_Line13 | EXTI_Line14 | EXTI_Line15 );
    EXTI_ClearFlag( EXTI_Line4 | EXTI_Line5 | EXTI_Line6 | EXTI_Line7 );

    PWR_EnterSTOPMode(PWR_Regulator_LowPower,PWR_STOPEntry_WFE);
    SystemInit();
    SystemCoreClockUpdate();
    USBFS_RCC_Init();

    /* Fully re-initialize USBFS (all registers lost during STOP mode) */
    USBFS_Device_Init(ENABLE);

    /* Reset sleep status to prevent immediate re-entry into STOP mode.
     * Without this, USBFS_DevSleepStatus remains 0x03 after wakeup,
     * causing the next SUSPEND interrupt to trigger STOP mode again,
     * leading to enumeration failures. */
    USBFS_DevSleepStatus = 0;

    if( EXTI_GetFlagStatus( EXTI_Line12 | EXTI_Line13 | EXTI_Line14 | EXTI_Line15 ) != RESET  )
    {
        EXTI_ClearFlag( EXTI_Line12 | EXTI_Line13 | EXTI_Line14 | EXTI_Line15 );
        USBFS_Send_Resume( );
    }
    else if( EXTI_GetFlagStatus( EXTI_Line4 | EXTI_Line5 | EXTI_Line6 | EXTI_Line7 ) != RESET )
    {
        EXTI_ClearFlag( EXTI_Line4 | EXTI_Line5 | EXTI_Line6 | EXTI_Line7 );
        USBFS_Send_Resume( );
    }
    __enable_irq( );
}

 extern uint8_t sd_card_channel_state; 

/* CH9329_SendResponse enqueues the response packet instead of sending directly.
 * When BLE CH9329 response routing is enabled (via Peripheral_SetBleCh9329Response),
 * the response is sent via BLE notification instead of USB EP3 queue. */
void CH9329_SendResponse(uint8_t addr, uint8_t cmd_code, uint8_t* pdata,
                         uint8_t len, uint8_t resp_mode)
{
    uint8_t packet[64];
    uint8_t index = 0;

    /* Debug output removed - was causing latency in hot path */
    /* PRINT("CH9329_SendResponse: cmd=0x%02X, len=%d, ble_resp=%d\n", cmd_code, len, Peripheral_GetBleCh9329Response()); */

    if (len > 58) len = 58; // max 58 bytes payload (64 - 6 header+checksum)
    packet[index++] = 0x57; // FRAME_HEAD1
    packet[index++] = 0xAB; // FRAME_HEAD2
    packet[index++] = addr;
    packet[index++] = cmd_code | (resp_mode ? 0x80 : 0xC0);
    packet[index++] = len;

    if (pdata && len > 0) {
        memcpy(&packet[index], pdata, len);
        index += len;
    }

    uint8_t checksum = 0;
    for (uint8_t i = 0; i < index; i++) checksum += packet[i];
    packet[index++] = checksum;

#ifdef ENABLE_BLE
    /* If response is for a BLE CH9329 command, send via BLE notification */
    if (Peripheral_GetBleCh9329Response())
    {
        app_usb_notify(packet, index);
        return;
    }
#endif

    Queue_Push_Response(packet, index);
}
//      Dedicated "Get Info" Command Response
void CH9329_Cmd_GetInfo_Reply(uint8_t addr)
{
    uint8_t data[8] = {0};

    /* Protocol requires a fixed 8-byte payload */

    /* Hardware selector: mirrors the board version (1 = KVM-GO,
     * 2 = MiniKVM, 3 = KeyMod). */
    data[0] = version;
    /* Device connection / enumeration status */
    if (USBFS_Connect == 1) {
        data[1] = 0x01;   // Connected (enumerated)
    } else {
        data[1] = 0x00;   // Not connected
    }
    data[2] = Keyboard_GetLEDStatus() & 0x07; // bit2-bit0: LED status
    data[3] = 0x06; // bit6 placeholder
    data[4] = 0x00; // bit5 placeholder
    data[5] = 0x00; // bit4 placeholder
    data[6] = 0x00; // bit3 placeholder
    data[7] = 0x00; // bit3 placeholder
    CH9329_SendResponse(addr, CMD_GET_INFO , data, 8, 1);
}

static void CH9329_Cmd_USBWorkMode(uint8_t addr, uint8_t cmd_code, uint8_t *pdata, uint8_t data_len)
{
    uint8_t response[12];
    uint16_t flags;
    uint32_t capability_mask;
    uint8_t status = STATUS_SUCCESS;

    if (data_len > 0) {
        switch (pdata[0]) {
        case 0x00:
            break;
        case 0x01:
            if (data_len < 2) {
                status = STATUS_ERR_PARAM;
            }
            else {
                uint16_t requested_flags = 0;
                if (data_len >= 4) {
                    requested_flags = (uint16_t)pdata[2] | ((uint16_t)pdata[3] << 8);
                }
                if (!USB_ModeConfig_Write(pdata[1], requested_flags)) {
                    status = STATUS_ERR_PARAM;
                }
            }
            break;
        case 0x02:
            if (!USB_ModeConfig_Clear()) {
                status = STATUS_ERR_FRAME;
            }
            break;
        case 0x03:
            CH9329_SendResponse(addr, cmd_code, &status, 1, 1);
            USB_ModeConfig_Reboot();
            return;
        default:
            status = STATUS_ERR_PARAM;
            break;
        }
    }

    flags = USB_ModeConfig_GetFlags();
    capability_mask = USB_ModeConfig_GetCapabilityMask();
    response[0] = status;
    response[1] = USB_ModeConfig_GetCompiledMode();
    response[2] = USB_ModeConfig_GetActiveMode();
    response[3] = USB_ModeConfig_GetStoredMode();
    response[4] = USB_ModeConfig_GetStatus();
    response[5] = (uint8_t)flags;
    response[6] = (uint8_t)(flags >> 8);
    response[7] = (uint8_t)capability_mask;
    response[8] = (uint8_t)(capability_mask >> 8);
    response[9] = (uint8_t)(capability_mask >> 16);
    response[10] = (uint8_t)(capability_mask >> 24);
    response[11] = 0;
    CH9329_SendResponse(addr, cmd_code, response, sizeof(response), 1);
}


// -------------------- CH9329 Command Dispatcher --------------------
/*********************************************************************
 * @fn      CH9329_DispatchCommand
 *
 * @brief   Dispatch and handle CH9329 commands
 *
 * @param   addr - Device address
 * @param   cmd_code - Command code
 * @param   pdata - Pointer to command data
 * @param   data_len - Length of command data
 *
 * @return  none
 */
void CH9329_DispatchCommand(uint8_t addr, uint8_t cmd_code, uint8_t* pdata, uint8_t data_len)
{
    switch (cmd_code)
    {   
        //cmd=01,Obtain information such as chip version
        case CMD_GET_INFO:
            CH9329_Cmd_GetInfo_Reply(addr);
            break;
        //cmd=02,Send USB keyboard regular data
        case CMD_SEND_KB_GENERAL_DATA:
        {
            uint8_t st;
            if (data_len != 8) {
                st = STATUS_ERR_PARAM;
                CH9329_SendResponse(addr, cmd_code, &st, 1, 0);
            } else {
                Keyboard_HandleData(addr, cmd_code, pdata, data_len);
                st = STATUS_SUCCESS;
                CH9329_SendResponse(addr, cmd_code, &st, 1, 1);
            }
        }
        break;
        //cmd=03,Send USB media keyboard data
        case CMD_SEND_KB_MEDIA_DATA:
        {
            uint8_t st;
            if (data_len < 2) {
                st = STATUS_ERR_PARAM;
                CH9329_SendResponse(addr, cmd_code, &st, 1, 0);
            } else {
                Keyboard_HandleMediaData(addr, cmd_code, pdata, data_len);
                st = STATUS_SUCCESS;
                CH9329_SendResponse(addr, cmd_code, &st, 1, 1);
            }
        }
        break;
        //cmd=04,Send USB absolute mouse data
        case CMD_SEND_MS_ABS_DATA:
        {
            uint8_t st;
            if (data_len < 7) {
                st = STATUS_ERR_PARAM;
                CH9329_SendResponse(addr, cmd_code, &st, 1, 0);
            } else {
                Mouse_HandleAbsoluteData(addr, cmd_code, pdata, data_len);
                st = STATUS_SUCCESS;
                CH9329_SendResponse(addr, cmd_code, &st, 1, 1);
            }
        }
        break;
        //cmd=05,Send USB relative mouse data
        case CMD_SEND_MS_REL_DATA:
        {
            uint8_t st;
            if (data_len < 5) {
                st = STATUS_ERR_PARAM;
                CH9329_SendResponse(addr, cmd_code, &st, 1, 0);
            } else {
                Mouse_HandleRelativeData(addr, cmd_code, pdata, data_len);
                st = STATUS_SUCCESS;
                CH9329_SendResponse(addr, cmd_code, &st, 1, 1);
            }
        }
        break;
        //cmd=17,Switch and return SD card status
        case CMD_SD_SWITCH:
            SD_USB_Switch(addr, cmd_code, pdata, data_len);
            break;
        //cmd=18,Return temperture
        case CMD_DS18B20_GET_TEMP:
            // Always pass length = 5 to DS18B20 command handler
            Temp_Command(addr, cmd_code, pdata, 5);
            break;
        case CMD_USB_WORK_MODE:
            CH9329_Cmd_USBWorkMode(addr, cmd_code, pdata, data_len);
            break;
        //cmd=22,BLE security control (pairing window / bond management)
        case CMD_BLE_SECURITY:
#ifdef ENABLE_BLE
            Peripheral_SecurityCommand(addr, cmd_code, pdata, data_len);
#else
        {
            uint8_t err = STATUS_ERR_CMD;
            CH9329_SendResponse(addr, cmd_code, &err, 1, 0);
        }
#endif
            break;
        //cmd=0x19,BLE advertising parameter control (get/set channel map)
        case CMD_BLE_ADV_PARAM:
#ifdef ENABLE_BLE
            Peripheral_BleAdvParamCommand(addr, cmd_code, pdata, data_len);
#else
        {
            uint8_t err = STATUS_ERR_CMD;
            CH9329_SendResponse(addr, cmd_code, &err, 1, 0);
        }
#endif
            break;
        //cmd=0x23,BLE name configuration (set name / reset to factory)
        case CMD_BLE_SET_NAME:
#if (ENABLE_BLE_NAME_CONFIG == 1)
            {
                uint8_t status = STATUS_SUCCESS;
                char name[21] = {0};

                /* Debug output removed - was causing latency in command path */
                /* PRINT("CMD_BLE_SET_NAME reached! data_len=%d\n", data_len); */

                if (data_len == 0 || pdata[0] == 0x00) {
                    // Empty payload or 0x00 is not allowed for setting name
                    status = STATUS_ERR_PARAM;
                }
                else {  // Set new name
                    // Safely copy name (max 20 bytes + null terminator)
                    uint8_t copy_len = (data_len > 20) ? 20 : data_len;
                    memcpy(name, pdata, copy_len);
                    name[copy_len] = '\0';  // Ensure null termination

                    // Perform Flash write BEFORE sending response
                    BLE_NameConfig_Status_t set_status = BLE_NameConfig_Set(name);
                    switch (set_status) {
                        case BLE_NAME_OK:
                            status = STATUS_SUCCESS;
                            break;
                        case BLE_NAME_ERR_NULL_NAME:
                        case BLE_NAME_ERR_INVALID_LENGTH:
                            status = STATUS_ERR_PARAM;
                            break;
                        case BLE_NAME_ERR_FLASH_ERASE:
                            status = STATUS_ERR_FLASH_ERASE;
                            break;
                        case BLE_NAME_ERR_FLASH_PROGRAM:
                            status = STATUS_ERR_FLASH_PROGRAM;
                            break;
                        case BLE_NAME_ERR_FLASH_VERIFY:
                            status = STATUS_ERR_FLASH_VERIFY;
                            break;
                    }
                }

                // Send response AFTER Flash operation with appropriate error code
                // Use resp_mode=1 (0x80) for success, resp_mode=0 (0xC0) for error
                uint8_t resp_mode = (status == STATUS_SUCCESS) ? 1 : 0;
                CH9329_SendResponse(addr, cmd_code, &status, 1, resp_mode);

                // Update advertising data and reboot if name changed successfully
                if (status == STATUS_SUCCESS) {
                    Peripheral_UpdateBLEAdvertisingData();
                    // Give BLE stack time to notify, then reboot
                    Peripheral_ScheduleSystemReset(500);
                }
            }
#else
        {
            uint8_t err = STATUS_ERR_CMD;
            CH9329_SendResponse(addr, cmd_code, &err, 1, 0);
        }
#endif
            break;
        case CMD_SYSTEM_RESET:
            {
                uint8_t status = STATUS_SUCCESS;

#ifdef ENABLE_BLE
                // Check if payload contains 0x00 (reset to default name)
                if (data_len > 0 && pdata[0] == 0x00) {
                    // Reset BLE name to factory default BEFORE sending response
                    BLE_NameConfig_Status_t reset_status = BLE_NameConfig_Reset();
                    if (reset_status == BLE_NAME_OK) {
                        status = STATUS_SUCCESS;
                        Peripheral_UpdateBLEAdvertisingData();
                    } else {
                        status = STATUS_ERR_FLASH_ERASE;
                    }
                }
#endif

                // Send response AFTER Flash operation with appropriate error code
                uint8_t resp_mode = (status == STATUS_SUCCESS) ? 1 : 0;
                CH9329_SendResponse(addr, cmd_code, &status, 1, resp_mode);
#ifdef ENABLE_BLE
                // Schedule reset after 500ms to allow BLE notification to be sent
                Peripheral_ScheduleSystemReset(500);
#else
                // No BLE, reset immediately
                NVIC_SystemReset();
#endif
            }
            break;
        //cmd=08,Get parameter configuration(Only used for matching with ch9329)
        case CMD_GET_PARA_CFG:
        {
            uint8_t response[] = {
                0x80, 0x80, 0x00, 0x00, 0x01, 0xC2, 
                0x00, 0x08, 0x00, 0x00, 0x03, 0x86, 
                0x1A, 0x29, 0xE1, 0x00, 0x00, 0x00, 
                0x01, 0x00, 0x0D, 0x00, 0x00, 0x00, 
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00
            };

            // Send parameter configuration response
            CH9329_SendResponse(addr, cmd_code, response, sizeof(response), 1);
        }
        break;

        // ECM IP configuration commands (0x26/0x27)
        case CMD_SET_ECM_IP_CONFIG:
        {
#if USB_WORK_MODE_COMPILES_BRIDGE_ECM
            uint8_t status = STATUS_SUCCESS;
            bool send_response_now = true;

            if (data_len < 1) {
                status = STATUS_ERR_PARAM;
                send_response_now = false;
            } else {
                uint8_t sub_cmd = pdata[0];

                switch (sub_cmd) {
                    case 0x00: // Set All
                        if (data_len != 21) { // 1 byte sub_cmd + 20 bytes data
                            status = STATUS_ERR_PARAM;
                            send_response_now = false;
                        } else {
                            ECM_IPConfig_t config = {
                                .record_version = ECM_IP_CONFIG_VERSION,
                                .host_ip = { pdata[1], pdata[2], pdata[3], pdata[4] },
                                .dev_ip = { pdata[5], pdata[6], pdata[7], pdata[8] },
                                .subnet_mask = { pdata[9], pdata[10], pdata[11], pdata[12] },
                                .target_host_ip = { pdata[13], pdata[14], pdata[15], pdata[16] },
                                .target_dev_ip = { pdata[17], pdata[18], pdata[19], pdata[20] },
                                .reserved = { 0, 0 },
                                .padding = 0
                            };

                            // Validate config first
                            if (!ECM_IPConfig_Validate(&config)) {
                                status = STATUS_ERR_PARAM;
                                send_response_now = false;
                            } else {
                                // Send success response immediately
                                CH9329_SendResponse(addr, cmd_code, &status, 1, 1);
                                // Then perform Flash write operation
                                uint8_t result = ECM_IPConfig_Set(&config);
                                status = (result == ECM_IP_CONFIG_OK) ? STATUS_SUCCESS : result;
                                // Note: We don't send another response here as we already sent success
                                // Flash errors will need to be detected by user via GET commands
                                send_response_now = false; // Already sent response
                            }
                        }
                        break;

                    case 0x01: // Set Host Side
                        if (data_len != 13) { // 1 byte sub_cmd + 12 bytes data
                            status = STATUS_ERR_PARAM;
                            send_response_now = false;
                        } else {
                            // Create temporary config to validate
                            ECM_IPConfig_t temp_config;
                            ECM_IPConfig_GetCurrent(&temp_config);
                            memcpy(temp_config.host_ip, &pdata[1], 4);
                            memcpy(temp_config.dev_ip, &pdata[5], 4);
                            memcpy(temp_config.subnet_mask, &pdata[9], 4);

                            // Validate config first
                            if (!ECM_IPConfig_Validate(&temp_config)) {
                                status = STATUS_ERR_PARAM;
                                send_response_now = false;
                            } else {
                                // Send success response immediately
                                CH9329_SendResponse(addr, cmd_code, &status, 1, 1);
                                // Then perform Flash write operation
                                uint8_t result = ECM_IPConfig_SetHostSide(&pdata[1], &pdata[5], &pdata[9]);
                                status = (result == ECM_IP_CONFIG_OK) ? STATUS_SUCCESS : result;
                                // Note: We don't send another response here as we already sent success
                                // Flash errors will need to be detected by user via GET commands
                                send_response_now = false; // Already sent response
                            }
                        }
                        break;

                    case 0x02: // Set Target Side
                        if (data_len != 9) { // 1 byte sub_cmd + 8 bytes data
                            status = STATUS_ERR_PARAM;
                            send_response_now = false;
                        } else {
                            // Create temporary config to validate
                            ECM_IPConfig_t temp_config;
                            ECM_IPConfig_GetCurrent(&temp_config);
                            memcpy(temp_config.target_host_ip, &pdata[1], 4);
                            memcpy(temp_config.target_dev_ip, &pdata[5], 4);

                            // Validate config first
                            if (!ECM_IPConfig_Validate(&temp_config)) {
                                status = STATUS_ERR_PARAM;
                                send_response_now = false;
                            } else {
                                // Send success response immediately
                                CH9329_SendResponse(addr, cmd_code, &status, 1, 1);
                                // Then perform Flash write operation
                                uint8_t result = ECM_IPConfig_SetTargetSide(&pdata[1], &pdata[5]);
                                status = (result == ECM_IP_CONFIG_OK) ? STATUS_SUCCESS : result;
                                // Note: We don't send another response here as we already sent success
                                // Flash errors will need to be detected by user via GET commands
                                send_response_now = false; // Already sent response
                            }
                        }
                        break;

                    case 0x03: // Reset to Default
                        if (data_len != 1) {
                            status = STATUS_ERR_PARAM;
                            send_response_now = false;
                        } else {
                            // Reset to default is always valid (we know the default config is valid)
                            // Send success response immediately
                            CH9329_SendResponse(addr, cmd_code, &status, 1, 1);
                            // Then perform Flash write operation
                            uint8_t result = ECM_IPConfig_Reset();
                            status = (result == ECM_IP_CONFIG_OK) ? STATUS_SUCCESS : result;
                            // Note: We don't send another response here as we already sent success
                            // Flash errors will need to be detected by user via GET commands
                            send_response_now = false; // Already sent response
                        }
                        break;

                    default:
                        status = STATUS_ERR_PARAM;
                        send_response_now = false;
                        break;
                }
            }

            // Send response if we haven't sent it yet (for error cases or if we deferred sending)
            if (send_response_now) {
                CH9329_SendResponse(addr, cmd_code, &status, 1, (status == STATUS_SUCCESS) ? 1 : 0);
            }
#else
            uint8_t st = STATUS_ERR_CMD;
            CH9329_SendResponse(addr, cmd_code, &st, 1, 0);
#endif
        }
        break;

        case CMD_GET_ECM_IP_CONFIG:
        {
#if USB_WORK_MODE_COMPILES_BRIDGE_ECM
            extern volatile uint8_t g_host_ecm_data_if_active;
            extern volatile uint8_t g_target_ecm_data_if_active;
            extern volatile uint8_t USBFS_Connect;

            uint8_t status = STATUS_SUCCESS;

            if (data_len != 1) {
                status = STATUS_ERR_PARAM;
                CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
                break;
            }

            uint8_t sub_cmd = pdata[0];
            uint8_t response[21]; // Max: 1 byte status + 20 bytes data

            switch (sub_cmd) {
                case 0x00: // Get All
                {
                    ECM_IPConfig_t config;
                    ECM_IPConfig_Get(&config);
                    response[0] = STATUS_SUCCESS;
                    memcpy(&response[1], config.host_ip, 4);
                    memcpy(&response[5], config.dev_ip, 4);
                    memcpy(&response[9], config.subnet_mask, 4);
                    memcpy(&response[13], config.target_host_ip, 4);
                    memcpy(&response[17], config.target_dev_ip, 4);
                    CH9329_SendResponse(addr, cmd_code, response, 21, 1);
                }
                break;

                case 0x01: // Get Host Side
                {
                    ECM_IPConfig_t config;
                    ECM_IPConfig_Get(&config);
                    response[0] = STATUS_SUCCESS;
                    memcpy(&response[1], config.host_ip, 4);
                    memcpy(&response[5], config.dev_ip, 4);
                    memcpy(&response[9], config.subnet_mask, 4);
                    CH9329_SendResponse(addr, cmd_code, response, 13, 1);
                }
                break;

                case 0x02: // Get Target Side
                {
                    ECM_IPConfig_t config;
                    ECM_IPConfig_Get(&config);
                    response[0] = STATUS_SUCCESS;
                    memcpy(&response[1], config.target_host_ip, 4);
                    memcpy(&response[5], config.target_dev_ip, 4);
                    CH9329_SendResponse(addr, cmd_code, response, 9, 1);
                }
                break;

                case 0x03: // Get ECM Bridge Status
                {
                    response[0] = STATUS_SUCCESS;
                    response[1] = USBFS_Connect;           // Target connection status
                    response[2] = g_host_ecm_data_if_active;   // Host ECM active
                    response[3] = g_target_ecm_data_if_active; // Target ECM active
                    CH9329_SendResponse(addr, cmd_code, response, 4, 1);
                }
                break;

                default:
                    status = STATUS_ERR_PARAM;
                    CH9329_SendResponse(addr, cmd_code, &status, 1, 0);
                    break;
            }
#else
            uint8_t st = STATUS_ERR_CMD;
            CH9329_SendResponse(addr, cmd_code, &st, 1, 0);
#endif
        }
        break;

        default:
        {
            uint8_t st = STATUS_ERR_CMD;
            CH9329_SendResponse(addr, cmd_code, &st, 1, 0);
        }
        break;
    }
}

int CH9329_DataParser(uint8_t* buf, uint16_t len)
{
    // Check that buffer is at least long enough for header + minimal fields
    if (len < 6) return 0;

    // Verify frame header
    if (buf[0] != CH9329_FRAME_HEAD1 || buf[1] != CH9329_FRAME_HEAD2) {
        // Not a valid frame header �� skip one byte and try again
        return 1;
    }

    uint8_t addr     = buf[2];
    uint8_t cmd_code = buf[3];
    uint8_t data_len = buf[4];
    uint16_t frame_total_len = 2 + 1 + 1 + 1 + (uint16_t)data_len + 1;

    // Wait until the full frame is received
    if (len < frame_total_len) return 0;

    uint8_t *pdata = &buf[5];
    uint8_t recv_sum = buf[5 + data_len];

    // Calculate checksum
    uint8_t sum = 0;
    for (uint16_t i = 0; i < frame_total_len - 1; i++) {
        sum += buf[i];
    }

    if (sum != recv_sum) {
        // Checksum mismatch �� treat as invalid frame, skip full length
        return frame_total_len;
    }

    // Valid frame �� dispatch command
    CH9329_DispatchCommand(addr, cmd_code, pdata, data_len);
    return frame_total_len;
}

static uint8_t rx_buf[RX_BUF_SIZE];
static uint16_t rx_start = 0;  /* offset of the first unparsed byte */
static uint16_t rx_len = 0;    /* number of valid bytes from rx_start */

void CH9329_RxBuffer_Add(uint8_t *data, uint16_t len) {
    if (len == 0 || data == NULL) return;

    /* NOTE: This function must ONLY be called from the main loop (USB_DataRx_To_KMHandle),
     * NEVER from interrupt context. Interrupt handlers should copy data to the ring buffer
     * (UART2_Tx_Buf) and let the main loop parse it here.
     *
     * Do NOT use __disable_irq() here — it would block USB HID interrupts during parsing,
     * causing keyboard/mouse latency. The ring buffer approach ensures safe data transfer
     * from interrupt to main loop without disabling interrupts. */

    /* Reset the cursor whenever the buffer empties so rx_start cannot drift to
     * the end of the array across many small frames. */
    if (rx_len == 0) rx_start = 0;

    /* Compact only when the tail cannot hold the new bytes:
     * at most one memmove per Add (amortized O(1)). */
    if ((uint32_t)rx_start + rx_len + len > RX_BUF_SIZE) {
        if ((uint32_t)rx_len + len > RX_BUF_SIZE) {
            /* Still too big even after compaction: drop the oldest bytes so the
             * newest data survives. */
            uint16_t drop = (uint16_t)((uint32_t)rx_len + len - RX_BUF_SIZE);
            rx_start += drop;
            rx_len -= drop;
        }
        if (rx_len > 0) memmove(rx_buf, rx_buf + rx_start, rx_len);
        rx_start = 0;
    }

    /* Append new data */
    memcpy(rx_buf + rx_start + rx_len, data, len);
    rx_len += len;

    /* Parse frame by frame: a consumed frame only advances rx_start — no
     * shuffling. CH9329_DataParser runs on contiguous memory and is unchanged. */
    while (rx_len > 0) {
        int consumed = CH9329_DataParser(rx_buf + rx_start, rx_len);
        if (consumed < 0) {
            /* Parsing error — clear buffer */
            rx_start = 0;
            rx_len = 0;
            break;
        } else if (consumed == 0) {
            /* Incomplete frame — wait for more data */
            break;
        } else {
            rx_start += (uint16_t)consumed;
            rx_len -= (uint16_t)consumed;
        }
    }
}

void USB_DataRx_To_KMHandle(void) {
    // Step 1: Handle data from BLE ring buffer
#ifdef ENABLE_BLE
    while (RingMemBLE.CurrentLen > 0) {
        uint8_t temp[64];
        uint8_t len = (RingMemBLE.CurrentLen > sizeof(temp)) ? sizeof(temp) : (uint8_t)RingMemBLE.CurrentLen;
        if (RingMemRead(&RingMemBLE, temp, len) == SUCCESS) {
            CH9329_RxBuffer_Add(temp, len);
        } else {
            break;
        }
    }
#endif /* ENABLE_BLE */

    // Step 2: Handle data from UART2 transmit buffer (EP5 OUT ring buffer)
    while (Uart.Tx_RemainNum) {
        if (Uart.Tx_CurPackLen == 0x00) {
            Uart.Tx_CurPackLen = Uart.Tx_PackLen[Uart.Tx_DealNum];
            Uart.Tx_CurPackPtr = Uart.Tx_DealNum * DEF_USB_FS_PACK_LEN;
        }

        uint8_t temp[255];
        uint8_t copy_len = (Uart.Tx_CurPackLen > sizeof(temp)) ? sizeof(temp) : (uint8_t)Uart.Tx_CurPackLen;

        for (uint16_t i = 0; i < copy_len; i++) {
            temp[i] = UART2_Tx_Buf[Uart.Tx_CurPackPtr++];
        }

        /* Route data based on active USB mode:
         * - CH9329 command frames go to CH9329 parser
         * - Other data routes to ACM/ECM bridges per active mode */
#if USB_WORK_MODE_COMPILES_BRIDGE_ACM || USB_WORK_MODE_COMPILES_BRIDGE_ECM || USB_WORK_MODE_HAS_HOST_ACM_AND_ECM || USB_WORK_MODE_HAS_HOST_CDC_ACM
        if (USBD_IsCh9329ModeCommandFrame(temp, copy_len)) {
            CH9329_RxBuffer_Add(temp, copy_len);
        }
#if USB_WORK_MODE_COMPILES_BRIDGE_ACM
        else if (USB_ModeConfig_ActiveModeBridgeAcm()) {
            ACM_Bridge_HostRxDone(temp, copy_len);
        }
#endif
#if USB_WORK_MODE_HAS_HOST_CDC_ACM
        else if (USB_ModeConfig_ActiveModeHasHostCdcAcm()) {
            /* Host CDC ACM mode: all non-CH9329 data goes to CH9329 parser
             * (legacy behavior — CH9329 parser handles it) */
            CH9329_RxBuffer_Add(temp, copy_len);
        }
#endif
#if USB_WORK_MODE_COMPILES_BRIDGE_ECM
        else if (USB_ModeConfig_ActiveModeBridgeEcm()) {
            ECM_Bridge_HostRxDone(temp, copy_len);
        }
#endif
        /* Other modes: silently drop non-CH9329 data */
#else
        /* No bridge modes compiled — all data goes to CH9329 parser */
        CH9329_RxBuffer_Add(temp, copy_len);
#endif

        Uart.Tx_CurPackLen -= copy_len;
        if (Uart.Tx_CurPackLen == 0) {
            Uart.Tx_PackLen[Uart.Tx_DealNum] = 0;
            Uart.Tx_DealNum = (Uart.Tx_DealNum + 1) % DEF_UARTx_TX_BUF_NUM_MAX;
            Uart.Tx_RemainNum--;
        }
    }

    /* Re-enable EP5 OUT if it was stopped due to buffer full */
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    if ((Uart.USB_Down_StopFlag == 0x01) && (Uart.Tx_RemainNum < 2)) {
        SetEPRxValid(ENDP5);
        Uart.USB_Down_StopFlag = 0x00;
    }
#endif
}


// -------------------- Keyboard Compatibility Functions --------------------
// These functions provide backward compatibility for existing code

/*********************************************************************
 * @fn      CH9329_SetAutoReleaseMode
 *
 * @brief   Configure keyboard auto-release behavior (backward compatibility)
 *
 * @param   sender_handles_release - true if sender will handle releases, false for auto-release
 *
 * @return  none
 */
void CH9329_SetAutoReleaseMode(bool sender_handles_release) {
    Keyboard_SetAutoReleaseMode(sender_handles_release);
}

/*********************************************************************
 * @fn      CH9329_GetAutoReleaseMode
 *
 * @brief   Get current keyboard auto-release mode (backward compatibility)
 *
 * @return  true if sender handles releases, false if auto-release is enabled
 */
bool CH9329_GetAutoReleaseMode(void) {
    return Keyboard_GetAutoReleaseMode();
}

/*********************************************************************
 * @fn      CH9329_ResetAutoDetection
 *
 * @brief   Reset auto-detection and re-enable auto-release mode (backward compatibility)
 *
 * @return  none
 */
void CH9329_ResetAutoDetection(void) {
    Keyboard_ResetAutoDetection();
}

/*********************************************************************
 * @fn      Keyboard_SetLEDStatus (Backward Compatibility)
 *
 * @brief   Set keyboard LED status (backward compatibility wrapper)
 *
 * @param   led_status - LED status byte
 *
 * @return  none
 */
void KB_SetLEDStatus(uint8_t led_status) {
    Keyboard_SetLEDStatus(led_status);
}

/*********************************************************************
 * @fn      Keyboard_GetLEDStatus (Backward Compatibility)
 *
 * @brief   Get current keyboard LED status (backward compatibility wrapper)
 *
 * @return  Current LED status byte
 */
uint8_t KB_GetLEDStatus(void) {
    return Keyboard_GetLEDStatus();
}

// -------------------- Mouse Compatibility Functions --------------------
// These functions provide backward compatibility for existing code

/*********************************************************************
 * @fn      CH9329_HandleRelativeMouse (Backward Compatibility)
 *
 * @brief   Handle relative mouse data (backward compatibility wrapper)
 *
 * @param   addr - Device address
 * @param   cmd_code - Command code
 * @param   data - Mouse data
 * @param   data_len - Length of data
 *
 * @return  none
 */
void CH9329_HandleRelativeMouse(uint8_t addr, uint8_t cmd_code, uint8_t* data, uint8_t data_len) {
    Mouse_HandleRelativeData(addr, cmd_code, data, data_len);
}

/*********************************************************************
 * @fn      CH9329_HandleAbsoluteMouse (Backward Compatibility)
 *
 * @brief   Handle absolute mouse data (backward compatibility wrapper)
 *
 * @param   addr - Device address
 * @param   cmd_code - Command code
 * @param   data - Mouse data
 * @param   data_len - Length of data
 *
 * @return  none
 */
void CH9329_HandleAbsoluteMouse(uint8_t addr, uint8_t cmd_code, uint8_t* data, uint8_t data_len) {
    Mouse_HandleAbsoluteData(addr, cmd_code, data, data_len);
}