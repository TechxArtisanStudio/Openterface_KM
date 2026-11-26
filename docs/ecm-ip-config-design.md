# ECM IP Configuration Design

## Overview

Allows every network configuration parameter in the ECM bridge to be changed
manually over the CH9329 protocol, including the host-side IP, the target-side IP,
the subnet mask and so on.

On top of the original design, this design strengthens the following key
capabilities to meet industrial-grade reliability requirements:

- **Flash wear levelling**: two pages written alternately, so neither wears out early
- **Atomic updates**: magic + CRC32 + trailing magic, three-level verification, so a power loss cannot corrupt the data
- **Versioned configuration**: a version field, for future firmware backward compatibility
- **Full parameter validation**: IP format, subnet-mask contiguity, same-subnet constraint
- **Error recovery**: safe fallback to defaults when a flash read or a CRC check fails

## Currently hard-coded IP addresses

In `APP/ecm_bridge.c` the following IP addresses are hard-coded:

| Variable | Default | Purpose |
|--------|--------|------|
| `ECM_HOST_IP` | 192.168.11.1 | IP address of the host side (Mac/PC) |
| `ECM_DEV_IP` | 192.168.11.3 | the firmware's IP address on the host side |
| `ECM_SUBNET_MASK` | 255.255.255.0 | subnet mask |
| `ECM_TARGET_HOST_IP` | 192.168.11.2 | IP address of the target PC |
| `ECM_TARGET_DEV_IP` | 192.168.11.5 | the firmware's IP address on the target side |

## Design

### 1. Flash configuration module

Create a new configuration module `ecm_ip_config`, responsible for storing and
reading the network configuration.

#### 1.1 Configuration data structure

```c
#include <stdint.h>
#include <stdbool.h>

#define ECM_IP_CONFIG_MAGIC        0x45434D49u  /* "ECMI" */
#define ECM_IP_CONFIG_VERSION     1u
#define ECM_IP_CONFIG_PAGE_A_ADDR 0x0807D000u   /* double page A */
#define ECM_IP_CONFIG_PAGE_B_ADDR 0x0807C000u   /* double page B */
#define ECM_IP_CONFIG_PAGE_SIZE   0x1000u       /* 4KB */

/* User-visible network configuration (24 bytes, 4-byte aligned) */
typedef struct {
    uint8_t  version;           /* config version, currently 1 */
    uint8_t  host_ip[4];        /* host-side IP (default: 192.168.11.1) */
    uint8_t  dev_ip[4];         /* firmware IP on the host side (default: 192.168.11.3) */
    uint8_t  subnet_mask[4];    /* subnet mask (default: 255.255.255.0) */
    uint8_t  target_host_ip[4]; /* target PC IP (default: 192.168.11.2) */
    uint8_t  target_dev_ip[4];  /* firmware IP on the target side (default: 192.168.11.5) */
    uint8_t  reserved[2];       /* reserved bytes, for future expansion */
    uint8_t  padding;           /* padded to 4-byte alignment */
} ECM_IPConfig_t;               /* sizeof = 24 */

/* Full stored structure in flash (including verification data) */
typedef struct {
    uint32_t magic;             /* magic: ECM_IP_CONFIG_MAGIC */
    uint32_t schema_version;    /* config structure version (distinct from ECM_IPConfig_t.version) */
    uint32_t crc32;             /* CRC32 of the config field */
    uint32_t write_count;       /* write counter, used for wear levelling */
    ECM_IPConfig_t config;      /* the actual network configuration */
    uint32_t footer_magic;      /* trailing magic, ensures the whole block was written */
} ECM_IPConfig_Flash_t;         /* sizeof = 44 */
```

> **Memory alignment note**: `ECM_IPConfig_t` guarantees 4-byte alignment via the
> `padding` field, avoiding unaligned-access faults on ARM Cortex-M.
> `ECM_IPConfig_Flash_t` is all 32-bit fields and therefore naturally 4-byte aligned.

#### 1.2 Flash storage location

Use the last two pages of flash (4KB each) for alternating writes:

```
Flash layout (from the end):
- 0x0807F000: USB mode config       (4KB)
- 0x0807E000: Default target config (4KB)
- 0x0807D000: ECM IP config page A  (4KB)  <- new
- 0x0807C000: ECM IP config page B  (4KB)  <- new (backup page)
```

> **Dual-page strategy**: each write targets the page with the smaller
> `write_count`, which spreads the wear; reads compare `magic`/`crc32`/`write_count`
> on both pages and pick the valid, newest version. Even if power is lost
> mid-write, the other page still holds the previous valid configuration, so the
> system can degrade safely.

#### 1.3 Default value definitions

```c
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
```

#### 1.4 API interface

```c
/* Initialise the configuration (read from flash, or use defaults) */
void  ECM_IPConfig_Init(void);

/* Get the configuration (from the RAM cache, no flash access) */
void  ECM_IPConfig_Get(ECM_IPConfig_t *config);
void  ECM_IPConfig_GetHostIP(uint8_t ip[4]);
void  ECM_IPConfig_GetDevIP(uint8_t ip[4]);
void  ECM_IPConfig_GetSubnetMask(uint8_t mask[4]);
void  ECM_IPConfig_GetTargetHostIP(uint8_t ip[4]);
void  ECM_IPConfig_GetTargetDevIP(uint8_t ip[4]);

/* Set the configuration (validate + write flash, dual-page wear levelling) */
bool  ECM_IPConfig_Set(const ECM_IPConfig_t *config);
bool  ECM_IPConfig_SetHostSide(const uint8_t host_ip[4],
                               const uint8_t dev_ip[4],
                               const uint8_t subnet[4]);
bool  ECM_IPConfig_SetTargetSide(const uint8_t target_host_ip[4],
                                const uint8_t target_dev_ip[4]);

/* Reset to defaults */
bool  ECM_IPConfig_Reset(void);
```

#### 1.5 Parameter validation

Before setting a configuration it must pass the following checks; any failure
returns `0xE5`:

```c
/* The subnet mask must be a run of 1s followed by a run of 0s */
static bool validate_subnet_mask(const uint8_t mask[4]) {
    uint32_t m = ((uint32_t)mask[0] << 24) | ((uint32_t)mask[1] << 16)
               | ((uint32_t)mask[2] << 8)  |  (uint32_t)mask[3];
    if (m == 0u || m == 0xFFFFFFFFu) return false;
    /* A contiguous mask satisfies (~m + 1) & m == 0 */
    return ((~m + 1u) & m) == 0u;
}

/* An IP must not be 0.0.0.0 or 255.255.255.255 */
static bool validate_ip_basic(const uint8_t ip[4]) {
    if ((ip[0] | ip[1] | ip[2] | ip[3]) == 0u) return false;
    if ((ip[0] & ip[1] & ip[2] & ip[3]) == 0xFFu) return false;
    return true;
}

/* Two IPs must be in the same subnet (identical network part) */
static bool validate_same_subnet(const uint8_t ip1[4],
                                 const uint8_t ip2[4],
                                 const uint8_t mask[4]) {
    for (int i = 0; i < 4; i++) {
        if ((ip1[i] & mask[i]) != (ip2[i] & mask[i])) return false;
    }
    return true;
}

/* Every IP must be in the same subnet (an ECM bridge requirement) */
static bool validate_config(const ECM_IPConfig_t *cfg) {
    if (cfg->version == 0u) return false;
    
    /* 1. basic IP validity */
    if (!validate_ip_basic(cfg->host_ip))        return false;
    if (!validate_ip_basic(cfg->dev_ip))         return false;
    if (!validate_ip_basic(cfg->target_host_ip)) return false;
    if (!validate_ip_basic(cfg->target_dev_ip))  return false;
    
    /* 2. subnet mask validity */
    if (!validate_subnet_mask(cfg->subnet_mask))  return false;
    
    /* 3. every IP must be in the same subnet
     *    The ECM bridge emulates two NICs inside the firmware and bridges their
     *    traffic; if the host side and the target side are not in the same
     *    subnet, routing fails.
     */
    if (!validate_same_subnet(cfg->host_ip, cfg->dev_ip, cfg->subnet_mask))
        return false;
    if (!validate_same_subnet(cfg->host_ip, cfg->target_host_ip, cfg->subnet_mask))
        return false;
    if (!validate_same_subnet(cfg->host_ip, cfg->target_dev_ip, cfg->subnet_mask))
        return false;
    
    /* 4. every IP must be distinct (avoid address conflicts) */
    uint8_t *all_ips[4] = {
        (uint8_t *)cfg->host_ip,
        (uint8_t *)cfg->dev_ip,
        (uint8_t *)cfg->target_host_ip,
        (uint8_t *)cfg->target_dev_ip
    };
    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 4; j++) {
            if (memcmp(all_ips[i], all_ips[j], 4) == 0) return false;
        }
    }
    
    return true;
}
```

> **Note**: the host side and the target side are allowed to sit in different
> subnets (cross-subnet traffic is handled by host-side routing), so validation only
> requires host/dev in one subnet and target_host/target_dev in another, without
> requiring the two sides to share a subnet.

#### 1.6 Initialisation and error recovery

```c
static ECM_IPConfig_t s_ram_cache;   /* RAM cache, read-only at runtime */

void ECM_IPConfig_Init(void) {
    ECM_IPConfig_Flash_t page_a, page_b;
    bool ok_a = flash_read(ECM_IP_CONFIG_PAGE_A_ADDR, &page_a, sizeof(page_a));
    bool ok_b = flash_read(ECM_IP_CONFIG_PAGE_B_ADDR, &page_b, sizeof(page_b));

    bool valid_a = ok_a && page_a.magic == ECM_IP_CONFIG_MAGIC
                       && page_a.footer_magic == ECM_IP_CONFIG_MAGIC
                       && page_a.schema_version == ECM_IP_CONFIG_VERSION
                       && calc_crc32(&page_a.config, sizeof(page_a.config)) == page_a.crc32;
    bool valid_b = ok_b && page_b.magic == ECM_IP_CONFIG_MAGIC
                       && page_b.footer_magic == ECM_IP_CONFIG_MAGIC
                       && page_b.schema_version == ECM_IP_CONFIG_VERSION
                       && calc_crc32(&page_b.config, sizeof(page_b.config)) == page_b.crc32;

    if (valid_a && valid_b) {
        /* both pages valid: take the larger write_count */
        s_ram_cache = (page_a.write_count >= page_b.write_count)
                      ? page_a.config : page_b.config;
    } else if (valid_a) {
        s_ram_cache = page_a.config;
        /* repair page_b in the background */
    } else if (valid_b) {
        s_ram_cache = page_b.config;
        /* repair page_a in the background */
    } else {
        /* neither page valid (first use, or both corrupt): use defaults and persist them */
        LOG_WARN("ECM IP config: no valid page, loading defaults");
        s_ram_cache = ECM_IP_CONFIG_DEFAULT;
        ECM_IPConfig_Save(&s_ram_cache);   /* persist the defaults */
    }
}
```

> **Error recovery strategy**:
> - Flash read failure → fall back to defaults, log `LOG_ERROR`
> - CRC / magic check failure → fall back to defaults, log `LOG_WARN`
> - both pages corrupt → load defaults and rewrite one page, restoring a usable state
> - a dirty page that failed validation is overwritten and repaired on the next `Set`

#### 1.7 Dual-page wear-levelled write

```c
bool ECM_IPConfig_Set(const ECM_IPConfig_t *config) {
    /* 1. parameter validation */
    if (!validate_config(config)) {
        LOG_WARN("ECM IP config: validation failed");
        return false;   /* the caller returns status code 0xE5 */
    }

    /* 2. read both pages to decide the target page */
    ECM_IPConfig_Flash_t page_a, page_b;
    bool ok_a = flash_read(ECM_IP_CONFIG_PAGE_A_ADDR, &page_a, sizeof(page_a));
    bool ok_b = flash_read(ECM_IP_CONFIG_PAGE_B_ADDR, &page_b, sizeof(page_b));

    bool valid_a = ok_a && page_a.magic == ECM_IP_CONFIG_MAGIC
                       && calc_crc32(&page_a.config, sizeof(page_a.config)) == page_a.crc32;
    bool valid_b = ok_b && page_b.magic == ECM_IP_CONFIG_MAGIC
                       && calc_crc32(&page_b.config, sizeof(page_b.config)) == page_b.crc32;

    uint32_t next_count;
    uint32_t target_addr;

    if (valid_a && valid_b) {
        /* write to the smaller write_count, which is what wears the pages evenly */
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
        /* neither page valid: write page A */
        target_addr = ECM_IP_CONFIG_PAGE_A_ADDR;
        next_count  = 1u;
    }

    /* 3. assemble the structure to write */
    ECM_IPConfig_Flash_t to_write;
    to_write.magic         = ECM_IP_CONFIG_MAGIC;
    to_write.schema_version= ECM_IP_CONFIG_VERSION;
    to_write.write_count   = next_count;
    to_write.config        = *config;
    to_write.crc32         = calc_crc32(&to_write.config, sizeof(to_write.config));
    to_write.footer_magic = ECM_IP_CONFIG_MAGIC;

    /* 4. erase the target page */
    if (!flash_erase_page(target_addr)) {
        LOG_ERROR("ECM IP config: erase failed @0x%08lX", (unsigned long)target_addr);
        return false;   /* the caller returns status code 0xE9 */
    }

    /* 5. program */
    if (!flash_write(target_addr, &to_write, sizeof(to_write))) {
        LOG_ERROR("ECM IP config: program failed @0x%08lX", (unsigned long)target_addr);
        return false;   /* the caller returns status code 0xEA */
    }

    /* 6. read-back verification */
    ECM_IPConfig_Flash_t verify;
    if (!flash_read(target_addr, &verify, sizeof(verify))) {
        LOG_ERROR("ECM IP config: verify read failed");
        return false;   /* the caller returns status code 0xEB */
    }
    if (verify.crc32 != to_write.crc32
        || verify.footer_magic != ECM_IP_CONFIG_MAGIC
        || memcmp(&verify.config, &to_write.config, sizeof(ECM_IPConfig_t)) != 0) {
        LOG_ERROR("ECM IP config: verify mismatch");
        return false;   /* the caller returns status code 0xEB */
    }

    /* 7. update the RAM cache */
    s_ram_cache = *config;
    LOG_INFO("ECM IP config: saved (page %s, count=%lu)",
             target_addr == ECM_IP_CONFIG_PAGE_A_ADDR ? "A" : "B",
             (unsigned long)next_count);
    return true;
}
```

> **Atomicity guarantee**: writes only ever target an erased blank page; the old
> page is not erased until the read-back verification passes. If power is lost
> mid-write the old page remains intact, and the next boot has `Init` pick the old
> page automatically, so no configuration is lost.

### 2. CH9329 command design

> **Command code collision check**: before implementing, cross-check the existing
> command code definitions in `APP/include/usbd_composite_km.h` and confirm that
> 0x26 / 0xA6 and 0x27 / 0xA7 are free. If they collide, pick the next free codes
> and record the choice in the protocol document.

#### Command 0x26 - Set ECM IP Config

**Function:** set the ECM network configuration

**Request format:**
```
57 AB <addr> 26 <len> <sub_cmd> <data...> <checksum>
```

**Sub-commands:**

| sub_cmd | Name | Data format | Notes |
|---------|------|----------|------|
| 0x00 | Set All | [host_ip 4B][dev_ip 4B][subnet 4B][target_host_ip 4B][target_dev_ip 4B] = 20B | set the whole configuration |
| 0x01 | Set Host Side | [host_ip 4B][dev_ip 4B][subnet 4B] = 12B | set the host side only |
| 0x02 | Set Target Side | [target_host_ip 4B][target_dev_ip 4B] = 8B | set the target side only |
| 0x03 | Reset to Default | no data | reset to defaults |

**Response format:**
```
57 AB <addr> A6 01 <status> <checksum>
```

**Status codes:**
- 0x00: success
- 0xE5: bad parameter (wrong length, invalid IP format, illegal subnet mask, IPs not in the same subnet, duplicate IP addresses)
- 0xE9: flash erase failed
- 0xEA: flash program failed
- 0xEB: flash verification failed

> **Partial update constraint**: when using `Set Host Side` (0x01) or
> `Set Target Side` (0x02), the command handler must first read the current full
> configuration from the RAM cache, overlay the new values onto the relevant
> fields, and finally call `validate_config()` to check that all four IPs are still
> in the same subnet. If the user changes only the host-side IP and that puts it in
> a different subnet from the target side, validation fails and 0xE5 is returned.

**Examples:**

Set the whole configuration to the 10.0.0.x subnet:
```
Request : 57 AB 00 26 15 00 0A 00 00 01 0A 00 00 03 FF FF FF 00 0A 00 00 02 0A 00 00 05 <sum>
Response: 57 AB 00 A6 01 00 <sum>
```

Set the host side only:
```
Request : 57 AB 00 26 0D 01 C0 A8 01 01 C0 A8 01 03 FF FF FF 00 <sum>
Response: 57 AB 00 A6 01 00 <sum>
```

Reset to defaults:
```
Request : 57 AB 00 26 01 03 <sum>
Response: 57 AB 00 A6 01 00 <sum>
```

> **Response timing**: a flash write takes about 30–80 ms (erase + program + 
> read-back). The handler blocks, but it runs outside interrupt context, so USB HID
> real-time behaviour is unaffected. The response is only sent once the write has
> completed, so the status code always reflects the persisted result.

#### Command 0x27 - Get ECM IP Config

**Function:** read the ECM network configuration

**Request format:**
```
57 AB <addr> 27 01 <sub_cmd> <checksum>
```

**Sub-commands:**

| sub_cmd | Name | Response data |
|---------|------|----------|
| 0x00 | Get All | [host_ip 4B][dev_ip 4B][subnet 4B][target_host_ip 4B][target_dev_ip 4B] = 20B |
| 0x01 | Get Host Side | [host_ip 4B][dev_ip 4B][subnet 4B] = 12B |
| 0x02 | Get Target Side | [target_host_ip 4B][target_dev_ip 4B] = 8B |
| 0x03 | Get Link Status | [host_ecm_active 1B][target_ecm_active 1B] = 2B |
| 0x05 | Get Diagnostics | [detach_dp_level 1B][reenum_count 2B LE][bus_reset_count 2B LE][dhcp_count 2B LE][dhcp_nak_count 2B LE][target_ecm_active 1B][host_ecm_active 1B] = 11B |

**Response format:**
```
57 AB <addr> A7 <len> <data...> <checksum>
```

**Examples:**

Get the whole configuration:
```
Request : 57 AB 00 27 01 00 <sum>
Response: 57 AB 00 A7 14 C0 A8 0B 01 C0 A8 0B 03 FF FF FF 00 C0 A8 0B 02 C0 A8 0B 05 <sum>
```

Get the target side only:
```
Request : 57 AB 00 27 01 02 <sum>
Response: 57 AB 00 A7 08 C0 A8 0B 02 C0 A8 0B 05 <sum>
```

> **Get commands read the RAM cache only**: no flash access, response latency <1 ms.

### 3. Changes to the ECM bridge code

Modify `APP/ecm_bridge.c` to replace the hard-coded IP addresses with values read
from the configuration module:

```c
/* remove the hard-coded constants */
/* static const uint8_t ECM_HOST_IP[4] = { 192, 168, 11, 1 }; */
/* static const uint8_t ECM_DEV_IP[4]  = { 192, 168, 11, 3 }; */
/* ... */

/* network parameters used at runtime (filled in by the ECM_IPConfig module) */
static uint8_t ecm_host_ip[4];
static uint8_t ecm_dev_ip[4];
static uint8_t ecm_subnet_mask[4];
static uint8_t ecm_target_host_ip[4];
static uint8_t ecm_target_dev_ip[4];

void ECM_Bridge_Init(void) {
    /* read from the config module's RAM cache (Init was called earlier in peripheral_main) */
    ECM_IPConfig_GetHostIP(ecm_host_ip);
    ECM_IPConfig_GetDevIP(ecm_dev_ip);
    ECM_IPConfig_GetSubnetMask(ecm_subnet_mask);
    ECM_IPConfig_GetTargetHostIP(ecm_target_host_ip);
    ECM_IPConfig_GetTargetDevIP(ecm_target_dev_ip);

    /* other initialisation code... */
}
```

> **When configuration takes effect**: the ECM bridge reads the configuration into
> local arrays once at `ECM_Bridge_Init` and never touches flash at runtime. After
> `ECM_IPConfig_Set` successfully writes flash it calls `ECM_Bridge_UpdateIPConfig()`
> to refresh those local arrays immediately, so **no reboot is required**:
> - When a host-side address changes (host/dev/subnet), the firmware replies to the
>   command first, then about 500 ms later performs a soft re-enumeration of the
>   host-facing USBLIB controller (drop the D+ pull-up and re-enable it), forcing the
>   host to re-enumerate the ECM interface and re-DHCP — a link up/down notification
>   alone is not enough on macOS to clear the stale interface/neighbour cache. During
>   re-enumeration the host's CDC ACM and ECM drop briefly and recover; the
>   target-facing USBFS is unaffected.
> - When a target-side address changes (target_host/target_dev), about 500 ms later
>   the target-facing USBFS controller is soft re-enumerated the same way
>   (`USBFS_Device_Init(DISABLE)` drops the D+ pull-up, held for 1 s, then
>   `USBFS_Device_Init(ENABLE)`), so the target really sees a disconnect and
>   re-enumerates the ECM function and re-DHCPs — a 150 ms disconnect window is too
>   short and the target (Linux/Windows) usually keeps the old adapter, hence never
>   re-DHCPs. During re-enumeration the target's HID keyboard/mouse are interrupted
>   for about 1 s and then recover automatically. After re-attaching, the firmware
>   replays the link down/up sequence (so the host does not skip the bus reset) and
>   NAKs DHCP REQUESTs arriving from the target that point at addresses in the old
>   subnet (RFC 2131 4.3.2), forcing the client to drop the stale lease and
>   immediately DISCOVER again.
> - Diagnostics: GET 0x05 returns the D+ level sampled at the last target-side
>   detach, the re-enumeration count, the bus reset count, the DHCP request/NAK
>   counts, and the state of both ECM data interfaces, which directly answers "did
>   the target really see the disconnect" and "did the target re-DHCP".
> - The two sides do not affect each other; only the side whose addresses actually
>   changed is re-enumerated, otherwise just the link down/up fallback is used.
> - Initialisation is split per side: a USBD reset calls `ECM_Bridge_Init()`, which
>   resets only the host-side session state; a USBFS bus reset calls
>   `ECM_Bridge_InitTarget()`, which resets only the target-side session state. That
>   way re-enumerating one side does not clear the other side's active ECM data
>   interface (the peer would not resend SET_INTERFACE). Both reload the five IP
>   addresses from the RAM cache.

### 4. File list

Files to create and modify:

**New files:**
1. `APP/include/ecm_ip_config.h` - configuration module header
2. `APP/ecm_ip_config.c` - configuration module implementation (dual-page wear levelling, CRC32, parameter validation)

**Modified files:**
1. `APP/include/usbd_composite_km.h` - add the command 0x26/0xA6, 0x27/0xA7 definitions (check for collisions first)
2. `APP/usbd_compostie_km.c` - add the command handlers (call `ECM_IPConfig_Set/Get`, map status codes)
3. `APP/ecm_bridge.c` - use configurable IPs instead of hard-coded values
4. `APP/peripheral_main.c` - call `ECM_IPConfig_Init()` before `ECM_Bridge_Init()` during initialisation
5. `docs/ch9329-communication-protocol.md` - document the new commands
6. `build/sources.mk` - add the new source file

### 5. Default value handling

If flash holds no valid configuration (first use, corruption, or a flash read
failure), use the current hard-coded defaults and persist them immediately to
restore a usable two-page state:

```c
#define ECM_IP_CONFIG_DEFAULT_HOST_IP        { 192, 168, 11, 1 }
#define ECM_IP_CONFIG_DEFAULT_DEV_IP         { 192, 168, 11, 3 }
#define ECM_IP_CONFIG_DEFAULT_SUBNET_MASK    { 255, 255, 255, 0 }
#define ECM_IP_CONFIG_DEFAULT_TARGET_HOST_IP { 192, 168, 11, 2 }
#define ECM_IP_CONFIG_DEFAULT_TARGET_DEV_IP { 192, 168, 11, 5 }
```

The defaults must also pass `validate_config()` (the factory defaults are valid by
construction), ensuring that what is written during initialisation can be read back
later.

### 6. Verification flow

#### 6.1 Unit tests (offline)

1. **Validation function tests**
   - valid/invalid subnet masks (e.g. `255.255.255.0` valid, `255.0.255.0` invalid)
   - valid/invalid IPs (`0.0.0.0`, `255.255.255.255` invalid)
   - same-subnet/cross-subnet IP pairs

2. **CRC32 consistency tests**
   - the same configuration produces the same CRC every time
   - changing any single byte changes the CRC

3. **Dual-page selection logic tests**
   - simulate four scenarios: both valid, only A valid, only B valid, neither valid

#### 6.2 Integration tests (device)

1. **Read the default configuration**
   - send the 0x27 command to get the current configuration
   - verify the values are the defaults (192.168.11.x)

2. **Change the configuration**
   - send the 0x26 command to set new IPs (e.g. the 10.0.0.x subnet)
   - verify the response status is success (0x00)

3. **Parameter validation tests**
   - send an illegal subnet mask (e.g. 255.0.255.0), verify 0xE5
   - send a host_ip/dev_ip pair outside one subnet, verify 0xE5
   - send a request with a mismatched length, verify 0xE5

4. **Verify persistence**
   - reboot the device
   - send the 0x27 command to read the configuration
   - verify the configuration was saved

5. **Power-loss recovery test**
   - cut power while a 0x26 write is in progress (simulated power loss)
   - reboot and verify the configuration is not corrupt (falls back to the previous valid configuration or the defaults)

6. **Wear levelling test**
   - write the configuration N times in a row (N ≥ 10)
   - read `write_count` on both pages and verify it alternates

7. **Functional verification**
   - verify DHCP replies use the new IPs
   - verify host-side/target-side network communication works

### 7. Notes

1. **IP format validation**: when setting the configuration, an IP address must not
   be `0.0.0.0` or `255.255.255.255`.

2. **All IPs must be in the same subnet**: the ECM bridge emulates two NICs inside
   the firmware and bridges their traffic, so **host_ip, dev_ip, target_host_ip and
   target_dev_ip must all be in the same subnet**. If the host side and the target
   side are not in the same subnet, routing fails and the network cannot come up.
   For example:
   - ✅ valid: `192.168.11.1`, `192.168.11.3`, `192.168.11.2`, `192.168.11.5`, mask `255.255.255.0`
   - ❌ invalid: `192.168.11.1`, `192.168.11.3`, `10.0.0.2`, `10.0.0.5`, mask `255.255.255.0` (host side and target side on different subnets)

3. **IP addresses must be distinct**: the four IP addresses must all differ, to
   avoid conflicts. For example `host_ip` must not equal `dev_ip`.

4. **Subnet mask validation**: the mask must be a run of 1s followed by a run of 0s
   (e.g. `255.255.255.0` and `255.255.0.0` are valid; `255.0.255.0` is not).

5. **Flash wear levelling**: dual-page alternating writes plus a `write_count`
   counter keep either page from wearing out early. Even so, avoid changing the
   configuration often; a higher layer should offer a "configuration changed, restart
   to apply?" prompt rather than writing repeatedly on its own.

6. **Atomic updates**: writes go only to a new page, and the old page is not erased
   until the read-back verification passes. On power loss the old page survives
   intact and the next boot recovers automatically.

7. **Takes effect on reboot**: after changing the configuration the device must be
   restarted for it to take effect (the ECM bridge reads the configuration into
   local arrays at `Init` and does not access flash at runtime).

8. **Conditional compilation**: the ECM IP configuration module is only compiled
   when `USB_WORK_MODE_COMPILES_BRIDGE_ECM` is set; firmware variants without the
   ECM bridge do not include it.

9. **Error recovery**: on a flash read failure or a CRC/magic check failure, degrade
   safely to the defaults and log it, without blocking boot.

10. **Security (optional hardening)**: changing the network configuration is a
    sensitive operation. If the device may be exposed to an untrusted environment, a
    later iteration could add simple authentication (e.g. require an unlock command
    carrying a pre-shared key), or restrict configuration changes to the physical
    interface (UART) only. The current version implements no authentication and relies
    on the physical isolation of the CH9329 channel.

### 8. Implementation steps

1. Create the `ecm_ip_config` module (header and implementation, with CRC32, dual-page wear levelling, parameter validation, error recovery)
2. Add the command definitions to `usbd_composite_km.h` (check 0x26/0x27/0xA6/0xA7 for collisions first)
3. Implement the command handlers in `usbd_compostie_km.c` (map validation/flash errors to status codes)
4. Change `ecm_bridge.c` to use configurable IPs
5. Initialise in `peripheral_main.c` in the order `ECM_IPConfig_Init()` → `ECM_Bridge_Init()`
6. Update `sources.mk` with the new files
7. Update the protocol document
8. Unit tests (validation functions, CRC, dual-page selection)
9. Integration tests (defaults, changes, persistence, power-loss recovery, wear levelling, function)

## Summary

On top of the original design this introduces **dual-page wear levelling**, **magic
+ CRC32 + trailing magic triple verification**, **versioned configuration** and
**full parameter validation**, giving the ECM IP configuration industrial-grade
reliability:

- **Power-loss safe**: the atomic write strategy guarantees the configuration is never corrupted
- **Long-lived**: alternating writes across two pages spread the flash wear
- **Robust**: parameter validation rejects illegal configurations (all IPs in one subnet, all distinct), and error recovery keeps boot from blocking
- **Evolvable**: the version number and schema version support future firmware backward compatibility

**Key constraint**: the way the ECM bridge works requires host_ip, dev_ip,
target_host_ip and target_dev_ip to all be in the same subnet, otherwise routing
between the host side and the target side fails. The configuration commands'
validation logic enforces this, so any illegal configuration is rejected.

The design follows the existing flash configuration pattern and offers good
backward compatibility and extensibility.
