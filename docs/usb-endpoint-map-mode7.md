# Mode 7 端点分布图（HID + Host ACM + Host ECM + Target ECM）

本文描述 `USB_WORK_MODE=hid_acm_ecm_bridge`（mode 7）下的完整 USB 端点布局。
内容包括：两个控制器的端点分配、PMA/DMA 内存图、主机视角的接口映射、数据流，
以及修复前的异常点对照。

---

## 0. 拓扑

```
                    ┌───────────────────────────────┐
                    │        CH32V208 (mode 7)      │
  Mac / Host  ──────┤ USBLIB @0x40005C00 (512B PMA) │
  (Host 角色)       │   · CDC ACM                   │
                    │   · Host ECM                  │
                    │                               │
                    │ USBFS @0x50000000 (SRAM DMA)  │
  Target PC  ───────┤   · HID ×4                    │
  (Device 角色)     │   · Target ECM                │
                    └───────────────────────────────┘
```

两个控制器共享同一个物理 USB 口，但端点空间、缓冲方式和中断完全独立：

| 控制器 | 基址 | 缓冲 | 面向 |
|---|---|---|---|
| USBLIB（老库） | `0x40005C00`，IRQ 36 | 512B PMA @`0x40006000` | Mac / Host |
| USBFS（新库） | `0x50000000`，IRQ 59 | 普通 SRAM（`UEP_DMA + 0x20000000`） | Target PC |

关键原则：**Host ACM / Host ECM 只在 USBLIB 上，Target ECM 只在 USBFS 上**。
两套描述符都必须遵守这条边界，否则会出现重复接口和错误路由。

---

## 1. USBLIB 控制器（Mac 侧）

### 1.1 端点分配（修复后）

| EP | 方向 | 类型 | MPS | PMA 地址 | 绑定接口 | 说明 |
|---|---|---|---|---|---|---|
| EP0 | IN/OUT | Control | 64 | `0x040` / `0x080` | 全部 | 控制传输 |
| EP1 | IN | Interrupt | 16 | `0x0C0` | IF0 ACM comm | ACM 通知 |
| EP5 | OUT | Bulk | 64 | `0x0D0` | IF1 ACM data | Mac → 设备 |
| EP3 | IN | Bulk | 64 | `0x110` | IF1 ACM data | 设备 → Mac（CH9329 应答） |
| EP2 | IN | Interrupt | 16 | `0x150` | IF2 Host ECM comm | ECM 通知 |
| EP6 | OUT | Bulk | 32 | `0x180` | IF3 Host ECM data | Mac → 设备 |
| EP6 | IN | Bulk | 32 | `0x1A0` | IF3 Host ECM data | 设备 → Mac |

Host ECM 数据使用**单个双向端点 EP6**（OUT+IN 同号），与 Target 侧 ECM 的
EP7 双向端点对称；EP4 在 combined 模式下不再分配给 Host ECM。
Target ECM 不占 USBLIB 端点：它在 USBFS 上（EP5 通知 + EP7 双向数据）。

### 1.2 PMA 地址图（512B）

```
0x000 ┌─────────────────────────────┐
      │ BTABLE (8 EP × 8B = 64B)    │
0x040 │ EP0 RX              64B     │
0x080 │ EP0 TX              64B     │
0x0C0 │ EP1 TX   ACM 通知    16B    │
0x0D0 │ EP5 RX   ACM 数据↓   64B    │
0x110 │ EP3 TX   ACM 数据↑   64B    │
0x150 │ EP2 TX   HostECM 通知 16B   │
0x180 │ EP6 RX   HostECM ↓   32B    │
0x1A0 │ EP6 TX   HostECM ↑   32B    │
0x1C0 ├──────── 空闲 64B ────────────┤
0x200 └─────────────────────────────┘
```

总占用 `0x1C0 = 448B`。ACM 数据方向使用满速 64B（EP5/EP3），
Host ECM 方向按 32B 配置以节省 PMA（`0x160` 处的 EP4 TX 已不再使用）。

---

## 2. USBFS 控制器（Target PC 侧）

### 2.1 端点分配（修复后）

| EP | 方向 | 类型 | MPS | DMA 缓冲区 | 绑定接口 |
|---|---|---|---|---|---|
| EP0 | IN/OUT | Control | 64 | `USBFS_EP0_Buf[64]` | 全部 |
| EP1 | IN | Interrupt | 8 | `USBFS_EP1_Buf` | IF0 HID 键盘 |
| EP2 | IN | Interrupt | 8 | `USBFS_EP2_Buf` | IF1 HID 相对鼠标 |
| EP3 | IN | Interrupt | 8 | `USBFS_EP3_Buf` | IF2 HID 绝对鼠标 |
| EP4 | IN | Interrupt | 8 | `USBFS_EP4_Buf` | IF3 HID 多媒体 |
| EP5 | IN | Interrupt | 16 | `EP5_DatabufHD[128]` | IF4 Target ECM comm（通知） |
| EP7 | OUT | Bulk | 32 | `EP7_DatabufHD + 0` | IF5 Target ECM data |
| EP7 | IN | Bulk | 32 | `EP7_DatabufHD + 64` | IF5 Target ECM data |

### 2.2 端点模式寄存器

```
UEP4_1_MOD = EP1_TX_EN | EP4_TX_EN
UEP2_3_MOD = EP2_TX_EN | EP3_TX_EN
UEP5_6_MOD = EP5_TX_EN
UEP_MOD(3) = EP7_TX_EN | EP7_RX_EN
```

### 2.3 DMA 缓冲布局

USBFS 不使用 PMA，每个端点缓冲区位于普通 SRAM：

```
EPn_DatabufHD[128]
    +0   ┌───────────────┐
         │ RX 半区 (64B) │  ← 仅端点配置为 RX 时使用
    +64  ├───────────────┤
         │ TX 半区 (64B) │  ← TX_EN 且 RX_EN 同时置位、且未开 BUF_MOD 时固定写 +64
    +127 └───────────────┘
```

EP7 同时使能 TX+RX，所以 RX 在 `+0`、TX 在 `+64`。因此单端点缓冲必须 ≥ 96B，
当前 128B 满足。

---

## 3. 主机视角：接口 ↔ 端点映射

### 3.1 Mac 看到的样子（USBLIB，4 接口 / 154B）

```
IF0  CDC ACM 控制   ─ EP1 IN  16B (通知)
IF1  CDC ACM 数据   ─ EP5 OUT 64B / EP3 IN 64B
IF2  CDC ECM 控制   ─ EP2 IN  16B (通知)
IF3  CDC ECM 数据   ─ EP6 OUT 32B / EP6 IN 32B（单端点双向）
```

### 3.2 Target PC 看到的样子（USBFS，6 接口 / 181B）

```
IF0  HID 键盘       ─ EP1 IN 8B
IF1  HID 相对鼠标   ─ EP2 IN 8B
IF2  HID 绝对鼠标   ─ EP3 IN 8B
IF3  HID 多媒体     ─ EP4 IN 8B
IF4  CDC ECM 控制   ─ EP5 IN 16B (通知)
IF5  CDC ECM 数据   ─ alt0 空 / alt1: EP7 OUT 32B + EP7 IN 32B
```

CDC-ECM 控制接口必须恰好声明 1 个中断 IN 端点，`cdc_ether` 依赖它获取
`NETWORK_CONNECTION` 通知；没有通知端点时网卡会停留在 NO-CARRIER。

---

## 4. 数据流

```
Mac ──EP6 OUT──► ECM_Bridge_HostRxDone ──► g_host_to_target
                                              │
                                              ▼ poll_target_tx()
                                        USBFS EP7 IN ──► Target PC

Target PC ──USBFS EP7 OUT──► ECM_Bridge_TargetRxDone ──► g_target_to_host
                                                             │
                                                             ▼ poll_host_tx()
                                                   USBLIB EP6 IN ──► Mac
```

通知路径：

```
Target ECM link state ─► poll_target_notification() ─► USBFS EP5 IN (wIndex=IF4)
Host   ECM link state ─► poll_host_notification()   ─► USBLIB EP2 IN (wIndex=IF2)
```

---

## 5. 修复前的异常点（对照）

| # | 位置 | 异常 | 影响 |
|---|---|---|---|
| 1 | USBLIB EP7（IF4/5） | Target ECM 被复制到 Mac 侧 | 重复接口 + 数据回环 |
| 2 | USBLIB EP6 IN | 描述符改到 EP6 IN，但 PMA/发送代码从未跟上 | ACM 串口无应答 |
| 3 | USBLIB EP3 IN | 硬件有、描述符无 | 回包发到主机不读的端点 |
| 4 | USBLIB EP6 OUT / EP4 IN | 32B PMA 被按 64B 访问 | 槽位互相覆盖 |
| 5 | USBFS EP3 | 描述符 IN / 硬件配置为 RX | HID 绝对鼠标不可用 |
| 6 | USBFS EP5 | 已使能、未声明 | 通知发不出去 |
| 7 | USBFS EP6 | 已使能、未声明 | 脏数据进入 Target RX |
| 8 | USBFS IF4 | `bNumEndpoints=1` 但无端点描述符 | cdc_ether 绑定/通知失败 |
| 9 | USBFS EP7 | 32B 硬件 / 64B 发送 | ZLP 规则失效、帧粘连 |
| 10 | 静态 vs 运行时描述符 | HID 数量 2 vs 4、HID 偏移硬编码 | HID 枚举拿到垃圾数据 |
| 11 | `ecm_bridge.c` | `#ifdef USBFS_DEVICE` 永不成立 | Target 回包发到 Mac |
| 12 | `ecm_bridge.c` | Target 通知用 EP5，但 mode 7 无 EP5 | link 状态收不到 |
| 13 | `SET_INTERFACE` | 用 IF7 匹配实际 IF5 | alt=1 激活逻辑不执行 |
| 14 | 通知 wIndex | Host ECM 写 4，实际 IF2 | Mac 丢弃通知 |
| 15 | USBFS ECM 描述符选择 | 仅用 `HasHostCdcEcm` 判断，导致 mode 4/5 也套用 mode 7 的 EP7 块 | 描述符与 EP6/EP7 硬件配置不符 |

---

## 6. 待办（尚未完成）

- `GET_DESCRIPTOR(HID)` 仍按静态 `MyCfgDescr` 的固定偏移 18/43/68/93 取数据，
  与运行时 4 个 HID 接口的布局不一致。
- `docs/controllable-usb-endpoint-modes.md` 尚未补充 mode 6/7。
- mode 5（HID+UAC+ECM）运行时描述符把 ECM 端点整体 +2（EP6→EP8、EP7→EP9），
  超出 USBFS 的 EP0~EP7 范围，需要重新规划 UAC 与 ECM 的端点分配。

---

## 7. 修复落点

| 文件 | 修改内容 |
|---|---|
| `APP/usbd_desc.c` | Target ECM 控制接口补上 EP5 IN 通知端点（块 72→79B）；只有 mode 6/7 使用 EP5+EP7 块，mode 4/5 回到 EP5/EP6/EP7 经典布局 |
| `APP/ch32v20x_usbfs_device.c` | `SET_INTERFACE` 数据接口基准改为 IF5；USBFS 端点初始化按 HID EP1~EP4 + EP5 通知 + EP7 双向配置；删除 EP3 RX / EP6 残留配置与 `EP3_DatabufHD` |
| `APP/ecm_bridge.c` | Target 发送固定走 USBFS EP7、32B 分片与 32B ZLP 边界；Host 通知接口号改为 IF2 |
| `APP/km_ring.c` | 恢复按端点分发 HID 报告（EP1 键盘 8B / EP2 相对鼠标 4B / EP3 绝对鼠标 5~6B / EP4 多媒体 2/4B），去掉 Report ID 前缀 |
| `APP/USBLIB/CONFIG/usb_desc.c` | 删除重复的 Target ECM 描述符块；ACM 数据 IN 保持 EP3；Host ECM 数据改为 EP6 OUT `0x06` + EP6 IN `0x86`（32B 双向） |
| `APP/USBLIB/CONFIG/usb_conf.h` | PMA 布局收敛到 `0x1C0`；新增 `ENDP6_TX_ECM_ADDR (0x1A0)`；`ECM_USBD_BULK_IN_ENDP`/`_BUSY`/`EP_BULK_IN` 指向 EP6 / `USBD_Endp6_Busy` / `EP6_IN` |
| `APP/USBLIB/CONFIG/usb_prop.c` | Host ECM 用 EP6 双向：reset 与 `SET_INTERFACE(IF3 alt1)` 同时配置 EP6 TX/RX；删除 USBLIB EP7 初始化 |
| `APP/USBLIB/CONFIG/usb_endp.c` | 新增 `EP6_IN_Callback` 与 `USBD_Endp6_Busy`；`USBD_ENDPx_DataUp` 的 ECM bulk IN 分支从 EP4 改为 EP6；EP6 OUT 按 32B 处理 |
