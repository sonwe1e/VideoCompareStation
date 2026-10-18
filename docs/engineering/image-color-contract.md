# 图片像素与颜色数据契约（COLOR1 / COLOR3 / COLOR4 实现记录）

更新日期：2026-10-17。状态：工作区实现（基线 `8c90005` 加未提交改动）。
本页记录当前代码里的数据分界、颜色状态来源与验证入口。COLOR4 只为**显示派生结果**增加
SDR 到 sRGB 的转换；分析输入、统计、像素读数与导出码值仍不经过颜色引擎。“显示环境已验证”
仍是 COLOR5 才可声明的事实。计划与工单编号见
[收尾后分阶段改进计划](../plans/CompareStation_收尾后分阶段改进计划_2026-10-17.md)，
样本设计见 [image-color-samples.md](image-color-samples.md)，缺陷状态仍只在
[问题台账](visual-review-backlog.md) 记录。

## 1. 四段数据路径

同一个“像素值”在链路上有四个不同身份，不能互换称呼：

| 段 | 当前持有者 | 含义 | 允许的称呼 |
|---|---|---|---|
| 文件原始码值 | 未保留原始平面（8-bit RGBA 源除外） | 文件自身通道码值 | 仅当解码源就是 8-bit RGBA、且没有位深/通道/格式转换时才可称为原码值 |
| 解码转换缓冲 | `media::StillImage`：RGBA8 `rgba`；高位深源的 RGBA64 sidecar `rgba16` | 解码器/swscale 转换后的显示值 | “解码转换值”“显示缓冲值”；16-bit sidecar 也**不是**文件原始 16-bit 平面 |
| 分析输入 | 差异分析与探针读 `ImageReviewController` 的 `primary_`/`secondary_`（RGBA8）；16-bit 统计另读 sidecar | MAE/峰值/Alpha 统计与像素读数 | “分析缓冲值”；高位深统计须标注“转换后 RGBA16” |
| 显示派生结果 | `channelView()`、差异图、编辑副本显示；COLOR4 起可再经文件配置转 sRGB | 只影响屏幕的呈现 | “显示派生结果”；不能作为统计、编辑或“原始像素”的证据 |
| 导出语义 | PNG 样本导出＝解码像素；复制对比图/截图＝显示结果 | 由具体入口决定 | 每类导出必须带自己的说明，不能统称“无损导出” |

契约类型：

- `dvs::media::StillImage` 新增 `iccProfile`（原始 ICC 字节）、`colorPrimaries`、
  `colorTransfer`、`colorSpace`。媒体层只搬运，不解析、不应用。
- `dvs::ui::ImageColorInfo`（`src/ui_qml/include/dvs/ui/ImageColorContract.h`）保存配置分类、
  SHA-256、ICC 签名（数据色彩空间/配置类/主版本）与文件声明的 primaries/transfer/space。
- 当前 FFmpeg 构建禁用了 zlib，因此**没有 PNG 解码器**；PNG 由 Qt 回退解码。为了让这条真实
  路径也有颜色状态，`media::extractEmbeddedIccProfile()` 从容器读取 ICC 字节（PNG iCCP、
  JPEG APP2、WebP ICCP、TIFF tag 34675），组合根通过
  `ImageReviewController::setProcessStillImageColorProbe()` 注入；Qt 回退路径因此仍能给出
  “嵌入配置未应用/损坏”等状态，而不是“未探测”。
- 16-bit PNG 经 Qt 回退只得到 RGBA8，没有转换后 RGBA64 sidecar；该路径不能提供“转换后
  16-bit”统计。解码转换值与文件原始码值的区分仍成立，但这条精度路径待后续依赖决策补齐。
- `StillImageSourceInfo::color` 随 `ImagePairLoader::Result` 一起提交；替换、对调、关闭时与
  对应素材同生命周期。
- COLOR4 在组合根把同一份 ICC 字节解析成 `StillImageSourceInfo::colorSpace`（Qt
  `QColorSpace`）。它只供显示派生结果使用；解析失败时该侧降级为 `EmbeddedUnsupported`，
  不允许“结构支持但引擎不认”的静默近似。
- `ImageReviewController::primaryAnalysisValuePath()` 等属性把“分析输入是哪一段”变成可断言
  的枚举，不再让 UI 从格式字符串推断“原码值”。

## 2. 颜色配置状态

`ImageColorProfileStatus` 由 `classifyImageColorProfile()` 只做结构判定，不需要颜色引擎：

| 状态 | token | 判定 | UI 必须表达 |
|---|---|---|---|
| `NotProbed` | `not-probed` | 加载路径未提供配置字节（例如 Qt 回退加载） | 未探测，不能当作“无配置” |
| `None` | `none` | 文件没有嵌入配置 | 无配置；按 sRGB 假设观察，仍是未验证 |
| `EmbeddedSupported` | `embedded-supported` | ICC v2–v4，RGB/GRAY 数据空间，已知 profile class 与 PCS | “嵌入配置未应用”；只有转换真的发生后才可显示“已验证转换” |
| `EmbeddedUnsupported` | `embedded-unsupported` | 头部有效但色彩空间/类/版本超出首批 | 明确回退，不能显示为已色管 |
| `Corrupt` | `corrupt` | 字节存在但头部不合格（长度、声明大小、`acsp` 签名） | 明确损坏回退 |

结构判定只说明“字节是什么”，不证明配置文件可用于显示。分类结果里的
`conversionApplied` 始终是 false（它属于文件/分析契约）；COLOR4 另用
`primaryColorDisplayApplied()`／`secondaryColorDisplayApplied()` 记录“显示派生结果是否真的
经过该侧配置转换”。`displayColorEnvironmentVerified == false` 仍保持到 COLOR5 验证 Windows
输出链路之后。

## 3. 状态文案是单一来源

`ImageReviewController::primaryColorStatusText()`／`secondaryColorStatusText()` 生成每个侧
唯一的颜色状态句子（含“显示环境未验证”）。徽章、悬停提示、复制/导出的说明必须读同一属性，
不能在 QML 里各自拼一套，否则“配置未应用”和“已验证”会漂移。

颜色状态与“格式转换到 RGBA8”的徽章条件已经分开：原生 RGBA8 带 ICC 时也能看到颜色处理状态。

### COLOR4 显示转换（2026-10-17）

- **引擎**：Qt `QColorSpace` + `QColorTransform`（`QImage::convertedToColorSpace`）。选择它
  而不是引入 LittleCMS，是因为 COLOR2 的参考值恰好由 LittleCMS 独立生成；应用与校验不使用
  同一引擎。首批 ICC 为 v2 matrix/shaper，Qt 能解析；若 Qt 解析失败，该侧降级为
  `EmbeddedUnsupported` 并显示回退，不做近似。
- **只作用于显示**：`ImageReviewController::displayColorImage()` 在把 `primary_`/`secondary_`
  交给 `channelView()` 之前转换成 sRGB。分析缓冲、差异统计、Alpha 统计、悬停/冻结探针读的
  仍是解码缓冲；差异图仍由原始缓冲计算。关闭开关后显示回到解码像素，原始缓冲逐字节不变。
- **开关与状态**：默认对 `EmbeddedSupported` 且已解析出色彩空间的侧开启；状态条新增
  「颜色转换：开/关」徽章（`imageColorDisplayBadge`），点击切换 `colorDisplayEnabled`。
  状态句区分「已应用文件配置转换」「显示转换已关闭」「转换引擎未就绪」，且始终追加
  「显示环境未验证」。开关当前不持久化，重开窗口回到默认开启。
- **像素读数**：探针继续显示解码缓冲值；颜色转换只改屏幕。也就是说读数与屏幕颜色可能不同，
  这是本阶段有意的诚实状态，而不是遗漏。16-bit PNG 经 Qt 回退仍是 RGBA8，没有转换后
  RGBA64 sidecar；其颜色转换作用于 8-bit 显示值。
- **导出语义**：样本 manifest 明确写 `color_management.source_profile_applied = false`、
  `member_reference = decoded_rgba8_swscale`、`display_environment_verified = false`；
  复制/保存对比图带「显示结果 · 屏幕像素 · 未色彩管理」标注；片段导出仍是原始码流拷贝，
  不做颜色转换。

## 4. 不变量

- 颜色显示转换开关、观察模式（RGBA／Alpha 灰度／RGB 忽略）、Alpha 过滤、数值格式、差异增益、
  分割方向与淡化位置都不改变已提交的分析缓冲、探针值或统计。
- 分析缓冲只由素材提交写入；显示派生结果不得回写分析缓冲。
- 异步打开与差异结果按 request id／素材代次提交；迟到结果不能覆盖更新的素材。
- 透明像素的隐藏 RGB 保留在分析缓冲中；`displayConverted` 与颜色状态是两个独立事实。

## 5. 验证入口

| 内容 | 入口 |
|---|---|
| 配置分类、token、哈希、分析路径、状态文案、对调/关闭生命周期、容器探测回填 | `ui.ImageColorContractTests`（12 项） |
| 观察模式/Alpha 过滤/数值格式/增益变化后统计与原始缓冲不变 | `ui.ImageColorContractTests.AnalysisDataAndStatisticsDoNotMoveWithPresentationState` |
| 真实 PNG/JPEG/WebP ICC 字节提取与损坏配置保留 | `media.StillImageDecoderTests`（`ExtractsEmbeddedSrgbProfileFromRealPng`、`ExtractsCorruptPngProfileBytesForClassification`、`ExtractsJpegApp2ProfileSegmentsInOrder`、`ExtractsWebpIccpChunk`） |
| 样本清单、必需类别、哈希、独立参考字段与分类 | `ui.ImageColorSampleTests`（2 项，含 25 个样本与 13 个类别的计数下限） |
| 独立 LittleCMS 参考、样本哈希、PNM 尺寸与 AV1/VP9 探测 | `tools/fixtures/verify-fixtures.py --check-reproduction`（652 项检查，37/37 字节一致） |
| 迟到结果被丢弃 | `ui.ImageReviewControllerTests.StaleLoadResultCannotCommitUnderANewerPendingRequest`（注入旧 requestId 的成功结果）＋ `AsyncPairOpenReturnsImmediatelyAndRejectsLateN`、`CancelledOrClosedOpenNeverPublishesLateResult` |
| 主窗颜色状态可见性 | `ui.MainQmlContractTests.ImageColorStateIsVisiblePerSideWithoutHover` |
| COLOR4 引擎接受首批配置并拒绝损坏/Lab、Qt 转换与 LittleCMS 独立参考一致 | `ui.ImageColorConversionTests`（3 项：14 个带配置样本的接受/拒绝、9 个样本逐探针比对、同码值不同配置必须不同） |
| COLOR4 只改显示、不动原始缓冲，开关/状态/URL 同步 | `ui.ImageReviewControllerTests.ColorDisplayConversionChangesOnlyThePresentedImage`、`ColorDisplayConversionNeedsAParsedProfile`、`UnsupportedOrCorruptProfilesNeverGetConverted` |
| 主窗「颜色转换」徽章切换与 A/B 状态联动 | `ui.MainQmlContractTests.ImageColorStateIsVisiblePerSideWithoutHover`（同一用例覆盖开关、URL、原始像素与对调后的状态） |
| 样本导出不冒充已色管 | `media.SampleExportWriterTests.ExportsThePlannedTripletWithExactFrameIdentityAndManifest`（`color_management` 读回断言） |

## 6. 变异证据（2026-10-17）

- **错用显示缓冲做统计**：把 `requestDifferenceForCurrentMode` 的 A/B 参数换成
  `channelView()` 派生缓冲后，`AnalysisDataAndStatisticsDoNotMoveWithPresentationState`
  **失败**（观察模式切换后的统计改变）；恢复后通过。证明该测试真的约束分析输入。
- **漏检旧代次结果**：去掉 `handleLoadFinished` 的 `requestId != activeLoadRequestId` 校验后，
  新增的 `StaleLoadResultCannotCommitUnderANewerPendingRequest` **失败**（旧成功结果提交并
  把 `openPending()` 错清为 false，真正请求的 pendingA 未提交）；恢复后通过。
- **损坏配置被误转换**：去掉 `displayColorImage()` 的
  `profileStatus == EmbeddedSupported` 守卫后，`UnsupportedOrCorruptProfilesNeverGetConverted`
  **失败**（Corrupt + 有效色彩空间的显示像素被转换）；恢复后通过。证明该测试真的约束
  “状态不合格不得进入颜色引擎”。
- 原有 `AsyncPairOpenReturnsImmediatelyAndRejectsLateN` 与
  `BlockedDifferenceCannotPublishOrCacheAfterNewPairCommits` 在这两处变异下仍通过：loader 会在
  用户请求被替换/取消时取消作业，旧作业的成功结果不会到达；因此新增了受控注入 seam，
  不用旧测试冒充“已覆盖该行”。seam 仅测试使用，生产路径仍走同一身份校验。

## 7. 明确未完成

- **COLOR5 未验证**：Windows 输出链路、跨屏/DCI-P3 显示器、系统 HDR/Advanced Color 是否
  按预期解释转换结果，仍无物理显示或系统级证据；所有状态句继续写“显示环境未验证”。
- **16-bit 精度**：16-bit PNG 经 Qt 回退只有 RGBA8；颜色转换作用于显示值，没有转换后
  RGBA64 sidecar，也没有“应用配置后的 16-bit 读数”。
- **Alpha/合成空间**：显示转换保留 alpha，但预乘边界与 Alpha 合成空间未在 COLOR4 重新定义。
- **编辑副本与另存**：仍沿用既有语义；不要从“显示已转换”推断“编辑/另存保留原始精度或
  颜色配置”。
- **开关持久化**：`colorDisplayEnabled` 目前是会话内状态，重启回到默认开启。
