# KVMGo ECM支持关键修复 - EP3端点配置问题

## 问题描述
KVMGo使用`USB_WORK_MODE=hid_acm_ecm`编译后，虽然USB设备能够枚举（COM18出现），但设备完全不响应任何CH9329命令。

## 根本原因
USB描述符中的端点配置与USBFS控制器的实际端点初始化不匹配：

### 描述符配置（错误）
```
IF0: Keyboard HID     - EP1 IN (中断)
IF1: Mouse HID        - EP2 IN (中断)
IF2: CDC ACM Comm     - EP5 IN (中断通知)
IF3: CDC ACM Data     - EP6 OUT (批量), EP7 IN (批量)
IF4: CDC ECM Comm     - 无端点
IF5: CDC ECM Data     - EP3 OUT (批量), EP4 IN (批量)
```

### USBFS控制器初始化（原始代码）
```c
USBFSD->UEP2_3_MOD = USBFS_UEP2_TX_EN | USBFS_UEP3_TX_EN;
```
这把EP3配置为**TX**（设备→主机），但描述符中EP3是**OUT**（主机→设备），应该是**RX**！

## 解决方案

### 1. 修改端点方向配置
**文件**: `APP/ch32v20x_usbfs_device.c`
**函数**: `USBFS_Device_Endp_Init()`

```c
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    /* 组合ACM+ECM模式：
     * EP3: ECM数据OUT (RX) - 主机→设备
     * EP4: ECM数据IN (TX) - 设备→主机 */
    USBFSD->UEP2_3_MOD = USBFS_UEP2_TX_EN | USBFS_UEP3_RX_EN;
#else
    USBFSD->UEP2_3_MOD = USBFS_UEP2_TX_EN | USBFS_UEP3_TX_EN;
#endif
```

### 2. 添加EP3 OUT中断处理
**文件**: `APP/ch32v20x_usbfs_device.c`
**位置**: `USBFS_IRQHandler()` 中的 `USBFS_UIS_TOKEN_OUT` 处理

```c
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    case USBFS_UIS_TOKEN_OUT | DEF_UEP3:
        /* ECM主机侧数据OUT（主机→设备）- 组合ACM+ECM模式 */
        if( intst & USBFS_UIS_TOG_OK )
        {
            ECM_Bridge_HostRxDone( EP3_DatabufHD, USBFSD->RX_LEN );
            USBFSD->UEP3_RX_CTRL ^= USBFS_UEP_R_TOG;
        }
        USBFSD->UEP3_RX_CTRL = ( USBFSD->UEP3_RX_CTRL & ~USBFS_UEP_R_RES_MASK ) | USBFS_UEP_R_RES_ACK;
        break;
#endif
```

### 3. 配置EP3 RX初始状态
**文件**: `APP/ch32v20x_usbfs_device.c`

```c
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    USBFSD->UEP3_RX_CTRL = USBFS_UEP_R_RES_ACK;  /* ECM主机侧数据OUT */
#else
    USBFSD->UEP3_TX_CTRL = USBFS_UEP_T_RES_NAK;
#endif
```

### 4. 配置EP3 DMA缓冲区
**文件**: `APP/ch32v20x_usbfs_device.c`

```c
#if USB_WORK_MODE_HAS_HOST_ACM_AND_ECM
    USBFSD->UEP3_DMA = (uint32_t)EP3_DatabufHD;  /* ECM主机侧数据OUT */
#else
    USBFSD->UEP3_DMA = (uint32_t)USBFS_EP3_Buf;
#endif
```

## 修改的文件
1. `APP/usbd_desc.c` - 添加组合模式描述符支持（之前的修复）
2. `APP/ecm_bridge.c` - 更新ECM接口编号（之前的修复）
3. `APP/USBLIB/CONFIG/usb_conf.h` - 更新端点配置（之前的修复）
4. `APP/ch32v20x_usbfs_device.c` - **本次修复：EP3端点方向和中断处理**

## 关键技术点

### USB端点方向
- **TX_EN** = 设备 → 主机 (IN端点)
- **RX_EN** = 主机 → 设备 (OUT端点)

### CH32V20x USBFS端点限制
- EP0-EP7 共8个端点
- 每个端点可配置为TX、RX或双向
- 组合模式下需要精心分配端点

### 组合模式端点分配
```
EP0: 控制端点（双向）
EP1: 键盘HID IN (TX)
EP2: 鼠标HID IN (TX)
EP3: ECM数据OUT (RX) ← 关键修复点
EP4: ECM数据IN (TX)
EP5: ACM中断IN (TX)
EP6: ACM数据OUT (RX)
EP7: ACM数据IN (TX)
```

## 测试步骤

### 1. 烧录固件
使用WCH-ISP工具烧录：
```
E:\work\Openterface_KM\output\openterface-km-firmware-kvmgo.bin
```

### 2. 验证设备枚举
Windows设备管理器应显示：
- ✅ 1个COM端口（CDC ACM）
- ✅ 1个网络适配器（CDC ECM）
- ✅ 键盘HID设备
- ✅ 鼠标HID设备

### 3. 测试串口通信
```powershell
python test_ecm_ip_immediate.py COM18
```

### 4. 预期结果
- ✅ COM端口响应CH9329命令
- ✅ 可以读取和设置ECM IP配置
- ✅ IP修改后立即生效（无需USB重连）
- ✅ Windows显示ECM网络适配器
- ✅ 键盘和鼠标正常工作

## 经验教训

### 1. USB描述符与硬件配置必须一致
描述符中的端点方向（IN/OUT）必须与USBFS控制器的端点模式（TX/RX）匹配。

### 2. 中断处理必须完整
每个使用的端点都需要在中断处理函数中有相应的处理代码。

### 3. 调试技巧
- 使用USB协议分析仪查看实际的USB事务
- 检查Windows设备管理器中的设备状态
- 使用串口调试工具测试基本通信
- 检查固件大小和端点配置是否合理

## 相关文件
- 设计文档：`docs/superpowers/specs/2026-10-19-ecm-ip-immediate-effect-design.md`
- 实施总结：`docs/superpowers/specs/2026-10-19-ecm-ip-immediate-effect-implementation-summary.md`
- KVMGo ECM修复：`docs/superpowers/specs/2026-10-19-kvmgo-ecm-support-fix.md`
- 本次修复：`docs/superpowers/specs/2026-10-19-kvmgo-ecm-ep3-fix.md`

## 下一步
如果测试成功，ECM IP即时生效功能将完全可用。如果仍有问题，可能需要：
1. 检查ECM_Bridge_HostRxDone()函数实现
2. 验证数据包路由逻辑
3. 使用USB协议分析仪捕获实际事务
