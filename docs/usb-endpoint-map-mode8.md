# Mode 8 端点分布图（HID + Target CDC NCM）

本文描述 `USB_WORK_MODE=hid_ncm_bridge`（mode 8）下的完整 USB 端点布局。
内容包括：USBFS 控制器的端点分配、SRAM DMA 内存图、主机视角的接口映射、数据流。

---

## 0. 拓扑

```
                    ┌───────────────────────────────┐
                    │        CH32V208 (mode 8)      │
                    │                               │
                    │ USBFS @0x50000000 (SRAM DMA)  │
  Windows PC  ───────┤   · HID ×4                    │
  (Host 角色)       │   · Target CDC NCM            │
                    └───────────────────────────────┘
```

Mode 8 仅使用 USBFS 控制器（Target 侧），面向 Windows PC 作为 CDC NCM 网卡设备。
Host 侧（USBLIB 控制器）不使用。

---

## 1. USBFS 控制器（Target PC 侧 / Windows PC 看到的设备）

### 1.1 端点分配

| EP | 方向 | 类型 | MPS | DMA 缓冲区 | 绑定接口 | 说明 |
|---|---|---|---|---|---|---|
| EP0 | IN/OUT | Control | 64 | `USBFS_EP0_Buf[64]` | 全部 | 控制传输 |
| EP1 | IN | Interrupt | 8 | `USBFS_EP1_Buf` | IF0 HID 键盘 | |
| EP2 | IN | Interrupt | 8 | `USBFS_EP2_Buf` | IF1 HID 相对鼠标 | |
| EP3 | IN | Interrupt | 8 | `USBFS_EP3_Buf` | IF2 HID 绝对鼠标 | |
| EP4 | IN | Interrupt | 8 | `USBFS_EP4_Buf` | IF3 HID 多媒体 | |
| EP6 | IN | Interrupt | 16 | `EP6_DatabufHD[128]` | IF4 NCM comm | NCM 通知 |
| EP5 | OUT | Bulk | 64 | `EP5_DatabufHD[128]` | IF5 NCM data | Host → 设备 |
| EP7 | IN | Bulk | 64 | `EP7_DatabufHD[128]` | IF5 NCM data | 设备 → Host |

### 1.2 端点模式寄存器

```
UEP4_1_MOD = EP1_TX_EN | EP4_TX_EN
UEP2_3_MOD = EP2_TX_EN | EP3_TX_EN
UEP5_6_MOD = EP5_RX_EN | EP6_TX_EN
UEP_MOD(3) = EP7_TX_EN
```

### 1.3 DMA 缓冲布局

USBFS 不使用 PMA，每个端点缓冲区位于普通 SRAM：

```
EPn_DatabufHD[128]
    +0   ┌───────────────┐
         │ RX 半区 (64B) │  ← 端点配置为 RX 时使用
    +64  ├───────────────┤
         │ TX 半区 (64B) │  ← TX_EN 且 RX_EN 同时置位时固定写 +64
    +127 └───────────────┘
```

EP6/EP7 同时使能 TX+RX，所以 RX 在 `+0`、TX 在 `+64`。因此单端点缓冲必须 ≥ 96B，
当前 128B 满足。

---

## 2. 主机视角：接口 ↔ 端点映射

### 2.1 Windows 看到的样子（USBFS，6 接口）

```
IF0  HID 键盘       ─ EP1 IN 8B
IF1  HID 相对鼠标   ─ EP2 IN 8B
IF2  HID 绝对鼠标   ─ EP3 IN 8B
IF3  HID 多媒体     ─ EP4 IN 8B
IF4  CDC NCM 控制   ─ EP6 IN 16B (通知)
IF5  CDC NCM 数据   ─ alt0 空 / alt1: EP5 OUT 64B + EP7 IN 64B
```

CDC-NCM 控制接口必须声明 1 个中断 IN 端点（EP6），`usbncm.sys` 依赖它获取
`NETWORK_CONNECTION` 通知；没有通知端点时网卡会停留在 NO-CARRIER。

---

## 3. 数据流

```
Windows PC ──EP5 OUT──► NCM_Bridge_HostRxDone ──► g_host_tx_queue
                                                             │
                                                             ▼ NCM_Bridge_Poll()
                                                       USBFS EP7 IN ──► Windows PC

NCM_Bridge_HostRxDone():
  - 解析 NTB (Network Transfer Block)
  - 提取 Ethernet 帧
  - ARP/ICMP/DHCP 处理
  - 入队 g_host_tx_queue

NCM_Bridge_Poll():
  - 若 EP7 IN 空闲且 g_host_tx_queue 不空
  - Build NTB (单帧)
  - 发送至 USBFS EP7 IN
```

通知路径：

```
NCM link state ─► poll_host_notification() ─► USBFS EP6 IN (wIndex=IF4)
```

通知状态机：`DOWN (1) → UP (2) → SPEED (3) → BEACON (4)`
- DOWN: 发送 NETWORK_CONNECTION=0 (link down)
- UP: 发送 NETWORK_CONNECTION=1 (link up)
- SPEED: 发送 CONNECTION_SPEED_CHANGE (12 Mbps)
- BEACON: 周期性发送 NETWORK_CONNECTION=1

---

## 4. NCM 协议实现

### 4.1 NTB 结构

```
NTB Header (14 bytes):
  wBlockLength    (2 bytes) - Total NTB length (little-endian)
  wSequence       (2 bytes) - Sequence number
  wNdpIndex       (2 bytes) - Offset to NDP
  wNdpIndexAlt    (2 bytes) - 0 (no alternate NDP)
  Reserved        (4 bytes) - Zero

NDP (Network Datagram Pointer) - 16 bytes for single frame:
  dwSignature     (4 bytes) - 'NCM0'
  wLength         (2 bytes) - NDP length
  wNextNdpIndex   (2 bytes) - 0 (last)
  Datagram[0]:
    wDatagramIndex  (2 bytes) - Frame offset
    wDatagramLength (2 bytes) - Frame length
  Terminator: 0x0000, 0x0000
```

### 4.2 关键参数

| 参数 | 值 | 说明 |
|---|---|---|
| `wMaxSegmentSize` | 1514 | 标准以太网 MTU |
| `wNtbInMaxSize` | 16384 (0x4000) | 最大入站 NTB |
| `wNdpInMaxSize` | 1024 (0x0400) | 最大 NDP |
| `wNtbOutMaxSize` | 16384 (0x4000) | 最大出站 NTB |
| `bmNetworkCapabilities` | 0x0001 | BIT0=NET_ADDRESS |

### 4.3 类请求处理

| 请求 | Code | 处理 |
|---|---|---|
| GET_NTB_PARAMETERS | 0x80 | 返回 28 字节参数结构 |
| GET_NTB_INPUT_SIZE | 0x87 | 返回 wNtbInMaxSize |
| SET_NTB_INPUT_SIZE | 0x86 | 接受忽略 |
| GET_NTB_FORMAT | 0x89 | 返回 16-bit 格式 |
| SET_NTB_FORMAT | 0x88 | 仅支持 16-bit |
| GET_MAX_DATAGRAM | 0x8A | 返回 wMaxSegmentSize |
| SET_MAX_DATAGRAM | 0x8B | 接受忽略 |
| SET_PACKET_FILTER | 0x43 | 接受忽略 |

---

## 5. 关键代码位置

| 文件 | 相关函数/结构 |
|---|---|
| `APP/usbd_desc.c` | `USBFS_RuntimeNcmBlock`, `USBFS_BuildRuntimeConfigDescriptor()` |
| `APP/ncm_bridge.c` | `NCM_BuildNtb()`, `NCM_ParseNtb()`, `poll_host_notification()`, `NCM_Bridge_Poll()` |
| `APP/ch32v20x_usbfs_device.c` | USBFS 端点初始化、类请求处理 |
| `APP/USBLIB/CONFIG/usb_prop.c` | USBD 端点初始化 |
| `APP/USBLIB/CONFIG/usb_endp.c` | EP5 OUT / EP6 IN / EP7 IN 回调 |
| `APP/include/ncm_bridge.h` | API 声明 |
| `APP/include/usb_work_mode.h` | Mode 8 定义 |

---

## 6. 构建与测试

### 6.1 构建命令

```bash
export PATH="/home/bbot/tools/MRS-linux-x64/resources/app/resources/linux/components/WCH/Toolchain/RISC-V Embedded GCC/bin:$PATH"
make PRODUCT=keymod-plus USB_WORK_MODE=hid_ncm_bridge
```

### 6.2 烧录

```bash
python3 tools/ctrl_board.py flash build/openterface-km-firmware.bin
```

### 6.3 Windows 验证

1. 设备管理器应显示 "Remote NCM Compatible Device" 或 "CDC NCM" 类设备
2. 网络适配器应出现，自动获取 IP（DHCP）或使用静态 IP
3. `ping <device_ip>` 应能通
4. `ssh user@<device_ip>` 应能连接

### 6.4 Linux 验证

```bash
# 检查驱动绑定
lsusb -t | grep cdc_ncm
# 应显示: Driver=cdc_ncm

# 检查接口
ip link show enp0s21f0u6i4
# 应显示 UP 状态并有 IP

# 测试连通性
ping 192.168.100.1
ssh user@192.168.100.1
```

---

## 7. 已知问题

1. **Linux 内核描述符解析**: Linux `cdc_ncm` 驱动期望 NCM Functional Descriptor 的 subtype 为 `0x1A` (USB_CDC_NCM_TYPE)，而非规范定义的 `0x0D`。当前固件同时包含两个 descriptor (0x0D 和 0x1A) 以兼容。

2. **Carrier/Link-up**: 通知端点发送 NETWORK_CONNECTION=1，但接口可能保持 NO-CARRIER 状态，直到主机在 `IFF_UP` 后轮询通知端点。

3. **USBFS 控制器**: Mode 8 使用 USBFS 控制器（Target 侧），端点配置在 `ch32v20x_usbfs_device.c` 中完成。

## 8. 已解决问题

1. **描述符传输错误 (Nov 19, 2026)**: USBFS 控制器传输了错误的 NCM 描述符数据。
   - 根本原因: 添加静态 NCM 描述符代码后未重新编译固件
   - 修复:
     - 在 `usbd_desc.c` 中创建静态 `USBFS_StaticNcmConfigDescriptor`
     - 在 USBFS 中断处理器中添加直接引用 (`ch32v20x_usbfs_device.c`)
     - 使用 `USB_WORK_MODE=hid_ncm_bridge` 重新编译固件
   - 详细调试过程见 `docs/ncm-descriptor-debug.md`