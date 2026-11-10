/**
 * @file ble_pair_config.h
 * @brief BLE pairing configuration: PIN override and bond slot limit
 *
 * Both values live in one Flash page (0x0806A000) so a factory reset and a
 * PIN change touch a single record. Everything not stored in Flash falls back
 * to the factory defaults below, which is also what a corrupt or blank page
 * resolves to.
 */

#ifndef BLE_PAIR_CONFIG_H
#define BLE_PAIR_CONFIG_H

#include <stdint.h>

/** Initial pairing code. Every unit leaves the factory with this PIN. */
#define BLE_PAIR_DEFAULT_PIN        666666u
#define BLE_PAIR_PIN_MAX            999999u

/** Bond slots: one phone by default, configurable up to the link DB limit. */
#define BLE_PAIR_MIN_BONDS          1u
#define BLE_PAIR_MAX_BONDS          4u

/** Dedicated 4 KB page, below the ECM IP / name / mode config pages. */
#define BLE_PAIR_FLASH_ADDR         0x0806A000u

typedef enum {
    BLE_PAIR_OK = 0,
    BLE_PAIR_ERR_PARAM,   /**< Value outside the accepted range */
    BLE_PAIR_ERR_FLASH,   /**< Erase / program / verify failed */
} BLE_PairStatus_t;

/** Load the record from Flash; falls back to defaults when invalid. */
void BLE_PairConfig_Init(void);

/** Configured bond slot limit (always 1..BLE_PAIR_MAX_BONDS). */
uint8_t BLE_PairConfig_GetMaxBonds(void);

/** PIN to hand to the bond manager: the override if set, else 666666. */
uint32_t BLE_PairConfig_GetEffectivePin(void);

/** 1 when a runtime PIN override is in effect. */
uint8_t BLE_PairConfig_HasPinOverride(void);

/** Persist a new PIN override (0..999999). */
BLE_PairStatus_t BLE_PairConfig_SetPin(uint32_t pin);

/** Persist a new bond slot limit (1..4). */
BLE_PairStatus_t BLE_PairConfig_SetMaxBonds(uint8_t max_bonds);

/** Factory reset: drop the PIN override and restore a single bond slot. */
BLE_PairStatus_t BLE_PairConfig_ResetAll(void);

#endif /* BLE_PAIR_CONFIG_H */
