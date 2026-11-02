/**
 * @file default_target_config.c
 * @brief Flash-based configuration for default target IP/port
 */

#include "include/default_target_config.h"
#include "include/ble_eth_protocol.h"
#include "include/ecm_ip_config.h"
#include "ch32v20x_flash.h"
#include <string.h>

#define DEFAULT_TARGET_CONFIG_MAGIC             0x44544346u  /* "DTCF" */
#define DEFAULT_TARGET_CONFIG_VERSION           1u
#define DEFAULT_TARGET_CONFIG_FLASH_BASE        0x08000000u
#define DEFAULT_TARGET_CONFIG_FLASH_SIZE        (448u * 1024u)
#define DEFAULT_TARGET_CONFIG_FLASH_PAGE_SIZE   4096u
/* Store at offset -8KB from end (USB mode config is at -4KB) */
#define DEFAULT_TARGET_CONFIG_FLASH_ADDR        (DEFAULT_TARGET_CONFIG_FLASH_BASE + DEFAULT_TARGET_CONFIG_FLASH_SIZE - 2 * DEFAULT_TARGET_CONFIG_FLASH_PAGE_SIZE)

typedef struct {
    uint32_t magic;
    uint8_t  record_version;
    uint8_t  ip[4];
    uint16_t port;
    uint8_t  enabled;
    uint32_t sequence;
    uint32_t crc;
} DefaultTargetConfig_Record_t;

static DefaultTargetConfig_t g_config;

static uint32_t config_crc(const DefaultTargetConfig_Record_t *record)
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

static const DefaultTargetConfig_Record_t *flash_record(void)
{
    return (const DefaultTargetConfig_Record_t *)DEFAULT_TARGET_CONFIG_FLASH_ADDR;
}

void DefaultTargetConfig_Init(void)
{
    const DefaultTargetConfig_Record_t *record = flash_record();

    /* Set compile-time defaults */
#if USB_WORK_MODE_COMPILES_BRIDGE_ECM
    /* In ECM bridge builds the target lives on the ECM target subnet, so
     * derive the fallback from the configured target host address instead of
     * the legacy compile-time IP. ECM_IPConfig_Init() runs before this. */
    ECM_IPConfig_GetTargetHostIP(g_config.ip);
#else
    {
        const uint8_t default_ip[4] = BLE_ETH_DEFAULT_TARGET_IP;
        memcpy(g_config.ip, default_ip, 4);
    }
#endif
    g_config.port = BLE_ETH_DEFAULT_TARGET_PORT;
    g_config.enabled = false;

    /* Try to load from flash */
    if (record->magic == DEFAULT_TARGET_CONFIG_MAGIC &&
        record->record_version == DEFAULT_TARGET_CONFIG_VERSION &&
        record->crc == config_crc(record)) {
        memcpy(g_config.ip, record->ip, 4);
        g_config.port = record->port;
        g_config.enabled = (record->enabled != 0);
    }
}

void DefaultTargetConfig_GetIP(uint8_t ip[4])
{
    memcpy(ip, g_config.ip, 4);
}

uint16_t DefaultTargetConfig_GetPort(void)
{
    return g_config.port;
}

bool DefaultTargetConfig_IsEnabled(void)
{
    return g_config.enabled;
}

static bool write_record(const uint8_t ip[4], uint16_t port, bool enabled)
{
    DefaultTargetConfig_Record_t record;
    FLASH_Status status;
    uint16_t word_index;
    const uint32_t *words;

    record.magic = DEFAULT_TARGET_CONFIG_MAGIC;
    record.record_version = DEFAULT_TARGET_CONFIG_VERSION;
    memcpy(record.ip, ip, 4);
    record.port = port;
    record.enabled = enabled ? 1 : 0;
    record.sequence = flash_record()->sequence + 1u;
    record.crc = config_crc(&record);

    FLASH_Unlock();
    status = FLASH_ErasePage(DEFAULT_TARGET_CONFIG_FLASH_ADDR);
    if (status == FLASH_COMPLETE) {
        words = (const uint32_t *)&record;
        for (word_index = 0; word_index < (sizeof(record) / sizeof(uint32_t)); word_index++) {
            status = FLASH_ProgramWord(
                DEFAULT_TARGET_CONFIG_FLASH_ADDR + ((uint32_t)word_index * sizeof(uint32_t)),
                words[word_index]
            );
            if (status != FLASH_COMPLETE) {
                break;
            }
        }
    }
    FLASH_Lock();

    if (status == FLASH_COMPLETE) {
        DefaultTargetConfig_Init();
        return true;
    }
    return false;
}

bool DefaultTargetConfig_Set(const uint8_t ip[4], uint16_t port)
{
    return write_record(ip, port, true);
}

bool DefaultTargetConfig_SetIP(const uint8_t ip[4])
{
    /* Keep the stored port and auto-connect selection; only the address moves. */
    return write_record(ip, g_config.port, g_config.enabled);
}

bool DefaultTargetConfig_Disable(void)
{
    FLASH_Status status;

    FLASH_Unlock();
    status = FLASH_ErasePage(DEFAULT_TARGET_CONFIG_FLASH_ADDR);
    FLASH_Lock();

    if (status == FLASH_COMPLETE) {
        DefaultTargetConfig_Init();
        return true;
    }
    return false;
}
