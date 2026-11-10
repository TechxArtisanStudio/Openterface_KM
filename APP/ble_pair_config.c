/**
 * @file ble_pair_config.c
 * @brief Flash-persistent BLE pairing configuration
 *
 * Mirrors the record layout used by ble_name_config.c / usb_mode_config.c:
 * magic, version, payload, sequence, FNV-1a CRC. A page only holds one record;
 * writes are rare (operator-driven), so no wear leveling is needed.
 *
 * Flash allocation: 0x0806A000 (the free page before the ECM IP config pages)
 */

#include "ble_pair_config.h"
#include "ch32v20x_flash.h"
#include <string.h>

#define BLE_PAIR_CONFIG_MAGIC       0x50454C42u  /* "BLEP" */
#define BLE_PAIR_CONFIG_VERSION     1u

typedef struct {
    uint32_t magic;
    uint8_t  record_version;
    uint8_t  max_bonds;
    uint8_t  has_pin_override;
    uint8_t  padding;
    uint32_t pin_override;
    uint32_t sequence;
    uint32_t crc;
} __attribute__((packed)) BLE_PairConfig_Record_t;

static uint8_t  g_max_bonds = BLE_PAIR_MIN_BONDS;
static uint8_t  g_has_pin_override = 0;
static uint32_t g_pin_override = BLE_PAIR_DEFAULT_PIN;

static uint32_t pairconfig_crc(const BLE_PairConfig_Record_t *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t crc = 2166136261u;
    uint16_t offset;

    for (offset = 0; offset < (sizeof(*record) - sizeof(record->crc)); offset++) {
        crc ^= bytes[offset];
        crc *= 16777619u;
    }
    return crc;
}

static const BLE_PairConfig_Record_t *flash_record(void)
{
    return (const BLE_PairConfig_Record_t *)BLE_PAIR_FLASH_ADDR;
}

static uint8_t max_bonds_valid(uint8_t value)
{
    return (value >= BLE_PAIR_MIN_BONDS && value <= BLE_PAIR_MAX_BONDS) ? 1u : 0u;
}

static BLE_PairStatus_t write_record(uint8_t max_bonds, uint8_t has_override, uint32_t pin)
{
    BLE_PairConfig_Record_t record;
    const BLE_PairConfig_Record_t *current = flash_record();
    FLASH_Status flash_status;
    uint16_t word_index;
    const uint32_t *words;

    memset(&record, 0, sizeof(record));
    record.magic = BLE_PAIR_CONFIG_MAGIC;
    record.record_version = BLE_PAIR_CONFIG_VERSION;
    record.max_bonds = max_bonds;
    record.has_pin_override = has_override;
    record.pin_override = has_override ? pin : 0u;
    record.sequence = (current->sequence == 0xFFFFFFFFu) ? 1u : current->sequence + 1u;
    record.crc = pairconfig_crc(&record);

    FLASH_Unlock();
    flash_status = FLASH_ErasePage(BLE_PAIR_FLASH_ADDR);
    if (flash_status != FLASH_COMPLETE) {
        FLASH_Lock();
        return BLE_PAIR_ERR_FLASH;
    }

    words = (const uint32_t *)&record;
    for (word_index = 0; word_index < (sizeof(record) / sizeof(uint32_t)); word_index++) {
        flash_status = FLASH_ProgramWord(BLE_PAIR_FLASH_ADDR + ((uint32_t)word_index * sizeof(uint32_t)),
                                         words[word_index]);
        if (flash_status != FLASH_COMPLETE) {
            FLASH_Lock();
            return BLE_PAIR_ERR_FLASH;
        }
    }
    FLASH_Lock();

    if (flash_record()->magic != record.magic || flash_record()->crc != record.crc) {
        return BLE_PAIR_ERR_FLASH;
    }

    g_max_bonds = max_bonds;
    g_has_pin_override = has_override;
    g_pin_override = has_override ? pin : BLE_PAIR_DEFAULT_PIN;
    return BLE_PAIR_OK;
}

void BLE_PairConfig_Init(void)
{
    const BLE_PairConfig_Record_t *record = flash_record();

    g_max_bonds = BLE_PAIR_MIN_BONDS;
    g_has_pin_override = 0;
    g_pin_override = BLE_PAIR_DEFAULT_PIN;

    if (record->magic != BLE_PAIR_CONFIG_MAGIC ||
        record->record_version != BLE_PAIR_CONFIG_VERSION ||
        record->crc != pairconfig_crc(record)) {
        return;
    }

    if (max_bonds_valid(record->max_bonds)) {
        g_max_bonds = record->max_bonds;
    }
    if (record->has_pin_override && record->pin_override <= BLE_PAIR_PIN_MAX) {
        g_has_pin_override = 1;
        g_pin_override = record->pin_override;
    }
}

uint8_t BLE_PairConfig_GetMaxBonds(void)
{
    return g_max_bonds;
}

uint32_t BLE_PairConfig_GetEffectivePin(void)
{
    return g_pin_override;
}

uint8_t BLE_PairConfig_HasPinOverride(void)
{
    return g_has_pin_override;
}

BLE_PairStatus_t BLE_PairConfig_SetPin(uint32_t pin)
{
    if (pin > BLE_PAIR_PIN_MAX) {
        return BLE_PAIR_ERR_PARAM;
    }
    return write_record(g_max_bonds, 1u, pin);
}

BLE_PairStatus_t BLE_PairConfig_SetMaxBonds(uint8_t max_bonds)
{
    if (!max_bonds_valid(max_bonds)) {
        return BLE_PAIR_ERR_PARAM;
    }
    return write_record(max_bonds, g_has_pin_override, g_pin_override);
}

BLE_PairStatus_t BLE_PairConfig_ResetAll(void)
{
    return write_record(BLE_PAIR_MIN_BONDS, 0u, BLE_PAIR_DEFAULT_PIN);
}
