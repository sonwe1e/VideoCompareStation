# 未使用代码与过时测试审计（2026-10-18）

范围：`src/`（domain、application、presentation_contract、app、media_ffmpeg、persistence_json、
platform_windows、shell_windows、ui_qml）、`tests/`、`tools/`。基线为当前工作区（含未提交改动），
审计期间未修改 HEAD。

方法：五路并行只读审计（core、media、platform/shell、ui_qml、tests/tools/QML），每一路先给出
`file:line` 级候选，再由主审计逐条复核：精确词检索生产/测试/工具/文档引用，读实现与调用点，
必要时用 `git log -S` 追引入与移除历史。**子审计的结论不直接采信**；本轮至少推翻了 4 条误报
（例如 `MultiSourceFrameProvider` 的队列容量实际已钳制、`probeSinglePixel` 并不存在、
`sourceSetSignature`/`videoSessionEpoch` 由变更信号读取）。只有同时满足“零生产引用、零测试
引用（或测试本身只测该死代码）、零工具/文档契约”的符号才删除。

## 1. 结论

- 删除 14 组已证实无任何调用者的代码，净减约 480 行；其中包含一套遗留双栏几何、一个被
  `FileTraceSink` 取代的内存 sink、一套未接线的对齐缓存键。
- 删除 2 个结构性无效的禁用测试，改写 1 个并启用，修正 1 个仍禁用测试的读取与断言；禁用测试从
  4 个降到 1 个，保留的那个对应真实且尚未实现的无障碍缺口。
- 修复 5 个真实缺陷（循环播放跳帧漏计、启动图竞态、持久化仓库生命周期、Explorer 回调终止风险、
  渲染器释放后残留 ACK 状态）；另有 2 个高优先级缺陷因需要改接口/状态机，只给证据不擅自修。
- `docs/agent-guide.md` 中指向已删除文件与旧架构的描述已更正。

## 2. 已删除

| # | 符号/文件 | 证据 | 影响 |
|---|---|---|---|
| 1 | `media::decodeStillImageFile`（声明+定义，约 66 行） | 全仓仅声明与定义两处；生产走 `decodeStillImageBytes`，测试亦无调用 | 删除第二套 demux+decode 流程，能力不损失 |
| 2 | `IssueRecordRepository::{documentPath, loadDocument, saveDocument}` | 三者零调用；端口只有 `load/save`。测试里的 `documentPath()` 是 fixture 自己的临时路径函数 | 端口面收窄；`loadDocumentFrom` 改为私有 |
| 3 | `SurfaceSplitLayout`、`computeSurfaceSplit`、整数版 `aspectFitRect` | 前两者仅 `ComparisonSurfaceTests` 使用；生产用 `computeSurfacePanelLayout`/`aspectFitRectFloat`。遗留双栏模型已被 N 源面板取代 | 删除旧双栏几何；测试改用 `aspectFitRectFloat`，覆盖不降 |
| 4 | `MemoryTraceSink` 及其测试 | 生产只用 `FileTraceSink`；`MemoryTraceSink` 仅被自身测试构造 | 删除被取代的默认 sink |
| 5 | `makeDerivedAlignmentCacheKey`、FNV 辅助函数、`kSequenceAlignmentAlgorithmVersion` | 零调用、零测试；定义文件 160+ 行缩到 109 行 | 缓存键从未接线，删除不改变持久化格式 |
| 6 | `PlaybackRunEvidence::{startedAtMicroseconds, stoppedAtMicroseconds}` | 只写不读（含测试与诊断输出）；`closeActiveRun` 的入参随之删除 | 去掉两个死字段 |
| 7 | `ComparisonModeDescriptor::supportsRoi` | 读写均为 0，指定初始化器从未设置 | 契约结构去掉死字段 |
| 8 | `SessionSnapshot::isConsistent` 中 `if (playbackRangeLoop && !playbackRangeIn.has_value())` | 位于 `playbackRangeIn.has_value()` 分支内，条件恒假 | 去掉不可达分支 |
| 9 | `ReviewShellController::cancelPendingAction` | 声明+定义外零引用 | 删除死 Q_INVOKABLE |
| 10 | `TimingDiagnosticsController::renderToAckMaxMs`（属性/getter/字段/两处写入） | 零 QML、零测试、零 C++ 读取；同阶段 p95 保留 | 删除只写不读的诊断属性 |
| 11 | `IssueLogController::documentPath` 全链（属性/getter/字段/setter/两处写入） | getter 零调用；字段只写不读；`setDefaultDocumentPath` 仅测试 SetUp 调用 | 删除死属性链与测试里的无效设置 |
| 12 | 三个死 context property：`reviewController`、`reviewPreferences`、`reviewSession` | QML 只读 `reviewFacade`；三个名字在 qml/js 中零读取 | 组合根少三个全局注入 |
| 13 | QML 死属性：`ImageEditDialogs.bodyWidth`、`ReviewInputDialogs.comparisonVisible`、`VcsMenuBar.menuBarBorderColor`、`VcsTheme.strongFocus/successFillHover` | 零读零写；无变更信号处理器 | 删除无效绑定 |
| 14 | 过时测试：`DISABLED_RangeLoopNeverPresentsOutsideInclusiveRange`、`DISABLED_ScrubLatestWinsWithoutGenerationStorm` | 前者直接改假 snapshot 的 `displayedFrame`，绕过协调器；真实保证由 `PlaybackCoordinatorTests` 的区间内核用例覆盖（backlog 09-29 已判“已过时、应删除”）。后者把“未合并的 20 条 seek”写成期望值，且 `seekFrame(0)` 是合法 no-op，第一轮即失败；合并能力仍未实现，留在 backlog | 删除两个永远无法正确失败的基线探针 |

## 3. 修复的缺陷

| # | 位置 | 问题 | 修法 | 验证 |
|---|---|---|---|---|
| 1 | `application/src/PlaybackEvidence.cpp` | 跳帧计数用高水位 `max(last, frame)`；循环播放 90→40 后，40→45 的真实缺口永远不计数 | 每次发布都成为下一次基线；回卷只重置基线，不把回卷本身算作跳帧 | 新增 `CountsPublishGapsAfterALoopRewind`（90,91,40,45 → 4），改前会得 0 |
| 2 | `ui_qml/src/DesktopApplication.cpp` | `splash_` 在 `window_->show()` **之后**赋值；`sceneGraphInitialized` 在 show 期间由渲染线程以 DirectConnection 触发，读到 nullptr，且与 GUI 线程写形成数据竞争 | 在 `show()` 前移交 splash | 全量启动/关闭 smoke 用例 |
| 3 | `app/Main.cpp` | `InterpolationProtocolRepository` 在 `DesktopApplication` 之后构造，析构先于窗口；QML 控制器持有其裸指针 | 把仓库声明移到窗口之前，保证依赖比使用者活得久 | 编译 + 启动 smoke |
| 4 | `shell_windows/ExplorerCommand.cpp` | `GetTitle`/`GetToolTip` 为 `noexcept` 却会分配内存；`bad_alloc` 会终止 Explorer 宿主进程 | 按同文件 `GetIcon` 的方式 try/catch，先置空输出再 `SHStrDupW` | 编译；注册表 smoke 用例 |
| 5 | `platform_windows/src/D3d11ComparisonRenderer.cpp` | `releaseResources()` 不清 `pendingAcknowledgement_`/`highestAcknowledgementSerial_`/`acknowledgementClosed_`；换设备后旧 ACK 可能重放，或永久返回 Closed | 在释放设备代际时一并清空三者 | 编译；渲染/WARP 用例 |

## 4. 已确认但未修（需要接口或状态机改动）

1. **渲染失败被报成解码失败**：`PlaybackCoordinator.cpp:1381/3942/4406` 对所有
   `RenderPublishResult::Closed` 统一用 `kMediaDecodeFailed` + `MediaOperation::kMediaDecode`；
   而 `D3d11RenderChannel.cpp:22-28` 把 `InvalidSet/StaleContext/DeviceUnavailable/Closed`
   折叠成同一个 `Closed`。后果：GPU/设备问题在界面上显示为“解码失败”，且
   `handleGraphicsReady` 只清图形错误码，图形恢复后这条假解码错误仍留在快照里。
   正确修法需要让 `RenderPublishResult` 携带失败原因并逐条映射，属于接口变更。
2. **设备换代后 `GpuTransferActor` 无法恢复**：`GpuTransferActor.cpp:994` 在代际变化时置
   `accepting=false`，全文件没有恢复为 `true` 的路径；`submit` 在 `:1143` 直接返回 Closed。
   broker 支持新代际，但同一 actor 之后不再接单。需要产品决策：重建 actor 还是提供显式
   revive 路径，并补设备丢失/恢复用例。
3. 低概率异常安全：`FileAssociationRegistration.cpp:349-380` 在注册表写入成功后才
   `undos_.push_back`，`bad_alloc` 会破坏回滚；`WindowsPaths.cpp:71-82` 在构造
   `filesystem::path` 抛异常时泄漏 `CoTaskMem`。两者都需要故障注入才能验证，未在本轮改。

## 5. 明确保留（不作为死代码删除）

| 符号 | 理由 |
|---|---|
| `ReviewShellController::removeActiveSource(int)` | 零 QML 读取，但两个测试用它钉住“排队上限/取消”契约；identity API 会合并重复意图，迁移测试会削弱该覆盖 |
| `openIntent`/`pendingAction` 属性 | 无 QML 读者，但 3 处测试以它们观测意图暂存；删除只是把断言换成间接断言 |
| `openComparisonSet` 的 `Q_INVOKABLE` | 仅测试调用；去掉注解不改变行为，收益不足 |
| `createCpuNv12/createCpuP010`、`tryConsumeNotification`、`isClosed`、`FrameSet::isComplete`、`duringForwardPlayback`、`acceptedSequenceAlignments`、`timelineMasterSourceId`、`makeFrameLumaSignature` | 测试缝/测试专用观测点或便捷重载；生产零调用但删除需重写覆盖，且没有对应缺陷证据。列为下一轮“测试缝收口”候选 |
| `LabelGeometry` 整模块 | 2026-10-11 新交付的 P12 能力，尚未接线；是新功能欠账，不是遗留 |
| `InterpolationProtocolRepository::encodeDocument` | 生产零调用，但它是测试读取线格式的契约入口，与 `decodeDocument` 成对 |
| Explorer `setExplorerCommandEnabled`/`removeExplorerCommandRegistration` | 仅测试调用，但与文档化的 `-Uninstall`/用户选择语义对应；应用内还没有等价入口，先保留 |
| `tools/branding/*.py`、`tools/testing/analyze-*.ps1` 等 | 一次性生成器/历史证据脚本，被归档计划引用或产物已入库；保留可复现性，不按死代码删 |

## 6. 测试面变更

- 删除 2 个禁用测试（见 §2 #14）。
- `DISABLED_TimelineAccessibleValueMatchesPreviewOrCurrentFrame` 改写为启用的
  `TimelineExposesAccessibleSliderRoleAndName`：Qt 6.11.1 的 `Accessible` 附加类型没有
  `value` 属性（赋值会编译失败），因此改为断言真实存在的 role/name/Tab 焦点；帧位置要到
  读屏层需要自定义 `QAccessibleInterface`，继续留在 backlog。
- `DISABLED_WipeHandleIsKeyboardAdjustable`：改用 `QQmlProperty(..., qmlContext(...))` 读附加
  属性（`QObject::property()` 读不到），role 断言从错误的魔数 8 改为
  `QAccessible::Slider`（实际值 51）。功能仍未实现，保持禁用。
- `IssueLogControllerTests` 的 SetUp 不再调用已删除的 `setDefaultDocumentPath`。

## 7. 文档

`docs/agent-guide.md`：删除对已不存在的 `TimelineBar.qml`/`ComparisonToolbar.qml` 的指路；
`StillImageDecoder.cpp` 路径改到 `src/media_ffmpeg/src`；`ReviewSessionFacade` 描述改为当前
三 capability 组合。

## 8. 验证

| 项目 | 结果 |
|---|---|
| `cmake --build --preset dev` | 通过 |
| `format-check` | 通过（432 个 C++ 文件 + QML） |
| `qmllint --max-warnings 0` | 全部 QML 通过 |
| clang-tidy（改动且属于 lint 集的 11 个源文件） | 通过；`platform_windows` 按 `cmake/Quality.cmake:99-102` 的既有策略不在 lint 集 |
| 定向用例 | persistence/media 24、application/platform 20、几何 31、PlaybackEvidence 16、QML 契约/时间线 5、控制器 101，全部通过 |
| 全量 `ctest --preset dev -j 8` | 1449 项执行：1447 通过；2 项超时（`quality.build-scripts` 50.8 s 撞 60 s、`app.ui-single-source-smoke` 4.3 s 撞 40 s，均为 8 路并发下的启动/脚本争用）。两项单独 `-j 1` 重跑均通过。另有 3 项环境 skip 与 1 项 disabled（`WipeHandleIsKeyboardAdjustable`，功能未实现）。详见 `out/verification/audit-2026-10-18/full-ctest.log` 与 `timeout-rerun.log` |

复现脚本与原始日志：`out/verification/audit-2026-10-18/`（API 普查、测试清单、逐轮构建/测试日志、
clang-tidy 日志）。
