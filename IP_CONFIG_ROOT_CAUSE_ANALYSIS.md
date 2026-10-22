# IP Configuration Issue - Root Cause Analysis

**日期**: 2026-10-22  
**问题**: CH9329命令修改IP配置后不生效

## 问题描述

使用CH9329命令修改整个网段（从192.168.11.x到192.168.22.x）后：
- 命令返回成功（0x00）
- 但旧IP（192.168.11.x）仍然可以ping通
- 新IP（192.168.22.x）无法ping通
- 即使拔插USB后，旧IP仍然可以访问

## 代码流程分析

### 1. 命令处理流程

```
TCP包到达
  ↓
handle_host_control_frame() [ecm_bridge.c:1433]
  ↓
tcp_payload_from_ipv4_frame() [ecm_bridge.c:1365]
  - 提取TCP payload（CH9329命令）
  ↓
ch9329_payload_valid() [ecm_bridge.c:197]
  - 验证CH9329帧格式和校验和
  ↓
CH9329_RxBuffer_Add() [usbd_compostie_km.c:715]
  - 添加到RX缓冲区
  ↓
CH9329_DataParser() [usbd_compostie_km.c:673]
  - 解析CH9329帧
  - 验证校验和：sum(buf[0]到buf[len-2]) == buf[len-1]
  ↓
CH9329_DispatchCommand() [usbd_compostie_km.c:226]
  - 分发命令
  ↓
CMD_SET_ECM_IP_CONFIG处理 [usbd_compostie_km.c:436]
  - sub_cmd 0x00: Set All
  - 解析21字节数据（5个IP地址，每个4字节）
  ↓
ECM_IPConfig_Set() [ecm_ip_config.c:215]
  - 验证配置
  - 写入Flash（Page A或B）
  - 更新RAM cache：s_ram_cache = *config
  - 调用ECM_Bridge_UpdateIPConfig()
  ↓
ECM_Bridge_UpdateIPConfig() [ecm_bridge.c:2069]
  - 从RAM cache读取IP配置
  - 更新全局变量：
    - ecm_host_ip
    - ecm_dev_ip
    - ecm_subnet_mask
    - ecm_target_host_ip
    - ecm_target_dev_ip
```

### 2. 关键代码检查

#### ✅ CH9329帧验证（ecm_bridge.c:166-195）
```c
static uint8_t ch9329_frame_valid(const uint8_t *data, uint16_t len, uint16_t *frame_len)
{
    // 检查帧头：0x57 0xAB
    if (data[0] != CH9329_FRAME_HEAD1 || data[1] != CH9329_FRAME_HEAD2) {
        return 0;
    }
    
    // 计算总长度：6 + data[4]
    total_len = (uint16_t)(ECM_CH9329_MIN_FRAME_LEN + data[4]);
    
    // 校验和：sum(data[0]到data[total_len-2]) == data[total_len-1]
    for (index = 0; index < (uint16_t)(total_len - 1u); index++) {
        checksum = (uint8_t)(checksum + data[index]);
    }
    if (checksum != data[total_len - 1u]) {
        return 0;
    }
    
    return 1;
}
```
**状态**: ✅ 正确

#### ✅ SET_ECM_IP_CONFIG处理（usbd_compostie_km.c:436-484）
```c
case CMD_SET_ECM_IP_CONFIG:
{
    uint8_t status = STATUS_SUCCESS;
    
    if (data_len < 1) {
        status = STATUS_ERR_PARAM;
    } else {
        uint8_t sub_cmd = pdata[0];
        
        switch (sub_cmd) {
            case 0x00: // Set All
                if (data_len != 21) { // 1 byte sub_cmd + 20 bytes data
                    status = STATUS_ERR_PARAM;
                } else {
                    ECM_IPConfig_t config = {
                        .record_version = ECM_IP_CONFIG_VERSION,
                        .host_ip = { pdata[1], pdata[2], pdata[3], pdata[4] },
                        .dev_ip = { pdata[5], pdata[6], pdata[7], pdata[8] },
                        .subnet_mask = { pdata[9], pdata[10], pdata[11], pdata[12] },
                        .target_host_ip = { pdata[13], pdata[14], pdata[15], pdata[16] },
                        .target_dev_ip = { pdata[17], pdata[18], pdata[19], pdata[20] },
                    };
                    
                    // 验证配置
                    if (!ECM_IPConfig_Validate(&config)) {
                        status = STATUS_ERR_PARAM;
                    } else {
                        // 先发送成功响应
                        CH9329_SendResponse(addr, cmd_code, &status, 1, 1);
                        // 然后写入Flash
                        uint8_t result = ECM_IPConfig_Set(&config);
                        // 更新ECM bridge
                        if (result == ECM_IP_CONFIG_OK) {
                            ECM_Bridge_UpdateIPConfig();
                        }
                    }
                }
                break;
        }
    }
}
```
**状态**: ✅ 正确

#### ✅ Flash写入和RAM cache更新（ecm_ip_config.c:215-294）
```c
uint8_t ECM_IPConfig_Set(const ECM_IPConfig_t *config)
{
    // 1. 验证参数
    if (!validate_config(config)) {
        return ECM_IP_CONFIG_ERR_PARAM;
    }
    
    // 2. 读取两个Flash page
    ok_a = flash_read_page(ECM_IP_CONFIG_PAGE_A_ADDR, &page_a, sizeof(page_a));
    ok_b = flash_read_page(ECM_IP_CONFIG_PAGE_B_ADDR, &page_b, sizeof(page_b));
    
    // 3. 选择目标page（wear leveling）
    // 4. 组装写入数据
    to_write.config = *config;
    to_write.crc32 = calc_crc32(&to_write.config, sizeof(to_write.config));
    
    // 5. 擦除目标page
    // 6. 写入数据
    // 7. 读回验证
    if (verify.crc32 != to_write.crc32 || ...) {
        return ECM_IP_CONFIG_ERR_FLASH_VERIFY;
    }
    
    // 8. 更新RAM cache
    s_ram_cache = *config;
    
    // 9. 更新bridge层IP变量
    ECM_Bridge_UpdateIPConfig();
    
    return ECM_IP_CONFIG_OK;
}
```
**状态**: ✅ 正确

#### ✅ Bridge层IP变量更新（ecm_bridge.c:2069-2090）
```c
void ECM_Bridge_UpdateIPConfig(void)
{
    // 从ECM_IPConfig模块的RAM cache加载网络配置
    ECM_IPConfig_GetHostIP(ecm_host_ip);
    ECM_IPConfig_GetDevIP(ecm_dev_ip);
    ECM_IPConfig_GetSubnetMask(ecm_subnet_mask);
    ECM_IPConfig_GetTargetHostIP(ecm_target_host_ip);
    ECM_IPConfig_GetTargetDevIP(ecm_target_dev_ip);
    
    // 重置GARP发送标志
    g_host_garp_sent = 0;
    g_target_garp_sent = 0;
    
    USB_DEBUG("ECM IP config updated\r\n");
}
```
**状态**: ✅ 正确

## 问题定位

经过详细代码审查，所有关键函数都看起来正确。问题可能在于：

### 可能原因1: TCP包未被正确识别为CH9329命令

**检查点**:
- TCP包是否到达handle_host_control_frame()？
- tcp_payload_from_ipv4_frame()是否成功提取payload？
- ch9329_payload_valid()是否验证通过？

**调试方法**:
```c
// 在handle_host_control_frame()中添加调试日志
USB_DEBUG("HCF: TCP packet received, len=%d\r\n", frame_len);

// 在tcp_payload_from_ipv4_frame()中添加调试日志
USB_DEBUG("TCP payload extracted: len=%d\r\n", *payload_len);

// 在ch9329_payload_valid()中添加调试日志
USB_DEBUG("CH9329 frame valid: %d\r\n", result);
```

### 可能原因2: CH9329命令未被正确解析

**检查点**:
- CH9329_RxBuffer_Add()是否被调用？
- CH9329_DataParser()是否成功解析命令？
- 校验和计算是否正确？

**调试方法**:
```c
// 在CH9329_RxBuffer_Add()中添加调试日志
USB_DEBUG("CH9329_RxBuffer_Add: len=%d\r\n", len);

// 在CH9329_DataParser()中添加调试日志
USB_DEBUG("CH9329 frame: cmd=0x%02X, len=%d\r\n", cmd_code, data_len);

// 在校验和计算后添加调试日志
USB_DEBUG("Checksum: calculated=0x%02X, received=0x%02X\r\n", sum, recv_sum);
```

### 可能原因3: Flash写入失败

**检查点**:
- flash_erase_page()是否成功？
- flash_write_page()是否成功？
- 读回验证是否通过？

**调试方法**:
```c
// 在ECM_IPConfig_Set()中添加调试日志
USB_DEBUG("Flash erase: %s\r\n", ok ? "OK" : "FAIL");
USB_DEBUG("Flash write: %s\r\n", ok ? "OK" : "FAIL");
USB_DEBUG("Flash verify: %s\r\n", ok ? "OK" : "FAIL");
```

### 可能原因4: 运行时IP变量未更新

**检查点**:
- ECM_Bridge_UpdateIPConfig()是否被调用？
- 全局变量ecm_host_ip等是否真的更新了？

**调试方法**:
```c
// 在ECM_Bridge_UpdateIPConfig()中添加调试日志
USB_DEBUG("ECM IP config updated:\r\n");
USB_DEBUG("  Host IP: %d.%d.%d.%d\r\n", ecm_host_ip[0], ecm_host_ip[1], ecm_host_ip[2], ecm_host_ip[3]);
USB_DEBUG("  Dev IP: %d.%d.%d.%d\r\n", ecm_dev_ip[0], ecm_dev_ip[1], ecm_dev_ip[2], ecm_dev_ip[3]);
USB_DEBUG("  Target Host IP: %d.%d.%d.%d\r\n", ecm_target_host_ip[0], ecm_target_host_ip[1], ecm_target_host_ip[2], ecm_target_host_ip[3]);
USB_DEBUG("  Target Dev IP: %d.%d.%d.%d\r\n", ecm_target_dev_ip[0], ecm_target_dev_ip[1], ecm_target_dev_ip[2], ecm_target_dev_ip[3]);
```

### 可能原因5: 网络栈仍使用旧IP

**检查点**:
- ARP响应是否使用新的IP地址？
- ICMP响应是否使用新的IP地址？
- DHCP响应是否使用新的IP地址？

**分析**:
在ecm_bridge.c中，ARP、ICMP、DHCP响应都使用全局变量ecm_dev_ip、ecm_target_dev_ip等。如果这些变量更新了，响应应该使用新IP。

但是，**Mac的ARP cache可能仍然缓存了旧IP到MAC的映射**。这可能导致：
- Mac发送包到旧IP时，使用旧的MAC地址
- 设备收到包后，发现目标IP不匹配，丢弃包

**解决方案**:
发送Gratuitous ARP (GARP) 包，通知Mac更新ARP cache。

## 建议的调试步骤

1. **启用USB_DEBUG输出**
   - 重新编译固件，启用USB_DEBUG
   - 查看串口输出，确认命令处理流程

2. **检查命令是否到达**
   - 在handle_host_control_frame()中添加日志
   - 确认TCP包被正确识别

3. **检查CH9329解析**
   - 在CH9329_DataParser()中添加日志
   - 确认命令被正确解析

4. **检查Flash写入**
   - 在ECM_IPConfig_Set()中添加日志
   - 确认Flash写入成功

5. **检查IP变量更新**
   - 在ECM_Bridge_UpdateIPConfig()中添加日志
   - 确认IP变量已更新

6. **测试ARP响应**
   - 发送ARP请求到新IP
   - 检查是否收到正确的ARP响应

## 下一步行动

1. 添加详细的调试日志到关键函数
2. 重新编译并烧录固件
3. 发送SET_ECM_IP_CONFIG命令
4. 查看串口输出，定位问题所在
5. 根据日志结果修复问题

## 临时解决方案

如果问题无法快速解决，可以考虑：

1. **要求用户重新插拔USB**
   - 这会触发USB重新枚举
   - ECM_Init()会被调用，从Flash重新加载IP配置
   - 新IP配置会生效

2. **发送GARP包**
   - 在ECM_Bridge_UpdateIPConfig()中发送GARP包
   - 通知Mac更新ARP cache
   - 这可能解决旧IP仍然可访问的问题
