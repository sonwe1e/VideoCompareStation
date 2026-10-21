# 真实环境验收走查清单（runbook）

创建日期：2026-10-20。状态：清单就绪，全部条目未执行。
本文只写"如何验"：素材、命令、环境身份、判据、证据落点。发现的问题记入
[问题台账](visual-review-backlog.md)，不在本文平行记录缺陷；结论必须绑定提交、
工作区 diff 与素材，格式见[可复现基线记录](reproducible-baseline.md)。
行号与键名为 2026-10-20 校对值，执行前以当前代码为准。

执行位置受[测试环境分离](test-environment-separation.md)约束：GPU、物理显示与
可见窗口项不在主力机自动执行，只在用户安排的独占时段进行。WARP、VM、
GameViewer/Oray 等虚拟显示适配器不构成验收证据；预检与硬件协议见
[self-hosted runner](../self-hosted-runner.md)。当前状态：性能四门槛从未实跑
（[缓存优先路线](../plans/cache-first-roadmap-2026-10-20.md) §批次E、
[能力台账](capability-ledger.md) F03），本清单是消除该缺口的操作手册。

## 0. 通用规则

- 证据统一落点 `out/verification/real-env-<日期>/`（一个日期一个子目录，附
  环境身份表）。门禁脚本自身的日志已写在 `<FixtureRoot>\results\<RunName>-*.log`，
  归档时引用路径，不复制内容。
- 每个时段开始先填 §1 环境身份表，再执行任何条目；身份不全的结果按 unknown 处理，
  不事后补记。
- 每条目的"通过判据"是最小判据：判定通过后仍要把原始证据留档，供后续
  同提交复核。

## 1. 环境身份（每次会话必记）

| 字段 | 来源 |
|---|---|
| 提交 SHA、工作区是否 dirty、构建类型 | 门禁摘要 `BUILD sha=… dirty=… type=…` 行自动输出；走查类条目手记 `git rev-parse HEAD` + `git status --porcelain` |
| GPU 型号与驱动版本 | 手记（如 RTX 4090 / 32.0.16.1074，以当日实测为准） |
| 物理显示器型号、连接方式、刷新率 | 手记；脚本只记录 `screen_refresh_hz`，型号与 PCI/虚拟归属必须手记 |
| DPI 缩放、多屏拓扑 | 手记 |
| 系统 HDR / Advanced Color 状态 | 手记（当前基线"未记录"，见 reproducible-baseline §2） |
| 显示路径 | 门禁 `-DisplayPath physical/remote/unknown` 参数 |
| 比较模式、播放档、排除区间 | 摘要 `PLAY`/`JOG` 行与报告 `play.timing_scope`、`play.system_presentation` |
| 素材 SHA-256 | diff 配对由 contract 自动核对；其余素材首次置备时各记一次 |

## 2. 性能四门槛门禁

### 2.0 前置准备

1. 硬件预检（约 1 分钟，必须先过）：
   `.\tools\testing\test-hardware-runner.ps1 -MinimumRefreshRate 120`
   输出 `DVS_HARDWARE_RUNNER_READY` 才继续；交互桌面 + PCI 显卡驱动的 ≥120 Hz
   物理输出，不得用虚拟显示或降刷新率绕过。
2. 构建（走 wrapper，约 10–20 分钟，可安排在占机时段外）：
   `pwsh tools/build/build.ps1 -Preset release`，
   得到 `out\build\release\bin\CompareStationCli.exe`。
3. 素材：门禁默认从 `out\performance`（或 `DVS_PERFORMANCE_FIXTURE_ROOT`）读取。
   本机该目录当前**不存在**，以下 9 个文件全部缺：

| 文件 | 用途 | 置备方式 |
|---|---|---|
| gate-1080p60-a/b/c.mp4 | 1/2/3 源 1080p60 | 无入库生成器，按 self-hosted-runner.md 清单置备，记录来源与 SHA-256 |
| gate-1080p60-diff-b.mp4 + .contract.json | diff 模式配对 | 置备 a 之后运行 `tools/testing/generate-performance-diff-fixture.ps1`（自动带哈希 contract） |
| gate-1080p60-rot90-a.mp4 | 旋转元数据 | 随基础素材一并置备 |
| gate-1080p120-a/b/c.mp4 | 1/2/3 源 1080p120 | 同上 |

4K 不属于门禁矩阵（self-hosted-runner.md 已注明），4K/P010 走 §3.2 人工项。

### 2.1 命令入口

```powershell
pwsh tools/testing/run-performance-gate.ps1 `
  -Profile 1080p60-2source `
  -Executable out\build\release\bin\CompareStationCli.exe `
  -ComparisonMode side -DisplayPath physical -BuildType release `
  -RunName 1080p60-2source-20261020
```

- Profile 八选一：`1080p60-1source`、`1080p60-2source`、`1080p60-2source-diff`
  （只许 diff 模式）、`1080p60-2source-rotated`、`1080p60`（3 源）、
  `1080p120-1source/2source/3source`。
- `-DurationSeconds` 默认 300；1080p120 组按既有协议用 60（self-hosted-runner.md
  "阻断门槛"段）。
- 单次运行内含 play、held-step、JOG、seek 各阶段；产物为
  `DVS_BENCHMARK_SUMMARY` 块、`DVS_GATE_PASSED`/`RESULT FAIL` 行与
  `results\<RunName>-stdout/-stderr.log`。脚本缺键即失败
  （`requiredReportKeys`，含全部 `jog_*` 键）。

### 2.2 四门槛判据对照

| 门槛 | 判定在哪 | 依据键 |
|---|---|---|
| 组级丢帧 ≤0.5% | 可执行内硬门：`drop_ratio > 0.005` 即失败（Main.cpp ui-performance 通过条件，2028 行）；FrameSet 不拆源：`source_split_observations != 0` 即失败 | `presented_frames`、`dropped_frames`、`drop_ratio`、`source_split_observations` |
| seek P95 ≤500 ms | 可执行内硬门：`seek_p95_ms > 500` 失败；JOG 释放停稳 `jog_stop_p95_ms > 500` 亦为脚本硬门（EX-02）；`held_step_p95_ms` 仅记录待首基线 | `seek_p95_ms`、`jog_stop_p95_ms`、`held_step_p95_ms` |
| UI 响应 ≤100 ms | 可执行内硬门：`playback_response_ms > 100` 失败 | `playback_response_ms`、`ui_loop_gap_p95_ms`（记录） |
| 解码帧缓存 ≤256 MiB | **无脚本硬门，人工判读**：`CACHE` 行 `bytes=`（FrameSet 邮箱缓存）与各源解码后端 `cache_bytes/entries/evictions` 都不得越过现行池。现行池 224 MiB（`ReviewRuntime.cpp:52`；capability-ledger F03 注明旧称 256 MiB 是历史红线不是现行值） | `frameset_cache_bytes`、`frameset_cache_evictions`、`decoder_cache_hit_ratio`、各源 `cache_bytes`；`gpu_peak_mb` 实测或保持 unknown（批次A，路线 §2A） |

环境硬门一并生效：`screen_refresh_hz ≥ 120`、`allHardware`（落到 WARP 即失败）、
关闭 ≤7000 ms、`comparison_mode_verified`、diff 模式亮像素 ≥1%、JOG 结构门
（`jog_legs ≥ 4`、stop 样本数 == legs、forward > 0、`jog_stale_updates == 0`、
`jog_reverse_fps / jog_forward_fps ≥ 0.2`）。任何一项失败整跑即 FAIL。

### 2.3 建议执行集与附加判据

按顺序跑完全部八个 profile（§4 S1）；全绿后加两条路线图判据
（[缓存优先路线](../plans/cache-first-roadmap-2026-10-20.md) §2A/§2B）：

- 同一段 20 次重复回看：能从报告与 trace 回答命中几次、淘汰几帧、峰值多少、
  预留失败几次。
- 单源 1080p 同段单步反向 20 次：`reverse_window_hits ≥ 8`，且 `exact_seeks`
  下降、`cache_hits` 上升。**注意**：批次B诊断口径未修前，`cacheHitCount` 含
  播放环周转污染，先修口径再取数，否则证据无效。

## 3. 真机走查项

| # | 怎么验 | 通过判据 | 证据 | 依赖 |
|---|---|---|---|---|
| 3.1 真实影片观感（EX-01 前半） | 用户真实 1080p60 素材 2–3 源，side/wipe/diff 各开一次；播放 5 分钟、暂停、10 次 seek、按住 Left 倒退 20 步 | 无可见卡顿/花屏/源间失步；倒退手感与 §2 的 `jog_stop_p95_ms` 量级一致 | 屏幕录制 + 笔记入当日目录 | 独占桌面；真实素材由用户提供 |
| 3.2 4K/P010 反向窗口（EX-01 后半） | 单源 4K（可得则含 P010/10-bit）硬件解码，倒退 20 步 | 每步不整窗重解（trace/诊断 `reverse_window_*`）；`jog_stop_p95_ms ≤ 500`；观察显存不溢出 | 报告 JSON + trace + 显存读数截图 | D-014/D-017 改判素材（临时假设验证项）；4K 不在门禁矩阵 |
| 3.3 多屏 DPI | 双屏不同缩放，拖窗跨屏、跨屏后进入各比较模式 | 无模糊/错位/命中偏移，控件可达 | 跨屏前后截图 | 双物理屏时段 |
| 3.4 读屏 + 键盘 | NVDA 或 Narrator；仅键盘完成 Open→Play→Pair→Wipe→Timeline→Range→Help | 控件名称/值被播报、焦点环可见、无逐帧朗读轰炸（用例参照 playback-overhaul [10 号文档](../plans/playback-overhaul/10_最终验证与发布门禁.md)"仅键盘"条目） | 朗读记录/笔记 | 已有 `Accessible.*` 契约测试为前置，非替代 |
| 3.5 触摸/手写笔 | 触摸走带按钮、视口捏合缩放/平移、长按逐帧 | 命中即响应，无 hover-only 死区 | 录屏 | 触屏设备 |
| 3.6 系统高对比度 | 开启 Windows 高对比度主题运行主窗与对话框 | 文本/控件边界/焦点可辨，语义色不塌 | 截图 | — |
| 3.7 COLOR5 显示链路 | 按[计划](../plans/CompareStation_收尾后分阶段改进计划_2026-10-17.md) COLOR5 验收标准走：SDR 桌面、跨显示器移动、睡眠唤醒、HDR/Advanced Color 开启 | 应用对未验证环境显示明确"未验证/降级"状态，不出现错误成功标记；判据细则见 [image-color-contract §7](image-color-contract.md) 与计划 COLOR5 节，此处不复制 | `displayColorEnvironmentVerified` 状态截图 + 显示器身份手记 | COLOR4 收口（G3-5，交付计划 §3.4） |
| 3.8 窄窗 960×640 截图 | 重跑 `MainQmlContractTests.ChromeGeometryAndScreenshotEvidence`（1440×900 与 960×640 两档实抓），另用真实素材在 960×640 手动截图 | 无重叠/裁断/不可达（契约用例已有几何断言，人工复核像素证据） | 沿用既有落点 `out/verification/ui-audit/`，本次另存日期副本 | **UX-13 轮完成**（960×640 折叠重排，[ui-ux-review](ui-ux-review-8d937eb.md)）；重排前截图不验收新布局 |
| 3.9 EX 系列其余人工项 | EX-05：真实模型 + 真实图走抠图，看边缘软硬与小目标；EX-03/04/06/07/08 契约测试已锁，真机只抽验一次交互 | EX-05 无硬二值锯齿、无模型时不假成功；其余与契约断言一致 | 截图 + 笔记 | EX-05 需可用模型路径（评审文档批次3注明模型路径不可得时只能验证入口） |

## 4. 执行顺序与占机估计

| 阶段 | 内容 | 依赖 | 占机估计 |
|---|---|---|---|
| P0 | 素材置备、diff 生成、release 构建、runner 预检 | 无（构建/生成不占显示环境；预检 1 分钟） | ≈0（时段外） |
| S1 | 八个 profile 门禁（§2.3）：1080p60 五个各约 10–12 分钟（300 s 采样 + open/seek/held/JOG 阶段），1080p120 三个各约 5 分钟（60 s） | P0、独占交互桌面、物理 ≥120 Hz | ≈75–90 min |
| S2 | 重复回看 20 次 + 单源反向 20 次 + 4K/P010（§2.3、§3.2） | S1 全绿；批次B诊断口径先修 | ≈30–45 min |
| S3 | 真实影片观感、多屏 DPI、高对比度、COLOR5（§3.1/3.3/3.6/3.7） | S1；COLOR5 另依赖 COLOR4 收口 | ≈90–120 min |
| S4 | 读屏 + 触摸（§3.4/3.5） | S3 同一时段可顺带 | ≈45–60 min |
| S5 | 窄窗 960×640 截图（§3.8） | **UX-13 轮完成** | ≈30 min |

合计约 4.5–6 小时占机，建议拆 2–3 个时段：S1+S2 一段，S3+S4 一段，S5 独立等
UX-13。所有阶段共享同一环境前提（登录态交互桌面 + PCI 显卡 + 物理 ≥120 Hz），
按[测试环境分离](test-environment-separation.md)不与普通测试并行。

## 5. 引用

- [self-hosted runner](../self-hosted-runner.md)：素材清单、runner 注册、预检与安全边界
- [测试环境分离](test-environment-separation.md)：哪些项能在哪类环境执行
- [可复现基线记录](reproducible-baseline.md)：环境身份字段模板与历史基线
- [缓存优先路线](../plans/cache-first-roadmap-2026-10-20.md)：§2A/§2B 判据来源、批次E证据缺口
- [问题台账](visual-review-backlog.md)：EX/UX 缺陷状态唯一来源
- [UI/UX 评审](ui-ux-review-8d937eb.md)：UX-13 与截图证据机制
- [产品体验质量评审](product-quality-review-2026-10-19.md)：EX-01~08 背景
- [图片颜色契约](image-color-contract.md)：COLOR4/COLOR5 边界
- [能力台账](capability-ledger.md)：F03 现行预算口径
