# 收尾团队 Journal

跨会话唯一真相源。规则见 [团队协作章程](team-charter.md) §5：只追加与更正，
不删历史；先读后写；完成声明 ≠ 效果证据；未知写未知。

记录格式：`日期 | section | 角色 | 动作与证据 | 决策与理由 | 坑 | 下一步`

---

## 2026-10-31 | §0 基建与基线 | 架构师

**状态盘点（只读命令核实）**

- HEAD = `c0dba16` chore(release): prepare CompareStation 2.7.0。
- 工作树有**未提交的大规模重构**：108 文件、+610/−34876，主体是把 `src/ui_qml/src/` 与
  `src/platform_windows/src/` 的平铺文件迁入功能子目录（`image/` `playback/` `comparison/`
  `files/` `outputs/` `workspace/` `graphics/` `support/`），CMakeLists、测试 CMake、
  三个 QML 测试与新增 `docs/architecture/repository-map.md` 同步更新。内容为重排而非重写。
- 构建痕迹：`out/build/dev/build.ninja` 生成于 10-30 13:00，`CompareStation.exe` 构建于
  10-30 13:20；全量 ctest 日志存在（1452 例）。推断：重构当日已构建；**未经本轮复验**。

**决策**：§0 第一动作是复验工作树构建健康度（增量构建），通过后才在其上规划四类润色。
未授权前不 commit、不 stash、不改动重构本身。

**坑（已知）**：`out/build/*.log` 是共享 scratch，读取时注意可能是旧副本；本轮构建结论
以当次命令输出为准。

**坑（缓存失效，2026-10-31 实踩）**：dev preset 的 `CMakeCache.txt` 里
`CMAKE_MAKE_PROGRAM` 指向已卸载的 WinGet 版 ninja；wrapper 缓存校验判 stale，
只有 `-Fresh` 能通过（`-Configure` 会被同一校验拦下）。`-Fresh` 保留 `out/vcpkg*`，
全量重编 dev 预设。无 sccache（本机未安装），编译缓存关闭。

**下一步**：跑 `pwsh tools/build/build.ps1 -Preset dev -Fresh` 全量重建；绿 → 按
`measure-startup.ps1` 协议建启动基线（无文件 + 带 1080p60 素材，release，5 轮 + 1 预热）。
重构本身的内容验证昨日已做（ctest 日志在 `out/build/dev-ctest.log`），本轮 §0 改动为零代码
变更（仅重建），按影响范围（D-021）不默认重跑全量套件；全量回归留到 §5 总验收。

---

## 2026-10-31 | §0 完成 | 助手

- dev 与 release 双双 `-Fresh` 全量重建成功（各 304 步，MSVC 头依赖校验通过）。
  工作树重构（10-30）编译健康，可作为后续工作地面。
- 启动基线（release、热缓存、5+1 轮）落盘：
  `out/startup-measurements/polish-b0-nofile-20261101-020220/`、
  `out/startup-measurements/polish-b0-onefile-20261101-020236/`。
- 基线关键值（中位）：无文件 spawn→窗口 454.6 ms、→exec 735.3 ms；带文件首帧提交
  （kind 8）820.1 ms；QML 装载段（splash-painted→qml-loaded）359.1 ms；
  show-enter→sg-initialized 195.2 ms；sg-init→首帧 85.4 ms；进程装载 23.9 ms。
  与历史 v2.1.0 数字（645.8/949.9）跨月，只作参考，**不作回归判定**。

---

## 2026-10-31 | §1 启动速度 · 研究 | 研究员

**问题**：用户提案「首次打开后常驻后台一小段时间，二次打开更快」。用户所说「每次都要审阅
环境」来自 splash 副标题「正在准备审阅环境」（`DesktopApplication.cpp:223`）——启动路径
**没有**环境审计，耗时在 QML 实例化 + D3D11 设备/场景图初始化。

**已核实的基础设施（读码，非推断）**：

- 单实例交接已完整：`StartupRequestBroker`（QLocalServer/QLocalSocket 命名管道 + 选举锁文件
  + PID 问候 + 前台授权 + `OK\n` 确认）；`Main.cpp` 把转发请求接到
  `StartupRequestDispatcher` → `applyStartupRequest` → `activateWindow()`
  （show+raise+SetForegroundWindow，已支持最小化恢复）。
- 退出路径：`Main.qml onClosing` 仅守卫未保存图片编辑，否则默认 quit-on-last-window-closed。
- broker 仅非 smoke/非 stillImage 模式创建（测量与自动化路径不受影响）。

**候选与结论**：

| 候选 | 评估 |
|---|---|
| A. 关窗后进程驻留（用户提案） | **推荐**。close→hide；转发请求路径已存在；加空闲超时（匹配「一小段时间」）与显式退出入口。预期二次打开 ≈ IPC+show ≈ 数十 ms（冷启动 735 ms）。风险：驻留 RSS 待实测；退出语义偏离 Windows 惯例（需设置开关+提示）；长时间驻留更易遇显示变更/设备丢失（与 ENG-02 设备代际相关）；**测量口径变化**：`measure-startup.ps1` 每轮靠关窗收尾，常驻后第 2 轮起会转发给驻留进程——冷启动 A/B 必须改为轮间 kill 或独立驻留基准，否则数据失真 |
| B. 暖 helper 进程 | 不推荐。Windows 无法跨进程转移 HWND/QML 对象；进程装载仅 23.9 ms 且 DLL 常驻 standby，收益接近零 |
| C. qmlcachegen 预编译 | 已否决（startup-performance-v2.md Step 2：实例化主导，零收益）。仅作对照 |
| D. 冷启动本体再压缩 | 无近期廉价杠杆：Step 8 证明成本弥散（按组件懒加载只有比例切片）；Step 5 证明请求提前架构性不可行；sg-init 195 ms 属设备生命周期架构改动。splash 已覆盖感知（~44 ms 上屏到首帧） |

**待用户决策（charter §8 产品取舍）**：常驻的退出语义与内存代价——(a) 默认开启 +
空闲自动退出（如 15–30 min）+ 菜单「退出」+ 设置可关；(b) 默认关闭、设置内开启；(c) 不做。
模型建议 (a)：用户工作流（反复开关做对比审查）收益最大，代价有界且可关。**未获选择前不动手实现。**

**下一步**：用户决策后，架构师定 A 的设计契约（驻留条件、超时、退出入口、设备丢失行为、
测量协议适配），builder 实现，verifier 补行为测试 + 变异证据 + 驻留 RSS/二次打开实测。
---

## 2026-11-01 | 工程整理与缺陷复核 | 协调者 · 架构师 · 研究员 · 构建者

**动作与证据**

- 目录重组落库：`315ac65 refactor(repo): group ui and platform sources by feature`。提交前把 77 组
  删除/新增对按 blob 哈希逐一核对为逐字节迁移（两个 `Module.cpp` 是同名交叉比对误报，内容正确），
  `git status` 由「77 D + 14 未跟踪」变成 77 条 rename；提交前跑过增量 dev 构建，日志见
  `out/verification/tidy-build-2026-11-01.log`。
- 仓库根清理：删除 `fuzz.obj`、`probe.obj`、`symbolize.obj`、`vc140.pdb` 与 `Testing/`；`out/try-run`、
  `out/package/verify-28168` 是过期包副本（全仓无路径引用）一并删除。`.gitignore` 原本已覆盖这些产物，
  规则未改。
- 团队记录补全：`TEAM.md` 迁入 `docs/engineering/team-charter.md` 并追加「本仓库落地约定」（记录位置
  映射、证据状态、与构建/验收规则的衔接）；journal 头部死链改指该文件；`docs/README.md` 增加索引行。
- 打包链实跑（同日）：release 增量构建 → `cpack --preset release-zip` → `verify-release-package.ps1`。
  CPack 前的 staging 门通过，产物 `CompareStation-2.7.0-windows-x64.zip` 27.1 MiB（sha256
  `3D9E0BE1…B1AB360`），包自校验 16 项全 PASS。这是本机打包证据，不等于发布门禁。
- 打包与发布：`release.yml` 的未签名发布白名单删去没有发布记录的 `v2.6.0`/`v2.6.1`；
  `verify-release-package.ps1` 的示例版本改为 2.7.0；两个 adapter 的 CMake 源清单补入 10 个私有头，
  让「CMakeLists 是源文件事实来源」这条自述成立。
- 缺陷复核（只读，两名独立 agent 分行 + 主线核对）：ENG-01、ENG-02、PERF-01、PERF-03、DPI-01 在当前
  源码**全部仍成立**，一条也不能关闭；新增 ENG-04（`clear` 丢返回值）与 ACK-01（重试未重武装背压）
  写入台账。DPI-01 已改源码：`LayoutMetrics.fitWindowToScreen` 纯函数 + `Main.qml` 绑定 + 宿主居中
  钳制，锁定用例在 `tst_layout_metrics.qml`，实机未验。

**更正**

- 本文件 10-31 首条记的「108 文件、+610/−34876」是当时扫描值；提交前 `git diff HEAD --shortstat`
  为 109 files, +619/−34876（新增 `CheckNoPlaybackQImageTests.cmake` 与索引行）。

**决策与理由**

- 只做能验证的修复。DPI-01 用纯函数切出可测边界；ENG-01/ENG-02/PERF-01/PERF-03 牵动图形恢复协议与
  调度语义，且 `GpuTransferActorTests.cpp:543-578` 现在锁定的正是 ENG-02 的缺陷行为，必须与修复同批
  改写，因此不在本轮顺手动，避免留下未经验证的并发改动。
- charter 采用重建而不是把 journal 链接改指别处：§5/§8 的引用需要一个真实存在的规则来源，「记录
  位置映射」也能阻止团队再建第二份状态表。

**坑**

- 本机 DSH 沙箱对既有目录只读：`docs/`、`src/`、`out/`、`.git/` 的写入与删除都被拒；文件工具
  （write/edit）可写而 `pwsh` 不可，构建、测试与提交需单独申请一次更宽的执行权限。这是本机状态，
  不是仓库缺陷。

**下一步**

- ENG-02 + 邮箱换代恢复（同时改写 `RejectsWorkAfterDeviceLossTransition`）、ENG-01 的失败原因通道、
  ACK-01 的背压重武装，各自配反例测试后单独提交。
- DPI-01 的自然启动、跨屏与 100–200% 缩放实机复核。

---

## 2026-11-01 | ENG-02 图形恢复协议 | 架构师 · 构建者

**决策（协议先定，再改）**

两条候选：A 组合根按代次重建 actor/通道/邮箱；B 同一 actor 复活 + 邮箱换代前移。选 B：
`PlaybackCoordinator::Dependencies` 的 `renderChannel` 是构造期固定的 `shared_ptr`，A 需要新增跨层
替换 API 与线程切换约定；B 只动 platform 适配器，而且 `D3d11ComparisonRenderer::ensureDeviceResources`
本来就按 `(device, generation)` 重建着色器与状态，渲染侧已具备恢复语义。tombstone 仍只用于拒绝
**旧代**结果，不再用于永久关闭。

**动作与证据**

- `GraphicsDeviceBroker`：Loss 之后接受**身份不同**的新设备（同一台被判丢失的设备继续拒绝），
  收养后 generation+1 并发布 Ready；新增 `platform.GraphicsDeviceBrokerTests.AdoptsARebuiltDeviceAfterALossButNotTheLostOne`。
- `GpuTransferActor`：`submit` 在「已被换代墓碑」或「仍持旧代租约」时复活（清 lease／scope 高水位／
  lossReported，调用 `advanceDeviceGeneration`，统计 `recoveredGenerations`）；`reportLoss` 与两次
  租约失败不再 `FrameMailbox::shutdown`；`isClosed()` 改为只表示 shutdown。
- 测试：删除锁定缺陷行为的 `RejectsWorkAfterBrokerDeviceLossTransition`，换成
  `RejectsWorkFromTheLostDeviceGeneration`（旧代仍被拒）与 `ResumesWorkAfterANewDeviceIsAdopted`
  （换代 → 收养 WARP 新设备 → submit 返回 Accepted → 新代可读）。
- 运行证据：`platform.` 组件套件 28 例全过（`out/verification/eng02-platform-suite-2026-11-01.log`）；
  UI 全矩阵 731 例全过（`out/verification/ui-matrix-2026-11-01.log`，DPI-01 之后补跑）。

**坑**

- 第一次实现只在 `!accepting` 时复活，测试暴露另一种顺序：换代后 worker 还没跑
  `invalidateForBrokerTransition`，actor 仍持旧代 lease 且 `accepting` 为真，`submit` 先撞上
  「租约代次不符」返回 StaleContext。复活条件必须同时覆盖「持旧代租约」。

**未验证**

- 真实 TDR／驱动重置下 Qt 场景图是否重新交出设备——这是本路径能否被触发的上游条件，未验证。
- ENG-01 仍未修：恢复前的 publish 失败仍按 `kMediaDecodeFailed` 上报，`handleGraphicsReady` 不会清它。

---

## 2026-11-01 | ENG-01 失败原因通道 | 构建者

**动作**：`RenderPublishResult` 增加 `DeviceUnavailable`／`StaleContext`／`InvalidSet`，通道逐项映射
`GpuTransferSubmitResult`；协调层三处 publish 失败统一走 `renderPublishError`，设备类原因映射为
`kGraphicsUnavailable`／`kGraphicsDeviceLost`（操作标 `kFramePresentation`），只有帧组不可用仍是
`kMediaDecodeFailed`。图形类错误会被 `handleGraphicsReady` 清除，恢复后不再常驻。

**证据**：`application.PlaybackCoordinatorTests` 102 例 + `platform.` 187 例共 289 例全过
（`out/verification/eng01-tests-2026-11-01.log`）；`format-check` 通过。新增
`DeviceUnavailablePublishKeepsItsReason`、`StalePublishContextIsReportedAsDeviceLost`（用可配置
publish 结果的 FakeRenderChannel 驱动一次交互步进失败）与
`platform.D3d11RenderChannelTests.KeepsTheActorReasonInsteadOfCollapsingEveryFailure`。

**没做**：跨代 publish 不再被误报，但仍按一次失败终止本轮播放；自动重试或续播要另立判据，否则
容易变成「丢帧掩盖问题」。真机 TDR 仍未验，ENG-01 与 ENG-02 都停在「源码已修，待实机验证」。

---

## 2026-11-01 | ACK-01 + ENG-04 小切片 | 构建者

**动作**

- ACK-01：`retryPendingAcknowledgement` 在重试仍遇 ACK 队列满时补一次 `notifyAckBackpressured()`。
  原来只有首次路径会通知；重试那次把 pending 留着却不重排重绘，暂停/逐帧时没有新帧来触发下一次
  重试，协调层只能等满五秒的呈现超时。
- ENG-04：`application::IRenderChannel::clear` 改为 `[[nodiscard]] bool`，通道原样返回 actor 的结论；
  协调层六处调用显式 `static_cast<void>`（拒绝即「没有可作废的 scope」），假通道与 `CapturingRenderChannel`
  同步返回 true；通道测试补「同一 scope 只能装一次墓碑」。

**证据**：`application.PlaybackCoordinatorTests` + `platform.` 共 289 例全过
（`out/verification/ack-eng04-tests-2026-11-01.log`），`format-check` 通过。

**坑**：`clear` 加上 `[[nodiscard]]` 后，`GpuTransferActorTests.cpp:389` 那处裸调用在 `/W4 /WX` 下直接变成
C4834 编译错误——接口收紧会立刻暴露所有历史忽略点，这类改动要先 grep 全部调用方。

**未做**：ACK-01 没有新增反例（需要「ACK 队列保持满 + 暂停态重绘」的夹具，`CountingActivitySink`
是接缝）；协调层尚未消费 `clear` 的返回值。两者都写进了台账的当前缺口列。

