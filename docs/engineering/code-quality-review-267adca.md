# VideoCompareStation 代码质量与技术方向审查（证据复核版）

日期：2026-10-05
范围：`main` @ `267adca6114c912796465944a51468598b8f0762`（**审查结束前已再次核对：HEAD == origin/main == 267adca6…，未漂移**）
仓库：https://github.com/sonwe1e/VideoCompareStation/tree/267adca6114c912796465944a51468598b8f0762

本文件是对一份待复核审查报告的**逐条证据核验**。原报告提出 4 个 P1、4 个 P2 与一组架构判断；本次对每一项都回到该 SHA 的源码、测试、文档与仓库自带素材，并补齐了两项此前缺失的**本地最小复现**。

核验结论：**原报告的优先级排序和「保留现有技术栈 + 单一协调循环」的方向判断成立，可照此执行**；但其中两项的证据等级需要修正（一项上调、一项补充关键机制），两项措辞需要收紧。修正见 §3。

---

## 0. 本次核验的验证边界（先说清做不到什么）

| # | 做了什么 | 没做什么 |
|---|---|---|
| 1 | GitHub 只读读取固定 SHA 的源码、测试、AGENTS、架构/产品文档、发布说明、近期提交 | 未修改远端、未推送、未触发 GitHub Actions |
| 2 | 本地 `ffprobe 7.1.5` 实测仓库自带 fixture 的真实 PTS 原点、时间基与关键帧结构 | 不是项目锁定的 FFmpeg 8.1.2 |
| 3 | **用仓库自身未修改的 `ClipExportPlanner.cpp` / `RationalRate.cpp` / `FrameTimeline.cpp` / `MediaError.cpp` / `MediaDescriptor.cpp` 编译并运行了一个最小探针**（`out/verification/clock-base-probe/`） | 这不是全项目构建或全量测试 |
| 4 | **用本地 FFmpeg 实测了「显示路径转 NV12」与「指标路径转 RGBA」在机制上不等价**（`out/verification/pixel-contract-probe/`） | 不冒充该项目/该版本的像素验收，未做 GPU/D3D 回读 |
| 5 | 逐行核对了 HLSL 阈值控制流、QML 数据结构、导出提交流、Render ACK 语义、构造函数成员次序 | 未运行 Windows GUI、Qt/D3D11 渲染、D3D11VA、多源长播、完整测试套件、真实用户素材 |
| 6 | 核对了 git 工作区：**本次引用的 9 个文件在工作区中均未被修改**，因此结论对 HEAD 与当前工作区同样成立 | 工作区另有未提交的新增工作（启动画面、续播、列表重排等），不属于本 SHA，本审查不评述其对错 |

补充一条对阅读者重要的边界：v2.1.0 的 852 项通过是**维护者明确授权的本次发布例外**（`docs/releases/v2.1.0.md#L38-56` 自述未完成五分钟 D3D11VA 门禁、完整 DPI、shutdown-soak、干净 Windows 与用户 videos 目录验收）。它不能替代未做的硬件长测与真实用户工作流。

---

## 1. 结论（方向判断：成立）

保留 C++20 + Qt Quick + FFmpeg + D3D11，保留单一 `PlaybackCoordinator` 循环、完整多源帧组与异步身份校验。**这一轮不值得换框架或重写，值得把「当前看的是哪帧、画面和数值是否同一口径、导出的是否同一段」做成可靠闭环。**

当前问题不是功能太少：三路视频、联动局部观察、反向逐帧、VFR、图片 Alpha/16-bit 辅助数据、轻编辑、区间导出都已实现。本轮确认的四个 P1 全部落在**模块之间的接线处**——每个模块自身的单测都可以是正确的，而组合后的用户体验仍然错误。这正是不能拿「852 项通过」当作用户工作流验收的原因。

产品范围建议维持：本地、静音、常见手机/游戏录屏与图片质量审查；视频保留 GT + 两个候选；图片维持双图。暂不扩大到音频、字幕、4K/8K 性能工程、完整剪辑器或跨平台移植。

---

## 2. 逐项证据核验结果

### P1-A 导出时间基准：**已本地复现**（证据等级上调：源码推断 → 本地复现器）

这是本次核验中唯一由「读码推断」升级为「跑出来」的问题。

**机制**：区间规划得到的时间以 canonical frame 0 为原点；`keyframeTimes()` 返回的是容器原始 packet PTS。两者直接进入 `alignClipExportStart()`。

- `src/domain/include/dvs/domain/FrameTimeline.h#L12-13`：「Values are **normalized to frame zero** and expressed in microseconds」
- `src/domain/src/RationalRate.cpp#L164-175`：`frameStartTime` 只做 `frameId` → 微秒换算，**没有任何 first-PTS 偏移**
- `src/domain/src/FrameTimeline.cpp#L33-37`：`create()` 甚至**强制要求** `displayTimes.front() == 0`，否则直接失败
- 于是 `plan.startMicroseconds` 对 `inInclusive = 0` 恒为 `0`
- `src/media_ffmpeg/src/ClipExportWriter.cpp#L315-316,336,349`：`times.push_back(av_rescale_q(pts/entry->timestamp, stream.time_base, kMicrosecondsBase))` —— 原始 PTS，不减首帧
- `src/application/src/ClipExportPlanner.cpp#L91-102`：`aligned = keyframeTimes.front()`，随后 `aligned >= endMicroseconds` → 失败

**最小反例（已实测）**：`tests/fixtures/media/h264_nonzero_start_64x48_30fps_12.mp4`

ffprobe 实测事实（`out/verification/clock-base-probe/fixture-facts.txt`）：

```
stream: time_base=1/15360, start_pts=15360, start_time=1.000000, avg_frame_rate=30/1, nb_frames=12
frames: 1.000000 I, 1.033333 B, 1.066667 B, 1.100000 B, 1.133333 P, ... 共 12 帧
key packets: 1（唯一的 I 帧在 PTS 1.000000 s）
```

所以该文件的关键帧表**就是** `{1'000'000}` 微秒，而规划器给全区间算出 `[0, 400000)`。

**探针输出**（`out/verification/clock-base-probe/run.log`，使用仓库自身未修改的源码编译）：

```
== container-origin keyframes (what ClipExportWriter::keyframeTimes returns) ==
all 12 frames: planned [0, 400000) us
all 12 frames: alignClipExportStart FAILED: The requested range holds no keyframe, so the stream copy would be empty.
frames 0-4: planned [0, 166667) us
frames 0-4: alignClipExportStart FAILED: The requested range holds no keyframe, so the stream copy would be empty.
frames 3-7: planned [100000, 266667) us
frames 3-7: alignClipExportStart FAILED: The requested range holds no keyframe, so the stream copy would be empty.

== same keyframes after normalising to the source's first PTS ==
all 12 frames: aligned start=0 us shift=0 us firstFrame=0
frames 3-7: aligned start=0 us shift=-100000 us firstFrame=0
```

**对照组**：把关键帧表改为归零后的 `{0}`，同一规划器立刻正常对齐（`shift=-100000`），与零起始 fixture 的形状逐位一致。这把因果钉死了：**问题在时间原点，不在规划算法。**

**影响**：任何首 PTS 非零的可播放录屏（常见于剪辑导出、录屏、拼接文件）都截不出所选片段，报错文案还会误导用户以为是区间或关键帧设置问题。

**测试为何漏**：`tests/component/media/ClipExportWriterTests.cpp` 全程只用 `h264_a_320x180_30fps_12.mp4`（零起始）与 `frames 3-7 → shift -100000` 的**零起始形状**。仓库**有**非零起始 fixture，`tests/component/media/SoftwareDecoderTests.cpp#L740-752` 用它断言 `presentationTime == 1'000'000`（播放路径是对的），但导出链路**从未**接入它。

**最低验收（修订）**：不能只删掉报错条件。要把「源首 PTS / 归零媒体时间 / 导出时间基准」三件事统一。建议把 `keyframeTimes()` 改为返回**已减去源首 PTS** 的归零时间（与规划器同基），writer 侧 `av_seek_frame` 与 `endPts` 再相应加回首 PTS。验收用 0 起始、正起始、B 帧重排、VFR 各选首/中/尾区间；重新解码成片，与源实际帧身份逐项核对；关键帧前滚与尾部参考帧必须在界面上明确展示。

---

### P1-B 指标与差异画面不同像素口径：**机制已本地实测**（证据等级上调）

**机制**：两条路径在色度处理上分叉。

- 显示路径：`src/media_ffmpeg/src/SoftwareDecoder.cpp#L855-874` 明确注释「every source (RGB, 4:4:4, 4:2:2 or 4:2:0, 8- or 10-bit) is converted to the **NV12/P010 4:2:0** display path」
- 指标路径：`src/media_ffmpeg/src/PairMetricsDecodeSession.cpp#L105-136` 直接从 `sourceFormat` 转 `AV_PIX_FMT_RGBA`，**不经过 NV12**

**关键补充（原报告未指出，但直接强化此项）**：`rgbaFromFrame()` 上方的注释（`PairMetricsDecodeSession.cpp#L84-86`）写着「The conversion **mirrors the display normalization contract** (matrix/range-correct swscale) so measured differences **describe the pixels the renderer shows**」。这句话与代码事实不符——显示路径要过 NV12 色度抽取，指标路径不过。**这是一个文档与实现互相矛盾的注释，比原报告的表述更值得修。**

**本地实测复现**（`out/verification/pixel-contract-probe/run.log`，本地 FFmpeg 7.1.5）。4×4 `yuv444p`，Y=128、V=128，A 的 U 逐列 64/192 交替，B 为反相（一条只在完整色度分辨率下存在的细横彩色边）：

```
display path  NV12  (SoftwareDecoder -> GPU): IDENTICAL
metrics path RGBA8 (PairMetricsDecodeSession): DIFFERENT (max abs delta 254 over 32/64 bytes)
RGBA RGB MAE (mean over bytes): 76.00
max abs RGBA delta: 254
```

即：**眼前两幅画面一模一样，而 MAE 读数高达 76、最大差 254。** 这直接违反产品文档关于「两候选采用相同阈值、增益、色尺；局部误差不会因自动缩放色尺而误导」的承诺。

**界面提示为何不足**：`src/application/src/ComparisonExactness.cpp#L14-17,55-61,126` 的 `preservesNormalizedPlaneCodes()` 只对 `yuv420p/yuvj420p/yuv420p10le/nv12/p010le` 判 `pixelExact`，其余格式确实会被标为 `DisplaySpaceConverted`，界面也会显示「显示空间转换 (非原生直通码值)」。**但这句提示说的是「不是原生码值」，并没有告诉用户「你看到的（NV12）和我测的（RGBA）是两套像素口径」**——所以原报告「提示没有区分这两种口径」的判断是准确的，只是应当精确表述为：提示存在但语义含糊。

**最低改法**：先决定指标比较「源解码 RGB」还是「显示缓冲」，并把标签写清楚（当前固定名 `cpu-rgb-absolute-v1`，`src/application/include/dvs/application/ComparisonMetrics.h#L13`，本身就是「源 RGB」语义）。若承诺对应眼前误差图，就复用同一归一化契约并做 CPU/GPU 对拍。**保留原有快路径，不要立刻把所有播放改成 RGBA。**

---

### P1-C 正阈值清黑叠加高亮：**静态控制流已确认**（原判成立）

`src/ui_qml/shaders/Nv12ToRgb.hlsl` 的 `DifferencePixelShader` 结构是：

```
L179   channelDifference = abs(rgbA - rgbB)
L180   if (thresholdEnabled != 0U) {
L181-194     thresholdSample = ...            # 三种策略
L195     if (thresholdSample < differenceThreshold) {
L196-197     const float alpha = saturate(opacity);
            return float4(0.0f, 0.0f, 0.0f, alpha);   # ← 立即返回黑色
         }
       }
L200-229   各差异模式分支：4=ExactPlanes, 5=SignedSubtract, 6=Highlight, 7=Crossfade, else=RgbAbsolute
```

阈值过滤**先于**所有模式分支执行，未达阈值一律返回黑色。

- **Highlight（mode 6）**：`L218-224` 的设计意图是「保留 A 并 tint」，低于阈值应保留原画。相同非黑画面 + 阈值 1 → **整幅变黑**。
- **SignedSubtract（mode 5）**：`L214-217` 注释自述「mid-gray is zero」，低于阈值应输出中灰。现在输出黑色 = 把「无差异」误报成「A 明显暗于 B」。

原报告的判断成立，且这两条与仓库自身文档冲突：`docs/engineering/visual-review-backlog.md#L981-982`（2026-09-25 区块）明确写着「上轮已核实着色器中 Highlight 指标本就是『保留 A 路原图＋按增益对超阈值区域红色 tint』，与审查描述一致」。**实现与已核实的契约相反。**

**测试为何漏（已逐条确认）**：
- `tests/component/ui/ComparisonSurfaceTests.cpp#L1916` `ThresholdMaskBlacksDifferencesBelowTheSelectedPolicy` **没有设置 `differenceMetric`**，即只覆盖默认的 RgbAbsolute，而「低于阈值 → 黑」对该模式本来就是对的。
- 两个「every metric」循环都只枚举 4 个旧模式：`#L1501-1506`（`RgbAbsolute, Luma, Chroma, Heatmap`）与 `#L2085-2090`（同 4 项）。**SignedSubtract(5)、Highlight(6)、Crossfade(7)、ExactPlanes(4) 全部不在任何枚举里。**

**Fade 的处理（原报告正确）**：视频侧 Fade 已从主界面移除并回退到 Wipe（`src/ui_qml/qml/Main.qml#L504-512`），因此 Crossfade(7) 的阈值交互不是当前用户可达缺陷，**不计入本轮用户问题**。但请注意 `ComparisonSurface::setViewMode` 仍接受 `Fade`（`src/ui_qml/src/ComparisonSurface.cpp#L337-339`），只是入口不可达。

**最低改法**：阈值只应过滤差异贡献，各模式保留自己的零差异外观；补「相同原图 + 阈值 1」的可见像素断言，并把两个 every-metric 循环扩到全部 8 个模式。

---

### P1-D 覆盖导出不是事务性替换：**错误处理序列已确认**（原判成立）

`src/media_ffmpeg/src/ClipExportWriter.cpp#L553-567`：

```cpp
std::filesystem::rename(workPath, job.outputPath, renameError);
if (renameError) {
    std::error_code ignored;
    std::filesystem::remove(job.outputPath, ignored);        // ← 删掉旧目标
    std::filesystem::rename(workPath, job.outputPath, renameError);
    if (renameError) { /* 报告 kFailed ... */ }
}
```

删除成功、第二次 rename 失败，或进程在两步之间退出 → **旧目标已丢失**。第一次失败无法区分「目标已存在」与「其他 I/O 原因」，因此这个 remove 对磁盘满、句柄占用、权限问题同样会执行。

两处补充证据（原报告未指出）：
1. 临时文件名 `workFilePath()`（`#L92-100`）是 `<stem>.<requestId>.partial<ext>`，**确定性推导而非独占创建**：同一 requestId 的两次导出会相互覆盖，且崩溃残留的 `.partial` 不会被识别。
2. 全文件**没有**源路径与目标/临时路径的同一文件防护。用户把导出目标选成源文件时，`perform()`（`#L362-374`）只检查空路径，会先读完源、再把副本 rename 到源上。

**已有可复用资产**：`src/platform_windows/src/AtomicFilePublisher.cpp#L326-366` 的 `publishReplacingExisting()` 用 `ReplaceFileW` + 备份 + `ERROR_UNABLE_TO_MOVE_REPLACEMENT_2` 恢复路径，正是这个场景，但导出链路未使用它。

**最低验收**：故障注入覆盖「提交 / 替换 / 恢复」各步骤，旧目标内容始终可恢复；临时文件独占创建；显式防止源与目标/临时指向同一文件。

---

### P2-A VFR 被导出入口挡住：**控制流已确认**（原判成立）

- `src/media_ffmpeg/src/MediaProbe.cpp#L514-519`：CFR 校验失败时 `frameRate = std::nullopt; timingConfidence = kVariableFrameRate` —— 注释明确说这是「valid variable-frame-rate classification, not a probe error」
- `src/ui_qml/src/ClipExportController.cpp#L262-273`：`makeRequest()` 要求 `validated->canonicalRate().has_value()`，注释写「A still-image session has no canonical rate; clip export is video-only」

结果：VFR 可播放、可设区间，**不能导出**。而底层规划器已支持 VFR（`tests/unit/application/ClipExportPlannerTests.cpp#L65-78` 就是 VFR 用例）。缺的是入口契约：应当区分「无 rate 因为是图片」与「无 rate 因为是 VFR」，后者应放行。

顺带一条准确的文案缺陷：`src/ui_qml/qml/Main.qml#L604-605` 失败时提示「请先设置入点与出点」，而 VFR 场景下入出点**可能已设好**，提示指向错误原因。

---

### P2-B 坏点占比界面丢数值：**JS 语义已实测确认**（原判成立且可精确描述）

`src/ui_qml/qml/TabbedInspector.qml`：

```qml
// L105-108
const ratioText = ratioPercent >= 0.01 ? ratioPercent.toFixed(2) + "%" : "< 0.01%";
const mismatchRow = qsTr("坏点占比（%1 ≥ 阈值 %2）").arg(...).arg(...);   // ← 裸字符串
const rows = [[...MAE...], [...MSE...], [...PSNR...], [...最大...],
              mismatchRow,                       // ← 混进 [标题, 值] 数组
              [qsTr("坏点数"), ...], ...];
```

delegate（`#L576-595`）取 `modelData[0]` / `modelData[1]`。对 JS 字符串取下标得到**字符**。本地实测：

```
"坏点占比（任一通道 ≥ 阈值 12）".length = 18
row[0] = "坏"   row[1] = "点"
```

所以这一行渲染成**「坏」|「点」两个字**，比例、策略、阈值全部不显示，而 `ratioText` 在 `L106` 算完后被完全丢弃（死变量）。

**修法**：改为 `[mismatchRow, ratioText]`。测试必须断言**最终可见标签与比例**，不能只断言控制器输出。

---

### P2-C Render ACK 的语义边界：**代码语义已确认**（原判成立）

`src/platform_windows/src/D3d11ComparisonRenderer.cpp`：

```cpp
// L938-942
if (!drawPreparedSet(prepared, lease)) { return ResourceFailure; }
frontPublication_ = publication;
return acknowledge(publication, *publication.set);     // ← draw 返回后立即 ACK
```

`acknowledge()`（`#L1622-1645`）随即把 `FrameSetPresented` 推入 mailbox。

Direct3D 绘图命令进入**设备内部命令缓冲**（[ID3D11DeviceContext::Flush 文档](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-flush)），Qt 也另有帧提交/交换信号（[QQuickWindow::afterFrameEnd](https://doc.qt.io/qt-6.8/qquickwindow.html#afterFrameEnd)），二者都不能简单等同物理扫描输出。

结论：这是一组**「已交给渲染器」的连续性证据**，是有价值的背压与身份保护机制，但**不能**独自证实合成层/显示器没有错过呈现。若拿它判断插帧视频本身卡顿属过度推论。原报告「未声称已在硬件观察到丢帧」的措辞是恰当的。

**最低改法**：把统计口径命名准确；在同一帧身份上补 Qt 提交时点与外部真实呈现证据；**不要**靠每帧同步等待 GPU 来「修」计数。

---

### P2-D 色彩管理边界：**已确认为明确未实现**（原判成立，但应下调为用户影响待定）

- `src/media_ffmpeg/src/MediaProbe.cpp` 接受 LINEAR/sRGB transfer，但颜色变换只消费 `matrix`/`range`/`bitDepth`（`src/media_ffmpeg/src/PairMetricsDecodeSession.cpp#L80-82` 的 `swsColorSpace()` 只取 matrix；`src/platform_windows/src/D3d11ComparisonRenderer.cpp#L784-828` 同理），**transfer 与 primaries、chroma siting 未进入完整契约**
- 「能够打开」不等于「已统一到共同显示色彩空间」
- PQ/HLG 被**明确拒绝**，这是已声明的支持边界，不是偷偷错误显示
- 图片完整 ICC 链未实现，且提示只在 `displayConverted` 条件下出现（`src/ui_qml/qml/ImageWorkspace.qml#L3017-3030`），原生 RGBA8 带 ICC 时可能无提示

原报告「不要在没有样例时武断归因」的处理是正确的。这是**契约明确性**问题，不是当前已证实的用户可见缺陷。建议先明确 SDR/code-value 审查口径、对无法正确解释的素材告知，再按真实素材需求补转换。

---

## 3. 对原报告的修正（必读）

| # | 原报告表述 | 核验后修正 |
|---|---|---|
| 1 | P1-A「已用仓库自带 12 帧样本及当前核心源码复现」 | **成立，且本次已把复现固化为可重跑探针**（`out/verification/clock-base-probe/`），用仓库自身未修改源码编译运行。证据等级可由「源码推断」上调为「本地复现器」。 |
| 2 | P1-B「显示先转 4:2:0，指标直接转 RGBA」 | 机制**成立**，且已用本地 FFmpeg 实测（显示路径字节相同、指标路径最大差 254）。但原报告漏了一处更强的证据：`PairMetricsDecodeSession.cpp#L84-86` 的注释**自诩**「mirrors the display normalization contract… describe the pixels the renderer shows」，与实现直接矛盾。这是「注释说谎」，应一并修。 |
| 3 | P2-B「这一行变成『坏』『点』两个字符」 | **精确成立**，已用 node 实测 JS 字符串下标语义。另确认 `ratioText` 为死变量。 |
| 4 | 「SourceDecodeActor 在成员初始化列表启动 worker，而 workerThreadId_ 在 worker_ 之后才初始化」 | **次序判断成立**（声明序 `SourceDecodeActor.h#L143 worker_` → `#L144 workerThreadId_`，而 C++ 按声明序初始化），但应补一句限定：`run()` 先取 `mutex_`（`#L134`，在 `worker_` 之前构造）再写该成员，构造体随后等 `started_` 握手，因此**当前实现下是良性的**，不是已发生的故障。真正值得收紧的是 `cache_`/`cacheKey_`（`#L157-158`）在 worker 启动**之后**才构造——一旦 `run()` 的早段改到碰它们，就是 use-before-construction。修法仍是「声明体启动线程」而非改线程模型。 |
| 5 | 「PlaybackCoordinator 约 4719 行、ReviewController 约 2830、Main.qml 约 3650、ImageWorkspace.qml 约 3131」 | 实测总行数分别为 **4727 / 2850 / 3762 / 3132**（代码行 4464 / 2596 / 2988 / 2892）。原报告数字基本正确，引用时建议用实测值并说明是总行数。 |
| 6 | 「224MiB 播放 FrameBudget」 | 确认为 `src/app/ReviewRuntime.cpp#L51` `kPlaybackFrameBudgetBytes = 224 MiB`，且它只是播放帧预算，不代表进程/GPU 总内存承诺。原判成立。 |

另确认成立的支撑性事实：四条 `DISABLED_` 测试（`MainQmlContractTests.cpp#L4216,4311,4361,4413`：RangeLoop / ScrubLatestWins / WipeHandle 键盘 / TimelineAccessible）确实仍禁用；GameDVR 真实录屏回归确实由 `DVS_TEST_GAMEDVR_CAPTURE` 门控、缺样本即 skip（`SoftwareDecoderTests.cpp#L760-773`）；quality workflow 的覆盖率门禁确实作用于 fixture 自测数据（`.github/workflows/quality.yml#L41-49` 传 `tests\fixtures\coverage\sample.cobertura.xml`），不是当前应用实测覆盖率；`ReviewSessionFacade` 确实是转发现有 controller 的薄层（`src/ui_qml/src/ReviewSessionFacade.cpp#L15-25`）；四条近期提交均存在且与描述相符。

---

## 4. 优先级（修订后）

保持不变的前三件事，但把 P1-B 的修法具体化：

1. **P1-A 导出时间基准**（已有本地复现器 + fixture；改 `keyframeTimes()` 归零 + writer 侧加回首 PTS；归零 fixture 做对照组）
2. **P1-B 像素口径**（先决定「源解码 RGB」还是「显示缓冲」并修 `PairMetricsDecodeSession.cpp#L84-86` 的失实注释；如承诺对应误差图则做 CPU/GPU 对拍；保留快路径）
3. **P1-C 阈值高亮**（让阈值只过滤差异贡献；把两个 every-metric 循环扩到全部 8 模式；补「相同原图 + 阈值 1」像素断言；顺手修死变量 `ratioText`，即 P2-B）

随后：P1-D 事务覆盖（接 `AtomicFilePublisher`）与 P2-A VFR 入口。P2-C/P2-D 属于契约与观测口径整理，可并入第 3 步而不必抢先。

**不行动项**：不要补做已存在的裁剪、涂画、马赛克；不要把不可达的 Fade/Crossfade 阈值问题算作当前用户缺陷；不要在没有样例的情况下把「RGBA 偏蓝」归因于色彩管理。

---

## 5. 三步实施路线（原报告路线核验后仍适用）

**第 1 步：可信帧、时间与像素基准**
- 修 P1-A / P1-C / P2-B / P1-D / P2-A
- 明确指标像素口径并修正失实注释
- 建立少量固定基准素材，每个样本写明正确帧身份、时间与像素口径
- 结束条件：每个缺陷有能先失败后通过的端到端反例；**不增加新播放能力**

**第 2 步：确定性的交互与真实呈现验证**
- 用固定真实录屏验证 GT + 两候选；区分「插帧审查」与「原速播放」，把播放器跳帧、源重复帧、显示层节奏分别计数并说明
- 按 render-submit / Qt-frame-submit / 外部呈现证据三层区分观测口径；先测瓶颈再优化
- 结束条件：在目标 Windows 硬件上跑完用户短流程与必要长播，记录该 SHA、素材、模式、刷新率与失败边界；**不能用 WARP 或旧版本结果代替**

**第 3 步：把已有能力整理成好用的审查流程**
- 首屏/状态栏用用户语言说明当前源、按帧还是按时间映射、是否重采样、指标是否对应当前帧
- 导出明确「实际导出哪一路」并展示真实前滚；记录问题时保存完整观察上下文并验证重开可复现
- 顺带拆出单一职责的状态投影/工作流，保留单一协调循环，不做全仓重构

---

## 6. 建议的固定验收包（小而真实）

| 样本 | 用途 | 必须写明的口径 |
|---|---|---|
| 两段手机/游戏录屏（含 VFR、非零 PTS、长 GOP） | seek、连续逐帧、三路切换、取消 | 正确帧身份、媒体时间、映射关系 |
| 一组 GT + 两个候选 | GT/候选语义、切换不漂移 | 角色与参考源 |
| 一组彩色细线 / 1px 文字 | **P1-B 的分叉** | 显示缓冲 vs 源 RGBA 两套差值 |
| 透明 PNG 与 16-bit PNM | 图片 Alpha 辅助数据 | 是否 displayConverted |

执行「正常播放 → 暂停 → 前后逐帧 → seek → 切候选 → 局部观察 → 记录/导出 → 重开复核」，同时记录眼前表现与 trace。**不以新增测试数量作为交付目标。**

---

## 7. 近期方向核对

反向 GOP 与比较语义（`06c2a4a4`）、9 月底逐帧游标修复（`4a67d1bc`）、2.1 文件夹播放与指标复用（`54460e9`）、区间导出落地（`978189d`）都与使用场景一致。反过来说：发布/脚本/样式层大量通过的记录，不能替代画面、帧身份、时间基准的贯穿测试——本轮四个 P1 全部是这一类缺口。

---

## 8. 本次核验的产出物

| 路径 | 内容 |
|---|---|
| `out/verification/clock-base-probe/` | `probe.cpp` + `Run-Probe.ps1` + `run.log` + `fixture-facts.txt`：用仓库自身源码复现 P1-A |
| `out/verification/pixel-contract-probe/` | `Run-PixelProbe.ps1` + `run.log`：实测 P1-B 两条路径不等价 |

两者都是一次性探针，不是第二套测试套件；若要长期保留，应按 `tests/component/media/ClipExportWriterTests.cpp` 的既有形态转成回归用例，并配变异证据。

**最终建议**：下一轮实施任务只领取「可信观察与导出闭环」这一批，有明确样例与退出条件；不以新增功能数、代码量或测试总数评价完成。
