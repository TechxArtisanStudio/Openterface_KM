/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_prop.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2021/08/08
 * Description        : All processing related to Virtual Com Port Demo
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/ 
#include "usb_lib.h"
#include "usb_conf.h"
#include "usb_prop.h"
#include "usb_desc.h"
#include "../../include/Version_selection.h"
#include "usb_pwr.h"
#include "hw_config.h"
#include "UART.h"
#include "ch32v20x_usb.h"
#include "../../include/uac_desc.h"
#include "../../include/uac_handler.h"
#include "../../include/acm_bridge.h"
#include "../../include/ecm_bridge.h"
#include "../../include/usb_mode_config.h"

uint8_t Request = 0;

#if USB_WORK_MODE_COMPILES_HOST_CDC_ECM
#define USBD_CDC_ECM_REQ_SET_MC_FILTERS  0x40u
#define USBD_CDC_ECM_REQ_SET_PM_PATTERN  0x41u
#define USBD_CDC_ECM_REQ_GET_PM_PATTERN  0x42u
#define USBD_CDC_ECM_REQ_SET_PACKET      0x43u
#define USBD_CDC_ECM_REQ_GET_STATISTIC   0x44u
static uint8_t USBD_CDC_ECM_CtrlBuf[64];
static uint8_t USBD_CDC_ECM_Stat[4] = { 0, 0, 0, 0 };
static volatile uint16_t USBD_CDC_ECM_PacketFilter;
#endif

extern uint8_t USBD_Endp3_Busy;
extern uint8_t USBD_Endp1_Busy;
extern uint8_t USBD_Endp2_Busy;
extern uint8_t USBD_Endp6_Busy;

DEVICE Device_Table =
{
	EP_NUM,
	1
};

DEVICE_PROP Device_Property =
{
	USBD_init,
	USBD_Reset,
	USBD_Status_In,
	USBD_Status_Out,
	USBD_Data_Setup,
	USBD_NoData_Setup,
	USBD_Get_Interface_Setting,
	USBD_GetDeviceDescriptor,
	USBD_GetConfigDescriptor,
	USBD_GetStringDescriptor,
	0,
	DEF_USBD_UEP0_SIZE                                 
};

USER_STANDARD_REQUESTS User_Standard_Requests =
{
	USBD_GetConfiguration,
	USBD_SetConfiguration,
	USBD_GetInterface,
	USBD_SetInterface,
	USBD_GetStatus, 
	USBD_ClearFeature,
	USBD_SetEndPointFeature,
	USBD_SetDeviceFeature,
	USBD_SetDeviceAddress
};

ONE_DESCRIPTOR Device_Descriptor =
{
	(uint8_t*)USBD_DeviceDescriptor,
	USBD_SIZE_DEVICE_DESC
};

ONE_DESCRIPTOR Config_Descriptor =
{
	(uint8_t*)USBD_ConfigDescriptor,
	USBD_SIZE_CONFIG_DESC
};

ONE_DESCRIPTOR String_Descriptor[] =
{
	{(uint8_t*)USBD_StringLangID,  USBD_SIZE_STRING_LANGID},
	{(uint8_t*)USBD_StringVendor,  USBD_SIZE_STRING_VENDOR},
	{(uint8_t*)USBD_StringProduct, USBD_SIZE_STRING_PRODUCT},
  {(uint8_t*)USBD_StringSerial,  USBD_SIZE_STRING_SERIAL},
#if USB_WORK_MODE_COMPILES_HOST_CDC_ECM
  {(uint8_t*)USBD_StringEcmMac,  USBD_SIZE_STRING_ECM_MAC},
#endif
};

/*********************************************************************
 * @fn      USBD_SetConfiguration.
 *
 * @brief     Update the device state to configured.
 *
 * @return    None.
 */
void USBD_SetConfiguration(void)
{
  DEVICE_INFO *pInfo = &Device_Info;

  if (pInfo->Current_Configuration != 0)
  {
    bDeviceState = CONFIGURED;

    /* Enable EP3 IN endpoint immediately after configuration.
     * This ensures CH9329 responses can be sent without waiting for
     * CDC_SET_LINE_CODING request from the host. */
    if (USB_ModeConfig_ActiveModeHasHostCdcAcm())
    {
        _ClearDTOG_TX(ENDP3);
        SetEPTxStatus(ENDP3, EP_TX_NAK);
    }
  }
}

/*******************************************************************************
 * @fn         USBD_SetDeviceAddress.
 *
 * @brief      Update the device state to addressed.
 *
 * @return     None.
 */
void USBD_SetDeviceAddress (void)
{
  bDeviceState = ADDRESSED;
}


/*********************************************************************
 * @fn      USBD_SetDeviceFeature.
 *
 * @brief   SetDeviceFeature Routine.
 *
 * @return  none
 */
void USBD_SetDeviceFeature (void)
{

}



/*********************************************************************
 * @fn      USBD_ClearFeature.
 *
 * @brief   ClearFeature Routine.
 *
 * @return  none
 */
void USBD_ClearFeature(void)
{

}





/*********************************************************************
 * @fn      USBD_Status_In.
 *
 * @brief    USBD Status In Routine.
 *
 * @return   None.
 */
void USBD_Status_In(void)
{
    uint32_t Request_No = pInformation->USBbRequest;
    if (Type_Recipient == (CLASS_REQUEST | INTERFACE_RECIPIENT))
    {
        if (Request_No == CDC_SET_LINE_CODING)
        {
            if (!USB_ModeConfig_ActiveModeBridgeAcm())
            {
            UART2_USB_Init();
            }
            /* Enable ACM data IN endpoint (EP3) after line coding is set.
             * EP3 starts as EP_TX_DIS in USBD_Reset(); it must be enabled
             * so the host COM-port driver can read data.                    */
            if (USB_ModeConfig_ActiveModeHasHostCdcAcm())
            {
                _ClearDTOG_TX(ENDP3);
                SetEPTxStatus(ENDP3, EP_TX_NAK);
            }
        }
    }
}

/*******************************************************************************
 * @fn       USBD_Status_Out
 *
 * @brief    USBD Status OUT Routine.
 *
 * @return   None.
 */
void USBD_Status_Out(void)
{
    
}

/*******************************************************************************
 * @fn       USBD_init.
 *
 * @brief    init routine.
 * 
 * @return   None.
 */
void USBD_init(void)
{
  uint8_t	i;
    
  pInformation->Current_Configuration = 0;
  PowerOn();
  for (i=0;i<8;i++) _SetENDPOINT(i,_GetENDPOINT(i) & 0x7F7F & EPREG_MASK);//all clear
  _SetISTR((uint16_t)0x00FF);//all clear
  USB_SIL_Init();
  bDeviceState = UNCONNECTED;
  
  USB_Port_Set(DISABLE, DISABLE);	
  Delay_Ms(20);
  USB_Port_Set(ENABLE, ENABLE);    
}

/*******************************************************************************
 * @fn      USBD_SetInterface_Impl
 *
 * @brief   Called by the USB stack when a SET_INTERFACE request completes.
 *          Enables or disables EP3 (ISO IN) and EP4 (ISO OUT) depending on
 *          whether the host selected alternate setting 1 (active) or 0 (idle).
 *          pInformation->USBwIndex0  = interface number
 *          pInformation->USBwValue0  = alternate setting
 *
 * @return  None.
 */
void USBD_SetInterface_Impl(void)
{
    uint8_t iface = pInformation->USBwIndex0;
    uint8_t alt   = pInformation->USBwValue0;

#ifdef ENABLE_UAC
    /* IF3 = AudioStreaming IN (EP4 ISO IN, 0x84 �?Target mic �?Host PC)
     * Host opens this interface to receive microphone audio.           */
  if (USB_ModeConfig_ActiveModeHasHostUac() && iface == 3)
    {
        if (alt == 1)
        {
        UAC_T2H_Reset();
            /* Prime EP4 with a silence frame so the first ISO IN token
             * from Windows gets a valid (zero) payload immediately.
             * Without this the endpoint never produces its first packet
             * and Windows does not show the device in the audio mixer.  */
            static const uint8_t silence[64] = {0};
            UserToPMABufferCopy((uint8_t*)silence, ENDP4_ISO_TXADDR, 64);
            UserToPMABufferCopy((uint8_t*)silence, ENDP4_ISO_TXADDR1, 64);
            SetEPDblBuffCount(ENDP4, EP_DBUF_IN, 64);
            _ClearDTOG_RX(ENDP4);
            _ClearDTOG_TX(ENDP4);
            SetEPTxStatus(ENDP4, EP_TX_VALID);
        }
        else
        {
            SetEPTxCount(ENDP4, 0);
            SetEPTxStatus(ENDP4, EP_TX_DIS);
        }
        g_uac_state.usbd_as_in_alt = alt;
    }
    /* IF4 = AudioStreaming OUT (EP2 ISO OUT, 0x02 �?Host PC �?Target spk)
     * Host opens this interface to send playback audio.               */
    else if (USB_ModeConfig_ActiveModeHasHostUac() && iface == 4)
    {
        if (alt == 1)
        {
        UAC_H2T_Reset();
          /* Activate EP2 ISO OUT double buffer: arm both PMA slots. */
          SetEPDblBuffCount(ENDP2, EP_DBUF_OUT, 64);
          _ClearDTOG_RX(ENDP2);
          _ClearDTOG_TX(ENDP2);
            SetEPRxStatus(ENDP2, EP_RX_VALID);
        }
        else
        {
            SetEPRxStatus(ENDP2, EP_RX_DIS);
        }
        g_uac_state.usbd_as_out_alt = alt;
    }
#endif /* ENABLE_UAC */

#if USB_WORK_MODE_COMPILES_BRIDGE_ECM
    /* IF1 = CDC-ECM Data interface. alt=0: endpoints disabled;
     * alt=1: enable EP3 (bulk IN) and EP5 (bulk OUT).
     * NOTE: In combined ACM+ECM mode (mode 6), IF1 is ACM data interface,
     * not ECM. This code should only run in pure ECM bridge mode (mode 4). */
    if (USB_ModeConfig_ActiveModeBridgeEcm() && !USB_ModeConfig_ActiveModeHasHostCdcAcm() && iface == 1) {
        if (alt == 1) {
            /* Clear data toggle for fresh endpoint */
            _ClearDTOG_TX(ENDP3);
            _ClearDTOG_RX(ENDP3);
            SetEPTxStatus(ENDP3, EP_TX_NAK);
            SetEPRxAddr(ENDP5, ENDP5_BULK_RXADDR);
            SetEPRxCount(ENDP5, 64);
            _ClearDTOG_RX(ENDP5);
            SetEPRxStatus(ENDP5, EP_RX_VALID);
            /* Mark interface as active so poll_host_tx() can send GARP/DHCP replies */
            extern volatile uint8_t g_host_ecm_data_if_active;
            g_host_ecm_data_if_active = 1;
        } else {
            SetEPTxStatus(ENDP3, EP_TX_DIS);
            SetEPRxStatus(ENDP5, EP_RX_DIS);
            extern volatile uint8_t g_host_ecm_data_if_active;
            g_host_ecm_data_if_active = 0;
        }
    }
#endif

#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    /* Combined ACM+ECM mode: IF3 = CDC-ECM Data interface.
     * alt=0: EP6 (bidirectional bulk) disabled;
     * alt=1: enable EP6 in both directions. */
    if (USB_ModeConfig_ActiveModeHasHostCdcAcm() &&
        USB_ModeConfig_ActiveModeHasHostCdcEcm() && iface == 3) {
        if (alt == 1) {
            _ClearDTOG_TX(ENDP6);
            _ClearDTOG_RX(ENDP6);
            SetEPTxStatus(ENDP6, EP_TX_NAK);
            SetEPRxAddr(ENDP6, ENDP6_RX_ECM_ADDR);
            SetEPRxCount(ENDP6, 32);
            SetEPRxStatus(ENDP6, EP_RX_VALID);
#if USB_WORK_MODE_COMPILES_BRIDGE_ECM
            extern volatile uint8_t g_host_ecm_data_if_active;
            g_host_ecm_data_if_active = 1;
#endif
        } else {
            SetEPTxStatus(ENDP6, EP_TX_DIS);
            SetEPRxStatus(ENDP6, EP_RX_DIS);
#if USB_WORK_MODE_COMPILES_BRIDGE_ECM
            extern volatile uint8_t g_host_ecm_data_if_active;
            g_host_ecm_data_if_active = 0;
#endif
        }
    }
#endif
}

/*******************************************************************************
 * @fn      USBD_Reset
 *
 * @brief   USBD reset routine
 *
 * @return  None.
 */
void USBD_Reset(void)
{
  pInformation->Current_Configuration = 0;
  pInformation->Current_Feature = USBD_GetActiveConfigAttributes();
  pInformation->Current_Interface = 0;

  SetBTABLE(BTABLE_ADDRESS);

  SetEPType(ENDP0, EP_CONTROL);
  SetEPTxStatus(ENDP0, EP_TX_STALL);
  SetEPRxAddr(ENDP0, ENDP0_RXADDR);
  SetEPTxAddr(ENDP0, ENDP0_TXADDR);
  Clear_Status_Out(ENDP0);
  SetEPRxCount(ENDP0, Device_Property.MaxPacketSize);
  SetEPRxValid(ENDP0);
  _ClearDTOG_RX(ENDP0);
  _ClearDTOG_TX(ENDP0);

  SetEPType(ENDP1, EP_INTERRUPT);
  SetEPTxStatus(ENDP1, EP_TX_NAK);
  SetEPTxAddr(ENDP1, ENDP1_TXADDR);
  SetEPRxStatus(ENDP1, EP_RX_DIS);
  _ClearDTOG_TX(ENDP1);
  _ClearDTOG_RX(ENDP1);

#ifdef ENABLE_UAC
  if (USB_ModeConfig_ActiveModeHasHostUac())
  {
    SetEPType(ENDP2, EP_ISOCHRONOUS);
    SetEPDoubleBuff(ENDP2);
    SetEPTxStatus(ENDP2, EP_TX_DIS);
    SetEPDblBuffAddr(ENDP2, ENDP2_ISO_RXADDR, ENDP2_ISO_RXADDR1);
    SetEPDblBuffCount(ENDP2, EP_DBUF_OUT, 64);
    SetEPRxStatus(ENDP2, EP_RX_DIS);
    _ClearDTOG_RX(ENDP2);
    _ClearDTOG_TX(ENDP2);
  }
#endif

  /* EP3: ECM bulk IN (host PC side). Start disabled per CDC-ECM spec;
   * enabled when the host selects data interface alt setting 1. */
  SetEPType(ENDP3, EP_BULK);
  SetEPTxStatus(ENDP3, EP_TX_DIS);
  SetEPTxAddr(ENDP3, ENDP3_TXADDR);
  SetEPRxStatus(ENDP3, EP_RX_DIS);
  _ClearDTOG_TX(ENDP3);
  _ClearDTOG_RX(ENDP3);

  /* EP5 RX buffer: allocate enough space for ECM bulk packets (64 bytes) */
  if (USB_ModeConfig_ActiveModeBridgeEcm()) {
      SetEPRxCount(ENDP5, 64);
  }

#ifdef ENABLE_UAC
  if (USB_ModeConfig_ActiveModeHasHostUac())
  {
    /* EP4: UAC ISO IN (Host audio → Target PC), double-buffered 64 B, disabled until alt=1 */
    SetEPType(ENDP4, EP_ISOCHRONOUS);
    SetEPDoubleBuff(ENDP4);
    SetEPDblBuffAddr(ENDP4, ENDP4_ISO_TXADDR, ENDP4_ISO_TXADDR1);
    SetEPDblBuffCount(ENDP4, EP_DBUF_IN, 64);
    _ClearDTOG_RX(ENDP4);
    _ClearDTOG_TX(ENDP4);
    SetEPTxStatus(ENDP4, EP_TX_DIS);
    SetEPRxStatus(ENDP4, EP_RX_DIS);

    /* Clear UAC bridge buffers and state flags so stale audio data from a
     * previous session cannot be forwarded after re-enumeration.          */
    UAC_Init();
  }
#endif /* ENABLE_UAC */

  SetEPType(ENDP5, EP_BULK);
  SetEPRxAddr(ENDP5, ENDP5_BULK_RXADDR);
  SetEPRxCount(ENDP5, USB_ModeConfig_ActiveModeBridgeEcm() ? 64 : DEF_USBD_MAX_PACK_SIZE);
  _ClearDTOG_RX(ENDP5);
  _ClearDTOG_TX(ENDP5);
  /* In ECM-only bridge mode (no ACM), EP5 is the ECM data endpoint — start
   * disabled; enabled by SET_INTERFACE.  In combined ACM+ECM mode (mode 6),
   * EP5 is the ACM bulk-OUT endpoint — must be enabled immediately.        */
  SetEPRxStatus(ENDP5, (USB_ModeConfig_ActiveModeBridgeEcm() && !USB_ModeConfig_ActiveModeHasHostCdcAcm()) ? EP_RX_DIS : EP_RX_VALID);
  SetEPTxStatus(ENDP5, EP_TX_DIS);

#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
  /* Combined ACM+ECM mode: initialize ECM endpoints (EP2, EP4, EP6) */
  if (USB_ModeConfig_ActiveModeHasHostCdcAcm() && USB_ModeConfig_ActiveModeHasHostCdcEcm()) {
      /* EP2: ECM interrupt IN (notification) */
      SetEPType(ENDP2, EP_INTERRUPT);
      SetEPTxStatus(ENDP2, EP_TX_NAK);
      SetEPTxAddr(ENDP2, ENDP2_TX_ECM_ADDR);
      SetEPRxStatus(ENDP2, EP_RX_DIS);
      _ClearDTOG_TX(ENDP2);
      _ClearDTOG_RX(ENDP2);

      /* EP6: ECM bulk, bidirectional (device ↔ host) */
      SetEPType(ENDP6, EP_BULK);
      SetEPRxAddr(ENDP6, ENDP6_RX_ECM_ADDR);
      SetEPRxCount(ENDP6, 32);
      SetEPTxAddr(ENDP6, ENDP6_TX_ECM_ADDR);
      _ClearDTOG_RX(ENDP6);
      _ClearDTOG_TX(ENDP6);
      /* Enable both directions immediately so the Host can send DHCP and
       * the device can answer without waiting for SET_INTERFACE. */
      SetEPRxStatus(ENDP6, EP_RX_VALID);
      SetEPTxStatus(ENDP6, EP_TX_NAK);
#if USB_WORK_MODE_COMPILES_BRIDGE_ECM
      extern volatile uint8_t g_host_ecm_data_if_active;
      g_host_ecm_data_if_active = 1;
#endif
  }
#endif

  SetDeviceAddress(0);

  USBD_Endp3_Busy = 0;
  USBD_Endp1_Busy = 0;
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
  USBD_Endp2_Busy = 0;
  USBD_Endp6_Busy = 0;
#endif
  if (USB_ModeConfig_ActiveModeBridgeAcm())
  {
    ACM_Bridge_Init();
  }
  else if (USB_ModeConfig_ActiveModeBridgeEcm())
  {
    ECM_Bridge_Init();
  }
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
  else if (USB_ModeConfig_ActiveModeHasHostCdcEcm() &&
           USB_ModeConfig_ActiveModeHasHostCdcAcm())
  {
    ECM_Bridge_Init();
#if USB_WORK_MODE_COMPILES_BRIDGE_ECM
    /* Force-enable Host ECM interface since EP6/EP4 are enabled at init */
    extern volatile uint8_t g_host_ecm_data_if_active;
    g_host_ecm_data_if_active = 1;
#endif
  }
#endif

  bDeviceState = ATTACHED;
}

/*******************************************************************************
 * @fn      USBD_GetDeviceDescriptor.
 *
 * @brief   Gets the device descriptor.
 *
 * @param   Length.
 *
 * @return  The address of the device descriptor.
 */
uint8_t *USBD_GetDeviceDescriptor(uint16_t Length)
{
  return Standard_GetDescriptorData(Length, &Device_Descriptor);
}

/*******************************************************************************
 * @fn       USBD_GetConfigDescriptor.
 *
 * @brief    get the configuration descriptor.
 *
 * @param    Length.
 *
 * @return   The address of the configuration descriptor.
 */
uint8_t *USBD_GetConfigDescriptor(uint16_t Length)
{
  uint16_t config_len;
  Config_Descriptor.Descriptor = (uint8_t *)USBD_GetActiveConfigDescriptorData(&config_len);
  Config_Descriptor.Descriptor_Size = config_len;
  return Standard_GetDescriptorData(Length, &Config_Descriptor);
}

/*******************************************************************************
 * @fn        USBD_GetStringDescriptor
 *
 * @brief     Gets the string descriptors according to the needed index
 *
 * @param     Length.
 *
 * @return    The address of the string descriptors.
 */
uint8_t *USBD_GetStringDescriptor(uint16_t Length)
{
  uint8_t wValue0 = pInformation->USBwValue0;
  if (wValue0 >= (sizeof(String_Descriptor) / sizeof(String_Descriptor[0])))
  {
    return NULL;
  }
  else
  {
    return Standard_GetDescriptorData(Length, &String_Descriptor[wValue0]);
  }
}

/*********************************************************************
 * @fn      USBD_Get_Interface_Setting.
 *
 * @brief  test the interface and the alternate setting according to the
 *                  supported one.
 *
 * @param  Interface: interface number.
 *                  AlternateSetting: Alternate Setting number.
 *
 * @return USB_UNSUPPORT or USB_SUCCESS.
 */
RESULT USBD_Get_Interface_Setting(uint8_t Interface, uint8_t AlternateSetting)
{
#if USB_WORK_MODE_RUNTIME_PROFILES
  if (USB_ModeConfig_ActiveModeHasHostCdcAcm() && USB_ModeConfig_ActiveModeHasHostCdcEcm())
  {
    /* Combined ACM+ECM: IF0/IF1=ACM (alt0 only), IF2=ECM comm (alt0), IF3=ECM data (alt0/1) */
    if (Interface == 0 || Interface == 2)
    {
      if (AlternateSetting > 0)
        return USB_UNSUPPORT;
    }
    else if (Interface == 1)
    {
      if (AlternateSetting > 0)
        return USB_UNSUPPORT;
    }
    else if (Interface == 3)
    {
      if (AlternateSetting > 1)
        return USB_UNSUPPORT;
    }
    else
    {
      return USB_UNSUPPORT;
    }
  }
  else if (USB_ModeConfig_ActiveModeHasHostCdcEcm())
  {
    if (Interface == 0)
    {
      if (AlternateSetting > 0)
        return USB_UNSUPPORT;
    }
    else if (Interface == 1)
    {
      if (AlternateSetting > 1)
        return USB_UNSUPPORT;
    }
    else
    {
      return USB_UNSUPPORT;
    }
  }
  else if (USB_ModeConfig_ActiveModeHasHostUac())
  {
    if (Interface <= 2)
    {
      if (AlternateSetting > 0)
        return USB_UNSUPPORT;
    }
    else if (Interface <= 4)
    {
      if (AlternateSetting > 1)
        return USB_UNSUPPORT;
    }
    else
    {
      return USB_UNSUPPORT;
    }
  }
  else if (Interface > 1 || AlternateSetting > 0)
  {
    return USB_UNSUPPORT;
  }
#elif USB_WORK_MODE_HAS_HOST_CDC_ACM && USB_WORK_MODE_HAS_HOST_CDC_ECM
  /* Static combined ACM+ECM: IF0/IF1=ACM (alt0), IF2=ECM comm (alt0), IF3=ECM data (alt0/1) */
  if (Interface == 0 || Interface == 2)
  {
    if (AlternateSetting > 0)
      return USB_UNSUPPORT;
  }
  else if (Interface == 1)
  {
    if (AlternateSetting > 0)
      return USB_UNSUPPORT;
  }
  else if (Interface == 3)
  {
    if (AlternateSetting > 1)
      return USB_UNSUPPORT;
  }
  else
  {
    return USB_UNSUPPORT;
  }
#elif USB_WORK_MODE_HAS_HOST_CDC_ECM
  if (Interface == 0)
  {
    if (AlternateSetting > 0)
      return USB_UNSUPPORT;
  }
  else if (Interface == 1)
  {
    if (AlternateSetting > 1)
      return USB_UNSUPPORT;
  }
  else
  {
    return USB_UNSUPPORT;
  }
#else
#ifdef ENABLE_UAC
  /* Interface layout:
   *   IF0: CDC Comm              — alt 0 only
   *   IF1: CDC Data              — alt 0 only
   *   IF2: AudioControl          — alt 0 only
   *   IF3: AS IN  (EP4 ISO IN)   — alt 0+1
  *   IF4: AS OUT (EP2 ISO OUT)  — alt 0+1 */
  if (Interface <= 2)
  {
    if (AlternateSetting > 0)
      return USB_UNSUPPORT;
  }
  else if (Interface <= 4)
  {
    if (AlternateSetting > 1)
      return USB_UNSUPPORT;
  }
  else
  {
    return USB_UNSUPPORT;
  }
#else
  if (Interface > 1 || AlternateSetting > 0)
  {
    return USB_UNSUPPORT;
  }
#endif
#endif

  return USB_SUCCESS;
}

/*********************************************************************
 * @fn      USB_CDC_GetLineCoding.
 *
 * @brief   send the linecoding structure to the PC host.
 *
 * @param   Length
 *
 * @return  Inecoding structure base address.
 */
uint8_t *USB_CDC_GetLineCoding( uint16_t Length )
{
    if( Length == 0 )
    {
        pInformation->Ctrl_Info.Usb_wLength = 7;
        return( NULL );
    }
    return (uint8_t *)&Uart.Com_Cfg[ 0 ];
}

/*********************************************************************
 * @fn      USB_CDC_SetLineCoding.
 *
 * @brief   Set the linecoding structure fields.
 *
 * @param   Length
 *
 * @return  Inecoding structure base address.
 */
uint8_t *USB_CDC_SetLineCoding( uint16_t Length )
{
    if( Length == 0 )
    {
        pInformation->Ctrl_Info.Usb_wLength = 7;
        return( NULL );
    }
    return(uint8_t *)&Uart.Com_Cfg[ 0 ];
}


/* UAC 1.0 sampling frequency buffer used for GET_CUR / SET_CUR responses.
 * Value = 16000 Hz in 24-bit little-endian (UAC format).
 * Both GET and SET share this buffer: SET overwrites it (value is ignored). */
static uint8_t uac_sam_freq[3] = { 0x40, 0x3E, 0x00 };  /* 0x003E40 = 16000 */

/* CopyRoutine for UAC sampling-frequency class requests (GET and SET). */
static uint8_t *UAC_GetSamFreq(uint16_t Length)
{
    if (Length == 0) {
        /* First call: report available data length. */
        pInformation->Ctrl_Info.Usb_wLength = sizeof(uac_sam_freq);
        return NULL;
    }
    return uac_sam_freq;
}

#if USB_WORK_MODE_COMPILES_HOST_CDC_ECM
static uint8_t *USBD_CDC_ECM_GetStat(uint16_t Length)
{
  if (Length == 0)
  {
    pInformation->Ctrl_Info.Usb_wLength = sizeof(USBD_CDC_ECM_Stat);
    return NULL;
  }
  return USBD_CDC_ECM_Stat;
}

static uint8_t *USBD_CDC_ECM_SetCtrl(uint16_t Length)
{
  if (Length == 0)
  {
    uint16_t requested = pInformation->USBwLength;
    if (requested > sizeof(USBD_CDC_ECM_CtrlBuf))
    {
      requested = sizeof(USBD_CDC_ECM_CtrlBuf);
    }
    pInformation->Ctrl_Info.Usb_wLength = requested;
    return NULL;
  }
  return USBD_CDC_ECM_CtrlBuf;
}
#endif

/*********************************************************************
 * @fn      USBD_Data_Setup
 *
 * @brief    handle the data class specific requests
 *
 * @param    Request Nb.
 *
 * @return   USB_UNSUPPORT or USB_SUCCESS.
 */
RESULT USBD_Data_Setup(uint8_t RequestNo)
{
  uint32_t Request_No = pInformation->USBbRequest;
  uint8_t *(*CopyRoutine)(uint16_t);
  CopyRoutine = NULL;
  if (Type_Recipient == (STANDARD_REQUEST | INTERFACE_RECIPIENT))
  {
    return USB_UNSUPPORT;
  }
  else if (Type_Recipient == (CLASS_REQUEST | INTERFACE_RECIPIENT))
  {
#if USB_WORK_MODE_COMPILES_HOST_CDC_ECM
  if (USB_ModeConfig_ActiveModeHasHostCdcEcm() &&
    (Request_No == USBD_CDC_ECM_REQ_GET_STATISTIC || Request_No == USBD_CDC_ECM_REQ_GET_PM_PATTERN))
    {
      CopyRoutine = &USBD_CDC_ECM_GetStat;
    }
    else if (USB_ModeConfig_ActiveModeHasHostCdcEcm() &&
         (Request_No == USBD_CDC_ECM_REQ_SET_MC_FILTERS || Request_No == USBD_CDC_ECM_REQ_SET_PM_PATTERN))
    {
      CopyRoutine = &USBD_CDC_ECM_SetCtrl;
    }
    else if (!USB_ModeConfig_ActiveModeHasHostCdcEcm() || USB_ModeConfig_ActiveModeHasHostCdcAcm())
#endif
    {
    if (Request_No == CDC_GET_LINE_CODING)
    {
      CopyRoutine = &USB_CDC_GetLineCoding;
    }
    else if (Request_No == CDC_SET_LINE_CODING)
    {
      CopyRoutine = &USB_CDC_SetLineCoding;
    }
    else
    {
      return USB_UNSUPPORT;
    }
    }
  }
#ifdef ENABLE_UAC
  /* UAC 1.0 class-to-endpoint requests: GET_CUR/SET_CUR for sampling frequency.
   * bRequestType recipient bits = 0x02 (endpoint), type bits = 0x20 (class).
   * Windows usbaudio.sys sends these after SET_INTERFACE(alt=1) to confirm the
   * active sampling rate.  Respond with the fixed 16 kHz value. */
  else if (Type_Recipient == (CLASS_REQUEST | ENDPOINT_RECIPIENT))
  {
    uint8_t endpoint = pInformation->USBwIndex0;
    uint8_t control = pInformation->USBwValue1;
    if (USB_ModeConfig_ActiveModeHasHostUac() &&
      (control == 0x01) &&
        (endpoint == UAC_USBD_EP_IN_ADDR || endpoint == UAC_USBD_EP_OUT_ADDR))
    {
      CopyRoutine = &UAC_GetSamFreq;
    }
    else
    {
      return USB_UNSUPPORT;
    }
  }
#endif
  if (CopyRoutine)
  {
    pInformation->Ctrl_Info.CopyData = CopyRoutine;
    pInformation->Ctrl_Info.Usb_wOffset = 0;
    (*CopyRoutine)( 0 );
  }
  else
  {
    return( USB_UNSUPPORT );
  }
  return USB_SUCCESS;
}

/*******************************************************************************
 * @fn      USBD_NoData_Setup.
 *
 * @brief   handle the no data class specific requests.
 *
 * @param   Request Nb.
 *
 * @return  USB_UNSUPPORT or USB_SUCCESS.
 */
RESULT USBD_NoData_Setup(uint8_t RequestNo)      
{
  uint32_t Request_No = pInformation->USBbRequest;

  if (Type_Recipient == (CLASS_REQUEST | INTERFACE_RECIPIENT))
  {
#if USB_WORK_MODE_COMPILES_HOST_CDC_ECM
    if (USB_ModeConfig_ActiveModeHasHostCdcEcm() && Request_No == USBD_CDC_ECM_REQ_SET_PACKET)
    {
      USBD_CDC_ECM_PacketFilter = pInformation->USBwValue;
      return USB_SUCCESS;
    }
    if (USB_ModeConfig_ActiveModeHasHostCdcEcm() && !USB_ModeConfig_ActiveModeHasHostCdcAcm())
    {
      return USB_UNSUPPORT;
    }
#endif
    /* CDC-ACM control signals */
    if (Request_No == CDC_SET_LINE_CTLSTE)
    {
      return USB_SUCCESS;
    }
    if (Request_No == CDC_SEND_BREAK)
    {
      return USB_SUCCESS;
    }
    return USB_UNSUPPORT;
  }             
  return USB_SUCCESS;
}
