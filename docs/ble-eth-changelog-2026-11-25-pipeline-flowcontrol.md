# 变更说明：多消息流水线发送 + 下载方向 TCP 流控 + 重传幂等性
**日期**: 2026-11-25
**分支**: `feature/cdc-ncm`（Fedora 仓库为权威）
**提交**: `e1444c0`（perf）、`563a66a`（docs）
**烧录固件**: `output/openterface-km-firmware-keymod-plus.bin`（225,748 B，wchisp verify OK）
**构建命令**:
```bash
./build.sh \
  --compiler "/home/bbot/tools/MRS-linux-x64/resources/app/resources/linux/components/WCH/Toolchain/RISC-V Embedded GCC/bin" \
  --make-arg USB_WORK_MODE=hid_ncm_bridge \
  --make-arg USB_WORK_MODE_RUNTIME_PROFILES=0 \
  keymod-plus
```

---

## 1. 背景与动机

上一轮窗口化发送端（报告 §10）落地后遗留两个问题：

1. **2000B 额外 RTT**：载荷超过 `MAX_MESSAGE_LEN`（1476 B）时按边界拆成两条消息串行发送，每条各自等待全部分片 ACK，多付一次完整往返（2000B = 1476 + 524）。
2. **下载方向停滞**：设备 TCP ACK 固定通告 `0xFFFF` 窗口，target 可以把远超设备缓存（TX 队列 16 深，可用 14 chunk ≈ 3.4 KB）的数据灌进来；队列满 → PushTargetData 失败 → dup-ACK → target 等 RTO（Windows ≥300ms）重传，下载被重传节奏拖慢。

验证过程中，双向饱和还暴露出一个此前隐藏的**数据损坏 bug**（4000/6000B echo 必挂），一并修复。

---

## 2. 变更清单

| 文件 | 变更 |
|---|---|
| `tools/ble_eth_tunnel.py` | ① `send_data()` 超 1476B 载荷拆成 reassembly 尺寸分组、共享一个窗口跨组流水；② 窗口发送端分组感知，组失败整组从 seq0 重发（标 RETRANS，≤3 次）而非退化 legacy；③ 所有重传帧打 `FRAG_RETRANS` 并重算 checksum；④ `_await_pending_data_ack` 增加 strict 模式（组内失败抛异常触发重启） |
| `APP/ble_eth_protocol.c` | ① CMD_DATA 处理 `FRAG_RETRANS`（重传幂等三态）；② TCP ACK 窗口改为按 TX 队列空余动态通告 + PumpTick 窗口更新纯 ACK；③ 新增诊断计数 `g_dbg_tcp_win_update`（0x11 stats 偏移 55-56） |
| `APP/include/ble_eth_protocol.h` | 新增 `BLE_ETH_FRAG_RETRANS 0x20U`（原空闲位 bit5） |
| `build/makefile` | `BLE_BUFF_NUM ?= 8` → `12` |
| `tests/test_tunnel_full.py` | echo 尺寸增加 4000B / 6000B；0x11 stats 格式化输出 TCP 计数器尾部 |

---

## 3. 协议变更：FRAG_RETRANS (0x20)

分片头 `frag_flags` 原用位：bit7 `FRAG_MORE (0x80)`、bit6 `FRAG_FIRST (0x40)`、bit3-0 分片数。**bit5 (0x20) 空闲**，定义为"主机重传标记"。

- 仅主机→设备方向使用（上传 DATA 分片）；设备→主机下载分片无 ACK 机制，不用。
- **兼容性**：旧主机不打此位 → 设备走原有路径，行为不变；新主机对旧固件置位 → 设备把它当普通标志忽略（FIRST/MORE/COUNT 判断不受影响），仅失去幂等保护，不致损坏。

设备端三态逻辑（收到 FIRST|RETRANS 时）：

| 设备状态 | 判据 | 行为 |
|---|---|---|
| 消息仍在重组 | `reassembly.in_progress` | 不重启，dup-ACK 当前进度（`expected_seq = next_seq`），主机累积 ACK 语义自行收敛 |
| 上条消息已完成 | `!in_progress` 且 `g_last_data_ack.valid` 且 conn 匹配且 `expected_seq == total_frags` | **重放最终 ACK，不重组不转发**（防止 payload 二次转发） |
| 上次尝试中途 abort | `!in_progress` 且 `g_last_data_ack.valid == false` | 接受全新开始（唯一恢复路径） |

count 匹配门槛的意义：重传消息必然与最后完成消息分片数一致才可能是重复；不一致（如 1048B 的 count=5 尾组跟在 1476B count=6 后面）绝不重放，宁可走新开始路径，避免"重放吞掉真数据"。

三态决策流程：

```
        上传 DATA 分片, frag_flags 含 FIRST|RETRANS (主机整组重启)
                                |
                                v
              +---------------------------------+
              |  reassembly.in_progress ?       |
              +-----+---------------------+-----+
                 是 |                     | 否
                    v                     v
     [1] 报进度: dup-ACK      +---------------------------------+
       expected_seq=next_seq  |  g_last_data_ack.valid ?        |
       (主机靠累积 ACK 语义   |  && conn 匹配                   |
        自行收敛, 在途分片    |  && expected_seq == count ?     |
        继续正常接收)         +-----+---------------------+-----+
                                 是 |                     | 否
                                    v                     v
                    [2] 重放最终 ACK          [3] 全新开始重组
                      不重组、不转发            reassembly_clear()
                      (payload 已投递过,        + 正常重组; 上次中途
                       防止二次转发)            abort 后的唯一恢复
                                               路径, 收满后转发一次
```

触发场景对照：[1] 分片已收到但 ACK 丢失；[2] 消息已完成并转发、最终 ACK 没到主机；[3] 写丢失导致乱序 abort（FRAG_RETRANS 分支必须在 valid==false 时放行，否则 abort 后无法恢复）。


---

## 4. 修复的关键 bug（4000/6000B echo 损坏的完整链条）

### 4.1 根因：BLE 写丢失

每 session 稳定出现恰好 1 次 DATA ACK 超时——非随机丢包，是**设备端控制器缓存溢出丢弃 write-without-response**：`BLE_BUFF_NUM=8` 的包缓存在双向饱和（TX 通知 4 包/事件 + RX 写并发）下不够用。修复：`BLE_BUFF_NUM 8→12`（缓冲从 BLE 堆分配，RAM 富余，bss 不变）。

### 4.2 损坏路径 A：幽灵重组（已修）

写丢失发生时主机重传：

```
分片 seq0 (FIRST) 写丢失 → 设备从未收到 → 主机超时重传 seq0 (FIRST)
→ 设备空闲，原逻辑无条件 reassembly_clear() + 开始新重组
→ 重传的 seq1..5 依次被接受 → "幽灵消息"完成 → payload 二次转发给 TCP
→ echo 服务器收到重复数据 → 回显流从 1476 偏移开始错位
```

实测损坏模式与理论吻合（4000B: `diffs=738@1476` = 恰 3 个重传分片；6000B: `diffs=246@1476`）。修复：FRAG_RETRANS 三态（§3）。

### 4.3 损坏路径 B：FRAME_ERR 循环 → legacy 回退（已修）

中间分片写丢失时：设备收到乱序分片 → abort 重组 + FRAME_ERR（expected_seq = 缺失序号）→ 主机对**同一分片**重传（永远不是设备要的那个）→ FRAME_ERR 循环 → 重试耗尽 → 主机回退 **legacy 无 ACK 模式** → 后续分片静默丢失。

修复：主机改为**整组从 seq0 重启**（标 RETRANS）。设备 abort 后 `g_last_data_ack.valid=false` → 新开始路径接受 → 之后分片按序补齐，语义收敛。重启前 30ms 退避让设备排空。

### 4.4 TCP 窗口下限的调参教训

第一版窗口下限用 MSS（1460B）：当队列空余在 8-14 chunk 之间时，target 收到 ≥MSS 的窗口就发整段（1460B = 7 chunk），push 失败 → dup-ACK → RTO 停滞。改为**每 chunk 200B** 配比：200 × 7 chunk = 1400B，任何 ≤MSS 段组合都能装下（1460B 段 7 chunk，7×208=1456 ≥ 1460 不够 → 200 留了余量），push 不再失败。下限 1 chunk（244B）：设备不会主动发零窗口更新（对纯 ACK 探测不应答），通告 0 会死锁；≥1 chunk 时 target 始终保留一个可装下的小段在途，每收一段即回带新窗口的 ACK，流不断。

---

## 5. 验证结果

### 5.1 官方套件（`tests/test_tunnel_full.py`，Mac bleenv 运行）

**25 PASS / 0 WARN / 0 FAIL，连续两轮**（新增 4000B/6000B 后总数从 23 → 25）。

### 5.2 吞吐对比

| echo 尺寸 | §10 窗口化主机 | 本轮（流水线 + 流控） | 说明 |
|---|---|---|---|
| 1000 B | 14.4–15.6 KB/s | 13.0–14.3 KB/s | 波动范围内持平 |
| 1460 B | 11.3–17.5 KB/s | **19.0–19.1 KB/s** | 提升 ~15-60% |
| 2000 B | 10.4 KB/s（多 1 次 RTT） | **10.1–10.4 KB/s** | 流水线直发，无边界往返 |
| 4000 B | 损坏 FAIL | **8.1–8.9 KB/s** | 新用例，3 段 TCP 下载 + 3 组流水上传 |
| 6000 B | 损坏 FAIL | **6.9–7.4 KB/s** | 新用例，5 段下载 + 5 组上传 |

### 5.3 压力回归

`repro4000.py` 连续 6 轮 4000B echo：全部 `diffs=0`，重传/超时计数正常（每 session 1 次级别，均走安全恢复路径）。烧录后 ICMP NET_PING 正常（rtt 256ms，Windows 防火墙新规则后首包 ARP 延迟）。

---

## 6. 已知限制与后续方向

- **下载方向绝对速率仍受 notify 节拍限制**（连接间隔 15ms × BLE_TX_NUM_EVENT=4 × 244B ≈ 65 KB/s 理论上限，实测 7-19 KB/s 综合往返）；本轮消除的是 dup-ACK/RTO 停滞，不是带宽上限。
- 主机库重启路径的假设：**最老未确认消息 = 最后完成消息**（窗口阻塞保证了这一点）。若将来支持多连接并发分片上传，需引入消息 ID（全局重组缓冲目前也只支持单消息）。
- 全部分片写都丢失的极端场景（设备上条消息 ACK 有效 + 本条完全没收到）会误重放 ACK 吞掉该消息——概率极低（连续 12+ 次写全丢），现有协议无消息 ID 无法区分，接受该风险。
- TX 队列深度 16 无法加大：RAM 已用 58.4/64 KB（bss 末 0x2000e874，栈区 0x2000f800），无余量。
- Fedora 提交链（`cc51e4c…563a66a`）未推 origin；Mac worktree 仅内容镜像（HEAD 仍 `4c11581` + 未提交改动），同步前先 md5 对比（本轮已发现 Mac 的 makefile 落后并造成过一次构建回退）。

---

## 7. 操作记录（复现路径）

1. 编辑：Mac 本地 `~/projects/Openterface/Openterface_KM`（+ `/tmp/bletest` 为测试执行副本）。
2. 同步：scp 至 Fedora `/home/bbot/projects/Openterface/Openterface_KM`（先 md5 对比）。
3. 构建：§0 构建命令（Fedora，MRS 8.2.0 工具链，LTO）。
4. 烧录：`sudo reboot` → 等 ~60s → `python3 tools/ctrl_board.py isp on` → `wchisp flash output/openterface-km-firmware-keymod-plus.bin` → `isp off`（ISP 入口仅重启后首次可靠）。
5. 测试：Mac `/tmp/bletest` 下 `/tmp/bleenv/bin/python -u tests/test_tunnel_full.py --ble-addr 1CC44426-51DA-E45A-73C8-2B01006A2D1E --target 192.168.20.2 --port 9000`。
6. 环境：echo 服务器为 Windows 192.168.20.2:9000（多客户端 PowerShell runspace 版，schtasks TCPEcho9000）；BLE 设备 KeyMod-NCM。
