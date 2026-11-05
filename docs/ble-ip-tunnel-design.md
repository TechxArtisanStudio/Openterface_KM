# 通用 IP-over-BLE 隧道设计方案

- 分支：`fix/stability-p0-p1`
- 基线：`main` @ `19ef5c8`
- 适用固件：KeyMod Mini（mode 4）、KeyMod Plus / KVM-GO（mode 7）、MiniKVM（含 Target ECM 的模式）
- 状态：技术设计完整，但**建议搁置**。控制端确定为手机后（第 1 节核实：iOS/Android 都靠 App 内置 SSH 客户端 + BLE-Eth TCP 代理，均无 raw-IP 地基），本方案不解决当前痛点；第 11 节给出替代优先级

---

## 1. 调查结论：以前没有做过 IP 隧道

先把事实钉住，避免后面的方案建立在错误前提上。

**搜索范围与结果：**

| 位置 | 搜索内容 | 结果 |
|------|----------|------|
| `APP/`、`main.c` | `utun` / `tuntap` / `forward` / `route` / `nat` / `conntrack` / IP 分片重组 | 无命中。固件不存在任何形式的 IP 转发或虚拟网卡概念 |
| `docs/ble-eth-ssh-usage.md:48` | 协议能力边界 | 原文即已声明：`The BLE-Eth tunnel carries TCP only. It does not forward ICMP.` |
| `tools/ble_eth_tunnel.py`（1870 行）、`tools/ssh_through_ble_eth.py`（627 行） | host 侧隧道实现 | 有 `BleTcpProxy`（`ble_eth_tunnel.py:1663`），做的是 **TCP socket ↔ BLE-Eth 的应用层代理**：本地监听一个端口，把字节流送进隧道。完全不碰 IP 层，不建网卡，不改路由表 |
| `keymod-test`（PyQt 端） | tunnel / route / 网卡 | 无 |
| `Openterface_KeyCmd_iOS` | `NEPacketTunnelProvider`、Network Extension target、`com.apple.developer.networking.networkextension` entitlement | 三者都不存在。`KeyCmd/KeyCmd.entitlements` 只有 `com.apple.security.app-sandbox` 与 user-selected files |
| `Openterface_KeyCmd_Android` | `VpnService`、`BIND_VPN_SERVICE`、`ParcelFileDescriptor`、`tun2socks` | 全部不存在。`AndroidManifest.xml` 只声明一个 `BluetoothService`，无 VPN 服务、无 VPN 权限 |

**但手机端确实早就有可用的 SSH，只是它不走 IP 层：**

| | iOS | Android |
|---|-----|---------|
| SSH 客户端 | **libssh2**（`import CSSH`，来自 `Libssh2Prebuild` XCFramework）[SSHClient.swift:3,89](file:///Users/txa/project/Openterface_KeyCmd_iOS/KeyCmd/Services/SSH/SSHClient.swift#L89) | **JSch** `com.github.mwiede:jsch:0.2.17`（`app/build.gradle:204`，`SshClient.java`） |
| 套到 BLE 上的手法 | `socketpair(AF_UNIX, SOCK_STREAM)` 桥，libssh2 以为自己在读写一个普通 fd（[SSHClient.swift:385-407](file:///Users/txa/project/Openterface_KeyCmd_iOS/KeyCmd/Services/SSH/SSHClient.swift#L399)） | `session.setSocketFactory(...)` + 自定义 `BleEthSocket extends Socket`，其 `connect()` 直接抛异常 |
| 实际字节通路 | `BleEthTransport` 造 `CONNECT 0x10` + `DATA 0x11` 分片帧 | 同一套协议，Java 版 `BleEthTransport.java` |

也就是说：**手机 SSH = App 内置 SSH 客户端 + BLE-Eth TCP 代理，与 IP 层完全无关。** 这正好解释了"0.0.4 / 0.0.5 就能用"：`0b666f8`（2026-05-22，support ble-ecm path）已引入 `0x10/0x11/0x1F`，`a11973c`（2026-06-05，add SSH over BLE-Eth functionality）与 `5211f03`（2026-06-18，`SET_DEFAULT 0x20`）都在 tag `v0.0.4`（33f4ed5，2026-07-25）之前，`v0.0.5`（321d4e4，2026-09-24）之后同理。隧道那套 4 元组匹配逻辑在 `v0.0.4:ble_eth_protocol.c:1282` 与今天 `:1714` **逐字段相同**——手机 SSH 依赖的固件语义这一年来没变过。

**所以记忆中"以前做过的"是两件事**：`tools/ble_eth_tunnel.py:1663` 的 `BleTcpProxy`（PC 侧代理）和两个手机 App 的内置 SSH 客户端（App 侧代理）。两者都是**应用层逐协议手工适配**，不碰 IP 层、不建网卡、不改路由表。通用 IP-over-BLE 是另一件事，确实从未做过。

本方案要交付的**增量价值**因此收窄成一句话：让**没有内置适配的**程序（`ping`、UDP、mDNS、任意第三方 App）不改代码就能走蓝牙。**已经适配好的 SSH 一点好处都得不到。**

---

## 2. 目标与非目标

### 目标

1. BLE 链路成为设备的**第三条 IP 通路**（现有两条：Host USB ECM、Target USB ECM），且优先级不依赖 USB 插入。
2. host 侧 `ping <target_host_ip>` 与 `ssh <target_host_ip>` 直接可用，无需 `-p`、无需代理配置。
3. 默认关闭，一条命令开、一条命令关；关闭时行为与今天的固件逐字节一致。
4. 不破坏 HID 键鼠实时性（这是本产品的主业，隧道是附加功能）。

### 非目标

1. 不追求带宽。BLE 链路做不了高吞吐，也不该替代 USB ECM 走大文件。
2. IPv6、多跳路由、TLS offload、DHCP server over BLE —— 全部不做。
3. Phase 1 不支持 iOS（前提条件在第 8 节，缺 entitlement）。
4. 不修 Host ECM ↔ Target ECM 的 L2 桥（第 5.5 节解释为什么它和本方案无关，以及为什么它是另一个独立 bug）。

---

## 3. 现有可复用资产

方案不需要新的底层管道，所有钩子都已存在并在硬件上跑通过：

| 能力 | 位置 | 用途 |
|------|------|------|
| 向 Target 链路注入裸以太网帧 | `ECM_Bridge_SendRawFrame()` [APP/ecm_bridge.c:2592](file:///Users/txa/project/Openterface_KM/APP/ecm_bridge.c#L2592) | IP 转发的出口。BLE-Eth 已在用它：[`ecm_bridge_tx_handler()` APP/peripheral.c:537](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L537) |
| 收 Target 侧以太网帧 | `handle_target_frame()` [APP/ecm_bridge.c:2651](file:///Users/txa/project/Openterface_KM/APP/ecm_bridge.c#L2651) | IP 转发的入口；`:2686` 处已在把帧交给 `BLE_Eth_HandleTargetFrame()` |
| 从以太网帧里提取 IPv4 并按协议分派 | `BLE_Eth_HandleTargetFrame()` [APP/ble_eth_protocol.c:1714](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L1714) | 本方案的天然泛化点：现在的逻辑是"取 IPv4 头 → 只认 proto==6(TCP)，其余 return false"，只需把"只认 TCP"换成"交给 IP router" |
| 帧重组 + 分片 + ACK 状态机 | `g_reassembly`（1536 B）、`BLE_Eth_PushTargetData()` [APP/ble_eth_protocol.c:1238](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L1238) | 承载 IP 包，不需要新写传输层 |
| BLE 写帧路由白名单 | `Peripheral_IsBleEthCommandByte()` [APP/peripheral.c:239](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L239) | 新增控制命令必须在此登记，否则会被 CH9329/HID 路径二次消费（`STABILITY_FIX_PLAN` P0-2 的成因） |
| Target 链路 ARP / GARP | `handle_arp_frame()` [APP/ecm_bridge.c:293](file:///Users/txa/project/Openterface_KM/APP/ecm_bridge.c#L293)、`g_target_garp_sent` | 转发前需要解析 `target_host_ip` 的 MAC，机制已有 |
| ICMP 请求/应答（两侧） | `handle_icmp_packet()`（host 侧）、`handle_target_icmp_packet()` [APP/ecm_bridge.c:967](file:///Users/txa/project/Openterface_KM/APP/ecm_bridge.c#L967) | 隧道打开后 ICMP 会自然流经转发路径，这两个 handler 只在"目的就是我自己的 IP"时兜底，不冲突 |
| ECM IP 配置页 | `ECM_IPConfig` [APP/include/ecm_ip_config.h:29-42](file:///Users/txa/project/Openterface_KM/APP/include/ecm_ip_config.h#L29) | 提供 `dev_ip` / `target_dev_ip` 等现成地址；SNAT 出口直接取 `target_dev_ip` |

---

## 4. 硬约束（全部已在代码/构建产物中核实）

| 约束 | 数值 | 位置 | 对方案的影响 |
|------|------|------|--------------|
| **链路层单包上限** | `BLE_BUFF_MAX_LEN = 64` ⇒ **ATT_MTU 最大 60** | [HAL/include/config.h:115](file:///Users/txa/project/Openterface_KM/HAL/include/config.h#L115)、`ATT_MTU = BufMaxLen-4` [LIB/wchble.h:91](file:///Users/txa/project/Openterface_KM/LIB/wchble.h#L91)、`build/makefile:256` | **这是真正的第一瓶颈。** MTU 60 ⇒ 单次 notification 净数据最多 **57 B**，App 侧早就是按这个数写的（Android `SAFE_BLE_STREAM_CHUNK = 57`） |
| **每个连接事件可发包数** | `BLE_TX_NUM_EVENT = 1` | [HAL/include/config.h:121](file:///Users/txa/project/Openterface_KM/HAL/include/config.h#L121)、`build/makefile:259` | 1 packet / interval，不是"多个"。第 4 节吞吐必须按 1 算 |
| 控制器缓冲包数 | `BLE_BUFF_NUM = 5` | [HAL/include/config.h:118](file:///Users/txa/project/Openterface_KM/HAL/include/config.h#L118) | 与 `BufMaxLen` 一起决定 `BLE_MEMHEAP_SIZE` 需求 |
| 代码里的 notification 上限 | 180 B —— **当前配置下永远取不到** | `BLE_ETH_SAFE_NOTIFY_LEN` [APP/peripheral.c:46](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L46)，`max_notify_len = min(peripheralMTU-3, 180)`（[APP/peripheral.c:697-707](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L697)） | `peripheralMTU-3 ≤ 57`，所以 `min()` 恒等于 57。180 这个数是**可达性上限，不是现状** |
| 协议允许的单片净荷 | 246 B（`249-3`） | `BLE_ETH_MAX_PAYLOAD_LEN` [APP/include/ble_eth_protocol.h:41](file:///Users/txa/project/Openterface_KM/APP/include/ble_eth_protocol.h#L41)、`BLE_ETH_FRAG_HEADER_LEN=3` | 在 MTU 60 下，一个 246 B 片要吃 **5 个 notification**；`BLE_Eth_GetNextTxChunk`/`ConsumeTxChunk` 做的是保留余数的续传，不丢数据 |
| TX notification 队列 | 8 槽 × 255 B | `BLE_ETH_TX_QUEUE_DEPTH` [APP/ble_eth_protocol.c:37](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L37) | 突发即溢出，`tx_queue_full_drop` 计数可见 |
| RX 重组缓冲 | **1536 B，且全局只有一份** | `g_reassembly.data` [APP/include/ble_eth_protocol.h:316](file:///Users/txa/project/Openterface_KM/APP/include/ble_eth_protocol.h#L316) | 单个 IP 包必须 ≤ 6×246=1476 B；**且 IP 通道与 TCP 隧道抢同一个重组器**（第 6.2 节） |
| 分片计数只有 4 bit | ≤ 15 片 | `BLE_ETH_FRAG_COUNT_MASK 0x0FU` | 单消息硬上限 3690 B，够用 |
| TCP/隧道连接槽 | **2** | `BLE_ETH_MAX_CONNECTIONS` [APP/include/ble_eth_protocol.h:103](file:///Users/txa/project/Openterface_KM/APP/include/ble_eth_protocol.h#L103) | 不能再走"每条连接一个槽"的路子，见第 6.1 节 |
| ECM 每方向队列深度 | **2** 帧，帧缓冲 1514 B | `ECM_BRIDGE_QUEUE_DEPTH` / `ECM_BRIDGE_MTU_FRAME_SIZE` [APP/ecm_bridge.c:23-24](file:///Users/txa/project/Openterface_KM/APP/ecm_bridge.c#L23) | 转发必须做"队列满即丢包"，不能阻塞主循环 |
| 空闲 RAM | **约 10.76 KB**（`_end`→`_heap_end` gap） | `docs/internal-memory-usage.md:86`；当前 keymod-mini 报告 RAM 77.95% | 新增静态必须逐字节预算，见第 7 节 |
| BLE conn interval（申请值） | 10 × 1.25 ms = **12.5 ms** | `DEFAULT_DESIRED_MIN/MAX_CONN_INTERVAL` [APP/peripheral.c:65-68](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L65) | central 有权不采纳；实际 interval 须 Phase 0 实测 |
| App 写路径 | iOS：128 B 块 + 10 ms 间隔 + 全部 `.withResponse`（[BLEManager.swift:19,424-477](file:///Users/txa/project/Openterface_KeyCmd_iOS/KeyCmd/Managers/BLEManager.swift#L19)）；Android：57 B 块 + 10 ms + `WRITE_TYPE_DEFAULT` | 两端注释都写明"固件需要 BLE 层 ACK 才转发" | **上行被 write-with-response 串化成一次一个往返**，这是 App 层能改的最大一笔，与固件无关 |

**吞吐预算（按当前实际配置，非理想配置）：** MTU 60 ⇒ 每 notification 最多 57 B 净数据；`BLE_TX_NUM_EVENT = 1` ⇒ 每 12.5 ms 一个包 ⇒ **单向裸上限 57 B / 12.5 ms ≈ 4.56 kB/s ≈ 36.5 kbit/s**（还未计协议头与 ACK）。

落到 BLE-Eth 帧上：一个 246 B 的分片 = 255 B 帧 = 5 个 notification ≈ **62.5 ms**，且现在每个入向分片还回一条 DATA_ACK（[APP/ble_eth_protocol.c:590](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L590)），ACK 又占掉出向的槽 ⇒ 双向对拍时实际有效吞吐再打折。这与"App 用 write-with-response"叠加，就是手机 SSH 能连上但用起来发涩的完整解释。

**本方案的第 4 节因此不能独立成立**：如果 `BLE_BUFF_MAX_LEN` / `BLE_TX_NUM_EVENT` 不先解决，无论走 IP 隧道还是走现有 TCP 代理，体验都卡在同一个 36.5 kbit/s 的墙上。**优先级应高于本方案**（见第 11.5 节）。

---

## 5. 关键设计决策

### 5.1 承载裸 IPv4，不承载以太网帧

诱惑是把 Target ECM 的以太网帧原样搬到 BLE 上（"真·网桥"）。**不做**，理由：

1. 一旦透传 L2，host 侧就必须对 Target 子网做 ARP。而设备在 host 侧的 ARP 只对**自己的 IP** 应答 —— `handle_arp_frame()` [APP/ecm_bridge.c:293](file:///Users/txa/project/Openterface_KM/APP/ecm_bridge.c#L293) 只匹配 `ecm_dev_ip` / `ecm_target_dev_ip`。透传 L2 就得实现 ARP 代理（对整段子网全部应答），复杂度立刻上一个台阶。
2. `utun` 是点对点设备，OS 在 tun 层根本不做 ARP，直接给它 IP 包。**裸 IPv4 让 host 侧的 ARP 问题整体消失。**
3. 省掉 14 B 以太网头 + 4 B FCS，等于每片白送 18 B 净荷。
4. 帧内 MAC 地址在 BLE 这条链路上没有任何语义，透传等于把无关状态引入协议。

BLE 上只跑 IPv4 包（不含链路层），设备两端自己做 MAC 解析。

### 5.2 隧道 MTU = 576

不是随便取的：

- `576` 是 RFC 791 要求所有 IPv4 实现必须能重组的最小数据报，任何目标机都不会拒绝。
- 分片尺寸**保持协议现有的 246 B**，不要为了"一片一个 notification"去缩小它。在 MTU 60 的现实下 `246 B` 片本来就要 5 个 notification 发完，缩到 48 B 总 notification 数不变，但**每片一条 DATA_ACK 的开销会翻 5 倍**（ACK 逻辑见 [APP/ble_eth_protocol.c:590](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L590)）。`ceil(576/246) = 3` 片，落在 `BLE_Eth_PushTargetData()` 的 6 片上限内（[APP/ble_eth_protocol.c:1246](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L1246)），也在 1536 B 重组缓冲内 —— **两处上限都不用改**。
- 一个 576 B 包在空中的代价：3 片 × 5 notification = 15 个 notification，按 1 packet / 12.5 ms ≈ **188 ms 单向**。这决定了隧道只能承载小报文交互，不承载吞吐。
- TCP 侧把 MSS 夹到 `536`（576 − 20 IP − 20 TCP），`ssh` 从源头就不产生超限包，99% 流量天然合规。
- 超限的 UDP 包：**不分片、直接丢**，并在 Phase 3 回 `ICMP Fragmentation Needed`。Phase 1 先丢 + 计数。

### 5.3 BLE 侧独立子网 + SNAT 出 Target 链路

BLE 隧道用**自己的**子网，绝不复用 `192.168.30.0/24`（那是 ECM 配置页的语义空间，且默认值里 host/target 两侧本就在同一个 /24，混用必然和物理 ECM 适配器的路由打架）。

```
默认参数（全部可经 0x25 覆盖）：
  tunnel subnet   10.88.0.0/24
  host 侧 utun    10.88.0.2   ← OS 视角"我自己的地址"
  device 侧网关   10.88.0.1   ← utun 的 default/next-hop
  SNAT 出口 IP    = target_dev_ip（默认 192.168.30.5），取自 ECM_IPConfig
  路由注入        route add -net 192.168.30.0/24 10.88.0.1   （只加这条，不动 default）
```

转发规则只有两条：

- **BLE → Target**：源 IP 是 `10.88.0.0/24` 的包 → 查/发 Target 链路 ARP 得 `target_host_ip` 的 MAC → 重写 `src_ip = target_dev_ip`、重算 IP 校验和 → `ECM_Bridge_SendRawFrame()`。
- **Target → BLE**：`handle_target_frame()` 里 `dst_ip == target_dev_ip` 且 conntrack 命中的包 → 按表反向还原 `dst_ip` → 推入 BLE 通道。

用 SNAT 而不是双向 NAT64 式的路由广播，是因为 **Target 机器不需要任何配置、不需要回程路由**——它眼里所有连接都来自隔壁 `192.168.30.5`。这是本方案"目标侧零侵入"的关键，也是 KVM 场景（那台机器是别人的，你不能要求它装驱动加路由）唯一现实的形态。

### 5.4 数据面复用 DATA(`0x11`)，用 `conn_id = 0xFE` 标识 IP 通道

分片头里的 `conn_id` 是独立一字节，而连接槽只有 `0..1`（`BLE_ETH_MAX_CONNECTIONS`），`0xFE` 是天然可保留的"虚拟通道号"。

选它、而不是新加命令字节或占 `frag_flags` 空闲 bit 的理由：

1. 帧的构造、校验、ACK、重组、背压**一行都不用改**——`BLE_Eth_PushTargetData()` 和 RX 重组器只认 `conn_id`。
2. 唯一改动是 dispatch 处加一个分支：`if (conn_id == BLE_ETH_IP_CONN_ID) → ip_router_input()`，不走 `g_connections[]`。
3. `frag_flags` 的 bit5/bit4 虽然空闲，但占用旧 host 无法识别的 bit 会让 `FrameParser` 行为不可预期；`conn_id` 是 host 本来就要读的字段，改动面更小、可回退。
4. `CONNECT(0x10)` / `DISCONNECT(0x12)` / `ALLOC` 等所有按 `conn_id < BLE_ETH_MAX_CONNECTIONS` 校验的路径，只需加一条 `!= 0xFE` 守卫（与已落地的 `!is_icmp` 守卫同一模式，见 [APP/ble_eth_protocol.c:502](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L502)）。

host 侧 `tools/ble_eth_tunnel.py` 的 `DataReassembler` / `FrameParser` 需同步放开 `conn_id` 取值校验，这是 Phase 2 的活。

### 5.5 控制面：新增 `0x25 SET_IP_MODE` / `0xA5`

命令字节选择已核对过冲突：CH9329 占用 `0x22/0x23/0x26/0x27`（`CMD_BLE_SECURITY`、`CMD_BLE_SET_NAME`、`CMD_SET_ECM_IP_CONFIG`、`CMD_GET_ECM_IP_CONFIG`，见 [APP/include/usbd_composite_km.h:51-57](file:///Users/txa/project/Openterface_KM/APP/include/usbd_composite_km.h#L51)），BLE-Eth 已用 `0x10-0x12/0x1E/0x1F/0x20-0x24`。**`0x25` 在两个地址空间都干净**，`0xA5` 同理。不要再碰 `0x26/0x27`——那是历史遗留的撞号区。

```
0x25 SET_IP_MODE 请求 payload：
  [0]     enable            0=off, 1=on
  [1-4]   tunnel_host_ip    （off 时忽略；默认 10.88.0.2）
  [5-8]   tunnel_gw_ip      （默认 10.88.0.1）
  [9-12]  tunnel_subnet     （默认 255.255.255.0）
  [13-14] mtu               大端，仅接受 <= 576，其余拒绝为 PARAM_ERR
  [15]    forward_icmp      0=只转 TCP/UDP，1=也转 ICMP

0xA5 响应 payload（8 B）：
  [0] status  [1] active  [2] mtu  [3] fwd_icmp
  [4-5] fwd_pkts_out (BE16)  [6-7] fwd_pkts_in (BE16)
```

`enable` 必须是**运行时开关，绝不写 Flash**（`STABILITY_FIX_PLAN` 已把"配置写入无意义擦 Flash"列为问题）。隧道状态是易失的，掉电即关。

### 5.6 conntrack：8 条静态表项，不做大而全

```c
typedef struct {
    uint32_t orig_dst_ip;   /* host 侧看到的 Target IP */
    uint32_t orig_src_ip;   /* utun 侧源 IP */
    uint16_t orig_src_port;
    uint16_t nat_src_port;  /* SNAT 分配的源端口 */
    uint8_t  proto;         /* 6 / 17 / 1 */
    uint16_t icmp_id;       /* proto==1 时复用 */
    uint32_t last_ms;       /* 空闲 30 s 老化 */
} ip_ct_entry_t;            /* 24 B × 8 = 192 B */
```

只做 5 元组对称匹配 + 端口回收，不做状态跟踪、不做 TCP 状态机（那是 `ble_eth_conn_t` 的事，这里用不上）。8 条对"一台 Mac 同时开 3 个 ssh + ping + scp"足够；打满即拒新流并计数，不静默丢。

---

## 6. 架构与数据通路

```
            ┌──────────────────────── host (Mac / Linux / Android) ──────────────────────┐
            │  apps (ssh / ping / scp / 任意)                                            │
            │        │ IP 路由: 192.168.30.0/24 → 10.88.0.1                              │
            │  utun / VpnService  (10.88.0.2)                                            │
            │        │ 裸 IPv4 包                                                        │
            │  tools/ble_ip_tunnel.py 或 Android VPN 服务                                 │
            └────────┼──────────────────────────────────────────────────────────────────┘
                     │  BLE GATT: FFF2 write / FFF1 notify
                     │  BLE-Eth DATA(0x11), conn_id=0xFE, 分片≤171B, MTU 576
   ══════════════════╪═══════════════════════════════════════════════════════════════════
                     │  APP/ble_eth_protocol.c
        RX: 重组完成 ─┤→ conn_id==0xFE? ─→ ip_router_input() ─→ conntrack ─→ 建以太网帧
                      │                          │                      └→ ECM_Bridge_SendRawFrame()
        TX: ip_router_output() ←──────────────────┤                         → g_host_to_target → EP7 IN
                      │  handle_target_frame() 拦截 dst==target_dev_ip      → Target USB ECM
                      └→ ecm_bridge.c:2651/2686                             → Target PC (192.168.30.2)
   ═══════════════════════════════════════════════════════════════════════════════════════
        并存但互不影响： Host USB ECM (USBD)  |  HID 键鼠 (target 侧 EP)  |  CH9329 命令
```

### 6.2 与现有 TCP 隧道 / 0x24 NET_PING 的关系（必须先讲的取舍）

**全局只有一份 `g_reassembly`**，所以 IP 通道、TCP 隧道、`0x24` 三者**不可能并行接收大消息**。Phase 1 采取最诚实的做法：`SET_IP_MODE(enable=1)` 时**主动拆掉现有 TCP 隧道并拒绝新的 `CONNECT`**（返回 `CONN_FULL`），关闭后恢复。这不是偷懒，是把"共享重组器"这个既成事实显式化，而不是造一个会在并发下拼帧的隐藏状态机。

隧道打开后 `ping` **免费可用**（ICMP 只是被转发的又一个 proto）。`0x24 NET_PING` 不废弃：它是**唯一不需要 host 侧任何特权**的探测手段（macOS 上 utun 要 `sudo`，`0x24` 不要），二者职责不重叠。

---

## 7. 内存与 Flash 预算

| 项 | 字节 | 说明 |
|----|------|------|
| `ip_ct_entry_t[8]` | 192 | 第 5.6 节 |
| Target 链路 ARP 缓存 `4 × 12` | 48 | 复用现有 ARP 逻辑，只加缓存 |
| 入向 IP 暂存 `576 × 1` | 576 | 出向可直接从 `g_reassembly.data` 偏移引用，不额外缓冲 |
| 计数器 + 配置影子 + `ip_router` 状态 | ~96 | |
| **合计** | **约 0.9 KB** | 空闲 10.76 KB 的 **8.5%** |

Phase 1 结束时必须在 4 个目标上重跑 `size` 复核（keymod-mini 现 RAM 77.95% / FLASH 48.63%；`build.sh` 一次只接受**一个** target，多 target 要循环调用）。**若 RAM 占用超 82%，优先砍掉入向暂存缓冲改为直接引用 ECM 帧缓冲，而不是扩大 `g_reassembly`。**

Flash 增量估算 < 2 KB（新增 `APP/ip_router.c` 一个文件 + dispatch 分支），当前 48.6% 无风险。

---

## 8. host 侧虚拟网卡：各平台权限现实

这是决定"能不能落地"的外部约束，比固件工作量更硬。

| 平台 | 机制 | 权限 | 结论 |
|------|------|------|------|
| macOS | `utun`（`/dev/tun` 已废弃，走 `UTUNCONTROL` ioctl，或 `IPUtil` / NEPacketTunnelProvider） | **需要 root**；免 root 只有 `NEPacketTunnelProvider` 一条路，且要签名 App + entitlement | CLI helper + `sudo` 可接受；产品化必须走 NE |
| Linux | `/dev/net/tun` + `TUNSETIFF` | root 或 `CAP_NET_ADMIN` | 最顺；可给 helper 二进制 setcap，不必长期 root |
| Windows | Wintun（需管理员）/ tun2socks | **管理员** | 可行；`tools/` 里已有 Windows 脚本生态可挂 |
| Android | `VpnService.establish()` | **无需 root**，仅一次系统授权弹窗 | 唯一免特权的正统通道，且 `VpnService` 天生就是"点对点拿裸 IP 包"，与本方案对齐。**但 KeyCmd_Android 今天一行 VPN 代码都没有**（第 1 节核实），要从零加 `VpnService` + 用户授权流 + 前后台生命周期 |
| iOS | `NEPacketTunnelProvider` | 付费 Apple Developer + **Network Extension entitlement 审批** + 全新 app-extension target | KeyCmd 三者皆无（第 1 节）。**不是工作量问题，是准入问题**，属独立立项 |

**这一节在本轮核实后权重上升了。** 控制端既然确定是手机，那么两个手机平台**都没有任何 raw-IP 基础设施**，本方案不是"接着做下去"而是"从零建两套"：Android 要新写 `VpnService`，iOS 要等 entitlement 审批 + 新建 extension target。与之对照，两个 App 都已经把 SSH 做到可用（libssh2 / JSch + BLE-Eth），**"手机能 ssh"这件事对 IP 隧道的需求是零**。

---

## 9. 实施阶段

### Phase 0 — 可行性实测（不动固件，0.5 天）

产出决定后续所有数字是否成立，**必须先做**：

1. macOS 连接后**实测协商到的 conn interval** 和实际 MTU（central 可以不采纳 12.5 ms；`peripheralMTU` 由 `exchangeMTUReq.clientRxMTU` 得来，[APP/peripheral.c:793](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L793)）。
2. 用 `0x1E PING` / `0x11 DATA` 压测 notification/s，校准第 4 节吞吐预算，特别是**每片 ACK 吃掉一半槽位**这个折扣到底多大。
3. 单独验证 `sudo` 下 utun 建接口 + 注路由 + 读写裸 IPv4 包的 Python 通路（不涉及设备）。

若实测净吞吐 < 20 kbit/s，Phase 1 应当降级为"只做 ICMP 转发（让 `ping` 通），SSH 继续走现有 TCP 代理"。

### Phase 1 — 固件 IP 层（2–3 天）

| 改动 | 位置 |
|------|------|
| 新增 `APP/ip_router.c` + `APP/include/ip_router.h`：`ip_router_input/output`、conntrack、target 链路 ARP 解析、校验和 | 新文件 |
| IPv4 提取泛化：把 `BLE_Eth_HandleTargetFrame()` 的 `if (ip[9] != 6) return false` 改为 `else → ip_router_input()` | [APP/ble_eth_protocol.c:1714](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L1714) |
| RX 重组 dispatch 认 `conn_id == 0xFE` | `process_fragment()` 附近（[APP/ble_eth_protocol.c:584-604](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L584)） |
| `conn_id` 分配/`CONNECT`/`DISCONNECT` 加 `!= 0xFE` 守卫 | `BLE_Eth_AllocConnection()`、[APP/ble_eth_protocol.c:502](file:///Users/txa/project/Openterface_KM/APP/ble_eth_protocol.c#L502) |
| `0x25` dispatch + `0xA5` 响应 + INFO feature bit 5 `BLE_ETH_FEAT_IP_TUNNEL 0x20U` | `ble_eth_protocol.c` + [APP/include/ble_eth_protocol.h:71-103](file:///Users/txa/project/Openterface_KM/APP/include/ble_eth_protocol.h#L71) |
| `Peripheral_IsBleEthCommandByte()` 登记 `0x25` | [APP/peripheral.c:239](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L239) |
| MSS 夹到 536 / MTU 通告 | `ip_router` 内 TCP MSS 改写（Phase 3 可先不做，576 已够 ssh） |
| 模式门控：仅在 `USB_ModeConfig_ActiveModeHasTargetCdcEcm()` 为真时允许 enable | [APP/usb_mode_config.c:233](file:///Users/txa/project/Openterface_KM/APP/usb_mode_config.c#L233) |
| 文档 | `docs/ble-eth-protocol.md` 加 `0x25` 章节 + feature bit；新增 `docs/ble-ip-tunnel-usage.md` |

**构建门槛**：4 个 target 全绿 + `size` 在第 7 节预算内。
**硬件门槛**：`ping` 0% loss、`ssh` 可交互、HID 键鼠无丢键（观察 HID queue overflow 计数不增长）。

### Phase 2 — host 侧 helper（1–2 天）

`tools/ble_ip_tunnel.py`：BLE 连接 → `0x25 enable` → 建 utun/TUN（`--interface` 交给 OS）→ 双向搬裸 IPv4 → `--routes` 注入 Target 子网 → 退出时 `0x25 disable` + 拆路由。macOS/Linux 一套，Windows 单独 `wintun` 分支。

### Phase 3 — 稳定化（按 Phase 0/1 数据决定优先级）

1. **分片 ACK 合批**（每消息一次 ACK 或按 rx_window 累积），这是吞吐最大的一笔免费收益。
2. `ICMP Fragmentation Needed` 回给 host，让 PMTUD 工作。
3. 与 TCP 隧道共享重组器的**串行化调度**（若产品要求二者并存，需给 IP 通道独立 1536 B 重组器，代价 +1.5 KB RAM —— 届时重新评估）。
4. BLE 断连/重连、Target 重枚举后的隧道自动恢复。
5. conntrack 满 / ECM 队列满 / TX 队列满三类丢弃的可观测统计（扩展 `0xA5` 或加 `0x28 GET_IP_STATS`）。

### Phase 4 — Android `VpnService`（独立，未估）
### Phase 5 — iOS Network Extension（前置：entitlement 审批 + 新 extension target，未估）

---

## 10. 风险与未决问题

| # | 风险 | 严重度 | 处置 |
|---|------|--------|------|
| R1 | **安全面扩大**：BLE 配对一旦成为 IP 入口，任何配对成功的主机即获得 Target 内网入口，且 BLE 无链路层鉴权（当前是裸 GATT） | **高** | 默认关闭；`enable` 需要走 `0x22 CMD_BLE_SECURITY` 已建立的 bonded 会话；隧道设 10 分钟无流量自动关；只允许 `target_host_ip` 单主机，不做整段子网转发 |
| R2 | 吞吐可能达不到 SSH 舒适线（Phase 0 才能确认） | 中 | Phase 0 是闸门；不达标就退回"仅 ICMP 转发" |
| R3 | 单重组器导致 IP 隧道与 TCP 隧道互斥，用户会当成 bug 报 | 中 | 明确写进 `docs/ble-eth-protocol.md`；`0xA5` 回 `active`；UI 上开隧道即显示"TCP proxy 不可用" |
| R4 | SNAT 后 Target 侧所有连接源都是 `192.168.30.5`，排障时不可追溯到人 | 中 | conntrack 表保留原始 `orig_src_ip`，经 `GET_IP_STATS` 导出映射关系 |
| R5 | 每片一 ACK 的现有行为在 IP 通道上放大，可能表现为"ping 通但 scp 卡死" | 中 | Phase 3 第 1 项；Phase 1 明确不承诺大文件 |
| R6 | 转发路径与 `handle_target_icmp_packet()`（目的为自己的兜底应答）在 `dst_ip==target_dev_ip` 时先后顺序若写错，会出现"设备替目标机应答 ping" | 中 | dispatch 顺序固定为：conntrack 命中 → 转发；未命中且 dst==本机 → 兜底。写进单元测试用例 |
| R7 | macOS 上 `utun` 地址与真实 `en` 适配器路由打架 | 低 | 用独立 `10.88.0.0/24` + 只加 Target 子网明细路由，绝不设 default |
| R8 | 已知的 Host ECM ↔ Target ECM L2 桥本身是坏的：`handle_arp_frame()` 只对本机 IP 应答，而 DHCP 发的是 /24，所以文档里"Host PC 与 Target PC 可互 ping"在 USB 路径上根本不成立 | — | **本方案不修它，也不依赖它**（BLE 走裸 IPv4，绕开 ARP）。但要单独立项，否则用户会拿 USB 桥的预期来评判隧道 |

---

## 11. 作为工程师的判断：本方案建议**搁置**，先做三件更便宜的事

控制端已确定为手机（iOS + Android 双端都在，且都已核实）。这个事实把结论定死了。

1. **手机 SSH 与 IP 隧道无关。** 两端各自内置了完整 SSH 客户端（iOS libssh2 + `socketpair` 桥，Android JSch + 自定义 `SocketFactory`），底下就是 BLE-Eth 的 `0x10/0x11/0x12`。**做完 IP 隧道，手机 SSH 一个字节都不会变好。** "0.0.4/0.0.5 能用"印证的是 BLE-Eth TCP 代理这条路一直通，不是一条待补的 IP 通路。

2. **两个手机平台都没有 raw-IP 地基，iOS 还卡在准入而非工作量。** Android 要从零加 `VpnService`；iOS 要 Network Extension entitlement 审批 + 新建 extension target。固件侧只要 0.9 KB RAM 和 2–3 天，**但方案的真实成本 90% 在 App 侧和账号侧**，这个账在本轮核实前我是估不准的，现在能确定：不对称，不值。

3. **真正卡住手机体验的是链路层配置，不是协议层次。** `BLE_BUFF_MAX_LEN = 64` ⇒ ATT_MTU 只能到 60 ⇒ 单次 notification 净数据 57 B（Android 早就按 57 写死了），加上 `BLE_TX_NUM_EVENT = 1` ⇒ **每 12.5 ms 只能发一个包 ≈ 36.5 kbit/s 单向裸上限**。`BLE_ETH_SAFE_NOTIFY_LEN 180` 在当前配置下是永远取不到的死代码。SSH 之所以"能用但发涩"，根在这里。**换任何协议层次都绕不开这堵墙；把它抬上去，SSH、`0x24` ping、未来隧道同时受益。**

### 建议的实际优先级

| 顺位 | 事项 | 成本 | 收益 |
|------|------|------|------|
| **1** | 试 `BLE_BUFF_MAX_LEN=251`（ATT_MTU 247）+ `BLE_TX_NUM_EVENT=4`。两者都是 `build/makefile:256,259` 的 `?=` 变量，改值即可，不动源码；**但要同步把 `BLE_ETH_SAFE_NOTIFY_LEN` 从 180 抬到 244**（[APP/peripheral.c:46](file:///Users/txa/project/Openterface_KM/APP/peripheral.c#L46)），否则 MTU 抬上去后 180 立刻变成新的封顶 | 0.5 天 + 一次 `BLE_MEMHEAP_SIZE`（现 7 KB，[HAL/include/config.h:112](file:///Users/txa/project/Openterface_KM/HAL/include/config.h#L112)）复核；WCH 库内部缓冲需求**未在源码可见**，必须实测连接稳定性 | 单次 notification 净数据从 57 B 到 235 B，再乘 4 包/event，理论上限从 36.5 kbit/s 抬到 ≈ **780 kbit/s**；即使只做到一半也是量级改善 |
| **2** | App 侧把 write-with-response 的强制策略改成"仅控制帧用 withResponse，DATA 用 noResponse" | 两端各几行；**注意两端注释都记录了当初改成全 withResponse 是因为"handshake OK 但按键到不了服务端"**（`BLEManager.swift:438-467`，见 `docs/ble-eth-protocol-spec.md:277-298`），所以这条必须先复现那个故障再动 | 上行不再一次一个往返，与第 1 项叠加才有效 |
| **3** | 手机端补 ping：固件 `0x24 NET_PING` 已实现（未提交），**两个 App 都还没有任何 ping 功能**（iOS 无 `0x24`；Android 连 `0x1E` 都没定义）。各加一个调用 + UI 即可，**零特权** | 每端约 0.5 天 | 直接满足"通过蓝牙 ping 通"的原始诉求 |
| **4** | 修默认目标 IP 不一致：Android `DEFAULT_KEYCMD_HOST = 192.168.11.2`、iOS `defaultKeyCmdHost = 192.168.12.1` 且对话框默认 `192.168.11.2`、固件 fallback `192.168.11.2:22`，而现场 ECM 子网是 `192.168.30.0/24`（Target PC = `.30.2`）。`docs/ble-eth-ssh-usage.md` 与 `ble-eth-tunnel-examples.md` 自己就写了两个不同的 target IP | 小；或让 App 用 `0x23 GET_ECM_IP` 自动发现 | **这很可能是"手机 ssh 连不上"的真实原因**，而且它和 IP 隧道毫无关系 |
| 5 | 本 IP-over-BLE 方案 | 固件 2–3 天 + 两套 App 的 raw-IP 地基 + iOS 账号审批 | 仅在出现"必须跑第三方未适配 App"的产品需求时才开工。届时从 Phase 0 重新校准第 4 节数字 |

**一句话**：本方案的每一处技术设计都成立，但它在手机这个控制端上解决的不是用户此刻的痛。上面 1–4 项加起来不到一周，收益远大于隧道。

---

## 12. 验收标准（Phase 1–2）

1. macOS：`sudo python3 tools/ble_ip_tunnel.py --name keymod` 起来后，`ping -c 4 192.168.30.2` **0% loss**，且**不需要**在 Target 机器上加任何配置。
2. `ssh user@192.168.30.2` 交互可用，`exit` 后 `0x25 disable` 生效。
3. 隧道开启时，键鼠 HID 无丢键、无卡顿（HID queue overflow 计数为 0）。
4. `0x1F INFO` 的 feature bit 5 正确反映固件能力，旧 host 工具遇到不支持的固件能明确报错而非静默超时。
5. BLE 断连 → 重连 → 重新 enable，隧道可用；Target 侧 `SET_INTERFACE(alt=1)` 重枚举后隧道可用。
6. 4 个构建目标（keymod-mini / keymod-plus / kvmgo / minikvm）全绿，`size` 的 RAM/FLASH 增量在第 7 节预算内。
7. `docs/ble-eth-protocol.md` 的 `0x25` 章节字段与实现逐字节一致，示例帧经 `tools/debug_ble_eth.py` 校验通过。
