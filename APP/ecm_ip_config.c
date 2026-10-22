#include "include/ecm_ip_config.h"

#if USB_WORK_MODE_COMPILES_BRIDGE_ECM

#include "ch32v20x_flash.h"
#include "include/ecm_bridge.h"
#include "include/default_target_config.h"
#include "include/ble_eth_protocol.h"
#include <string.h>

/* Default configuration */
static const ECM_IPConfig_t ECM_IP_CONFIG_DEFAULT = {
    .record_version = ECM_IP_CONFIG_VERSION,
    .host_ip        = ECM_IP_CONFIG_DEFAULT_HOST_IP,
    .dev_ip         = ECM_IP_CONFIG_DEFAULT_DEV_IP,
    .subnet_mask    = ECM_IP_CONFIG_DEFAULT_SUBNET_MASK,
    .target_host_ip = ECM_IP_CONFIG_DEFAULT_TARGET_HOST_IP,
    .target_dev_ip  = ECM_IP_CONFIG_DEFAULT_TARGET_DEV_IP,
    .reserved       = { 0, 0 },
    .padding        = 0,
};

/* RAM cache - runtime read-only access */
static ECM_IPConfig_t s_ram_cache;

/* CRC32 calculation (FNV-1a variant, same as usb_mode_config.c) */
static uint32_t calc_crc32(const void *data, uint32_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 2166136261u;
    uint32_t i;

    for (i = 0; i < len; i++) {
        crc ^= bytes[i];
        crc *= 16777619u;
    }
    return crc;
}

/* Validate subnet mask: must be contiguous 1s followed by contiguous 0s */
static bool validate_subnet_mask(const uint8_t mask[4])
{
    uint32_t m = ((uint32_t)mask[0] << 24) | ((uint32_t)mask[1] << 16)
               | ((uint32_t)mask[2] << 8)  |  (uint32_t)mask[3];
    if (m == 0u || m == 0xFFFFFFFFu) return false;
    /* Contiguous mask: inverted mask (~m) must be of form 2^n - 1
     * i.e., (~m) & ((~m) + 1) == 0 */
    uint32_t inv = ~m;
    return (inv & (inv + 1u)) == 0u;
}

/* Validate IP: cannot be 0.0.0.0 or 255.255.255.255 */
static bool validate_ip_basic(const uint8_t ip[4])
{
    if ((ip[0] | ip[1] | ip[2] | ip[3]) == 0u) return false;
    if ((ip[0] & ip[1] & ip[2] & ip[3]) == 0xFFu) return false;
    return true;
}

/* Validate two IPs are in same subnet */
static bool validate_same_subnet(const uint8_t ip1[4],
                                 const uint8_t ip2[4],
                                 const uint8_t mask[4])
{
    int i;
    for (i = 0; i < 4; i++) {
        if ((ip1[i] & mask[i]) != (ip2[i] & mask[i])) return false;
    }
    return true;
}

/* Validate complete configuration */
static bool validate_config(const ECM_IPConfig_t *cfg)
{
    uint8_t *all_ips[4];
    int i, j;

    if (cfg->record_version == 0u) return false;

    /* 1. Basic IP validity */
    if (!validate_ip_basic(cfg->host_ip))        return false;
    if (!validate_ip_basic(cfg->dev_ip))         return false;
    if (!validate_ip_basic(cfg->target_host_ip)) return false;
    if (!validate_ip_basic(cfg->target_dev_ip))  return false;

    /* 2. Subnet mask validity */
    if (!validate_subnet_mask(cfg->subnet_mask))  return false;

    /* 3. All IPs must be in same subnet (ECM bridge requirement) */
    if (!validate_same_subnet(cfg->host_ip, cfg->dev_ip, cfg->subnet_mask))
        return false;
    if (!validate_same_subnet(cfg->host_ip, cfg->target_host_ip, cfg->subnet_mask))
        return false;
    if (!validate_same_subnet(cfg->host_ip, cfg->target_dev_ip, cfg->subnet_mask))
        return false;

    /* 4. All IPs must be unique (avoid address conflicts) */
    all_ips[0] = (uint8_t *)cfg->host_ip;
    all_ips[1] = (uint8_t *)cfg->dev_ip;
    all_ips[2] = (uint8_t *)cfg->target_host_ip;
    all_ips[3] = (uint8_t *)cfg->target_dev_ip;
    for (i = 0; i < 4; i++) {
        for (j = i + 1; j < 4; j++) {
            if (memcmp(all_ips[i], all_ips[j], 4) == 0) return false;
        }
    }

    return true;
}

/* Read Flash page into buffer */
static bool flash_read_page(uint32_t addr, void *buf, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)addr;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t i;

    for (i = 0; i < len; i++) {
        dst[i] = src[i];
    }
    return true;
}

/* Erase Flash page */
static bool flash_erase_page(uint32_t addr)
{
    FLASH_Status status;

    FLASH_Unlock();
    status = FLASH_ErasePage(addr);
    FLASH_Lock();

    return (status == FLASH_COMPLETE);
}

/* Write data to Flash (word-by-word) */
static bool flash_write_page(uint32_t addr, const void *data, uint32_t len)
{
    const uint32_t *words = (const uint32_t *)data;
    uint32_t word_count = (len + 3) / 4;
    uint32_t i;
    FLASH_Status status;

    FLASH_Unlock();
    for (i = 0; i < word_count; i++) {
        status = FLASH_ProgramWord(addr + (i * 4), words[i]);
        if (status != FLASH_COMPLETE) {
            FLASH_Lock();
            return false;
        }
    }
    FLASH_Lock();
    return true;
}

/* Check if Flash page is valid */
static bool is_page_valid(const ECM_IPConfig_Flash_t *page)
{
    if (page->magic != ECM_IP_CONFIG_MAGIC) return false;
    if (page->footer_magic != ECM_IP_CONFIG_MAGIC) return false;
    if (page->schema_version != ECM_IP_CONFIG_VERSION) return false;
    if (calc_crc32(&page->config, sizeof(page->config)) != page->crc32) return false;
    return true;
}

void ECM_IPConfig_Init(void)
{
    /* Read flash and validate before using */
    const volatile ECM_IPConfig_Flash_t *page_a =
        (const volatile ECM_IPConfig_Flash_t *)ECM_IP_CONFIG_PAGE_A_ADDR;

    /* Check if flash data is valid */
    if (page_a->magic == ECM_IP_CONFIG_MAGIC &&
        page_a->footer_magic == ECM_IP_CONFIG_MAGIC &&
        page_a->schema_version == ECM_IP_CONFIG_VERSION &&
        calc_crc32((const void *)&page_a->config, sizeof(page_a->config)) == page_a->crc32) {
        /* Valid data in flash, use it */
        s_ram_cache = page_a->config;
    } else {
        /* Flash data invalid or uninitialized, use defaults */
        s_ram_cache = ECM_IP_CONFIG_DEFAULT;
    }
}

void ECM_IPConfig_Get(ECM_IPConfig_t *config)
{
    *config = s_ram_cache;
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

uint8_t ECM_IPConfig_Set(const ECM_IPConfig_t *config)
{
    ECM_IPConfig_Flash_t page_a, page_b;
    bool ok_a, ok_b;
    bool valid_a, valid_b;
    uint32_t next_count;
    uint32_t target_addr;
    ECM_IPConfig_Flash_t to_write;
    ECM_IPConfig_Flash_t verify;

    /* 1. Parameter validation */
    if (!validate_config(config)) {
        return ECM_IP_CONFIG_ERR_PARAM;
    }

    /* 2. Read both pages, decide target page */
    ok_a = flash_read_page(ECM_IP_CONFIG_PAGE_A_ADDR, &page_a, sizeof(page_a));
    ok_b = flash_read_page(ECM_IP_CONFIG_PAGE_B_ADDR, &page_b, sizeof(page_b));

    valid_a = ok_a && is_page_valid(&page_a);
    valid_b = ok_b && is_page_valid(&page_b);

    if (valid_a && valid_b) {
        /* Both valid, write to one with lower write_count (wear leveling) */
        if (page_a.write_count <= page_b.write_count) {
            target_addr = ECM_IP_CONFIG_PAGE_A_ADDR;
            next_count  = page_a.write_count + 1u;
        } else {
            target_addr = ECM_IP_CONFIG_PAGE_B_ADDR;
            next_count  = page_b.write_count + 1u;
        }
    } else if (valid_a) {
        target_addr = ECM_IP_CONFIG_PAGE_B_ADDR;
        next_count  = page_a.write_count + 1u;
    } else if (valid_b) {
        target_addr = ECM_IP_CONFIG_PAGE_A_ADDR;
        next_count  = page_b.write_count + 1u;
    } else {
        /* Both invalid, write to page A */
        target_addr = ECM_IP_CONFIG_PAGE_A_ADDR;
        next_count  = 1u;
    }

    /* 3. Assemble write structure */
    to_write.magic          = ECM_IP_CONFIG_MAGIC;
    to_write.schema_version = ECM_IP_CONFIG_VERSION;
    to_write.write_count    = next_count;
    to_write.config         = *config;
    to_write.crc32          = calc_crc32(&to_write.config, sizeof(to_write.config));
    to_write.footer_magic   = ECM_IP_CONFIG_MAGIC;

    /* 4. Erase target page */
    if (!flash_erase_page(target_addr)) {
        return ECM_IP_CONFIG_ERR_FLASH_ERASE;
    }

    /* 5. Program write */
    if (!flash_write_page(target_addr, &to_write, sizeof(to_write))) {
        return ECM_IP_CONFIG_ERR_FLASH_PROG;
    }

    /* 6. Read-back verification */
    if (!flash_read_page(target_addr, &verify, sizeof(verify))) {
        return ECM_IP_CONFIG_ERR_FLASH_VERIFY;
    }
    if (verify.crc32 != to_write.crc32
        || verify.footer_magic != ECM_IP_CONFIG_MAGIC
        || memcmp(&verify.config, &to_write.config, sizeof(ECM_IPConfig_t)) != 0) {
        return ECM_IP_CONFIG_ERR_FLASH_VERIFY;
    }

    /* 7. Update RAM cache */
    s_ram_cache = *config;

    /* 8. Immediately update bridge layer IP variables to make changes take effect */
    ECM_Bridge_UpdateIPConfig();

    /* 9. Sync target host IP to BLE SSH default target configuration */
    /* This ensures Bluetooth SSH connects to the correct target PC after IP changes */
    DefaultTargetConfig_Set(config->target_host_ip, BLE_ETH_DEFAULT_TARGET_PORT);

    return ECM_IP_CONFIG_OK;
}

uint8_t ECM_IPConfig_SetHostSide(const uint8_t host_ip[4],
                                  const uint8_t dev_ip[4],
                                  const uint8_t subnet[4])
{
    ECM_IPConfig_t new_config;
    memset(&new_config, 0, sizeof(new_config)); // Zero-init to avoid uninitialized padding
    memcpy(&new_config, &s_ram_cache, sizeof(s_ram_cache)); // Copy current config
    memcpy(new_config.host_ip, host_ip, 4);
    memcpy(new_config.dev_ip, dev_ip, 4);
    memcpy(new_config.subnet_mask, subnet, 4);
    // reserved and padding already zero from memset

    return ECM_IPConfig_Set(&new_config);
}

uint8_t ECM_IPConfig_SetTargetSide(const uint8_t target_host_ip[4],
                                   const uint8_t target_dev_ip[4])
{
    ECM_IPConfig_t new_config;
    memset(&new_config, 0, sizeof(new_config)); // Zero-init to avoid uninitialized padding
    memcpy(&new_config, &s_ram_cache, sizeof(s_ram_cache)); // Copy current config
    memcpy(new_config.target_host_ip, target_host_ip, 4);
    memcpy(new_config.target_dev_ip, target_dev_ip, 4);
    // reserved and padding already zero from memset

    return ECM_IPConfig_Set(&new_config);
}

uint8_t ECM_IPConfig_Reset(void)
{
    return ECM_IPConfig_Set(&ECM_IP_CONFIG_DEFAULT);
}

/* Validate config (check if parameters are valid before setting) */
bool ECM_IPConfig_Validate(const ECM_IPConfig_t *config)
{
    return validate_config(config);
}

/* Get current config from RAM cache (for building partial updates) */
void ECM_IPConfig_GetCurrent(ECM_IPConfig_t *config)
{
    *config = s_ram_cache;
}

#endif /* USB_WORK_MODE_COMPILES_BRIDGE_ECM */
