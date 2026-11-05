#include "include/keyboard_handler.h"
#include "ch32v20x_usbfs_device.h"
#include "usbd_composite_km.h"
#include "config.h"
#include "HAL.h"
#include "km_ring.h"
#include <string.h>
#include <stdbool.h>

/*******************************************************************************/
/* Global Variables */

volatile uint8_t  KB_Scan_Done = 0x00;
volatile uint16_t KB_Scan_Result = 0xF000;
volatile uint16_t KB_Scan_Last_Result = 0xF000;
volatile uint8_t  KB_LED_Last_Status = 0x00;
volatile uint8_t  KB_LED_Cur_Status = 0x00;

uint8_t KB_Data_Pack[8];
uint8_t mod_keys = 0;
uint8_t normal_keys[6] = {0};

/*******************************************************************************/
/* Private Variables */

static uint8_t kb_last_data[8] = {0};
static uint8_t media_last_data[4] = {0};

/*******************************************************************************/
/* External Functions */

extern void CH9329_SendAck(uint8_t addr, uint8_t cmd_code, uint8_t status);

/*******************************************************************************/
/* Function Implementations */

/*********************************************************************
 * @fn      Keyboard_Init
 *
 * @brief   Initialize keyboard handler and internal states
 *
 * @return  none
 */
void Keyboard_Init(void) {
    memset(KB_Data_Pack, 0, sizeof(KB_Data_Pack));
    memset(normal_keys, 0, sizeof(normal_keys));
    memset(kb_last_data, 0, sizeof(kb_last_data));
    
    mod_keys = 0;
    
    KB_Scan_Done = 0x00;
    KB_Scan_Result = 0xF000;
    KB_Scan_Last_Result = 0xF000;
    KB_LED_Last_Status = 0x00;
    KB_LED_Cur_Status = 0x00;
}

/*********************************************************************
 * @fn      Keyboard_SendDataToUSB
 *
 * @brief   Send keyboard report to USB endpoint
 *
 * @param   data - 8-byte keyboard HID report
 *
 * @return  none
 */
void Keyboard_SendDataToUSB(uint8_t* data) {
    Queue_Push_Keyboard(data);
}

/*********************************************************************
 * @fn      Keyboard_HandleData
 *
 * @brief   Handle keyboard input from CH9329
 *          Only release keys if host explicitly sends release report
 *
 * @param   addr     - Device address
 * @param   cmd_code - Command code
 * @param   data     - 8-byte keyboard data
 * @param   data_len - Length of data (should be 8)
 *
 * @return  none
 */
void Keyboard_HandleData(uint8_t addr, uint8_t cmd_code, uint8_t* data, uint8_t data_len)
{


    uint8_t hid_report[8] = {0}; 
    uint8_t hid_mod = 0;
    uint8_t keys[6] = {0};
    int key_idx = 0;

    uint8_t available = (data_len >= 8) ? 8 : data_len;

    if (available >= 1) {
        hid_mod |= data[0];
    }


    for (int i = 2; i < (int)available && i < 8; ++i) {
        uint8_t k = data[i];
        if (k == 0) continue;
        if (k >= 0xE0 && k <= 0xE7) {
            hid_mod |= (1 << (k - 0xE0));
        } else {
            if (key_idx < 6) {
                keys[key_idx++] = k;
            }
        }
    }

    hid_report[0] = hid_mod; // modifier
    hid_report[1] = 0x00;    // reserved
    memcpy(&hid_report[2], keys, 6);

    bool is_release = true;
    for (int i = 0; i < 8; ++i) {
        if (hid_report[i] != 0) {
            is_release = false;
            break;
        }
    }

    if (memcmp(hid_report, kb_last_data, 8) != 0) {
        Keyboard_SendDataToUSB(hid_report);

        memcpy(kb_last_data, hid_report, 8);
    }

    if (is_release) {
        memset(kb_last_data, 0, sizeof(kb_last_data));
    }

    mod_keys = hid_report[0];
    memcpy(normal_keys, &hid_report[2], 6);
}


/*********************************************************************
 * @fn      Keyboard_SetLEDStatus
 *
 * @brief   Set keyboard LED status (Caps Lock, Num Lock, Scroll Lock)
 *
 * @param   led_status - LED status byte from HID SET_REPORT
 *
 * @return  none
 */
void Keyboard_SetLEDStatus(uint8_t led_status) {
    KB_LED_Last_Status = KB_LED_Cur_Status;
    KB_LED_Cur_Status = led_status;
}

/*********************************************************************
 * @fn      Keyboard_GetLEDStatus
 *
 * @brief   Get current keyboard LED status
 *
 * @return  Current LED status byte
 */
uint8_t Keyboard_GetLEDStatus(void) {
    return KB_LED_Cur_Status;
}

/*********************************************************************
 * @fn      Keyboard_GetLastLEDStatus
 *
 * @brief   Get last keyboard LED status
 *
 * @return  Last LED status byte
 */
uint8_t Keyboard_GetLastLEDStatus(void) {
    return KB_LED_Last_Status;
}

/*********************************************************************
 * @fn      Keyboard_HandleMediaData
 *
 * @brief   Handle media key data from CH9329 (CMD=0x03)
 *
 * @param   addr     - Device address
 * @param   cmd_code - Command code (0x03)
 * @param   data     - 2-byte media key bitmap
 * @param   data_len - Length of data
 *
 * @return  none
 */
/*********************************************************************
 * @fn      Keyboard_HandleMediaData
 *
 * @brief   Handle media key data from CH9329 (CMD=0x03)
 *          CH9329 protocol format:
 *            ACPI keys:  data[0]=0x01, data[1]=acpi_code (Power/Sleep/Wake)
 *            Media keys: data[0]=0x02, data[1..3]=24-bit bitmap
 *            Apple row:  data[0]=0x03, data[1]=brightness/Mission Control/Launchpad
 *
 * @param   addr     - Device address
 * @param   cmd_code - Command code (0x03)
 * @param   data     - data bytes from CH9329 frame (Report ID + payload)
 * @param   data_len - Length of data (2 for ACPI and Apple row, 4 for media)
 *
 * @return  none
 */
void Keyboard_HandleMediaData(uint8_t addr, uint8_t cmd_code, uint8_t* data, uint8_t data_len)
{
    if (data_len < 2) return;

    uint8_t report_id = data[0];
    if (report_id != 0x01 && report_id != 0x02 && report_id != 0x03) return;

    /* 0x02 Consumer carries 3 data bytes; 0x01 ACPI and 0x03 Apple function
     * row carry one each. */
    uint8_t req_len = (report_id == 0x02) ? 4 : 2;
    if (data_len < req_len) return;

    /* Skip if identical to last report */
    if (report_id == media_last_data[0] &&
        data[1] == media_last_data[1] &&
        (report_id != 0x02 || (data[2] == media_last_data[2] && data[3] == media_last_data[3]))) {
        return;
    }

    /* Build report: [Report_ID, byte1, byte2, byte3] */
    uint8_t report[4] = {0};
    report[0] = report_id;
    report[1] = data[1];
    if (report_id == 0x02) {
        report[2] = (data_len > 2) ? data[2] : 0;
        report[3] = (data_len > 3) ? data[3] : 0;
    }

    Queue_Push_Media(report);

    /* Update last state */
    media_last_data[0] = report[0];
    media_last_data[1] = report[1];
    media_last_data[2] = report[2];
    media_last_data[3] = report[3];

    /* Clear tracking on release */
    if (report[1] == 0 && report[2] == 0 && report[3] == 0) {
        media_last_data[0] = 0;
    }
}
