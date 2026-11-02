/**
 * @file default_target_config.h
 * @brief Flash-based configuration for default target IP/port
 *
 * Stores default target IP address and port in flash for auto-connect
 * on BLE link-up. Falls back to compile-time defaults if not configured.
 */

#ifndef __DEFAULT_TARGET_CONFIG_H
#define __DEFAULT_TARGET_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Default target configuration structure
 */
typedef struct {
    uint8_t  ip[4];       /**< Target IP address (big-endian) */
    uint16_t port;        /**< Target port (big-endian) */
    uint8_t  enabled;     /**< 1 = auto-connect enabled, 0 = disabled */
    uint8_t  reserved[9]; /**< Padding for alignment */
} DefaultTargetConfig_t;

/**
 * @brief Initialize default target configuration
 *
 * Reads config from flash. If invalid/missing, uses compile-time defaults.
 */
void DefaultTargetConfig_Init(void);

/**
 * @brief Get current default target IP
 *
 * @param ip  Output buffer (4 bytes, big-endian)
 */
void DefaultTargetConfig_GetIP(uint8_t ip[4]);

/**
 * @brief Get current default target port
 *
 * @return Target port (big-endian)
 */
uint16_t DefaultTargetConfig_GetPort(void);

/**
 * @brief Check if auto-connect is enabled
 *
 * @return true if auto-connect is enabled
 */
bool DefaultTargetConfig_IsEnabled(void);

/**
 * @brief Set default target IP/port and enable auto-connect
 *
 * @param ip   Target IP address (4 bytes, big-endian)
 * @param port Target port (big-endian)
 * @return true if successfully written to flash
 */
bool DefaultTargetConfig_Set(const uint8_t ip[4], uint16_t port);

/**
 * @brief Update only the default target IP, keeping the stored port and the
 *        auto-connect enable state unchanged.
 *
 * @param ip Target IP address (4 bytes, big-endian)
 * @return true if successfully written to flash
 */
bool DefaultTargetConfig_SetIP(const uint8_t ip[4]);

/**
 * @brief Disable auto-connect (clears flash config)
 *
 * @return true if successfully cleared
 */
bool DefaultTargetConfig_Disable(void);

#ifdef __cplusplus
}
#endif

#endif /* __DEFAULT_TARGET_CONFIG_H */
