/**
 * @file ble_name_config.c
 * @brief BLE device name dynamic configuration implementation
 *
 * Implements Flash-persistent BLE name storage following the same pattern
 * as usb_mode_config.c and default_target_config.c.
 *
 * Storage layout:
 *   - Magic: 0x424C454E ("BLEN" in ASCII)
 *   - Version: 1
 *   - Name: up to 20 characters + null terminator (21 bytes)
 *   - Name length: actual name length (excluding null terminator)
 *   - Sequence: incremented on each write (protects against old config overwriting new)
 *   - CRC: FNV-1a based checksum over all fields except crc itself
 *
 * Flash allocation: 0x0806D000 (third-to-last 4KB page)
 *   - USB Mode Config:       0x0806F000 (last 4KB)
 *   - Default Target Config: 0x0806E000 (second-to-last 4KB)
 *   - BLE Name Config:       0x0806D000 (third-to-last 4KB)
 *
 * 【Compile-time guarantee】Entire file is excluded when ENABLE_BLE_NAME_CONFIG is not 1.
 */

#include "ble_name_config.h"
#include <string.h>
#include "ch32v20x_flash.h"
#include <string.h>

#if (ENABLE_BLE_NAME_CONFIG == 1)

#define BLE_NAME_CONFIG_MAGIC             0x424C454Eu  /* "BLEN" */
#define BLE_NAME_CONFIG_VERSION           1u
#define BLE_NAME_CONFIG_FLASH_BASE        0x08000000u
#define BLE_NAME_CONFIG_FLASH_SIZE        (448u * 1024u)  /* 448KB */
#define BLE_NAME_CONFIG_FLASH_PAGE_SIZE   4096u           /* 4KB page */

/* Store at offset -12KB from end (after USB mode config and default target config) */
#define BLE_NAME_CONFIG_FLASH_ADDR        (BLE_NAME_CONFIG_FLASH_BASE + \
                                           BLE_NAME_CONFIG_FLASH_SIZE - \
                                           3 * BLE_NAME_CONFIG_FLASH_PAGE_SIZE)

typedef struct {
    uint32_t magic;           /**< Magic number: 0x424C454E ("BLEN") */
    uint8_t  record_version;  /**< Record version number (currently fixed at 1) */
    uint8_t  name[21];        /**< Name buffer (max 20 chars + null terminator) */
    uint8_t  name_length;     /**< Actual name length (excluding null terminator) */
    uint8_t  padding;         /**< Padding to align structure size to 4 bytes (Flash programming requirement) */
    uint32_t sequence;        /**< Sequence number (increments on each write) */
    uint32_t crc;             /**< CRC32 of all fields except this crc field */
} __attribute__((packed)) BLE_NameConfig_Record_t;

/** Runtime state */
#ifndef BLE_DEVICE_NAME
#define BLE_DEVICE_NAME "Mini-KVM"
#endif
BLE_NameConfig_t g_ble_name = {
    .name = BLE_DEVICE_NAME,
    .is_custom = false
};

/**
 * @brief Calculate FNV-1a based configuration CRC
 *
 * Uses same algorithm as usb_mode_config.c and default_target_config.c:
 *   FNV offset basis: 2166136261
 *   FNV prime: 16777619
 *
 * @param record  Pointer to configuration record (crc field ignored)
 * @return Computed CRC value
 */
static uint32_t nameconfig_crc(const BLE_NameConfig_Record_t *record)
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

/**
 * @brief Get pointer to Flash resident configuration record
 *
 * @return Pointer to Flash storage location for BLE name config
 */
static const BLE_NameConfig_Record_t *flash_record(void)
{
    return (const BLE_NameConfig_Record_t *)BLE_NAME_CONFIG_FLASH_ADDR;
}

/**
 * @brief Initialize BLE name configuration from Flash
 *
 * Loads configuration from Flash if valid; otherwise uses factory defaults.
 * Valid configuration requires:
 *   - Correct magic number (0x424C454E)
 *   - Correct version (1)
 *   - Valid name length (0-20)
 *   - Valid CRC
 */
void BLE_NameConfig_Init(void)
{
    const BLE_NameConfig_Record_t *record = flash_record();

    /* Set factory defaults */
    g_ble_name.is_custom = false;
    strcpy(g_ble_name.name, BLE_DEVICE_NAME);

    /* Debug: print Flash record info */

    /* Try to load valid configuration from Flash */
    if (record->magic == BLE_NAME_CONFIG_MAGIC &&
        record->record_version == BLE_NAME_CONFIG_VERSION &&
        record->name_length <= 20) {
        uint32_t computed_crc = nameconfig_crc(record);
        if (record->crc == computed_crc) {
            /* Copy name from Flash (ensuring null termination) */
            memcpy(g_ble_name.name, record->name, record->name_length);
            g_ble_name.name[record->name_length] = '\0';
            g_ble_name.is_custom = true;
        } else {
        }
    } else {
    }
}

/**
 * @brief Set new BLE device name and persist to Flash
 *
 * @param new_name  Null-terminated string containing new name (1-20 characters)
 * @return BLE_NAME_OK if name successfully set and persisted to Flash
 * @return BLE_NAME_ERR_NULL_NAME if new_name is NULL
 * @return BLE_NAME_ERR_INVALID_LENGTH if name length is 0 or > 20
 * @return BLE_NAME_ERR_FLASH_ERASE if Flash page erase failed
 * @return BLE_NAME_ERR_FLASH_PROGRAM if Flash word program failed
 * @return BLE_NAME_ERR_FLASH_VERIFY if Flash read-back verification failed
 */
BLE_NameConfig_Status_t BLE_NameConfig_Set(const char *new_name)
{
    BLE_NameConfig_Record_t record;
    FLASH_Status status;
    uint16_t word_index;
    const uint32_t *words;
    size_t name_len;

    /* Validate input */
    if (new_name == NULL) {
        return BLE_NAME_ERR_NULL_NAME;
    }
    name_len = strlen(new_name);
    if (name_len == 0 || name_len > 20) {
        return BLE_NAME_ERR_INVALID_LENGTH;
    }


    /* Prepare configuration record for Flash */
    memset(&record, 0, sizeof(record));
    record.magic = BLE_NAME_CONFIG_MAGIC;
    record.record_version = BLE_NAME_CONFIG_VERSION;
    memcpy(record.name, new_name, name_len);
    record.name_length = (uint8_t)name_len;

    /* Read current sequence from Flash */
    const BLE_NameConfig_Record_t *current = flash_record();
    uint32_t current_seq = current->sequence;

    /* Handle unprogrammed Flash (all 0xFF) */
    if (current_seq == 0xFFFFFFFFu) {
        record.sequence = 1u;
    } else {
        record.sequence = current_seq + 1u;
    }
    record.crc = nameconfig_crc(&record);


    /* Write to Flash: erase then program */
    FLASH_Unlock();
    status = FLASH_ErasePage(BLE_NAME_CONFIG_FLASH_ADDR);
    if (status != FLASH_COMPLETE) {
        FLASH_Lock();
        return BLE_NAME_ERR_FLASH_ERASE;
    }

    words = (const uint32_t *)&record;
    for (word_index = 0; word_index < (sizeof(record) / sizeof(uint32_t)); word_index++) {
        status = FLASH_ProgramWord(BLE_NAME_CONFIG_FLASH_ADDR + ((uint32_t)word_index * sizeof(uint32_t)),
                                  words[word_index]);
        if (status != FLASH_COMPLETE) {
            FLASH_Lock();
            return BLE_NAME_ERR_FLASH_PROGRAM;
        }
    }
    FLASH_Lock();

    /* Verify Flash write by reading back */
    const BLE_NameConfig_Record_t *verify = flash_record();
    if (verify->magic != record.magic || verify->crc != record.crc) {
        return BLE_NAME_ERR_FLASH_VERIFY;
    }

    /* Update runtime state on successful Flash write */
    memcpy(g_ble_name.name, new_name, name_len);
    g_ble_name.name[name_len] = '\0';
    g_ble_name.is_custom = true;
    return BLE_NAME_OK;
}

/**
 * @brief Get current active BLE device name
 *
 * @return Pointer to null-terminated current name string
 */
const char *BLE_NameConfig_Get(void)
{
    return g_ble_name.name;
}

/**
 * @brief Reset BLE device name to factory default
 *
 * Erases Flash configuration and restores runtime state to BLE_DEVICE_NAME.
 *
 * @return BLE_NAME_OK if Flash erase succeeded
 * @return BLE_NAME_ERR_FLASH_ERASE if Flash erase failed (hardware fault)
 */
BLE_NameConfig_Status_t BLE_NameConfig_Reset(void)
{
    FLASH_Status status;

    FLASH_Unlock();
    status = FLASH_ErasePage(BLE_NAME_CONFIG_FLASH_ADDR);
    FLASH_Lock();

    if (status == FLASH_COMPLETE) {
        /* Reset runtime state to factory defaults */
        g_ble_name.is_custom = false;
        strcpy(g_ble_name.name, BLE_DEVICE_NAME);
        return BLE_NAME_OK;
    }
    return BLE_NAME_ERR_FLASH_ERASE;
}

/**
 * @brief Check if current name is custom (non-factory-default)
 *
 * @return true if using custom name set via BLE_NameConfig_Set()
 * @return false if using factory default name (BLE_DEVICE_NAME)
 */
bool BLE_NameConfig_IsCustom(void)
{
    return g_ble_name.is_custom;
}

#else /* !ENABLE_BLE_NAME_CONFIG */

/* Provide a definition for g_ble_name when BLE name config is disabled.
 * This is referenced by peripheral.c but the functions are empty macros. */
#ifndef BLE_DEVICE_NAME
#define BLE_DEVICE_NAME "Mini-KVM"
#endif
BLE_NameConfig_t g_ble_name = {
    .name = BLE_DEVICE_NAME,
    .is_custom = false
};

#endif /* ENABLE_BLE_NAME_CONFIG */