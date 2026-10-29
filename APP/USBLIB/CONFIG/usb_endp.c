/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_endp.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2021/08/08
 * Description        : Endpoint routines
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/ 
#include "usb_lib.h"
#include "usb_desc.h"
#include "usb_mem.h"
#include "hw_config.h"
#include "../../include/Version_selection.h"
#include "usb_istr.h"
#include "usb_pwr.h"
#include "usb_prop.h"
#include "UART.h"
#include "../../include/uac_handler.h"
#include "../../include/acm_bridge.h"
#include "../../include/ecm_bridge.h"
#include "../../include/usb_mode_config.h"
#include "../../include/usbd_composite_km.h"

uint8_t USBD_Endp1_Busy;
uint8_t USBD_Endp3_Busy;
uint8_t USBD_Endp2_Busy;  /* ECM INT busy (also used in ACM+ECM combined mode) */
uint8_t USBD_Endp6_Busy;  /* ECM bulk IN busy — EP6 is bidirectional in combined mode */
uint16_t USB_Rx_Cnt=0;

/* Debug counters for EP5_OUT_Callback */
volatile uint32_t g_ep5_out_call_count = 0;
volatile uint32_t g_ep5_out_total_bytes = 0; 

#if USB_WORK_MODE_COMPILES_BRIDGE_ACM || USB_WORK_MODE_COMPILES_BRIDGE_ECM || USB_WORK_MODE_HAS_HOST_ACM_AND_ECM || USB_WORK_MODE_HAS_HOST_CDC_ACM
/* CH9329 command frame validator - checks for any valid CH9329 command frame
 * Not limited to USB work mode command (0x30), accepts all CH9329 commands */
uint8_t USBD_IsCh9329ModeCommandFrame(const uint8_t *buf, uint16_t len)
{
    uint16_t frame_len;
    uint8_t sum = 0;
    uint16_t i;

    if (buf == 0 || len < 6) {
        return 0;
    }
    // Check header only (0x57 0xAB), accept any command
    if (buf[0] != CH9329_FRAME_HEAD1 || buf[1] != CH9329_FRAME_HEAD2) {
        return 0;
    }

    frame_len = (uint16_t)(6u + buf[4]);
    if (len < frame_len) {
        return 0;
    }

    // Verify checksum
    for (i = 0; i < (uint16_t)(frame_len - 1u); i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    return (sum == buf[frame_len - 1u]) ? 1u : 0u;
}
#endif

/*********************************************************************
 * @fn      EP2_IN_Callback
 *
 * @brief  Endpoint 1 IN.
 *
 * @return  none
 */
void EP1_IN_Callback (void)
{
    USBD_Endp1_Busy = 0;
}

/*********************************************************************
 * @fn      EP2_IN_Callback
 *
 * @brief  Endpoint 2 IN — ECM interrupt notification (combined ACM+ECM mode).
 *         Called when the interrupt transfer completes.
 *
 * @return  none
 */
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
void EP2_IN_Callback (void)
{
    if (USB_ModeConfig_ActiveModeHasHostCdcAcm() &&
        USB_ModeConfig_ActiveModeHasHostCdcEcm()) {
        USBD_Endp2_Busy = 0;
    }
}
#endif





/*********************************************************************
 * @fn      EP2_OUT_Callback
 *
 * @brief  Endpoint 2 OUT.
 *
 * @return  none
 */
void EP2_OUT_Callback (void)
{

#ifdef ENABLE_UAC
    uint16_t len = GetEPRxCount(ENDP2);
    UAC_USBD_EP4_RxDone(len);
#else
	uint32_t len;
    len = GetEPRxCount( EP2_OUT & 0x7F );
    PMAToUserBufferCopy( &UART2_Tx_Buf[ ( Uart.Tx_LoadNum * DEF_USB_FS_PACK_LEN ) ], GetEPRxAddr( EP2_OUT & 0x7F ), len );
    Uart.Tx_PackLen[ Uart.Tx_LoadNum ] = len;
    Uart.Tx_LoadNum++;
    if( Uart.Tx_LoadNum >= DEF_UARTx_TX_BUF_NUM_MAX )
    {
        Uart.Tx_LoadNum = 0x00;
    }
    Uart.Tx_RemainNum++;

	if( Uart.Tx_RemainNum >= ( DEF_UARTx_TX_BUF_NUM_MAX - 2 ) )
    {
        Uart.USB_Down_StopFlag = 0x01;
    }
    else
    {
        SetEPRxValid( ENDP2 );
    }
#endif
}
/*********************************************************************
 * @fn      EP3_IN_Callback
 *
 * @brief  Endpoint 3 IN.
 *
 * @return  none
 */
void EP3_IN_Callback (void)
{
	USBD_Endp3_Busy = 0;
	Uart.USB_Up_IngFlag = 0x00;
}



/*********************************************************************
 * @fn      EP4_IN_Callback
 *
 * @brief  Endpoint 4 IN (UAC ISO IN — Host audio → Target PC).
 *         Called when the Target PC consumed one ISO frame.
 *
 * @return  none
 */
void EP4_IN_Callback(void)
{
#ifdef ENABLE_UAC
    UAC_USBD_EP3_TxDone();   /* loads next H→T frame into EP4 PMA buffer */
#endif
}

void EP4_OUT_Callback(void)
{
#ifdef ENABLE_UAC
    uint16_t len = GetEPRxCount(ENDP4);
    UAC_USBD_EP4_RxDone(len);
    /* Keep ISO OUT RX status untouched, but refresh PMA receive count
     * so the next frame always has buffer space. */
    SetEPRxCount(ENDP5, DEF_USBD_MAX_PACK_SIZE);
#endif
}

void EP5_OUT_Callback(void)
{
    /* IMPORTANT: Always copy to ring buffer (UART2_Tx_Buf) in interrupt context.
     * Never call CH9329_RxBuffer_Add or other parsing functions directly from
     * interrupt — they may disable interrupts or take too long, causing USB HID
     * latency. The main loop (USB_DataRx_To_KMHandle) handles parsing and routing. */
    uint32_t len;
    len = GetEPRxCount( ENDP5 );
    PMAToUserBufferCopy( &UART2_Tx_Buf[ ( Uart.Tx_LoadNum * DEF_USB_FS_PACK_LEN ) ], GetEPRxAddr( ENDP5 ), len );
    Uart.Tx_PackLen[ Uart.Tx_LoadNum ] = len;
    Uart.Tx_LoadNum++;
    if( Uart.Tx_LoadNum >= DEF_UARTx_TX_BUF_NUM_MAX )
    {
        Uart.Tx_LoadNum = 0x00;
    }
    Uart.Tx_RemainNum++;

    // Debug counters for EP5 OUT
    g_ep5_out_call_count++;
    g_ep5_out_total_bytes += len;

	if( Uart.Tx_RemainNum >= ( DEF_UARTx_TX_BUF_NUM_MAX - 2 ) )
	{
        Uart.USB_Down_StopFlag = 0x01;
	}
    else
    {
        SetEPRxValid( ENDP5 );
    }
}

#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
/*********************************************************************
 * @fn      EP6_IN_Callback
 *
 * @brief  Endpoint 6 IN — Host ECM bulk data (device → host).
 *         EP6 is bidirectional in combined ACM+ECM mode: the same
 *         endpoint number carries bulk OUT and bulk IN.
 *
 * @return  none
 */
void EP6_IN_Callback(void)
{
    if (USB_ModeConfig_ActiveModeHasHostCdcAcm() &&
        USB_ModeConfig_ActiveModeHasHostCdcEcm()) {
        USBD_Endp6_Busy = 0;
    }
}

/*********************************************************************
 * @fn      EP6_OUT_Callback
 *
 * @brief  Endpoint 6 OUT — Host ECM bulk data (host → device).
 *         Only used in combined ACM+ECM mode.
 *
 * @return  none
 */
void EP6_OUT_Callback(void)
{
    if (USB_ModeConfig_ActiveModeHasHostCdcAcm() &&
        USB_ModeConfig_ActiveModeHasHostCdcEcm()) {
        uint8_t buf[32];
        uint32_t len;
        len = GetEPRxCount( ENDP6 );
        if (len > sizeof(buf)) {
            len = sizeof(buf);
        }
        PMAToUserBufferCopy( buf, GetEPRxAddr( ENDP6 ), len );
        ECM_Bridge_HostRxDone( buf, (uint16_t)len );
        SetEPRxCount( ENDP6, 32 );
        SetEPRxValid( ENDP6 );
    }
}
#endif

/*********************************************************************
 * @fn      USBD_ENDPx_DataUp
 *
 * @brief  USBD ENDPx DataUp Function
 * 
 * @param   endp - endpoint num.
 *          *pbuf - A pointer points to data.
 *          len - data length to transmit.
 * 
 * @return  data up status.
 */
uint8_t USBD_ENDPx_DataUp( uint8_t endp, uint8_t *pbuf, uint16_t len )
{
    if( endp == ENDP1 )
    {
        if( len > DEF_USBD_MAX_PACK_SIZE )
        {
            return USB_ERROR;
        }
        if (USBD_Endp1_Busy)
        {
            return USB_ERROR;
        }
        USB_SIL_Write( EP1_IN, pbuf, len );
        USBD_Endp1_Busy = 1;
        SetEPTxStatus( ENDP1, EP_TX_VALID );
    }
    else if( endp == ENDP3 )
	{
        /* ECM bridge sends full Ethernet frames; use 64-byte bulk packets. */
        uint16_t max_pack = USB_ModeConfig_ActiveModeBridgeEcm() ? 64 : DEF_USBD_MAX_PACK_SIZE;
        if( len > max_pack )
        {
            return USB_ERROR;
        }
		if (USBD_Endp3_Busy)
		{
			return USB_ERROR;
		}
		USB_SIL_Write( EP3_IN, pbuf, len );
		USBD_Endp3_Busy = 1;
		SetEPTxStatus( ENDP3, EP_TX_VALID );
	}
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    else if( endp == ENDP2 )
    {
        /* ECM interrupt notification (combined ACM+ECM mode) */
        if (len > DEF_USBD_MAX_PACK_SIZE)
        {
            return USB_ERROR;
        }
        if (USBD_Endp2_Busy)
        {
            return USB_ERROR;
        }
        USB_SIL_Write( EP2_IN, pbuf, len );
        USBD_Endp2_Busy = 1;
        SetEPTxStatus( ENDP2, EP_TX_VALID );
    }
    else if( endp == ENDP6 )
    {
        /* ECM bulk IN — same bidirectional endpoint as the bulk OUT above,
         * 32-byte PMA slot shared with the Host ECM packet size. */
        if (len > 32)
        {
            return USB_ERROR;
        }
        if (USBD_Endp6_Busy)
        {
            return USB_ERROR;
        }
        USB_SIL_Write( EP6_IN, pbuf, len );
        USBD_Endp6_Busy = 1;
        SetEPTxStatus( ENDP6, EP_TX_VALID );
    }
#endif

	else
	{
		return USB_ERROR;
	}
	return USB_SUCCESS;
}
