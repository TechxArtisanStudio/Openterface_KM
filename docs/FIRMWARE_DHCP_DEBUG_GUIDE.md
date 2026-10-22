# 固件 DHCP 服务器调试指南

## 问题描述

### 现象
- Bot 设备通过以太网连接到 Openterface 的 Target 端口
- Bot 设备配置为 DHCP 模式（自动获取 IP）
- Bot 设备无法获取 IP 地址，日志显示 "no lease"
- 手动配置静态 IP 192.168.11.2 后可以正常通信

### 预期行为
固件应该实现 DHCP 服务器，为 Target 设备自动分配 IP 地址：
1. Bot 设备发送 DHCP Discover
2. Openterface 固件响应 DHCP Offer（IP: 192.168.11.2）
3. Bot 设备发送 DHCP Request
4. Openterface 固件响应 DHCP ACK

### 实际行为
- Bot 设备发送 DHCP Discover
- 固件未响应 DHCP Offer
- Bot 设备超时失败

## 代码分析

### 关键文件

#### 1. APP/ecm_bridge.c
- **handle_target_dhcp_packet()** (第 705-1100 行)
  - 处理来自 Target PC 的 DHCP Discover/Request
  - 分配 IP 192.168.11.2
  - 构建 DHCP Offer/ACK 响应

- **handle_target_frame()** (第 1763-1780 行)
  - 处理从 Target 端接收到的以太网帧
  - 调用 handle_target_dhcp_packet() 处理 DHCP 请求

- **ECM_Bridge_TargetRxDone()** (第 1754-1761 行)
  - Target 端数据接收完成回调
  - 将数据传递给 handle_target_frame()

#### 2. APP/include/usb_work_mode.h
- **USB_WORK_MODE_COMPILES_TARGET_CDC_ECM** (第 56 行)
  - 控制是否编译 Target CDC ECM 功能
  - 当前设置为：`(USB_WORK_MODE_HAS_TARGET_CDC_ECM || USB_WORK_MODE_RUNTIME_PROFILES)`
  - 对于 USB_WORK_MODE=6 (HID_ACM_ECM)，此宏应该为真

#### 3. APP/include/ecm_ip_config.h
- **ECM_IP_CONFIG_DEFAULT_TARGET_HOST_IP** (第 32 行)
  - 默认值：{ 192, 168, 11, 2 }
  - 这是分配给 Target PC 的 IP 地址

### 数据流

```
Bot 设备 (DHCP Discover)
    ↓
Target 端口 (物理以太网)
    ↓
USBFS 设备栈 (EP6 OUT)
    ↓
ECM_Bridge_TargetRxDone()
    ↓
handle_target_frame()
    ↓
handle_target_dhcp_packet()
    ↓
构建 DHCP Offer
    ↓
ECM_Bridge_SendRawFrame() 或类似函数
    ↓
USBFS 设备栈 (EP7 IN)
    ↓
Target 端口
    ↓
Bot 设备
```

## 调试步骤

### 步骤 1：验证编译配置

检查固件是否正确编译了 Target CDC ECM 功能：

```bash
# 检查编译时的 USB_WORK_MODE
grep -n "USB_WORK_MODE" build/makefile

# 检查是否定义了 USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
grep -n "USB_WORK_MODE_COMPILES_TARGET_CDC_ECM" APP/include/usb_work_mode.h

# 检查实际编译的值
# 在 APP/ecm_bridge.c 中添加调试代码：
#if USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
    USB_DEBUG("Target CDC ECM: ENABLED\r\n");
#else
    USB_DEBUG("Target CDC ECM: DISABLED\r\n");
#endif
```

**预期输出**: "Target CDC ECM: ENABLED"

### 步骤 2：验证数据接收

检查 Target 端是否正确接收到 DHCP 请求：

```c
// 在 APP/ecm_bridge.c 的 ECM_Bridge_TargetRxDone() 函数中添加：
void ECM_Bridge_TargetRxDone(const uint8_t *data, uint16_t len)
{
    USB_DEBUG("Target RX: len=%d\r\n", len);
    if (len >= 14) {
        USB_DEBUG("  Dst MAC: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                  data[0], data[1], data[2], data[3], data[4], data[5]);
        USB_DEBUG("  Src MAC: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                  data[6], data[7], data[8], data[9], data[10], data[11]);
        USB_DEBUG("  EthType: 0x%02x%02x\r\n", data[12], data[13]);
    }
    
    // ... 原有代码
}
```

**预期输出**: 应该看到 DHCP 请求的数据包（EthType: 0x0800，协议: UDP，端口: 67）

### 步骤 3：验证 DHCP 处理函数

检查 handle_target_dhcp_packet() 是否被调用：

```c
// 在 APP/ecm_bridge.c 的 handle_target_frame() 函数中修改：
static uint8_t handle_target_frame(const uint8_t *frame, uint16_t frame_len)
{
    USB_DEBUG("Target frame: len=%d\r\n", frame_len);
    
    /* Handle DHCP requests from Target PC first */
    if (handle_target_dhcp_packet(frame, frame_len)) {
        USB_DEBUG("DHCP response queued\r\n");
        return 1;
    }
    
    // ... 原有代码
}
```

**预期输出**: 应该看到 "DHCP response queued"

### 步骤 4：验证 DHCP 请求解析

检查 DHCP 请求是否正确解析：

```c
// 在 APP/ecm_bridge.c 的 handle_target_dhcp_packet() 函数中添加：
static uint8_t handle_target_dhcp_packet(const uint8_t *frame, uint16_t frame_len)
{
    USB_DEBUG("DHCP handler entered, frame_len=%d\r\n", frame_len);
    
    // 在解析 Ethernet header 后：
    USB_DEBUG("  EthType: 0x%04X\r\n", eth_type);
    
    // 在解析 IP header 后：
    USB_DEBUG("  IP proto: %d\r\n", ip[9]);
    
    // 在解析 UDP header 后：
    USB_DEBUG("  UDP dst port: %d\r\n", get_be16(&udp[2]));
    
    // 在解析 DHCP message type 后：
    USB_DEBUG("  DHCP msg type: %d\r\n", dhcp_msg_type);
    
    // ... 原有代码
}
```

**预期输出**: 
- EthType: 0x0800
- IP proto: 17 (UDP)
- UDP dst port: 67
- DHCP msg type: 1 (Discover) 或 3 (Request)

### 步骤 5：验证 DHCP 响应构建

检查 DHCP 响应是否正确构建：

```c
// 在 handle_target_dhcp_packet() 的响应构建部分添加：
USB_DEBUG("Building DHCP response:\r\n");
USB_DEBUG("  yiaddr: %d.%d.%d.%d\r\n", 
          reply[yiaddr_offset], reply[yiaddr_offset+1], 
          reply[yiaddr_offset+2], reply[yiaddr_offset+3]);
USB_DEBUG("  dst IP: %d.%d.%d.%d\r\n",
          reply[dst_ip_offset], reply[dst_ip_offset+1],
          reply[dst_ip_offset+2], reply[dst_ip_offset+3]);
```

**预期输出**: 
- yiaddr: 192.168.11.2
- dst IP: 255.255.255.255 (Discover) 或 192.168.11.2 (Request)

### 步骤 6：验证响应发送

检查 DHCP 响应是否正确发送：

```c
// 在 handle_target_dhcp_packet() 返回前添加：
USB_DEBUG("DHCP response ready, len=%d\r\n", reply_len);

// 在 ECM_Bridge_SendRawFrame() 或类似函数中添加：
void ECM_Bridge_SendRawFrame(const uint8_t *data, uint16_t len)
{
    USB_DEBUG("Sending raw frame: len=%d\r\n", len);
    // ... 原有代码
}
```

**预期输出**: 应该看到 "Sending raw frame" 和正确的长度

### 步骤 7：验证 USBFS 端点配置

检查 Target 端的 USBFS 端点是否正确配置：

```c
// 在 APP/ch32v20x_usbfs_device.c 的 USBFS_Device_Endp_Init() 中添加：
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    USB_DEBUG("USBFS endpoints:\r\n");
    USB_DEBUG("  EP3: ECM data OUT (RX)\r\n");
    USB_DEBUG("  EP4: ECMdata IN (TX)\r\n");
    USB_DEBUG("  EP6: ACMdata OUT (RX)\r\n");
    USB_DEBUG("  EP7: ACMdata IN (TX)\r\n");
#endif
```

**预期输出**: 端点配置正确

### 步骤 8：验证 MAC 地址

检查 Target 端的 MAC 地址配置：

```c
// 在 APP/ecm_bridge.c 中检查：
USB_DEBUG("Target device MAC: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
          ECM_TARGET_DEV_MAC[0], ECM_TARGET_DEV_MAC[1], ECM_TARGET_DEV_MAC[2],
          ECM_TARGET_DEV_MAC[3], ECM_TARGET_DEV_MAC[4], ECM_TARGET_DEV_MAC[5]);
```

**预期输出**: 02:00:00:00:03:00（或其他配置的 MAC）

## 可能的原因

### 原因 1：Target CDC ECM 未编译
- **症状**: USB_DEBUG 显示 "Target CDC ECM: DISABLED"
- **解决**: 检查 USB_WORK_MODE 配置，确保为 6 (HID_ACM_ECM)

### 原因 2：数据未到达 Target 端
- **症状**: ECM_Bridge_TargetRxDone() 未被调用
- **解决**: 检查 USBFS 端点配置，确保 EP6 OUT 正确配置

### 原因 3：DHCP 请求未正确解析
- **症状**: handle_target_dhcp_packet() 返回 0
- **解决**: 检查以太网帧格式，确保符合 DHCP 规范

### 原因 4：DHCP 响应未正确构建
- **症状**: DHCP 响应数据错误
- **解决**: 检查 DHCP 响应构建逻辑，确保字段正确

### 原因 5：DHCP 响应未发送
- **症状**: 响应构建成功但未发送
- **解决**: 检查 ECM_Bridge_SendRawFrame() 或类似函数

### 原因 6：USBFS 端点配置错误
- **症状**: 端点未正确初始化
- **解决**: 检查 USBFS_Device_Endp_Init() 中的端点配置

### 原因 7：MAC 地址不匹配
- **症状**: DHCP 请求被过滤
- **解决**: 检查 MAC 地址过滤逻辑

## 修复方案

### 方案 A：启用调试日志

在 APP/ecm_bridge.c 中添加详细的调试日志：

```c
#define USB_DEBUG_ENABLED 1

#if USB_DEBUG_ENABLED
#define USB_DEBUG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define USB_DEBUG(fmt, ...) ((void)0)
#endif
```

然后在关键位置添加 USB_DEBUG() 调用。

### 方案 B：检查数据包流

使用 USB 分析仪或逻辑分析仪捕获 Target 端的数据包：
1. 检查 Bot 设备是否发送了 DHCP Discover
2. 检查 Openterface 设备是否接收到数据包
3. 检查 Openterface 设备是否发送了 DHCP Offer
4. 检查 Bot 设备是否接收到 DHCP Offer

### 方案 C：手动测试 DHCP 响应

临时修改代码，强制发送 DHCP 响应：

```c
// 在 ECM_Bridge_Init() 中添加测试代码：
void ECM_Bridge_Init(void)
{
    // ... 原有代码
    
    // 测试：发送一个假的 DHCP Offer
    uint8_t test_dhcp_offer[300];
    // ... 构建测试 DHCP Offer
    ECM_Bridge_SendRawFrame(test_dhcp_offer, sizeof(test_dhcp_offer));
}
```

### 方案 D：检查固件版本

确保使用最新版本的固件：

```bash
# 检查固件版本
python3 check_firmware_version.py

# 如果需要，重新编译固件
cd build
make clean
make USB_WORK_MODE=6
```

## 验证步骤

修复后，验证 DHCP 服务器是否正常工作：

1. **重新编译固件**
   ```bash
   cd build
   make clean
   make USB_WORK_MODE=6
   ```

2. **烧录固件**
   ```bash
   # 使用适当的烧录工具
   ```

3. **重启 Bot 设备**
   ```bash
   # 在 Bot 设备上重启网络
   sudo systemctl restart NetworkManager
   ```

4. **检查 Bot 设备日志**
   ```bash
   journalctl -u NetworkManager --since "1 minute ago" | grep dhcp
   ```

5. **验证 IP 地址**
   ```bash
   ip addr show enp0s20f0u4u1i4
   # 应该显示 inet 192.168.11.2/24
   ```

6. **测试连接**
   ```bash
   ping 192.168.11.3  # Openterface 设备
   ping 192.168.11.1  # 主机
   ```

## 参考文档

- RFC 2131: Dynamic Host Configuration Protocol
- RFC 2132: DHCP Options and BOOTP Vendor Extensions
- CH32V20x USBFS 设备控制器用户手册
- CDC ECM 类规范

## 附录

### A. DHCP 消息类型

| 类型 | 值 | 描述 |
|------|-----|------|
| DHCP Discover | 1 | 客户端发现 DHCP 服务器 |
| DHCP Offer | 2 | 服务器响应 Discover |
| DHCP Request | 3 | 客户端请求 IP |
| DHCP Decline | 4 | 客户端拒绝 IP |
| DHCP ACK | 5 | 服务器确认 Request |
| DHCP NAK | 6 | 服务器拒绝 Request |
| DHCP Release | 7 | 客户端释放 IP |
| DHCP Inform | 8 | 客户端请求配置参数 |

### B. DHCP 选项

| 选项 | 值 | 描述 |
|------|-----|------|
| Subnet Mask | 1 | 子网掩码 |
| Router | 3 | 默认网关 |
| DNS Server | 6 | DNS 服务器 |
| Domain Name | 15 | 域名 |
| Requested IP | 50 | 请求的 IP 地址 |
| Lease Time | 51 | IP 租期 |
| Message Type | 53 | DHCP 消息类型 |
| Server ID | 54 | DHCP 服务器标识 |
| End | 255 | 选项结束 |

### C. 网络拓扑

```
┌─────────────┐
│   Host PC   │
│ 192.168.11.1│
└──────┬──────┘
       │ USB CDC ECM (en13)
       │
┌──────┴──────┐
│ Openterface │
│   Device    │
│ 192.168.11.3│
└──────┬──────┘
       │ Target Port (Ethernet)
       │
┌──────┴──────┐
│  Bot Device │
│ 192.168.11.2│ (期望)
└─────────────┘
```

### D. 关键变量

| 变量 | 位置 | 值 | 描述 |
|------|------|-----|------|
| ecm_host_ip | ecm_bridge.c | 192.168.11.1 | 主机 IP |
| ecm_dev_ip | ecm_bridge.c | 192.168.11.3 | 设备 IP |
| ecm_target_host_ip | ecm_bridge.c | 192.168.11.2 | Target PC IP |
| ecm_target_dev_ip | ecm_bridge.c | 192.168.11.5 | 设备 Target 侧 IP |
| ECM_TARGET_DEV_MAC | ecm_bridge.c | 02:00:00:00:03:00 | Target 设备 MAC |
| ECM_TARGET_HOST_MAC | ecm_bridge.c | 02:00:00:00:00:20 | Target PC MAC |

---

**文档版本**: 1.0  
**创建日期**: 2026-10-20  
**最后更新**: 2026-10-20  
**作者**: Claude Code
