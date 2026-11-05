# Firmware 稳定性修复计划

- 分支：`fix/stability-p0-p1`
- 基线：`main` @ `19ef5c8`
- 适用固件：KVM-GO（mode 7）、KeyMod Plus（mode 7）、KeyMod Mini（mode 4）
- 状态：Phase 1 已实施（4 个产品模式构建通过；硬件验证待执行，见第 5 节）

---

## 1. 目标与范围

本轮修复只处理**会导致数据损坏、卡死、协议串扰或不可恢复状态**的问题，不改变 BLE-Eth / CH9329 的对外协议字节定义，不做无关重构。

目标：

1. 隧道数据在正常负载和中断并发下不再出现"拼帧"或静默损坏。
2. BLE-Eth 帧不再被旧 CH9329/USB HID 路径二次消费。
3. TCP 发送路径不再使用 1.5 KB 栈帧，失败时不再提前推进序号。
4. BLE 断连、目标侧重枚举后，隧道状态可预测、可恢复。
5. 配置写入不再无意义地擦写 Flash。

不在本轮范围（见第 6 节 Phase 2）：TCP 重传/RTO、BLE-Eth ACK 端到端背压、重枚举看门狗、Flash 加载语义校验、链接脚本保留段。

---

## 2. 问题总览

| ID | 级别 | 问题 | 位置 | 后果 | Phase |
|----|------|------|------|------|-------|
| P0-1 | P0 | ECM 队列被 USBFS/USBD 中断和主循环并发读写，无临界区；`queue_peek` 指针跨多次 32B 传输，队列满时被 flush 覆盖 | `APP/ecm_bridge.c` `queue_push/peek/pop/clear`、`poll_*_tx`、`ECM_Bridge_*RxDone` | 发出拼帧、rd/wr/count 竞争、对端丢包后隧道永久卡死 | 1 |
| P0-2 | P0 | BLE-Eth 帧同时写入 `RingMemBLE`，被 `USBSendData()` 送到 EP2（HID 鼠标）并被 `CH9329_RxBuffer_Add()` 解析 | `APP/peripheral.c` `ble_usb_ServiceEvt` | 隧道字节被当成 HID 报文/串口噪声；CH9329 回 0xC0 错误帧 | 1 |
| P0-3 | P0 | `send_tcp_frame()` 使用 1514B 栈缓冲；`g_ecm_tx_cb` 失败也返回成功并推进 `local_seq`；发送层无重传 | `APP/ble_eth_protocol.c` `send_tcp_frame`、`BLE_Eth_SendDataToEth` | ISR 上下文栈溢出；队列丢弃后序号永久错位 | 1 |
| P1-1 | P1 | BLE 断连不清理 BLE-Eth（parser/reassembly/tx_queue/connections） | `APP/peripheral.c` `Peripheral_LinkTerminated` | 跨连接残留半帧、旧通知；重连后状态错乱 | 1 |
| P1-2 | P1 | notify 失败立即重试，不检查连接句柄 | `APP/peripheral.c` `SBP_PROCESS_BLEETH_TX_EVT` | 断连后主循环空转 | 1 |
| P1-3 | P1 | 目标侧重枚举后不重置 BLE-Eth 连接、不通知客户端 | `APP/ecm_bridge.c` `poll_target_reenum` | 客户端仍认为 ESTABLISHED，继续发旧 seq | 1 |
| P1-4 | P1 | `BLE_Eth_HandleTargetFrame` 未校验 IP total length | `APP/ble_eth_protocol.c` | 无符号下溢，错误长度导致越界读 | 1 |
| P1-5 | P1 | 重枚举状态机无超时/成功判据/重试；attach 后不验证枚举结果 | `APP/ecm_bridge.c` `poll_*_reenum` | 一次失败后需拔插；目标 HID 丢失约 1.1s | 2 |
| P1-6 | P1 | 主机侧 `ECM_Bridge_Init()` 清 `g_host_to_target`，而隧道 seq 已推进 | `APP/ecm_bridge.c` `ECM_Bridge_Init` | USB 复位/睡眠唤醒时静默丢段 | 1（缓解）/2（根治） |
| P2-1 | P2 | 每次 `ECM_IPConfig_Set()` 都擦写，不比较是否变化；加载时不语义校验 | `APP/ecm_ip_config.c` | 无谓磨损；非法记录可配死网络 | 1（比较）/2（校验） |
| P2-2 | P2 | `txq_lock/unlock` 在中断里无条件 `__enable_irq()` | `APP/ble_eth_protocol.c` | 破坏调用者中断状态 | 1 |
| P2-3 | P2 | 0x22/0x23 依赖 payload 形状区分，协议脆弱 | `APP/peripheral.c` | 后续扩展易回归 | 2 |
| P2-4 | P2 | 链接脚本未显式保留 0x6B000/0x6C000 配置页 | `HAL/Link_440k.ld` | 代码增长后可能覆盖配置 | 2 |

---

## 3. Phase 1 详细设计

### 3.1 P0-1 ECM 队列并发保护

**根因**

- `ECM_Bridge_TargetRxDone()` / `ECM_Bridge_HostRxDone()` 在 USB 中断中调用，最终执行 `queue_push()` / `queue_clear()`。
- 主循环的 `poll_host_tx()` / `poll_target_tx()` 执行 `queue_peek()` / `queue_pop()`。
- `ECM_FrameQueue_t` 的 `rd/wr/count` 不是原子操作，没有临界区。
- `queue_peek()` 返回的指针会跨多次 32 字节 `USBD_ENDPx_DataUp()` 使用；队列满时 `queue_push()` 执行 `rd = wr; count = 0`，同一 slot 被新帧覆盖，导致"前半旧帧 + 后半新帧"。

**修复**

1. 增加基于 WCH CSR `0x800` bit3 的 save/restore 临界区（不能用 `__get_MSTATUS/__set_MSTATUS`，见 `docs/HID_QUEUE_RELIABILITY_FIX.md`）：

```c
static inline uint32_t ecm_irq_save(void)
{
    uint32_t csr;
    __asm volatile ("csrr %0, 0x800" : "=r"(csr));
    __disable_irq();
    return (csr >> 3) & 0x1u;
}

static inline void ecm_irq_restore(uint32_t state)
{
    if (state) __enable_irq();
}
```

2. `ECM_FrameQueue_t` 增加 `volatile uint8_t in_flight` 和 `volatile uint16_t gen`。
3. `queue_push()`：
   - 长度越界直接返回 0。
   - 队满时若 `in_flight`，**丢弃新帧并返回 0**，绝不覆盖正在发送的头帧。
   - 队满且无在途帧时保留原有 flush 行为并 `gen++`。
4. `queue_clear()`：加锁，清零 `in_flight`，`gen++`。
5. 新增 `queue_tx_load()` / `queue_tx_complete()`：
   - `queue_tx_load()` 在临界区内把当前头帧的下一段（≤32B）或 ZLP 标记复制到本地 chunk，并置 `in_flight = 1`。
   - `queue_tx_complete()` 校验 `gen` 和头指针未变后再弹出；若中途发生 `queue_clear()` 则放弃弹出。
   - `poll_*_tx()` 不再长期持有队列内指针，USB 写入使用本地 chunk。
6. `ECM_Bridge_SendRawFrame()` 返回 `queue_push()` 结果，供 P0-3 使用。

**验收**

- 高负载下 `overflow` 计数可以增长，但抓包不再出现以太网帧长度/校验异常。
- 隧道 + HID 100Hz + USB 复位并发压测 1 小时无卡死。

### 3.2 P0-2 BLE-Eth 与 CH9329 路径隔离

**根因**

`ble_usb_ServiceEvt()` 只识别 CH9329 命令字节；0x10/0x11/0x12/0x1F/0x1E/0x20/0x21 落入 else 分支后，除了 `BLE_Eth_Parse()`，还被写入 `RingMemBLE`，由 `USBSendData()` 发到 EP2，并由 `USB_DataRx_To_KMHandle()` 喂给 CH9329 解析器。两套协议校验和算法相同，CH9329 会对未知命令回 `STATUS_ERR_CMD`。

**修复**

1. 新增 `Peripheral_IsBleEthCommandByte(cmd)`：识别 0x10–0x12、0x1E、0x1F、0x20、0x21。
2. `ble_usb_ServiceEvt()` 改为三分支：
   - BLE-Eth（含 0x22/0x23 的 ECM IP 形状）→ 只调用 `BLE_Eth_Parse()` + 触发 TX 事件，**不写 RingMemBLE、不喂 CH9329**。
   - CH9329 命令 → 维持现状。
   - 其它非协议数据 → 保留原有 RingMemBLE 原始透传路径。
3. 0x22/0x23 的 CH9329 帧（BLE security / set name）仍由 `Peripheral_IsBleEthEcmIpFrame()` 判为 false 后进入 CH9329 分支。

**验收**

- 发送 0x11 DATA 时，目标侧 EP2 不再出现 HID 报文。
- 发送 0x10/0x11/0x12/0x1F 时，串口/USB 响应队列不再出现 0xC0 错误帧。
- CH9329 键鼠、BLE 改名、BLE 配对查询回归通过。

### 3.3 P0-3 TCP 发送栈与失败语义

**修复**

1. `send_tcp_frame()` 的 1514 字节缓冲改为模块级 static。该函数既在 TMOS/BLE 任务（SYN/DATA）中执行，也会从 USBFS 中断（对目标数据的 ACK）进入，因此"组帧 + 入队"整个过程用 `txq_lock()`（CSR 0x800 bit3 保存/恢复）串行化，避免嵌套发送重写共享缓冲。
2. `ble_eth_ecm_tx_cb_t` 与 `BLE_Eth_SetEcmtxCb()` 改为返回 `uint8_t`；`ECM_Bridge_SendRawFrame()` 返回入队结果。
3. 仅当入队成功时推进 `local_seq`（SYN/FIN/数据统一规则），失败返回 0。
4. `BLE_Eth_SendDataToEth()` 在失败时停止后续分段，并累加 `eth_tx_fail` 计数。
5. `send_tcp_syn()` 保留原有失败回滚逻辑。

**验收**

- `-fstack-usage` 中 `send_tcp_frame` 栈用量从 ~1.5 KB 降到 < 100 B。
- 人为把队列压满时，不再出现序号提前推进；`eth_tx_fail` 可观测。

> 注：本轮**不实现** TCP 重传。丢段仍是错误路径，但已从"静默 + 序号错位"变为"可观测 + 序号一致"。真正的端到端保证见 Phase 2。

### 3.4 P1-1 / P1-2 BLE 链路生命周期

1. 新增 `BLE_Eth_OnLinkLost()`：
   - 清 parser、分片重组、TX 队列；
   - 将所有连接置为 CLOSED 并清 `active`；
   - 复位最后 ACK 状态。
2. `Peripheral_LinkTerminated()` 调用 `BLE_Eth_OnLinkLost()`，并 `tmos_stop_task(SBP_PROCESS_BLEETH_TX_EVT)`。
3. `SBP_PROCESS_BLEETH_TX_EVT`：
   - notify 前校验 `connHandle != GAP_CONNHANDLE_INIT`；
   - 失败时用 `tmos_start_task(..., N)` 延迟重试，连续失败 3 次后停止本轮发送并保留队列。

### 3.5 P1-3 目标侧重枚举后的隧道处理

1. 新增 `BLE_Eth_ResetAllTunnels(uint8_t status)`：
   - 对所有 active 连接发送 `BLE_ETH_CMD_CONN_CLOSED (0xD2)` 通知（payload: conn_id + status）；
   - 清除连接状态。
2. **实施位置调整**：在 `schedule_target_reenum()` 中调用 `BLE_Eth_ResetAllTunnels()`，而不是 detach 阶段。原因是排队帧要到 500 ms 后才 detach，在 detach 时再入队的 RST 根本来不及发出，且 attach 阶段会清空 `g_host_to_target`。在 schedule 时关闭隧道可让 RST 利用仍存活的 ECM 链路发往目标，同时 `CONN_CLOSED` 有充足时间送达 BLE 客户端。
3. 主机侧重枚举（`ECM_Bridge_Init`）不关闭隧道；见 3.8。

### 3.6 P1-4 IP 长度校验

在 `BLE_Eth_HandleTargetFrame()` 中增加：

```c
uint16_t ip_total = frame_get_be16(&ip[2]);
if (ip_total < ip_header_len) return false;
if ((uint32_t)off + ip_total > len) return false;
```

再用 `ip_total - ip_header_len` 计算 TCP 段长度，避免无符号下溢。

### 3.7 P2-1 / P2-2 细节修复

1. `ECM_IPConfig_Set()` 在参数校验后比较 RAM cache；完全相同则直接返回 `ECM_IP_CONFIG_OK`，不擦写、不触发重枚举。
2. `txq_lock/txq_unlock` 改为与 `ecm_irq_save/restore` 相同的 CSR 0x800 bit3 保存/恢复。

### 3.8 P1-6 主机侧重枚举的静默丢段（Phase 1 缓解）

`ECM_Bridge_Init()` 由 Mac 侧 USBD 复位触发。该路径不影响目标侧 ECM 链路，因此当存在 active 或 pending 隧道时**不再清空 `g_host_to_target`**（该队列同时承载隧道帧与桥接帧）；只有在没有隧道时才 flush 旧会话帧。这样避免隧道帧在 `local_seq` 已推进的情况下被静默丢弃。彻底方案（区分队列来源）列入 Phase 2。

---

## 4. 涉及文件

| 文件 | 改动 |
|------|------|
| `APP/ecm_bridge.c` | 队列临界区、in_flight/gen、TX chunk 复制、`SendRawFrame` 返回值、目标重枚举关闭隧道、主机重枚举保留隧道队列 |
| `APP/include/ecm_bridge.h` | `ECM_Bridge_SendRawFrame` 返回类型 |
| `APP/ble_eth_protocol.c` | static TX 缓冲、回调返回值、seq 推进规则、IP 长度校验、`BLE_Eth_OnLinkLost`、`BLE_Eth_ResetAllTunnels`、txq lock |
| `APP/include/ble_eth_protocol.h` | 新 API 声明、回调签名、统计字段 |
| `APP/peripheral.c` | BLE-Eth 命令白名单、断连清理、notify 重试退避、TX 回调返回值 |
| `APP/ecm_ip_config.c` | 配置未变化时短路 |

### 4.1 实施与构建验证（2026-11-02）

Phase 1 改动已全部落在 `fix/stability-p0-p1` 分支，四个产品模式构建通过且无编译警告：

| 模式 | FLASH | RAM |
|------|-------|-----|
| kvmgo (mode 7) | 219968 B / 48.82% | 51064 B / 77.92% |
| minikvm (无 BLE) | 49924 B / 11.08% | 37704 B / 57.53% |
| keymod-plus (mode 7) | 217864 B / 48.35% | 51048 B / 77.89% |
| keymod-mini (mode 4) | 217784 B / 48.34% | 51048 B / 77.89% |

静态发送缓冲把约 1.5 KB 栈占用转移到 BSS；RAM 余量约 22%，后续 Phase 2 增加重传缓冲前需先复核余量。

---

## 5. 测试计划

### 5.1 构建与静态检查

1. 用 MounRiver 工具链构建 `kvmgo`（mode 7）与 `keymod-mini`（mode 4）：

```bash
RISC_COMPILER_PATH="/Users/txa/MRS_Toolchain_MAC_V240/Toolchain/RISC-V Embedded GCC/bin" ./build.sh kvmgo
RISC_COMPILER_PATH="/Users/txa/MRS_Toolchain_MAC_V240/Toolchain/RISC-V Embedded GCC/bin" ./build.sh keymod-mini
```

2. 检查无新增 warning；`output/*.map` 中 `__stack_size` 仍为 2048，`_ebss` 余量不低于 12 KB。
3. 生成 `-fstack-usage`，确认 `send_tcp_frame`、`BLE_Eth_Parse`、`ECM_Bridge_TargetRxDone` 调用链最坏栈 < 1.5 KB。
4. 对 `APP/*.c` 跑 `cppcheck --enable=warning,performance`，确认无新增告警。

### 5.2 协议隔离回归（无需额外硬件）

1. BLE 发 0x01/0x02/0x05：键鼠正常。
2. BLE 发 0x22 单字节 0x01–0x03：BLE 配对查询正常。
3. BLE 发 0x23 + 名称文本：改名 + 重启正常。
4. BLE-Eth 发 0x10/0x11/0x12/0x1F：抓目标侧 EP2，确认无 HID 报文；串口无 0xC0 错误帧。
5. `tools/ble_ecm_ip.py get` / `set` 反复 50 次：配置正确，设备不重启。

### 5.3 数据完整性与并发

1. BLE-Eth `scp` 10 MB + `sha256sum`：连续 20 次全部一致。
2. 隧道传输期间 100 Hz 发 HID 报文：数据仍一致，键鼠无卡顿。
3. 隧道传输期间触发目标侧 IP 变更（重枚举）：客户端在 1 s 内收到 `CONN_CLOSED`，重连后数据完整。
4. 隧道传输期间 Mac 睡眠/唤醒、USB 复位各 20 次：连接可恢复，无永久卡死。
5. 把 ECM 队列故意压满（降低 `ECM_BRIDGE_QUEUE_DEPTH` 或注入大流量）：确认不再出现拼帧，`eth_tx_fail` 增长。

### 5.4 链路与重枚举

1. BLE 1 s 连 / 1 s 断 100 次：无残留 parser、无 TX 事件空转、重连后立即可用。
2. 200 次 `SET_ECM_IP`（相同/不同交替）：`GET 0x05` 的 `reenum_count` 与命令数一致，`detach_dp_level == 0`。
3. 重枚举 detach 窗口内断电/复位：重启后状态机从 0 开始。
4. 隧道中连续 20 次 `SET_ECM_IP` 目标侧地址：每次都能收到 CONN_CLOSED，设备不锁死。

### 5.5 长稳

1. 72 h 隧道 + 周期 HID + 每小时一次 IP 查询：无 IWDG 复位（4 s），无内存水位持续下降。
2. 记录 `g_target_to_host.overflow`、`g_host_to_target.overflow`、`tx_queue_full_drop`、`frag_reassembly_abort`、`eth_tx_fail`，结束后均不持续增长。

---

## 6. Phase 2（后续）

| 项 | 说明 |
|----|------|
| TCP 重传/RTO | 为设备→目标方向增加 2 段发送缓存，3 次 dup-ACK 或 300 ms RTO 触发重传 |
| BLE-Eth ACK 背压 | 最后一个分片的 ACK 延迟到 ECM 入队成功后发送；失败时让客户端重传整个 TCP 段 |
| 重枚举看门狗 | attach 后等待 `g_*_ecm_data_if_active`，超时重试 1–2 次并上报 |
| 主机侧重枚举 | 评估 USBD 复位时是否保留隧道；必要时同样关闭并通知客户端 |
| Flash 语义校验 | `ECM_IPConfig_Init()` 加载后跑 `validate_config()`，失败回退默认值 |
| 链接脚本 | 为 0x6B000/0x6C000 配置页增加 `ASSERT`/保留段 |
| 协议标识 | 为 BLE-Eth 使用独立命令字节或 addr 标识，移除 0x22/0x23 形状判别 |
| 诊断 | `GET 0x05` 追加队列溢出、发送失败、重传计数 |

---

## 7. 验收标准（Phase 1）

- [ ] 四种构建目标全部编译通过，FLASH/RAM 不超限。
- [ ] 协议隔离回归全部通过。
- [ ] 10 MB 数据传输 20 次 sha256 一致。
- [ ] BLE 断连风暴 100 次后无异常残留。
- [ ] 目标侧重枚举后客户端收到关闭通知。
- [ ] `send_tcp_frame` 栈用量 < 100 B。
- [ ] 72 h 长稳无看门狗复位。

---

## 8. 风险、兼容性与回滚

**兼容性**

- 不修改 BLE-Eth / CH9329 命令字节与 payload 格式。
- `BLE_Eth_SetEcmtxCb()` 回调签名变化仅影响 `peripheral.c` 与 `ecm_bridge.c` 的内部连接。
- `ECM_Bridge_SendRawFrame()` 返回值变化对现有调用者向后兼容（忽略返回值仍可编译）。

**风险**

- 临界区会短暂关闭中断（组帧 + 1.5 KB memcpy，约数十微秒），需确认 USB/BLE 无丢事件。
- `queue_push()` 队满时改为丢新帧（存在在途帧时）可能让高负载下丢包更早暴露，但不会拼帧。
- `BLE_Eth_OnLinkLost()` 会主动关闭所有隧道，依赖客户端重连。

**回滚**

- 每个问题独立提交，可按提交回滚。
- 若临界区导致 USB 异常，先回滚 P0-1，再单独评估。

---

## 9. 提交拆分建议

1. `fix(ecm): serialize frame queues between USB ISRs and main loop`
2. `fix(ble): route BLE-Eth frames away from legacy CH9329/USB path`
3. `fix(ble): avoid 1.5KB stack frame and only advance TCP seq on enqueue success`
4. `fix(ble): reset BLE-Eth state on link loss and back off notify retries`
5. `fix(ecm): close BLE-Eth tunnels before target-side re-enumeration`
6. `fix(ble): validate IPv4 total length before parsing TCP`
7. `fix(config): skip flash write when ECM IP configuration is unchanged`
8. `fix(ble): restore interrupt state in tx queue lock`
