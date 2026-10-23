# 决策登记册

- 建立日期：2026-10-18
- 目的：把"拍过的板、假设、否决、触发结论"集中到一处，让任何一条都能回答三个问题：
  **谁定的、凭什么、现在还算不算数。**
- 与 [问题台账](visual-review-backlog.md) 的分工：台账记"哪里有问题、验收到什么程度"；
  本册记"遇到分歧时选了哪一边"。同一件事两边都出现时，以本册的状态为准，以台账的证据为准。

## 状态定义

| 状态 | 含义 | 什么时候可以改 |
|---|---|---|
| **已证实** | 有代码、测试或实测证据支持，当前行为与决策一致 | 新证据推翻时改为"已否决"并留旧证据 |
| **临时假设** | 为解除阻塞先按此推进，**不是批准**。文档与 UI 必须能看出它是假设 | 决策人或实测证据确认后改"已证实"，否则过期 |
| **已否决** | 明确不做；需要重启必须写新条目并引用旧条目 | 不得在原条目上直接翻转 |
| **未触发** | 条件项，等待触发条件出现 | 触发条件满足时改为"临时假设"或"已证实" |

规则：**纯口头拍板不计入任何状态。** 每条必须落到"UI 可见"或"文档可查"或"测试可复现"之一。

## 登记内容

| ID | 决策 | 状态 | 依据与证据 | 影响位置 |
|---|---|---|---|---|
| D-001 | 默认帧对齐＝严格索引（按序号），30/60 fps 混比不插值、不补帧；可切自动/手动对齐 | 已证实 | [对齐说明](../alignment.md)；模式徽章 `Main.qml:3811` | `MultiSourceFrameProvider`、对比浮标 |
| D-002 | "平均差异"＝RGB 三通道绝对差均值；Alpha 永不混入 MAE/MSE，单独统计 | 已证实 | `src/domain/include/dvs/domain/PixelDifference.h:33`；图片侧 Alpha 独立均值 `ImagePairLoader.cpp:557-562` | 图片读数、视频指标、导出标注 |
| D-003 | 视频 100%＝一个源像素占一个**物理**像素（含 DPR）；不可达时禁用并说明原因。图片 100% 标注为"100% 真实尺寸"，与"1:1 像素基准"区分 | 已证实 | [视口像素尺度](../engineering/viewport-pixel-scale.md)（DPR 1/1.25/1.5/2 各 40/40）；`ImageWorkspace.qml:638` | 100% 按钮、读数、截图 |
| D-004 | 对比截图标注＝"显示结果 · 屏幕像素 · 未色彩管理；非源文件原始码值" | 已证实（复制/另存为路径由 D-018 取代） | 台账 FLOW2（2026-10-17）；样本 manifest `member_identity` | 导出标注；片段导出与样本 manifest 仍带该声明 |
| D-005 | 图片**不做**三图对比 | 已否决 | 台账 C-01（2026-09-22 用户拍板） | `CompareModeBar`、`ImageReviewController` |
| D-006 | 放大镜**不作为新组件**实现，只保留"按住放大"快捷键 | 已否决（作为新组件） | 需求洞察报告 R-10 裁决；缩放＋ROI 已覆盖 | 工具栏 |
| D-007 | AV1/VP9 **不扩解码器**，只保证报错可读 | 已证实 | 台账 MEDIA1：真实样本返回 `kUnsupportedCodec`，语料库无此类素材 | `MediaProbe.cpp`、README 格式矩阵 |
| D-008 | 三源默认画幅＝固定 GT＋候选切换精看；五宫格只作为可切 Overview，不作默认 | 临时假设 | 代码中无五宫格布局痕迹；两份材料（PRD 与工具栏改版）仍写成 A/B 两说 | `Main.qml` 三源布局 |
| D-009 | 生成帧识别规则＝命名／元数据／用户指定三选一，先落规则文档，不建工具 | 未触发 | 需求洞察报告 R-15（阻塞 R-16）；触发器：用户给出生成帧判定规则，或插帧审查成为现实需求；到期：连续两批未触发按登记册规则降级为已否决 | 插帧协议、问题记录 |
| D-010 | 图片格式范围以 [README 支持矩阵](../../README.md#支持的格式) 为准，未列出格式诚实报错 | 临时假设 → 待一次实测复核 | README（2026-10-18）；台账 MEDIA1 | `MediaPaths.cpp`、README |
| D-011 | 硬件/性能门禁**不恢复为每批阻塞**；发布采用"已验证/未验证/已知不支持"三段声明 | 临时假设 | README「验证状态与发布声明」；2.5.0 例外一次性 | 发布流程、README |
| D-012 | 批量审查上下文、跨天复盘、问题记录旧 schema 兼容均为**未触发** | 未触发 | 触发器：手动切目录成为可感知摩擦；每周重开旧目录找缺陷＞1 次 | `ImageFolderPairModel`、`IssueLogController` |
| D-013 | 目录排序＝自然序（数字按量级、大小写折叠）+ 非 ASCII 按码点；**不引入 ICU/拼音排序** | 已证实 | `src/ui_qml/include/dvs/ui/NaturalOrder.h`；同一份实现供视频列表与图片游标使用 | 视频文件夹列表、单图翻页 |
| D-014 | 解码帧缓存**不按系统内存总量分档**：默认播放池保持 224 MiB，每源容量只由 `min(预算份额, 24 帧)` 决定，不设固定字节上限 | 临时假设（待单源长 GOP 实测确认 24 帧够用；触发改判：素材变 4K／10-bit 或三源满窗） | [缓存优先路线](../plans/cache-first-roadmap-2026-10-20.md) §0、§2C；`src/app/ReviewRuntime.cpp:52`；`src/media_ffmpeg/src/SourceFrameCache.h:49` | `MultiSourceFrameProvider` 份额、`ReviewRuntime` 预算、设置面 |
| D-015 | **16-bit PNG 与 Deflate TIFF 保存不做**；图片保存只维护 PNG／JPEG，JPEG 另存沿用显式合成背景 | 已否决 | 路线 §4；`NormalizedFrameFormat` 只有 NV12／P010／BGRA8；`src/ui_qml/src/ImageEditController.cpp:1759,1778` | 图片保存、发布载荷、能力台账 F03 邻项 |
| D-016 | 倒退（Left／A）体验以**单源**为主场景，双源比较不据此优化；反向窗口准入与门禁 profile 按单源设计 | 已证实 | 用户 2026-10-20 陈述，记录于[缓存优先路线](../plans/cache-first-roadmap-2026-10-20.md) §0 | 反向窗口准入规则、性能门禁素材选择 |
| D-017 | 倒退体验统一为**尾迹保留**一条原语：播放后的回退与 seek 后的回退共用"解码器刚走过的帧不丢"，播放环显式化但**不缩小**现有深度，独立的反向窗口构建不再保留 | 临时假设（探针确认 D3D11VA 显存占用与播放环现状后改判） | 路线 §批次 B 修订（2026-10-20）；播放路径已由 `fillReadAhead` 形成隐式环（`SourceDecodeActor.cpp:505-570,572`），缓存只在 open／close 清空（`:406,423`） | `SourceDecodeActor` 缓存策略、`SoftwareDecoder` 逐帧出口、性能门禁诊断口径 |
| D-018 | 复制／另存为对比图＝**纯净对比画面**：不拼标注条，也不含视口内的来源标签、徽章、视图按钮与横幅。来源与观察信息改存 PNG 文本元数据（`Description`），剪贴板路径不带 | 已证实（用户 2026-10-21 明确选择"完全只要视频帧"，已知会失去烧录在画面上的声明） | 用户选择记录于本轮对话；`ComparisonExportController::capturePicture` 抓取前抑制画面项的全部兄弟节点；`ComparisonExportSuppressesSiblingOverlays` 逐像素验证无 chrome 泄漏且元数据可读回 | `CompareModeBar`、`ComparisonExportController`、`Main.qml`；取代 D-004 在复制/另存为路径上的做法 |
| D-019 | 智能抠图（SAM2）**不进发布 ZIP**：发布包维持无 `onnxruntime`、无 `models/`；入口按 `smartSegmentAvailable` 能力显隐，需要者本机自置模型目录解锁；改为内置须另起打包批次，先解决 S03 的 GUI 线程同步推理风险并重审载荷门禁 | 临时假设（待用户在“内置 vs 本机安装”之间正式拍板；UX-19 已把入口做成能力可知的显隐，不冒充可用） | 2.5.0／2.5.1 包清单均 272 文件、无 onnxruntime／models（[发布说明](../releases/v2.5.1.md)）；[能力台账](capability-ledger.md) F03／S03；UX-19 契约（ImageWorkspace* 显隐）；本机模型 138.6 MB 无安装规则 | CPack 载荷门禁、ImageWorkspace 智能抠图入口、EX-05 验收（真实模型边缘质量仍待验） |
| D-020 | 图片窄窗档的「详情」面板用 **bar 上方的根级临时浮层**：显式展开/收起，展开时不改画布尺寸，但会临时盖住画布底缘一条 | 临时假设（S3 按此落地；提案 D3 的「不遮画布」字面更严格，须用户复核后改判——若要完全不遮挡须换面板位置） | [窄窗提案](../plans/ux13-narrow-window-redesign.md) §8；`ImageWorkspace.qml` 的 `imageCompactDetailsPanel`／`relayoutStatusHints`；`ImageStatusBarCompactTierCollapsesToSingleBand`（浮层打开时 bar ≤40、面板在 bar 之上、采样填入后表格长高） | ImageWorkspace 状态栏、详情浮层、S5 验收 |
| D-021 | 日常代码修改按影响范围验收；小且隔离的修改允许相关测试通过后交付，不默认追加全仓测试／lint；高风险、广泛影响和发布仍保留对应门禁 | 已证实（流程决策已落文档，耗时收益未测量） | 用户 2026-10-22 明确授权减轻验证负担；[构建指南选择表](../building.md#开发循环与完整验收)、根目录 AGENTS.md；现有测试与 CI 不删减 | Agent 验收选择、日常构建与 lint 调用 |
| D-022 | 图片标签页以**一次审查任务**为单位（单图或 A/B 对），打开默认新建标签、重复素材聚焦；标签不跨重启保存；最多 12 个，总工作集沿用单对 1.5 GiB 硬顶；当前仍只有一个编辑会话，按素材路径归属到标签 | 临时假设（用户按行业惯例授权；每标签独立编辑、窄窗视觉矩阵和真实使用反馈待验） | `ImageTaskHostTests` 6/6、`tst_image_task_tabs.qml`、`MainQmlContractTests.ImageTabsOpenSwitchAndCloseBackToTheNeighbour`、`MainQmlContractTests.ImageTabEditSessionStaysBoundToItsOwnTab`；`Main.qml` 的 `adoptActiveImageTab` 显式重绑；`ReviewImageProvider` token 路由 | `ImageTaskHost`、`ImageTaskTabs.qml`、`Main.qml` 图片打开/关闭路由、`ImageWorkspace.controller` |

## 新增或修改一条的步骤

1. 起一个 `D-0NN` 编号，写清"决策一句话"，不要写成愿望或待办。
2. 状态必须是上表四个之一；`临时假设` 要写明"何时过期"或"谁来证伪"。
3. 证据只接受三类：代码位置（文件:行）、文档链接、可复现测试名。
4. 若推翻旧条目，不删旧行，改为 `已否决` 并链接新条目。
5. 每批交付结束复查一次：连续两批未触发的 `未触发` 项，正式降级为 `已否决`。
