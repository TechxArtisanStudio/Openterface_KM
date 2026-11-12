# KeyMod / KVM-GO BLE 配对安全设计方案

- 分支：`feature/ble-pairing-security`
- 基线：`main` @ `1ee6c00`
- 适用固件：KeyMod（`BOARD_VERSION=3`）、KVM-GO（`BOARD_VERSION=1`）等 `ENABLE_BLE` 机型
- 状态：**M1 + M2 + 按键长按已实现**（见 §12）；M3 只剩 App 侧工作；§14 的 P1/P1b/P3 仍需台架验
- 决策沿革：2026-11-03 定 PIN 模型与"无配对窗口"原则；2026-11-09 上限从 4 改为 **1**（用户："一台手机更安全"），同日决定上限**改为可配置项存 Flash**，单绑定由此从硬约束变成默认档位；2026-11-10 初始 PIN 定为**全设备统一 666666**（用户要求），并开始实现

---

## 1. 现状：配对链路是个空壳

bonding 参数名义上开着，实际任何路人手机都能操控设备。逐条钉住事实。

| 项 | 现状 | 位置 |
|---|---|---|
| 数据通路准入 | FFF2 值属性权限 `GATT_PERMIT_WRITE`，未配对/未加密即可写 → 直接操控 HID、CH9329 全命令、BLE-Eth 隧道 | [ble_usb_service.c:135-140](file:///Users/txa/project/Openterface_KM/Profile/ble_usb_service.c#L135-L140) |
| 订阅准入 | FFF1 CCCD 权限 `GATT_PERMIT_READ \| GATT_PERMIT_WRITE`，未加密即可订阅通知 | [ble_usb_service.c:121-126](file:///Users/txa/project/Openterface_KM/Profile/ble_usb_service.c#L121-L126) |
| passkey | 写死 `0`（"000000"），全网所有设备同一个值 | [peripheral.c:471-483](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L471-L483)（实现后：默认 `666666`，Flash 可改，见 §4） |
| Bond 回调 | `gapBondCBs_t` 两个成员全 NULL，应用对配对成功/失败毫无感知 | [peripheral.c:380-384](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L380-L384)（实现后：`pairStateCB` 已接入，见 §10） |
| `0x22` 安全命令 | 0x01/0x02/0x03 三个子命令全是 TODO 桩，返回假成功；0x03 恒报 0 bond | [peripheral.c:1294-1334](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L1294-L1334)（实现后：0x02/0x03/0x04/0x05/0x06，见 §8） |
| bond 满行为 | `GAPBOND_ERASE_AUTO` 应用未设置 → ROM 默认**开启**：第 N+1 台配对会静默清空全部旧 bond | [wchble_rom.h:1122](file:///Users/txa/project/Openterface_KM/LIB/wchble_rom.h#L1122) |
| 遗留死代码 | `SimpleProfile_AddService` 被调用，但函数体注册逻辑整段注释，是空函数。广告里的 `SIMPLEPROFILE_SERV_UUID`(0xFFF0) 恰与 `ble_usb` 服务同 UUID，属命名误导。**非攻击面**，仅清理 | [peripheral.c:489](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L489)、[gattprofile.c:294-313](file:///Users/txa/project/Openterface_KM/Profile/gattprofile.c#L294-L313) |

结论：BLE 侧写 FFF2 = 向目标机注入任意键盘输入，且无需任何凭据。这比隧道设计文档里记的 R1 风险更靠前——[ble-ip-tunnel-design.md:309](file:///Users/txa/project/Openterface_KM/docs/ble-ip-tunnel-design.md#L309) 说"IP 入口需要 bonded 会话"，但那时入口本身还是裸的。

## 2. 威胁模型

| 攻击者 | 当前 | 本方案后 |
|---|---|---|
| 路过扫码的任意手机 | 直接写 FFF2，完全控制 | 拦在协议栈层，写被拒 |
| 会操作 BLE 配对的人（无 PIN） | 用 "000000" 配对，获得持久访问 | 配对失败 |
| 知道初始 PIN 666666 的人 | 用 "000000" 配对 | **仍可配对**（槽空且未改 PIN 时），见 §6 的驱逐权衡 |
| 插得上 USB 的人 | 完全控制 | 完全控制——USB 是恢复路径，不设准入 |

主要威胁是第一、二行（无 PIN 路人），本方案解决它。第三行在未改 PIN 时等同于公开凭据，属可接受残留风险（首次配对后改 PIN 即消除），§6 给出 v2 补法。

## 3. 设计原则

1. **PIN = 信任根**。初始 6 位 PIN `666666`，可改为私有 PIN；`MITM=TRUE` + `GAPBOND_IO_CAP_DISPLAY_ONLY`。设备无屏无键，PIN 只从手机侧输入；设置时不回显，仅允许经 USB 或已加密 BLE 用 `0x22 0x07` 显式读回（2026-11-12 起，见 §8）——读回只对已绑定的现任手机开放，未加密链路拿不到。
2. **准入压在协议栈层**，不靠藏广播、不靠应用层判断。应用层判断只作纵深兜底（因为 ROM 行为不可读，见 P1）。
3. **UI 极简**：无配对窗口、无短按、无多色 LED。绑定槽空则随时可配，占用则拒绝。
4. **改安全参数分级授权**：绑定上限只走 USB（它决定访问模型）；改 PIN 可以走 USB 或已加密 BLE（现任手机自己）；已认证 BLE 链路还能解绑自己。
5. **失败可恢复**：清除 override 后 666666 永远可用（恢复码语义）；USB 通路始终可用。

## 4. 安全模型：初始 PIN 666666

### 4.1 PIN 生效值

```
effective_pin = has_pin_override ? override_pin : BLE_PAIR_DEFAULT_PIN (= 666666)
```

初始 PIN 全设备统一 `666666`（`BLE_PAIR_DEFAULT_PIN`，编译期常量，不占 Flash），启动时 `GAPBondMgr_SetParameter(GAPBOND_PERI_DEFAULT_PASSCODE /*0x407*/, 4, &effective_pin)`。不需要标签打印流程。

**这个选择的代价必须写清楚**：666666 是公开常量，任何知道本方案的人都能配对空槽设备。所以出厂状态的安全等级取决于"槽已被占用"——第一台手机绑上之后，新手机必须等 `0x22 0x02` 解绑。**安全使用的前提是首次配对后立刻用 `0x22 0x04` 改成私有 PIN**，App 应在首次连接成功后引导这一步。若坚持"拿到设备本体才能用"的占有语义，应改回每片唯一 PIN（原方案：`CRC32(MAC) % 1000000` 打印到标签）。

passcode 回调保持 NULL 是有意选择：`DISPLAY_ONLY` + 预设 passcode 时栈不需要应用参与，但此行为未经实测（P3）。

### 4.2 PIN 可改，666666 是恢复码

新增运行时 override（存 Flash，§9）。重置时清 override → 666666 重新生效。新 PIN 只从 USB 或已加密 BLE 链路下发，固件不回显。

## 5. Bond 策略：默认单绑定，上限可配置

### 5.1 判定式

全设备只有一条容量判定式，在三处刷新（开机后、每次配对成功后、SET 上限落盘后）：

```c
GAPBondMgr_GetParameter(GAPBOND_BOND_COUNT /*0x415*/, &cnt);
uint8_t mode = (cnt >= g_pair_cfg.max_bonds) ? GAPBOND_PAIRING_MODE_NO_PAIRING /*0x00*/
                                             : GAPBOND_PAIRING_MODE_WAIT_FOR_REQ /*0x01*/;
GAPBondMgr_SetParameter(GAPBOND_PERI_PAIRING_MODE /*0x400*/, 1, &mode);
```

配套参数：`GAPBOND_ERASE_AUTO(0x41E) = 0` **必须显式关闭**（默认开启 = 静默清空旧绑定，见 §1）；`GAPBOND_BOND_AUTO(0x418) = 1` 保持默认，bond 掉电保存。

### 5.2 上限可配置

| 项 | 值 |
|---|---|
| 默认 | **1**（一台手机，排他） |
| 可配范围 | 1..4，固件钳位。上界 4 有依据：`GATT_MAX_NUM_CONN = 4`（[wchble.h:647](file:///Users/txa/project/Openterface_KM/LIB/wchble.h#L647)），CCCCD 数组也开的 4 项 |
| 存储 | Flash 记录（§9），断电保持 |

ROM 栈自身的 bond 容量上限未公开（P2）。默认 1 意味着**这个未知量不再阻塞发版**——协议栈不可能支持少于 1 个 bond。只有用户主动调到 4 时才需要那组多手机实测。

### 5.3 上限的精确语义：只门控新配对，不驱逐存量

下调上限**不产生任何清理动作**。上限从 4 改回 1、已有 3 台绑定：这 3 台用已存 LTK 直接发起加密重连，不走 pairing 通道，不受 `NO_PAIRING` 影响（前提 P1b 需实测）；只是新配对被拒。

用户想要"上限即当前访问名单"，走显式 `0x22 0x02` 全擦再重绑。**固件不做隐式超额清理**——出问题难排查，且违反原则 3。

### 5.4 单绑定消掉的旧问题

- "手机 2-4 免手势添加"整个概念消失。
- "4 台全丢 bond 且满员 → 新手机锁死"边界消失：槽要么空要么占用，状态机两格。
- 单设备吊销的遗憾消失：槽只有一条，`GAPBOND_ERASE_ALLBONDS` 即吊销。`ERASE_SINGLEBOND(0x417)` 不需要（本来也无法枚举，公开 API 只给数量不给名单）。

## 6. 单绑定的代价：换机要显式交接，且有驱逐窗口

这是本方案唯一需要拍板的取舍，写清楚。

### 6.1 换手机流程（主路径，不需要 USB）

断开连接**不再释放槽位**——bond 是持久化的，断开只是断链。所以必须有显式交接：

1. 旧手机在 App 里点"解除本机绑定" → 在**自己已加密的链路**上发 `0x22 0x02`；
2. 固件擦除唯一那条 bond（就是这台手机自己），槽位空出；
3. 旧手机用户到系统蓝牙设置里"忽略此设备"（iOS 不做这步下次配对会异常，App 提示语必须带）；
4. 新手机用当前 PIN（出厂 666666）配对。

安全论证：已认证手机擦掉**唯一属于自己的**那条 bond 不构成新攻击面——它随时可以走开。所以 `0x22 0x02` 从"USB only"放开为"USB 或加密 BLE"。这是单绑定带来的规则放宽，多槽模型下不能这么放开（否则任一台可 DoS 全体）。

### 6.2 残留弱点：PIN 持有者可驱逐现任

单绑定 + 已知初始 PIN 的组合弱点：一旦槽位空（现任解绑/设备被 USB 重置），**任何持有 PIN 的人可以立即配对并独占**。4 槽模型下攻击者顶多占一个坑，现任不受影响；1 槽下是排他驱逐。

所以"一台更安全"成立于挡住无 PIN 的路人（主要威胁，成立），对持有 PIN 的第三方反而给了排他权。

**v2 补法（不进本版）**：配对成功后 App 在加密链路上协商一个设备本地生成的每手机所有权密钥，存 App/钥匙串；此后新手机要成为"主人"必须由现任签名移交。没有这套，就把 PIN 当封条管理。

## 7. 数据通路准入（方案承重部分）

两层，缺一不可。这是整个方案里真正挡攻击者的部分。

| 层 | 改动 | 位置 |
|---|---|---|
| ① GATT 权限 | FFF2 值：`GATT_PERMIT_WRITE` → `GATT_PERMIT_WRITE \| GATT_PERMIT_ENCRYPT_WRITE`(0x80)；CCCD 追加 `GATT_PERMIT_ENCRYPT_WRITE` | [ble_usb_service.c:135-140](file:///Users/txa/project/Openterface_KM/Profile/ble_usb_service.c#L135-L140)、[:121-126](file:///Users/txa/project/Openterface_KM/Profile/ble_usb_service.c#L121-L126)，位定义在 [wchble.h:604](file:///Users/txa/project/Openterface_KM/LIB/wchble.h#L604) |
| ② 应用兜底 | `ble_usb_ServiceEvt` 的 `BLE_USB_EVT_BLE_DATA_RECIEVED` 分支首行：`if(!linkDB_State(connection_handle, LINK_ENCRYPTED)) return;` | [peripheral.c:1107](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L1099-L1158)，API 在 [wchble_rom.h:3120](file:///Users/txa/project/Openterface_KM/LIB/wchble_rom.h#L3120)、`LINK_ENCRYPTED=0x10` 在 [:270](file:///Users/txa/project/Openterface_KM/LIB/wchble_rom.h#L270) |

**为什么两层都要**：①的强制由 ROM 内 ATT 层完成，我们无法读源码确认它真的检查了 encryption 位（P1）。②是自己可控的确定闸门。①单独存在可能是空的，②单独存在则每次写都要查库——两个都留着。

未加密写的预期错误码：`ATT_ERR_INSUFFICIENT_ENCRYPT` = **0x0F**（[wchble.h:565](file:///Users/txa/project/Openterface_KM/LIB/wchble.h#L565)）。注意不是 0x08 `INSUFFICIENT_AUTHOR`——那是 signed write 用的。

USB 侧（ACM/CH9329 串行通路）**不加**准入：插线本身即占有证明，且必须保留它作为"BLE 把自己锁死"时的恢复路径。

### 7.1 连接后立即请求加密（鼠标延迟修复，2026-11-10）

M2 合入后用户反馈鼠标出现 1–2 s 延迟。根因不在报文通路（MS_REL 队列 64 条 drop-oldest、主循环每轮排空，USB EP 忙等上限 20 ms，BLE 发送 1 包/连接事件 ≈ 12.5 ms，都堆不出秒级），而在**准入闸门的上游**：

1. 固件原先从不主动发起安全流程，链路在 host 第一次访问受保护属性之前一直是明文；
2. 首连时 App 的 CCCD/FFF2 写被 ATT 以 0x0F 拒绝；
3. iOS 对失败的 CCCD 写**不重试**，App 自己 0.5 s 后只重订阅一次，且 `connectionState` 未到 `.connected` 前 `sendTouchData` 全部丢弃。

于是每次（重）连都有 0.5–2 s 的指针死窗口，正好是用户感知到的延迟。修法是让从机在链路建立后主动发起加密（Slave Security Request）：

- `Peripheral_LinkEstablished` 里 `tmos_start_task(Peripheral_TaskID, SBP_SECURITY_REQ_EVT, 4)`（4 tick ≈ 2.5 ms，等连接事件稳定），[peripheral.c:947](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L947)；
- 事件处理调 `GAPBondMgr_PeriSecurityReq(handle)`，仅在已连接且未 `LINK_ENCRYPTED` 时；已绑定链路在一个连接事件内加密，首配链路立即弹 PIN 输入，[peripheral.c:825-835](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L825-L835)；
- `Peripheral_LinkTerminated` 里 `tmos_stop_task` 防止残留事件在下一链路误触发，[peripheral.c:979](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L979)。

副作用：加密时机从"host 首次写"提前到"连上即做"，**P1b（NO_PAIRING 是否拦已绑定链路加密）从边缘前提变成必测**——满员时老手机重连必须仍能加密成功，T4 覆盖此路径。

死代码清理：删 `SimpleProfile_AddService` 调用及 `gattprofile.c` 里对应的注释块。这是卫生问题，不影响安全（§1 已说明注册体是注释掉的）。

### 7.2 持续拖动时的延迟：HID 报文 ACK + 链路预算（2026-11-11）

§7.1 消除的是**连接瞬间**的死窗口。用户复查 iOS 客户端：连上后开头不再卡，但**持续拖动**时指针仍落后手指 1–2 s，且越拖越远。这是稳态问题，与配对准入无关（`18b3b8c` 对热路径只加了权限位和一次 `linkDB_State`），三层叠加：

1. **每个 HID 报文都被 ACK**：`ble_usb_ServiceEvt` 对 BLE 来的 CH9329 命令统一开 `g_ble_ch9329_response`（[peripheral.c:1269](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L1269)），鼠标/键盘 case 无条件 `resp_mode=1`（[usbd_compostie_km.c:286](file:///Users/txa/project/Openterface_KM/APP/usbd_compostie_km.c#L286)）→ 每个移动 = 上行 11 B + 下行 7 B 通知。
2. **出向预算只有 1 包/连接事件**：`BLE_TX_NUM_EVENT=1`、控制器缓存 5 包**收发共享**（[wchble.h:87-93](file:///Users/txa/project/Openterface_KM/LIB/wchble.h#L87-L93)）；固件申请 12.5 ms 被 iOS 拒绝（App 类配件下限 15 ms，实测多落 30 ms）→ 33–66 包/s，ACK 把预算吃满。
3. **App 每个触摸采样发一包**：不合流、不查 `canSendWriteWithoutResponse`（[BLEManager.swift:1056](file:///Users/txa/project/Openterface_KeyCmd_iOS/KeyCmd/Managers/BLEManager.swift#L1056)），60–120 包/s 超过链路供给 → 积压在手机侧发送队列，松手后才排空。

固件侧修复（2026-11-11 已实现）：

- **B — HID 报告不回 ACK**：新增 `BLE_CH9329_RESP_NONE`，HID 命令（0x02/0x03/0x04/0x05）从 BLE 来时不产生任何应答；查询类命令（0x01/0x08/0x19/0x22/0x23/0x26/0x27/0x30…）仍走 BLE 应答，安全面板/改名/ECM 读取不受影响（[peripheral.h:81-96](file:///Users/txa/project/Openterface_KM/APP/include/peripheral.h#L81-L96)、[usbd_compostie_km.c:119-134](file:///Users/txa/project/Openterface_KM/APP/usbd_compostie_km.c#L119-L134)）。
- **C — 抬链路上限**：申请区间 10→12（15 ms，[peripheral.c:66-72](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L66-L72)）；`BLE_TX_NUM_EVENT=4`；同时 `BLE_BUFF_NUM 5→8`（4 个 TX 会把共享缓存压到只剩 1 个 RX）与 `BLE_MEMHEAP_SIZE 6144→8192`（[makefile:297-312](file:///Users/txa/project/Openterface_KM/build/makefile#L297-L312)）。RAM 占用 53160/65536 = 81%。
- 兼容性代价：依赖 HID ACK 判错的第三方客户端看不到应答（本厂 App 不依赖；`keymod-test` 需单独确认）。

**未做**：App 侧仍逐采样发送，没有合流和 `peripheralIsReady` 门控。若台架仍有滞后，下一步按 ~40 Hz 累积 dx/dy 合流 + `canSendWriteWithoutResponse` 门控改客户端。

## 8. `0x22 CMD_BLE_SECURITY` 命令集

命令字节定义在 [usbd_composite_km.h:51](file:///Users/txa/project/Openterface_KM/APP/include/usbd_composite_km.h#L51)，分发在 [usbd_compostie_km.c:303](file:///Users/txa/project/Openterface_KM/APP/usbd_compostie_km.c#L303)。当前实现 [peripheral.c:1294-1334](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L1294-L1334) 全部替换。

| 子命令 | 语义 | 载荷/应答 | 链路要求 |
|---|---|---|---|
| `0x01` | 原"开配对窗口"——**废弃** | 返回 `STATUS_ERR_PARAM` | — 本模型没有配对窗口概念 |
| `0x02` | 擦除全部 bond + 清 PIN override + 上限回 1（= 出厂重置安全参数） | 应答 `[status]` | USB **或**加密 BLE（§6.1 交接路径） |
| `0x03` | bond 状态 | 应答 `[status, count, max_bonds]`，count 读 `GAPBOND_BOND_COUNT`，max 读 RAM 缓存 | USB 或加密 BLE |
| `0x04` | **新**：设置 PIN override | 6 字节 ASCII 数字，校验 0..999999 | USB 或加密 BLE，**不回显 PIN** |
| `0x05` | **新**：设置绑定上限 | 1 字节，钳位 1..4 | **仅 USB**（原则 4） |
| `0x06` | **新**：读绑定上限 | 应答 `[status, max_bonds]` | USB 或加密 BLE |
| `0x07` | **新**（2026-11-12）：读有效 PIN | 应答 `[status]` + 6 字节 ASCII（override 或 `666666`） | USB 或加密 BLE；旧固件返回 `E2` |

注意与 BLE-Eth ECM IP 命令的字节复用消歧规则：`0x22 [0x03]` 保留给 CH9329 bond 查询，`0x22 [0x04]+9/13/21B` 等形状走 BLE-Eth（[peripheral.c:201-234](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L201-L234)）。新增 0x04/0x05/0x06 单字节子命令必须复核不与 `Peripheral_IsBleEthEcmIpFrame` 的形状判据冲突——0x04 已被 ECM "reset to defaults" 占用，故 PIN 子命令的判据必须靠长度区分（PIN 是 6 字节 ASCII，非 9/13/21）。这是实现时的具体坑，测试矩阵 T11 覆盖。

## 9. Flash 记录

页已用到 `0x0806B000..0x0806F000`（[ble_name_config.c:16-19](file:///Users/txa/project/Openterface_KM/APP/ble_name_config.c#L16-L19)、[ecm_ip_config.h:17-26](file:///Users/txa/project/Openterface_KM/APP/include/ecm_ip_config.h#L17-L26)）。**下一张空闲页 `0x0806A000`** 放本记录。

PIN override 与 max_bonds 合并成一条记录，不要开两页：

```c
#define BLE_PAIR_MAGIC     0x50454C42u  /* "BLEP" */
#define BLE_PAIR_VERSION   1u
#define BLE_PAIR_FLASH_ADDR 0x0806A000u
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  max_bonds;          /* 1..4，默认 1 */
    uint8_t  has_pin_override;
    uint8_t  rsvd;
    uint32_t pin_override;       /* 0..999999 */
    uint16_t seq;                /* 写序号，防旧记录胜出 */
    uint32_t crc;                /* FNV-1a，同 ble_name_config 模式 */
} BLE_PairConfig_t;
```

新增 `APP/ble_pair_config.c` + `APP/include/ble_pair_config.h`，接口对齐 `usb_mode_config` 风格：`BLE_PairConfig_Init/Get/SetMax/GetEffectivePin/SetPinOverride/ClearPinOverride`。无效/损坏记录 → 落默认值 `{max=1, no override}` 并在下次写时自愈。整页重写即可，该页写频极低（改 PIN/上限是人工操作），不需要磨损均衡。

## 10. LED 与广播

**LED（PC7 单色 PWM，无 RGB）**：现有 `g_ble_led_state` 0-4 语义不动（[peripheral_main.c:184-211](file:///Users/txa/project/Openterface_KM/APP/peripheral_main.c#L184-L211)、定义 [peripheral.c:155](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L155)）。由 pairStateCB 新增两态，持续约 3 s 后回落到连接态：

- `5` = 配对成功：双短闪
- `6` = 配对失败/PIN 错：单急闪

不加颜色、不加手势模式。

**广播容量位**：方案讨论过在 scanRsp 加 `[len, 0xFF, vid, occupied, count]` 让第 N+1 台连前就知道满员。当前 `advertData` 最长 7+1+1+20=29 B（名称上限 20），`scanRspData` 已用 18+name（[peripheral.c:429-450](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L429-L450)），再加 5 B 会挤掉 conn-interval 项或压名称。

**v1 不做**。App 连上后先查 `0x22 0x03`，再决定提示"已占用/请解绑"——单绑定下这就是两态文案，代价只是多一次往返。等确有裸客户端（非本厂 App）需求时再加。

## 11. 出厂重置（BOOT 键长按 5 s）

**KeyMod 的 BOOT 键在固件运行时可以采样**（2026-11-10 用户核实），接在 **PB8**。实现是 [button.c](file:///Users/txa/project/Openterface_KM/APP/button.c) + [button.h](file:///Users/txa/project/Openterface_KM/APP/include/button.h#L18)，主循环每轮调 `Button_Poll()`（[peripheral_main.c:83](file:///Users/txa/project/Openterface_KM/APP/peripheral_main.c#L83)）。

时序：

1. 按下 → 20 ms 消抖，开始计时；
2. 持续按住 **5 s**（[button.c:81](file:///Users/txa/project/Openterface_KM/APP/button.c#L81)）→ 进入待确认，PC7 **100 ms 急闪**，提示松手；
3. 松开 → 20 ms 消抖后执行 `Peripheral_FactoryResetSecurity()`（擦全部 bond → 清 PIN override → 上限回 1），再 `Peripheral_ScheduleSystemReset(500)` 重启（[button.c:99](file:///Users/txa/project/Openterface_KM/APP/button.c#L99)）；
4. 未满 5 s 松开 → 静默取消，状态不变。

**为什么是"松手后"才重启**：按住时 BOOT0 为高，此刻 `NVIC_SystemReset()` 会让芯片复位后直接进 ROM ISP，而不是跑固件。把重启放到松手之后（BOOT0 已回低）才安全。LED 急闪因此同时承担两个语义："请松手"和"松手即执行"。

**电气约束**：PB8 在 QFN28 的 CH32V208GBU6 上与 BOOT0 合封，数据手册注明合封后该脚输入功能受限，因此这里按**浮空输入**配置、**绝不开内部上拉**——上拉会把 BOOT0 拉高，导致下次上电进 ISP。按键空闲为低（外部下拉）、按下为高，与烧录时"按住 BOOT 才进 ISP"一致。若原理图上按键不在 PB8，只改 [button.h:18](file:///Users/txa/project/Openterface_KM/APP/include/button.h#L18) 的端口/引脚宏即可。

**上电防误触**：初始化时若读到高电平，先要求看到一次低电平才允许起计时（[button.c:52](file:///Users/txa/project/Openterface_KM/APP/button.c#L52)），避免接错网络或缺少下拉时反复进"待确认"。

**台架自检**：按住 5 s，LED 应开始急闪；松手后设备重启，USB `0x22 0x03` 返回 `count=0, max=1`，旧手机需在系统蓝牙里"忽略此设备"后才能用 666666 重新配对。按住后 LED 完全不闪 = 该脚没读到按键，先查原理图网络。

USB `0x22 0x02`（[peripheral.c](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L453)）和加密 BLE 的 `0x22 0x02`（§6.1 换机路径）仍然存在，与按键共用同一个 `Peripheral_FactoryResetSecurity()`。单绑定下这个函数同时是日常换机流程的兜底。



## 12. 实施计划

改动清单（编号沿用 §1-§11，状态截至 2026-11-10）：

| # | 文件 | 改动 | 里程碑 | 状态 |
|---|---|---|---|---|
| 1 | `Profile/ble_usb_service.c` | FFF2 + CCCD 权限加 `GATT_PERMIT_ENCRYPT_WRITE` | M1 | 已实现 |
| 2 | `APP/peripheral.c` | `ble_usb_ServiceEvt` 加 `linkDB_State(LINK_ENCRYPTED)` 兜底 | M1 | 已实现 |
| 3 | `APP/peripheral.c` | 删 `SimpleProfile_AddService` 调用（空函数） | M1 | 已实现 |
| 4 | `APP/peripheral.c` | `0x22 0x03` 返回真实 count + max | M1 | 已实现 |
| 5 | `APP/peripheral.c` | Bond Manager 块：passkey 改 `effective_pin`、显式 `ERASE_AUTO=0`、容量判定函数 | M2 | 已实现 |
| 6 | `APP/peripheral.c` | 实现 `pairStateCB`（LED 5/6、bond 刷新、模式切换）；passcodeCB 保持 NULL | M2 | 已实现 |
| 7 | `APP/ble_pair_config.c/.h`（新） | Flash 记录 + `BLE_PairConfig_Init` 接入 `Peripheral_Init` | M2 | 已实现 |
| 8 | `APP/peripheral.c` | `0x22 0x02` 实装、`0x04/0x05/0x06` 新增 | M2 | 已实现 |
| 9 | `APP/button.c` + `APP/include/button.h`（新） | 5 s 长按 → LED 急闪 → 松手执行出厂重置 + 重启；PB8 浮空输入 | M3 | 已实现 |
| 10 | `keymod-test` | 配对引导 + PIN 输入框、`0x22 0x03/0x04/0x05/0x06/0x02` 接入、"解除本机绑定"按钮 + iOS"忽略此设备"提示、上限 UI | M2/M3 | 未开始 |
| 11 | `APP/peripheral.c` | 连接后 4 tick 主动 `GAPBondMgr_PeriSecurityReq` + 断开停表（消除 App 首连 0.5 s 死窗口，§7.1） | M1/M2 | 已实现 |
| 12 | `APP/peripheral.c`、`APP/usbd_compostie_km.c`、`peripheral.h` | HID 报告（0x02/0x03/0x04/0x05）从 BLE 来时不回 ACK（`BLE_CH9329_RESP_NONE`，§7.2） | M4 | 已实现 |
| 13 | `build/makefile`、`APP/peripheral.c` | 连接间隔 12.5→15 ms、`BLE_TX_NUM_EVENT=4`、`BLE_BUFF_NUM=8`、堆 8 KB（§7.2） | M4 | 已实现 |

实现备注：新文件必须登记进 `build/APP/subdir.mk`（三个列表：`C_SRCS`、`C_DEPS`、`OBJS`），该文件是 MounRiver 生成的静态清单，不做 glob。`bluetooth` 记录页 `0x0806A000` 位于 440 KB 代码区之内，与 ECM IP 配置页同一风险：镜像必须保持在 426 KB 以内（当前 ~220 KB）。

**M1 — 关门**（1-4）：未配对即不可用，bond 仍可白嫖 666666。改动小、可独立台架验证。**风险**：旧版 App 直连失败，必须与 App 同步发版；USB 通路保留兜底。
**M2 — 真 PIN + 单绑定**（5-8）：安全模型成立，初始 PIN 666666，可改。
**M3 — 体验**（9、10）：按键重置已实现（§11）、上限调整 UI、App 配对引导。

一句话优先级：承重的是 §7 两层，三行代码把"任何路人"降级成"拿到初始 PIN 的人"。若只做一件事，做 M1。

## 13. 测试矩阵

台架：KeyMod + 2 台 iOS + 2 台 Android + USB 串口（sudo）。逐条对应 §7/§5/§6/§8/§9。

| # | 用例 | 预期 |
|---|---|---|
| T1 | 未加密写 FFF2 / 写 CCCD | ATT 错误 **0x0F** INSUFFICIENT_ENCRYPT；`linkDB_State` 兜底命中 |
| T2 | 用 666666 配对 | 成功、bond 落盘、LED 态 5、`0x22 0x03` 返回 count=1, max=1 |
| T3 | 错 PIN 配对 | 失败断链、count 不变、LED 态 6、可重试 |
| T4 | 配对后重启 | 已绑定手机免输入直接加密重连（验证 P1b；满员下调到 1 也要验） |
| T5 | 占用中第二台配对 | **失败**（`NO_PAIRING` 生效） |
| T6 | App 解绑 → 系统"忽略设备" → 新手机用 666666 配对 | 全程不需要 USB（§6.1 主路径成立） |
| T7 | USB `0x22 0x02` | bond 清空、PIN override 清、上限回 1、666666 可配 |
| T8 | `0x22 0x05` 从 BLE 发 | 拒绝；从 USB 发成功 |
| T9 | `0x22 0x05` 钳位 | 写 0 → 1；写 255 → 4；断电重启保持 |
| T10 | 上限 4 配 3 台后调回 1 | 3 台存量重连仍成功，新配对被拒（§5.3） |
| T11 | `0x22 0x04`（6B PIN）与 BLE-Eth ECM 帧 | 消歧正确（PIN 帧 `buf[4]=7`，不匹配 1/9/13/21），不误触 ECM 默认值重置 |
| T12 | Flash 记录损坏（人为写垃圾到 `0x0806A000`） | 落默认值、下次写自愈、不硬 fault |
| T13 | iOS/Android 双端 passkey entry | 弹输入框（验证 P3：passcodeCB=NULL 时栈行为） |
| T14 | 回归 | BLE-Eth 隧道、CH9329 全命令、`0x19`、改名在加密链路上行为不变 |
| T15 | （用户调到 4 时才需）连续配 6 台 + `ERASE_AUTO=0` | 不静默清空，栈实际上限可预期（P2） |
| T16 | `0x22 0x04` 改成 123456 后重启 | 断电保持；旧 bond 不受影响，新配对应要求 123456 |
| T17 | BOOT 键按住 5 s 后松开 | 第 5 s LED 急闪；松手后重启，`0x22 0x03` 为 count=0/max=1；旧手机"忽略设备"后可 666666 重配 |
| T18 | BOOT 键按住 3 s 松开 | 无变化：不闪、不重启（提前松手取消） |
| T19 | 已绑定手机重连后立即拖鼠标 | FFF1 一次订阅即成功（无 0.5 s 重试），从首帧起指针跟手；首配时 PIN 框在连接瞬间弹出 |
| T20 | 连续拖动 10 s（iOS，ProMotion 机型） | 滞后不累积；指针与手指间距恒定在 150 ms 内；FFF1 上不再出现按移动速率刷屏的 7 B ACK |
| T21 | BLE 上发 0x01/0x22 0x03/0x23/0x26 查询 | 应答照常（安全面板、改名、ECM 读取可用）；确认 B 只掐 HID 的 ACK |
| T22 | B 已生效后仍有残留滞后 | 记录协商后的 connInterval（外设侧临时打印 `peripheralConnList.connInterval`）；若仍 30 ms，转 App 侧合流/流控 |

## 14. 待验证前提

P1、P1b 不成立则 M1 的机制需重做，开工第 0 步就该在台上验掉，而不是写完代码才发现。

| ID | 前提 | 若不成立 | 何时验 |
|---|---|---|---|
| P1 | ROM ATT 层真的检查属性权限的 encryption 位 | ①形同虚设，全靠②兜底 + 需改为显式 reject 并回错误码 | M1 第一件事，T1 |
| P1b | `GAPBOND_PAIRING_MODE_NO_PAIRING` 不拦已绑定链路的加密命令 | 满员时老手机连不上 → 降级为"满员只靠广播位 + App 拒绝，固件保持 WAIT_FOR_REQ" | M2，T4/T5/T19（§7.1 改为连上即请求加密后升为必测） |
| P2 | ROM bond 容量 ≥ 用户可配上限 | 上限钳位下调到实测值 | 仅当有人调到 >1，T15 |
| P3 | `DISPLAY_ONLY` + 预设 passcode + passcodeCB=NULL 时手机侧弹 PIN 框 | 注册 passcodeCB 只做显示 | M2 第一天，T13 |
| P4 | 密钥分发含必需 key（默认 `0x77`，`GAPBOND_PERI_KEY_DIST_LIST=0x406`）足以支持重连加密 | 调整分发 mask | M2，T4 |
| T-HW | ~~KeyMod 板上是否有可 GPIO 采样的按键~~ | **已结论（2026-11-10）**：按键可采样，接 PB8；按浮空输入实现 5 s 长按（§11） | 已关闭 |

## 15. 风险与已知不足

| 风险 | 处置 |
|---|---|
| 旧 App 与新固件断层 | M1 与 App 同批发版；USB 通路永久可用 |
| PIN 持有者驱逐现任（§6.2） | v1 接受 + PIN 当封条；v2 上 App 层每手机所有权密钥 |
| 丢失手机无法单独吊销 | 单绑定下 = 走 §6.1/§11 重置，全擦即吊销，无残留 |
| iOS 系统层残留 bond 记录 | App 解绑引导必须包含"忽略此设备"步骤（T6） |
| `0x22` 与 BLE-Eth 命令字节继续复用 | 新子命令一律用长度形状区分，T11 守门；长期看应给安全命令换独立命令字节 |
| BLE 吞吐上限（MTU 60，约 36.5 kbit/s） | 与本方案无关，但 PIN 校验/命令往返都在同一条链路上，勿在其上再叠握手层 |
