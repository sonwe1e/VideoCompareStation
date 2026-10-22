# UX-13 窄窗层叠重排设计提案

状态：**设计提案，待 lead 评议；评议通过前不进入实现。**
范围：960×640 问题档的 chrome 层叠重排。本轮只设计方案，不改 `src/` 下任何文件。
作者：visual 镜头；供体验 / 性能 / 工程镜头补充与质疑。

## 1. 目标尺寸与基线

两档目标尺寸沿用台账既有口径（逻辑像素；证据 `out/verification/ui-audit-2026-10-19/chrome-geometry.txt`，屏 2560×1516@1，证据台请求 1440×900）：

| 项 | 大窗 1440×900（现状档） | 960×640（问题档） | 出处 |
|---|---|---|---|
| 图片命令面板 imageCommandPanel | h=81（编辑态 121） | h=119 | chrome-geometry `image1440/imageEdit1440/image960` |
| 图片状态栏 imageStatusBar | h=72（y=788） | h=72（y=528） | 同上 |
| 图片工作区根 imageWorkspaceRoot | 1412×860 | 932×600 | 同上 |
| 图片画布 stage | 1388×683，面积 948004 | 908×385，面积 349580（=大窗 36.9%） | 同上 |
| 视频源条 activeSourceStrip | h=40（y=0） | 未采集（测量缺口，见 §5） | `video1440` |
| 视频模式栏 compareModeBar | h=40（y=40） | 未采集（测量缺口） | `video1440` |
| 视频视口 mediaViewportFocusTarget | 1412×615（y=86） | 未采集（测量缺口） | `video1440` |
| 视频走带 transport | 1412×161（y=709，docked） | 932×161（y=449，仍 docked） | `video1440/video960` |

问题档推算（root 高 ≈600）：视频 chrome = 40 + 40 + 161 = 241，视口只剩 ≈359（60%）；图片 chrome = 119 + 72 = 191，画布 385（64%）。**矛盾集中在高度**：三段常驻 chrome 各自合理，叠起来把画布压到四成以下。

触发档位：新增一个布局档位布尔（`compactChrome`，阈值：宿主窗口高度 ≤ 680）。最初取 ≤ 640，S2 门禁发现 Main.qml 窗口最小即 960×640（OS 钳制一切更小请求，落定高度在 640/641 间抖动，`QWindowsWindow::setGeometry` 告警为证），640 的阈值正好压在最小值上：生产几乎不可达（S1 档位实际休眠）、测试随机翻转，故抬到 680，给最小值 + 帧取整留出稳定带——641-680 的真实小窗口（笔记本常态）可靠进入紧凑档，960×640 证据档（落 641）也稳定触发。大窗行为完全不变；阈值以下逐项生效。**不引入新的常驻计时器或逐帧绑定**（见 §4）。

## 2. 具体重排方案

原则（产品 §7）：窄窗允许换行与收纳，**不允许重叠或缩小文字**；极简不得隐藏错误或改变比较语义。因此本轮**不把"浮动走带"设为窄档默认**——浮动是沉浸模式（`H`，`oscState 1/2`）的既有行为，由 V-10 保护；窄档默认仍是 docked，只做紧凑。

### D1 视频走带窄档紧凑（常驻 161 → 目标 ≤120）

现状：PlayerOsc 自上而为 状态栏 → MetricTimelineLane（可选）→ TimelineTracks 时间轴 → TransportBar（`compact: true` 已全量生效，见 PlayerOsc.qml 内 TransportBar 实例）。161px 是内容驱动（TransportBar `implicitHeight: transportColumn.implicitHeight`），不落在 VcsTheme 的 controlStrip 64–80 带内。

窄档做法：
- MetricTimelineLane 默认折叠为开关（无指标时本就无内容；打开才占高）。**不新增采样**——指标仍由检查器 surface 打开驱动（§4）。
- TransportBar 区间行（mark/working/export 三面，`transportRangeRow`）窄档压到 `Theme.hitAreaMinimum`（32）行高，chip 宽度用既有 compact 上限 168；主行播放键用 `Theme.playButtonHeight`（40）。
- 状态栏一行化：读数与"未测量"句合并为单行（UX-02 的如实播报保留，`mutedText`，不升警告色）。

### D2 图片命令面板窄档单行折叠（119 → 目标 ≤48；编辑态 ≤88）

现状：面板含多簇按钮（缩放/适应、比较、问题记录等），编辑态另起一行。
窄档做法：常用簇（缩放、适应窗口、重置）保留为 32px 高图标按钮一行；次要簇收进右侧「更多」菜单（`Theme.menu` 面、`menuBorder`）。工具提示与禁用原因照旧走 ToolTip。间距只用 EX-06 阶梯：簇内 `spaceMain`，簇间 `spaceGroup`，行内边距 `spaceMain`。

### D3 图片状态栏窄档单行化（72 → 目标 ≤40）

现状：72px 双行（含"规范化帧值 YCbCr"通道表、状态句）。
窄档做法：一行摘要（当前状态句 + 关键读数，`fontCaption` 11 / 数值 `fontMono` 13）；完整 YCbCr 表改为点击展开的浮层（`Theme.menu` 面 + `menuBorder`，`radiusSmall`）。浮层是显式展开，不违反"必须读的信息不缩没"——警告句仍在一行内可见。

### D4 视频源条 + 模式栏合并为一行（两行 80 → 一行 40，最高风险，放最后做）

现状：ActiveSourceStrip（y=0，40px）与 CompareModeBar（y=40，40px）纵向堆叠（Main.qml：CompareModeBar `anchors.top: sourceBar.bottom`）。
窄档做法：一行三簇——
- 左簇：源 chips 压缩为「字母 + 状态点」（身份色 `sourceColor/sourceBorder` 不变，长名移入 tooltip；`singleMode` 时保留添加入口）。
- 中簇：模式切换收成分段控件（当前模式名文字 + 图标），六个模式 + 布局子菜单放不下时进「更多」菜单。
- 右簇：「添加视频…」与检查器开关。
- 行高 `Theme.barRowHeight`（40）；不再往下压（`barRowHeightMin` 36 留给未来，本轮不用）。
- 契约保持：EX-03 单视频可见 + 禁用原因、EX-04 每项 `helpText` 悬停说明、UX-10「分割线」术语、V02 当前模式/源焦点 checked 态可见。

### 预估收益（落地以证据台实测为准，此处为设计目标，非承诺）

| 项 | 现状 @960×640 | 目标 | 画布增益 |
|---|---|---|---|
| 视频 chrome | 241 | ≤160（40 合并行 + ≤120 走带） | 视口 ≈359 → ≈440（+23%） |
| 图片 chrome | 191 | ≤88（≤48 面板 + ≤40 状态栏） | 画布 385 → ≈490（面积 +27%，回到大窗 47%） |

## 3. 不变量清单（每步验收都过一遍）

- **V-10**：悬浮播放条不得盖住视口「适应窗口/重置视图」按钮（既有 45 用例级证据）。
- **U-03**：指针在控制条上时控制条不淡出（`PlayerOsc.pointerInsidePanel`/`hideTimer`；`tst_player_osc`）。
- **UX-14**：提示条贴 stage 右缘内缩 16，不遮被判定依据的分割线（几何断言）。
- **EX-03/EX-04**：单视频时对比能力仍可见且带禁用原因；模式项 helpText 悬停可得。
- **UX-10**：分割线术语三处一致（模式栏/快捷键表/主窗口）。
- **比较语义与错误可见性**：当前模式、源身份 A/B/C、`error` 红只用于真失败——合并行与浮层不得让当前态或失败态变得不可辨；`fontMicro`(10) 不承载关键信息（VcsTheme 既有规则）。
- **`canvasMinimumWidth` 600 守卫**：600 以下既有 drawer 重排不变，本轮不动。
- **沉浸模式行为**：`H` 收起 chrome、唤醒条/手势唤出走带（`oscRevealRequested(interactive)`）路径不变。

## 4. 性能约束

- 不新增常驻 UI 线程刷新：D1–D4 全部是**静态布局折叠/合并**，无新 Timer、无逐帧绑定；`compactChrome` 档位切换是离散事件。
- 诊断类采样维持现状：仅 TabbedInspector surface 打开时采样（TabbedInspector.qml 既有约定）；MetricTimelineLane 折叠不引入新采样路径。
- 浮动/隐藏走带复用既有 `oscState` 三态机（0 docked / 1 floating / 2 hidden），不加第四态。
- 溢出「更多」菜单为按需创建的弹出层，不常驻场景图。

## 5. 验证计划

- 证据台：仓库自带 `ChromeGeometryAndScreenshotEvidence`，两档 960×640 + 1440×900 各采一轮（沿用 ui-audit-2026-10-19 的做法：12 张 PNG + `chrome-geometry.txt`，`QT_SCALE_FACTOR=0.5714` 适配 DPR 1.75）。证据落 `out/verification/ux13-<步骤号>/`。
- **补测量缺口**：现有证据缺 `video960` 的源条/模式栏/视口三项。缺口位置明确：`MainQmlContractTests.ChromeGeometryAndScreenshotEvidence` 的 video960 段（`tests/component/ui/MainQmlContractTests.cpp`，"Supported floor size" 注释后 `resize(960, 640)` 处）目前只 `recordItem("video960", "transport", true)`，而 video1440 段记录了 `activeSourceStrip`/`compareModeBar`/`transport`/`mediaViewportFocusTarget` 四个 required 项加可选 `viewportPixelReadoutPlate`。S1 的第一步是在 video960 段补齐同样的三个 required 项（+可选读数板），随后由 lead 以 `DVS_REVIEW_EVIDENCE_DIR` 指向 `out/verification/ux13-s1-baseline/` 跑一次证据用例，重录**改动前**基线（chrome-geometry.txt 三项齐全 + `video-min-960.png`），之后 S1 的布局改动才有完整 before/after。
- 每步验收判据：① §2 表中对应几何数字达标；② 两档截图并排人工对比无回归（重点：分割线两侧、提示条位置、源身份色）；③ 相关 ui.* 套件由 lead 统一跑绿。
- 需改基线/重录的契约测试清单：`ModeBarOrganizesTasksWithLayoutSubmenu`、`ShortcutHelpMatchesBoundShortcuts`（UX-10）；EX-03/04/06/08 契约；`tst_player_osc`（UX-02 未测量句、U-03）；ImageWorkspace 命令面板/状态栏相关契约（含 UX-19 显隐、UX-01 徽章）；V-10 窄窗 45 用例；ChromeGeometry 两档基线全量重录。EX-06 令牌阶梯断言必须继续通过（§2 所有间距/字号均取自既有阶梯，无新令牌；若实现中发现确需新令牌，按 task-2 模式在 VcsTheme.js 定义 + tst_vcs_theme 钉值后再用）。

## 6. 分步落地顺序（每步独立可验证、可回退）

| 步 | 内容 | 独立验收 | 回退方式 |
|---|---|---|---|
| S1 | D1 视频走带窄档紧凑 | transport.h ≤120 @960×640，大窗 h 仍 161 | 单 commit 还原；`compactChrome` 关掉即回现状 |
| S2 | D2 图片命令面板窄档折叠 | commandPanel ≤48（编辑态 ≤88） | 同上 |
| S3 | D3 图片状态栏单行 + YCbCr 展开浮层 | statusBar ≤40，浮层展开可读且不遮画布 | 同上 |
| S4 | D4 视频源条+模式栏合并行 | chrome 行 2→1，EX-03/04、UX-10、V02 契约全绿 | 同上（风险最高故最后） |
| S5 | 收尾：全量两档截图对比、chrome 几何重录、台账 UX-13 状态改"已实现（窄档）" | 证据归档 + 台账更新 | 不适用 |

顺序理由：S1–S3 是**纯高度压缩**，不动结构、风险低、收益已占大半；S4 是结构合并，放最后并在前几步证据稳定后进行。

## 7. 评议裁定（已定案，B 轮按此执行）

提案已评议通过。三个决策点裁定如下：

- **Q1 窄档走带不做自动浮动**（维持本提案默认）：浮动留沉浸模式，避免与 V-10/U-03 重新纠缠。
- **Q2 合并行的模式簇用文字缩写分段控件**（非图标）：两字标签（并排/分割/局部/差异等），与 UX-10 术语统一；EX-04 的 helpText 悬停保留；不引入新图标语言。
- **Q3 编辑态命令面板保留两行 ≤88**（维持本提案默认）。

执行顺序：S1→S5；B 轮共享任务由 lead 建立（S1 走带紧凑、S2 命令面板、S3 状态栏先行，S4 合并行最后），A 轮门禁与台账落定后开工。S1 任务描述必须包含 §5 的 video960 三项基线补采准备。
