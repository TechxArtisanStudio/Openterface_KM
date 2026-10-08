/**
 * @file ecm_ip_config.c
 * @brief ECM bridge IP address configuration implementation
 *
 * Implements Flash-persistent ECM IP config storage following the same
 * pattern as usb_mode_config.c and default_target_config.c, with dual-page
 * wear leveling.
 *
 * Flash allocation:
 *   - USB Mode Config:       0x0806F000 (last 4KB)
 *   - Default Target Config: 0x0806E000 (second-to-last 4KB)
 *   - BLE Name Config:       0x0806D000 (third-to-last 4KB)
 *   - ECM IP Config Page A:  0x0806C000 (fourth-to-last 4KB)
 *   - ECM IP Config Page B:  0x0806B000 (fifth-to-last 4KB)
 */

#include "include/ecm_ip_config.h"
#include "ch32v20x_flash.h"
#include "debug.h"
#include <string.h>

#define ECM_IP_CONFIG_FLASH_BASE        0x08000000u
#define ECM_IP_CONFIG_FLASH_SIZE        (448u * 1024u)
#define ECM_IP_CONFIG_FLASH_PAGE_SIZE   4096u
#define ECM_IP_CONFIG_PAGE_A_ADDR       (ECM_IP_CONFIG_FLASH_BASE + ECM_IP_CONFIG_FLASH_SIZE - 4 * ECM_IP_CONFIG_FLASH_PAGE_SIZE)
#define ECM_IP_CONFIG_PAGE_B_ADDR       (ECM_IP_CONFIG_FLASH_BASE + ECM_IP_CONFIG_FLASH_SIZE - 5 * ECM_IP_CONFIG_FLASH_PAGE_SIZE)

/* Flash storage structure (includes validation metadata) */
typedef struct {
    uint32_t magic;              /* magic: ECM_IP_CONFIG_MAGIC */
    uint32_t schema_version;     /* schema version (vs. ECM_IPConfig_t.version) */
    uint32_t crc;                /* FNV-1a of config field */
    uint32_t write_count;        /* write counter for wear leveling */
    ECM_IPConfig_t config;       /* actual network config */
    uint32_t footer_magic;       /* footer magic, ensures complete write */
} ECM_IPConfig_Flash_t;          /* sizeof = 44 */

/* Default values */
#define ECM_IP_CONFIG_DEFAULT_HOST_IP        { 192, 168, 11, 1 }
#define ECM_IP_CONFIG_DEFAULT_DEV_IP         { 192, 168, 11, 3 }
#define ECM_IP_CONFIG_DEFAULT_SUBNET_MASK    { 255, 255, 255, 0 }
#define ECM_IP_CONFIG_DEFAULT_TARGET_HOST_IP { 192, 168, 11, 2 }
#define ECM_IP_CONFIG_DEFAULT_TARGET_DEV_IP  { 192, 168, 11, 5 }

static const ECM_IPConfig_t ECM_IP_CONFIG_DEFAULT = {
    .version         = ECM_IP_CONFIG_VERSION,
    .host_ip         = ECM_IP_CONFIG_DEFAULT_HOST_IP,
    .dev_ip          = ECM_IP_CONFIG_DEFAULT_DEV_IP,
    .subnet_mask     = ECM_IP_CONFIG_DEFAULT_SUBNET_MASK,
    .target_host_ip  = ECM_IP_CONFIG_DEFAULT_TARGET_HOST_IP,
    .target_dev_ip   = ECM_IP_CONFIG_DEFAULT_TARGET_DEV_IP,
    .reserved        = { 0, 0 },
    .padding         = 0,
};

/* RAM cache — runtime reads go here */
static ECM_IPConfig_t s_ram_cache;

/* ── FNV-1a hash (same algorithm as usb_mode_config.c) ──────────────── */

static uint32_t calc_fnv1a(const void *data, uint16_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t hash = 2166136261u;
    uint16_t i;

    for (i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

/* ── Flash page access (memory-mapped read) ─────────────────────────── */

static const ECM_IPConfig_Flash_t *flash_page_A(void)
{
    return (const ECM_IPConfig_Flash_t *)ECM_IP_CONFIG_PAGE_A_ADDR;
}

static const ECM_IPConfig_Flash_t *flash_page_B(void)
{
    return (const ECM_IPConfig_Flash_t *)ECM_IP_CONFIG_PAGE_B_ADDR;
}

static bool flash_page_is_valid(const ECM_IPConfig_Flash_t *page)
{
    return page->magic          == ECM_IP_CONFIG_MAGIC
        && page->footer_magic   == ECM_IP_CONFIG_MAGIC
        && page->schema_version == ECM_IP_CONFIG_VERSION
        && page->crc            == calc_fnv1a(&page->config, sizeof(page->config));
}

/* Scan both pages: used by both Init() and Set() */
static void scan_pages(bool *valid_a, bool *valid_b)
{
    const ECM_IPConfig_Flash_t *pa = flash_page_A();
    const ECM_IPConfig_Flash_t *pb = flash_page_B();

    *valid_a = flash_page_is_valid(pa);
    *valid_b = flash_page_is_valid(pb);
}

/* ── Parameter validation ───────────────────────────────────────────── */

static bool validate_subnet_mask(const uint8_t mask[4])
{
    uint32_t m = ((uint32_t)mask[0] << 24) | ((uint32_t)mask[1] << 16)
               | ((uint32_t)mask[2] << 8)  |  (uint32_t)mask[3];
    if (m == 0u || m == 0xFFFFFFFFu) return false;
    /* contiguous mask satisfies: (~m + 1) & m == 0 */
    return ((~m + 1u) & m) == 0u;
}

static bool validate_ip_basic(const uint8_t ip[4])
{
    if ((ip[0] | ip[1] | ip[2] | ip[3]) == 0u) return false;
    if ((ip[0] & ip[1] & ip[2] & ip[3]) == 0xFFu) return false;
    return true;
}

static bool validate_same_subnet(const uint8_t ip1[4],
                                 const uint8_t ip2[4],
                                 const uint8_t mask[4])
{
    uint8_t i;
    for (i = 0; i < 4; i++) {
        if ((ip1[i] & mask[i]) != (ip2[i] & mask[i])) return false;
    }
    return true;
}

static bool validate_config(const ECM_IPConfig_t *cfg)
{
    uint8_t i, j;
    const uint8_t *all_ips[4];

    if (cfg->version == 0u) return false;

    /* 1. Basic IP validity */
    if (!validate_ip_basic(cfg->host_ip))        return false;
    if (!validate_ip_basic(cfg->dev_ip))         return false;
    if (!validate_ip_basic(cfg->target_host_ip)) return false;
    if (!validate_ip_basic(cfg->target_dev_ip))  return false;

    /* 2. Subnet mask validity */
    if (!validate_subnet_mask(cfg->subnet_mask)) return false;

    /* 3. All IPs must be in the same subnet */
    if (!validate_same_subnet(cfg->host_ip, cfg->dev_ip, cfg->subnet_mask))
        return false;
    if (!validate_same_subnet(cfg->host_ip, cfg->target_host_ip, cfg->subnet_mask))
        return false;
    if (!validate_same_subnet(cfg->host_ip, cfg->target_dev_ip, cfg->subnet_mask))
        return false;

    /* 4. All IPs must be mutually distinct */
    all_ips[0] = cfg->host_ip;
    all_ips[1] = cfg->dev_ip;
    all_ips[2] = cfg->target_host_ip;
    all_ips[3] = cfg->target_dev_ip;
    for (i = 0; i < 4; i++) {
        for (j = (uint8_t)(i + 1); j < 4; j++) {
            if (memcmp(all_ips[i], all_ips[j], 4) == 0) return false;
        }
    }

    return true;
}

/* ── Initialization ─────────────────────────────────────────────────── */

void ECM_IPConfig_Init(void)
{
    bool valid_a, valid_b;
    const ECM_IPConfig_Flash_t *pa = flash_page_A();
    const ECM_IPConfig_Flash_t *pb = flash_page_B();

    scan_pages(&valid_a, &valid_b);

    if (valid_a && valid_b) {
        /* Both valid — pick the one with higher write_count */
        s_ram_cache = (pa->write_count >= pb->write_count)
                      ? pa->config : pb->config;
    } else if (valid_a) {
        s_ram_cache = pa->config;
    } else if (valid_b) {
        s_ram_cache = pb->config;
    } else {
        /* Both invalid — use defaults and persist to Flash */
        LOG_WARN("ECM IP config: no valid page, loading defaults\r\n");
        s_ram_cache = ECM_IP_CONFIG_DEFAULT;
        ECM_IPConfig_Set(&s_ram_cache);
    }
}

/* ── Getters (RAM cache, no Flash access) ────────────────────────────── */

void ECM_IPConfig_Get(ECM_IPConfig_t *config)
{
    if (config != NULL) {
        memcpy(config, &s_ram_cache, sizeof(ECM_IPConfig_t));
    }
}

void ECM_IPConfig_GetHostIP(uint8_t ip[4])
{
    memcpy(ip, s_ram_cache.host_ip, 4);
}

void ECM_IPConfig_GetDevIP(uint8_t ip[4])
{
    memcpy(ip, s_ram_cache.dev_ip, 4);
}

void ECM_IPConfig_GetSubnetMask(uint8_t mask[4])
{
    memcpy(mask, s_ram_cache.subnet_mask, 4);
}

void ECM_IPConfig_GetTargetHostIP(uint8_t ip[4])
{
    memcpy(ip, s_ram_cache.target_host_ip, 4);
}

void ECM_IPConfig_GetTargetDevIP(uint8_t ip[4])
{
    memcpy(ip, s_ram_cache.target_dev_ip, 4);
}

/* ── Setter core (shared by all Set variants) ────────────────────────── */

ECM_IPConfig_Status_t ECM_IPConfig_Set(const ECM_IPConfig_t *config)
{
    ECM_IPConfig_Flash_t to_write;
    bool valid_a, valid_b;
    const ECM_IPConfig_Flash_t *pa;
    const ECM_IPConfig_Flash_t *pb;
    uint32_t next_count;
    uint32_t target_addr;
    FLASH_Status flash_status;
    uint16_t word_index;
    const uint32_t *words;
    const ECM_IPConfig_Flash_t *verify_ptr;

    /* 1. Validate */
    if (!validate_config(config)) {
        LOG_WARN("ECM IP config: validation failed\r\n");
        return ECM_IP_CONFIG_ERR_PARAM;
    }

    /* 2. Scan pages, pick target */
    scan_pages(&valid_a, &valid_b);
    pa = flash_page_A();
    pb = flash_page_B();

    if (valid_a && valid_b) {
        if (pa->write_count <= pb->write_count) {
            target_addr = ECM_IP_CONFIG_PAGE_A_ADDR;
            next_count  = pa->write_count + 1u;
        } else {
            target_addr = ECM_IP_CONFIG_PAGE_B_ADDR;
            next_count  = pb->write_count + 1u;
        }
    } else if (valid_a) {
        target_addr = ECM_IP_CONFIG_PAGE_B_ADDR;
        next_count  = pa->write_count + 1u;
    } else if (valid_b) {
        target_addr = ECM_IP_CONFIG_PAGE_A_ADDR;
        next_count  = pb->write_count + 1u;
    } else {
        target_addr = ECM_IP_CONFIG_PAGE_A_ADDR;
        next_count  = 1u;
    }

    /* 3. Assemble Flash record */
    to_write.magic          = ECM_IP_CONFIG_MAGIC;
    to_write.schema_version = ECM_IP_CONFIG_VERSION;
    to_write.write_count    = next_count;
    to_write.config         = *config;
    to_write.crc            = calc_fnv1a(&to_write.config, sizeof(to_write.config));
    to_write.footer_magic   = ECM_IP_CONFIG_MAGIC;

    /* 4. Erase page */
    FLASH_Unlock();
    flash_status = FLASH_ErasePage(target_addr);
    if (flash_status != FLASH_COMPLETE) {
        FLASH_Lock();
        LOG_ERROR("ECM IP config: erase failed @0x%08lX\r\n", (unsigned long)target_addr);
        return ECM_IP_CONFIG_ERR_FLASH_ERASE;
    }

    /* 5. Program page (word by word) */
    words = (const uint32_t *)&to_write;
    for (word_index = 0; word_index < (sizeof(to_write) / sizeof(uint32_t)); word_index++) {
        flash_status = FLASH_ProgramWord(
            target_addr + ((uint32_t)word_index * sizeof(uint32_t)),
            words[word_index]
        );
        if (flash_status != FLASH_COMPLETE) {
            FLASH_Lock();
            LOG_ERROR("ECM IP config: program failed @0x%08lX\r\n",
                      (unsigned long)(target_addr + ((uint32_t)word_index * sizeof(uint32_t))));
            return ECM_IP_CONFIG_ERR_FLASH_PROGRAM;
        }
    }
    FLASH_Lock();

    /* 6. Read-back verify */
    verify_ptr = (const ECM_IPConfig_Flash_t *)target_addr;
    if (verify_ptr->crc           != to_write.crc
        || verify_ptr->footer_magic != ECM_IP_CONFIG_MAGIC
        || memcmp(&verify_ptr->config, &to_write.config, sizeof(ECM_IPConfig_t)) != 0) {
        LOG_ERROR("ECM IP config: verify mismatch\r\n");
        return ECM_IP_CONFIG_ERR_FLASH_VERIFY;
    }

    /* 7. Update RAM cache */
    s_ram_cache = *config;
    LOG_INFO("ECM IP config: saved (page %s, count=%lu)\r\n",
             target_addr == ECM_IP_CONFIG_PAGE_A_ADDR ? "A" : "B",
             (unsigned long)next_count);
    return ECM_IP_CONFIG_OK;
}

/* ── Partial setters ────────────────────────────────────────────────── */

ECM_IPConfig_Status_t ECM_IPConfig_SetHostSide(const uint8_t host_ip[4],
                                                const uint8_t dev_ip[4],
                                                const uint8_t subnet[4])
{
    ECM_IPConfig_t new_cfg;
    ECM_IPConfig_Get(&new_cfg);
    memcpy(new_cfg.host_ip, host_ip, 4);
    memcpy(new_cfg.dev_ip, dev_ip, 4);
    memcpy(new_cfg.subnet_mask, subnet, 4);
    return ECM_IPConfig_Set(&new_cfg);
}

ECM_IPConfig_Status_t ECM_IPConfig_SetTargetSide(const uint8_t target_host_ip[4],
                                                  const uint8_t target_dev_ip[4])
{
    ECM_IPConfig_t new_cfg;
    ECM_IPConfig_Get(&new_cfg);
    memcpy(new_cfg.target_host_ip, target_host_ip, 4);
    memcpy(new_cfg.target_dev_ip, target_dev_ip, 4);
    return ECM_IPConfig_Set(&new_cfg);
}

/* ── Reset to factory defaults ──────────────────────────────────────── */

ECM_IPConfig_Status_t ECM_IPConfig_Reset(void)
{
    return ECM_IPConfig_Set(&ECM_IP_CONFIG_DEFAULT);
}
