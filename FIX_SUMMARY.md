# USB 接口枚举问题修复总结

## 问题描述
Windows 只识别到 2 个接口（MI_00 和 MI_02），而不是预期的 6 个接口：
- ❌ 缺少 HID 接口（键盘/鼠标）
- ❌ 缺少 CDC ACM Data 接口
- ❌ 缺少 CDC ECM 接口
- ❌ ECM 网络适配器未出现

## 根本原因分析

### 1. 固件二进制文件分析
通过十六进制转储分析固件二进制文件，发现：
- ✅ 设备描述符正确（bDeviceClass=0xEF，支持 IAD）
- ✅ 配置描述符正确（wTotalLength=197，bNumInterfaces=6）
- ✅ 所有 6 个接口的描述符都完整存在

### 2. 端点初始化代码问题
在 `APP/ch32v20x_usbfs_device.c` 的 `USBFS_Device_Endp_Init()` 函数中发现：

**问题代码：**
```c
#if defined(ENABLE_UAC) || USB_WORK_MODE_COMPILES_TARGET_CDC_ACM || USB_WORK_MODE_COMPILES_TARGET_CDC_ECM
    // 只检查了 TARGET CDC 端点
    // 缺少对 HOST CDC ACM 端点（EP5, EP6, EP7）的初始化
#endif
```

**缺失的初始化：**
- EP5: CDC ACM 中断 IN（Host→Target）
- EP6: CDC ACM 批量 OUT（Host→Target）
- EP7: CDC ACM 批量 IN（Host→Target）

这些端点用于 Host PC 的 CDC ACM 串口通信，但由于缺少初始化代码，端点未被启用，导致 Windows 无法正确枚举这些接口。

## 修复内容

### 修改文件
`APP/ch32v20x_usbfs_device.c`

### 修改位置
`USBFS_Device_Endp_Init()` 函数中的三处代码：

#### 1. 端点模式寄存器初始化
```c
#if defined(ENABLE_UAC) || USB_WORK_MODE_COMPILES_TARGET_CDC_ACM || USB_WORK_MODE_COMPILES_TARGET_CDC_ECM || USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    // 添加了对 USB_WORK_MODE_HAS_HOST_ACM_AND_ECM 的检查
    // 为 Host CDC ACM 启用 EP5, EP6, EP7
#endif
```

#### 2. DMA 地址初始化
```c
#if defined(ENABLE_UAC) || USB_WORK_MODE_COMPILES_TARGET_CDC_ACM || USB_WORK_MODE_COMPILES_TARGET_CDC_ECM || USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    // 为 Host CDC ACM 配置 DMA 缓冲区
    USBFSD->UEP5_DMA = (uint32_t)EP5_DatabufHD;
    USBFSD->UEP6_DMA = (uint32_t)EP6_DatabufHD;
    USBFSD->UEP7_DMA = (uint32_t)EP7_DatabufHD;
#endif
```

#### 3. 控制寄存器初始化
```c
#if defined(ENABLE_UAC) || USB_WORK_MODE_COMPILES_TARGET_CDC_ACM || USB_WORK_MODE_COMPILES_TARGET_CDC_ECM || USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    // 为 Host CDC ACM 配置端点控制寄存器
    USBFSD->UEP5_TX_LEN  = 0;
    USBFSD->UEP5_TX_CTRL = USBFS_UEP_T_RES_NAK;
    USBFSD->UEP6_RX_CTRL = USBFS_UEP_R_RES_ACK;
    USBFSD->UEP7_TX_LEN  = 0;
    USBFSD->UEP7_TX_CTRL = USBFS_UEP_T_RES_NAK;
#endif
```

## 编译结果
- ✅ 编译成功
- ✅ 固件大小：215752 字节（增加 48 字节）
- ✅ 无编译错误

## 接口和端点布局（修复后）

```
IF0: Keyboard HID     - EP1 IN (中断)
IF1: Mouse HID        - EP2 IN (中断)
IF2: CDC ACM Comm     - EP5 IN (中断通知)
IF3: CDC ACM Data     - EP6 OUT (批量), EP7 IN (批量)
IF4: CDC ECM Comm     - 无端点（通知功能禁用）
IF5: CDC ECM Data     - EP3 OUT (批量), EP4 IN (批量)
```

## 下一步测试

### 1. 烧录新固件
将 `output/openterface-km-firmware-kvmgo.bin` 烧录到设备

### 2. 验证 Windows 设备枚举
- 打开设备管理器
- 检查是否出现 6 个接口
- 验证 HID 设备（键盘/鼠标）是否被识别
- 验证 CDC ACM 串口是否出现
- 验证 CDC ECM 网络适配器是否出现

### 3. 测试 ECM IP 即时生效功能
```python
# 使用 test_ecm_simple.py 测试
python test_ecm_simple.py COM18
```

### 4. 预期结果
- ✅ 所有 6 个接口都应该被 Windows 正确识别
- ✅ HID 键盘/鼠标应该可以正常使用
- ✅ CDC ACM 串口应该可以正常通信
- ✅ CDC ECM 网络适配器应该出现
- ✅ ECM IP 即时生效功能继续正常工作

## 技术细节

### 关键宏定义
- `USB_WORK_MODE_HAS_HOST_ACM_AND_ECM`: 表示 Host PC 同时具有 CDC ACM 和 CDC ECM
- `USB_WORK_MODE_COMPILES_TARGET_CDC_ACM`: 表示编译了 Target CDC ACM 支持
- `USB_WORK_MODE_COMPILES_TARGET_CDC_ECM`: 表示编译了 Target CDC ECM 支持

### 端点分配原则
- **Target 端点**: 用于 MCU 与 Target PC 之间的通信
- **Host 端点**: 用于 Host PC 与 MCU 之间的通信
- 在组合模式下，需要同时初始化两组端点

## 总结
这是一个端点初始化遗漏的问题。虽然描述符正确定义了所有 6 个接口，但端点初始化代码只考虑了 Target CDC 端点，忽略了 Host CDC ACM 端点。修复后，所有端点都被正确启用，Windows 应该能够正确枚举所有接口。
