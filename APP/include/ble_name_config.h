/**
 * @file ble_name_config.h
 * @brief BLE device name dynamic configuration with Flash persistence
 *
 * Supports runtime modification of BLE device name via CH9329 command,
 * with persistent storage in Flash. Factory reset restores default name.
 *
 * 【Compile-time guarantee】Only enabled when ENABLE_BLE_NAME_CONFIG is defined (KVM-GO).
 * MiniKVM firmware (built without ENABLE_BLE_NAME_CONFIG) has zero BLE name config code.
 */

#ifndef _BLE_NAME_CONFIG_H
#define _BLE_NAME_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "Version_selection.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BLE name configuration operation results
 *
 * Distinguishes between input validation failures and the specific
 * Flash operation that failed (erase / program / verify). Callers map
 * these to CH9329 status codes before sending a response.
 *
 * This enum is always defined (both KVM-GO and MiniKVM builds) so that
 * command handlers can use the symbolic names regardless of build target.
 * MiniKVM macros return matching integer literals.
 */
typedef enum {
    BLE_NAME_OK = 0,               /**< Operation succeeded */
    BLE_NAME_ERR_NULL_NAME,        /**< new_name was NULL */
    BLE_NAME_ERR_INVALID_LENGTH,   /**< Name length was 0 or > 20 */
    BLE_NAME_ERR_FLASH_ERASE,      /**< Flash page erase failed */
    BLE_NAME_ERR_FLASH_PROGRAM,    /**< Flash word program failed */
    BLE_NAME_ERR_FLASH_VERIFY      /**< Flash read-back verification mismatched */
} BLE_NameConfig_Status_t;

#if (ENABLE_BLE_NAME_CONFIG == 1)

/**
 * @brief Initialize BLE name configuration module
 *
 * Loads saved custom name from Flash (if exists and valid).
 * Falls back to factory default name (BLE_DEVICE_NAME) if no valid config.
 * Initializes runtime state structure @ref g_ble_name.
 *
 * This function must be called before BLE stack initialization
 * (i.e., early in Peripheral_Init()).
 *
 * 【Compile-time guarantee】Only exists when ENABLE_BLE is defined.
 */
void BLE_NameConfig_Init(void);

/**
 * @brief Set new BLE device name and persist to Flash
 *
 * Validates input name: length must be 1-20 bytes (excluding null terminator).
 * If valid, erases Flash page and writes new config record.
 * Updates runtime state, advertising data, and GAP characteristics on success.
 * Name change takes effect in next advertising cycle (via advertising restart).
 *
 * @param new_name  Pointer to null-terminated string with new name
 * @return BLE_NAME_OK if name successfully set and persisted
 * @return BLE_NAME_ERR_NULL_NAME if new_name is NULL
 * @return BLE_NAME_ERR_INVALID_LENGTH if name length was 0 or > 20
 * @return BLE_NAME_ERR_FLASH_ERASE if Flash page erase failed
 * @return BLE_NAME_ERR_FLASH_PROGRAM if Flash word program failed
 * @return BLE_NAME_ERR_FLASH_VERIFY if Flash read-back verification mismatched
 *
 * @note This function is not interrupt-safe; caller must ensure non-interrupt context.
 * @note 【Compile-time guarantee】Only exists when ENABLE_BLE is defined.
 */
BLE_NameConfig_Status_t BLE_NameConfig_Set(const char *new_name);

/**
 * @brief Get current active BLE device name
 *
 * Returns read-only pointer to internal name buffer.
 * Returned string is always null-terminated.
 * Do not modify the returned buffer contents.
 *
 * @return Pointer to current name (const char*)
 *
 * @note Return value may change after calling BLE_NameConfig_Set() or BLE_NameConfig_Reset().
 * @note 【Compile-time guarantee】Only exists when ENABLE_BLE is defined.
 */
const char *BLE_NameConfig_Get(void);

/**
 * @brief Reset BLE device name to factory default
 *
 * Erases Flash page containing BLE name config record.
 * Re-initializes runtime state to factory default name (BLE_DEVICE_NAME).
 * Updates advertising data and GAP characteristics to reflect default name.
 * Restarts advertising to make change immediate.
 *
 * This operation is idempotent: multiple calls have same effect.
 *
 * @return BLE_NAME_OK if Flash erase succeeded
 * @return BLE_NAME_ERR_FLASH_ERASE if Flash erase failed (hardware fault)
 *
 * @note This function is not interrupt-safe; caller must ensure non-interrupt context.
 * @note 【Compile-time guarantee】Only exists when ENABLE_BLE is defined.
 */
BLE_NameConfig_Status_t BLE_NameConfig_Reset(void);

/**
 * @brief Check if current name is custom (non-factory-default)
 *
 * Convenience for upper-layer logic to determine if "modified" status should be displayed.
 *
 * @return true  if current name was set via BLE_NameConfig_Set()
 * @return false if current name is factory default (BLE_DEVICE_NAME)
 *
 * @note Depends on BLE_NameConfig_Init() having been called correctly.
 * @note 【Compile-time guarantee】Only exists when ENABLE_BLE is defined.
 */
bool BLE_NameConfig_IsCustom(void);

/**
 * @brief Runtime state structure for BLE name configuration
 */
typedef struct {
    char     name[21];        /**< Current active name (null-terminated) */
    bool     is_custom;       /**< true if using custom name, false if factory default */
} BLE_NameConfig_t;

/** Global runtime state instance */
extern BLE_NameConfig_t g_ble_name;

#else /* !ENABLE_BLE_NAME_CONFIG */

/**
 * @brief Empty implementation macros (MiniKVM compilation eliminates call-site code)
 *
 * When ENABLE_BLE_NAME_CONFIG is not 1, provides empty definitions so call code compiles
 * but produces no instructions. Avoids need for #ifdef in peripheral.c etc.
 */
#define BLE_NameConfig_Init()           do {} while(0)
#define BLE_NameConfig_Set(x)           (BLE_NAME_ERR_NULL_NAME)
#define BLE_NameConfig_Get()            (BLE_DEVICE_NAME)
#define BLE_NameConfig_Reset()          (BLE_NAME_ERR_FLASH_ERASE)
#define BLE_NameConfig_IsCustom()       (false)

typedef struct {
    char name[21];
    bool is_custom;
} BLE_NameConfig_t;

extern BLE_NameConfig_t g_ble_name;  /* Still declared to avoid compile errors, but unusable */

#endif /* ENABLE_BLE */

#ifdef __cplusplus
}
#endif

#endif /* _BLE_NAME_CONFIG_H */
