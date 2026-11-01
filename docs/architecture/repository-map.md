# 仓库与功能地图

本页帮助开发者和 agent 根据要改的功能找到目录、调用链和测试。它描述当前代码的归属，
不代表功能已完成验收：产品范围看[产品目标](../product/visual-review.md)，问题与待验条件看
[问题台账](../engineering/visual-review-backlog.md)，具体症状和排查路径看
[快速定位指南](../agent-guide.md)。

## 先从哪里开始

1. 在下方「功能 → 代码和测试」查入口；界面上的一个动作通常会跨越多个目录。
2. 从界面调用一路跟到规则或适配器，检查具体调用者与状态所有者，选择实际受影响的测试。
3. 默认行为或产品范围涉及取舍时查[决策登记册](../engineering/decision-register.md)；
   测试命令和验收范围以[构建指南](../building.md#开发循环与完整验收)为准。

## 顶层目录

| 目录 | 放什么／何时修改 |
|---|---|
| [`src/`](../../src/) | 产品代码。按架构层或适配器划分；详见下一节。 |
| [`tests/unit/`](../../tests/unit/) | 领域、应用、表现契约、持久化、启动和 shell 的单元测试，按模块分目录。 |
| [`tests/component/`](../../tests/component/) | 媒体、Windows 平台、UI/QML、Explorer 的组件和接线测试；`ui/qml/` 是 Qt Quick 测试。 |
| [`tests/fixtures/`](../../tests/fixtures/) | 媒体、图片、覆盖率和 CI 路由样本；图片与媒体样本各有说明。 |
| [`tests/support/`](../../tests/support/) | 测试共用助手；`tests/smoke/` 和 `tests/hardware/` 分别覆盖启动/打包契约与硬件场景。 |
| [`docs/`](../) | 产品、现行契约、问题台账、设计决策、构建与发布记录；导航见 [docs/README.md](../README.md)。 |
| [`tools/`](../../tools/) | 构建、质量、测试、素材、发布和 Explorer 脚本，入口见 [tools/README.md](../../tools/README.md)。 |
| [`cmake/`](../../cmake/) | 依赖与架构约束、构建质量检查、资源/打包规则和其契约测试。 |
| [`.github/`](../../.github/) | 托管 CI、硬件性能和发布工作流，以及共用的 Windows 环境准备动作。 |
| [`assets/`](../../assets/) / [`licenses/`](../../licenses/) / [`packaging/`](../../packaging/) | 品牌与图标、许可证、发布资源；运行载荷的规则另见构建指南。 |
| `out/` | 本机构建、缓存及一次性验证产物；不是源代码，也不提交。 |

`.gitignore` 列出了其他本机状态和生成物，例如 `Testing/`、根目录的临时 `.obj`/`.pdb`，以及
agent/IDE 的本地目录；它们不是产品功能目录。

## 生产代码的边界

每个模块的 `CMakeLists.txt` 是该模块构建目标及源文件的事实来源。多数模块的公共头文件在
`include/dvs/<模块>/`，实现和私有头文件在 `src/`；公共头文件不按下面的功能子目录改名。
层次依赖与允许的目标关系由[依赖规则](dependency-rules.md)和 `cmake/Architecture.cmake` 约束。

| 目录 | 职责与查找入口 |
|---|---|
| [`src/domain/`](../../src/domain/) | 不依赖 Qt、FFmpeg 或 Windows 的领域规则：素材描述、时间线、比较对、像素差、插帧协议等。 |
| [`src/application/`](../../src/application/) | 用例、端口与协调：播放会话、完整帧组、对齐、指标、导出计划、观察记录。单一协调循环在 `src/application/src/PlaybackCoordinator.cpp`。 |
| [`src/presentation_contract/`](../../src/presentation_contract/) | 纯 C++ 比较模式、枚举与能力描述；UI 和渲染器共享。 |
| [`src/media_ffmpeg/`](../../src/media_ffmpeg/) | 媒体探测、视频解码/缓存/组帧、图片解码和颜色探测、分析与导出适配器。不要在此决定 UI 交互。 |
| [`src/persistence_json/`](../../src/persistence_json/) | 设置、问题记录、插帧协议的 JSON 读写和事务发布。 |
| [`src/platform_windows/`](../../src/platform_windows/) | Windows 支持和 D3D11 图形实现；`src/support/` 是文件发布、路径、计时与跟踪等，`src/graphics/` 是设备、帧资源、传输、渲染和 ACK。`include/dvs/platform/` 保持统一公共接口；`tools/` 有 HLSL 编译工具，`include/onnxruntime/` 是第三方头文件。 |
| [`src/ui_qml/`](../../src/ui_qml/) | QML 界面、UI 控制器、Qt Quick 与图形桥接。下表进一步列出功能子目录。 |
| [`src/shell_windows/`](../../src/shell_windows/) | 不依赖 Qt/FFmpeg 的 Explorer 命令、注册与文件关联。 |
| [`src/app/`](../../src/app/) | GUI/CLI 入口与组合根：创建具体适配器、接线、启动请求转发和运行时桥接。 |

### UI 源码分区

| `src/ui_qml/` 下的目录 | 职责 |
|---|---|
| `qml/` | 实际屏幕、控件、交互与样式：`Main.qml` 组合，`ImageWorkspace.qml` 管图片工作区，`PlayerOsc.qml` / `TransportBar.qml` / `TimelineTracks.qml` 管视频操作，`ComparisonViewport.qml` 管视频视口，`Vcs*` 与 `VcsTheme.js` 管共用控件/主题。 |
| `src/image/` | 图片查看、配对、任务、颜色、编辑、副本及对应图片 provider。 |
| `src/playback/` | 视频播放状态投影、走带、缩略图与视频任务标签。 |
| `src/comparison/` | 比较画面、图形桥接、呈现 ACK、比较指标控制及画面导出。 |
| `src/files/` | 视频文件夹模型和最近文件的共享辅助代码。图片目录配对在 `src/image/`。 |
| `src/outputs/` | 区间/精确样本导出、问题记录和插帧协议的 UI 控制器。协议仓储与算法仍在各自的持久化/领域层。 |
| `src/workspace/` | 窗口和 QML 注册/接线、会话 facade、源列表、偏好、启动、文件关联与诊断控制。 |
| `include/dvs/ui/` | UI 公共头文件；按类型名查找，不等同于功能子目录。 |
| `qmltypes/` / `shaders/` | QML 工具类型描述 / D3D11 着色器。注册契约见 [ADR 0006](../adr/0006-qml-type-registration.md)。 |

功能目录与构建目标是不同维度：例如 `src/image/` 中的 `ImageFolderPairModel.cpp` 属于仅依赖
Qt Core 的 `dvs_ui_models`，`ImageReviewController.cpp` 属于 `dvs_ui_d3d11_bridge`。
以 `src/ui_qml/CMakeLists.txt` 为准，不要把整个功能目录自动加入同一个目标。
类似地，`dvs_windows_support` 和 `dvs_graphics_d3d11` 是独立目标，`dvs_platform_windows`、
`dvs_ui_qml` 是兼容聚合，不是额外实现。

QML 文件目前保持在同一 `qml/` 目录：组件通过同目录可见性及 `dvs_ui_qml_resources.qrc` 的资源
别名装载。按功能查找使用下表；若以后实际拆分 QML 目录，必须同时处理导入、资源别名、源码
路径测试、格式/lint 和部署扫描，不能只移动文件。

## 功能 → 代码和测试

表中前缀都是相对于仓库根目录的目录：`Q` = `src/ui_qml/qml`，`U` = `src/ui_qml/src`，
`D` = `src/domain/src`，`A` = `src/application/src`，`M` = `src/media_ffmpeg/src`，
`G` = `src/platform_windows/src/graphics`，`P` = `src/persistence_json/src`，
`TU` = `tests/unit`，`TC` = `tests/component`。例如 `U/playback/ReviewController.cpp` 即
`src/ui_qml/src/playback/ReviewController.cpp`。列出的测试是起点，不是自动影响分析或验收状态。

| 要改的功能 | 界面与控制器 | 规则、适配器或接线 | 先看哪些测试 |
|---|---|---|---|
| 打开素材、拖放、文件夹和最近历史 | `Q/ReviewInputDialogs.qml`、`Q/VideoFolderSidebar.qml`、`Q/FolderPairSidebar.qml`；`U/files/VideoFolderModel.cpp`、`U/image/ImageFolderPairModel.cpp`、`U/workspace/ReviewShellController.cpp` | `A/MediaPaths.cpp`；`src/app/Main.cpp`、`U/workspace/DesktopApplication.cpp` 接线 | `TC/ui/VideoFolderModelTests.cpp`、`ImageFolderPairModelTests.cpp`、`MainQmlContractTests.cpp` |
| 图片/视频任务标签、切换与关闭 | `Q/ImageTaskTabs.qml`、`Q/VideoTaskTabs.qml`；`U/image/ImageTaskHost.cpp`、`U/playback/VideoTaskHost.cpp` | `U/workspace/ReviewShellController.cpp`；视频实际会话由 `A/PlaybackCoordinator.cpp` 管理 | `TC/ui/ImageTaskHostTests.cpp`、`VideoTaskHostTests.cpp`、`MainQmlContractTests.cpp` |
| 视频播放、暂停、逐帧、走带、循环 | `Q/ReviewActions.qml`、`Q/PlayerOsc.qml`、`Q/TransportBar.qml`；`U/playback/ReviewController.cpp`、`JogStateMachine.cpp` | `A/PlaybackCoordinator.cpp`、`PlaybackTiming.cpp`；`M/SourceDecodeActor.cpp`、`SoftwareDecoder.cpp` | `TU/application/PlaybackCoordinatorTests.cpp`、`TC/ui/ReviewControllerTests.cpp`、`JogStateMachineTests.cpp`、`TC/media/SourceDecodeActorTests.cpp` |
| 时间轴、悬停缩略图 | `Q/TimelineTracks.qml`、`TimelineThumbnailPopup.qml`、`TimelineThumbnailCache.qml`；`U/playback/PreviewThumbnailController.cpp` | `M/PreviewThumbnailService.cpp`；实际 seek 走 `A/PlaybackCoordinator.cpp` | `TC/ui/qml/tst_timeline_tracks.qml`、`tst_timeline_thumbnail_cache.qml`、`TC/ui/PreviewThumbnailControllerTests.cpp`、`TC/media/PreviewThumbnailServiceTests.cpp` |
| 同步/对齐、缺帧、插帧协议 | `Q/TabbedInspector.qml`、`Q/NearbyFramesCard.qml`；`U/outputs/InterpolationProtocolController.cpp` | `A/Alignment.cpp`、`ComparisonExactness.cpp`；`M/AlignmentAnalysisService.cpp`、`MultiSourceFrameProvider.cpp`；`D/InterpolationProtocol.cpp`、`P/InterpolationProtocolRepository.cpp` | `TU/application/AlignmentTests.cpp`、`ComparisonExactnessTests.cpp`、`TU/domain/InterpolationProtocolTests.cpp`、`TC/media/MultiSourceFrameProviderTests.cpp` |
| 视频比较模式、比较对和参考源 | `Q/CompareModeBar.qml`、`Q/ComparisonViewport.qml`；`U/workspace/ReviewPreferencesController.cpp`、`U/playback/ReviewController.cpp` | `D/ComparisonSelection.cpp`、`ComparisonValidator.cpp`；`src/presentation_contract/include/dvs/presentation/ComparisonContract.h`；`G/D3d11ComparisonRenderer.cpp` | `TU/domain/ComparisonSelectionTests.cpp`、`TU/presentation_contract/ComparisonContractTests.cpp`、`TC/ui/MainQmlContractTests.cpp` |
| 视频画面、缩放、ROI、分割线、呈现确认 | `Q/ComparisonViewport.qml`、`Q/WipeHandle.qml`；`U/comparison/ComparisonSurface.cpp`、`RenderAckRelay.cpp` | `G/D3d11ComparisonRenderer.cpp`、`GpuTransferActor.cpp`、`FrameMailbox.cpp`；`A/PlaybackCoordinator.cpp` | `TC/ui/ComparisonSurfaceTests.cpp`、`RenderAckRelayTests.cpp`、`TC/platform/GpuTransferActorTests.cpp` |
| 差异图、MAE/PSNR、阈值与指标缓存 | `Q/TabbedInspector.qml`、`Q/MetricTimelineLane.qml`；`U/comparison/PairMetricsController.cpp` | `D/PixelDifference.cpp`、`A/ComparisonMetrics.cpp`、`M/PairMetricsService.cpp`；图片差异另看 `U/image/ImagePairLoader.cpp` | `TU/domain/PixelDifferenceTests.cpp`、`TU/application/ComparisonMetricsTests.cpp`、`TC/ui/PairMetricsThresholdReuseTests.cpp`、`ImageReviewControllerTests.cpp` |
| 图片解码、格式、颜色与透明度 | `Q/ImageWorkspace.qml`；`U/image/ImageReviewController.cpp`、`ImagePairLoader.cpp`、`ImageColorContract.cpp` | `M/StillImageDecoder.cpp`、`ImageColorProfileProbe.cpp`；实际格式边界见[媒体支持](../media-support.md) | `TC/media/StillImageDecoderTests.cpp`、`TC/ui/ImageReviewControllerTests.cpp`、`ImageColorContractTests.cpp`、`ImageColorSampleTests.cpp` |
| 图片裁剪、画笔、变换、撤销和副本保存 | `Q/ImageWorkspace.qml`、`Q/ImageEditDialogs.qml`；`U/image/ImageEditController.cpp`、`ImageEditOperations.cpp`、`EditImageProvider.cpp` | `D/LabelGeometry.cpp`；智能分割另看 `U/image/Sam2Segmenter.cpp` 和台账 S03 | `TC/ui/ImageEditControllerTests.cpp`、`ImageEditOperationsTests.cpp`、`MainQmlContractTests.cpp` |
| 问题记录、恢复观察状态 | `Q/IssueNoteDialog.qml`；`U/outputs/IssueLogController.cpp` | `A/IssueRecord.cpp` 与 `src/application/include/dvs/application/IssueRecord.h`；`P/IssueRecordRepository.cpp` | `TC/ui/IssueLogControllerTests.cpp`、`TU/application/IssueRecordTests.cpp`、`TU/persistence_json/IssueRecordRepositoryTests.cpp` |
| 区间流拷贝、精确样本、对比图导出 | `Q/ClipExportDialog.qml`、`Q/SampleExportDialog.qml`；`U/outputs/ClipExportController.cpp`、`SampleExportController.cpp`；`U/comparison/ComparisonExportController.cpp` | `A/ClipExportPlanner.cpp`、`SampleExportPlanner.cpp`；`M/ClipExportWriter.cpp`、`SampleExportWriter.cpp` | `TU/application/ClipExportPlannerTests.cpp`、`SampleExportPlannerTests.cpp`、`TC/media/ClipExportWriterTests.cpp`、`SampleExportWriterTests.cpp`、`TC/ui/MainQmlContractTests.cpp` |
| 设置、菜单、快捷键、主题 | `Q/ApplicationMenuBar.qml`、`ReviewShortcuts.qml`、`ShortcutCatalog.js`、`VcsTheme.js`；`U/workspace/ReviewPreferencesController.cpp` | `P/SettingsRepository.cpp`；窗口组合看 `Q/Main.qml` | `TC/ui/ReviewPreferencesControllerTests.cpp`、`TC/ui/qml/tst_review_actions.qml`、`tst_vcs_theme.qml`、`TU/persistence_json/SettingsRepositoryTests.cpp` |
| 启动、二次启动、Explorer 和文件关联 | `U/workspace/DesktopApplication.cpp`、`StartupSplash.cpp`；`src/app/Main.cpp`、`StartupRequestBroker.cpp` | `src/shell_windows/ExplorerCommandRegistration.cpp`、`FileAssociationRegistration.cpp`；`tools/shell/` | `TU/app/StartupRequestBrokerTests.cpp`、`TU/shell_windows/`、`TC/shell_windows/ExplorerCommandSmokeTests.cpp` |
| 打包、发布、运行库和脚本 | `packaging/`、`cmake/Install.cmake`、`cmake/Packaging.cmake`、`tools/release/` | 运行载荷边界见[构建指南](../building.md#zip-只打包必要运行时) | `tests/smoke/`、构建指南的发布检查；实机门禁另按发布合同执行 |

同一单元格内未重复前缀的文件名位于紧邻的已标明目录；测试选择仍需沿调用链检查。要定位
明确症状（例如「长按走带停不稳」而不是整个播放功能），继续查[快速定位指南](../agent-guide.md)。

## 新增或移动代码时

- 放在拥有规则的最内层；具体 I/O、FFmpeg、Win32、D3D11 和 Qt 细节留在相应适配器。
  若功能跨层，让每层只承担自己的职责，在 `src/app/` 或 `DesktopApplication` 接线。
- 新增 C++ 文件时，除了功能目录，还要将其列入正确模块的 `CMakeLists.txt` 和目标；新增
  QML 文件时检查 `dvs_ui_qml_resources.qrc`、QML 类型注册/描述及对应测试。
- 测试按 `tests/unit/<模块>/`、`tests/component/<模块>/` 找到同类行为；样本放
  `tests/fixtures/`，临时验证放 `out/verification/`。先读调用者，再按影响范围选测试。
- 目录、入口或测试路径改变时同步更新本页、[快速定位指南](../agent-guide.md)及受影响的
  构建/检查脚本；脚本的必需检查锚点必须存在，不能因路径失效静默通过。
