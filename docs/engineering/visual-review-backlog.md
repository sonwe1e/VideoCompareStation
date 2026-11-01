# 视觉审查问题台账

核对日期：2026-10-25；源码基线 `4ca3abb`（2.6.0 发布准备），清理前工作区干净。
本页只保留当前缺口、验收边界和已解决问题的定位索引。实现状态经源码核对；
没有在本次文档整理中重新运行产品测试、GUI 或硬件门禁。

**待修复**表示有明确缺口；**待验证**表示实现已有但证据不足；**待复核**表示旧报告的
发现尚不能直接定性当前版本；**待决策**不构成实现授权。产品取舍见
[决策登记册](decision-register.md)，代码/测试路由见 [Agent 指南](../agent-guide.md)。

## 当前缺口

| ID | 状态 | 问题、下一步与退出依据 |
|---|---|---|
| DPI-01（原 10-24 P0） | 源码已修，待实机验证 | 原实现 `Main.qml` 写死 1440×900、最小 960×640，未按可用屏幕几何钳制；可用区更小时窗口原点为负、底部控件被裁（10-25 取证更正见下）。2026-11-01 改为由 `LayoutMetrics.fitWindowToScreen(Screen.width, Screen.height, 1440, 900, 960, 640)` 派生尺寸与两个下限，`DesktopApplication` 的跨屏居中同时钳制在可用区内；纯函数用例 `tst_layout_metrics.qml::test_window_fit_keeps_the_frame_inside_the_screen`。**仍待**自然启动、不同启动入口、100–200% 缩放与多屏的真实环境复核；测试宿主的样式/DPI 截图不能替代 |
| EX-01 / EX-02 / F03 | 部分实现，待测量 | 源缓存已去掉 48 MiB 固定上限；播放池 224 MiB，每源不超过预算份额和 24 帧。反向 seed/walk 仍在，新请求可抢占 seed；seek 尾迹保留尚未实现。按[缓存路线](../plans/cache-first-roadmap-2026-10-20.md)补单源长 GOP 的 seek 后首步、连续倒退、停止延迟与预算分账证据；不得把容量增加当作体验已达标 |
| ENG-01（10-18 审计遗留） | 源码已修，待实机验证 | `RenderPublishResult` 现区分 `Closed`／`DeviceUnavailable`／`StaleContext`／`InvalidSet`，`graphics/D3d11RenderChannel.cpp` 逐项映射 `GpuTransferSubmitResult`；`PlaybackCoordinator` 三处 publish 失败改走 `renderPublishError`：设备不可用→`kGraphicsUnavailable`、换代或关闭→`kGraphicsDeviceLost`、帧组不可用→`kMediaDecodeFailed`，操作标为 `kFramePresentation`。图形类错误会被 `handleGraphicsReady` 清除，设备恢复后不再常驻。锁定测试：`application.PlaybackCoordinatorTests.DeviceUnavailablePublishKeepsItsReason`、`.StalePublishContextIsReportedAsDeviceLost`、`platform.D3d11RenderChannelTests.KeepsTheActorReasonInsteadOfCollapsingEveryFailure`。**仍缺**：真机 TDR 复核；跨代 publish 仍按一次失败终止本轮播放，没有自动重试或续播（恢复需用户再触发） |
| ENG-02（10-18 审计遗留） | 源码已修，待实机验证 | 2026-11-01 按「同一 actor 复活 + 邮箱换代前移」落地，不改跨层契约：`GraphicsDeviceBroker::adoptQtDevice` 在 Loss 之后接受**身份不同**的新设备（同一次丢失的旧设备仍返回 `AlreadyUnavailable`），收养后 generation+1 并发布 Ready；`graphics/GpuTransferActor.cpp` 的 `submit` 在「已墓碑」或「持旧代租约」时复活（清 lease／scope 高水位／lossReported，调用 `advanceDeviceGeneration`，累计 `recoveredGenerations`），`reportLoss` 与两次租约失败不再调 `FrameMailbox::shutdown`（关闭是终态，正是它把一次设备丢失变成终生黑屏），`isClosed()` 只表示 shutdown。锁定测试为 `platform.GraphicsDeviceBrokerTests.AdoptsARebuiltDeviceAfterALossButNotTheLostOne`、`platform.GpuTransferActorTests.RejectsWorkFromTheLostDeviceGeneration`、`...ResumesWorkAfterANewDeviceIsAdopted`（原先锁定缺陷行为的 `RejectsWorkAfterBrokerDeviceLossTransition` 已删除）。**仍缺**：真实 TDR 后 Qt 场景图是否重新交出设备（本路径的触发的上游条件）、ENG-01 的失败原因通道、真机 TDR/驱动重置验证 |
| ENG-03（10-18 审计遗留） | 待复核 | `FileAssociationRegistration` 注册写入后的回滚记录分配、`WindowsPaths` 构造路径异常时的 `CoTaskMem` 释放曾有异常安全风险。按当前实现补故障注入后定性；不因普通成功路径测试而关闭 |
| S03 / EX-05 | 部分实现，待修复/验证 | SAM2 仍在 `ImageEditController::smartSegmentRect` 调用链同步编码/推理，可能阻塞 GUI；模型失败后的缓存有效性也需覆盖。软 Alpha、小目标处理与空 mask 报错已实现，真实模型边缘质量待验。发布 ZIP 不含模型/运行库的当前策略见 D-019 |
| V04（路线） | 工作区已实现，真实操作体验待验 | 2026-10-27 新增「与原图比较」：独立临时 RGBA8 快照，A 原图、B 含标注结果，同版复用、新版隔离；沿用任务数/工作集预算。像素、版本隔离、预算失败与主窗接线已有行为测试；见[实施方案](../plans/ux-continuity-2026-10-27.md)。快照不直接持久化问题记录，保留原图不可变 |
| SCRUB-01 | 待复核 | 旧审查提出高频 scrub 缺少 latest-wins 合并；当前逐帧导航和悬停缩略图已有各自的合并机制，不能据此推断时间线拖动是否仍有缺口。旧禁用探针删除不代表功能完成；需按实际拖动路径复核请求数量、最终目标和过期结果 |
| A11Y-01 | 待实现/验证 | 时间线已有可访问角色、名称与焦点；向读屏器提供当前位置值仍需自定义可访问接口或等效实现。Wipe 手柄键盘路径已落地；焦点环、读屏、触摸和系统高对比度需真实环境走查 |
| A3（旧界面收口） | 待复核 | 通知栈与窄窗右侧面板可能重叠的旧观察未复现也未关闭；五工作区改版后先检查 `Main.qml` 的舞台边界，不能照旧截图直接修改 |
| A-01 | 持续维护 | 保留分层、单一协调循环、完整 FrameSet、呈现 ACK 与异步身份。Facade 已是 playback/comparison/shell 三 capability 组合；其余 `var` 服务按功能迁移为类型化接口，不为局部修复强制全量重构。见 [ADR 0006](../adr/0006-qml-type-registration.md) |
| PERF-01（2026-10-28 性能批次） | 待修复 | CPU 上传路径（软解回退/无 D3D11VA 源）每帧组新建 staging/default 纹理+SRV+事件查询并在发布前同步等 fence（`GpuTransferActor.cpp`）。硬门禁要求全源 D3D11VA，标准门禁测不到该路径。方向：按 (格式,宽,高,设备代) 池化 + 拷贝入队后立即发布、staging 按 fence 完成延迟回收；须先按 FrameBudget 记账口径与 ENG-02 换代语义补证据 |
| PERF-02 | 待决策/待证据 | 协调层播放期前视深度恒 1 帧，`PrefetchScheduler::duringForwardPlayback`（3 帧前视）是死代码。exactPrefetchWindow 注释记录过投机解码引发 DecoderReopen（kind 11）风暴的教训；加深须以 fps 阈值+队列空闲为前提并用 kind 11 计数验证，先补恢复期 trace 证据 |
| PERF-03 | 待修复 | `postCritical` 满队（256）阻塞等待把唯一的节奏定时器线程与 ACK relay 变成队头阻塞点。改投递语义须保持 DeadlineElapsed 至少一次送达且不得与 postRealtime 单槽混用 |
| PERF-04 | 待增强 | 测量工具缺口：通用 profile 无交错 A/B 入口（run-t5-gate-ab.ps1 范式未推广）、gate 结果不落机器可读 JSON、物理呈现层无采样（0.5% 掉帧门只覆盖应用 ACK 口径，与 V-01 相邻）、基础素材无哈希 contract、UI 响应门实际只有 play 一个样本。定位为门禁/发布证据增强，不进日常开发循环 |
| ENG-04（2026-11-01 复核） | 源码已修 | `application::IRenderChannel::clear` 改为 `[[nodiscard]] bool`：`graphics/D3d11RenderChannel.cpp` 原样返回 actor 的 `clear`（无 actor 时为 false），协调层六处调用显式 `static_cast<void>` 并注明「拒绝即无 scope 可作废」，三个测试假通道与 `DirectCompare` 的 `CapturingRenderChannel` 同步返回 true。`platform.D3d11RenderChannelTests.KeepsTheActorReasonInsteadOfCollapsingEveryFailure` 断言同一 scope 只能装一次墓碑（先 true 后 false）。**仍缺**：协调层目前不消费该结果；若将来需要在 UI 提示「陈旧画面仍被保留」，可直接使用这个返回值而无需再改端口 |
| ACK-01（2026-11-01 复核） | 源码已修，缺针对性反例 | `retryPendingAcknowledgement`（`graphics/D3d11ComparisonRenderer.cpp`）在重试仍遇 `Full` 时重新调用 `notifyAckBackpressured()`：此前只有首次路径通知，重试那次留着 pending 却不重排重绘，暂停或逐帧时没有新帧触发下一次重试，协调层要等满五秒的呈现超时。已修但**没有**新增用例——它需要「ACK 队列保持满 + 暂停态重绘」的夹具，`ComparisonSurfaceTests` 的 `CountingActivitySink` 已能计数背压通知，可作为下一次补测的接缝；本轮只以 `platform.` 187 例回归通过为准 |

### 2026-11-01 复核记录

复核基线：目录重组提交 `315ac65` 之前的 `c0dba16` 工作树，加上当天新增的测试与文档。方法为只读
源码核对（两名独立 agent 分工：仓库/打包结构审计与缺陷审查），未取得运行期硬件证据。

- **迁移可核对**：77 个删除/新增对按 blob 哈希逐一比对为逐字节迁移；25 个 CMakeLists 的 177 条
  `src/`/`include/` 路径全部存在，`src/*/src` 下无未被任何目标引用的 `.cpp`；迁移后的子目录都已进入
  对应测试目标的 include 目录。
- **打包链一致并且实跑过**：`Install.cmake`/`VerifyPackageStage.cmake` 引用的安装期文件、必需载荷、
  版本派生名（`CompareStationShell-2.7.dll`、`CompareStation-2.7.0-windows-x64`）与 2.7.0 一致。
  2026-11-01 本地顺序执行 release 增量构建 → `cpack --preset release-zip` → `verify-release-package.ps1`：
  CPack 前的 staging 门通过，产物 `CompareStation-2.7.0-windows-x64.zip` 27.1 MiB，包自校验 16 项
  全部 PASS。这证明打包管线在整理后仍可用，但**不是**发布门禁：硬件/性能门禁、干净机解压与真实
  注册表行为都没有在本轮验证。
- **仍未关闭**：ENG-01、ENG-02、PERF-01、PERF-03 与上一节新增的 ENG-04、ACK-01。它们的共同点是需要
  反例测试（邮箱换代、失败原因通道、critical 队列满队、ACK 重武装），本轮没有硬件的运行期证据，不按
  「已修」上报。
- **DPI-01**：源码修复与纯函数用例已落地，实机自然启动与 100–200% 缩放仍未验证。

2026-10-28 性能批次工作区实现（行为测试已过，实机硬件门禁待验，见 V-01/D-011）：节奏
定时器改高分辨率等待定时器；播放遥测拆独立 `playbackTelemetryChanged`（约 4 Hz 节流、
异常态即时）并按输入身份缓存投影集合（markers/媒体信息/兼容性），消除播放期每帧
`stateChanged` 全量扇出；多源解码器并行开箱、每 actor 只做一次源身份验证；PTS 索引落盘
（含回读校验）移出探测关键路径；D3D11VA 零拷贝 y/uv SRV 随解码表面资源缓存；对齐分析
worker 降 BELOW_NORMAL；软解 sws 色彩配置按需重设；悬停缩略图缓存改插入序逐出、播放中
跳过无效 grab。测试入口：`SteadyDeadlineSchedulerTests`（新增亚量子精度用例）、
`ReviewControllerTests`（遥测拆分/缓存重建）、`tst_timeline_thumbnail_cache.qml`。暖缓存
启动 A/B（同机交替、GameViewer 虚拟显示会话）：空载 810→790ms、单源 803→780ms、双源
首帧组 1240→1200ms（median）；物理屏硬件门禁未跑，虚拟桌面数字不作为发布证据。

2026-10-27 体验改进的工作区实现还包括：返回查看保留编辑会话与撤销历史；未保存的后台任务和窗口退出提供按任务命名的保存/放弃入口；图片/视频标签补键盘、可访问名称和活动项滚入视野；原大按钮明确为「原大 100%」。新截图与样式/DPI 测试来自测试宿主，不作为 DPI-01 的自然启动或真实读屏验收。详见同一[实施方案](../plans/ux-continuity-2026-10-27.md)。

### DPI-01 取证更正

10-24 的“应用继承父进程 DPI 感知性、DPR=1、提高 z 即可修复”解释已被 10-25 的记录修正：
应用实测为 per-monitor DPI 感知，系统缩放 100% 与 EDID 推导 DPR≈1.77 可以同时出现。
该机器不同启动路径都观察到底部裁切，旧 z=500 实验未能复现。当前只保留裁切现象和
默认窗口未按屏幕约束的源码事实，不把绘制顺序当作已确认根因。

历史截图在 `out/verification/ui-design-2026-10-24/baseline/`，更正记录在清理前提交
`4ca3abb` 的本文件中。复验要记录父进程、应用实际 DPI 感知性、Qt DPR、客户区物理尺寸、
可用屏幕几何与自然启动结果；强制调整窗口后截图不能代替自然启动验收。

## 已实现但仍有验证缺口

| ID / 范围 | 已有行为与待验边界 | 入口 |
|---|---|---|
| V-01 / V-02 / D01 | 播放状态、两种连续性策略、完整帧组、呈现 ACK 已有；负载统计和最新候选的真实 D3D11VA 长播仍需验证。呈现 ACK 间隙、调度跳过与源重复标记不能相加，也不等于物理屏幕漏刷 | `PlaybackCoordinator`、`RenderAckRelay`、`PlaybackEvidenceCollector`；[trace](trace-schema.md)、[runner](../self-hosted-runner.md) |
| V-03 | 索引/时间/人工锚点和 PTS 精确性已有；30/60 fps、VFR、非零起始、偏移与缺帧真实素材仍需端到端核对，换 GT 不得改变时间线或区间 | [对齐契约](../alignment.md)、`ComparisonExactnessTests`、`MultiSourceFrameProviderTests` |
| V-05 / I-02 / COLOR5 | 视频规范化平面、图片 RGBA8/转换后 RGBA16 与 ICC 显示转换已有；源平面保真、完整高位深和 Windows 输出/跨屏/物理颜色不能据此宣称已验证 | [媒体支持](../media-support.md)、[颜色契约](image-color-contract.md)、[颜色样本](image-color-samples.md) |
| V-07 | MAE/PSNR、当前帧优先窗口、阈值/策略缓存复用、泳道已接线；真实素材性能与数值口径走查待验。指标使用转换后全帧 RGBA8 RGB，不受视口 ROI/显示增益影响 | [窗口性能](pair-metrics-window-performance.md)、[阈值复用](pair-metrics-threshold-reuse.md)、[ADR 0005](../adr/0005-pixel-difference-metrics.md) |
| V01 / V02 / V03（路线） | 同 ROI 局部窗、全画布直看、双参考差异均已有；D3D11VA、DPR、旋转/SAR、三源持续切对仍待实机走查。盲比随机顺序等扩展未做 | `ComparisonSurface`、`D3d11ComparisonRenderer`、`MainQmlContractTests` |
| TASK-01 / D-022 / D-024 | 图片/视频任务标签已随 `9640b52` 合入，`377cb2a` 补同名消歧。待验视频切回时延/稳定性、后台任务资源、倍率恢复和标签窄窗/DPI。图片仍只有一个编辑会话，跨重启恢复明确不做；批量多开、关闭其他/全部不属于现有能力 | [视频标签契约](../plans/video-task-tabs-design.md)、`ImageTaskHostTests`、`VideoTaskHostTests`、`MainQmlContractTests` |
| FLOW1 / FLOW2 / E01 / E03 | 图片记录恢复、来源/观察信息交接和精确样本 UI 已接通；真实素材的打开→记录→重开→恢复→导出独立读回仍需当前候选证据；便携问题包与批处理是后续扩展 | `IssueLogController`、`SampleExportController`、`SampleExportWriter`；[记录方法](reproducible-baseline.md) |
| UX-13 / D-020 | 窄窗重排已实现；2026-10-28 工作区改动按可用宽度恢复图片打开／差异入口，并避免视频时间码与播放按钮重叠，具体验证见[工作区布局](../plans/ui-workspace-redesign-2026-10-24.md#13-2026-10-28-显示与交互精修)。详情浮层临时遮挡、紧凑状态点辨识与少量 fontMicro 可读性仍待用户复核。旧 ≤120 px 走带目标未达（10-22 为139），不能自动放宽为139；单/双源 compact 添加按钮边界仍待补 | [窄窗约定](../plans/ux13-narrow-window-redesign.md)、[工作区布局](../plans/ui-workspace-redesign-2026-10-24.md) |
| A4b / S-01 / S-02 | 二次启动 PID 握手和前台授权已有；Explorer 双击唤起、旧安装路径迁移与用户机器重复菜单需真实桌面复验。测试进程缺前台授权时的结果不能证明产品成功或失败 | `StartupRequestBroker`、`FileAssociationRegistration`、shell smoke |
| RELEASE | 干净 Windows 解压包、应用本地运行库、物理显示、D3D11VA 长播及适用覆盖率等门禁需绑定具体发布候选；过去的例外不会自动豁免新版本 | [发布记录](../releases/)、[打包政策](../building.md#zip-只打包必要运行时)、[真实走查](real-environment-verification.md) |

## 已解决或已有能力索引

以下保留旧 ID 以免重复开工。“已实现”不覆盖上一节列出的人工/性能验证缺口。
逐轮命令和测试数字可在 Git 历史读取，不能用旧数字认证当前构建。

| ID | 当前状态与证据入口 |
|---|---|
| V-04 / MEDIA1 / MEDIA2 | 支持范围已写入 README/媒体支持；AV1/VP9 当前不扩展（D-007），不是等待无条件实施的缺陷 |
| V-06 | `PreviewThumbnailService` 已为未播放帧独立解码；无结果降级时间码，近似缓存不冒充精确帧。多源预览条带尚未实现 |
| I-01 / I-05 | Alpha/背景/通道、独立 Alpha 差异与统一 RGB MAE 已有；增益只改差异显示，不改统计。`ImagePairLoader::analyzeDifference/renderDifference` |
| I-03 | PNM 家族入口及格式样本已具备，实际支持边界以 [媒体支持](../media-support.md) 为准 |
| I-04 | 图片真实尺寸与视频物理像素尺度已有；[视口像素尺度](viewport-pixel-scale.md)保留回读方法与证据 |
| I-06 | 单像素取样与差值场/渲染缓存已有；≤2K 使用基线不等于 8K 专项验收 |
| C-01 | 图片不做三图（D-005）；视频三源、GT 与比较对分离已实现 |
| C-02 / F01 / D02 | 图片单图 A/B 与淡化、插帧协议导入/生成帧导航、附近帧诊断卡已有；不按奇偶猜生成帧，真实素材审查待验 |
| U-01 / U-02 / U-03 | 打开入口、对调/单侧替换、控制条 hover 保持均已接线；`MainQmlContractTests`、`ImageReviewControllerTests`、`tst_player_osc.qml` |
| V-08 / FLOW3 / C1 | 文件夹/共享最近历史、单图目录翻页、单视频续播已有；[文件夹契约](video-folder-browser.md) |
| V-09 / V-10 | 时间线身份色与窄窗 OSC 遮挡已有修复；[标记图例](timeline-marker-legend.md)与主窗契约 |
| P1-A / P1-C / P1-D / P2-A / P2-B（10-05 审查） | 导出时间原点、阈值高亮背景、事务发布、VFR 导出入口和坏点占比已修；`ClipExportWriterTests`、`ClipExportControllerTests`、`ComparisonSurfaceTests`，相关 clip-export 专题保留 |
| P1-B / P2-C / P2-D（10-05 审查） | 指标/显示口径和 ACK 边界已有说明；图片 ICC 已实现，物理显示和视频完整色管仍见 COLOR5/V-05 |
| UX-01–12 / UX-14–20 / EX-03–04 / EX-06–08 | 相关菜单/键盘/文案、模式入口、令牌与导出反馈已实现；UX-15 手柄键盘路径、UX-17 字号令牌均已收口，UX-19 交付选择保留 D-019 |
| E-01 / E01 / E02 | 构建依赖修复、启动测量/首帧前 splash、诊断可关闭已有。构建环境以 [构建指南](../building.md) 为准；[启动专题](startup-performance-v2.md)只证明其标注版本 |
| 五工作区布局 | `9640b52` 合入，`377cb2a` 收敛背景选择器/同名标签，`d3ce2a9` 收敛图片来源 pill；弹窗轻阴影仍为推迟项，DPI-01 未修复 |

## 后续能力与更新规则

未启动的运动/纹理/时间指标、配方批处理、标签变换接线、数据谱系等继续保留在
[能力索引](capability-ledger.md)及[长期路线](../CompareStation_长期产品与技术路线_2026-10-10.md)。
它们不是已经批准的每批前置条件。生成帧规则来自用户提供的协议；媒体范围、三源默认布局、
SAM2 发布方式等取舍以决策登记册为准，不能由文档清理擅自改判。

新问题更新对应行；关闭时记录解决行为、提交/差异、验证范围与结果，以及剩余限制。
局部修复按影响范围验收，不要求先清完本页。历史取证可用
`git show 4ca3abb:docs/engineering/visual-review-backlog.md`，更早状态用 `git log -- <路径>` 查找。
