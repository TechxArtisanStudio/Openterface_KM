#ifndef USB_MODE_CONFIG_H
#define USB_MODE_CONFIG_H

#include <stdint.h>
#include "usb_work_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_MODE_CONFIG_STATUS_VALID       0x01u
#define USB_MODE_CONFIG_STATUS_MISMATCH    0x02u

void USB_ModeConfig_Init(void);
uint8_t USB_ModeConfig_GetCompiledMode(void);
uint8_t USB_ModeConfig_GetActiveMode(void);
uint8_t USB_ModeConfig_GetStoredMode(void);
uint16_t USB_ModeConfig_GetFlags(void);
uint8_t USB_ModeConfig_GetStatus(void);
uint32_t USB_ModeConfig_GetCapabilityMask(void);
uint8_t USB_ModeConfig_IsModeSupported(uint8_t mode);
uint8_t USB_ModeConfig_ModeHasTargetUac(uint8_t mode);
uint8_t USB_ModeConfig_ModeHasHostUac(uint8_t mode);
uint8_t USB_ModeConfig_ModeHasTargetCdcAcm(uint8_t mode);
uint8_t USB_ModeConfig_ModeHasHostCdcAcm(uint8_t mode);
uint8_t USB_ModeConfig_ModeHasTargetCdcEcm(uint8_t mode);
uint8_t USB_ModeConfig_ModeHasHostCdcEcm(uint8_t mode);
uint8_t USB_ModeConfig_ModeHasHostCdcNcm(uint8_t mode);
uint8_t USB_ModeConfig_ModeBridgeUac(uint8_t mode);
uint8_t USB_ModeConfig_ModeBridgeAcm(uint8_t mode);
uint8_t USB_ModeConfig_ModeBridgeEcm(uint8_t mode);
uint8_t USB_ModeConfig_ModeBridgeNcm(uint8_t mode);
uint8_t USB_ModeConfig_ActiveModeHasTargetUac(void);
uint8_t USB_ModeConfig_ActiveModeHasHostUac(void);
uint8_t USB_ModeConfig_ActiveModeHasTargetCdcAcm(void);
uint8_t USB_ModeConfig_ActiveModeHasHostCdcAcm(void);
uint8_t USB_ModeConfig_ActiveModeHasTargetCdcEcm(void);
uint8_t USB_ModeConfig_ActiveModeHasHostCdcEcm(void);
uint8_t USB_ModeConfig_ActiveModeHasHostCdcNcm(void);
uint8_t USB_ModeConfig_ActiveModeBridgeUac(void);
uint8_t USB_ModeConfig_ActiveModeBridgeAcm(void);
uint8_t USB_ModeConfig_ActiveModeBridgeEcm(void);
uint8_t USB_ModeConfig_ActiveModeBridgeNcm(void);
uint8_t USB_ModeConfig_Write(uint8_t mode, uint16_t flags);
uint8_t USB_ModeConfig_Clear(void);
void USB_ModeConfig_Reboot(void);
void USB_ModeConfig_RequestReboot(void);
void USB_ModeConfig_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_MODE_CONFIG_H */