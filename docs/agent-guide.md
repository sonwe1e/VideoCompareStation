# Agent 快速定位指南

更新日期：2026-09-22。用途：先定位产品问题，再进入最小相关代码和验证入口。
这不是完整架构说明，也不是发布验收记录。

## 开始工作前的五分钟

1. 阅读 [产品目标](product/visual-review.md)，确认四条工作流、透明度优先级和静音范围。
2. 阅读 [问题台账](engineering/visual-review-backlog.md)，按问题 ID 查证据、状态和验收条件。
3. 执行以下只读命令，区分已提交代码和工作区中的新实现：

   ```powershell
   git status --short
   git log -1 --format='%H %s'
   git diff --stat
   git diff -- src/ui_qml/qml/PlayerOsc.qml
   ```

4. 沿下表读取实际调用链；先确认组件被主界面实例化，不能只搜索到一个属性或枚举就判定功能可用。
5. 修改前选择与问题相关的验收场景；完成后更新台账中的状态、代码依据和验证证据。

2026-09-22 的审查基线为 `main @ 0a74c466f3bef7aa8becf77463efaa424de15c55`。
当次工作区另有播放状态、图片 Alpha/PNM 等未提交实现，台账分别记录，不能当作 main 已合入。
后续工作必须重新检查自己的 SHA 和 diff；已有未提交修改不应被覆盖或重复实现。

## 从用户现象找代码

下列路径相对于仓库根目录；优先用文件名和符号搜索，历史文档中的行号可能已经失效。

| 用户现象／任务 | 首要入口与继续追踪 | 验证入口 |
|---|---|---|
| 卡顿、倍速不准、播放变慢、跳帧统计 | `src/application/src/PlaybackCoordinator.cpp`：`playbackTargetAt`、呈现 ACK 提交；`SessionSnapshot.h` → `ReviewController.cpp` → `Main.qml` → `PlayerOsc.qml` | `tests/unit/application/PlaybackCoordinatorTests.cpp`；`tests/component/ui/qml/tst_player_osc.qml`；真实硬件门禁 |
| 点时间轴、快速逐帧、倒退、区间循环 | `ReviewActions.qml`、`TimelineTracks.qml` → `PlaybackCoordinator.cpp` → `src/media_ffmpeg/src/SourceDecodeActor.cpp`、`SoftwareDecoder.cpp` | `PlaybackCoordinatorTests.cpp`、`SourceDecodeActorTests.cpp`；[反向 GOP ADR](adr/0003-reverse-gop-window.md) |
| 悬停未播放位置无缩略图 | `TimelineThumbnailCache.qml`：`capture`、`urlForFrame`；`TimelineThumbnailPopup.qml` | `tests/component/ui/qml/tst_timeline_thumbnail_cache.qml`；台账 `V-06` |
| GT／候选切换后比较错对象 | `src/domain/src/ComparisonSelection.cpp`、`ComparisonValidator.cpp`；`PlaybackCoordinator.cpp`；`CompareModeBar.qml` | `ComparisonSelectionTests.cpp`、`PlaybackCoordinatorTests.cpp`；[比较语义 ADR](adr/0004-timeline-pair-continuity.md) |
| 不同帧率或 VFR 看起来不同步 | `src/media_ffmpeg/src/MultiSourceFrameProvider.cpp`；`src/application/src/Alignment.cpp`、`ComparisonExactness.cpp`；`ComparisonViewport.qml` | `ComparisonExactnessTests.cpp`、`AlignmentTests.cpp`、`MultiSourceFrameProviderTests.cpp`；[对齐说明](alignment.md) |
| 滚轮缩放、拖动、框选、分割线不对 | `ComparisonViewport.qml` → `ComparisonSurface.cpp` → `src/platform_windows/src/D3d11ComparisonRenderer.cpp` | `tests/component/ui/ComparisonSurfaceTests.cpp`、`MainQmlContractTests.cpp`、`qml/tst_wipe_handle.qml` |
| 热力图、高亮、阈值或颜色结果不可信 | `TabbedInspector.qml`、`src/presentation_contract/include/dvs/presentation/ComparisonContract.h`；`D3d11ComparisonRenderer.cpp`；`SoftwareDecoder.cpp` | `tests/unit/presentation_contract/ComparisonContractTests.cpp`；平台像素回读测试；台账 `V-04`、`V-05` |
| 视频格式打不开／软解回退 | `src/media_ffmpeg/src/MediaProbe.cpp` → `SoftwareDecoder.cpp`；`vcpkg.json` | `tests/component/media/MediaProbeTests.cpp`、`SoftwareDecoderTests.cpp`；[支持范围](media-support.md) |
| 图片打不开、PNM、位深或颜色不对 | `src/app/Main.cpp` 注入 loader → `src/media_ffmpeg/src/StillImageDecoder.cpp`；`ImagePairLoader.cpp`、`ImageHeaderProbe.h`、`ImageFolderPairModel.cpp`；`Main.qml` 文件入口 | `tests/component/ui/ImageReviewControllerTests.cpp`、`ImageFolderPairModelTests.cpp`；工作区新增的 `StillImageDecoderTests.cpp`，先确认已纳入构建 |
| 透明边缘、RGBA 数值、Alpha 差异 | `ImageWorkspace.qml` → `ImageReviewController.cpp`：`samplePixel`、`displayImage` → `ImagePairLoader.cpp`：`computeDifference` | `tests/component/ui/ImageReviewControllerTests.cpp`；台账 `I-01`、`I-02`、`I-06` |
| 图片 100%、手动 A/B 闪烁、分割线 | `ImageWorkspace.qml`、`ImageReviewController.h/.cpp`、`ImageFolderPairModel.cpp` | `MainQmlContractTests.cpp`、`ImageReviewControllerTests.cpp`；台账 `I-04`、`C-01`、`C-02` |
| MAE／PSNR、统计和误差图含义 | `src/domain/src/PixelDifference.cpp`；`src/application/include/dvs/application/ComparisonMetrics.h`；图片另查 `ImagePairLoader.cpp` | `PixelDifferenceTests.cpp`、`ComparisonMetricsTests.cpp`；[指标 ADR](adr/0005-pixel-difference-metrics.md) |
| 记录问题、截图、恢复观察位置 | `src/ui_qml/src/IssueLogController.cpp` → `src/application/include/dvs/application/IssueRecord.h` → `src/persistence_json/src/IssueRecordRepository.cpp` | `IssueLogControllerTests.cpp`、`IssueRecordTests.cpp`、`IssueRecordRepositoryTests.cpp` |
| 不知道功能在哪里、模式太多 | `EmptyReviewView.qml`、`ApplicationMenuBar.qml`、`CompareModeBar.qml`、`TabbedInspector.qml`、`ImageWorkspace.qml` | 对应 QML 测试与 `MainQmlContractTests.cpp`；按产品工作流做人工验收 |

表中无前缀的 UI 文件位于 `src/ui_qml/qml`、`src/ui_qml/src` 或 `src/ui_qml/include/dvs/ui`；
应用头文件位于 `src/application/include/dvs/application`。用 `rg --files src tests` 查找同名文件。

## 当前接线与易误判点

- 当前视频主界面由 `Main.qml` 实例化 `PlayerOsc` 和 `CompareModeBar`；不要只改旧
  `TimelineBar.qml` 或 `ComparisonToolbar.qml` 就认为用户能看到修复。
- 图片实际 loader 在 `src/app/Main.cpp` 注入，主要实现位于 `src/media_ffmpeg`。
  `src/ui_qml/src/StillImageDecoder.cpp` 的 stb 格式开关不代表应用最终支持矩阵。
- 视频已有三源、联动缩放、平移、ROI、高亮与热力图；图片仍须单独核对其模式枚举和 UI。
- `ComparisonCoordinator` 是旧架构图中的概念名；当前实现类仍是 `PlaybackCoordinator`。
- `ReviewSessionFacade` 中多个 capability 仍转发同一个控制器，不能把接口预留当作拆分已完成。
- 时间线主源、GT 参考源、当前比较对是不同角色。改变 GT 不应改变时间轴，A/B/C 槽位不应充当跨会话文件身份。
- 渲染了差异图，不等于已经接通 MAE/PSNR；有解码器，不等于有格式验收或无损显示路径。
- 源重复帧、播放器跳过完整帧组、呈现间隙、DF 时间码分别解释。无检测结果应显示未知，不应当作零缺陷。

## 验证与证据

仓库脚本要求 PowerShell 7。工具路径、缓存重建和脚本回归见 [构建指南](building.md)。
先用 `pwsh tools/build/build.ps1 -Preset dev -Doctor` 检查环境；不要猜测工具安装目录。
同一构建目录的 build/lint/test 必须串行执行。下列是后续实现任务的建议命令，不表示本次文档更新执行过这些测试：

```powershell
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex 'application.PlaybackCoordinatorTests'
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex 'ui.(ImageReviewControllerTests|ImageFolderPairModelTests|MainQmlContractTests|player_osc)'
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex 'media.(MediaProbeTests|SoftwareDecoderTests|StillImageDecoderTests)'
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex '(domain.PixelDifference|application.ComparisonMetrics|application.ComparisonExactness|presentation.ComparisonContract)'
pwsh tools/build/build.ps1 -Preset dev -Target format-check
pwsh tools/build/build.ps1 -Preset dev -Target lint
```

测试过滤器须核对当前 CTest 注册名与实际执行数量；零项匹配不是通过。
`-TestOnly` 只适合确认二进制与源码一致时使用。wrapper 支持 `-Target`，不支持旧示例中的
`-FormatCheck -Lint` 参数。

真实播放性能遵循 [runner 与发布门禁](self-hosted-runner.md) 和根目录 [AGENTS.md](../AGENTS.md)：
完整帧组、呈现 ACK、冷 seek、UI 响应与缓存上限都不能放宽。
WARP/单元测试不能证明实际 D3D11VA 流畅度；已有一次测试通过不能转移到另一个 SHA 或新 diff。
性能记录应包含提交、工作区差异、素材、GPU、显示器刷新率、倍率、播放模式和排除区间。

## 文档使用与维护

- 用户当前需求决定范围；[产品目标](product/visual-review.md) 区分明确需求、推荐设计和待确定参数。
- [问题台账](engineering/visual-review-backlog.md) 是当前问题定位入口，源码和针对当前版本的实测是行为证据。
- [依赖规则](architecture/dependency-rules.md)、[状态所有权](architecture/state-ownership.md)、
  [ADR](adr/) 和现行测试门禁约束实现；保持单一协调循环、异步身份和完整 FrameSet。
- `plans/playback-overhaul` 与 `engineering/behavior-baseline.md` 记录较早 SHA 的调查和迁移目标。
  它们的 Reference 耦合、无反向窗口、QML Range Loop 等描述须重新核对；音频扩展不在当前产品范围。
- 每个问题保留稳定 ID。合入后补提交、验证命令和结果，再将状态改为已验证；不能仅凭新增测试代码关闭问题。
- 避免另建与台账平行的缺陷列表；新功能同时更新产品范围、台账状态和有关的媒体支持说明。
