/********************************** (C) COPYRIGHT *******************************
 * File Name          : ch32v20x_usbfs_device.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2022/08/18
 * Description        : ch32v20x series usb interrupt processing.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/


/*******************************************************************************/
/* Header File */
#include "ch32v20x_usbfs_device.h"
#include "usbd_composite_km.h"
#include "peripheral.h"
#include "RingMem.h"
#include "include/keyboard_handler.h"
#include "include/usbd_desc.h"
#include "include/uac_desc.h"
#include "include/uac_handler.h"
#include "include/acm_bridge.h"
#include "include/ecm_bridge.h"
#include "include/ble_eth_protocol.h"
#include "include/usb_mode_config.h"
/*******************************************************************************/
/* Variable Definition */

/* Global */
const    uint8_t  *pUSBFS_Descr;
#define pMySetupReqPakHD    ((PUSB_SETUP_REQ)EP0_DatabufHD)
#define RepDescSize         62
/* Setup Request */
volatile uint8_t  USBFS_SetupReqCode;
volatile uint8_t  USBFS_SetupReqType;
volatile uint16_t USBFS_SetupReqValue;
volatile uint16_t USBFS_SetupReqIndex;
volatile uint16_t USBFS_SetupReqLen;

/* USB Device Status */
volatile uint8_t  USBFS_DevConfig;
volatile uint8_t  USBFS_DevAddr;
volatile uint8_t  USBFS_DevSleepStatus;
volatile uint8_t  USBFS_DevEnumStatus;
volatile uint8_t  USBFS_Connect;
/* HID Class Command */
volatile uint8_t  USBFS_HidIdle[ 4 ];
volatile uint8_t  USBFS_HidProtocol[ 4 ];

#define USBFS_MODE_HID_IF              0x03u
#define USBFS_MODE_HID_REPORT_ID       0x30u
#define USBFS_HID_REPORT_TYPE_FEATURE  0x03u
#define USBFS_MODE_HID_REPORT_LEN      DEF_USBD_UEP0_SIZE
static uint8_t USBFS_ModeHidLastStatus = STATUS_SUCCESS;

#define USBFS_MODE_HID_DIAG_VERSION_OFFSET          33u
#define USBFS_MODE_HID_DIAG_FLAGS_OFFSET            34u
#define USBFS_MODE_HID_DIAG_BLE_ETH_STATS_OFFSET    35u

static void USBFS_ModeHid_WriteU32LE(uint8_t *buf, uint16_t off, uint32_t v)
{
    if (!buf || (uint16_t)(off + 4u) > USBFS_MODE_HID_REPORT_LEN) return;
    buf[off + 0u] = (uint8_t)(v);
    buf[off + 1u] = (uint8_t)(v >> 8);
    buf[off + 2u] = (uint8_t)(v >> 16);
    buf[off + 3u] = (uint8_t)(v >> 24);
}

static void USBFS_ModeHid_FillReport(uint8_t *report)
{
    uint16_t flags = USB_ModeConfig_GetFlags();
    uint32_t capability_mask = USB_ModeConfig_GetCapabilityMask();

    memset(report, 0, USBFS_MODE_HID_REPORT_LEN);
    report[0] = USBFS_MODE_HID_REPORT_ID;
    report[1] = USBFS_ModeHidLastStatus;
    report[2] = USB_ModeConfig_GetCompiledMode();
    report[3] = USB_ModeConfig_GetActiveMode();
    report[4] = USB_ModeConfig_GetStoredMode();
    report[5] = USB_ModeConfig_GetStatus();
    report[6] = (uint8_t)flags;
    report[7] = (uint8_t)(flags >> 8);
    report[8] = (uint8_t)capability_mask;
    report[9] = (uint8_t)(capability_mask >> 8);
    report[10] = (uint8_t)(capability_mask >> 16);
    report[11] = (uint8_t)(capability_mask >> 24);
#if USB_WORK_MODE_COMPILES_BRIDGE_ECM
    report[12] = g_host_ecm_data_if_active;
    report[13] = g_host_garp_sent;
    report[14] = g_target_ecm_data_if_active;
    report[15] = g_target_garp_sent;
    report[14] = g_ecm_tx_state;
    report[15] = g_ecm_led_override;
    report[16] = (uint8_t)g_ecm_tx_count;
    report[17] = (uint8_t)(g_ecm_tx_count >> 8);
    report[18] = (uint8_t)g_usbfs_ep6_rx_count;
    report[19] = (uint8_t)(g_usbfs_ep6_rx_count >> 8);
    report[20] = (uint8_t)(g_usbfs_ep6_rx_count >> 16);
    report[21] = (uint8_t)(g_usbfs_ep6_rx_count >> 24);
    report[22] = g_usbfs_ep6_tog_ok;
    report[23] = (uint8_t)g_usbfs_ep6_rx_len;
    report[24] = (uint8_t)(g_usbfs_ep6_rx_len >> 8);
    report[25] = USBFS_Endp_Busy[DEF_UEP7];
    report[26] = USBFSD->UEP7_TX_CTRL;
    report[27] = (uint8_t)USBFSD->UEP7_TX_LEN;
    report[28] = (uint8_t)(USBFSD->UEP7_TX_LEN >> 8);
    report[29] = USBFSD->UEP6_RX_CTRL;
    report[30] = USBFSD->UEP5_TX_CTRL;
    report[31] = (uint8_t)g_usbfs_ep7_in_count;
    report[32] = (uint8_t)(g_usbfs_ep7_in_count >> 8);
#endif

    /* Append-only diagnostics (do not change existing offsets above). */
    report[USBFS_MODE_HID_DIAG_VERSION_OFFSET] = 0x01u;
#if USB_WORK_MODE_BRIDGE_ECM
    report[USBFS_MODE_HID_DIAG_FLAGS_OFFSET] = 0x01u; /* bit0: BLE-Eth stats present */

    ble_eth_reliability_stats_t st;
    BLE_Eth_GetReliabilityStats(&st);
    USBFS_ModeHid_WriteU32LE(report, USBFS_MODE_HID_DIAG_BLE_ETH_STATS_OFFSET + 0u,  st.frag_rx_total);
    USBFS_ModeHid_WriteU32LE(report, USBFS_MODE_HID_DIAG_BLE_ETH_STATS_OFFSET + 4u,  st.frag_rx_duplicate);
    USBFS_ModeHid_WriteU32LE(report, USBFS_MODE_HID_DIAG_BLE_ETH_STATS_OFFSET + 8u,  st.frag_rx_out_of_order);
    USBFS_ModeHid_WriteU32LE(report, USBFS_MODE_HID_DIAG_BLE_ETH_STATS_OFFSET + 12u, st.frag_reassembly_abort);
    USBFS_ModeHid_WriteU32LE(report, USBFS_MODE_HID_DIAG_BLE_ETH_STATS_OFFSET + 16u, st.tx_queue_full_drop);
    USBFS_ModeHid_WriteU32LE(report, USBFS_MODE_HID_DIAG_BLE_ETH_STATS_OFFSET + 20u, st.frame_rx_len_err);
    USBFS_ModeHid_WriteU32LE(report, USBFS_MODE_HID_DIAG_BLE_ETH_STATS_OFFSET + 24u, st.frame_rx_checksum_err);
#else
    report[USBFS_MODE_HID_DIAG_FLAGS_OFFSET] = 0x00u; /* bit0: no BLE-Eth stats */
#endif
}

static void USBFS_ModeHid_HandleSetReport(uint8_t *report, uint16_t report_len)
{
    uint16_t requested_flags = 0;

    USBFS_ModeHidLastStatus = STATUS_SUCCESS;
    if (report_len < 2 || report[0] != USBFS_MODE_HID_REPORT_ID) {
        USBFS_ModeHidLastStatus = STATUS_ERR_PARAM;
        return;
    }

    switch (report[1]) {
        case 0x00:
            break;
        case 0x01:
            if (report_len < 3) {
                USBFS_ModeHidLastStatus = STATUS_ERR_PARAM;
            }
            else {
                if (report_len >= 5) {
                    requested_flags = (uint16_t)report[3] | ((uint16_t)report[4] << 8);
                }
                if (!USB_ModeConfig_Write(report[2], requested_flags)) {
                    USBFS_ModeHidLastStatus = STATUS_ERR_PARAM;
                }
            }
            break;
        case 0x02:
            if (!USB_ModeConfig_Clear()) {
                USBFS_ModeHidLastStatus = STATUS_ERR_FRAME;
            }
            break;
        case 0x03:
            USB_ModeConfig_RequestReboot();
            break;
        default:
            USBFS_ModeHidLastStatus = STATUS_ERR_PARAM;
            break;
    }
}

#if USB_WORK_MODE_COMPILES_TARGET_CDC_ACM || USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
#define USBFS_CDC_ACM_IF_COMM          4u
#define USBFS_CDC_ACM_REQ_SET_LINE     0x20u
#define USBFS_CDC_ACM_REQ_GET_LINE     0x21u
#define USBFS_CDC_ACM_REQ_SET_CONTROL  0x22u
static uint8_t USBFS_CDC_LineCoding[7] = { 0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08 };
static volatile uint16_t USBFS_CDC_ControlLineState;
#endif

#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
#define USBFS_CDC_ECM_IF_COMM             4u
#define USBFS_CDC_ECM_REQ_SET_MC_FILTERS  0x40u
#define USBFS_CDC_ECM_REQ_SET_PM_PATTERN  0x41u
#define USBFS_CDC_ECM_REQ_GET_PM_PATTERN  0x42u
#define USBFS_CDC_ECM_REQ_SET_PACKET      0x43u
#define USBFS_CDC_ECM_REQ_GET_STATISTIC   0x44u
static uint8_t USBFS_CDC_ECM_CtrlBuf[64];
static uint8_t USBFS_CDC_ECM_Stat[4] = { 0, 0, 0, 0 };
static volatile uint16_t USBFS_CDC_ECM_PacketFilter;
#endif

/* Endpoint Buffer */
__attribute__ ((aligned(4))) uint8_t USBFS_EP0_Buf[ DEF_USBD_UEP0_SIZE ];     //ep0(64)
__attribute__ ((aligned(4))) uint8_t USBFS_EP1_Buf[ DEF_USB_EP1_FS_SIZE ];    //ep1_in(64)
__attribute__ ((aligned(4))) uint8_t USBFS_EP2_Buf[ DEF_USB_EP2_FS_SIZE ];    //ep2_in(64)
__attribute__ ((aligned(4))) uint8_t USBFS_EP3_Buf[ DEF_USB_EP3_FS_SIZE ];    //ep3_in(64)
__attribute__ ((aligned(4))) uint8_t USBFS_EP4_Buf[ DEF_USB_EP4_FS_SIZE ];    //ep4_in(64) - Media keys

#ifdef ENABLE_UAC
/* UAC ISO endpoint buffers: single-buffer, 64 bytes each. */
__attribute__ ((aligned(4))) uint8_t USBFS_EP5_Buf[ UAC_ISO_PACKET_SIZE ];     //ep5_in  ISO TX
__attribute__ ((aligned(4))) uint8_t USBFS_EP6_Buf[ UAC_ISO_PACKET_SIZE ];     //ep6_out ISO RX
#endif /* ENABLE_UAC */

/* USB IN Endpoint Busy Flag */
volatile uint8_t  USBFS_Endp_Busy[ DEF_UEP_NUM ];

/* USB Endpoint Busy Timeout Recovery
 *
 * If a USB transfer completion interrupt is lost (due to EMI, clock jitter,
 * etc.), the busy flag can remain set forever, causing the endpoint to stop
 * sending data. This mechanism records when each endpoint became busy and
 * provides a recovery function that can be called from the main loop to
 * detect and recover from stuck endpoints.
 *
 * Zero overhead in normal path: only compares when busy flag is set.
 */
#define USB_ENDPOINT_BUSY_TIMEOUT_MS  20  /* 20ms timeout (>> normal <1ms transfer) */
static uint32_t USBFS_Endp_Busy_StartTime[ DEF_UEP_NUM ];
volatile uint32_t USBFS_Endp_Timeout_Count[ DEF_UEP_NUM ];  /* Error counters for diagnostics */

/* External systick_ms from RGB.c (1ms timer tick) */
extern volatile uint32_t systick_ms;

/* EP6 OUT diagnostic — set in USBFS_IRQHandler, read by main loop */
volatile uint8_t  g_usbfs_ep6_int_flag = 0;  /* 1 = EP6 OUT interrupt fired */
volatile uint8_t  g_usbfs_ep6_tog_ok = 0;    /* last TOG_OK result */
volatile uint16_t g_usbfs_ep6_rx_len = 0;    /* last RX length */
volatile uint32_t g_usbfs_ep6_rx_count = 0;  /* total EP6 OUT interrupts (any TOG state) */
volatile uint16_t g_usbfs_ep7_in_count = 0;   /* total EP7 IN completions */

/******************************************************************************/
/* Interrupt Service Routine Declaration*/
void USBFS_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
#if DEF_USBD_SPEED == DEF_USBD_SPEED_LOW
  #define RepDataLoadLen    8
#else
  #define RepDataLoadLen    64
#endif

/******************************************************************************/
/* Endpoint Buffer */
__attribute__((aligned(4))) UINT8 EP0_DatabufHD[64];      //ep0(64)
__attribute__((aligned(4))) UINT8 EP1_DatabufHD[64 + 64]; //ep1_out(64)+ep1_in(64)
__attribute__((aligned(4))) UINT8 EP2_DatabufHD[64 + 64]; //ep2_out(64)+ep2_in(64)
__attribute__((aligned(4))) UINT8 EP3_DatabufHD[64 + 64]; //ep3_out(64)+ep3_in(64)
__attribute__((aligned(4))) UINT8 EP4_DatabufHD[64 + 64]; //ep4_out(64)+ep4_in(64)
__attribute__((aligned(4))) UINT8 EP5_DatabufHD[64 + 64]; //ep5_out(64)+ep5_in(64)
__attribute__((aligned(4))) UINT8 EP6_DatabufHD[64 + 64]; //ep6_out(64)+ep6_in(64)
__attribute__((aligned(4))) UINT8 EP7_DatabufHD[64 + 64]; //ep7_out(64)+ep7_in(64)

PUINT8 pEP0_RAM_Addr; //ep0(64)
PUINT8 pEP1_RAM_Addr; //ep1_out(64)+ep1_in(64)
PUINT8 pEP2_RAM_Addr; //ep2_out(64)+ep2_in(64)
PUINT8 pEP3_RAM_Addr; //ep3_out(64)+ep3_in(64)
PUINT8 pEP4_RAM_Addr; //ep4_out(64)+ep4_in(64)
PUINT8 pEP5_RAM_Addr; //ep5_out(64)+ep5_in(64)
PUINT8 pEP6_RAM_Addr; //ep6_out(64)+ep6_in(64)
PUINT8 pEP7_RAM_Addr; //ep7_out(64)+ep7_in(64)

const UINT8    *pDescr;
volatile UINT8  USBFS_Dev_SetupReqCode = 0xFF;   /* USB2.0 full-speed device setup package command */
volatile UINT16 USBFS_Dev_SetupReqLen = 0x00;    /* USB2.0 full-speed device setup package length */
volatile UINT8  USBFS_Dev_SetupReqValueH = 0x00; /* USB2.0 full-speed device setup package value high byte */
volatile UINT8  USBFS_Dev_Config = 0x00;         /* USB2.0 full-speed device configuration value */
volatile UINT8  USBFS_Dev_Address = 0x00;        /* USB2.0 full-speed device address value */
volatile UINT8  USBFS_Dev_SleepStatus = 0x00;    /* USB2.0 full-speed device sleep state */
volatile UINT8  USBFS_Dev_EnumStatus = 0x00;     /* USB2.0 full-speed device enumeration status */
volatile UINT8  USBFS_Dev_Endp0_Tog = 0x01;      /* USB2.0 Full-speed device endpoint 0 sync flag*/
volatile UINT8  USBFS_Dev_Speed = 0x01;          /* USB2.0 Full-speed device speed */

volatile UINT16 USBFS_Endp1_Up_Flag = 0x00;   /* USB2.0 full-speed device endpoint 1 data upload status: 0: free; 1: upload; */
volatile UINT8  USBFS_Endp1_Down_Flag = 0x00; /* USB2.0 full-speed device endpoint 1 Pass successful logo */
volatile UINT8  USBFS_Endp1_Down_Len = 0x00;  /* USB2.0 full-speed device endpoint 1 download length */
volatile BOOL   USBFS_Endp1_T_Tog = 0;        /* USB2.0 full speed device endpoint 1 sends tog bit flip */
volatile BOOL   USBFS_Endp1_R_Tog = 0;

volatile UINT16 USBFS_Endp2_Up_Flag = 0x00;    /* USB2.0 full-speed device endpoint 2 Data upload status: 0: free; 1: upload; */
volatile UINT16 USBFS_Endp2_Up_LoadPtr = 0x00; /* USB2.0 full-speed device endpoint 2 Data upload loading offset */
volatile UINT8  USBFS_Endp2_Down_Flag = 0x00;  /* USB2.0 full-speed device endpoint 2 download success flag */
volatile BOOL   USBFS_Endp2_T_Tog = 0;         /* USB2.0 full speed device endpoint 2 sends tog bit flip */
volatile BOOL   USBFS_Endp2_R_Tog = 0;

volatile UINT32V Endp2_send_seq = 0x00;
volatile UINT8   DevConfig;
volatile UINT8   SetupReqCode;
volatile UINT16  SetupReqLen;

/******************************************************************************/
/* Device Descriptor */
const UINT8 MyDevDescrHD[] = {

    0x12, 0x01, 0x10, 0x01, 0xFF, 0x00, 0x00, DEF_USBD_UEP0_SIZE,
    0x86, 0x1A, 0x23, 0x75, 0x63, 0x02, 0x00, 0x02,
    0x00, 0x01};

/* Configration Descriptor */
const UINT8 MyCfgDescrHD[] =
    {
        0x09, 0x02, 0x27, 0x00, 0x01, 0x01, 0x00, 0x80, 0xf0, //Configure descriptor, interface descriptor, endpoint descriptor
        0x09, 0x04, 0x00, 0x00, 0x03, 0xff, 0x01, 0x02, 0x00,
        0x07, 0x05, 0x82, 0x02, 0x20, 0x00, 0x00, //Bulk upload endpoint
        0x07, 0x05, 0x02, 0x02, 0x20, 0x00, 0x00, //Bulk download endpoint
        0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x01};

/* Language Descriptor */
const UINT8 MyLangDescrHD[] =
    {
        0x04, 0x03, 0x09, 0x04};

/* Manufactor Descriptor */
const UINT8 MyManuInfoHD[] =
    {
        0x0E, 0x03, 'w', 0, 'c', 0, 'h', 0, '.', 0, 'c', 0, 'n', 0};

/* Product Information */
const UINT8 MyProdInfoHD[] =
    {
        0x0C, 0x03, 'C', 0, 'H', 0, '1', 0, '0', 0, 'x', 0};

/* Product descriptor */
const uint8_t StrDesc[28] =
    {
        0x1C, 0x03, 0x55, 0x00, 0x53, 0x00, 0x42, 0x00,
        0x32, 0x00, 0x2E, 0x00, 0x30, 0x00, 0x2D, 0x00,
        0x53, 0x00, 0x65, 0x00, 0x72, 0x00, 0x69, 0x00,
        0x61, 0x00, 0x6C, 0x00};

const uint8_t Return1[2] = {0x31, 0x00};
const uint8_t Return2[2] = {0xC3, 0x00};
const uint8_t Return3[2] = {0x9F, 0xEE};

void USBFS_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
/*********************************************************************
 * @fn      USBFS_RCC_Init
 *
 * @brief   Initializes the usbfs clock configuration.
 *
 * @return  none
 */
void USBFS_RCC_Init( void )
{
    RCC_ClocksTypeDef RCC_ClocksStatus={0};
    RCC_GetClocksFreq(&RCC_ClocksStatus);
    if( RCC_ClocksStatus.SYSCLK_Frequency == 144000000 )
    {
        RCC_USBCLKConfig( RCC_USBCLKSource_PLLCLK_Div3 );
    }
    else if( RCC_ClocksStatus.SYSCLK_Frequency == 96000000 ) 
    {
        RCC_USBCLKConfig( RCC_USBCLKSource_PLLCLK_Div2 );
    }
    else if( RCC_ClocksStatus.SYSCLK_Frequency == 48000000 ) 
    {
        RCC_USBCLKConfig( RCC_USBCLKSource_PLLCLK_Div1 );
    }
#if defined(CH32V20x_D8W) || defined(CH32V20x_D8)
    else if ( RCC_ClocksStatus.SYSCLK_Frequency == 240000000 && RCC_USB5PRE_JUDGE() == SET )
    {
        RCC_USBCLKConfig( RCC_USBCLKSource_PLLCLK_Div5 );
    }
#endif
    RCC_AHBPeriphClockCmd( RCC_AHBPeriph_USBFS, ENABLE );
}

/*********************************************************************
 * @fn      USBFS_Device_Endp_Init
 *
 * @brief   Initializes USB device endpoints.
 *
 * @return  none
 */
void USBFS_Device_Endp_Init( void )
{
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    /* Combined ACM+ECM mode with USBFS stack:
     * EP1: Keyboard HID IN (TX)
     * EP2: Mouse HID IN (TX)
     * EP3: ECM data OUT (RX) - host to device
     * EP4: ECM data IN (TX) - device to host
     * EP5: ACM interrupt IN (TX)
     * EP6: ACM data OUT (RX) - host to device
     * EP7: ACM data IN (TX) - device to host
     */
    USBFSD->UEP4_1_MOD = USBFS_UEP1_TX_EN | USBFS_UEP4_TX_EN;
    USBFSD->UEP2_3_MOD = USBFS_UEP2_TX_EN | USBFS_UEP3_RX_EN;
#else
    USBFSD->UEP4_1_MOD = USBFS_UEP1_TX_EN | USBFS_UEP4_TX_EN;
    USBFSD->UEP2_3_MOD = USBFS_UEP2_TX_EN | USBFS_UEP3_TX_EN;
#endif

#if defined(ENABLE_UAC) || USB_WORK_MODE_COMPILES_TARGET_CDC_ACM || USB_WORK_MODE_COMPILES_TARGET_CDC_ECM || USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    /* EP5: ISO IN  (Target→Host audio) or ACM interrupt IN (Host→Target).
     * EP6: ISO OUT (Host→Target audio) or ACM data OUT (Host→Target).
     * EP7: ACM data IN (Host→Target).
     *
     * NOTE: WCH USBFS BUF_MOD ping-pong hard-wires the second-half offset to
     * 64 bytes regardless of TX_LEN.  For 192-byte ISO packets this would alias
     * into the wrong memory, so we must NOT use BUF_MOD for large ISO packets.
     * Software double-buffering is handled in uac_handler.c instead. */
#ifdef ENABLE_UAC
    if (USB_ModeConfig_ActiveModeHasTargetUac() && !USB_ModeConfig_ActiveModeHasTargetCdcEcm()) {
        USBFSD->UEP5_6_MOD = USBFS_UEP5_TX_EN | USBFS_UEP6_RX_EN;
        USBFSD_UEP_MOD( 3 ) = 0;
    }
    else if (USB_ModeConfig_ActiveModeHasTargetCdcAcm() || USB_ModeConfig_ActiveModeHasTargetCdcEcm()) {
#else
    if (USB_ModeConfig_ActiveModeHasTargetCdcAcm() || USB_ModeConfig_ActiveModeHasTargetCdcEcm()) {
#endif
        USBFSD->UEP5_6_MOD = USBFS_UEP5_TX_EN | USBFS_UEP6_RX_EN;
        USBFSD_UEP_MOD( 3 ) = USBFS_UEP7_TX_EN;
    }
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    else {
        /* Host CDC ACM: EP5 INT IN, EP6 BULK OUT, EP7 BULK IN */
        USBFSD->UEP5_6_MOD = USBFS_UEP5_TX_EN | USBFS_UEP6_RX_EN;
        USBFSD_UEP_MOD( 3 ) = USBFS_UEP7_TX_EN;
    }
#else
    else {
        USBFSD->UEP5_6_MOD = 0;
        USBFSD_UEP_MOD( 3 ) = 0;
    }
#endif
#endif

    USBFSD->UEP0_DMA = (uint32_t)USBFS_EP0_Buf;
    USBFSD->UEP1_DMA = (uint32_t)USBFS_EP1_Buf;
    USBFSD->UEP2_DMA = (uint32_t)USBFS_EP2_Buf;
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    USBFSD->UEP3_DMA = (uint32_t)EP3_DatabufHD;  /* ECM host-side data OUT */
#else
    USBFSD->UEP3_DMA = (uint32_t)USBFS_EP3_Buf;
#endif
    USBFSD->UEP4_DMA = (uint32_t)USBFS_EP4_Buf;
#if defined(ENABLE_UAC) || USB_WORK_MODE_COMPILES_TARGET_CDC_ACM || USB_WORK_MODE_COMPILES_TARGET_CDC_ECM || USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
#ifdef ENABLE_UAC
    if (USB_ModeConfig_ActiveModeHasTargetUac() && !USB_ModeConfig_ActiveModeHasTargetCdcEcm()) {
        USBFSD->UEP5_DMA = (uint32_t)USBFS_EP5_Buf;
        USBFSD->UEP6_DMA = (uint32_t)USBFS_EP6_Buf;
    }
    else if (USB_ModeConfig_ActiveModeHasTargetCdcAcm() || USB_ModeConfig_ActiveModeHasTargetCdcEcm()) {
#else
    if (USB_ModeConfig_ActiveModeHasTargetCdcAcm() || USB_ModeConfig_ActiveModeHasTargetCdcEcm()) {
#endif
        USBFSD->UEP5_DMA = (uint32_t)EP5_DatabufHD;
        USBFSD->UEP6_DMA = (uint32_t)EP6_DatabufHD;
        USBFSD->UEP7_DMA = (uint32_t)EP7_DatabufHD;
    }
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    else {
        /* Host CDC ACM: EP5 INT IN, EP6 BULK OUT, EP7 BULK IN */
        USBFSD->UEP5_DMA = (uint32_t)EP5_DatabufHD;
        USBFSD->UEP6_DMA = (uint32_t)EP6_DatabufHD;
        USBFSD->UEP7_DMA = (uint32_t)EP7_DatabufHD;
    }
#endif
#endif

    USBFSD->UEP0_RX_CTRL = USBFS_UEP_R_RES_ACK;
    USBFSD->UEP0_TX_CTRL = USBFS_UEP_T_RES_NAK;
    USBFSD->UEP1_TX_CTRL = USBFS_UEP_T_RES_NAK;
    USBFSD->UEP2_TX_CTRL = USBFS_UEP_T_RES_NAK;
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    USBFSD->UEP3_RX_CTRL = USBFS_UEP_R_RES_ACK;  /* ECM host-side data OUT */
#else
    USBFSD->UEP3_TX_CTRL = USBFS_UEP_T_RES_NAK;
#endif
    USBFSD->UEP4_TX_CTRL = USBFS_UEP_T_RES_NAK;
#if defined(ENABLE_UAC) || USB_WORK_MODE_COMPILES_TARGET_CDC_ACM || USB_WORK_MODE_COMPILES_TARGET_CDC_ECM || USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    /* EP5 ISO IN: NAK until alt-setting 1 activated; TX_LEN=0 initially */
#ifdef ENABLE_UAC
    if (USB_ModeConfig_ActiveModeHasTargetUac() && !USB_ModeConfig_ActiveModeHasTargetCdcEcm()) {
        USBFSD->UEP5_TX_LEN  = 0;
        USBFSD->UEP5_TX_CTRL = USBFS_UEP_T_RES_NAK;
        USBFSD->UEP6_RX_CTRL = USBFS_UEP_R_RES_NAK;
    }
    else if (USB_ModeConfig_ActiveModeHasTargetCdcAcm() || USB_ModeConfig_ActiveModeHasTargetCdcEcm()) {
#else
    if (USB_ModeConfig_ActiveModeHasTargetCdcAcm() || USB_ModeConfig_ActiveModeHasTargetCdcEcm()) {
#endif
        USBFSD->UEP5_TX_LEN  = 0;
        USBFSD->UEP5_TX_CTRL = USBFS_UEP_T_RES_NAK;
        USBFSD->UEP6_RX_CTRL = USBFS_UEP_R_RES_ACK;
        USBFSD->UEP7_TX_LEN  = 0;
        USBFSD->UEP7_TX_CTRL = USBFS_UEP_T_RES_NAK;
    }
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    else {
        /* Host CDC ACM: EP5 INT IN, EP6 BULK OUT, EP7 BULK IN */
        USBFSD->UEP5_TX_LEN  = 0;
        USBFSD->UEP5_TX_CTRL = USBFS_UEP_T_RES_NAK;
        USBFSD->UEP6_RX_CTRL = USBFS_UEP_R_RES_ACK;
        USBFSD->UEP7_TX_LEN  = 0;
        USBFSD->UEP7_TX_CTRL = USBFS_UEP_T_RES_NAK;
    }
#endif
#endif

    /* Clear End-points Busy Status */
    for(uint8_t i=0; i<DEF_UEP_NUM; i++ )
    {
        USBFS_Endp_Busy[ i ] = 0;
    }    
}

/*********************************************************************
 * @fn      USBFS_Device_Init
 *
 * @brief   Initializes USB device.
 *
 * @return  none
 */
void USBFS_Device_Init( FunctionalState sta )
{
    if( sta )
    {
        /* Reset USB module */
        USBFSH->BASE_CTRL = USBFS_UC_RESET_SIE | USBFS_UC_CLR_ALL;
        Delay_Us( 10 );
        USBFSH->BASE_CTRL = 0;

        /* Initialize USB device configuration */
        USBFS_Device_Endp_Init( );
        USBFSD->INT_EN = USBFS_UIE_SUSPEND | USBFS_UIE_BUS_RST | USBFS_UIE_TRANSFER;

        /* Wait for power stability before enabling D+ pull-up */
        Delay_Ms( 100 );

        USBFSD->BASE_CTRL = USBFS_UC_DEV_PU_EN | USBFS_UC_INT_BUSY | USBFS_UC_DMA_EN;
        USBFSD->UDEV_CTRL = USBFS_UD_PD_DIS | USBFS_UD_PORT_EN;
        NVIC_EnableIRQ( USBFS_IRQn );
    }
    else
    {
        USBFSH->BASE_CTRL = USBFS_UC_RESET_SIE | USBFS_UC_CLR_ALL;
        Delay_Us( 10 );
        USBFSD->BASE_CTRL = 0;
        NVIC_DisableIRQ( USBFS_IRQn );
    }
}

/*********************************************************************
 * @fn      USBFS_Endp_DataUp
 *
 * @brief   usbfs device data upload
 *
 * @return  none
 */
uint8_t USBFS_Endp_DataUp(uint8_t endp, uint8_t *pbuf, uint16_t len, uint8_t mod)
{
    uint8_t endp_mode;
    uint8_t buf_load_offset;

    /* DMA config, endp_ctrl config, endp_len config */
    if( ( endp >= DEF_UEP1 ) && ( endp <= DEF_UEP7 ) )
    {
        if( USBFS_Endp_Busy[ endp ] == 0 )
        {
            if( (endp == DEF_UEP1) || (endp == DEF_UEP4) )
            {
                /* endp1/endp4 */
                endp_mode = USBFSD_UEP_MOD( 0 );
                if( endp == DEF_UEP1 )
                {
                    endp_mode = (uint8_t)( endp_mode >> 4 );
                }
            }
            else if( ( endp == DEF_UEP2 ) || ( endp == DEF_UEP3 ) )
            {
                /* endp2/endp3 */
                endp_mode = USBFSD_UEP_MOD( 1 );
                if( endp == DEF_UEP3 )
                {
                    endp_mode = (uint8_t)( endp_mode >> 4 );
                }
            }
            else if( ( endp == DEF_UEP5 ) || ( endp == DEF_UEP6 ) )
            {
                /* endp5/endp6 */
                endp_mode = USBFSD_UEP_MOD( 2 );
                if( endp == DEF_UEP6 )
                {
                    endp_mode = (uint8_t)( endp_mode >> 4 );
                }
            }
            else
            {
                /* endp7 */
                endp_mode = USBFSD_UEP_MOD( 3 );
            }

            if( endp_mode & USBFSD_UEP_TX_EN )
            {
                if( endp_mode & USBFSD_UEP_RX_EN )
                {
                    if( endp_mode & USBFSD_UEP_BUF_MOD )
                    {
                        if( USBFSD_UEP_TX_CTRL(endp) & USBFS_UEP_T_TOG )
                        {
                            buf_load_offset = 192;
                        }
                        else
                        {
                            buf_load_offset = 128;
                        }
                    }
                    else
                    {
                        buf_load_offset = 64;
                    }
                }
                else
                {
                    if( endp_mode & USBFSD_UEP_BUF_MOD )
                    {
                        /* double tx buffer */
                        if( USBFSD_UEP_TX_CTRL( endp ) & USBFS_UEP_T_TOG )
                        {
                            buf_load_offset = 64;
                        }
                        else
                        {
                            buf_load_offset = 0;
                        }
                    }
                    else
                    {
                        buf_load_offset = 0;
                    }
                }
                if( buf_load_offset == 0 )
                {
                    if( mod == DEF_UEP_DMA_LOAD )
                    {
                        /* DMA mode */
                        USBFSD_UEP_DMA( endp ) = (uint16_t)(uint32_t)pbuf;
                    }
                    else
                    {
                        /* copy mode */
                        memcpy( USBFSD_UEP_BUF( endp ), pbuf, len );
                    }
                }
                else
                {
                    memcpy( USBFSD_UEP_BUF( endp ) + buf_load_offset, pbuf, len );
                }
                /* Set end-point busy */
                USBFS_Endp_Busy[ endp ] = 0x01;
                /* Record time when busy flag was set for timeout detection */
                USBFS_Endp_Busy_StartTime[ endp ] = systick_ms;
                /* tx length */
                USBFSD_UEP_TLEN( endp ) = len;
                /* response ack */
                USBFSD_UEP_TX_CTRL( endp ) = ( USBFSD_UEP_TX_CTRL( endp ) & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_ACK;
            }
        }
        else
        {
            return NoREADY;
        }
    }
    else
    {
        return NoREADY;
    }
    return READY;
}

/*********************************************************************
 * @fn      USBFS_IRQHandler
 *
 * @brief   USB device transfer process.
 *
 * @return  none
 */
void USBFS_IRQHandler( void )
{
    uint8_t  intflag, intst, errflag;
    uint16_t len;

    intflag = USBFSD->INT_FG;
    intst = USBFSD->INT_ST;

    if( intflag & USBFS_UIF_TRANSFER )
    {
        switch( intst & USBFS_UIS_TOKEN_MASK )
        {
            /* data-in stage processing */
            case USBFS_UIS_TOKEN_IN:
                switch( intst & ( USBFS_UIS_TOKEN_MASK | USBFS_UIS_ENDP_MASK ) )
                {
                    /* end-point 0 data in interrupt */
                    case USBFS_UIS_TOKEN_IN | DEF_UEP0:
                        if( USBFS_SetupReqLen == 0 )
                        {
                            USBFSD->UEP0_RX_CTRL = USBFS_UEP_R_TOG | USBFS_UEP_R_RES_ACK;
                        }

                        if ( ( USBFS_SetupReqType & USB_REQ_TYP_MASK ) != USB_REQ_TYP_STANDARD )
                        {
                            /* Non-standard request endpoint 0 Data upload */
                        }
                        else
                        {
                            switch( USBFS_SetupReqCode )
                            {
                                case USB_GET_DESCRIPTOR:
                                    len = USBFS_SetupReqLen >= DEF_USBD_UEP0_SIZE ? DEF_USBD_UEP0_SIZE : USBFS_SetupReqLen;
                                    memcpy( USBFS_EP0_Buf, pUSBFS_Descr, len );
                                    USBFS_SetupReqLen -= len;
                                    pUSBFS_Descr += len;
                                    USBFSD->UEP0_TX_LEN = len;
                                    USBFSD->UEP0_TX_CTRL ^= USBFS_UEP_T_TOG;
                                    break;

                                case USB_SET_ADDRESS:
                                    USBFSD->DEV_ADDR = ( USBFSD->DEV_ADDR & USBFS_UDA_GP_BIT ) | USBFS_DevAddr;
                                    break;

                                default:
                                    break;
                            }
                        }
                        break;

                    /* end-point 1 data in interrupt */
                    case USBFS_UIS_TOKEN_IN | DEF_UEP1:
                        USBFSD->UEP1_TX_CTRL = ( USBFSD->UEP1_TX_CTRL & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_NAK;
                        USBFSD->UEP1_TX_CTRL ^= USBFS_UEP_T_TOG;
                        USBFS_Endp_Busy[ DEF_UEP1 ] = 0;
                        break;

                    /* end-point 2 data in interrupt */
                    case USBFS_UIS_TOKEN_IN | DEF_UEP2:
                        USBFSD->UEP2_TX_CTRL = ( USBFSD->UEP2_TX_CTRL & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_NAK;
                        USBFSD->UEP2_TX_CTRL ^= USBFS_UEP_T_TOG;
                        USBFS_Endp_Busy[ DEF_UEP2 ] = 0;
                        break;

                    /* end-point 3 data in interrupt */
                    case USBFS_UIS_TOKEN_IN | DEF_UEP3:
                        USBFSD->UEP3_TX_CTRL = ( USBFSD->UEP3_TX_CTRL & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_NAK;
                        USBFSD->UEP3_TX_CTRL ^= USBFS_UEP_T_TOG;
                        USBFS_Endp_Busy[ DEF_UEP3 ] = 0;
                        break;

                    /* end-point 4 data in interrupt */
                    case USBFS_UIS_TOKEN_IN | DEF_UEP4:
                        USBFSD->UEP4_TX_CTRL = ( USBFSD->UEP4_TX_CTRL & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_NAK;
                        USBFSD->UEP4_TX_CTRL ^= USBFS_UEP_T_TOG;
                        USBFS_Endp_Busy[ DEF_UEP4 ] = 0;
                        break;

#if defined(ENABLE_UAC) || USB_WORK_MODE_COMPILES_TARGET_CDC_ACM || USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
                    case USBFS_UIS_TOKEN_IN | DEF_UEP5:
#ifdef ENABLE_UAC
                        if (USB_ModeConfig_ActiveModeHasTargetUac()) {
                            UAC_USBFS_EP5_TxDone();
                        }
                        else
#endif
                        {
                        USBFSD->UEP5_TX_CTRL = ( USBFSD->UEP5_TX_CTRL & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_NAK;
                        USBFSD->UEP5_TX_CTRL ^= USBFS_UEP_T_TOG;
                        USBFS_Endp_Busy[ DEF_UEP5 ] = 0;
                        }
                        break;

#if USB_WORK_MODE_COMPILES_TARGET_CDC_ACM || USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
                    case USBFS_UIS_TOKEN_IN | DEF_UEP7:
                        g_usbfs_ep7_in_count++;
                        USBFSD->UEP7_TX_CTRL = ( USBFSD->UEP7_TX_CTRL & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_NAK;
                        USBFSD->UEP7_TX_CTRL ^= USBFS_UEP_T_TOG;
                        USBFS_Endp_Busy[ DEF_UEP7 ] = 0;
                        break;
#endif
#endif

                    default :
                        break;
                }
                break;

            /* data-out stage processing */
            case USBFS_UIS_TOKEN_OUT:
                switch( intst & ( USBFS_UIS_TOKEN_MASK | USBFS_UIS_ENDP_MASK ) )
                {
                    /* end-point 0 data out interrupt */
                    case USBFS_UIS_TOKEN_OUT | DEF_UEP0:
                        if( intst & USBFS_UIS_TOG_OK )
                        {
                            if( ( USBFS_SetupReqType & USB_REQ_TYP_MASK ) != USB_REQ_TYP_STANDARD )
                            {
                                if( ( USBFS_SetupReqType & USB_REQ_TYP_MASK ) == USB_REQ_TYP_CLASS )
                                {
                                    switch( USBFS_SetupReqCode )
                                    {
                                        case HID_SET_REPORT:
                                            if( ( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == USBFS_MODE_HID_IF ) &&
                                                ( (uint8_t)( USBFS_SetupReqValue >> 8 ) == USBFS_HID_REPORT_TYPE_FEATURE ) &&
                                                ( (uint8_t)( USBFS_SetupReqValue & 0xFF ) == USBFS_MODE_HID_REPORT_ID ) )
                                            {
                                                USBFS_ModeHid_HandleSetReport( USBFS_EP0_Buf, USBFS_SetupReqLen );
                                            }
                                            else if( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == 0x00 )
                                            {
                                                Keyboard_SetLEDStatus(USBFS_EP0_Buf[ 0 ]);
                                            }
                                            USBFS_SetupReqLen = 0;
                                            break;
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ACM || USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
                                        case USBFS_CDC_ACM_REQ_SET_LINE:
                                            if( USBFS_SetupReqLen > sizeof( USBFS_CDC_LineCoding ) )
                                            {
                                                USBFS_SetupReqLen = sizeof( USBFS_CDC_LineCoding );
                                            }
                                            memcpy( USBFS_CDC_LineCoding, USBFS_EP0_Buf, USBFS_SetupReqLen );
                                            USBFS_SetupReqLen = 0;
                                            break;
#endif
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
                                        case USBFS_CDC_ECM_REQ_SET_MC_FILTERS:
                                        case USBFS_CDC_ECM_REQ_SET_PM_PATTERN:
                                            if( USBFS_SetupReqLen > sizeof( USBFS_CDC_ECM_CtrlBuf ) )
                                            {
                                                USBFS_SetupReqLen = sizeof( USBFS_CDC_ECM_CtrlBuf );
                                            }
                                            memcpy( USBFS_CDC_ECM_CtrlBuf, USBFS_EP0_Buf, USBFS_SetupReqLen );
                                            USBFS_SetupReqLen = 0;
                                            break;
#endif
#ifdef ENABLE_UAC
                                        case 0x01:  /* UAC SET_CUR */
                                            if( USB_ModeConfig_ActiveModeHasTargetUac() &&
                                                ( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP ) &&
                                                ( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == UAC_USBFS_EP_IN_ADDR ||
                                                  (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == UAC_USBFS_EP_OUT_ADDR ) &&
                                                ( (uint8_t)( USBFS_SetupReqValue >> 8 ) == 0x01 ) )
                                            {
                                                USBFS_SetupReqLen = 0;
                                            }
                                            break;
#endif /* ENABLE_UAC */
                                        default:
                                            break;
                                    }
                                }
                            }
                            else
                            {
                                /* Standard request end-point 0 Data download */
                                /* Add your code here */
                            }
                        }
                        if( USBFS_SetupReqLen == 0 )
                        {
                            USBFSD->UEP0_TX_LEN  = 0;
                            USBFSD->UEP0_TX_CTRL = USBFS_UEP_T_TOG | USBFS_UEP_T_RES_ACK;
                        }
                        break;

#if defined(ENABLE_UAC) || USB_WORK_MODE_COMPILES_TARGET_CDC_ACM || USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
                    case USBFS_UIS_TOKEN_OUT | DEF_UEP6:
#ifdef ENABLE_UAC
                        if (USB_ModeConfig_ActiveModeHasTargetUac()) {
                            UAC_USBFS_EP6_RxDone( USBFSD->RX_LEN );
                        }
                        else
#endif
                        {
                        /* Diagnostic: capture EP6 OUT interrupt state for main-loop LED display */
                        g_usbfs_ep6_int_flag = 1;
                        g_usbfs_ep6_tog_ok = (intst & USBFS_UIS_TOG_OK) ? 1 : 0;
                        g_usbfs_ep6_rx_len = USBFSD->RX_LEN;
                        g_usbfs_ep6_rx_count++;  /* count every OUT interrupt */

                        if( intst & USBFS_UIS_TOG_OK )
                        {
                            if (USB_ModeConfig_ActiveModeBridgeAcm()) {
                                ACM_Bridge_TargetRxDone( EP6_DatabufHD, USBFSD->RX_LEN );
                            }
                            else if (USB_ModeConfig_ActiveModeBridgeEcm()) {
                                ECM_Bridge_TargetRxDone( EP6_DatabufHD, USBFSD->RX_LEN );
                            }
                            USBFSD->UEP6_RX_CTRL ^= USBFS_UEP_R_TOG;
                        }
                        USBFSD->UEP6_RX_CTRL = ( USBFSD->UEP6_RX_CTRL & ~USBFS_UEP_R_RES_MASK ) | USBFS_UEP_R_RES_ACK;
                        }
                        break;

#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
                    case USBFS_UIS_TOKEN_OUT | DEF_UEP3:
                        /* ECM host-side data OUT (host to device) - combined ACM+ECM mode */
                        if( intst & USBFS_UIS_TOG_OK )
                        {
                            ECM_Bridge_HostRxDone( EP3_DatabufHD, USBFSD->RX_LEN );
                            USBFSD->UEP3_RX_CTRL ^= USBFS_UEP_R_TOG;
                        }
                        USBFSD->UEP3_RX_CTRL = ( USBFSD->UEP3_RX_CTRL & ~USBFS_UEP_R_RES_MASK ) | USBFS_UEP_R_RES_ACK;
                        break;
#endif
#endif

                    default:
                        break;
                }
                break;

            /* Setup stage processing */
            case USBFS_UIS_TOKEN_SETUP:
                USBFSD->UEP0_TX_CTRL = USBFS_UEP_T_TOG | USBFS_UEP_T_RES_NAK;
                USBFSD->UEP0_RX_CTRL = USBFS_UEP_R_TOG | USBFS_UEP_R_RES_NAK;

                /* Store All Setup Values */
                USBFS_SetupReqType  = pUSBFS_SetupReqPak->bRequestType;
                USBFS_SetupReqCode  = pUSBFS_SetupReqPak->bRequest;
                USBFS_SetupReqLen   = pUSBFS_SetupReqPak->wLength;
                USBFS_SetupReqValue = pUSBFS_SetupReqPak->wValue;
                USBFS_SetupReqIndex = pUSBFS_SetupReqPak->wIndex;
                len = 0;
                errflag = 0;

                if( ( USBFS_SetupReqType & USB_REQ_TYP_MASK ) != USB_REQ_TYP_STANDARD )
                {
                    if( ( USBFS_SetupReqType & USB_REQ_TYP_MASK ) == USB_REQ_TYP_CLASS )
                    {
                        /* Class Request */
                        switch( USBFS_SetupReqCode )
                        {
                            case HID_SET_REPORT:
                                if( ( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) != 0x00 ) &&
                                    !( ( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == USBFS_MODE_HID_IF ) &&
                                       ( (uint8_t)( USBFS_SetupReqValue >> 8 ) == USBFS_HID_REPORT_TYPE_FEATURE ) &&
                                       ( (uint8_t)( USBFS_SetupReqValue & 0xFF ) == USBFS_MODE_HID_REPORT_ID ) ) )
                                {
                                    errflag = 0xFF;
                                }
                                break;

                            case HID_GET_REPORT:
                                if( ( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == USBFS_MODE_HID_IF ) &&
                                    ( (uint8_t)( USBFS_SetupReqValue >> 8 ) == USBFS_HID_REPORT_TYPE_FEATURE ) &&
                                    ( (uint8_t)( USBFS_SetupReqValue & 0xFF ) == USBFS_MODE_HID_REPORT_ID ) )
                                {
                                    USBFS_ModeHid_FillReport( USBFS_EP0_Buf );
                                    USBFS_SetupReqLen = USBFS_MODE_HID_REPORT_LEN;
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                                break;

                            case HID_SET_IDLE:
                                if( USBFS_SetupReqIndex == 0x00 )
                                {
                                    USBFS_HidIdle[ 0 ] = (uint8_t)( USBFS_SetupReqValue >> 8 );
                                }
                                else if( USBFS_SetupReqIndex == 0x01 )
                                {
                                    USBFS_HidIdle[ 1 ] = (uint8_t)( USBFS_SetupReqValue >> 8 );
                                }
                                else if( USBFS_SetupReqIndex == 0x02 )
                                {
                                    USBFS_HidIdle[ 2 ] = (uint8_t)( USBFS_SetupReqValue >> 8 );
                                }
                                else if( USBFS_SetupReqIndex == 0x03 )
                                {
                                    USBFS_HidIdle[ 3 ] = (uint8_t)( USBFS_SetupReqValue >> 8 );
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                                break;

                            case HID_SET_PROTOCOL:
                                if( USBFS_SetupReqIndex == 0x00 )
                                {
                                    USBFS_HidProtocol[ 0 ] = (uint8_t)USBFS_SetupReqValue;
                                }
                                else if( USBFS_SetupReqIndex == 0x01 )
                                {
                                    USBFS_HidProtocol[ 1 ] = (uint8_t)USBFS_SetupReqValue;
                                }
                                else if( USBFS_SetupReqIndex == 0x02 )
                                {
                                    USBFS_HidProtocol[ 2 ] = (uint8_t)USBFS_SetupReqValue;
                                }
                                else if( USBFS_SetupReqIndex == 0x03 )
                                {
                                    USBFS_HidProtocol[ 3 ] = (uint8_t)USBFS_SetupReqValue;
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                                break;

                            case HID_GET_IDLE:
                                if( USBFS_SetupReqIndex == 0x00 )
                                {
                                    USBFS_EP0_Buf[ 0 ] = USBFS_HidIdle[ 0 ];
                                    len = 1;
                                }
                                else if( USBFS_SetupReqIndex == 0x01 )
                                {
                                    USBFS_EP0_Buf[ 0 ] = USBFS_HidIdle[ 1 ];
                                    len = 1;
                                }
                                else if( USBFS_SetupReqIndex == 0x02 )
                                {
                                    USBFS_EP0_Buf[ 0 ] = USBFS_HidIdle[ 2 ];
                                    len = 1;
                                }
                                else if( USBFS_SetupReqIndex == 0x03 )
                                {
                                    USBFS_EP0_Buf[ 0 ] = USBFS_HidIdle[ 3 ];
                                    len = 1;
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                                break;

                            case HID_GET_PROTOCOL:
                                if( USBFS_SetupReqIndex == 0x00 )
                                {
                                    USBFS_EP0_Buf[ 0 ] = USBFS_HidProtocol[ 0 ];
                                    len = 1;
                                }
                                else if( USBFS_SetupReqIndex == 0x01 )
                                {
                                    USBFS_EP0_Buf[ 0 ] = USBFS_HidProtocol[ 1 ];
                                    len = 1;
                                }
                                else if( USBFS_SetupReqIndex == 0x02 )
                                {
                                    USBFS_EP0_Buf[ 0 ] = USBFS_HidProtocol[ 2 ];
                                    len = 1;
                                }
                                else if( USBFS_SetupReqIndex == 0x03 )
                                {
                                    USBFS_EP0_Buf[ 0 ] = USBFS_HidProtocol[ 3 ];
                                    len = 1;
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                                break;

#if USB_WORK_MODE_COMPILES_TARGET_CDC_ACM || USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
                            case USBFS_CDC_ACM_REQ_SET_LINE:
                                if( USB_ModeConfig_ActiveModeHasTargetCdcAcm() &&
                                    ( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_INTERF ) &&
                                    ( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == USBFS_CDC_ACM_IF_COMM ) )
                                {
                                    if( USBFS_SetupReqLen > sizeof( USBFS_CDC_LineCoding ) )
                                    {
                                        USBFS_SetupReqLen = sizeof( USBFS_CDC_LineCoding );
                                    }
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                                break;

                            case USBFS_CDC_ACM_REQ_GET_LINE:
                                if( USB_ModeConfig_ActiveModeHasTargetCdcAcm() &&
                                    ( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_INTERF ) &&
                                    ( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == USBFS_CDC_ACM_IF_COMM ) )
                                {
                                    memcpy( USBFS_EP0_Buf, USBFS_CDC_LineCoding, sizeof( USBFS_CDC_LineCoding ) );
                                    if( USBFS_SetupReqLen > sizeof( USBFS_CDC_LineCoding ) )
                                    {
                                        USBFS_SetupReqLen = sizeof( USBFS_CDC_LineCoding );
                                    }
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                                break;

                            case USBFS_CDC_ACM_REQ_SET_CONTROL:
                                if( USB_ModeConfig_ActiveModeHasTargetCdcAcm() &&
                                    ( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_INTERF ) &&
                                    ( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == USBFS_CDC_ACM_IF_COMM ) )
                                {
                                    USBFS_CDC_ControlLineState = USBFS_SetupReqValue;
                                    USBFS_SetupReqLen = 0;
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                                break;
#endif

#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
                            case USBFS_CDC_ECM_REQ_SET_PACKET:
                                if( USB_ModeConfig_ActiveModeHasTargetCdcEcm() &&
                                    ( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_INTERF ) &&
                                    ( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == USBFS_CDC_ECM_IF_COMM ) )
                                {
                                    USBFS_CDC_ECM_PacketFilter = USBFS_SetupReqValue;
                                    USBFS_SetupReqLen = 0;
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                                break;

                            case USBFS_CDC_ECM_REQ_GET_STATISTIC:
                            case USBFS_CDC_ECM_REQ_GET_PM_PATTERN:
                                if( USB_ModeConfig_ActiveModeHasTargetCdcEcm() &&
                                    ( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_INTERF ) &&
                                    ( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == USBFS_CDC_ECM_IF_COMM ) )
                                {
                                    memcpy( USBFS_EP0_Buf, USBFS_CDC_ECM_Stat, sizeof( USBFS_CDC_ECM_Stat ) );
                                    if( USBFS_SetupReqLen > sizeof( USBFS_CDC_ECM_Stat ) )
                                    {
                                        USBFS_SetupReqLen = sizeof( USBFS_CDC_ECM_Stat );
                                    }
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                                break;

                            case USBFS_CDC_ECM_REQ_SET_MC_FILTERS:
                            case USBFS_CDC_ECM_REQ_SET_PM_PATTERN:
                                if( USB_ModeConfig_ActiveModeHasTargetCdcEcm() &&
                                    ( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_INTERF ) &&
                                    ( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == USBFS_CDC_ECM_IF_COMM ) )
                                {
                                    if( USBFS_SetupReqLen > sizeof( USBFS_CDC_ECM_CtrlBuf ) )
                                    {
                                        USBFS_SetupReqLen = sizeof( USBFS_CDC_ECM_CtrlBuf );
                                    }
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                                break;
#endif

                            default:
#ifdef ENABLE_UAC
                                                                /* UAC 1.0 endpoint sampling-frequency control. */
                                                                if( USB_ModeConfig_ActiveModeHasTargetUac() &&
                                                                    ( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP ) &&
                                                                        ( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == UAC_USBFS_EP_IN_ADDR ||
                                                                            (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == UAC_USBFS_EP_OUT_ADDR ) &&
                                                                        ( (uint8_t)( USBFS_SetupReqValue >> 8 ) == 0x01 ) &&
                                                                        ( USBFS_SetupReqCode >= 0x81 && USBFS_SetupReqCode <= 0x84 ) )
                                {
                                    USBFS_EP0_Buf[0] = 0x40;  /* 16000 Hz = 0x003E40, LSB first */
                                    USBFS_EP0_Buf[1] = 0x3E;
                                    USBFS_EP0_Buf[2] = 0x00;
                                    if( USBFS_SetupReqLen > 3 ) USBFS_SetupReqLen = 3;
                                }
                                                                else if( USB_ModeConfig_ActiveModeHasTargetUac() &&
                                                                                 ( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP ) &&
                                                                                 ( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == UAC_USBFS_EP_IN_ADDR ||
                                                                                     (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == UAC_USBFS_EP_OUT_ADDR ) &&
                                                                                 ( (uint8_t)( USBFS_SetupReqValue >> 8 ) == 0x01 ) &&
                                                                                 ( USBFS_SetupReqCode == 0x01 ) )
                                {
                                                                        /* SET_CUR data stage is accepted in the EP0 OUT handler. */
                                }
                                else
#endif /* ENABLE_UAC */
                                {
                                    errflag = 0xFF;
                                }
                                break;
                        }
                    }
                }
                else
                {
                    /* usb standard request processing */
                    switch( USBFS_SetupReqCode )
                    {
                        /* get device/configuration/string/report/... descriptors */
                        case USB_GET_DESCRIPTOR:
                            switch( (uint8_t)( USBFS_SetupReqValue >> 8 ) )
                            {
                                /* get usb device descriptor */
                                case USB_DESCR_TYP_DEVICE:
                                    pUSBFS_Descr = MyDevDescr;
                                    len = DEF_USBD_DEVICE_DESC_LEN;
                                    break;

                                /* get usb configuration descriptor */
                                case USB_DESCR_TYP_CONFIG:
                                    pUSBFS_Descr = USBFS_GetActiveConfigDescriptorData(&len);
                                    break;

                                /* get usb hid descriptor */
                                case USB_DESCR_TYP_HID:
                                    if( USBFS_SetupReqIndex == 0x00 )
                                    {
                                        pUSBFS_Descr = &MyCfgDescr[ 18 ];
                                        len = 9;
                                    }
                                    else if( USBFS_SetupReqIndex == 0x01 )
                                    {
                                        pUSBFS_Descr = &MyCfgDescr[ 43 ];
                                        len = 9;
                                    }
                                    else if( USBFS_SetupReqIndex == 0x02 )
                                    {
                                        pUSBFS_Descr = &MyCfgDescr[ 68 ];  // HID descriptor for absolute mouse
                                        len = 9;
                                    }
                                    else if( USBFS_SetupReqIndex == 0x03 )
                                    {
                                        pUSBFS_Descr = &MyCfgDescr[ 93 ];  // HID descriptor for media keys
                                        len = 9;
                                    }
                                    else
                                    {
                                        errflag = 0xFF;
                                    }
                                    break;

                                /* get usb report descriptor */
                                case USB_DESCR_TYP_REPORT:
                                    if( USBFS_SetupReqIndex == 0x00 )
                                    {
                                        pUSBFS_Descr = KeyRepDesc;
                                        len = DEF_USBD_REPORT_DESC_LEN_KB;
                                    }
                                    else if( USBFS_SetupReqIndex == 0x01 )
                                    {
                                        pUSBFS_Descr = MouseRepDesc;
                                        len = DEF_USBD_REPORT_DESC_LEN_REL_MS;
                                    }
                                    else if( USBFS_SetupReqIndex == 0x02 )
                                    {
                                        pUSBFS_Descr = AbsMouseRepDesc;
                                        len = DEF_USBD_REPORT_DESC_LEN_ABS_MS;
                                    }
                                    else if( USBFS_SetupReqIndex == 0x03 )
                                    {
                                        pUSBFS_Descr = MediaKeyRepDesc;
                                        len = DEF_USBD_REPORT_DESC_LEN_MEDIA;
                                    }
                                    else
                                    {
                                        errflag = 0xFF;
                                    }
                                    break;

                                /* get usb string descriptor */
                                case USB_DESCR_TYP_STRING:
                                    switch( (uint8_t)( USBFS_SetupReqValue & 0xFF ) )
                                    {
                                        /* Descriptor 0, Language descriptor */
                                        case DEF_STRING_DESC_LANG:
                                            pUSBFS_Descr = MyLangDescr;
                                            len = DEF_USBD_LANG_DESC_LEN;
                                            break;

                                        /* Descriptor 1, Manufacturers String descriptor */
                                        case DEF_STRING_DESC_MANU:
                                            pUSBFS_Descr = MyManuInfo;
                                            len = DEF_USBD_MANU_DESC_LEN;
                                            break;

                                        /* Descriptor 2, Product String descriptor */
                                        case DEF_STRING_DESC_PROD:
                                            pUSBFS_Descr = MyProdInfo;
                                            len = DEF_USBD_PROD_DESC_LEN;
                                            break;

                                        /* Descriptor 3, Serial-number String descriptor */
                                        case DEF_STRING_DESC_SERN:
                                            pUSBFS_Descr = MySerNumInfo;
                                            len = DEF_USBD_SN_DESC_LEN;
                                            break;

#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
                                        case 0x04:
                                            if( USB_ModeConfig_ActiveModeHasTargetCdcEcm() )
                                            {
                                                pUSBFS_Descr = MyEcmMacInfo;
                                                len = DEF_USBD_ECM_MAC_DESC_LEN;
                                            }
                                            else
                                            {
                                                errflag = 0xFF;
                                            }
                                            break;
#endif

                                        default:
                                            errflag = 0xFF;
                                            break;
                                    }
                                    break;

                                default :
                                    errflag = 0xFF;
                                    break;
                            }

                            /* Copy Descriptors to Endp0 DMA buffer */
                            if( USBFS_SetupReqLen > len )
                            {
                                USBFS_SetupReqLen = len;
                            }
                            len = ( USBFS_SetupReqLen >= DEF_USBD_UEP0_SIZE ) ? DEF_USBD_UEP0_SIZE : USBFS_SetupReqLen;
                            memcpy( USBFS_EP0_Buf, pUSBFS_Descr, len );
                            pUSBFS_Descr += len;
                            break;

                        /* Set usb address */
                        case USB_SET_ADDRESS:
                            USBFS_DevAddr = (uint8_t)( USBFS_SetupReqValue & 0xFF );
                            break;

                        /* Get usb configuration now set */
                        case USB_GET_CONFIGURATION:
                            USBFS_EP0_Buf[ 0 ] = USBFS_DevConfig;
                            if( USBFS_SetupReqLen > 1 )
                            {
                                USBFS_SetupReqLen = 1;
                            }
                            break;

                        /* Set usb configuration to use */
                        case USB_SET_CONFIGURATION:
                            USBFS_DevConfig = (uint8_t)( USBFS_SetupReqValue & 0xFF );
                            USBFS_DevEnumStatus = 0x01;
                            USBFS_Connect = 0x01;
                            break;

                        /* Clear or disable one usb feature */
                        case USB_CLEAR_FEATURE:
                            if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_DEVICE )
                            {
                                /* clear one device feature */
                                if( (uint8_t)( USBFS_SetupReqValue & 0xFF ) == USB_REQ_FEAT_REMOTE_WAKEUP )
                                {
                                    /* clear usb sleep status, device not prepare to sleep */
                                    USBFS_DevSleepStatus &= ~0x01;
                                }
                            	else
                                {
                                    errflag = 0xFF;
                                }
							}
                            else if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP )
                            {
                                if( (uint8_t)( USBFS_SetupReqValue & 0xFF ) == USB_REQ_FEAT_ENDP_HALT )
                                {
                                    /* Clear End-point Feature */
                                    switch( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) )
                                    {
                                        case ( DEF_UEP_IN | DEF_UEP1 ):
                                            /* Set End-point 1 OUT ACK */
                                            USBFSD->UEP1_TX_CTRL = USBFS_UEP_T_RES_NAK;
                                            break;

                                        case ( DEF_UEP_IN | DEF_UEP2 ):
                                            /* Set End-point 2 IN NAK */
                                            USBFSD->UEP2_TX_CTRL = USBFS_UEP_T_RES_NAK;
                                            break;

                                        case ( DEF_UEP_IN | DEF_UEP3 ):
                                            /* Set End-point 3 IN NAK */
                                            USBFSD->UEP3_TX_CTRL = USBFS_UEP_T_RES_NAK;
                                            break;

                                        case ( DEF_UEP_IN | DEF_UEP4 ):
                                            /* Set End-point 4 IN NAK */
                                            USBFSD->UEP4_TX_CTRL = USBFS_UEP_T_RES_NAK;
                                            break;

#if USB_WORK_MODE_COMPILES_TARGET_CDC_ACM || USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
                                        case ( DEF_UEP_IN | DEF_UEP5 ):
                                            USBFSD->UEP5_TX_CTRL = USBFS_UEP_T_RES_NAK;
                                            break;

                                        case ( DEF_UEP_OUT | DEF_UEP6 ):
                                            USBFSD->UEP6_RX_CTRL = USBFS_UEP_R_RES_ACK;
                                            break;

                                        case ( DEF_UEP_IN | DEF_UEP7 ):
                                            USBFSD->UEP7_TX_CTRL = USBFS_UEP_T_RES_NAK;
                                            break;
#endif

                                        default:
                                            errflag = 0xFF;
                                            break;
                                    }
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                            }
                            else
                            {
                                errflag = 0xFF;
                            }
                            break;

                        /* set or enable one usb feature */
                        case USB_SET_FEATURE:
                            if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_DEVICE )
                            {
                                /* Set Device Feature */
                                if( (uint8_t)( USBFS_SetupReqValue & 0xFF ) == USB_REQ_FEAT_REMOTE_WAKEUP )
                                {
                                    if( USBFS_GetActiveConfigAttributes() & 0x20 )
                                    {
                                        /* Set Wake-up flag, device prepare to sleep */
                                        USBFS_DevSleepStatus |= 0x01;
                                    }
                                    else
                                    {
                                        errflag = 0xFF;
                                    }
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                            }
                            else if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP )
                            {
                                /* Set Endpoint Feature */
                                if( (uint8_t)( USBFS_SetupReqValue & 0xFF ) == USB_REQ_FEAT_ENDP_HALT )
                                {
                                    switch( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) )
                                    {
                                        case ( DEF_UEP_IN | DEF_UEP1 ):
                                            USBFSD->UEP1_TX_CTRL = ( USBFSD->UEP1_TX_CTRL & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_STALL;
                                            break;

                                        case ( DEF_UEP_IN | DEF_UEP2 ):
                                            USBFSD->UEP2_TX_CTRL = ( USBFSD->UEP2_TX_CTRL & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_STALL;
                                            break;

                                        case ( DEF_UEP_IN | DEF_UEP3 ):
                                            USBFSD->UEP3_TX_CTRL = ( USBFSD->UEP3_TX_CTRL & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_STALL;
                                            break;

                                        case ( DEF_UEP_IN | DEF_UEP4 ):
                                            USBFSD->UEP4_TX_CTRL = ( USBFSD->UEP4_TX_CTRL & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_STALL;
                                            break;

#if USB_WORK_MODE_COMPILES_TARGET_CDC_ACM || USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
                                        case ( DEF_UEP_IN | DEF_UEP5 ):
                                            USBFSD->UEP5_TX_CTRL = ( USBFSD->UEP5_TX_CTRL & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_STALL;
                                            break;

                                        case ( DEF_UEP_OUT | DEF_UEP6 ):
                                            USBFSD->UEP6_RX_CTRL = ( USBFSD->UEP6_RX_CTRL & ~USBFS_UEP_R_RES_MASK ) | USBFS_UEP_R_RES_STALL;
                                            break;

                                        case ( DEF_UEP_IN | DEF_UEP7 ):
                                            USBFSD->UEP7_TX_CTRL = ( USBFSD->UEP7_TX_CTRL & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_STALL;
                                            break;
#endif

                                        default:
                                            errflag = 0xFF;
                                            break;
                                    }
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                            }
                            else
                            {
                                errflag = 0xFF;
                            }
                            break;

                        /* This request allows the host to select another setting for the specified interface  */
                        case USB_GET_INTERFACE:
#ifdef ENABLE_UAC
                            if( USB_ModeConfig_ActiveModeHasTargetUac() && USBFS_SetupReqIndex == 5 )
                                USBFS_EP0_Buf[ 0 ] = g_uac_state.usbfs_as_in_alt;
                            else if( USB_ModeConfig_ActiveModeHasTargetUac() && USBFS_SetupReqIndex == 6 )
                                USBFS_EP0_Buf[ 0 ] = g_uac_state.usbfs_as_out_alt;
                            else
#endif /* ENABLE_UAC */
                                USBFS_EP0_Buf[ 0 ] = 0x00;
                            if( USBFS_SetupReqLen > 1 )
                            {
                                USBFS_SetupReqLen = 1;
                            }
                            break;

                        case USB_SET_INTERFACE:
#ifdef ENABLE_UAC
                            /* UAC AudioStreaming interfaces: IF5=AS_IN (EP5), IF6=AS_OUT (EP6) */
                            if( USB_ModeConfig_ActiveModeHasTargetUac() && USBFS_SetupReqIndex == 5 )
                            {
                                /* IF5 alternate setting: 0=idle, 1=active ISO IN */
                                g_uac_state.usbfs_as_in_alt = (uint8_t)(USBFS_SetupReqValue & 0xFF);
                                if( g_uac_state.usbfs_as_in_alt == 1 )
                                {
                                    /* Pre-fill both ping-pong halves so the very first
                                     * SOF fires a valid frame, not silence/garbage. */
                                    UAC_USBFS_EP5_Preload();
                                    USBFSD->UEP5_TX_LEN  = UAC_ISO_PACKET_SIZE;
                                    /* ISO IN: T_RES_NONE — host never sends ACK for isochronous */
                                    USBFSD->UEP5_TX_CTRL = ( USBFSD->UEP5_TX_CTRL & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_NONE;
                                }
                                else
                                {
                                    USBFSD->UEP5_TX_LEN  = 0;
                                    USBFSD->UEP5_TX_CTRL = ( USBFSD->UEP5_TX_CTRL & ~USBFS_UEP_T_RES_MASK ) | USBFS_UEP_T_RES_NAK;
                                }
                            }
                            else if( USB_ModeConfig_ActiveModeHasTargetUac() && USBFS_SetupReqIndex == 6 )
                            {
                                /* IF6 alternate setting: 0=idle, 1=active ISO OUT */
                                g_uac_state.usbfs_as_out_alt = (uint8_t)(USBFS_SetupReqValue & 0xFF);
                                if( g_uac_state.usbfs_as_out_alt == 1 )
                                {
                                    /* ISO OUT: R_RES_NONE — device never sends NAK/ACK for isochronous */
                                    USBFSD->UEP6_RX_CTRL = ( USBFSD->UEP6_RX_CTRL & ~USBFS_UEP_R_RES_MASK ) | USBFS_UEP_R_RES_NONE;
                                }
                                else
                                {
                                    USBFSD->UEP6_RX_CTRL = ( USBFSD->UEP6_RX_CTRL & ~USBFS_UEP_R_RES_MASK ) | USBFS_UEP_R_RES_NAK;
                                }
                            }
#endif /* ENABLE_UAC */

#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
                            /* CDC-ECM data interface: alt=0 disables endpoints,
                             * alt=1 enables EP6 (bulk OUT) and EP7 (bulk IN).
                             * Interface number depends on configuration:
                             *   HID+ECM only: ECM data is IF4/IF5
                             *   HID+ACM+ECM:  ECM data is IF6/IF7 (ACM takes IF4/IF5)
                             *   HID+UAC+ECM:  ECM data is IF7/IF8 (UAC takes IF4-IF6)
                             *   HID+UAC+ACM+ECM: ECM data is IF8/IF9 */
                            if (USB_ModeConfig_ActiveModeHasTargetCdcEcm()) {
                                /* Determine which interface number is the ECM data interface */
                                uint8_t ecm_data_if = 4;  /* Default: HID+ECM only */
                                if (USB_ModeConfig_ActiveModeHasTargetCdcAcm()) {
                                    ecm_data_if += 2;  /* ACM adds 2 interfaces (4,5) */
                                }
                                if (USB_ModeConfig_ActiveModeHasTargetUac()) {
                                    ecm_data_if += 3;  /* UAC adds 3 interfaces (4,5,6 or 7,8,9) */
                                }

                                if (USBFS_SetupReqIndex == ecm_data_if) {
                                    uint8_t alt = (uint8_t)(USBFS_SetupReqValue & 0xFF);
                                    if (alt == 1) {
                                        /* Enable EP6 bulk OUT (ACK, DATA0) and EP7 bulk IN (NAK, DATA0).
                                         * Do NOT set UEP_R_TOG/UEP_T_TOG — bulk endpoints start at
                                         * DATA0; setting TOG would make the hardware expect DATA1
                                         * and reject the host's first DATA0 packet. */
                                        USBFSD->UEP6_RX_CTRL = USBFS_UEP_R_RES_ACK;
                                        USBFSD->UEP7_TX_CTRL = USBFS_UEP_T_RES_NAK;
                                        USBFSD->UEP7_TX_LEN  = 0;
                                        USBFS_Endp_Busy[DEF_UEP7] = 0;
                                        /* Reset GARP flag so it gets re-queued on every activation. */
                                        g_target_garp_sent = 0;
                                        /* Externally visible flag — ecm_bridge.c checks this
                                         * before sending through USBFS EP7. */
                                        g_target_ecm_data_if_active = 1;
                                    }
                                    else {
                                        /* Keep EP6 RX active even on alt=0 — macOS CDC ECM
                                           may deactivate then immediately reactivate, and we
                                           need to receive ARP requests during this window. */
                                        USBFSD->UEP6_RX_CTRL = USBFS_UEP_R_RES_ACK;
                                        USBFSD->UEP7_TX_CTRL = USBFS_UEP_T_RES_NAK;
                                        USBFSD->UEP7_TX_LEN  = 0;
                                        USBFS_Endp_Busy[DEF_UEP7] = 0;
                                        /* Keep active so GARP/ARP-reply TX path stays open */
                                        g_target_ecm_data_if_active = 1;
                                    }
                                }
                            }
#endif /* USB_WORK_MODE_COMPILES_TARGET_CDC_ECM */
                            break;

                        /* host get status of specified device/interface/end-points */
                        case USB_GET_STATUS:
                            USBFS_EP0_Buf[ 0 ] = 0x00;
                            USBFS_EP0_Buf[ 1 ] = 0x00;
                            if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_DEVICE )
                            {
                                if( USBFS_DevSleepStatus & 0x01 )
                                {
                                    USBFS_EP0_Buf[ 0 ] = 0x02;
                                }
                                else
                                {
                                    USBFS_EP0_Buf[ 0 ] = 0x00;
                                }
                            }
                            else if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP )
                            {
                                if( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == ( DEF_UEP_IN | DEF_UEP1 ) )
                                {
                                    if( ( USBFSD->UEP1_TX_CTRL & USBFS_UEP_T_RES_MASK ) == USBFS_UEP_T_RES_STALL )
                                    {
                                        USBFS_EP0_Buf[ 0 ] = 0x01;
                                    }
                                }
                                else if( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == ( DEF_UEP_IN | DEF_UEP2 ) )
                                {
                                    if( ( USBFSD->UEP2_TX_CTRL & USBFS_UEP_T_RES_MASK ) == USBFS_UEP_T_RES_STALL )
                                    {
                                        USBFS_EP0_Buf[ 0 ] = 0x01;
                                    }
                                }
                                else if( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == ( DEF_UEP_IN | DEF_UEP3 ) )
                                {
                                    if( ( USBFSD->UEP3_TX_CTRL & USBFS_UEP_T_RES_MASK ) == USBFS_UEP_T_RES_STALL )
                                    {
                                        USBFS_EP0_Buf[ 0 ] = 0x01;
                                    }
                                }
                                else if( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == ( DEF_UEP_IN | DEF_UEP4 ) )
                                {
                                    if( ( USBFSD->UEP4_TX_CTRL & USBFS_UEP_T_RES_MASK ) == USBFS_UEP_T_RES_STALL )
                                    {
                                        USBFS_EP0_Buf[ 0 ] = 0x01;
                                    }
                                }
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ACM
                                else if( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == ( DEF_UEP_IN | DEF_UEP5 ) )
                                {
                                    if( ( USBFSD->UEP5_TX_CTRL & USBFS_UEP_T_RES_MASK ) == USBFS_UEP_T_RES_STALL )
                                    {
                                        USBFS_EP0_Buf[ 0 ] = 0x01;
                                    }
                                }
                                else if( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == ( DEF_UEP_OUT | DEF_UEP6 ) )
                                {
                                    if( ( USBFSD->UEP6_RX_CTRL & USBFS_UEP_R_RES_MASK ) == USBFS_UEP_R_RES_STALL )
                                    {
                                        USBFS_EP0_Buf[ 0 ] = 0x01;
                                    }
                                }
                                else if( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) == ( DEF_UEP_IN | DEF_UEP7 ) )
                                {
                                    if( ( USBFSD->UEP7_TX_CTRL & USBFS_UEP_T_RES_MASK ) == USBFS_UEP_T_RES_STALL )
                                    {
                                        USBFS_EP0_Buf[ 0 ] = 0x01;
                                    }
                                }
#endif
                                else
                                {
                                    errflag = 0xFF;
                                }
                            }
                            else
                            {
                                errflag = 0xFF;
                            }
                            if( USBFS_SetupReqLen > 2 )
                            {
                                USBFS_SetupReqLen = 2;
                            }
                            break;

                        default:
                            errflag = 0xFF;
                            break;
                    }
                }

                /* errflag = 0xFF means a request not support or some errors occurred, else correct */
                if( errflag == 0xFF )
                {
                    /* if one request not support, return stall */
                    USBFSD->UEP0_TX_CTRL = USBFS_UEP_T_TOG | USBFS_UEP_T_RES_STALL;
                    USBFSD->UEP0_RX_CTRL = USBFS_UEP_R_TOG | USBFS_UEP_R_RES_STALL;
                }
                else
                {
                    /* end-point 0 data Tx/Rx */
                    if( USBFS_SetupReqType & DEF_UEP_IN )
                    {
                        len = ( USBFS_SetupReqLen > DEF_USBD_UEP0_SIZE )? DEF_USBD_UEP0_SIZE : USBFS_SetupReqLen;
                        USBFS_SetupReqLen -= len;
                        USBFSD->UEP0_TX_LEN = len;
                        USBFSD->UEP0_TX_CTRL = USBFS_UEP_T_TOG | USBFS_UEP_T_RES_ACK;
                    }
                    else
                    {
                        if( USBFS_SetupReqLen == 0 )
                        {
                            USBFSD->UEP0_TX_LEN = 0;
                            USBFSD->UEP0_TX_CTRL = USBFS_UEP_T_TOG | USBFS_UEP_T_RES_ACK;
                        }
                        else
                        {
                            USBFSD->UEP0_RX_CTRL = USBFS_UEP_R_TOG | USBFS_UEP_R_RES_ACK;
                        }
                    }
                }
                break;

            default :
                break;
        }
        USBFSD->INT_FG = USBFS_UIF_TRANSFER;
    }
    else if( intflag & USBFS_UIF_BUS_RST )
    {
        /* usb reset interrupt processing */
        USBFS_DevConfig = 0;
        USBFS_DevAddr = 0;
        USBFS_DevSleepStatus = 0;
        USBFS_DevEnumStatus = 0;
        USBFS_Connect = 0;

        USBFSD->DEV_ADDR = 0;
        USBFS_Device_Endp_Init( );

        /* Restore critical control registers (consistent with USBFS_Device_Init) */
        USBFSD->INT_EN    = USBFS_UIE_SUSPEND | USBFS_UIE_BUS_RST | USBFS_UIE_TRANSFER;
        USBFSD->BASE_CTRL = USBFS_UC_DEV_PU_EN | USBFS_UC_INT_BUSY | USBFS_UC_DMA_EN;
        USBFSD->UDEV_CTRL = USBFS_UD_PD_DIS | USBFS_UD_PORT_EN;

        if (USB_ModeConfig_ActiveModeBridgeAcm()) {
            ACM_Bridge_Init();
        }
        else if (USB_ModeConfig_ActiveModeBridgeEcm()) {
            ECM_Bridge_Init();
        }
        USBFSD->INT_FG = USBFS_UIF_BUS_RST;
    }
    else if( intflag & USBFS_UIF_SUSPEND )
    {
        USBFSD->INT_FG = USBFS_UIF_SUSPEND;
        Delay_Us(10);
        /* usb suspend interrupt processing */
        if( USBFSD->MIS_ST & USBFS_UMS_SUSPEND )
        {
            USBFS_DevSleepStatus |= 0x02;
            /* Do NOT enter STOP mode here. STOP mode stops the entire MCU,
             * making it unable to respond to Host USB requests, which causes
             * the Host to disconnect the device (error -71 on reconnect).
             *
             * Instead, just reset the sleep status to keep USBFS in a
             * responsive state. The device will remain active and able to
             * handle USB requests even when the bus is idle/suspended. */
            USBFS_DevSleepStatus = 0;
        }
        else
        {
            USBFS_DevSleepStatus &= ~0x02;
        }
    }
    else
    {
        /* other interrupts */
        USBFSD->INT_FG = intflag;
    }
}


/*********************************************************************
 * @fn      USBFS_Send_Resume
 *
 * @brief   Send usb k signal, Wake up usb host
 *
 * @return  none
 */
void USBFS_Send_Resume( void )
{
    USBFSD->UDEV_CTRL ^= USBFS_UD_LOW_SPEED;
    Delay_Ms( 8 );
    USBFSD->UDEV_CTRL ^= USBFS_UD_LOW_SPEED;
    Delay_Ms( 1 );

}
void DevEP2_IN_Deal(UINT8 l)
{
    USBFSD->UEP2_TX_LEN = l;
    USBFSD->UEP2_TX_CTRL = (USBFSD->UEP2_TX_CTRL & ~USBFS_UEP_T_RES_MASK) | USBFS_UEP_T_RES_ACK;
}
/*********************************************************************
 * @fn      USBSendData
 *
 * @brief   Send data to the host
 *
 * @return  none
 */
uint8_t USBSendData(void)
{
#ifdef ENABLE_BLE
    if(USBFSD->UEP2_TX_CTRL & USBFS_UEP_T_RES_MASK == USBFS_UEP_T_RES_ACK)
    {
        return 0x01;
    }
    if(RingMemBLE.CurrentLen > 32)
    {
        RingMemRead(&RingMemBLE, pEP2_IN_DataBuf, 32);
        DevEP2_IN_Deal(32);
    }
    else
    {
        uint8_t len = RingMemBLE.CurrentLen;
        RingMemRead(&RingMemBLE, pEP2_IN_DataBuf, len);
        DevEP2_IN_Deal(len);
    }
    return SUCCESS;
#else
    return 0x01;
#endif
}

/*********************************************************************
 * @fn      USBFS_CheckEndpointBusyTimeout
 *
 * @brief   Check all USB endpoints for busy-flag timeout and recover.
 *
 *          If a USB transfer completion interrupt is lost (due to EMI,
 *          clock jitter, etc.), the USBFS_Endp_Busy flag can remain set
 *          forever, causing the endpoint to permanently stop sending data.
 *
 *          This function detects endpoints stuck in busy state for longer
 *          than USB_ENDPOINT_BUSY_TIMEOUT_MS and force-clears the busy flag.
 *
 *          Call this from the main loop (Main_Circulation) every iteration.
 *          Zero overhead in normal path: only compares when busy flag is set.
 *
 * @return  Total number of recovered endpoints (0 = no stuck endpoints)
 */
uint32_t USBFS_CheckEndpointBusyTimeout( void )
{
    uint32_t now = systick_ms;
    uint32_t recovered = 0;

    for( uint8_t ep = DEF_UEP1; ep < DEF_UEP_NUM; ep++ )
    {
        if( USBFS_Endp_Busy[ ep ] )
        {
            if( now - USBFS_Endp_Busy_StartTime[ ep ] > USB_ENDPOINT_BUSY_TIMEOUT_MS )
            {
                USBFS_Endp_Busy[ ep ] = 0;
                USBFS_Endp_Timeout_Count[ ep ]++;
                recovered++;
            }
        }
    }
    return recovered;
}
