# 视觉审查问题台账

更新：2026-09-25。需求见 [产品目标](../product/visual-review.md)，代码路由与命令见
[Agent 快速定位指南](../agent-guide.md)。此页是当前任务入口，不是发布完成清单。

## 2026-09-25 图片通道视图与淡化切换修复（P0，基线 `85be854` + 本轮工作区）

对应 2026-09-25 审查 §二：两处已确认缺陷的修复与回归验证。**正常 RGBA 视图「偏蓝」的
最终根因仍未确认**，本轮不把它与通道读取错误合并——`RgbaView` 不经过 `channelView()`
（直接返回原图），Alpha 灰度视图碰巧正确；深色底 `#090d14` 对半透明区域的合成影响仍是
待验证假设，等待用户提供问题原图与含通道模式/背景设置的截图后再排查。

- **通道派生视图 R/B 读反（误导颜色判断，I-02）**：解码链路统一输出
  `Format_RGBA8888`，而 `ImageReviewController::channelView()` 把每行直接
  `reinterpret_cast<const QRgb*>` 后使用 `qRed/qGreen/qBlue/qAlpha`。`QRgb` 是
  `0xAARRGGBB` 整数，小端目标上这样读会把 R/B 对调、Alpha 恰好对齐——「忽略 Alpha
  查看 RGB」红蓝颠倒而 Alpha 灰度看起来正常。`ImagePairLoader::analyzeDifference()`
  早已注明该陷阱并改为显式 RGBA8888 字节读取；本次把 `channelView()` 统一到同一
  口径（按格式归一后按 R,G,B,A 字节取值，RGBA8888 缓冲归一为零拷贝浅拷贝）。
  回归测试 `ChannelViewsReadRgba8888BuffersInTrueChannelOrder` 注入 RGBA8888 缓冲
  （生产格式），断言暖色/冷色像素在两侧派生视图中的真实通道值；既有 ARGB32 注入
  用例继续通过（此前正是 ARGB32 注入让该缺陷对测试套件不可见）。
- **淡化（Fade）切换被控制器拒绝（C-02）**：枚举定义 `Fade = 7`，但
  `setCompareMode()` 以 `AlphaDifference = 6` 为上限，进入模式 7 直接返回——
  2026-09-22 一轮把 Fade 记为可用并不准确，入口随后被隐藏。本轮上限放宽到 `Fade`
  并恢复工具栏入口（`visible: hasPair`）；`retainedCompareModeFor` 本就把 Fade 当
  纯显示模式保留，换对后观察模式不丢。按「点按钮后真的发生了什么」补契约验证：
  `ImageWorkspaceManualFlickerContract` 现在点击 `imageModeFade` 后断言
  `compareMode === 7`、`fadeOverlay`/`imageFadeSlider` 可见，再切回手动闪烁。
  控制器测试 `CompareModeAcceptsFadeAndKeepsItViewOnly` 同时覆盖无配对拒绝、
  有配对进入、纯显示（不触发差异管线）与 fadePosition 语义。
- **测试**：`pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex
  'ui.(ImageReviewControllerTests|MainQmlContractTests)'` —— 选择 75 项：
  `ui.ImageReviewControllerTests` 47/47 通过（含 2 项新用例），
  `ui.MainQmlContractTests` 24 通过 + 4 项既有禁用，零失败；格式与 lint 门禁通过
  （out/pr-channel-fade-tests.log、out/pr-channel-fade-format.log、
  out/pr-channel-fade-lint.log）。实窗与 Release 验收仍按既有要求单独执行。

## 2026-09-25 读数与显示口径第二批（P1，基线 `68212f7` + 本轮工作区）

按审查 §2.4（P1 部分）与 §2.5 修改图片差异链路与口径标注。用户已确认实际素材范围为
视频 ≤2K、基本不存在 16-bit 图片，因此本轮**没有**做 8K 专项调优，也**没有**扩展编码格式。

- **差异增益不再是写死的 ×4**：`ImagePairLoader::DifferenceOptions.gain`（1–16，默认 4）贯通
  控制器 `diffGain` 与图片工具栏「差异放大」菜单。增益只作用于渲染出的差异图，
  峰值／RGB MAE／alpha 统计始终是原始 8-bit 差值；`diffScopeText` 与读数行显示当前倍率。
  越界值收敛到 1/16，不会静默关闭放大。
- **每对素材只分析一次**：`analyzeDifference()` 生成 canonical 差值场（逐通道精确幅值
  RGBA8888 + 1 字节符号位图）并同时算出全部统计；`renderDifference()` 只读该差值场生成
  绝对／带符号／高亮／Alpha 变体。切模式、改增益、重采样开关变化不再重读两侧素材、不再重算
  统计。顺带移除了原先两次整图 `Format_ARGB32` 转换副本（解码缓冲本来就是 RGBA8888，
  只有格式不同才转换）。分析缓存按缓冲身份（`QImage::cacheKey`）+ 重采样判定失效，
  提交新配对、`clearCache()`、`closeAll()` 都会释放，绝不按文件名复用。
- **内存按整个工作集核算**：新增 `estimateSingleImageWorkingSet`／`estimatePairWorkingSet`
  （显示缓冲 + RGBA64 sidecar + 差值场 + 符号位图 + 派生差异图 + 重采样副本），
  `kImagePairWorkingSetBudgetBytes` 默认 1536 MiB、可用 `setWorkingSetBudgetBytes` 配置；
  超限时差异任务直接报错并给出估算值。`asyncStats()` 暴露分项
  （`working_set_*`、`analysis_runs`、`analysis_reuses`、`render_runs`）。
  旧的 `宽×高×4` 检查仍只保证单张解码缓冲，不再被当作配对峰值。
- **高位深差异统计（P1 部分）**：两侧都有同尺寸 RGBA64 sidecar 且未重采样时，
  `DifferenceResult` 额外给出 16-bit 峰值／均值／alpha／差异像素数，以及
  `nativeBeyondDisplayPixels`（RGBA8 相同、转换后 16-bit 不同的像素数）。读数行与
  `diffScopeText` 标注“转换后 RGBA16 码值”，并提示“显示缓冲看不出该差异”。
  重采样后不再声称像素级 16-bit 对应。
- **观看路径／数值审查路径口径**：视频检查器的像素维度与图片来源摘要都改为显式两分——
  原始码值 = 数值审查路径；经过显示空间转换 = 观看路径且非原码值比较。
  ICC 仍未实现，属于未覆盖能力，本轮不作任何颜色管理声明。
- **本机证据（7680×4320 合成 RGBA8888 配对，dev 构建，一次性基准后已移除该用例）**：
  首次差异（分析 + 首帧渲染）3073 ms；随后切增益 1352 ms、切模式 1385 ms，
  `analysis_runs=1`、`analysis_reuses=2`，工作集估算 538 MiB。这是本机 dev 观测，
  不是 Release 承诺；按实际 ≤2K 素材折算约首次 ~190 ms、每次显示变体 ~85 ms。
- **测试**：新增 `DifferenceGainAmplifiesDisplayButNotStatistics`、
  `HighDepthStatisticsDetectDifferenceTheDisplayCannotShow`、
  `PairWorkingSetEstimateCountsSidecarsAnalysisAndDerivedImage`、
  `DifferenceRefusesPairBeyondWorkingSetBudget`；`ui.ImageReviewControllerTests` 45/45 通过。

## 2026-09-25 结果可信第一批（基线 `cd79a2a` + 本轮工作区）

三项 P0 修复：指标会话复用、阈值/通道策略统一、连续逐帧停顿。全量 dev 测试、
格式与 lint 见本轮验证记录；硬件门禁证据见下。

- **指标会话复用缺陷（误导比较结论，最高优先）**：`PairMetricsService::prepareSessions`
  原来只在会话为空时构造解码会话；素材不匹配时调用旧会话的 `open()`，而 `open()` 重放
  **构造时**保存的旧 descriptor，于是 A/B → A/C 切换后第二槽实际解码 B 的文件、结果却按
  A/C 发布。现在不匹配的槽位整体重建（新 descriptor 构造新会话），`matches()` 复用判定
  加入完整文件身份（byteSize/mtime/SHA-256 指纹）与 source id。回归测试
  `SwitchingComparisonPairRebindsDecodeSessions` 用三段同几何素材连续切换
  A/B → A/C → B/C → A/B，末次测量必须与首次完全一致；判别前提（两对指标不同）在测试内
  运行时断言，不依赖对 fixture 内容的先验知识。
- **高亮与坏点统计的策略语义统一**：界面阈值策略（亮度／任一通道／全部通道）此前只传给
  GPU 高亮，`computeRgbAbsoluteMetrics()` 固定按任一通道统计。现在
  `domain::MismatchPolicy`（值序与 `presentation::ThresholdPolicy` 一致）贯通
  `PairMetricsRequest/Batch` → 服务 → `PairMetricsController`：同一策略同时驱动渲染过滤与
  坏点判定，CPU 判定按 8 位码值逐条镜像 GPU 着色器（亮度=BT.709 加权、AllChannels=min、
  AnyChannel=max，`>=` 阈值判过；阈值 0 不把相同像素计入）。策略变化清缓存重采样、回包
  策略不匹配即丢弃；读数行显示“坏点占比（任一通道 ≥ 阈值 N）”明确口径。
- **连续逐帧五秒超时（根因定位并修复）**：先用新增 trace 事件（`ProviderSubmitted`
  kind 2 启用发射；新增 `SourceDecodeStarted/Completed` kind 23/24，schema v1 上限 22→24，
  gate 与测试同步）在 10 秒三源组合负载上复现并定位：中点 seek（9900）后第一步命中预取
  缓存，其 read-ahead 在**主解码器**上 `decodeSequential(9902)`，而主解码器游标停在播放
  位置（约 606）——“顺序”解码从 606 逐帧走到 9902（约 9300 帧 ≈ 5.2 s，与 5 s 呈现期限
  吻合，三路同时阻塞、被超时 interrupt 同时释放，排队的主请求随后 1 ms 完成）。
  seek/预取走专用 exact 解码器，主解码器无人追赶。修复：`decodeSequential` 增加步长上限
  （默认 16 帧，`kDefaultMaximumSequentialStrideFrames`，可注入供测试），超限改走有界
  seek。回归测试 `SequentialDecodeBeyondTheStrideSeeksInsteadOfWalking`。
- **门禁残留竞态（修复步进停顿后暴露）**：harness 计数在最后一步提交与 ~33 ms 节流的
  状态投影通知之间存在竞态，299/300 误报（trace 证明 300/300 全部提交）。
  `kHeldStepFinalGraceMs = 250` 有界宽限等待最后帧被观察到。
- **本机证据（dev 构建、144 Hz、D3D11VA 三路 1080p60、`--review-load` 组合负载、15 秒）**：
  修复前 `held_step` 69 提交/1 呈现、五秒呈现超时门禁失败；修复后 **300/300 呈现、
  p95 63 ms、p99 71 ms、最大显示间隔 67 ms、零丢帧、seek P95 213 ms、UI 间隔 P95 51 ms，
  门禁通过（exit 0）**。trace 证据：`out/local-review-performance/{new-kinds,stride-fix,final}-smoke-trace.jsonl`
  与分析脚本同目录。这是本机 dev 证据，不能替代 runner 上的发布门禁（300 秒、Release），
  但失败签名与已记录的 2026-09-24 五分钟门禁完全一致，根因链条完整。
- 既有 `held_step_*` 指标为何漏报：被打断的解码调用不进入 `decoder_maximum_us`（观测到
  55 ms 上限而实际阻塞 5.2 s），顺序追赶也不产生任何 trace 事件——这正是本轮补
  kind 23/24 的原因。硬件复测时应保留 trace 以便直接分拣呈现链各跳延迟。

## 2026-09-24 本地审查执行（1 / 2 / 3）

基线为 3af4ee7 加既有未提交工作区；本次不提交、不覆盖已有工作，不代表发布验收。

- **V-03 / V-07**：新增应用层 FrameMapping，播放和指标共用时间、序列、人工锚点映射。
  指标通过快照共享 CoordinatorPublication 原有的不可变序列缓存，不再在接受序列对齐后退回
  固定偏移，也不逐帧复制整段映射。新增缺帧、待确认区段及共享发布身份回归。
- **V-05**：精确性原因和视频徽标改为描述尺寸/几何差异，不凭输入属性声称“已重采样”。
  图片原有基于实际 diffResampled 的状态保持不变。
- **V-06**：失败的悬停解码释放对应在途请求，允许再次悬停重试；旧请求失败不释放新请求。
  不增加无限自动重试，保留去抖与会话校验。
- **I-02**：图片状态栏显示转换后 16-bit Alpha 码值和四位小数百分比；仍明确区分显示 RGBA8。
  当前 Alpha 差异统计仍基于显示缓冲，不宣称高位深差异统计或 ICC 支持。
- **I-06**：8K 双图后台通道构建、连续切换、悬停取样、对调和替换回归通过。
  本机 dev 样例冷构建 144–207 ms，100 次悬停单次最大约 22.25 ms；这不是所有大图的性能保证。
- **自动验证**：定向 181 项通过，4 项原有禁用；新增 Alpha 实窗读数测试通过。
  日志 out/local-review-final-tests.log；截图 out/local-review-evidence/high-depth-alpha.png。
- **组合验证入口**：--ui-performance ... --review-load 或性能脚本的 -ReviewLoad，
  开启指标并每 500 ms 提交一次独立预览，报告请求/完成/指标样本计数；原有门槛不放宽。
  三源短测真实 D3D11VA、19 次预览完成且指标有结果，但连续步进阶段发生五秒呈现超时，
  因此该轮门禁失败，不能据此宣布组合体验通过。详细结果见本次后续验证记录。

### 本轮验证明细

- 全量 dev：out/local-review-full-tests.log，682 个条目中 675 通过、3 环境跳过、4 原有禁用。
  跳过为 Game DVR 素材缺失和两个大小写冲突文件系统用例。没有关闭或放宽既有测试。
- 格式、C++ clang-tidy 和零警告 qmllint 通过：out/local-review-final-format.log、
  out/local-review-lint.log、out/local-review-final-qml-lint.log。
- 实际双路 7680×4320 PNG（Alpha 128/129）的可见窗口文件夹流程通过：
  打开 660 ms、差异重算 5644 ms、正确识别 alpha_difference_only，关闭/重新打开成功。
  峰值 working set 1608073216 字节，UI 心跳 P95 2 ms、最大 149 ms；最大值高于 100 ms，
  不将功能通过当作所有交互性能达标。结果 out/local-review-images/visible-stderr.log。
  首次隐藏窗口测试停在 graphicsReady，未开始读取文件夹；该超时不是 8K 解码失败证据。
- 三源 10 秒对照（不加 review load）也发生连续逐帧五秒呈现超时，与组合短测一致。
  对照 seek P95 212 ms、组合短测 231 ms；两者 D3D11VA、整组跳帧计数为零且无拆组，
  但完整门禁均失败，不能据此归因于指标/预览，或宣称连续步进已验收。
  对照跟踪 out/local-review-performance/baseline-trace.jsonl，结果位于 results/ 目录。
- 三源 1080p60 五分钟组合测试（302 秒含 2 秒预热）已完成，门禁失败：
  全部 D3D11VA；预览请求 570 次、结果 569 次，指标样本 9 个；seek P95 216 ms。
  连续步进提交 87 帧、呈现 1 帧后发生同样的五秒超时，后续分析阶段未执行。
  UI 心跳 P95 63 ms、P99 95 ms、最大 1072 ms；呈现间隔最大 1095 ms。
  虽整组跳帧计数为零、未观察到拆组，仍存在长停顿，不能用零计数证明播放流畅。
  peak_frame_bytes 为 74649600，进程峰值 working set 为 669712384 字节；正常退出用时 272 ms。
  完整结果：out/local-review-performance/results/combined-5min-stderr.log。
  连续步进的根因已于 2026-09-25 定位并修复（顺序解码从陈旧游标全量追赶），见顶部本轮记录；
  该五分钟门禁失败在本机 15 秒复现中同一签名，修复后通过。
- 新组合入口是验证负载，不是新用户工作流；--review-load 开启指标并周期请求预览，
  原有基线行为和门槛保持不变。所有运行均为 dev，而非 Release 包；素材是既有 75 秒
  frame-id 测试视频无损流复制重复到 330 秒。源码/素材哈希在 manifest.json 中。

## 基线、状态与阅读方法

- 本轮起点：`HEAD @ 7299eda`。以下 2026-09-22 的实现已纳入提交，
  发布和硬件验收状态仍分别核对。
- 2026-09-24 当前工作区四项核心产品交互优化（审查后修订，勿按“全部完成”验收）：
  1. 视频对比对齐与精确性原因：新增按帧号 / 按时间 / 人工锚点模式；Timestamp 用源
     `frameAtOrBefore`/VFR timeline 映射，缺时间基标 Missing；Provider 已放行
     `TimeAligned`/`ExactIndex`；检查器时间块字段已接线。指标窗口已按 Timestamp/ManualAnchor 逐帧映射（`mappedSourceFrames`）。
  2. 视频播放控制与预览：控制条提供「流畅观看 / 逐帧检查」（写入 preferences）；悬停近似图
     仅在一格采样距内借用，`isExact` 仅当图=悬停帧；暂停相邻帧步进条 [-3]~[+3]。
     未播放帧已有独立解码悬停预览（`PreviewThumbnailService`，时间线主源）；
     无缓存时回退时间码胶囊。多源对比条带预览仍待做。
  3. 图片框选放大与显示说明：Shift+拖动框选并 `zoomToRect` 同步 A/B；框选后不再误切 A/B；
     状态栏为「显示缓冲 RGBA8」+ 重采样徽章，明确未应用 ICC；高位深读数标注为转换后 16-bit。
  4. 共同交互：对齐/阈值条件在视口 chrome 常显；图片三键齐全。视频侧已补「适应窗口/重置视图」；
     Shift+拖动与图片一致为框选放大，ROI 改为 Alt+拖动。重采样已移出主工具栏，
     改为状态徽章条件开关并持续显示。
- 2026-09-23 前期工作区：修正播放计数说明；图片 RGB 均值统一为三通道 MAE；
  视频时间精确性同时核对源时间戳；首屏增加双图入口；手动闪烁拖动阈值和窄窗状态栏已调整。
  新增 U-02 图片对调 A/B 与单侧替换。对应定向测试通过；硬件和 Release 验收仍待做。
- 2026-09-22 第二轮实现：V-07 视频指标全链路（`PairMetrics.h` 端口、
  `PairMetricsService`/`PairMetricsDecodeSession` 独立解码服务、`PairMetricsController` GUI 投影、
  检查器读数 + OSC 摘要 + 时间轴指标泳道）；I-02 高位深 sidecar 取样；I-03 对话框补 `*.pam`；
  I-06 `channelView` 跨线程缓存加互斥；图片 Fade 模式；视频视口 1:1 像素比例徽章；
  图片差异统计语义标注。单测/组件测试（`PairMetricsServiceTests`、`PairMetricsControllerTests`、
  `ImageReviewControllerTests` 含 16-bit 用例）已通过；硬件验收未执行。
- 用户已拍板（2026-09-22）：图片不做三图对比（C-01 关闭，理由"图片不做3图"）；
  AV1/VP9（V-04）暂不需要，降为延后池；视频对比不做 Fade/Flicker（原计划批次 1c 取消）。
  后续必须重新运行 `git status`、`git diff`，不能据此认定已合入。
- **已在基线**：有实现及调用路径；**工作区实现**：有未提交代码，不表示通过验收；
  **待实现**：缺少所需能力；**待验证**：行为、语义或性能尚需实验。
- **P0** 优先消除误导比较结论的风险；**P1** 完成核心工作流；**P2** 后续分析能力。
  优先级是建议次序，不要求先进行全面架构重写。

## 优先定位表

| ID | 用户问题 | 优先级 | 当次状态 | 先检查 |
|---|---|---|---|---|
| V-01 | 是素材卡顿，还是播放器没有跟上？ | P0 | 已接线并修正计数解释；真实负载下统计含义待验证 | `Main.qml` → `PlayerOsc.qml`；`RenderAckRelay.cpp` |
| V-02 | 倍速／过载时为何变慢或突然追赶？ | P0 | 两种策略已有；连续逐帧五秒超时根因已修复（2026-09-25，陈旧游标全量追赶），runner 发布门禁待复测 | `PlaybackCoordinator::playbackTargetAt`、`SoftwareDecoder::decodeSequential` |
| V-03 | 30/60 fps、VFR 比较是否同一时刻？ | P0 | 精确性增加源 PTS 一致条件并显示双源帧号/时间；完整时间映射待完善 | `MultiSourceFrameProvider.cpp`、`ComparisonExactness.cpp` |
| V-04 | 常见编码为什么打不开？ | P1 | H.264/HEVC/MPEG-4 Part 2 已有；AV1/VP9 待扩展 | `MediaProbe.cpp`、`vcpkg.json` |
| V-05 | 显示转换会不会改变细节？ | P0 | 转换及部分精确性标记已有；检查器与图片摘要改为显式区分“观看路径／数值审查路径”（2026-09-25）；原始保真路径待扩展 | `SoftwareDecoder.cpp`、`ComparisonViewport.qml`、`ImageWorkspace.qml` |
| V-06 | 未播放位置没有缩略图 | P1 | 未缓存悬停降级为时间码胶囊与准星线，Jog Wheel 可滚轮微调；合约测试已通过 | `TimelineThumbnailPopup.qml`、`TimelineTracks.qml` |
| V-07 | MAE/PSNR 能否实际用于视频评估？ | P2 | 独立解码服务、检查器读数、OSC 和时间轴指标泳道已接通；切换比较对的会话复用缺陷与阈值/通道策略口径已修复（2026-09-25）；硬件验收待做 | `PairMetrics.*`、`PairMetricsController`、`MetricTimelineLane.qml` |
| I-01 | 透明度哪里错了，贴背景后怎样？ | P1 | A/B/O 快捷键、高对比背景与观察状态浮标已实现；QML 合约测试通过 | `ImageWorkspace.qml`、`ImageReviewController` |
| I-02 | 读数是原始高位深值吗？颜色可信吗？ | P0 | 已加 RGBA64 sidecar 原始取样（16-bit 用例通过）；差异统计新增“转换后 RGBA16”口径与“显示缓冲看不到的差异”提示（2026-09-25）；ICC 仍无 | `StillImageDecoder.cpp`：`convertFrameToRgba`、`ImageReviewController::samplePixel/nativeStatsText` |
| I-03 | PNM 是否所有入口都能打开？ | P1 | 三个对话框已补 `*.pam` 过滤器；格式矩阵验收仍待做 | `ImageHeaderProbe.h`、`Main.qml` |
| I-04 | 点击 100% 后仍是放大状态 | P1 | 100% 重置 zoom=1.0，适应窗口与双击重置；合约测试已通过 | `ImageWorkspace.qml`：`imageTrueSizeButton` |
| I-05 | 图片“平均差异”和视频 MAE 是否同义？ | P0 | 图片 RGB 均值已统一为三通道 MAE，峰值仍为最大通道差；差异改为每对素材一次分析、增益与模式只改渲染（2026-09-25，增益 1–16 可调，统计不随增益变化） | `ImagePairLoader::analyzeDifference`、`renderDifference` |
| I-06 | 切换大图通道会不会卡 UI？ | P1 | 悬停取样已改为单像素计算，不再等待整图派生缓存；切模式／改增益只重渲染差值场，不再重读素材；用户确认实际素材 ≤2K，未做 8K 专项调优 | `ImageReviewController::channelView`、`samplePixel`、`diffGain` |
| C-01 | GT＋两个 Prediction 如何比较？ | P1 | 用户 2026-09-22 拍板：图片不做三图对比，条目关闭 | `ImageReviewController.h`、`CompareModeBar.qml` |
| C-02 | 如何找插帧形变、重影、时间跳变？ | P1/P2 | 手动 A/B 切换已加拖动阈值；淡化隐藏；真实素材实窗验收待做 | `ImageWorkspace.qml`：手动切换/同步准星 |
| U-01 | 不知道从哪里开始、功能藏在哪里 | P1 | 首屏入口、Diff 直达重采样与沉浸控制已接线；图片数量和混合拖入的误导已修正；实窗验收待做 | `EmptyReviewView.qml`、`Main.qml`、`PlayerOsc.qml` |
| U-02 | 反复选文件：对调 A/B、只换一张 | P1 | 对调与单侧替换已接线；控制器测试通过；实窗验收待做 | `ImageReviewController::swapSides/requestReplace*`、`ImageWorkspace.qml`、`Main.qml` |
| A-01 | 改动状态容易漏接或重复拥有 | 随功能推进 | 分层已有；capability 实现仍集中 | `ReviewSessionFacade`、状态所有权说明 |
| E-01 | 构建反复出错、工具路径失效、重新链接后启动崩溃 | P0 | 已修复，本机开发测试及质量门禁验证完成 | `tools/build/build.ps1`、`env.ps1`、`cmake/CheckMsvcDependencies.cmake` |

下文路径相对仓库根目录；无目录的 UI 文件按 [路由表](../agent-guide.md) 查找。
问题 ID 保持稳定，后续关闭或拆分时保留原 ID 和后继链接。

## 视频：证据与退出条件

### V-01 播放状态与统计含义

- **证据**：基线状态展示在未被当前主界面实例化的 `TimelineBar.qml`。
  已通过 `SessionSnapshot` → `ReviewController` → `Main.qml` → `PlayerOsc` 接入目标／实际倍率、落后量、追赶与跳过组数。
- **剩余问题**：`displayGapCount` 来自 `ComparisonSurface.droppedFrames` → `ReviewRuntime` →
  `RenderAckRelay::canonicalFrameGaps`，计算的是呈现 ACK 的 canonical 帧号间隙，可能与追赶跳过重合，
  不是物理屏幕刷新丢失测量。`sourceDuplicateCount` 来自数量受限的对齐时间线标记，
  不是完整源内容重复帧总数。UI 的解释必须符合数据来源；三项不能直接相加。
- **退出条件**：明确计数来源、范围与重置条件；未分析显示未知；过载、seek、倍速、暂停、循环和换源后无误导值。
  若报告真实刷新／呈现丢失，应另有对应测量依据。
- **验证入口**：`PlaybackCoordinatorTests`、`tst_player_osc.qml`、`MainQmlContractTests.cpp`。
  组件文案测试只能证明显示，需验证真实 Main 接线和统计含义。

### V-02 实时观看与完整逐帧审查

- **证据**：`ReviewEveryFrame` 保留完整 FrameSet，允许时间进度落后；`RealTime` 使用
  `kPlaybackCatchUpTolerance = 2000ms`，超过容忍窗口才追赶；`Contextual` 当前解析为 RealTime。
- **边界**：2 秒是现有策略，不是已经实测证明的性能缺陷；不能只改常数就声称播放流畅。
  4×、多路、高分辨率和每帧必呈现受硬件限制；屏幕刷新率也限制原速观看的帧覆盖。
- **退出条件**：确定性时钟下覆盖阈值前后卡顿、CFR/VFR、0.25/1/4× 和循环边界；
  状态准确表达实际倍率与落后。执行现有真实 D3D11VA 门禁，完整帧组与 Out 边界不回退。
- **验证入口**：`PlaybackCoordinatorTests` 的 catch-up、ReviewEveryFrame、RangeLoop 用例；
  [runner 文档](../self-hosted-runner.md)。

### V-03 帧序号与时间对应

- **证据**：`MultiSourceFrameProvider` 默认 `mappedFrame = canonicalFrame + offset`；
  `comparisonExactnessDimensions` 的 `temporalExact` 现在要求双方 `ExactIndex` 且源 PTS 相同。
  比较浮标可悬停查看两侧实际帧号与源 PTS；这不会自动建立跨帧率映射。
  当前已有全局偏移、序列分析、人工锚点，GT 也已与时间线主源分离。
- **方向**：明确“按序号”“按时间”“人工／生成帧映射”的规则。保留严格索引审查，
  不擅自对齐或补帧掩盖模型缺陷；未知时间关系不能标成同一时刻精确。
- **退出条件**：30 fps 输入＋60 fps GT/Prediction、VFR/CFR、非零起始时间、偏移和缺帧样例，
  能核对每源实际帧号／时间／映射依据；改变 GT 不改变时间轴或区间。
- **验证入口**：`ComparisonExactnessTests.cpp`、`AlignmentTests.cpp`、`MultiSourceFrameProviderTests.cpp`。

### V-04 编码支持矩阵

- **证据**：`src/media_ffmpeg/src/MediaProbe.cpp` 仍只允许 H.264、HEVC、MPEG-4 Part 2；PQ/HLG 拒绝。
  具体像素格式／色彩边界见 [媒体支持](../media-support.md)。
- **方向**：优先评估 AV1/VP9；ProRes/DNxHR 与 HDR 按真实素材分期，不能仅扩大扩展名列表。
- **退出条件**：每种新增格式覆盖单／双／三路、定位、前后逐帧、倍速、软解回退、损坏输入、发布包解码器可用性。
  分开记录“可打开”“可正确比较”“性能已达标”。
- **验证入口**：`MediaProbeTests.cpp`、`SoftwareDecoderTests.cpp`、媒体 fixtures 与性能门禁。

### V-05 原始像素与显示空间

- **证据**：`SoftwareDecoder.cpp` 将 RGB、4:2:2、4:4:4 转到 NV12/P010 的 4:2:0 路径；
  `ComparisonExactness.cpp` 中的 `preservesNormalizedPlaneCodes` 已限制哪些输入可称原码值。
  工作区新增说明和测试不等于新增原始保真解码／显示能力。
- **方向**：区分最终外观比较与原始数据检查；记录色度、位深、颜色范围和空间重采样。
- **退出条件**：RGB/422/444 细线及色度 pattern、8/10 位、full/limited 样例能证明标签和结果一致；
  转换后数值不冒充原始文件码值。新路径需像素回读及相应硬件证据。
- **验证入口**：`SoftwareDecoderTests.cpp`、`ComparisonExactnessTests.cpp`、`ComparisonSurfaceTests.cpp`。

### V-06 独立缩略图与定位预览

- **证据**：`TimelineThumbnailCache.qml::capture` 对已经显示的画布 `grabToImage`，`urlForFrame` 对未缓存位置返回空。原界面在未缓存帧上悬停时完全不显示浮动提示。
- **已实现**：
  1. 重构 `TimelineThumbnailPopup.qml` 实现双模态呈现：有缓存时显示完整缩略图与时间码，未缓存时优雅降级为紧凑型时间码胶囊（Compact Timecode Pill），无论何时悬停均能显示精准时间码、帧号及标记点标签（入点/出点/人工锚点等），消除时间轴悬停盲区；
  2. `TimelineTracks.qml` 新增悬停垂直准星参考线（`timelineHoverGuide`）与底纹微刻度（Tick Marks）；
  3. 支持时间轴滚轮逐帧微调（Jog Wheel，滚轮向上前进 1 帧、向下后退 1 帧，Shift 步进 5 帧）。
- **验证入口**：`tst_player_osc.qml`、`tst_timeline_tracks.qml`、`tst_timeline_thumbnail_cache.qml` 以及 `MainQmlContractTests.cpp` 中新增的 `TimelineUncachedHoverShowsTimecodePillAndGuide` 用例已通过验证。

### V-07 视频量化指标

- **证据**：`computeRgbAbsoluteMetrics`、`scoreActivePairRgbAbsolute` 有 MAE/MSE/PSNR 基础，
  但没有完整视频像素获取、SessionSnapshot／UI 或区间统计链，见 [ADR 0005](../adr/0005-pixel-difference-metrics.md)。
- **2026-09-22 已提交实现**：
  1. 新增 `application::IPairMetricsService` 端口（`PairMetrics.h`）：请求携带 `PlaybackRequestContext`
     身份、双源、对齐偏移、帧区间与坏点阈值；结果以带身份的批次异步发布。
  2. `media::PairMetricsService` + `PairMetricsDecodeSession`：独立于播放管线的双源软解会话
     （不占 FrameBudget/渲染缓存），中心向外采样顺序、分批发布、最新请求优先并协作取消旧作业、
     会话跨作业复用；RGBA 转换与显示路径同矩阵约定。
  3. `ui::PairMetricsController`：GUI 线程投影，线程安全接收器 + 过期批次按
     （会话/纪元/源对/对齐版本/阈值）校验丢弃；暂停时窗口 ±150 帧预取（泳道开启时），
     播放时单帧节流采样；提供 `samplePoints` 桶化曲线、`peakFrames` 局部极大查询。
  4. UI：检查器"差异"页当前帧指标块（MAE/MSE/PSNR/最大绝对差/坏点占比与数量/参与像素，
     指标名固定 `cpu-rgb-absolute-v1`，阈值与 GPU 高亮共享）；OSC 紧凑读数；
     `MetricTimelineLane.qml` 可收起泳道（MAE 曲线 + 峰值标记点击跳转 + 播放头指示）。
- **退出条件**：同图无限 PSNR、缺帧／错误尺寸 unavailable；不可用项不当作零误差平均；
  换 pair／seek 后陈旧结果被丢弃；显示增益不影响原统计值。组件测试已覆盖以上语义；
  真实素材下的数值对拍与性能（1080p60 窗口采样耗时）待硬件验收。
- **验证入口**：`PairMetricsServiceTests.cpp`（8 用例：同源零误差、跨编码可比、越界/尺寸不匹配
  不可比、非法请求拒绝、新请求打断、取消静默、会话复用）、`PairMetricsControllerTests.cpp`
  （7 用例：无对不可用、暂停单帧/泳道窗口请求、批次读数、纪元变更丢陈旧、阈值重建缓存、
  峰值与桶化查询）、既有 `PixelDifferenceTests.cpp`、`ComparisonMetricsTests.cpp`。

## 图片：证据与退出条件

### I-01 Alpha 工作流

- **证据**：早期基线只有 RGBA 取样、RGB 差异和 Alpha-only 提示；后续已加入 Alpha 灰度、
  忽略透明度 RGB、Alpha 差异、峰值／均值／变化像素和黑白／棋盘背景。
- **已实现**：
  1. 交互增强：`A` 与 `O` 分别切换 Alpha 灰度和忽略透明度 RGB；背景由下拉菜单直接选择深色、棋盘格、黑底或白底；按 2026-09-23 反馈移除循环背景按钮和 `B` 循环键。保留“α 直通（未预乘）”徽标；
  2. 棋盘格对比度升级：将画布背景 Canvas 替换为专业中性灰阶双色网格（`#22262e` / `#383e4a`），大幅提升半透明边界与镂空细节可辨识度；
  3. 观察态状态浮标（`imageAlphaObservationBadge`）：在激活非默认观察通道或背景时，于视口顶部实时浮现状态与快捷还原提示，支持点击一键复位；
  4. 快捷键帮助覆盖层：`ShortcutHelpOverlay` 深度整合图片工作区预设，展示图像与透明度检查全套快捷键。
- **验证入口**：`ImageReviewControllerTests.cpp` 的 Alpha stats 与 channel view 用例；
  `StillImageDecoderTests.cpp` 的实际含 Alpha 文件解码用例；`MainQmlContractTests.cpp` 新增
  `ImageWorkspaceAlphaWorkflowAndBackgroundShortcutsContract` 全流程契约测试已通过验证。

### I-02 来源位深、原始取样和颜色

- **证据**：`StillImageDecoder.cpp` 中的 `convertFrameToRgba` 仍输出 `AV_PIX_FMT_RGBA`；工作区新增来源格式、
  位深、通道和转换标签，没有保留可取样的原始 16 位数据，也未建立完整 ICC 链。
- **2026-09-22 已提交实现**：
  1. `StillImage` 新增 `rgba16` sidecar：源位深 >8 时同步转换出 RGBA64LE 原始码值缓冲；
  2. 加载链路（`ImagePairLoader` 加载器签名 + `Result`/缓存条目、`Main.cpp` 组合根、
     同步与异步两条打开路径）全程携带 sidecar，缓存字节核算包含 sidecar；
  3. `ImageReviewController::samplePixel` 在 RGBA 视图对 A/B 原图输出
     `nativeBitDepth/r16/g16/b16/a16`，`ImageWorkspace` 读数显示"原始 %1-bit：R… G… B…"；
  4. 直接注入路径（`openPairImages` 等）显式清空 sidecar，杜绝陈旧原始值。
- **2026-09-25 补充**：`channelView()` 派生视图曾按 `QRgb` 误读 RGBA8888 缓冲导致 R/B
  对调（「忽略 Alpha 查看 RGB」红蓝颠倒、Alpha 灰度碰巧正确），已改为按真实字节序读取
  并有 RGBA8888 注入回归用例；正常 RGBA 视图的「偏蓝」与此无关（`RgbaView` 直接返回
  原图），根因仍待用户样例排查（见顶部记录）。
- **退出条件**：16 位输入显示正确来源信息，取样值明确属于 RGBA8；需要原始数据时保留对应缓冲和码值
  （现已满足：>8-bit 源同时给出显示值与原始码值）；ICC 样本只有在确定转换契约并验证后才声称颜色正确（仍待实现）。
- **验证入口**：`StillImageDecoderTests.cpp`、`ImageReviewControllerTests.cpp`
  `HighBitDepthSourceReportsNativeSampleValues`（16-bit 码值精确断言）。

### I-03 PNM 与图片入口一致性

- **证据**：工作区新增 P1–P7 识别、头尺寸探测、文件夹及 PNM/PPM/PGM/PBM 对话框入口。
  当次检查 `.pam` 已进入解码／文件夹，但三个图片对话框过滤器仍缺该扩展名。
- **2026-09-22 已提交实现**：三个图片对话框过滤器已补 `*.pam`。
- **退出条件**：逐项验证 PBM/PGM/PPM 的文本／二进制、PAM、8/16 位、注释、损坏和超大尺寸输入；
  单图／双图／拖放／文件夹／发布包一致。“PPNM”暂按 PNM，真实样例到来后修订范围。
- **验证入口**：`StillImageDecoderTests.cpp`（工作区新增并已登记 CMake）、
  `ImageFolderPairModelTests.cpp`。扩展名识别通过不等于解码通过。

### I-04 100% 与适应窗口

- **证据**：原 `ImageWorkspace.qml::imageTrueSizeButton` 仅切换 `trueSize`，已有 zoom 仍乘到基础比例上。
- **已实现**：`imageTrueSizeButton` 将 `zoom` 同步重置为 1.0（实现 1 图像像素 = 1 物理像素）；“适应窗口”与“重置视图”按钮恢复完整画面；支持双击在 100% 真实尺寸与适应窗口间快速往返切换；支持鼠标中键无缝拖拽平移。
- **验证入口**：`MainQmlContractTests.cpp` 中 `ImageWorkspaceZoomResetAndTrueSizeContract` 用例已通过验证。

### I-05 图片统计定义

- **现状**：`ImagePairLoader::analyzeDifference` 的 `meanAbsDifference` 是 RGB 三通道全部样本的
  平均绝对差，与视频 MAE 同义；峰值仍取单像素最大通道差。增益是显示参数（`diffGain`，
  1–16，默认 4），只放大渲染出的差异图，统计始终是原始 8-bit 差值。
- **2026-09-25**：分析（差值场 + 全部统计）每对素材只做一次并缓存，`renderDifference` 按
  模式／增益派生显示变体；`nativeStatsText` 另外给出“转换后 RGBA16”口径与
  “RGBA8 相同、16-bit 不同”的像素数。重采样后不提供 16-bit 像素级统计。
- **退出条件**：RGB delta=(3,6,9) 的单像素案例峰值为 9、MAE 为 6；展示标签分别标明。
  Alpha 独立统计，不混入 RGB MAE。不同尺寸默认拒绝逐像素差异；允许重采样时记录方向与方法。
- **验证入口**：`ImageReviewControllerTests.cpp`（`DifferenceGainAmplifiesDisplayButNotStatistics`、
  `HighDepthStatisticsDetectDifferenceTheDisplayCannotShow`）、`PixelDifferenceTests.cpp`，
  及指标展示／导出契约。

### I-06 大图通道切换延迟

- **证据**：`ImageReviewController::channelView` 在每次换图或切换观察模式后首次生成派生通道时，
  同步分配并遍历整图；同一模式后续可使用派生缓存。悬停 `samplePixel` 已改为直接从原图
  派生单像素读数，不再进入整图缓存与其互斥锁。
- **2026-09-22 已提交实现**：当时确认 QML 图片提供器线程与 GUI 悬停取样会并发进入
  `channelView` 的可变缓存（原实现无锁，存在数据竞争）。已为缓存与失效计数加互斥
  （`viewCacheMutex_`）：先到线程承担唯一一次构建，显示路径本就在提供器线程预热，
  悬停现在不读派生缓存。
- **2026-09-25**：差异链路改为每对素材一次分析、切模式／改增益只重渲染差值场，也不再产生
  两次整图 `Format_ARGB32` 转换副本。工作集改为按显示缓冲 + sidecar + 差值场 + 派生图整体
  核算（`estimatePairWorkingSet`、`working_set_*` 统计）。用户确认实际素材 ≤2K，
  8K 未做专项调优；一次性 7680×4320 dev 基准记录在顶部第二批。
- **待验证**：缓存与复用降低重复开销，但不能证明首次大图操作满足 UI 延迟要求。
- **退出条件**：在允许尺寸／内存范围的大图上测首次切换、连续切换、悬停取样、换图取消；
  满足既有 100 ms UI 响应门禁。需要后台化时保留 generation/request 校验和有界缓存。

## 工作流与结构

### C-01 GT＋两个 Prediction

- **已有**：视频三联、参考聚焦、分析网格和任意两源比较；图片仍是 primary/secondary 与双文件夹模型。
- **状态（2026-09-22）**：用户拍板“图片不做 3 图”，本条目关闭。图片对比维持双图模型；
  视频侧三源模型（ThreeUp/ReferenceFocus/DifferenceEdge）独立保留。

### C-02 插帧审查与切换模式

- **已有**：视频局部放大、平移、ROI、区间循环、高亮及问题记录，不应重新当作缺失能力实现。
- **2026-09-23 调整**：
  1. 图片单图对比改为手动闪烁：默认 A，点击画面、按 `Space` 或 `T` 在 A/B 间切换；不自动计时交替。顶部 HUD 显示当前源并限制宽度；
  2. 差异模式收为显示当前选项的下拉菜单；淡化入口隐藏。原淡化按钮无效的直接原因是控制器拒绝模式值 `7`（2026-09-25 已修复并恢复入口，点击链路经契约测试验证，见顶部记录）；
  3. 并排模式跨图同步十字准星（Hover 时镜像侧精准投影目标瞄准环、辅助十字线与图像像素坐标，彻底消除并排观察微小伪影时的视线寻找负担）；
- **验证入口**：`MainQmlContractTests.cpp` 的 `ImageWorkspaceManualFlickerContract`、`ImageWorkspaceAlphaAndBackgroundSelectionContract` 与并排准星用例；`tst_image_workspace_manual.qml` 用 Qt Quick 鼠标点击画布两次，验证 A→B→A。
- **本轮验证**：`dev` 全套 648 项可运行测试中，除版本切换导致的发布契约元数据缺项外均通过；
  补齐 `1.7.0` 发布契约后该项定向重测通过。`format-check`、`lint` 与版本/EXE 校验通过。
  3 项环境跳过、4 项原有禁用；真实素材实窗、Release 包及硬件性能未验收。

### U-01 功能可发现性

- **证据**：原空白页仅有打开视频按钮；图片常用对比和文件夹对比入口藏匿较深；差异图在分辨率不一致时不可用但缺乏直接操作引导；全屏或纯净模式（Tab/H）下播放控制条完全消失且无法唤醒。
- **已实现**：
  1. 空白视图（`EmptyReviewView.qml`）新增首屏“打开图片…”与“对比文件夹…”直达卡片按钮，打通图片首屏链路；
  2. 差异图分辨率不一致的禁用提示条中内嵌一键“启用重采样并对比”快捷操作按钮；
  3. 文件夹侧边栏增加首项与末项一键跳转导航函数；
  4. 全屏与纯净模式（`!chromeVisible`）下新增屏幕底边感应唤醒条（`immersiveWakeStrip`），鼠标移至底端平滑滑出悬浮式 `PlayerOsc` 播放控制条，支持沉浸状态下直接拖拽进度与调速，鼠标移开 1.5 秒后自动平滑淡出。
- **2026-09-23 图片入口修正**：打开图片对要求恰好两张；三张及更多图片、图片与视频混合拖入直接提示，不再只打开前两张或错误进入视频流程。被拒绝的选择不改变当前画布。等待解码时显示进度状态和取消入口。尺寸不同时的重采样开关显示状态，并解释差异计算已缩放 B。
- **本轮验证**：`ui.ReviewControllerTests` 与 `ui.MainQmlContractTests` 共 70 项通过、4 项既有禁用；`format-check`、全量 `lint` 与最终 QML lint 通过。加载提示仍需在真实大图上检查视觉效果。
- **验证入口**：`MainQmlContractTests.cpp` 中 `EmptyReviewViewExposesImageAndFolderEntryPoints` 与 `ImmersiveModeBottomEdgeWakesOverlayOsc` 用例已通过验证。

### U-02 图片槽位：对调 A/B 与只换一张

- **证据**：打开图片对后必须整对重选才能换方向或换一侧；`requestOpenPrimary` 会清空 B，
  不能表达「只换 A」。审查 E35 也指出缺少指定槽位替换。
- **已实现**：
  1. `ImageReviewController::swapSides()`：交换 A/B（缓冲、路径、来源信息、identity），
     不改 `committedPairId`、zoom/pan；带符号差异/分割线/淡化方向随新 A/B 角色更新。
  2. `requestReplacePrimary` / `requestReplaceSecondary`：只替换命名侧；另一侧、
     `committedPairId` 与观察位置保留；失败保留原侧（沿用 T1 单侧语义，不是新的原子配对提交）。
     `requestOpenPrimary` 仍是「打开新的单图」并清空 B。
  3. UI：`ImageWorkspace` 工具栏「对调 A/B」「换图…」与打开菜单项；`Main.qml`
     替换 A/B 对话框与 `completeImageOpen` 的 replace 分支（不拆文件夹会话、不覆盖工作区）。
- **边界**：对调是会话内方向，不改写文件夹配对身份。单侧替换后画布可能与文件夹行路径不一致，
  这是有意保留的 T1 语义；文件夹导航仍按行配对。
- **本轮验证**：`ui.ImageReviewControllerTests` 39/39（含 6 项新用例）、
  `ui.MainQmlContractTests` 23/23 通过；`format-check`、`lint` 通过。实窗验收待做。
- **验证入口**：`ImageReviewControllerTests` 的 `SwapSides*`、`Replace*`、`FailedSideReplace*`。

### A-01 状态所有权与增量维护

- 保留分层、单一协调循环、完整 FrameSet、呈现 ACK 和异步身份。
  `ReviewSessionFacade` 的 capability 仍有共同后端；接口名不代表已完成物理拆分。
- 随功能移动状态到明确所有者，优先消除重复投影、遗漏接线和图片／视频指标定义分歧。
  不以新需求全面替换播放内核，也不要求简单 UI 修复先完成整个架构迁移。
- 验收遵循 [依赖规则](../architecture/dependency-rules.md)、[状态所有权](../architecture/state-ownership.md)
  和现有测试，核心层无 Qt/FFmpeg/D3D11 类型泄漏。

### E-01 构建工具与增量依赖可靠性

- **基线**：2026-09-22，`0a74c466f3bef7aa8becf77463efaa424de15c55` 加当前未提交工作区。
  此项验证构建基础设施与现有功能回归，不自动关闭上面的产品验收条目。
- **根因与修复**：旧 MSVC `/showIncludes` 依赖识别异常，使部分对象丢失头文件依赖；
  类布局变化后新旧对象混用，主界面测试出现 14 个启动崩溃。前轮已统一诊断语言、重新配置并完整重编；
  本轮补查生产控制器与 UI 测试对象的实际 Ninja 记录，避免只检查 domain 后误判整体正常。
  `-Fresh` 先重配再清理生成物；`-Doctor` 检查工具和缓存；构建保留 stdout/stderr，失败后不继续测试。
  工具发现、PowerShell 7、同 preset 互斥和零测试失败要求见 [构建指南](../building.md)。
- **开发回归**：`pwsh tools/build/build.ps1 -Preset dev -Test` 构建成功，选择 628 项，
  621 项通过、3 项跳过、4 项原有禁用、零失败；耗时 122.39 秒。
  其中主界面 15 项启用测试全部通过，覆盖此前 14 个崩溃用例；日志为
  `out/build-repair-verification.log`。
- **跳过范围**：一个 Game DVR 测试缺专用素材；两个文件名大小写冲突测试受当前文件系统限制。
  RangeLoop、Scrub、Wipe 键盘和 Timeline accessible 的四项 Main 测试在 HEAD 已禁用，本轮未更改。
- **新增门禁回归**：`quality.msvc_dependency_contracts` 的 5 个场景验证仅 core、健康 UI、
  空 UI 依赖、缺关键头文件、无依赖记录；使用独立真实 Ninja 数据库和带空格路径。
  生产对象检查接入构建包装器、format-check、lint 与 CTest。
- **质量验证**：`-Doctor`、`-Target format-check`、仓库指引和资源限制检查通过。
  最终执行 `pwsh tools/build/build.ps1 -Preset dev -Target lint -Test -TestRegex '^quality[.]'`，
  clang-tidy、零警告门限 qmllint 和 5 项 CTest 质量检查全部通过，日志为
  `out/build-repair-lint-final.log`。其中构建脚本检查包含 15 个场景，依赖门禁回归包含上述 5 个场景。
- **边界**：本次不代表 release 打包、真实 D3D11VA 性能、80% 覆盖率或禁用用例已验收。
  保留已有测试和门禁要求，不以本机开发回归替代发布证据。

## 实施批次与更新规则

| 建议批次 | 条目 | 结束条件 |
|---|---|---|
| 1：可信观察 | V-01/02/03/05、I-02/04/05 | 状态和数值不会误导素材质量判断；保留现有正确性门禁 |
| 2：基础工作流 | I-01/03/06、V-04、U-01 | 常见素材与透明度流程可用，新增工作区实现有验收证据 |
| 3：多候选与定位 | C-01、C-02 的 P1、V-06 | 同一区域切候选与定位预览稳定，用户能复现问题 |
| 4：评估分析 | V-07、C-02 的 P2 | 指标可追溯、区间统计有效且不影响播放 |

A-01 随对应批次推进；批次是排序建议，可按真实素材与反馈调整。
一次任务只领取相关条目，不因为阅读本页就自动获得发布、合并或扩大产品范围的授权。

关闭条目时记录：**提交／工作区差异、解决行为、测试命令、实际执行数量、结果、性能或截图证据、剩余限制**。
源码出现功能或新增测试不等于验收通过。新发现追加到对应条目；设计变化同步更新产品页与媒体支持范围。

## 已有能力，避免重复修复

- 时间线主源与 Reference 已分离；比较对按会话身份处理，拓扑重建有媒体身份重映射。
- 反向 GOP 窗口已实现；内核已有范围循环与 Out 边界，不能照搬旧文档的 QML-only 缺陷结论。
- 视频已有 1–3 源与局部观察；图片已有异步加载、原子双图提交、取消、差异缓存及文件夹配对。
- 旧 `d04339c` 基线和 playback-overhaul 计划用于追溯，不是当前缺陷状态；
  当前产品不含音频／字幕扩展，不以历史音频时钟方案作为新任务依据。
