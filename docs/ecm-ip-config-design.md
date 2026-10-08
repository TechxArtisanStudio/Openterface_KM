# ECM IP Configuration Design

## 概述

允许通过CH9329协议手动修改ECM桥接中的所有网络配置参数，包括主机侧IP、目标侧IP、子网掩码等。

本设计在原有方案基础上增强了以下关键能力，以满足工业级可靠性要求：

- **Flash 磨损均衡**：双页交替写入，避免单页过早磨损
- **原子更新**：魔数 + CRC32 + 尾部魔数三重校验，断电不损坏
- **配置版本化**：版本号字段，支持未来固件向后兼容
- **完整参数校验**：IP 格式、子网掩码连续性、同子网约束
- **错误恢复**：Flash 读取失败 / CRC 校验失败时安全降级到默认值

## 当前硬编码的IP地址

在 `APP/ecm_bridge.c` 中，以下IP地址是硬编码的：

| 变量名 | 默认值 | 用途 |
|--------|--------|------|
| `ECM_HOST_IP` | 192.168.11.1 | 主机侧（Mac/PC）的IP地址 |
| `ECM_DEV_IP` | 192.168.11.3 | 固件在主机侧的IP地址 |
| `ECM_SUBNET_MASK` | 255.255.255.0 | 子网掩码 |
| `ECM_TARGET_HOST_IP` | 192.168.11.2 | 目标PC的IP地址 |
| `ECM_TARGET_DEV_IP` | 192.168.11.5 | 固件在目标侧的IP地址 |

## 设计方案

### 1. Flash配置模块

创建新的配置模块 `ecm_ip_config`，负责存储和读取网络配置。

#### 1.1 配置数据结构

```c
#include <stdint.h>
#include <stdbool.h>

#define ECM_IP_CONFIG_MAGIC        0x45434D49u  /* "ECMI" */
#define ECM_IP_CONFIG_VERSION     1u
#define ECM_IP_CONFIG_PAGE_A_ADDR 0x0807D000u   /* 双页 A */
#define ECM_IP_CONFIG_PAGE_B_ADDR 0x0807C000u   /* 双页 B */
#define ECM_IP_CONFIG_PAGE_SIZE   0x1000u       /* 4KB */

/* 用户可见的网络配置（24 字节，4 字节对齐） */
typedef struct {
    uint8_t  version;           /* 配置版本号，当前为 1 */
    uint8_t  host_ip[4];        /* 主机侧IP (默认: 192.168.11.1) */
    uint8_t  dev_ip[4];         /* 固件在主机侧的IP (默认: 192.168.11.3) */
    uint8_t  subnet_mask[4];    /* 子网掩码 (默认: 255.255.255.0) */
    uint8_t  target_host_ip[4]; /* 目标PC的IP (默认: 192.168.11.2) */
    uint8_t  target_dev_ip[4];  /* 固件在目标侧的IP (默认: 192.168.11.5) */
    uint8_t  reserved[2];       /* 保留字节，用于未来扩展 */
    uint8_t  padding;           /* 填充至 4 字节对齐 */
} ECM_IPConfig_t;               /* sizeof = 24 */

/* Flash 中的完整存储结构（含校验信息） */
typedef struct {
    uint32_t magic;             /* 魔数：ECM_IP_CONFIG_MAGIC */
    uint32_t schema_version;    /* 配置结构版本号（区别于 ECM_IPConfig_t.version） */
    uint32_t crc32;             /* config 字段的 CRC32 */
    uint32_t write_count;       /* 写入计数，用于磨损均衡 */
    ECM_IPConfig_t config;      /* 实际网络配置 */
    uint32_t footer_magic;      /* 尾部魔数，确保整块写入完整 */
} ECM_IPConfig_Flash_t;         /* sizeof = 44 */
```

> **内存对齐说明**：`ECM_IPConfig_t` 通过 `padding` 字段保证 4 字节对齐，避免在 ARM Cortex-M 上出现非对齐访问异常。`ECM_IPConfig_Flash_t` 全部为 32 位字段，天然 4 字节对齐。

#### 1.2 Flash存储位置

使用 flash 最后区域的两页（各 4KB），实现双页交替写入：

```
Flash布局（从末尾开始）：
- 0x0807F000: USB mode config       (4KB)
- 0x0807E000: Default target config (4KB)
- 0x0807D000: ECM IP config page A  (4KB)  <- 新增
- 0x0807C000: ECM IP config page B  (4KB)  <- 新增（备份页）
```

> **双页策略**：每次写入选择 `write_count` 较小的页面进行擦写，实现磨损均衡；读取时比较两页的 `magic`/`crc32`/`write_count`，选取有效且最新的版本。即使写入过程中断电，另一页仍保留上一份有效配置，系统可安全降级。

#### 1.3 默认值定义

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

#### 1.4 API接口

```c
/* 初始化配置（从flash读取或使用默认值） */
void  ECM_IPConfig_Init(void);

/* 获取配置（从 RAM 缓存读取，不访问 flash） */
void  ECM_IPConfig_Get(ECM_IPConfig_t *config);
void  ECM_IPConfig_GetHostIP(uint8_t ip[4]);
void  ECM_IPConfig_GetDevIP(uint8_t ip[4]);
void  ECM_IPConfig_GetSubnetMask(uint8_t mask[4]);
void  ECM_IPConfig_GetTargetHostIP(uint8_t ip[4]);
void  ECM_IPConfig_GetTargetDevIP(uint8_t ip[4]);

/* 设置配置（校验 + 写入 flash，双页磨损均衡） */
bool  ECM_IPConfig_Set(const ECM_IPConfig_t *config);
bool  ECM_IPConfig_SetHostSide(const uint8_t host_ip[4],
                               const uint8_t dev_ip[4],
                               const uint8_t subnet[4]);
bool  ECM_IPConfig_SetTargetSide(const uint8_t target_host_ip[4],
                                const uint8_t target_dev_ip[4]);

/* 重置为默认值 */
bool  ECM_IPConfig_Reset(void);
```

#### 1.5 参数校验

设置配置前必须通过以下校验，任一失败即返回 `0xE5`：

```c
/* 子网掩码必须是连续的 1 后跟连续的 0 */
static bool validate_subnet_mask(const uint8_t mask[4]) {
    uint32_t m = ((uint32_t)mask[0] << 24) | ((uint32_t)mask[1] << 16)
               | ((uint32_t)mask[2] << 8)  |  (uint32_t)mask[3];
    if (m == 0u || m == 0xFFFFFFFFu) return false;
    /* 连续掩码满足 (~m + 1) & m == 0 */
    return ((~m + 1u) & m) == 0u;
}

/* IP 不能是 0.0.0.0 或 255.255.255.255 */
static bool validate_ip_basic(const uint8_t ip[4]) {
    if ((ip[0] | ip[1] | ip[2] | ip[3]) == 0u) return false;
    if ((ip[0] & ip[1] & ip[2] & ip[3]) == 0xFFu) return false;
    return true;
}

/* 两个 IP 必须在同一子网（network 部分相同） */
static bool validate_same_subnet(const uint8_t ip1[4],
                                 const uint8_t ip2[4],
                                 const uint8_t mask[4]) {
    for (int i = 0; i < 4; i++) {
        if ((ip1[i] & mask[i]) != (ip2[i] & mask[i])) return false;
    }
    return true;
}

/* 所有 IP 必须在同一子网（ECM 桥接要求） */
static bool validate_config(const ECM_IPConfig_t *cfg) {
    if (cfg->version == 0u) return false;
    
    /* 1. IP 基本合法性检查 */
    if (!validate_ip_basic(cfg->host_ip))        return false;
    if (!validate_ip_basic(cfg->dev_ip))         return false;
    if (!validate_ip_basic(cfg->target_host_ip)) return false;
    if (!validate_ip_basic(cfg->target_dev_ip))  return false;
    
    /* 2. 子网掩码合法性检查 */
    if (!validate_subnet_mask(cfg->subnet_mask))  return false;
    
    /* 3. 所有 IP 必须在同一子网
     *    ECM 桥接在固件内部模拟两个网卡并桥接流量，
     *    如果主机侧和目标侧不在同一子网，会导致路由失败。
     */
    if (!validate_same_subnet(cfg->host_ip, cfg->dev_ip, cfg->subnet_mask))
        return false;
    if (!validate_same_subnet(cfg->host_ip, cfg->target_host_ip, cfg->subnet_mask))
        return false;
    if (!validate_same_subnet(cfg->host_ip, cfg->target_dev_ip, cfg->subnet_mask))
        return false;
    
    /* 4. 所有 IP 必须互不相同（避免地址冲突） */
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

> **注意**：主机侧与目标侧允许处于不同子网（跨子网通信由主机侧路由处理），因此校验仅要求 host/dev 同子网、target_host/target_dev 同子网，但不要求两侧子网相同。

#### 1.6 初始化与错误恢复

```c
static ECM_IPConfig_t s_ram_cache;   /* RAM 缓存，运行期只读访问 */

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
        /* 两页均有效，选 write_count 较大者 */
        s_ram_cache = (page_a.write_count >= page_b.write_count)
                      ? page_a.config : page_b.config;
    } else if (valid_a) {
        s_ram_cache = page_a.config;
        /* 后台修复 page_b */
    } else if (valid_b) {
        s_ram_cache = page_b.config;
        /* 后台修复 page_a */
    } else {
        /* 两页均无效（首次使用或全部损坏），使用默认值并持久化 */
        LOG_WARN("ECM IP config: no valid page, loading defaults");
        s_ram_cache = ECM_IP_CONFIG_DEFAULT;
        ECM_IPConfig_Save(&s_ram_cache);   /* 落盘默认值 */
    }
}
```

> **错误恢复策略**：
> - Flash 读取失败 → 降级到默认值，记录 `LOG_ERROR`
> - CRC / 魔数校验失败 → 降级到默认值，记录 `LOG_WARN`
> - 双页均损坏 → 加载默认值并重写一页，恢复可用状态
> - 校验失败的脏页在下次 `Set` 时被覆盖修复

#### 1.7 双页磨损均衡写入

```c
bool ECM_IPConfig_Set(const ECM_IPConfig_t *config) {
    /* 1. 参数校验 */
    if (!validate_config(config)) {
        LOG_WARN("ECM IP config: validation failed");
        return false;   /* 上层返回状态码 0xE5 */
    }

    /* 2. 读取两页状态，决定写入目标页 */
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
        /* 选 write_count 较小者写入，实现磨损均衡 */
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
        /* 两页均无效，写 page A */
        target_addr = ECM_IP_CONFIG_PAGE_A_ADDR;
        next_count  = 1u;
    }

    /* 3. 组装写入结构 */
    ECM_IPConfig_Flash_t to_write;
    to_write.magic         = ECM_IP_CONFIG_MAGIC;
    to_write.schema_version= ECM_IP_CONFIG_VERSION;
    to_write.write_count   = next_count;
    to_write.config        = *config;
    to_write.crc32         = calc_crc32(&to_write.config, sizeof(to_write.config));
    to_write.footer_magic = ECM_IP_CONFIG_MAGIC;

    /* 4. 擦除目标页 */
    if (!flash_erase_page(target_addr)) {
        LOG_ERROR("ECM IP config: erase failed @0x%08lX", (unsigned long)target_addr);
        return false;   /* 上层返回状态码 0xE9 */
    }

    /* 5. 编程写入 */
    if (!flash_write(target_addr, &to_write, sizeof(to_write))) {
        LOG_ERROR("ECM IP config: program failed @0x%08lX", (unsigned long)target_addr);
        return false;   /* 上层返回状态码 0xEA */
    }

    /* 6. 回读验证 */
    ECM_IPConfig_Flash_t verify;
    if (!flash_read(target_addr, &verify, sizeof(verify))) {
        LOG_ERROR("ECM IP config: verify read failed");
        return false;   /* 上层返回状态码 0xEB */
    }
    if (verify.crc32 != to_write.crc32
        || verify.footer_magic != ECM_IP_CONFIG_MAGIC
        || memcmp(&verify.config, &to_write.config, sizeof(ECM_IPConfig_t)) != 0) {
        LOG_ERROR("ECM IP config: verify mismatch");
        return false;   /* 上层返回状态码 0xEB */
    }

    /* 7. 更新 RAM 缓存 */
    s_ram_cache = *config;
    LOG_INFO("ECM IP config: saved (page %s, count=%lu)",
             target_addr == ECM_IP_CONFIG_PAGE_A_ADDR ? "A" : "B",
             (unsigned long)next_count);
    return true;
}
```

> **原子性保证**：写入只在擦除后的空白页进行；旧页在回读验证通过前不被擦除。若写入中途断电，旧页仍完整可用，下次启动时 `Init` 会自动选取旧页，配置不丢失。

### 2. CH9329命令设计

> **命令码冲突检查**：实现前必须核对 `APP/include/usbd_composite_km.h` 中已有的命令码定义，确认 0x26 / 0xA6 与 0x27 / 0xA7 未被占用。若冲突，顺延选择空闲码并在协议文档中记录。

#### 命令 0x26 - Set ECM IP Config

**功能：** 设置ECM网络配置

**请求格式：**
```
57 AB <addr> 26 <len> <sub_cmd> <data...> <checksum>
```

**子命令：**

| sub_cmd | 名称 | 数据格式 | 说明 |
|---------|------|----------|------|
| 0x00 | Set All | [host_ip 4B][dev_ip 4B][subnet 4B][target_host_ip 4B][target_dev_ip 4B] = 20B | 设置全部配置 |
| 0x01 | Set Host Side | [host_ip 4B][dev_ip 4B][subnet 4B] = 12B | 仅设置主机侧配置 |
| 0x02 | Set Target Side | [target_host_ip 4B][target_dev_ip 4B] = 8B | 仅设置目标侧配置 |
| 0x03 | Reset to Default | 无数据 | 重置为默认值 |

**响应格式：**
```
57 AB <addr> A6 01 <status> <checksum>
```

**状态码：**
- 0x00: 成功
- 0xE5: 参数错误（长度不正确、IP格式无效、子网掩码非法、IP不在同一子网、IP地址重复）
- 0xE9: Flash擦除失败
- 0xEA: Flash编程失败
- 0xEB: Flash验证失败

> **部分更新约束**：使用 `Set Host Side` (0x01) 或 `Set Target Side` (0x02) 时，命令处理器需要先从 RAM 缓存读取当前完整配置，再用新值覆盖对应字段，最后调用 `validate_config()` 校验全部四个 IP 是否仍在同一子网。如果用户只修改了主机侧 IP 但导致与目标侧 IP 不在同一子网，校验将失败并返回 0xE5。

**示例：**

设置全部配置为 10.0.0.x 网段：
```
Request : 57 AB 00 26 15 00 0A 00 00 01 0A 00 00 03 FF FF FF 00 0A 00 00 02 0A 00 00 05 <sum>
Response: 57 AB 00 A6 01 00 <sum>
```

仅设置主机侧配置：
```
Request : 57 AB 00 26 0D 01 C0 A8 01 01 C0 A8 01 03 FF FF FF 00 <sum>
Response: 57 AB 00 A6 01 00 <sum>
```

重置为默认值：
```
Request : 57 AB 00 26 01 03 <sum>
Response: 57 AB 00 A6 01 00 <sum>
```

> **响应时序**：Flash 擦写耗时（擦除+编程+回读验证约 30–80ms），命令处理为阻塞式但已在非中断上下文执行，不影响 USB HID 实时性。响应在写入完成后才返回，确保状态码准确反映持久化结果。

#### 命令 0x27 - Get ECM IP Config

**功能：** 获取ECM网络配置

**请求格式：**
```
57 AB <addr> 27 01 <sub_cmd> <checksum>
```

**子命令：**

| sub_cmd | 名称 | 响应数据 |
|---------|------|----------|
| 0x00 | Get All | [host_ip 4B][dev_ip 4B][subnet 4B][target_host_ip 4B][target_dev_ip 4B] = 20B |
| 0x01 | Get Host Side | [host_ip 4B][dev_ip 4B][subnet 4B] = 12B |
| 0x02 | Get Target Side | [target_host_ip 4B][target_dev_ip 4B] = 8B |

**响应格式：**
```
57 AB <addr> A7 <len> <data...> <checksum>
```

**示例：**

获取全部配置：
```
Request : 57 AB 00 27 01 00 <sum>
Response: 57 AB 00 A7 14 C0 A8 0B 01 C0 A8 0B 03 FF FF FF 00 C0 A8 0B 02 C0 A8 0B 05 <sum>
```

仅获取目标侧配置：
```
Request : 57 AB 00 27 01 02 <sum>
Response: 57 AB 00 A7 08 C0 A8 0B 02 C0 A8 0B 05 <sum>
```

> **Get 命令只读 RAM 缓存**：不触发 Flash 访问，响应延迟 <1ms。

### 3. 修改ECM桥接代码

修改 `APP/ecm_bridge.c`，将硬编码的IP地址替换为从配置模块读取的值：

```c
/* 移除硬编码的常量 */
/* static const uint8_t ECM_HOST_IP[4] = { 192, 168, 11, 1 }; */
/* static const uint8_t ECM_DEV_IP[4]  = { 192, 168, 11, 3 }; */
/* ... */

/* 运行期使用的网络参数（由 ECM_IPConfig 模块填充） */
static uint8_t ecm_host_ip[4];
static uint8_t ecm_dev_ip[4];
static uint8_t ecm_subnet_mask[4];
static uint8_t ecm_target_host_ip[4];
static uint8_t ecm_target_dev_ip[4];

void ECM_Bridge_Init(void) {
    /* 从配置模块 RAM 缓存读取（Init 已在 peripheral_main 中先行调用） */
    ECM_IPConfig_GetHostIP(ecm_host_ip);
    ECM_IPConfig_GetDevIP(ecm_dev_ip);
    ECM_IPConfig_GetSubnetMask(ecm_subnet_mask);
    ECM_IPConfig_GetTargetHostIP(ecm_target_host_ip);
    ECM_IPConfig_GetTargetDevIP(ecm_target_dev_ip);

    /* 其他初始化代码... */
}
```

> **配置生效时机**：ECM 桥接在 `ECM_Bridge_Init` 时一次性读取配置到本地数组，运行期不再访问 Flash。因此配置修改后**必须重启设备**才能生效。`Set` 命令的响应中不包含"立即生效"语义，上层应用应在收到成功响应后提示用户重启。

### 4. 文件清单

需要创建和修改的文件：

**新增文件：**
1. `APP/include/ecm_ip_config.h` - 配置模块头文件
2. `APP/ecm_ip_config.c` - 配置模块实现（含双页磨损均衡、CRC32、参数校验）

**修改文件：**
1. `APP/include/usbd_composite_km.h` - 添加命令 0x26/0xA6、0x27/0xA7 定义（先核对冲突）
2. `APP/usbd_compostie_km.c` - 添加命令处理器（调用 `ECM_IPConfig_Set/Get`，映射状态码）
3. `APP/ecm_bridge.c` - 使用可配置IP替代硬编码值
4. `APP/peripheral_main.c` - 在初始化时先调用 `ECM_IPConfig_Init()`，再调用 `ECM_Bridge_Init()`
5. `docs/ch9329-communication-protocol.md` - 添加新命令文档
6. `build/sources.mk` - 添加新源文件

### 5. 默认值处理

如果flash中没有有效配置（首次使用、配置损坏或 Flash 读取失败），使用当前的硬编码默认值，并立即持久化一次以恢复双页可用状态：

```c
#define ECM_IP_CONFIG_DEFAULT_HOST_IP        { 192, 168, 11, 1 }
#define ECM_IP_CONFIG_DEFAULT_DEV_IP         { 192, 168, 11, 3 }
#define ECM_IP_CONFIG_DEFAULT_SUBNET_MASK    { 255, 255, 255, 0 }
#define ECM_IP_CONFIG_DEFAULT_TARGET_HOST_IP { 192, 168, 11, 2 }
#define ECM_IP_CONFIG_DEFAULT_TARGET_DEV_IP { 192, 168, 11, 5 }
```

默认值同样需通过 `validate_config()` 校验（出厂默认值天然合法），确保初始化阶段写入的数据可被后续读取识别。

### 6. 验证流程

#### 6.1 单元测试（离线）

1. **校验函数测试**
   - 合法/非法子网掩码（如 `255.255.255.0` 合法，`255.0.255.0` 非法）
   - 合法/非法 IP（`0.0.0.0`、`255.255.255.255` 非法）
   - 同子网/跨子网 IP 对

2. **CRC32 一致性测试**
   - 同一配置多次计算 CRC 一致
   - 篡改任一字节后 CRC 变化

3. **双页选择逻辑测试**
   - 模拟两页均有效、仅 A 有效、仅 B 有效、均无效四种场景

#### 6.2 集成测试（设备）

1. **读取默认配置**
   - 发送 0x27 命令获取当前配置
   - 验证返回值为默认值（192.168.11.x）

2. **修改配置**
   - 发送 0x26 命令设置新IP（如 10.0.0.x 网段）
   - 验证响应状态为成功（0x00）

3. **参数校验测试**
   - 发送非法子网掩码（如 255.0.255.0），验证返回 0xE5
   - 发送不在同一子网的 host_ip/dev_ip，验证返回 0xE5
   - 发送长度不匹配的请求，验证返回 0xE5

4. **验证持久化**
   - 重启设备
   - 发送 0x27 命令读取配置
   - 验证配置已保存

5. **断电恢复测试**
   - 发送 0x26 写入过程中拔电（模拟断电）
   - 重启设备，验证配置未损坏（回退到上一份有效配置或默认值）

6. **磨损均衡测试**
   - 连续写入 N 次配置（N ≥ 10）
   - 读取两页 `write_count`，验证交替递增

7. **功能验证**
   - 验证 DHCP 响应使用新 IP
   - 验证主机侧/目标侧网络通信正常

### 7. 注意事项

1. **IP 格式验证**：设置配置时校验 IP 地址不能是 `0.0.0.0` 或 `255.255.255.255`。

2. **所有 IP 必须在同一子网**：ECM 桥接在固件内部模拟两个网卡并桥接流量，**host_ip、dev_ip、target_host_ip、target_dev_ip 必须全部位于同一子网**。如果主机侧和目标侧不在同一子网，会导致路由失败，网络通信无法建立。例如：
   - ✅ 合法：`192.168.11.1`、`192.168.11.3`、`192.168.11.2`、`192.168.11.5`，掩码 `255.255.255.0`
   - ❌ 非法：`192.168.11.1`、`192.168.11.3`、`10.0.0.2`、`10.0.0.5`，掩码 `255.255.255.0`（主机侧与目标侧跨子网）

3. **IP 地址必须互不相同**：四个 IP 地址必须各不相同，避免地址冲突。例如 `host_ip` 不能等于 `dev_ip`。

4. **子网掩码验证**：子网掩码必须是连续的 1 后跟连续的 0（如 `255.255.255.0`、`255.255.0.0` 合法；`255.0.255.0` 非法）。

5. **Flash 磨损均衡**：采用双页交替写入 + `write_count` 计数，避免单页过早磨损。即便如此，仍应避免频繁修改配置；上层应用建议提供"配置已变更，是否重启生效"提示而非自动反复写入。

6. **原子更新**：写入只在新页进行，旧页在回读验证通过前不被擦除。断电时旧页完整保留，下次启动自动恢复。

7. **重启生效**：配置修改后需要重启设备才能生效（ECM 桥接在 `Init` 时读取配置到本地数组，运行期不访问 Flash）。

8. **条件编译**：ECM IP 配置模块仅在 `USB_WORK_MODE_COMPILES_BRIDGE_ECM` 时编译，未启用 ECM 桥接的固件变体不包含此模块。

9. **错误恢复**：Flash 读取失败、CRC/魔数校验失败时，安全降级到默认值并记录日志，不阻塞启动。

10. **安全性（可选增强）**：修改网络配置是敏感操作。若设备可能暴露在不可信环境，可在后续迭代中增加简单鉴权（如要求先发送解锁命令携带预共享密钥），或将配置修改限制为仅物理接口（UART）可达。当前版本不实现鉴权，依赖 CH9329 通道的物理隔离。

### 8. 实现步骤

1. 创建 `ecm_ip_config` 模块（头文件和实现，含 CRC32、双页磨损均衡、参数校验、错误恢复）
2. 在 `usbd_composite_km.h` 中添加命令定义（先核对 0x26/0x27/0xA6/0xA7 冲突）
3. 在 `usbd_compostie_km.c` 中实现命令处理器（映射校验/Flash 错误到状态码）
4. 修改 `ecm_bridge.c` 使用可配置IP
5. 在 `peripheral_main.c` 中按 `ECM_IPConfig_Init()` → `ECM_Bridge_Init()` 顺序初始化
6. 更新 `sources.mk` 添加新文件
7. 更新协议文档
8. 单元测试（校验函数、CRC、双页选择）
9. 集成测试（默认值、修改、持久化、断电恢复、磨损均衡、功能）

## 总结

该设计方案在原有基础上引入了**双页磨损均衡**、**魔数+CRC32+尾部魔数三重校验**、**配置版本化**和**完整参数校验**，使 ECM IP 配置具备工业级可靠性：

- **断电安全**：原子写入策略保证配置不损坏
- **长寿命**：双页交替写入分散 Flash 磨损
- **健壮性**：参数校验拦截非法配置（所有 IP 必须同子网、互不重复），错误恢复保证启动不阻塞
- **可演进**：版本号与 schema 版本支持未来固件向后兼容

**关键约束**：ECM 桥接的工作机制要求 host_ip、dev_ip、target_host_ip、target_dev_ip 必须全部位于同一子网，否则主机侧与目标侧之间的路由将失败。配置命令的校验逻辑会强制这一约束，确保任何非法配置都被拒绝。

设计遵循现有的 Flash 配置模式，提供了良好的向后兼容性和扩展性。
