# Toy / VCStation：最佳专业视频播放器与多源对比工作站整改总计划

> 审查基线：`sonwe1e/Toy`，`main @ d04339c66956c4aca26a2d6d8d3e680229ea5aad`。
> 最新代码发布基线：`dd21a4cb4c2bfa35c912c925c3b9397c2d58dddd`（VCStation 1.6.0）；`d04339c...` 主要是文档重组。
> 审查日期：2026-08-10。
> 目标产品定义：**以帧准确、多源同步、视觉差异分析为核心的本地专业视频播放器/评审工作站**，而不是先追求 VLC/mpv 式“所有媒体功能都支持”的通用播放器。

## 核心结论

Toy 已经拥有一套值得保留的专业级内核：C++20、Qt Quick、FFmpeg、D3D11、完整 `FrameSet` 原子发布、Presentation ACK 后提交、VFR/非零 PTS/B-frame 处理、硬件解码与软件回退、设备代次隔离、严格的异步身份以及较完整的单元/组件/GPU/性能门禁。**推倒重写、替换为 Qt Multimedia、或把每一路视频交给独立播放器同步，都不是正确方向。**

当前最阻碍它继续成长的不是“少几个按钮”，而是四个结构性问题：

1. **语义耦合**：`Reference` 同时承担业务对照源与 canonical timeline master；切换 Reference 会重建时间线。`Active Pair` 又用全局持久化的 A/B/C 序号表示，无法稳定表达“GT 对某个候选”。
2. **状态集中**：`PlaybackCoordinator.cpp`、`ReviewController.cpp`、`ReviewShellController.cpp`、`Main.qml` 仍承载过多状态与工作流。现有 façade 已预留窄 ViewModel 迁移入口，但迁移尚未完成。
3. **播放工作负载不对称**：连续 `+1` 已有顺序解码流；连续 `-1` 和多帧步进仍走 Exact Seek。Range Loop 仍由 QML 监听已呈现帧后再发 seek，不能从内核层保证不越过 Out 点。
4. **对比能力停留在“可视化模式”**：现有 Side/Three-up/Reference-focus/Wipe/Diff/Analysis-grid 很有价值，但还缺少身份明确的候选选择、Blink/Blend、像素检查、量化指标、范围统计、结果溯源和多候选扩展模型。

因此推荐的路线是：

```text
先建立可验证基线
    ↓
分离 TimelineMaster / Reference / ActivePair / FocusedSource
    ↓
把会话专属状态从全局 Preferences 和 Main.qml 移入 Application
    ↓
拆分窄 ViewModel 与内部工作流，但保留单一串行媒体真相
    ↓
补齐双向逐帧、内核 Range Loop、显式播放策略、可控 Scrub/Thumbnail
    ↓
升级多源比较、量化分析与 N 源逻辑模型
    ↓
最后扩展 HDR/色彩管理、音频、AV1/VP9、签名与供应链能力
```

## 最优先执行的五个工作包

| 优先级 | 工作包 | 直接解决的问题 | 必须先于 |
|---|---|---|---|
| P0 | Comparison Semantics V2 | Reference/canonical 混淆、A/B/C Pair、拓扑变化后选择失真 | 所有多候选与统计功能 |
| P0 | State Ownership Split | 巨型控制器、Main.qml 状态机、全量投影 | 大规模 UX 扩展 |
| P0 | Navigation/Playback V2 | 后退逐帧卡顿、隐藏式 catch-up、QML Range Loop | 高质量播放体验 |
| P1 | Scrub/Thumbnail/Status | 时间轴拖动迟钝、未播放位置无缩略图、反馈分散 | 长视频工作流 |
| P1 | Comparison Analytics | 缺少量化指标、最差帧、ROI/范围统计、多候选排名 | 专业评测工作流 |

**禁止**在前三个 P0 工作包完成前直接把上限从 3 路改成 8/16 路，也禁止直接在 `DifferenceEdge` 上增加更多枚举。否则会把当前的序号耦合扩大到更多模块，后续必然二次整改。

## 文档目录

1. [`01_现状架构与证据审计.md`](01_现状架构与证据审计.md)
   当前架构、可保留资产、代码证据、版本历史和审查边界。
2. [`02_根因与缺陷调查.md`](02_根因与缺陷调查.md)
   已确认问题、待实机验证风险、复现路径、根因链和优先级。
3. [`03_目标架构设计.md`](03_目标架构设计.md)
   目标领域模型、工作流拆分、异步身份、缓存、渲染和 ViewModel 设计。
4. [`04_播放内核与交互体验.md`](04_播放内核与交互体验.md)
   双向逐帧、播放策略、Range Loop、Scrub、时间轴、缩略图、音频时钟扩展。
5. [`05_多源对比与质量分析.md`](05_多源对比与质量分析.md)
   GT/候选选择、N 源模型、Wipe/Blink/Blend/Diff、指标引擎与结果溯源。
6. [`06_UI_UX与可访问性.md`](06_UI_UX与可访问性.md)
   信息架构、控件行为、错误恢复、快捷键、键盘/读屏/DPI 设计。
7. [`07_实施路线与代码改动.md`](07_实施路线与代码改动.md)
   逐批次改动、文件级任务、迁移策略、测试先行与退出条件。
8. [`08_独立正确性审查.md`](08_独立正确性审查.md)
   architecture / root-cause / implementation / independent correctness / regression / final verification 六个独立角色的职责和冲突裁决。
9. [`09_回归与边界测试矩阵.md`](09_回归与边界测试矩阵.md)
   媒体、拓扑、时序、GPU、输入、窗口、文件系统、故障注入的组合矩阵。
10. [`10_最终验证与发布门禁.md`](10_最终验证与发布门禁.md)
    CI、硬件、性能、视觉 Golden、Soak、安装升级、签名和发布证据。
11. [`11_关键决策与备选方案.md`](11_关键决策与备选方案.md)
    为什么不重写、不换播放器内核、不让 QML 做媒体时钟，以及各替代方案的取舍。
12. [`12_可复现实验与验收用例.md`](12_可复现实验与验收用例.md)
    当前问题的最小复现、实验记录格式和每个里程碑的验收场景。
13. [`13_风险登记与完成定义.md`](13_风险登记与完成定义.md)
    风险、缓解措施、Definition of Done 和项目健康指标。
14. [`reviews/00_Synthesis.md`](reviews/00_Synthesis.md)
    五份独立审查报告（`A_architecture`、`B_root_cause`、`C_migration_surface`、
    `D_correctness_review`、`E_regression_review`）的综合裁决与冲突解决记录。

## 证据可信度标记

文档中的结论使用以下标记，防止把推测写成事实：

- **C（Confirmed）**：能由当前源码、测试或版本历史直接确认。
- **H（Hypothesis）**：由结构和时序推导出的高概率风险，必须以最小复现或硬件实验确认。
- **G（Gap）**：明确缺失的能力或体验，不等同于已有 Bug。
- **D（Decision）**：推荐的架构或产品决策。
- **O（Optional）**：中长期可选扩展，不进入当前关键路径。

## 审查方式与限制

本次审查读取了仓库结构、核心头文件/实现、QML、架构文档、发布说明、CI 和硬件性能工作流。当前执行环境无法在 Linux 容器内构建这套 Windows/Qt/D3D11 完整产品，也未接入目标 GPU/显示器，因此：

- 代码可直接证明的结论标为 C；
- 需要 Windows 实机、真实媒体或 GPU capture 的结论标为 H；
- 未声称“本地测试已通过”；
- 最终计划把每个 H 都转换成了可运行实验和明确的期望结果。

当前会话没有可调度的独立子进程式“子 Agent”接口，因此没有伪造并行 Agent 已执行的事实。为保持独立性，审查按六条相互隔离的证据轨道完成：架构、根因、实现、独立正确性、回归边界、最终验证；合并时只接受代码、测试、日志和可复现实验，不以投票消解冲突。

## 不可破坏的现有不变量

任何整改都必须继续满足：

1. 一次呈现只发布一个完整 canonical `FrameSet`，不得出现 A 已更新而 B/C 尚未更新。
2. `displayedFrame` 只能在匹配的 Presentation ACK 后提交。
3. 所有异步结果必须携带足够的 session/epoch/topology/timeline/playback/device/request 身份，陈旧结果不得提交。
4. GUI 线程与 render 线程不得等待解码、探测、分析或文件 I/O。
5. 所有队列、缓存、read-ahead、反向窗口和分析结果必须有界。
6. 一个命令必须恰好得到一个终态；正常取消不得伪装成媒体故障。
7. Domain/Application 不得泄漏 Qt、FFmpeg、Win32 或 D3D11 类型。
8. UI 只发意图并投影状态，不拥有媒体时钟、范围循环和异步生命周期真相。
9. Difference/Metric 必须报告比较域和精确性，不得把缩放或色彩转换后的结果称为 pixel-exact。
10. 拓扑切换失败时保留最后一个可用 Ready Session，不得先销毁旧会话再尝试新会话。

## 建议的产品边界

“最好的视频播放器”需要先明确赛道。此计划建议 Toy 优先成为：

> **本地、专业、帧准确、多源同步、可解释差异与质量分析的播放器。**

它不应在当前阶段同时追求网络流媒体、DRM、电视直播、媒体库、插件生态和所有字幕格式。音频、HDR、AV1/VP9、会话保存可以进入后续里程碑，但不能以破坏 FrameSet 原子性、时间线确定性或比较溯源为代价。

## 使用方式

先阅读本文和 `02_根因与缺陷调查.md`，确认问题优先级；随后以 `03_目标架构设计.md` 作为不可随意偏离的目标，以 `07_实施路线与代码改动.md` 逐批次落地。每个批次开始前先从 `12_可复现实验与验收用例.md` 添加失败测试，合并前按 `08_独立正确性审查.md` 和 `09_回归与边界测试矩阵.md` 审查，发布时执行 `10_最终验证与发布门禁.md`。

## 完整性清单

文档行数、字节数与 SHA-256 见 [`MANIFEST.md`](MANIFEST.md)。归档中同时包含 `MANIFEST.sha256`。
