#ifndef ECM_IP_CONFIG_H
#define ECM_IP_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "usb_work_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Only compile when bridge ECM or NCM is active */
#if USB_WORK_MODE_COMPILES_BRIDGE_ECM || USB_WORK_MODE_COMPILES_BRIDGE_NCM

#define ECM_IP_CONFIG_MAGIC        0x45434D49u  /* "ECMI" */
#define ECM_IP_CONFIG_VERSION      2u
/* Flash storage addresses - must be within valid flash range (0x08000000 - 0x08070000 for 448KB flash)
 * Placed BEFORE other config modules to avoid conflicts:
 * - BLENAMECFG at 0x0806D000 (defined in linker script)
 * - DEFTCFG at 0x0806E000 (defined in linker script)
 * - USBMODECFG at 0x0806F000 (defined in linker script)
 * We use pages at 0x0806C000 and 0x0806B000 for dual-page wear leveling
 * These are in unused flash space before the reserved config regions */
#define ECM_IP_CONFIG_PAGE_A_ADDR  0x0806C000u   /* Dual-page A */
#define ECM_IP_CONFIG_PAGE_B_ADDR  0x0806B000u   /* Dual-page B (backup) */
#define ECM_IP_CONFIG_PAGE_SIZE    0x1000u       /* 4KB */

/* Default network configuration */
#define ECM_IP_CONFIG_DEFAULT_HOST_IP        { 192, 168, 20, 1 }
#define ECM_IP_CONFIG_DEFAULT_DEV_IP         { 192, 168, 20, 3 }
#define ECM_IP_CONFIG_DEFAULT_SUBNET_MASK    { 255, 255, 255, 0 }
#define ECM_IP_CONFIG_DEFAULT_TARGET_HOST_IP { 192, 168, 20, 2 }
#define ECM_IP_CONFIG_DEFAULT_TARGET_DEV_IP  { 192, 168, 20, 5 }
/* Captive portal DNS hijack: ON by default, answer address follows host_ip */
#define ECM_IP_CONFIG_DEFAULT_PORTAL_ENABLED 1u
#define ECM_IP_CONFIG_DEFAULT_PORTAL_MODE    1u   /* 1 = greet once, then NCSI settles */
#define ECM_IP_CONFIG_DEFAULT_PORTAL_IP      { 0, 0, 0, 0 }

/* User-visible network configuration */
typedef struct {
    uint8_t  record_version;    /* Config version, currently 2 */
    uint8_t  host_ip[4];        /* Host-side IP (default: 192.168.20.1) */
    uint8_t  dev_ip[4];         /* Firmware host-side IP (default: 192.168.20.3) */
    uint8_t  subnet_mask[4];    /* Subnet mask (default: 255.255.255.0) */
    uint8_t  target_host_ip[4]; /* Target PC IP (default: 192.168.20.2) */
    uint8_t  target_dev_ip[4];  /* Firmware target-side IP (default: 192.168.20.5) */
    uint8_t  portal_enabled;    /* Captive-portal DNS hijack: 0 = off, 1 = on */
    uint8_t  portal_mode;       /* 0 = off, 1 = greet once then let the OS probe
                                 *     settle, 2 = hijack every query forever */
    uint8_t  portal_ip[4];      /* Answer address; 0.0.0.0 = follow host_ip */
} ECM_IPConfig_t;               /* sizeof = 27 */

/* Complete storage structure in Flash (with verification) */
typedef struct {
    uint32_t magic;             /* Magic: ECM_IP_CONFIG_MAGIC */
    uint32_t schema_version;    /* Schema version (distinct from ECM_IPConfig_t.version) */
    uint32_t crc32;             /* CRC32 of config field */
    uint32_t write_count;       /* Write count for wear leveling */
    ECM_IPConfig_t config;      /* Actual network config */
    uint32_t footer_magic;      /* Footer magic, ensures complete write */
} ECM_IPConfig_Flash_t;         /* sizeof = 47 */

/* Error codes for CH9329 response */
#define ECM_IP_CONFIG_OK              0x00
#define ECM_IP_CONFIG_ERR_PARAM       0xE5  /* Invalid parameter */
#define ECM_IP_CONFIG_ERR_FLASH_ERASE 0xE9  /* Flash erase failed */
#define ECM_IP_CONFIG_ERR_FLASH_PROG  0xEA  /* Flash program failed */
#define ECM_IP_CONFIG_ERR_FLASH_VERIFY 0xEB /* Flash verify failed */

/* Initialize config (read from Flash or use defaults) */
void ECM_IPConfig_Init(void);

/* Get config (from RAM cache, no Flash access) */
void ECM_IPConfig_Get(ECM_IPConfig_t *config);
void ECM_IPConfig_GetHostIP(uint8_t ip[4]);
void ECM_IPConfig_GetDevIP(uint8_t ip[4]);
void ECM_IPConfig_GetSubnetMask(uint8_t mask[4]);
void ECM_IPConfig_GetTargetHostIP(uint8_t ip[4]);
void ECM_IPConfig_GetTargetDevIP(uint8_t ip[4]);

/* Validate config (check if parameters are valid before setting) */
bool ECM_IPConfig_Validate(const ECM_IPConfig_t *config);

/* Get current config from RAM cache (for building partial updates) */
void ECM_IPConfig_GetCurrent(ECM_IPConfig_t *config);

/* Set config (validate + write to Flash, dual-page wear leveling) */
/* Returns ECM_IP_CONFIG_OK on success, or error code */
uint8_t ECM_IPConfig_Set(const ECM_IPConfig_t *config);
uint8_t ECM_IPConfig_SetHostSide(const uint8_t host_ip[4],
                                  const uint8_t dev_ip[4],
                                  const uint8_t subnet[4]);
uint8_t ECM_IPConfig_SetTargetSide(const uint8_t target_host_ip[4],
                                   const uint8_t target_dev_ip[4]);

/* Reset to defaults */
uint8_t ECM_IPConfig_Reset(void);

/* Captive-portal DNS hijack configuration (stored with the IP config) */
void ECM_IPConfig_GetPortal(uint8_t *enabled, uint8_t *mode, uint8_t ip[4]);
uint8_t ECM_IPConfig_SetPortal(uint8_t enabled, uint8_t mode,
                               const uint8_t ip[4]);

#else /* USB_WORK_MODE_COMPILES_BRIDGE_ECM */

/* Stub functions when ECM bridge is not compiled */
#define ECM_IPConfig_Init()                  ((void)0)
#define ECM_IPConfig_Get(config)             ((void)0)
#define ECM_IPConfig_GetHostIP(ip)           ((void)0)
#define ECM_IPConfig_GetDevIP(ip)            ((void)0)
#define ECM_IPConfig_GetSubnetMask(mask)     ((void)0)
#define ECM_IPConfig_GetTargetHostIP(ip)     ((void)0)
#define ECM_IPConfig_GetTargetDevIP(ip)      ((void)0)
#define ECM_IPConfig_Validate(config)        (true)
#define ECM_IPConfig_GetCurrent(config)      ((void)0)
#define ECM_IPConfig_Set(config)             (0x00)
#define ECM_IPConfig_SetHostSide(h, d, s)    (0x00)
#define ECM_IPConfig_SetTargetSide(th, td)   (0x00)
#define ECM_IPConfig_Reset()                 (0x00)
#define ECM_IPConfig_GetPortal(e, m, ip)     ((void)0)
#define ECM_IPConfig_SetPortal(e, m, ip)     (0x00)

#endif /* USB_WORK_MODE_COMPILES_BRIDGE_ECM || USB_WORK_MODE_COMPILES_BRIDGE_NCM */

#ifdef __cplusplus
}
#endif

#endif /* ECM_IP_CONFIG_H */
