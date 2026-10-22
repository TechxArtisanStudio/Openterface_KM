# ECM DHCP 调试测试报告

**测试日期**: 2026-10-20  
**测试人员**:  
**固件版本**:  
**USB_WORK_MODE**: 6 (HID_ACM_ECM)

## 测试环境

- **Host PC**: macOS / Linux / Windows
- **Bot 设备**: (填写设备名称)
- **连接方式**: Bot → Target 端口 (以太网)

## 测试步骤

### 1. 编译固件

```bash
cd build
make clean
make USB_WORK_MODE=6
```

**编译结果**: ✅ 成功 / ❌ 失败

### 2. 烧录固件

使用烧录工具将固件烧录到设备。

**烧录结果**: ✅ 成功 / ❌ 失败

### 3. 连接设备并查看日志

连接 USB 串口查看调试日志（波特率：115200）。

### 4. 重启 Bot 设备网络

```bash
# Linux
sudo systemctl restart NetworkManager

# 或
sudo dhclient -r enp0s20f0u4u1i4
sudo dhclient enp0s20f0u4u1i4
```

## 预期日志输出

### 步骤 1：初始化日志

```
Target CDC ECM: ENABLED
USBFS endpoints configured:
  EP6: Target data OUT (RX)
  EP7: Target data IN (TX)
ECM init done
Network config:
  Host IP: 192.168.11.1
  Dev IP: 192.168.11.3
  Target Host IP: 192.168.11.2
  Target Dev IP: 192.168.11.5
Target device MAC: 02:00:00:00:03:00
```

**实际输出**: 
```
[粘贴日志]
```

**结果**: ✅ 符合预期 / ❌ 不符合预期

### 步骤 2：数据接收日志

当 Bot 设备发送 DHCP Discover 时，应该看到：

```
Target RX: len=300
  Dst MAC: ff:ff:ff:ff:ff:ff
  Src MAC: xx:xx:xx:xx:xx:xx
  EthType: 0x0800
```

**实际输出**: 
```
[粘贴日志]
```

**结果**: ✅ 看到日志 / ❌ 未看到日志

### 步骤 3：DHCP 处理日志

```
Target frame: len=300
Target DHCP handler entered, frame_len=300
  EthType: 0x0800
  IP proto: 17 (UDP)
  UDP dst port: 67
  DHCP msg type: 1 (Discover)
Target DHCP type=1 xid=0xXXXXXXXX
```

**实际输出**: 
```
[粘贴日志]
```

**结果**: ✅ 看到日志 / ❌ 未看到日志

### 步骤 4：DHCP 响应构建日志

```
Building DHCP response:
  yiaddr: 192.168.11.2
  dst IP: 255.255.255.255 (broadcast)
DHCP response ready, len=300
  Response type: OFFER
  Server ID: 192.168.11.5
  Queue push: OK
DHCP response queued
```

**实际输出**: 
```
[粘贴日志]
```

**结果**: ✅ 看到日志 / ❌ 未看到日志

## 验证 Bot 设备 IP

```bash
# 在 Bot 设备上执行
ip addr show enp0s20f0u4u1i4
```

**预期输出**: 
```
inet 192.168.11.2/24 ...
```

**实际输出**: 
```
[粘贴输出]
```

**结果**: ✅ 获取到 IP / ❌ 未获取到 IP

## 测试连接

```bash
# 从 Bot 设备 ping
ping 192.168.11.3  # Openterface 设备
ping 192.168.11.1  # Host PC
```

**结果**: 
- ping 192.168.11.3: ✅ 成功 / ❌ 失败
- ping 192.168.11.1: ✅ 成功 / ❌ 失败

## 问题分析

### 如果未看到 "Target RX" 日志

**可能原因**:
1. Target CDC ECM 未正确编译
2. USBFS 端点未正确配置
3. Bot 设备未发送 DHCP Discover

**解决方案**:
- 检查 `USB_WORK_MODE_COMPILES_TARGET_CDC_ECM` 是否为 1
- 检查 USBFS EP6/EP7 配置
- 使用 USB 分析仪捕获数据包

### 如果看到 "Target RX" 但未看到 "DHCP handler entered"

**可能原因**:
1. 以太网帧格式不正确
2. EtherType 不是 0x0800
3. 帧长度不足

**解决方案**:
- 检查 EthType 日志输出
- 检查帧长度

### 如果看到 "DHCP handler entered" 但未看到 "DHCP response ready"

**可能原因**:
1. DHCP 请求解析失败
2. UDP 端口不是 67
3. DHCP magic cookie 不正确

**解决方案**:
- 检查 UDP dst port 日志
- 检查 DHCP msg type 日志

### 如果看到 "DHCP response ready" 但 Bot 未获取 IP

**可能原因**:
1. DHCP 响应未正确发送
2. Bot 设备未收到 DHCP Offer
3. DHCP 响应格式不正确

**解决方案**:
- 检查 Queue push 结果
- 使用 USB 分析仪捕获响应包
- 检查 Bot 设备日志

## 总结

**测试结果**: ✅ DHCP 正常工作 / ❌ DHCP 仍有问题

**问题描述**: 
[如果有问题，详细描述]

**下一步**: 
[下一步计划]

---

**报告版本**: 1.0  
**创建日期**: 2026-10-20
