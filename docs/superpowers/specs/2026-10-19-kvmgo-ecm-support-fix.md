# KVMGo ECM支持修复总结

## 问题描述
KVMGo使用`USB_WORK_MODE=hid_acm_ecm`编译后，设备无法响应任何串口命令，也没有ECM网络适配器出现。

## 根本原因
USBFS设备栈（KVMGo使用）的描述符文件`APP/usbd_desc.c`缺少对组合ACM+ECM模式的支持。该描述符原本只支持：
- 纯HID模式
- HID + CDC ACM模式
- HID + CDC ECM模式

但不支持HID + CDC ACM + CDC ECM组合模式。

## 解决方案
由于CH32V20x只有8个USB端点（EP0-EP7），而组合模式需要：
- 4个HID接口使用EP1-EP4
- CDC ACM需要EP5、EP6、EP7
- CDC ECM还需要额外的端点

这超出了端点数量限制。

**解决方案**：在组合模式下，减少HID接口数量从4个到2个（仅保留键盘和鼠标），释放EP3和EP4供ECM使用。

## 修改的文件

### 1. APP/usbd_desc.c
**修改内容**：
- 添加`USB_WORK_MODE_HAS_HOST_ACM_AND_ECM`条件编译支持
- 组合模式下仅包含键盘和鼠标HID接口（IF0-IF1）
- 添加CDC ACM接口（IF2-IF3）：
  - EP5 IN: 中断通知
  - EP6 OUT: 批量数据（主机→设备）
  - EP7 IN: 批量数据（设备→主机）
- 添加CDC ECM接口（IF4-IF5）：
  - EP3 OUT: 批量数据（主机→设备）
  - EP4 IN: 批量数据（设备→主机）
  - 无中断通知端点（为节省端点）
- 更新设备描述符使用IAD（Interface Association Descriptor）
- 更新配置描述符大小：197字节（0xC5）
- 更新接口数量：6个（2 HID + 2 ACM + 2 ECM）

### 2. APP/ecm_bridge.c
**修改内容**：
- 更新`ECM_BRIDGE_HOST_COMM_IF`定义
- USBFS组合模式下使用IF4作为ECM通信接口（而非IF2）
- 添加详细注释说明接口布局

### 3. APP/USBLIB/CONFIG/usb_conf.h
**修改内容**：
- 区分USBFS和USBLIB栈的端点配置
- USBFS组合模式：
  - ECM使用EP3（批量OUT）和EP4（批量IN）
  - 无中断端点
- USBLIB组合模式保持原有配置

## USB接口和端点布局

### 组合模式（USBFS栈 + KVMGo）
```
IF0: Keyboard HID     - EP1 IN (中断)
IF1: Mouse HID        - EP2 IN (中断)
IF2: CDC ACM Comm     - EP5 IN (中断通知)
IF3: CDC ACM Data     - EP6 OUT (批量), EP7 IN (批量)
IF4: CDC ECM Comm     - 无端点（通知功能禁用）
IF5: CDC ECM Data     - EP3 OUT (批量), EP4 IN (批量)
```

### 对比：标准模式（USBLIB栈 + KeyMod）
```
IF0: CDC ACM Comm     - EP1 IN (中断通知)
IF1: CDC ACM Data     - EP5 OUT (批量), EP3 IN (批量)
IF2: CDC ECM Comm     - EP2 IN (中断通知)
IF3: CDC ECM Data     - EP6 OUT (批量), EP4 IN (批量)
```

## 功能限制
由于端点数量限制，组合模式下：
1. ❌ 绝对鼠标/触摸屏功能不可用
2. ❌ 媒体控制功能不可用
3. ❌ ECM中断通知功能不可用（不影响数据传输）

保留的功能：
1. ✅ 键盘HID
2. ✅ 鼠标HID
3. ✅ CDC ACM串口（用于CH9329命令）
4. ✅ CDC ECM以太网（用于网络通信）
5. ✅ ECM IP即时生效功能

## 测试步骤

### 1. 烧录固件
```powershell
# 使用WCH-ISP工具烧录output\openterface-km-firmware-kvmgo.bin
```

### 2. 验证设备枚举
设备应该在Windows设备管理器中出现：
- 1个COM端口（CDC ACM）
- 1个网络适配器（CDC ECM）
- 键盘和鼠标HID设备

### 3. 测试串口通信
```python
python test_ecm_ip_immediate.py COMx
```

### 4. 验证ECM IP即时生效
1. 获取当前IP配置
2. 设置新的IP配置
3. 立即再次读取配置（无需USB重连）
4. 验证配置已更新

## 预期结果
- ✅ COM端口响应CH9329命令
- ✅ 可以读取和设置ECM IP配置
- ✅ IP修改后立即生效（无需USB重连）
- ✅ Windows显示ECM网络适配器
- ✅ 键盘和鼠标正常工作
- ❌ 绝对鼠标和媒体控制不可用（预期行为）

## 后续优化建议
1. 如果需要绝对鼠标和媒体控制功能，可以考虑：
   - 使用USB HS（高速）模式，可能有更多端点
   - 重新设计端点分配策略
   - 使用动态端点配置
2. 添加ECM中断通知支持（如果找到可用端点）
3. 优化描述符大小，减少固件占用

## 相关文件
- 设计文档：`docs/superpowers/specs/2026-10-19-ecm-ip-immediate-effect-design.md`
- 实施总结：`docs/superpowers/specs/2026-10-19-ecm-ip-immediate-effect-implementation-summary.md`
- 测试脚本：`test_ecm_ip_immediate.py`
