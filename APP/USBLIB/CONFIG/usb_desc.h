/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_desc.h
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2021/08/08
 * Description        : This file contains all the functions prototypes for the  
 *                      USB description firmware library.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/ 
#ifndef __USB_DESC_H
#define __USB_DESC_H

#ifdef __cplusplus
 extern "C" {
#endif
	 
#include "ch32v20x.h"
#include "../../include/Version_selection.h"
	 
	 
#define USB_DEVICE_DESCRIPTOR_TYPE              0x01
#define USB_CONFIGURATION_DESCRIPTOR_TYPE       0x02
#define USB_STRING_DESCRIPTOR_TYPE              0x03
#define USB_INTERFACE_DESCRIPTOR_TYPE           0x04
#define USB_ENDPOINT_DESCRIPTOR_TYPE            0x05

#define DEF_USBD_UEP0_SIZE          64           
#define DEF_USBD_MAX_PACK_SIZE      16
       
#define USBD_SIZE_DEVICE_DESC        18
#if USB_WORK_MODE_HAS_HOST_CDC_ACM && USB_WORK_MODE_HAS_HOST_CDC_ECM && USB_WORK_MODE_HAS_TARGET_CDC_ECM
/* Combined ACM+Host ECM+Target ECM: 9(cfg) + 66(ACM) + 79(Host ECM) + 72(Target ECM) = 226 */
#define USBD_SIZE_CONFIG_DESC        226
#elif USB_WORK_MODE_HAS_HOST_CDC_ACM && USB_WORK_MODE_HAS_HOST_CDC_ECM
/* Combined ACM+ECM: 9(cfg) + 66(ACM) + 79(ECM) = 154 */
#define USBD_SIZE_CONFIG_DESC        154
#elif USB_WORK_MODE_HAS_HOST_CDC_ECM && USB_WORK_MODE_HAS_HOST_UAC
/* 9(cfg) + 79(CDC-ECM) + 173(UAC) = 261 */
#define USBD_SIZE_CONFIG_DESC        261
#elif USB_WORK_MODE_HAS_HOST_UAC
/* 9(cfg) + 66(CDC-ACM) + 173(UAC) = 248 */
#define USBD_SIZE_CONFIG_DESC        248
#elif USB_WORK_MODE_HAS_HOST_CDC_ECM
/* 9(cfg) + 79(CDC-ECM) = 88 */
#define USBD_SIZE_CONFIG_DESC        88
#else
/* 9(cfg) + 66(CDC-ACM) = 75 */
#define USBD_SIZE_CONFIG_DESC        75
#endif
#define USBD_SIZE_STRING_LANGID      4
#define USBD_SIZE_STRING_VENDOR      14
#define USBD_SIZE_STRING_PRODUCT     34
#define USBD_SIZE_STRING_SERIAL      22
#define USBD_SIZE_STRING_AUDIO_FUNC  34
#define USBD_SIZE_STRING_SPEAKER     30
#define USBD_SIZE_STRING_MICROPHONE  28
#define USBD_SIZE_STRING_ECM_MAC     26
#define USBD_SIZE_STRING_TARGET_ECM_MAC 26

extern const uint8_t USBD_DeviceDescriptor[USBD_SIZE_DEVICE_DESC];
extern const uint8_t USBD_ConfigDescriptor[USBD_SIZE_CONFIG_DESC];
extern const uint8_t USBD_StringLangID [USBD_SIZE_STRING_LANGID];
extern const uint8_t USBD_StringVendor [USBD_SIZE_STRING_VENDOR];
extern const uint8_t USBD_StringProduct[USBD_SIZE_STRING_PRODUCT];
extern uint8_t USBD_StringSerial [USBD_SIZE_STRING_SERIAL];
extern const uint8_t USBD_StringAudioFunction[USBD_SIZE_STRING_AUDIO_FUNC];
extern const uint8_t USBD_StringSpeaker[USBD_SIZE_STRING_SPEAKER];
extern const uint8_t USBD_StringMicrophone[USBD_SIZE_STRING_MICROPHONE];
extern const uint8_t USBD_StringEcmMac[USBD_SIZE_STRING_ECM_MAC];
extern const uint8_t USBD_StringTargetEcmMac[USBD_SIZE_STRING_TARGET_ECM_MAC];
const uint8_t *USBD_GetActiveConfigDescriptorData(uint16_t *len);
uint8_t USBD_GetActiveConfigAttributes(void);

#ifdef __cplusplus
}
#endif

#endif /* __USB_DESC_H */
