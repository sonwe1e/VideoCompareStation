# 视频指标窗口：顺序采样性能证据

日期：2026-10-01。对应台账：[`V-07`](visual-review-backlog.md#v-07-视频量化指标)。
基线为 `4a67d1b` 加已有工作区修改；本轮没有提交、发布或修改既有主界面／打包补丁。
这是“快速区间评测＋异常定位”的第一批：只完成窗口调度、当前帧优先发布和工作量观测。

## 行为变化与边界

- `PairMetricsRequest::priorityFrame` 表达交互请求的实际观察位置，必须位于闭区间内。
  未指定时按整个区间升序处理，不再把区间中点当作隐式播放头。
- 指定优先帧时先评分并立即发布一个样本；随后处理两个向前区间：
  `[priority+1, last]`、`[first, priority-1]`。不创建与帧数成正比的调度顺序数组。
- 单帧请求的第一批也是终批；窗口的优先样本不是终批，其余样本继续按最多 16 个分批发布。
- GUI 选择 `clamp(displayedFrame, requestedFirst, requestedLast)`，所以窗口在文件边缘被裁短、
  或播放头已经缓存而请求邻近缺口时，不会优先计算一个无关中点或区间外帧。
- 原有双源独立软解、完整请求身份、最新请求优先、协作取消、源身份验证和精确时间戳校验保留。
  稠密映射仍按 canonical 帧索引，缺口仍明确不可比较，公式仍为 `cpu-rgb-absolute-v1`。
- `PairMetricsService::workStats()` 是累计观测：`seekCount` 计实际 seek 调用，`decodedFrames`
  计成功的 `avcodec_receive_frame`（含前滚／恢复），`sampledFrames` 计成功返回的单源 RGBA 帧，
  `publishedBatches` 计发出的批次。它不是 GPU／屏幕丢帧统计；工作期间各原子字段可能独立增长。
  测量使用每轮独立服务，终批到达后且无下一请求时读取。

本轮没有实现阈值分布缓存、无界面区间评测 CLI、异常区间导航或 GPU 指标归约。
没有改变播放管线，也没有将分析 worker 合入播放／呈现线程。

## 测量协议

测试入口：`tests/component/media/PairMetricsPerformanceTests.cpp`，独立 target
`dvs_pair_metrics_performance_tests`，CTest 前缀 `metrics-performance.`，标签 `performance`。
常规 `dev`／`release` CTest 排除该标签；缺少显式素材路径时跳过，跳过不构成性能证据。

- Windows 11（10.0.26300），Intel Core i7-13700KF，MSVC Release，CPU 软解及标量评分。
  GPU 不参与本项统计，RTX 4090 的存在不能将本结果认定为 D3D11VA 证据。
- 同日同机顺序执行 A/B，不并行运行同一 build 目录；文件缓存为热缓存。
- 每个版本 1 轮预热不计、5 轮测量；中位数取排序第 3 个，P95 用 nearest-rank（本次即最大值）。
- 两路 H.264、1920×1080、60 fps、301 帧，GOP=60、B 帧=3、yuv420p；窗口 `[0,300]`。
  B 由同样的 testsrc2 加 `eq=brightness=0.02` 生成，避免相同输入的零误差捷径。
- A 的 SHA256：`9D966B2202F34E3CBFB6242CAF808939DA2DA7A8FB449F2DCAF0368B72EBC056`。
  B 的 SHA256：`231F96AA9D34611ADBDAFDAC2FB396F402CFC5BD0679D0E53D58B2BDC9E736FB`。
  生成后用 ffprobe 回读并确认帧数、尺寸和帧率，再将路径交给测试。
- 基线只加同样的工作量计数，保留中心向外交替采样和 16 样本首批；中点为 150。
  优化版本显式指定 priority=150，使用同一素材、范围、阈值 4、AnyChannel 和标量公式。
- 计时从 submit 前开始，涵盖请求排队、打开独立会话、身份核验、时间线准备、解码、RGBA 转换
  和评分；两源最初的 `MediaProbe::inspect` 在计时前完成，不包含在读数中。
- 首批计时在 worker sink 收到样本时结束，**不是 GUI 绘制、输入响应或屏幕呈现延迟**。

## 结果

时间单位 ms；计数为每轮累计，两路合计。

| 指标 | 基线中位 / P95 | 最终候选中位 / P95 |
|---|---:|---:|
| 首批可用时间 | 4356.27 / 4434.68 | 346.80 / 358.29 |
| 完整窗口时间 | 82737.80 / 83519.49 | 7205.01 / 7445.31 |
| 首批样本数 | 16 | 1 |
| seek 调用 | 600 | 4 |
| 实际解码帧 | 18240 | 662 |
| 成功返回的单源 RGBA 帧 | 602 | 602 |
| canonical 样本数 | 301 | 301 |
| 批次数 | 19 | 20 |
| 按 canonical 顺序求和的 MAE | 272.3277943351336 | 272.3277943351336 |

该样例窗口吞吐约 **11.48 倍**、首批约 **12.56 倍**改善；收益主要来自不再逐位置反复 seek
与解码前滚，不是少算帧或修改误差公式。MAE 汇总相等不是逐帧数值等价的全部证明：
组件测试另外逐项比较不同采样顺序下的 `PairMetricsSample`，覆盖 offset 与 dense mapping、
映射缺口及优先帧位于两端／中间的情形；benchmark 也逐轮比较全部排序后的样本。

初轮优化的窗口中位 6780.44 ms、首批中位 330.39 ms；上表使用 mutation 恢复、格式化和
全量质量检查之后重新构建的最终候选（另一次 1+5 轮），不挑选更快的一轮替代终版证据。
两轮优化的 seek、实际解码量、覆盖帧数和 MAE 汇总相同；耗时存在本机环境波动。

这组数据只证明本机这组合成 H.264 素材；不外推 HEVC、VFR、非单调映射、真实用户素材、
UI 响应或正常播放性能。遇到非连续源映射，精确 decoder 仍可能 seek；下一批可再测 GOP
分块、映射复用与分析缓存，但不得牺牲精确帧身份。

## 回归和 mutation

定向测试 28 项通过（15 项 media、13 项 UI）。实现前新增的顺序工作量与优先发布测试在旧
调度上实际失败，记录在 `red.log`，不是仅凭新增断言宣布覆盖。

恢复所有 mutation 的产品源字节后执行全量开发构建／测试：选择 821 项，814 通过、3 跳过、
4 原有禁用、0 失败，231.85 s。跳过仍为 Game DVR 专用素材及两个大小写冲突场景；
禁用仍为四个既有 Main QML 场景，没有为本改动删除、放宽或禁用测试。
`format-check`、完整 `lint`（clang-tidy、零警告 qmllint）和 `git diff --check` 通过。

20 个 mutation 均成功构建并被运行时断言检出（22 个定向 assertion-test 执行）；不是编译失败、
零项匹配或进程超时。脚本断言声明数、检出数、实际测试数和报告回读数，并在 finally 中逐字节
恢复源文件。对应新行为断言的反例：

| 断言类别 | 实际检出的 mutation |
|---|---|
| 升序且一次覆盖全部位置，少 seek／解码 | 旧调度 red；`drop-prefix`、`duplicate-priority` |
| 四项工作量字段反映实际工作 | `seek-counter`、`decode-counter`、`sample-counter`、`batch-counter` |
| 显式优先帧、立即单样本、窗口仍待完成 | `wrong-priority`、`delayed-priority` |
| 排序后数值与 offset 参考一致，dense mapping／缺口不漂移 | `wrong-dense-mapping`、`fill-mapping-gap` |
| 优先批次取消后不解码／发布余下窗口 | `ignore-active-cancel` |
| 单优先帧在首批完成 | `missing-single-final` |
| 拒绝无效帧及区间外优先帧 | `allow-invalid-frame`、`allow-outside-priority` |
| 裁短窗口／缓存缺口仍选实际播放头附近 | `ui-window-midpoint`（3 个 UI 测试） |
| benchmark 检出多算、canonical ID 错误、不可比、跨轮漂移 | `duplicate-priority`、`fake-canonical-id`、`noncomparable-samples`、`metrics-depend-on-request` |
| benchmark 必须真实提交并成功解码，不能失败后仍报告性能 | `reject-valid-submit`、`decoder-failure` |

## 重跑与本机产物

先为两项环境变量提供真实本地文件；测试会取两源共有的前 301 个位置（较短素材取全部）：

```powershell
$env:DVS_METRICS_BENCHMARK_A = (Resolve-Path 'source-a.mp4').Path
$env:DVS_METRICS_BENCHMARK_B = (Resolve-Path 'source-b.mp4').Path
pwsh tools/build/build.ps1 -Preset release -Target dvs_pair_metrics_performance_tests
.\out\build\release\bin\dvs_pair_metrics_performance_tests.exe

pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex '^(media[.]PairMetricsServiceTests|ui[.]PairMetricsControllerTests)[.]'
pwsh tools/build/build.ps1 -Preset dev -Test
pwsh tools/build/build.ps1 -Preset dev -Target format-check
pwsh tools/build/build.ps1 -Preset dev -Target lint
```

本机原始证据均在 `out/verification/pair-metrics-sampling/`：
`baseline-dev.log`、`baseline-performance.log/.xml`、`optimized-performance.log/.xml`、
`final-performance.log/.xml`、`comparison.json`、`red.log`、`green.log`、`mutation-results.json`、各 mutation 的 build/test 日志、
`mutate.ps1`、`full-dev.log`、`full-dev-LastTest.log`、`format-check.log`、`lint.log`。
正式 harness 只在仓库测试目录；out 中的脚本只是本轮 mutation 与证据产物。

**仍待验收**：真实用户素材和现有五分钟 D3D11VA 播放、seek、UI 响应、256 MiB 解码缓存、
覆盖率及 release ZIP 门禁。本轮 CPU benchmark 与开发全量测试不能替代这些门禁。
