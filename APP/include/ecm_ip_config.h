/**
 * @file ecm_ip_config.h
 * @brief ECM bridge IP address configuration with Flash persistence
 *
 * Stores 5 IP addresses (host, dev, subnet, target_host, target_dev) in
 * Flash using dual-page wear leveling with FNV-1a validation. Supports
 * runtime read/write via CH9329 commands, with immediate bridge reconfiguration.
 *
 * Only compiled when USB_WORK_MODE_COMPILES_BRIDGE_ECM is defined.
 */

#ifndef ECM_IP_CONFIG_H
#define ECM_IP_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ECM_IP_CONFIG_MAGIC        0x45434D49u  /* "ECMI" */
#define ECM_IP_CONFIG_VERSION      1u

/* User-visible network configuration (24 bytes, 4-byte aligned) */
typedef struct {
    uint8_t  version;           /* config version, currently 1 */
    uint8_t  host_ip[4];        /* host-side IP (default: 192.168.11.1) */
    uint8_t  dev_ip[4];         /* firmware host-side IP (default: 192.168.11.3) */
    uint8_t  subnet_mask[4];    /* subnet mask (default: 255.255.255.0) */
    uint8_t  target_host_ip[4]; /* target PC IP (default: 192.168.11.2) */
    uint8_t  target_dev_ip[4];  /* firmware target-side IP (default: 192.168.11.5) */
    uint8_t  reserved[2];       /* reserved for future use */
    uint8_t  padding;           /* padding for 4-byte alignment */
} ECM_IPConfig_t;               /* sizeof = 24 */

/**
 * @brief ECM IP config operation results
 *
 * Distinguishes between input validation failures and the specific
 * Flash operation that failed (erase / program / verify). Callers map
 * these to CH9329 status codes before sending a response.
 */
typedef enum {
    ECM_IP_CONFIG_OK = 0,            /**< Operation succeeded */
    ECM_IP_CONFIG_ERR_PARAM,         /**< Validation failed (bad IP, subnet, etc.) */
    ECM_IP_CONFIG_ERR_FLASH_ERASE,   /**< Flash page erase failed */
    ECM_IP_CONFIG_ERR_FLASH_PROGRAM, /**< Flash word program failed */
    ECM_IP_CONFIG_ERR_FLASH_VERIFY   /**< Flash read-back verification mismatched */
} ECM_IPConfig_Status_t;

/* Initialize config from Flash (or use defaults + persist) */
void ECM_IPConfig_Init(void);

/* Getters — read from RAM cache, no Flash access */
void ECM_IPConfig_Get(ECM_IPConfig_t *config);
void ECM_IPConfig_GetHostIP(uint8_t ip[4]);
void ECM_IPConfig_GetDevIP(uint8_t ip[4]);
void ECM_IPConfig_GetSubnetMask(uint8_t mask[4]);
void ECM_IPConfig_GetTargetHostIP(uint8_t ip[4]);
void ECM_IPConfig_GetTargetDevIP(uint8_t ip[4]);

/* Setters — validate, write Flash with dual-page wear leveling */
ECM_IPConfig_Status_t ECM_IPConfig_Set(const ECM_IPConfig_t *config);
ECM_IPConfig_Status_t ECM_IPConfig_SetHostSide(const uint8_t host_ip[4],
                                                const uint8_t dev_ip[4],
                                                const uint8_t subnet[4]);
ECM_IPConfig_Status_t ECM_IPConfig_SetTargetSide(const uint8_t target_host_ip[4],
                                                  const uint8_t target_dev_ip[4]);

/* Reset to factory defaults */
ECM_IPConfig_Status_t ECM_IPConfig_Reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ECM_IP_CONFIG_H */
