# KeyMod / KVM-GO BLE OTA 升级设计方案

- 日期：2026-11-11
- 基线：`feature/ble-pairing-security` @ `bf30ece`
- 适用固件：KeyMod（`BOARD_VERSION=3`）、KVM-GO（`BOARD_VERSION=1`）等 `ENABLE_BLE` 机型
- 状态：**方案阶段，未实现**。里程碑见 §12，全部未开始
- 依赖：BLE 配对安全方案（[ble-pairing-design.md](ble-pairing-design.md)）的加密准入先落地；OTA 建议在其之后开新分支 `feature/ble-ota`
- 决策沿革：2026-11-11 完成可行性核查（Flash/RAM/库体积实测），否决"boot 带 BLE 栈"与"迁移 ROM 库"两条路线，选定 **App 侧接收 + 压缩暂存 + 无 BLE 最小 boot**

---

## 1. 结论

**可以实现**。推荐路线：固件正常运行时通过 BLE 接收压缩镜像写入暂存区，重启后由一个**不含 BLE 协议栈**的最小 bootloader 校验、解压、覆盖 app 区并跳转。全程断电容错，USB ISP 兜底恢复。

不需要迁移 BLE 库、不需要改 RAM 布局、不需要 boot 支持 BLE。代价是引入压缩（必须，见 §3 空间账）和一个新的 boot 工程。

## 2. 现状事实（已逐条核实）

| 项 | 数值 | 证据 |
|---|---|---|
| Flash 总量 | **480K** = 448K 代码区（0x00000–0x6FFFF）+ 32K 数据区（0x70000–0x77FFF） | Flash 驱动 `ValidAddrEnd = FLASH_BASE + 0x78000`，[ch32v20x_flash.c:66](file:///Users/txa/project/Openterface_KM/SRC/Peripheral/src/ch32v20x_flash.c#L66) |
| 现行分区 | app 区 440K + 3×4K 配置页（BLENAMECFG 0x6D000 / DEFTCFG 0x6E000 / USBMODECFG 0x6F000） | [HAL/Link.ld:29-33](file:///Users/txa/project/Openterface_KM/HAL/Link.ld#L29-L33) |
| 当前固件 | 219,292 B ≈ **214.2K**（2026-11-10 构建） | `build/openterface-km-firmware.bin` |
| 其中 BLE 协议栈 | **198.1K**（202,812 B .text，`libwchble.a` Flash 版库，链接进 app 镜像） | map 文件逐段求和 |
| 应用自身代码 | ≈ 16K | 214.2K − 198.1K |
| bond 存储（SNV） | 数据区 **0x08077C00**，3×256B 块，`BLE_SNV=TRUE` | [config.h:103](file:///Users/txa/project/Openterface_KM/HAL/include/config.h#L103)、[MCU.c:110](file:///Users/txa/project/Openterface_KM/HAL/MCU.c#L110) |
| RAM | data+bss 50,924 B / 64K，空闲堆隙 ≈ 14.6K | [internal-memory-usage.md](internal-memory-usage.md) |
| Flash 写 API | 4K 页擦除 + 256B 页快速编程，SNV 已在用 | `Lib_Write_Flash`，[MCU.c:64-72](file:///Users/txa/project/Openterface_KM/HAL/MCU.c#L64-L72) |
| 向量表重定位 | startup 用链接符号 `_vector_base` 写 `mtvec`（模式 3），**改链接偏移即自动重定位**，无需额外代码 | [startup_ch32v20x_D8W.S:266-269](file:///Users/txa/project/Openterface_KM/SRC/Startup/startup_ch32v20x_D8W.S#L266-L269) |
| BLE 吞吐上限 | MTU 60、每连接事件 1 包 ≈ 36.5 kbit/s ≈ 4.5 KB/s | 台架实测（见记忆/隧道文档） |
| DFU 准入基础 | FFF2 已要求 `GATT_PERMIT_ENCRYPT_WRITE`（配对方案成果） | [ble_usb_service.c:138](file:///Users/txa/project/Openterface_KM/Profile/ble_usb_service.c#L138) |
| ROM 版 BLE 库 | 仓库带 `wchble_rom.hex`，占 0x40000–0x6CFFF（187.3K 含配置页初值）+ **16K RAM**（0x20000000–0x20003FFF）；当前构建**未使用** | [wchble_rom.h:1-15](file:///Users/txa/project/Openterface_KM/LIB/wchble_rom.h#L1-L15)，hex 地址实测 |

**关键推论：BLE 栈占 198K，任何"boot 里跑 BLE"的方案都会让 boot 膨胀到 ~200K**，这是路线选择的决定因素。

## 3. 候选路线对比

| 路线 | 描述 | 否决/采纳理由 |
|---|---|---|
| **A. App 接收 + 压缩暂存 + 最小 boot（采纳）** | app 用现有 BLE 栈收镜像 → 暂存区；boot 无 BLE，只做校验/解压/拷贝/跳转 | boot ≈ 6K；不动 BLE 库、不动 RAM；每个阶段断电都可恢复；压缩后传输 ~30s |
| B. 迁移 ROM 库 + 真 A/B 双区 | 栈固定在 0x40000，app 缩到 ~20K，boot/app 共享栈 | 长期最干净，但：① 库迁移需重新验证配对/SNV 全部行为；② ROM 库预留 16K RAM，app 只剩 48K，当前用 50.9K，**RAM 不够**，需削 `MEM_BUF` 等；③ 工作量与风险远大于 A。留作远期演进 |
| C. boot 带 BLE 栈直写 app 区 | boot 链接第二份 `libwchble.a`，收完直接覆盖 app | boot ≈ 206K + app 214K + 配置 12K = 432K，app 增长余量仅 ~16K；无暂存无回退，传输中断期间设备不可用；boot 体积是 A 的 30 倍。否决 |

A 的空间账（为什么必须压缩）：不压缩需要 app 214K + 暂存 214K + boot 8K + 配置 12K = 448K，**恰好占满且零余量**；压缩后暂存只需 ~130K，还能留出 40K 备用区。

## 4. Flash 分区（路线 A）

代码区 448K 重新划分，配置页与数据区**完全不动**：

| 区域 | 地址 | 大小 | 用途 |
|---|---|---|---|
| `BOOT` | 0x00000–0x01FFF | 8K | bootloader，无 BLE，运行时永不写 |
| `APP` | 0x02000–0x39FFF | 224K | 应用镜像（当前 214.2K，余量 ~10K） |
| `STAGING` | 0x3A000–0x61FFF | 160K | 压缩镜像暂存 |
| `META` | 0x62000–0x62FFF | 4K | 升级状态日志（§8） |
| `SPARE` | 0x63000–0x6CFFF | 40K | 备用，日后可划给 APP 或 STAGING |
| `BLENAMECFG` | 0x6D000 | 4K | 不变 |
| `DEFTCFG` | 0x6E000 | 4K | 不变 |
| `USBMODECFG` | 0x6F000 | 4K | 不变 |
| 数据区 | 0x70000–0x77FFF | 32K | SNV bond @0x77C00，**OTA 任何路径不得触碰** |

约束：
- 压缩镜像 ≤ 160K（打包工具强制检查）。heatshrink 对 RISC-V 固件典型压缩率 50–65%，214K → 110–140K，留有余量；M0 实测确认。
- app 余量 10K 偏紧，是已知取舍；需要时把 SPARE 划给 APP（改两个链接脚本常量，boot 逻辑不变）。
- boot 8K 预算：startup + flash 驱动 + heatshrink 解码器（~1K）+ CRC32 + meta 逻辑 + LED ≈ 5–6K。若 M4 加签名验证需扩到 16K（从 SPARE 借）。

## 5. 镜像格式与压缩

### 5.1 OTA 包（.ota 文件）

```
[ 64B 头部 ][ heatshrink 压缩数据 ][ 可选 64B Ed25519 签名（M4） ]
```

头部（小端）：

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | magic `KMOT` | 4B |
| 4 | hdr_ver | 1B，当前 1 |
| 5 | hw_id | 1B，`BOARD_VERSION`（KeyMod=3 / KVM-GO=1），不匹配拒收 |
| 6 | flags | 2B，bit0=签名存在 |
| 8 | fw_version | u32，**单调递增**，防回滚依据 |
| 12 | raw_size | u32，解压后字节数（= app 镜像长） |
| 16 | comp_size | u32，压缩数据长 |
| 20 | raw_crc32 | u32，解压后全量 CRC32 |
| 24 | comp_crc32 | u32，压缩数据全量 CRC32 |
| 28 | hdr_crc32 | u32，头部前 28 字节 CRC32 |
| 32–63 | reserved | 置 0 |

app 镜像本身不需要特殊尾部：校验用的 CRC 全部在 OTA 头部和 META 里，USB 直烧的裸 bin 不受影响（§7 boot 对无 META 记录的出厂态直接放行）。

### 5.2 压缩算法

选 **heatshrink**（LZ77 变体，嵌入式取向）：解码器 ~1K 代码、窗口 RAM 可配（取 2^10 窗口 + 2^4 前瞻，~1.3K RAM），编码只在主机工具里跑。备选 LZ4（解码更快、压缩率略差、RAM 更多），M0 用当前 bin 实测两者压缩率后定案。不选 zlib：解码器体积与 RAM 都大一个量级。

## 6. App 侧改动

1. **链接偏移**：app 链接脚本 `FLASH ORIGIN=0x02000 LENGTH=224K`，RAM 布局不变。向量表无需手动重定位——startup 已用链接符号写 `mtvec`（§2 已核实）。
2. **DFU GATT 特征**（挂在现有 ble_usb 服务下，复用配对方案的加密准入）：
   - `FFF3` DFU_CTRL：write + notify，权限 `GATT_PERMIT_READ | GATT_PERMIT_WRITE | GATT_PERMIT_ENCRYPT_WRITE`（与 FFF2 一致）
   - `FFF4` DFU_DATA：write without response，权限 `GATT_PERMIT_ENCRYPT_WRITE`
   - 不新建服务、不改广播；未配对设备连特征都写不进，准入压在协议栈层（与配对方案同一原则）。
3. **DFU 模式**：收到 START 后暂停 HID/CH9329/ECM/ACM/UAC 转发（升级期间设备不做 KVM 工作），只保留 BLE 连接与 TMOS 调度；每写若干页 notify 一次进度。Flash 编程会短暂 stall 总线，暂停其他通路可避免丢包与连接事件超时。
4. **写暂存**：按 256B 页调 `FLASH_ProgramPage_Fast`（与 SNV 同一套 API，已验证可用），擦除以 4K 页为单位。写入地址**硬编码限定在 STAGING 区间**，越界即拒绝——这是防止误写 boot/配置/SNV 的唯一屏障，必须做。
5. **RAM 预算**：块缓冲 256B + 页缓冲 256B + 状态机/头部 ≈ 1.5K，空闲堆隙 14.6K 足够。
6. **版本上报**：INFO 命令返回现有固件版本（devinfo/devinfoservice 口径）+ OTA fw_version。

## 7. Bootloader 设计

无 BLE、无 USB、无 TMOS，裸机循环，只做四件事：读 META → 校验 → （按需）应用镜像 → 跳转。

```
上电
 ├─ 读 META 最新有效记录
 ├─ state == PENDING？
 │    ├─ 校验 STAGING comp_crc32 ── 失败 → 置 FAIL，走"无可用镜像"
 │    ├─ 擦除 APP 区（4K/页）
 │    ├─ heatshrink 解压 STAGING → APP
 │    ├─ 校验 APP raw_crc32 ── 失败 → 保持 PENDING，下次上电重试
 │    └─ 写 META state=OK（含新 fw_version/raw_crc32）
 ├─ 校验 APP：META 有 OK 记录 → 按记录的 raw_crc32 校验；
 │            META 为空（出厂 USB 直烧态）→ 直接信任放行
 ├─ APP 有效 → 关全局中断，跳转 APP 复位向量（mtvec 由 app startup 自行重设）
 └─ APP 无效且无 PENDING → LED 错误闪烁，停在 boot，等待 USB ISP 恢复
```

设计要点：
- **boot 永不自我更新**，DFU 写路径与 boot 拷贝路径都不写 0x00000–0x01FFF。boot 损坏只能靠 USB ISP，而 boot 逻辑简单到可以一次写对。
- PENDING 状态下每次上电都**重新校验 STAGING 再动手**，不信任上次中途的状态，所以拷贝中断电无限次都安全。
- 全量 CRC32 校验 214K 在 144MHz 下约 10–20ms（表驱动），每次上电都做，启动延迟可忽略。
- 应用阶段耗时（擦 224K + 写 224K + 解压）估计 2–5s，M0 实测；期间 LED 常亮/慢闪指示"升级中勿断电"。
- 恢复路径与现在完全一致：`build/wchisp` USB ISP 烧录。boot 阶段设备不响应 BLE 是**有意设计**——恢复通道是 USB，不是再来一套 BLE boot。

## 8. META 日志（4K 页）

记录 32B，顺序追加写（flash 可不擦除重复编程直到位用尽），页满才擦除重写最新记录：

| 字段 | 说明 |
|---|---|
| magic / seq | seq 单调递增，boot 取 seq 最大且 CRC 有效的记录 |
| state | IDLE / PENDING / OK / FAIL |
| fw_version / comp_size / raw_size / raw_crc32 | PENDING/OK 时携带镜像信息 |
| rec_crc32 | 本记录校验 |

- **防回滚**：START 时 app 检查 `新 fw_version ≥ META 最近 OK 的 fw_version`，boot 应用前再查一次。版本号只增不减，由发布流程保证。
- 双份最新记录冗余不需要：追加写天然保留历史，任何时刻断电最多丢失最后一条未写完的记录，boot 回退到上一条有效记录。

## 9. GATT DFU 协议

CTRL（FFF3）命令字 1B，应答/事件走 notify；DATA（FFF4）走 write-no-response _bulk_ 通道，与 HID 命令通道（FFF2）物理隔离。

| 命令 | 载荷 | 行为 |
|---|---|---|
| 0x01 INFO | — | notify：boot_ver、app_ver、hw_id、max_image(160K)、META state |
| 0x02 START | 64B 头部 | 校验 hw_id/防回滚/comp_size≤160K/hdr_crc → 擦 STAGING → ACK 或 ERR |
| 0x03 DATA | FFF4：[seq u16][crc16][payload] | 顺序写入；payload 默认 244B（MTU 247），本机 MTU 60 时协商为 ~55B；CRC 错/乱序 → ERR + notify 期望 seq，主机从断点重传 |
| 0x04 VERIFY | — | 全量 comp_crc32 → ACK/ERR |
| 0x05 APPLY | — | 写 META PENDING → notify OK → 延迟 100ms 复位 |
| 0x06 ABORT | — | META 置 IDLE，退出 DFU 模式，恢复正常转发 |

notify 事件：进度（每 4K 一次：已写字节/总字节）、错误码（HW_MISMATCH / ROLLBACK / TOO_LARGE / CRC / FLASH / STATE）。

传输耗时估算：按实测上限 4.5 KB/s，压缩后 130K ≈ **30s**；MTU 协商到 247 后可再快数倍（取决于手机与连接参数，不在本方案承诺范围）。

升级完成后主机重连，读 INFO 确认 fw_version 已更新，作为端到端成功判据。

## 10. 安全性

| 层 | 措施 |
|---|---|
| 准入 | DFU 特征要求 bonded + 加密链路（继承配对方案，PIN 信任根）；未配对连 START 都发不出 |
| 完整性 | 每块 CRC16 + 压缩全量 CRC32 + 解压全量 CRC32，三级 |
| 防回滚 | fw_version 单调，app 与 boot 双查 |
| 防误写 | DFU 写路径地址区间硬限制；boot 区运行时不可写；SNV/配置页不在任何 OTA 路径内 |
| 签名（M4 可选） | Ed25519（monocypher，~5K 代码），主机签名、boot 验签后才应用；基线不做，理由：威胁模型主要是路人无凭证访问（配对已挡）与意外损坏（CRC 已挡），定向伪造固件需要物理接触或已破解配对，属残留风险，与配对方案 §2 口径一致 |

## 11. 断电容错分析

| 断电时刻 | 上电后状态 | 结果 |
|---|---|---|
| 下载中（任意进度） | META=IDLE，APP 完好 | 正常启动，主机重传 |
| VERIFY 后、写 PENDING 前 | 同上 | 同上 |
| PENDING 已写、擦除/拷贝任意时刻 | boot 见 PENDING，STAGING 完好 | 重新应用，无限次安全 |
| 拷贝完成、写 OK 前 | boot 校验 APP CRC 通过 → 补写 OK | 正常 |
| 新固件本身有 bug 能启动但功能坏 | 无自动回退（基线） | USB ISP 恢复；M4 可选回滚 |
| boot 区损坏 | 运行时任何路径不写 boot | 物理排除；兜底 USB ISP |

唯一"设备不可用"窗口是应用阶段的 2–5s（boot 正在拷贝），断电不会变砖，只会重来。

## 12. 里程碑与检查清单

### M0 台架可行性验证（先于一切代码改动）
- [ ] 实测 4K 擦除 + 256B 快速编程耗时（擦 224K / 写 224K 总时长）
- [ ] heatshrink 与 LZ4 对当前 214K bin 的压缩率、解压速度对比，定算法
- [ ] 最小实验：hello 固件链接到 0x2000，boot 跳转验证 mtvec 自动重定位
- [ ] 确认 SNV（0x77C00）与配置页在实验擦写后完好（读回 bond 记录）

### M1 分区与 boot
- [ ] app 链接脚本改 0x2000/224K；新建 boot 工程与链接脚本（8K）
- [ ] `build.sh` 增加 boot 目标与 boot+app 合并烧录产物（wchisp 一次烧全片）
- [ ] boot 状态机（无 PENDING 路径）+ LED 状态定义
- [ ] 台架：USB 烧录后正常启动，全部现有功能回归（HID/ECM/配对）

### M2 DFU 端到端
- [ ] FFF3/FFF4 特征 + 加密权限 + DFU 模式（暂停转发）
- [ ] STAGING 写入 + 地址硬限制 + META 日志
- [ ] `tools/ota_pack.py`（压缩+头部+尺寸检查）、`tools/ota_upload.py`（bleak，按完整 CoreBluetooth 地址连接，避免 keymod-test 的名字子串误匹配陷阱）
- [ ] 台架：首次 BLE OTA 成功，INFO 版本确认

### M3 健壮性
- [ ] 断电注入矩阵：下载中/擦除中/拷贝中/写 META 中随机断电 × 各 3 次
- [ ] 防回滚、hw_id 不匹配、超尺寸、ABORT、断点重传
- [ ] 升级后 bond/配置页/USB 模式完好性检查
- [ ] 传输耗时与进度通知实测记录

### M4 可选加固（默认不做，另行决策）
- [ ] Ed25519 签名（boot 扩 16K）
- [ ] 回滚：应用前把旧 app 压缩回 STAGING，新固件确认失败可退回

## 13. 测试方式

沿用台架分工：我给出每轮烧录的命令批次与片内计数器/读回命令，用户执行烧录与 sudo/主机侧操作。M3 断电注入需要可程控断电或手拔 USB 供电配合倒计时脚本，M0 时一并定方案。

## 14. 风险与开放问题

1. **app 余量仅 10K**：固件还在长（配对、隧道后续）。SPARE 40K 是缓冲，但重分区要动 boot 常量，宜早不宜迟——M1 时可直接把 APP 定 240K / STAGING 144K（压缩率需 ≤67%，M0 数据说话）。
2. **压缩率最坏情况**：若实测 >160K，打包工具拒收；对策是削 app 体积或扩 STAGING，不存在"压不下就砖"的路径。
3. **Flash 编程 stall 与 BLE 连接事件**：SNV 写入已证明可行，但 DFU 是持续密集写，M2 需盯连接断开率；必要时每页之间显式让出 TMOS。
4. **配对分支未合并**：DFU 权限依赖加密准入，分支顺序不能反。
5. **ROM 库迁移（路线 B）是远期方向**：若日后 app 代码大涨或需要真 A/B，再评估；届时 RAM 48K 上限是硬约束（需削 `MEM_BUF` 10K → 8K 等）。
6. **KeyCmd iOS/Android 集成**：本方案先交付 Python 工具链，App 集成协议照 §9 实现，不在本期范围。

## 15. 决策记录

- 2026-11-11：实测确认 BLE 栈 198K、Flash 480K（448K+32K）、SNV 在 0x77C00、ROM 库未启用且 RAM 预留 16K。据此否决路线 B/C，采纳路线 A；压缩定为必选项；签名与回滚降级为 M4 可选。