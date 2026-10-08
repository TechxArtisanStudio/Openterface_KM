# ECM IP Configuration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement runtime-configurable ECM bridge IP addresses via CH9329 protocol with dual-page Flash wear leveling, immediate reconfiguration, and full parameter validation.

**Architecture:** A new `ecm_ip_config` module stores 5 IP addresses in Flash using dual-page wear leveling with FNV-1a validation. CH9329 commands 0x26/0xA6 (Set/Set-ACK) and 0x27/0xA7 (Get/Get-ACK) read/write this config. On successful Set, `ECM_Bridge_Reconfigure()` reloads IPs into the ECM bridge's local arrays, applying changes immediately without reboot.

**Tech Stack:** C (gnu99), CH32V20x MCU, ch32v20x_flash.h HAL, FNV-1a hash for validation, MRS build system (GNU RISC-V toolchain)

**Spec:** `docs/superpowers/specs/2026-10-08-ecm-ip-config-design.md`

## Global Constraints

- Flash addresses: `0x0806C000` (page A), `0x0806B000` (page B) — 4KB each
- CRC algorithm: FNV-1a (offset basis 2166136261, prime 16777619)
- Conditional compilation: `#if USB_WORK_MODE_COMPILES_BRIDGE_ECM`
- All 5 IPs must be in the same subnet and mutually distinct
- Subnet mask must be contiguous bits (e.g., 255.255.255.0 valid, 255.0.255.0 invalid)
- IPs must not be 0.0.0.0 or 255.255.255.255
- Flash write operations use: FLASH_Unlock → FLASH_ErasePage → FLASH_ProgramWord loop → FLASH_Lock
- Flash read operations use: direct memory-mapped access via cast
- CH9329 status codes already defined: 0x00 (success), 0xE5 (param err), 0xE9 (erase fail), 0xEA (program fail), 0xEB (verify fail)
- CH9329 response: `CH9329_SendResponse(addr, cmd_code, pdata, len, resp_mode)` where resp_mode=1

## Review Focus

1. **Two-stage validation during partial updates:** Set Host Side (0x01) or Set Target Side (0x02) must read current config, apply partial update, then re-validate ALL four IPs — not just the two being changed. Test: set host side to a valid subnet, then set target side to a different subnet → should fail with 0xE5.
2. **Flash erase/program failure recovery:** If page A erase succeeds but program fails, page A is blank and page B still holds valid data. Next Init must correctly select page B. Test: simulate program failure, verify Init recovers.
3. **Reconfigure on Set success only:** ECM_Bridge_Reconfigure must only be called after successful Set (not on validation failure or Flash errors). Test: send invalid config, verify bridge IPs unchanged.
4. **Init before Bridge_Init:** Since ECM_Bridge_Init runs in USB bus reset interrupt, ECM_IPConfig_Init must complete before USB is initialized in main(). Test: verify Init is called before Set_USBConfig().
5. **`write_count` overflow:** `write_count` is a uint32_t that increments on each write. At typical usage (occasional config changes), overflow is not a concern. If it wraps to 0, Init's "select larger write_count" logic treats 0 as smaller than a non-zero value, which is correct — the older page with the real count wins.

---

### Task 1: Create ecm_ip_config module (header + implementation)

**Files:**
- Create: `APP/include/ecm_ip_config.h`
- Create: `APP/ecm_ip_config.c`

**Interfaces:**
- Produces:
  - `void ECM_IPConfig_Init(void)`
  - `void ECM_IPConfig_Get(ECM_IPConfig_t *config)`
  - `void ECM_IPConfig_GetHostIP(uint8_t ip[4])`
  - `void ECM_IPConfig_GetDevIP(uint8_t ip[4])`
  - `void ECM_IPConfig_GetSubnetMask(uint8_t mask[4])`
  - `void ECM_IPConfig_GetTargetHostIP(uint8_t ip[4])`
  - `void ECM_IPConfig_GetTargetDevIP(uint8_t ip[4])`
  - `ECM_IPConfig_Status_t ECM_IPConfig_Set(const ECM_IPConfig_t *config)`
  - `ECM_IPConfig_Status_t ECM_IPConfig_SetHostSide(const uint8_t host_ip[4], const uint8_t dev_ip[4], const uint8_t subnet[4])`
  - `ECM_IPConfig_Status_t ECM_IPConfig_SetTargetSide(const uint8_t target_host_ip[4], const uint8_t target_dev_ip[4])`
  - `ECM_IPConfig_Status_t ECM_IPConfig_Reset(void)`
  - `ECM_IPConfig_t` struct type
  - `ECM_IPConfig_Status_t` enum type (OK=0, ERR_PARAM, ERR_FLASH_ERASE, ERR_FLASH_PROGRAM, ERR_FLASH_VERIFY)
  - `ECM_IP_CONFIG_VERSION` constant (1u)

- [ ] **Step 1: Write the header file `APP/include/ecm_ip_config.h`**

```c
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
```

- [ ] **Step 2: Write the implementation file `APP/ecm_ip_config.c`**

```c
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
```

- [ ] **Step 3: Compile to verify**

Run: verify the build system picks up the new file. Since the makefile is not yet updated, do a manual syntax check:
```bash
gcc -fsyntax-only -std=gnu99 -I./APP/include -DCH32V20x_D8W -DUSB_WORK_MODE_COMPILES_BRIDGE_ECM APP/ecm_ip_config.c 2>&1
```

Expected: no syntax errors. Note that `ch32v20x_flash.h` and `debug.h` symbols may be unresolved (headers missing on host), but C syntax should be valid.

- [ ] **Step 4: Commit**

```bash
git add APP/include/ecm_ip_config.h APP/ecm_ip_config.c
git commit -m "feat: add ECM IP config module with dual-page Flash wear leveling

Core module for runtime-configurable ECM bridge IP addresses.
Implements FNV-1a validation, parameter checking (subnet mask
continuity, IP uniqueness, same-subnet constraint), and dual-page
wear leveling with read-back verification.

Uses flash pages at 0x0806C000 (A) and 0x0806B000 (B).

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 2: Add command definitions to usbd_composite_km.h

**Files:**
- Modify: `APP/include/usbd_composite_km.h` (add after CMD_USB_WORK_MODE line)

**Interfaces:**
- Consumes: (none — no compile dependencies, just #define constants)
- Produces:
  - `#define CMD_SET_ECM_IP_CONFIG  0x26`
  - `#define CMD_GET_ECM_IP_CONFIG  0x27`

- [ ] **Step 1: Add command code definitions**

In `APP/include/usbd_composite_km.h`, after the `CMD_USB_WORK_MODE` definition (line 55), add:

```c
#define CMD_SET_ECM_IP_CONFIG      0x26  /* Set ECM bridge IP configuration (persisted to Flash) */
#define CMD_GET_ECM_IP_CONFIG      0x27  /* Get ECM bridge IP configuration (from RAM cache) */
```

- [ ] **Step 2: Verify no conflicts**

Run:
```bash
grep -n "0x26\|0x27\|0xA6\|0xA7" APP/include/usbd_composite_km.h
```

Expected: only the two new lines appear (and the ACK codes are generated by adding 0x80 to the command codes: 0x26+0x80=0xA6, 0x27+0x80=0xA7 — these are computed at runtime by the existing CH9329 response handler, no #define needed).

- [ ] **Step 3: Commit**

```bash
git add APP/include/usbd_composite_km.h
git commit -m "feat: add CH9329 command codes for ECM IP config (0x26/0x27)

CMD_SET_ECM_IP_CONFIG (0x26): Set ECM bridge IP configuration
CMD_GET_ECM_IP_CONFIG (0x27): Get ECM bridge IP configuration

ACK codes (0xA6/0xA7) derived automatically via +0x80 in response handler.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 3: Add command handlers to usbd_compostie_km.c

**Files:**
- Modify: `APP/usbd_compostie_km.c` (add case blocks + handler functions)

**Interfaces:**
- Consumes:
  - `#include "include/ecm_ip_config.h"` (for ECM_IPConfig_t, API functions)
  - `#include "include/ecm_bridge.h"` (for ECM_Bridge_Reconfigure)
  - `CMD_SET_ECM_IP_CONFIG` (0x26) and `CMD_GET_ECM_IP_CONFIG` (0x27) from Task 2
  - `CH9329_SendResponse(addr, cmd_code, pdata, len, resp_mode)` — existing
  - `STATUS_SUCCESS` (0x00), `STATUS_ERR_PARAM` (0xE5), `STATUS_ERR_FLASH_ERASE` (0xE9), `STATUS_ERR_FLASH_PROGRAM` (0xEA), `STATUS_ERR_FLASH_VERIFY` (0xEB) — existing defines
- Produces: (none — modifies existing switch statement, no new public API)

- [ ] **Step 1: Add includes at top of file**

After the existing `#include "include/usb_mode_config.h"` line, add:

```c
#include "include/ecm_ip_config.h"
```

- [ ] **Step 2: Add CMD_SET_ECM_IP_CONFIG case block**

Find the `CMD_SYSTEM_RESET` case block in `CH9329_DataParser` (around line 382). Add after that case (and before any other `case` in the switch).

Note: `CH9329_SendResponse` internally ORs `cmd_code` with 0x80 when `resp_mode=1` (line 104: `cmd_code | (resp_mode ? 0x80 : 0xC0)`), so we pass the original command code — NOT `cmd_code | 0x80`. The function generates 0xA6 from 0x26 automatically.

```c
#if USB_WORK_MODE_COMPILES_BRIDGE_ECM
        case CMD_SET_ECM_IP_CONFIG:
        {
            uint8_t status = STATUS_SUCCESS;
            uint8_t sub_cmd;
            ECM_IPConfig_Status_t cfg_status = ECM_IP_CONFIG_OK;

            if (data_len < 1) {
                status = STATUS_ERR_PARAM;
            } else {
                sub_cmd = data[0];

                switch (sub_cmd) {
                case 0x00: /* Set All: 20 bytes */
                    if (data_len == 21) {
                        ECM_IPConfig_t cfg;
                        cfg.version = ECM_IP_CONFIG_VERSION;
                        memcpy(cfg.host_ip,        &data[1],  4);
                        memcpy(cfg.dev_ip,         &data[5],  4);
                        memcpy(cfg.subnet_mask,    &data[9],  4);
                        memcpy(cfg.target_host_ip, &data[13], 4);
                        memcpy(cfg.target_dev_ip,  &data[17], 4);
                        cfg.reserved[0] = 0;
                        cfg.reserved[1] = 0;
                        cfg.padding = 0;
                        cfg_status = ECM_IPConfig_Set(&cfg);
                    } else {
                        status = STATUS_ERR_PARAM;
                    }
                    break;

                case 0x01: /* Set Host Side: 12 bytes */
                    if (data_len == 13) {
                        cfg_status = ECM_IPConfig_SetHostSide(&data[1], &data[5], &data[9]);
                    } else {
                        status = STATUS_ERR_PARAM;
                    }
                    break;

                case 0x02: /* Set Target Side: 8 bytes */
                    if (data_len == 9) {
                        cfg_status = ECM_IPConfig_SetTargetSide(&data[1], &data[5]);
                    } else {
                        status = STATUS_ERR_PARAM;
                    }
                    break;

                case 0x03: /* Reset to Default */
                    cfg_status = ECM_IPConfig_Reset();
                    break;

                default:
                    status = STATUS_ERR_PARAM;
                    break;
                }
            }

            /* Map ECM_IPConfig_Status_t to CH9329 status codes */
            if (status == STATUS_SUCCESS) {
                switch (cfg_status) {
                case ECM_IP_CONFIG_OK:                status = STATUS_SUCCESS;          break;
                case ECM_IP_CONFIG_ERR_PARAM:         status = STATUS_ERR_PARAM;        break;
                case ECM_IP_CONFIG_ERR_FLASH_ERASE:   status = STATUS_ERR_FLASH_ERASE;  break;
                case ECM_IP_CONFIG_ERR_FLASH_PROGRAM: status = STATUS_ERR_FLASH_PROGRAM;break;
                case ECM_IP_CONFIG_ERR_FLASH_VERIFY:  status = STATUS_ERR_FLASH_VERIFY; break;
                default:                              status = STATUS_ERR_FLASH_VERIFY; break;
                }
            }

            CH9329_SendResponse(addr, CMD_SET_ECM_IP_CONFIG, &status, 1, 1);

            /* Immediately reconfigure ECM bridge on success */
            if (status == STATUS_SUCCESS) {
                ECM_Bridge_Reconfigure();
            }
        }
        break;

        case CMD_GET_ECM_IP_CONFIG:
        {
            uint8_t sub_cmd;
            uint8_t status = STATUS_SUCCESS;
            uint8_t response[21]; /* max: 20 data + 1 status */

            if (data_len < 1) {
                status = STATUS_ERR_PARAM;
                CH9329_SendResponse(addr, CMD_GET_ECM_IP_CONFIG, &status, 1, 1);
            } else {
                sub_cmd = data[0];
                ECM_IPConfig_t cfg;
                ECM_IPConfig_Get(&cfg);

                switch (sub_cmd) {
                case 0x00: /* Get All: 20 bytes */
                    memcpy(&response[0], cfg.host_ip, 4);
                    memcpy(&response[4], cfg.dev_ip, 4);
                    memcpy(&response[8], cfg.subnet_mask, 4);
                    memcpy(&response[12], cfg.target_host_ip, 4);
                    memcpy(&response[16], cfg.target_dev_ip, 4);
                    CH9329_SendResponse(addr, CMD_GET_ECM_IP_CONFIG, response, 20, 1);
                    break;

                case 0x01: /* Get Host Side: 12 bytes */
                    memcpy(&response[0], cfg.host_ip, 4);
                    memcpy(&response[4], cfg.dev_ip, 4);
                    memcpy(&response[8], cfg.subnet_mask, 4);
                    CH9329_SendResponse(addr, CMD_GET_ECM_IP_CONFIG, response, 12, 1);
                    break;

                case 0x02: /* Get Target Side: 8 bytes */
                    memcpy(&response[0], cfg.target_host_ip, 4);
                    memcpy(&response[4], cfg.target_dev_ip, 4);
                    CH9329_SendResponse(addr, CMD_GET_ECM_IP_CONFIG, response, 8, 1);
                    break;

                default:
                    status = STATUS_ERR_PARAM;
                    CH9329_SendResponse(addr, CMD_GET_ECM_IP_CONFIG, &status, 1, 1);
                    break;
                }
            }
        }
        break;
#endif /* USB_WORK_MODE_COMPILES_BRIDGE_ECM */
```

- [ ] **Step 3: Verify code compiles**

Check that the existing import structure supports `ECM_IPConfig_Get`. The `#ifdef USB_WORK_MODE_COMPILES_BRIDGE_ECM` guard ensures this code only compiles when the ecm_ip_config module is also compiled (which is guarded by the same macro).

- [ ] **Step 4: Commit**

```bash
git add APP/usbd_compostie_km.c
git commit -m "feat: add CH9329 command handlers for ECM IP config

Adds CMD_SET_ECM_IP_CONFIG (0x26) and CMD_GET_ECM_IP_CONFIG (0x27)
handlers in CH9329_DataParser. Supports sub-commands:
- Set All / Set Host Side / Set Target Side / Reset to Default
- Get All / Get Host Side / Get Target Side

Set commands map ECM_IPConfig_Set failures to CH9329 status codes
(0xE5 param, 0xE9 flash erase, 0xEA flash program, 0xEB verify).
On successful set, calls ECM_Bridge_Reconfigure() for immediate effect.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 4: Modify ecm_bridge (header + implementation) for configurable IPs

**Files:**
- Modify: `APP/include/ecm_bridge.h` (add Reconfigure declaration)
- Modify: `APP/ecm_bridge.c` (replace hardcoded IPs, add Reconfigure function)

**Interfaces:**
- Consumes:
  - `#include "include/ecm_ip_config.h"` (for getter functions)
  - `ECM_IPConfig_GetHostIP`, `ECM_IPConfig_GetDevIP`, `ECM_IPConfig_GetSubnetMask`, `ECM_IPConfig_GetTargetHostIP`, `ECM_IPConfig_GetTargetDevIP` from Task 1
- Produces:
  - `void ECM_Bridge_Reconfigure(void)` — called by Task 3 command handler

- [ ] **Step 1: Modify ecm_bridge.h — add Reconfigure declaration**

In `APP/include/ecm_bridge.h`, inside the `#if USB_WORK_MODE_COMPILES_BRIDGE_ECM` block (after the `ECM_Bridge_SendRawFrame` declaration), add:

```c
void ECM_Bridge_Reconfigure(void);  /* reload IPs from config module, no reboot needed */
```

Also add the corresponding macro stub in the `#else` block:

```c
#define ECM_Bridge_Reconfigure()           ((void)0)
```

- [ ] **Step 2: Replace hardcoded IPs in ecm_bridge.c — add include**

At the top of `APP/ecm_bridge.c`, add after the existing includes:

```c
#include "include/ecm_ip_config.h"
```

- [ ] **Step 3: Replace hardcoded IP constants with mutable arrays**

In `APP/ecm_bridge.c`, find the hardcoded constants (around lines 68-80):

```c
static const uint8_t ECM_HOST_IP[4]     = { 192, 168, 11, 1 };
static const uint8_t ECM_SUBNET_MASK[4] = { 255, 255, 255, 0 };
// ... ECM_DEV_IP, ECM_TARGET_HOST_IP, ECM_TARGET_DEV_IP
```

Replace with:

```c
/* Runtime IP configuration — loaded from ECM_IPConfig module */
static uint8_t ecm_host_ip[4];
static uint8_t ecm_subnet_mask[4];
static uint8_t ecm_dev_ip[4];
static uint8_t ecm_target_host_ip[4];
static uint8_t ecm_target_dev_ip[4];

/* ECM_SUBNET_MASK renamed to ecm_subnet_mask — update all references below */
```

- [ ] **Step 4: Add ECM_Bridge_Reconfigure() function**

Add to `APP/ecm_bridge.c`, right before `ECM_Bridge_Init`:

```c
/**
 * @brief Reload IP configuration from the ECM_IPConfig module.
 *
 * Reads current config from RAM cache (no Flash access) and updates
 * the local IP arrays used by the bridge. Called on successful
 * CMD_SET_ECM_IP_CONFIG to apply changes without reboot.
 */
void ECM_Bridge_Reconfigure(void)
{
    ECM_IPConfig_GetHostIP(ecm_host_ip);
    ECM_IPConfig_GetDevIP(ecm_dev_ip);
    ECM_IPConfig_GetSubnetMask(ecm_subnet_mask);
    ECM_IPConfig_GetTargetHostIP(ecm_target_host_ip);
    ECM_IPConfig_GetTargetDevIP(ecm_target_dev_ip);

    LOG_INFO("ECM bridge: IPs reconfigured\r\n");
}
```

- [ ] **Step 5: Modify ECM_Bridge_Init to call Reconfigure**

Find `ECM_Bridge_Init` in `APP/ecm_bridge.c` (around line 1639). At the top of the function body, add:

```c
    /* Load IP configuration (ECM_IPConfig_Init already called in main) */
    ECM_Bridge_Reconfigure();
```

- [ ] **Step 6: Update all references to the renamed variables**

The old variable names (`ECM_HOST_IP`, `ECM_DEV_IP`, `ECM_SUBNET_MASK`, `ECM_TARGET_HOST_IP`, `ECM_TARGET_DEV_IP`) need global rename to the new names (`ecm_host_ip`, `ecm_dev_ip`, `ecm_subnet_mask`, `ecm_target_host_ip`, `ecm_target_dev_ip`).

The old variables were `static const` arrays; the new variables are `static` (non-const) arrays. The renaming is mechanical — same name but lowercase prefix, `ECM_` → `ecm_`.

Run to find all references:
```bash
grep -n "ECM_HOST_IP\|ECM_DEV_IP\|ECM_SUBNET_MASK\|ECM_TARGET_HOST_IP\|ECM_TARGET_DEV_IP" APP/ecm_bridge.c
```

Replace in order:
- `ECM_HOST_IP` → `ecm_host_ip`
- `ECM_DEV_IP` → `ecm_dev_ip`
- `ECM_SUBNET_MASK` → `ecm_subnet_mask`
- `ECM_TARGET_HOST_IP` → `ecm_target_host_ip`
- `ECM_TARGET_DEV_IP` → `ecm_target_dev_ip`

- [ ] **Step 7: Commit**

```bash
git add APP/include/ecm_bridge.h APP/ecm_bridge.c
git commit -m "feat: make ECM bridge IPs runtime-configurable

Replace hardcoded IP constants with mutable arrays populated from
the ECM_IPConfig module. Add ECM_Bridge_Reconfigure() to reload
IPs without reboot, called on successful CMD_SET_ECM_IP_CONFIG.

ECM_Bridge_Init now calls ECM_Bridge_Reconfigure() to load IPs
from the config module's RAM cache.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 5: Add ECM_IPConfig_Init() to peripheral_main.c

**Files:**
- Modify: `APP/peripheral_main.c` (add Init call in main())

**Interfaces:**
- Consumes:
  - `#include "include/ecm_ip_config.h"` (for ECM_IPConfig_Init)
  - `ECM_IPConfig_Init()` from Task 1

- [ ] **Step 1: Add include**

After the existing `#include "include/default_target_config.h"` line, add:

```c
#include "include/ecm_ip_config.h"
```

- [ ] **Step 2: Add Init call in main()**

In `int main(void)`, after `DefaultTargetConfig_Init();` (around line 230), add:

```c
#if USB_WORK_MODE_COMPILES_BRIDGE_ECM
    ECM_IPConfig_Init();
#endif
```

This must be called BEFORE `Set_USBConfig()`, `USB_Init()`, etc., because `ECM_Bridge_Init` runs in the USB bus reset interrupt and needs the config ready.

- [ ] **Step 3: Commit**

```bash
git add APP/peripheral_main.c
git commit -m "feat: initialize ECM IP config before USB in peripheral_main

Call ECM_IPConfig_Init() after DefaultTargetConfig_Init() but before
USB initialization, so ECM_Bridge_Init() in the USB bus reset interrupt
has the IP config ready.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 6: Update build files

**Files:**
- Modify: `build/APP/subdir.mk`

**Interfaces:**
- Consumes: `APP/ecm_ip_config.c` and `APP/include/ecm_ip_config.h` from Task 1

- [ ] **Step 1: Add ecm_ip_config.c to build sources in subdir.mk**

In `build/APP/subdir.mk`, add `ecm_ip_config.c` to all three lists (C_SRCS, C_DEPS, OBJS) in alphabetical order.

Add to `C_SRCS` (after `ecm_bridge.c`):
```
../APP/ecm_ip_config.c \
```

Add to `C_DEPS` (after `ecm_bridge.d`):
```
./APP/ecm_ip_config.d \
```

Add to `OBJS` (after `ecm_bridge.o`):
```
./APP/ecm_ip_config.o \
```

- [ ] **Step 2: Verify build file consistency**

Run:
```bash
grep -c "ecm_ip_config" build/APP/subdir.mk
```
Expected: 3 (one in C_SRCS, one in C_DEPS, one in OBJS)

- [ ] **Step 3: Commit**

```bash
git add build/APP/subdir.mk
git commit -m "build: add ecm_ip_config.c to build sources

New ECM IP config module added to APP build sources.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 7: Update protocol documentation

**Files:**
- Modify: `docs/ch9329-communication-protocol.md` (append new commands section)

**Interfaces:**
- (Documentation only — no code dependencies)

- [ ] **Step 1: Check if protocol doc exists**

```bash
ls -la docs/ch9329-communication-protocol.md 2>&1 || echo "FILE NOT FOUND"
```

- [ ] **Step 2: Append command documentation**

Add the new command documentation to the protocol doc. If the file exists, append after existing content. If it doesn't exist, create it with the full protocol description.

```markdown
## 0x26 — Set ECM IP Configuration

Set the ECM bridge's host-side and target-side IP addresses. Changes are
persisted to Flash and take effect immediately (no reboot required).

**Request:**

```
57 AB <addr> 26 <len> <sub_cmd> <data...> <checksum>
```

**Sub-commands:**

| sub_cmd | Name | Data | Description |
|---------|------|------|-------------|
| 0x00 | Set All | host_ip(4) + dev_ip(4) + subnet(4) + target_host_ip(4) + target_dev_ip(4) = 20B | Set all IP config |
| 0x01 | Set Host Side | host_ip(4) + dev_ip(4) + subnet(4) = 12B | Set host-side config only |
| 0x02 | Set Target Side | target_host_ip(4) + target_dev_ip(4) = 8B | Set target-side config only |
| 0x03 | Reset to Default | (none) | Reset to 192.168.11.x defaults |

**Response:**

```
57 AB <addr> A6 01 <status> <checksum>
```

**Status codes:**
- `0x00`: Success (config persisted, bridge reconfigured)
- `0xE5`: Parameter error (wrong length, invalid IP, invalid subnet mask,
          IPs not in same subnet, duplicate IPs)
- `0xE9`: Flash erase failed
- `0xEA`: Flash program failed
- `0xEB`: Flash verify failed (read-back mismatch)

**Validation rules:**
- All IPs must be in the same subnet
- IPs must not be 0.0.0.0 or 255.255.255.255
- Subnet mask must be contiguous (e.g., 255.255.255.0 valid, 255.0.255.0 invalid)
- All four IPs must be mutually distinct

**Example — Set all to 10.0.0.x subnet:**
```
Request:  57 AB 00 26 15 00 0A 00 00 01 0A 00 00 03 FF FF FF 00 0A 00 00 02 0A 00 00 05 <sum>
Response: 57 AB 00 A6 01 00 <sum>
```

## 0x27 — Get ECM IP Configuration

Read the current ECM bridge IP configuration from RAM cache.

**Request:**

```
57 AB <addr> 27 01 <sub_cmd> <checksum>
```

**Sub-commands:**

| sub_cmd | Name | Response Data |
|---------|------|---------------|
| 0x00 | Get All | host_ip(4) + dev_ip(4) + subnet(4) + target_host_ip(4) + target_dev_ip(4) = 20B |
| 0x01 | Get Host Side | host_ip(4) + dev_ip(4) + subnet(4) = 12B |
| 0x02 | Get Target Side | target_host_ip(4) + target_dev_ip(4) = 8B |

**Response:**

```
57 AB <addr> A7 <len> <data...> <checksum>
```

**Example — Get all config:**
```
Request:  57 AB 00 27 01 00 <sum>
Response: 57 AB 00 A7 14 C0 A8 0B 01 C0 A8 0B 03 FF FF FF 00 C0 A8 0B 02 C0 A8 0B 05 <sum>
```

**Note:** Get reads from RAM cache, no Flash access — response time <1ms.
```

- [ ] **Step 3: Commit**

```bash
git add docs/ch9329-communication-protocol.md
git commit -m "docs: add ECM IP config commands to CH9329 protocol docs

Documents 0x26 (Set ECM IP Config) and 0x27 (Get ECM IP Config)
commands with sub-commands, status codes, validation rules,
and packet format examples.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```