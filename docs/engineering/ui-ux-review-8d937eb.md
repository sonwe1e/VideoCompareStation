# 界面与交互审查（UI/UX 评审）

日期：2026-10-19
范围：`main` @ `8d937eb9d00430de1de07ce0867fa212f3580a09`（`build: bound local build resources
and pin the toolchain`）。评审开始时工作区只有三个未跟踪文件（`.workbuddy/`、两份根目录报告），
没有已修改的受版本控制文件，因此本文结论对 HEAD 与当时的当前工作区同样成立。
性质：**评审**，不含产品代码改动。本文只提出判断与证据，修复由后续任务决定；
本轮落地的只有这份文档、台账摘要与 `docs/README.md` 的索引条目。

整改状态（同日第二轮）：UX-01 ~ UX-12、UX-14、UX-16 ~ UX-20 已在工作区实现，并各有契约测试
锁定；UX-13（960×640 折叠重排）、UX-15 的 `WipeHandle` 手柄键盘路径、UX-19 的发布载荷缺口
仍未处理。逐条状态与理由见[问题台账](visual-review-backlog.md)的 2026-10-19 整改条目。
**本页正文保持评审当时的原样**，行号与截图为 `8d937eb` 的事实记录，不作为当前代码的索引。

本页回答三个问题：界面在视觉与交互上是否自洽；设计系统是否真的被使用；哪些问题会
直接损害"审查结论可信"这一产品目标。问题编号用 `UX-` 前缀，避免与台账既有
`V-`/`I-`/`C-`/`U-` 编号冲突；可执行项同步登记在
[问题台账](visual-review-backlog.md)。

---

## 0. 核验边界（先说清做不到什么）

| # | 做了什么 | 没做什么 |
|---|---|---|
| 1 | 用仓库自带的 `MainQmlContractTests.ChromeGeometryAndScreenshotEvidence` 在 1440×900 与 960×640 两档实抓界面截图与几何（`out/verification/ui-audit/`，21 张 PNG + `chrome-geometry.txt`） | 未在 4K/HDR/多显示器 DPI 组合下重复；截图来自测试夹具的合成素材，不是真实影片 |
| 2 | 逐文件核对 QML 令牌消费、字号字面量、`Accessible.*`、`activeFocusOnTab`、快捷键门控 | 未运行读屏软件（NVDA/Narrator）实测朗读结果 |
| 3 | 从运行中的 2.5.1 进程用 `EnumWindows` 读取真实窗口标题 | 未截取原生标题栏渲染（DWM 合成部分） |
| 4 | 按 WCAG 2.x 相对亮度公式计算主题令牌对比度 | 对比度基于令牌十六进制值，不是屏幕实测像素；未做色弱模拟 |
| 5 | 只读检查发布 ZIP（`out/package/zip/CompareStation-2.5.1-windows-x64.zip`）内的实际载荷 | 未在干净 Windows 机器上安装并走查 |
| 6 | 未修改任何受版本控制文件；本轮新增测试证据目录在 `out/`（已被 `.gitignore` 忽略） | 未提交、未推送 |

**证据等级**：凡是标"实测"的，是本次运行产生或本次读取到的；标"代码"的是静态阅读
（`文件:行` 已回验）；标"判断"的是评审意见，需要产品侧确认后才能变成需求。

---

## 1. 结论摘要

界面的**视觉方向已经成立**：中性灰表面、单一系统蓝强调、克制的圆角与动效，
`VcsTheme.js` 是一份结构清晰、带注释说明取舍的设计令牌表，且有一套针对它的契约测试。

问题不在审美，而在**设计系统只落地了一半**：令牌被定义、被测试钉住，却在消费端被大量绕过；
更严重的是，产品的"诚实文案"与它的"颜色语言"在关键状态上互相矛盾——**界面用成功色报告未知**。

按影响排序的三条判断：

1. **颜色语义在关键状态上失效**（UX-01、UX-02）。未知/未测量被渲染成绿色和黄色，
   而黄红本应专属失败。后果是警报疲劳：真正的异常不再显眼。这是与产品目标 §2
   "无检测结果应显示未知，不应当作零缺陷"最直接的冲突。
2. **字体令牌从未接线**（UX-03、UX-04）。`fontFamilyUi` 全仓零引用，菜单把等宽令牌当字号用。
   这两处让"苹果式极简"停留在纸面。
3. **主界面存在键盘盲区**（UX-05、UX-06、UX-07）。菜单栏无 `Alt`/`F10` 入口、走带芯片不可
   聚焦、图片工作区的分割线只能拖动、`?` 帮助在图片工作区是死的——而图片版帮助内容
   已经写完并测过了。

---

## 2. 问题清单

| ID | 问题 | 证据 | 影响 |
|---|---|---|---|
| UX-01 | 图片颜色徽章用 `information`（绿）表示"未探测 / 显示未验证" | 代码 | 未知被读成正常 |
| UX-02 | 播放状态行在"尚未测量"时用警告黄 | 代码 + 实测 | 每次打开视频都报警，警报贬值 |
| UX-03 | `fontFamilyUi` 零引用，界面字体从未生效 | 代码 + 计数 | 设计意图未实现 |
| UX-04 | 菜单把 `fontMono`(13) 当字号，子菜单箭头比标签大 | 代码 + 实测 | 菜单比正文小，层级倒置 |
| UX-05 | 菜单栏无键盘入口（无 `Alt`/`F10`） | 代码 | 键盘用户无法开菜单 |
| UX-06 | 走带芯片与连续性开关不可 Tab 聚焦 | 代码 | 主走带只能鼠标操作 |
| UX-07 | `?` 帮助在图片工作区与空态失效，且无"帮助"菜单 | 代码 + 测试 | 已写好的帮助无法打开 |
| UX-08 | "设置"菜单只有一个条目，且服务缺失时整菜单消失 | 代码 | 设置无家，偏好分散 |
| UX-09 | "对比"菜单整体禁用，连带灰掉与视频数无关的命令 | 代码 | 单视频时命令不可达 |
| UX-10 | 分割线一词三写：分割线 / 划线 / 分屏线 | 代码 | 同一控件三个名字 |
| UX-11 | 同一动作两个名字（空态"对比两张图片…" vs 菜单"打开图片对…"） | 代码 | 用户以为是两件事 |
| UX-12 | 窗口标题重复应用名 | 实测 | 每张截图都带着 |
| UX-13 | 960×640 下 chrome 占 32% 高度，画布不足一半 | 实测 | 最小窗口难用于审查 |
| UX-14 | 居中提示横幅压在画面上并跨越分割线 | 实测 | 遮住正在审查的像素 |
| UX-15 | 图片分割线无键盘替代路径 | 代码 + 禁用测试 | 无障碍缺口无兜底 |
| UX-16 | 禁用态文字对比度 2.37–3.25:1 | 计算 | 解释性文字不可读 |
| UX-17 | 字号阶梯名存实亡（156 字面量 / 10 处离阶 / 31 处 10px） | 计数 | 排版不可控 |
| UX-18 | `tnum` 等宽数字只用 1 处，数字会横向抖动 | 代码 | 读数跳位 |
| UX-19 | "智能抠图"常驻按钮，但发布包不含模型与运行时 | 代码 + ZIP 实测 | 必然失败的功能占据工具栏 |
| UX-20 | 提示延迟 500ms / 650ms 混用 | 代码 | 同类反馈两种手感 |

---

## 3. 逐条展开

### UX-01 未知状态被渲染成成功色（最高优先）

图片工作区 A/B 两侧颜色徽章的点色：

```qml
// src/ui_qml/qml/ImageWorkspace.qml:6515（A 侧）/ :6532（B 侧）
dotColor: control.imageReview && control.imageReview.primaryColorProfileNeedsFallback
          ? Theme.warning : Theme.information
```

只有在"配置需要回退"时才转黄，其余一律 `information`（`#22c55e`，绿）。而徽章文案在
未探测到配置时是：

- `src/ui_qml/src/ImageReviewController.cpp:473` —— `颜色：未探测（该加载路径未提供配置文件）`
- `src/ui_qml/src/ImageReviewController.cpp:498` —— 追加 ` · 显示环境未验证`

即"不知道"与"已应用文件配置转换"共用同一个绿色圆点。用户扫一眼得到的信息是"正常"，
实际信息是"未知"。这与产品目标 §2 明确写的"无检测结果应显示未知，不应当作零缺陷"
相反，也与 `VcsTheme.js` 自己的注释（`information` 是"中性附加信息，不是状态"）不符——
未探测恰恰**是**一种状态。

**判断**：应把"未知/未验证"归入中性色（`mutedText` 或新增一个 `unknown` 令牌），
把 `warning` 留给真正的回退与失败。改动极小（两处表达式 + 一个令牌），
但这是整套诚实性叙事第一次在视觉上自洽。

### UX-02 "尚未测量"用警告色播报

`PlayerOsc.qml:49` 定义告警态：

```qml
readonly property bool playbackStatusAlert:
    control.playbackRunSkippedFrameSets > 0 || control.playbackCatchingUp
    || control.playbackLagMilliseconds >= 200 || control.playbackRunPresentedFrames === 0
```

`playbackRunPresentedFrames === 0` 覆盖了**任何刚打开、还没播过一帧**的会话；
`:178` 因此在状态行追加"本次播放尚未测量"，`:415` 用 `Theme.warning`（`#ffd60a`）渲染。

实测截图（`out/verification/ui-audit/video-default-1440.png`）里，打开视频后的第二行是：

> 流畅优先 · 目标 1.00× · **本次播放尚未测量**（黄色）

同一个黄色还用于真实跳帧、落后 200 ms、循环启动失败。结果是用户学会"黄色 = 正常状态"，
真正的异常失去信号价值。`Theme.warning` 的设计意图（"Limits and unavailable states"）是对的，
错在把"未测量"也算进了告警集合。

**判断**：把"未测量"拆出告警集合，用中性色；或让告警色只在真的落后/跳帧时出现。
文案本身很好，问题只在颜色。

### UX-03 界面字体令牌从未被使用

`VcsTheme.js:187` 定义了 `var fontFamilyUi = "Segoe UI Variable Text"`，
配套注释解释了为什么只能用单一字体族（QML `font` 值类型没有 `families` 列表）。

全仓计数：

- `fontFamilyUi` 在 `src/ui_qml/qml/*.qml` 的引用数：**0**
- `font.family` 总站点：52，其中 50 是 `Theme.fontFamilyMono`，另 1 是
  `ToolbarCombo.qml:48` 的自我透传（`control.font.family`）
- 应用层没有 `QGuiApplication::setFont`（`src/` 全量搜索无命中）

也就是说：等宽读数是唯一被真正接线的字体令牌，正文与菜单一律回落到 Qt 平台默认。
"苹果式极简"里的字体选择从未生效。

更值得注意的是**测试为什么没发现**：`tests/component/ui/qml/tst_vcs_theme.qml:222` 断言的是
令牌的**值**，`:246` 起的用例断言令牌能绑定到一个临时 `Text` 上——两者都不检查
**任何真实控件**是否消费了它。于是令牌从未生效，测试却全绿。

（该令牌由 `8c90005e`（2026-10-16，2.5.0）引入，到本次审查只有 3 天；问题不是"空转很久"，
而是**引入时就带着一条只验证值、不验证消费的测试**，所以第一次接线前没有任何环节会失败。）

**判断**：把 `fontFamilyUi` 接到根窗口的 `font.family` 上即可全局生效（一处改动）；
同时补一条"令牌必须有消费方"的契约断言，否则同类问题会以别的令牌再犯一次。

### UX-04 菜单把字号令牌当字体令牌用

```qml
// src/ui_qml/qml/VcsMenuBar.qml:31
font.pixelSize: Theme.fontMono      // 13
// src/ui_qml/qml/VcsMenuItem.qml:54
font.pixelSize: Theme.fontMono      // 13
```

`fontMono` 的语义是"等宽读数尺寸"（`VcsTheme.js:178`，注释写明是给时间码与帧计数用的），
这里却拿它当菜单字号，字体族仍是平台默认。两个后果：

1. 菜单项 13 px **比正文 14 px 小**——菜单是最高频的入口，反而比正文弱。
2. 同一行里子菜单箭头 `›` 用 `Theme.fontBody`（`VcsMenuItem.qml:69`，14 px），
   **比它跟随的标签（13 px）还大**，视觉层级倒置。

**判断**：菜单字号改回 `Theme.fontBody`；箭头应小于或等于标签。这是两行改动。

### UX-05 菜单栏没有键盘入口

全仓搜索 `Key_Alt` / `AltModifier` / `Key_F10`：QML 与 C++ 中**没有任何**应用级命中
（唯一两处是 `ComparisonViewport.qml:897` 的 Alt+拖动框选，与侧栏的 `Shift+F10` 上下文菜单）。

自定义 `VcsMenuBar.qml` 的 delegate 没有 `activeFocusOnTab`；`VcsMenu.qml:29` 虽有
`focus: true`，但那是弹出层内部焦点，不是进入路径。Qt 的 `MenuBar` 模板（核对
`out/build/dev/bin/qml/QtQuick/Templates/plugins.qmltypes`）只暴露 `delegate`/`menus`/
`contentData`/`menuAt`，没有内建的 Alt 激活属性。

**后果**：Windows 用户按 `Alt` 或 `F10` 什么都不会发生，菜单只能点。对比之下，
侧栏右键菜单**支持** `Menu` / `Shift+F10`（`VideoFolderSidebar.qml:342`、`:466`）——
同一个应用里存在两套标准。

### UX-06 主走带控件不可键盘聚焦

`activeFocusOnTab` 全仓只有 9 处，集中在 `ReviewActionButton`、`VcsToolButton`、
`ToolbarCombo`、`TimelineTracks`、`MetricTimelineLane`、`ImageWorkspace`、`VideoFolderSidebar`。

走带条的两个核心控件是自绘 `Rectangle + MouseArea`，**没有** `activeFocusOnTab`，也没有
`Keys` 处理：

- `TransportBar.qml` 的 `RangeChip` 组件（入点/出点/播放区间/循环/往返/清除/导出）
- `TransportBar.qml` 的 `ContinuityOption` 组件（流畅优先/逐帧完整）

两者都写了完整的 `Accessible.role/name/description/checked`（全仓 35 处 `Accessible.name`），
所以读屏能朗读——但键盘无法到达，读屏用户也就无法激活。

叠加 UX-05：**打开视频后，主走带操作事实上只能靠鼠标**。

### UX-07 帮助内容已交付，入口未接通

唯一的帮助入口是 `?`（`ReviewShortcuts.qml:347`），其 `enabled` 绑定
`control.shortcutsEnabled`，而后者来自 `Main.qml:649`：

```qml
readonly property bool globalMediaShortcutsEnabled:
    workspaceSession.videoActive && inputContext === 0 && (...)
```

第一项就是"有视频会话"。因此：

- **图片工作区**：`videoActive` 为假 → `?` 无效；`ImageWorkspace.qml` 自身也没有声明
  任何 `Shortcut`（全仓核对为空）。
- **空态首屏**：同样无效，新用户看不到快捷键表。

而图片版帮助表**已经写完并被测试钉住**：`MainQmlContractTests.cpp:11459`
（`ImageShortcutHelpDescribesTheRealEntries`）逐条断言了"空格/T"、"点击 A/B 标签"、
"复制颜色 ▾"、"Alpha 过滤"等 9 条文案。也就是说内容已交付、已验收，
但产品里没有任何路径能把它打开。

菜单栏也没有"帮助"顶层菜单（`ApplicationMenuBar.qml` 只有 文件/对比/分析/视图/设置），
也没有"关于/版本"入口——版本号只出现在标题栏。

**判断**：加一个"帮助"菜单（快捷键总表 / 版本 / 支持格式），并把 `?` 的启用条件
从"有视频会话"改为"无模态"。这同时解决 UX-07 与"设置菜单无家"的一半问题。

### UX-08 "设置"菜单名不副实

`ApplicationMenuBar.qml:617-629`：整个菜单只有一个条目"文件关联…"，且

```qml
menuItemVisible: control.fileAssociationService !== null
```

服务不存在时，**一个顶层菜单会整个消失**——菜单栏结构随运行环境变化，这本身违反
可预测性。与此同时，真正的偏好项分散在别处：

- 播放连续性、源对策略、对比对策略 → "对比"菜单
- 快捷键方案、双击自动播放 → "视图"菜单
- 文件关联 → "设置"菜单

用户想改一个设置，得先猜它在哪个语义菜单下。

### UX-09 "对比"菜单整体禁用，连累无关命令

```qml
// ApplicationMenuBar.qml:180
enabled: control.sourceCount > 1 && !control.busy
```

菜单内"播放连续性"子菜单自己判断的是 `menuItemEnabled: control.videoHasSession`（`:345`），
"显示/隐藏检查器"（`:424`）同样与视频数量无关。但父菜单 `enabled` 为假时整菜单打不开，
子项的门控永远不可达。结果是：**开一个视频时，连续性策略与检查器开关都点不到**。

**判断**：菜单级禁用应下放到条目级；"对比"菜单可保持可打开，只在无对比对象时灰掉对比相关条目。

### UX-10 / UX-11 命名不一致

**分割线有三种写法**（均为用户可见文案）：

| 写法 | 位置 |
|---|---|
| 分割线 | `ApplicationMenuBar.qml:199`、`ImageWorkspace.qml:3364`、`ReviewContextMenu.qml:74`、`TabbedInspector.qml:669`、`Main.qml:1983` |
| 划线 | `CompareModeBar.qml:129`（且被测试钉死，`MainQmlContractTests` 中断言 `text` 含"划线"） |
| 分屏线 | `ReviewShortcuts.qml:254/262/288/302`、`Main.qml:3457`（HUD 反馈） |

同一个控件在工具栏、菜单、帮助、HUD 里三个名字。测试把"划线"写成了契约，
所以这个词不能只改代码，还要同步改断言。

**同一动作两个名字**：空态按钮"对比两张图片…"（`EmptyReviewView.qml:160`）与菜单项
"打开图片对…"（`ApplicationMenuBar.qml:114`）走的是同一个
`requestImagePairOpen()`（`Main.qml:1758`）。用户会以为是两件事。

### UX-12 窗口标题重复应用名

运行中的 2.5.1 进程实测标题：

```
CompareStation 2.5.1 — 视觉审查 - CompareStation
```

`Main.qml:27` 已经组了完整标题，`DesktopApplication.cpp:151` 又设了
`setApplicationDisplayName("CompareStation")`，Qt 把它追加到窗口标题后。每张截图、
每个任务栏项都带着这个重复。

### UX-13 960×640 下画布不足一半

实测几何（`chrome-geometry.txt`）：

| 配置 | 窗口 | 画布 | 画布面积 |
|---|---|---|---|
| 图片 1440×900 | 1440×900 | 1388×684 | 949,392 px² |
| 图片 960×640 | 960×640 | 908×386 | 350,488 px² |
| 视频 1440×900 | 1440×900 | 1412×597 | — |
| 视频 960×640 | 960×640 | 932×? | — |

960 档里：命令面板 119 px + 状态栏 72 px = 191 px 常驻 chrome，加上模式行换行，
画布只剩 386 px 高（占窗口 60%，面积仅为大窗口的 37%）。模式芯片被迫换到第三行
（"深色/棋盘/黑底/白底"独占一行，见 `image-min-960.png`）。

审查工具的核心资产是像素面积。在最小窗口下，chrome 吃掉三分之一高度，
这不是"简洁"，是挤压。

**视频侧**：`transport.h` 在 1440 与 960 两档都是 **180 px**（不随高度收缩），
在 900 px 窗口里占 20%，且第二行（入点/出点/循环/导出）两侧大片留白、内容居中悬空。

### UX-14 居中提示横幅压在画面上

`Main.qml:5051-5062` 的 `notificationStack` 定位规则是
`y = stageFloor - 12 - height`，即贴着舞台底部、水平居中。实测在
`video-default-1440.png` 里，提示条占据逻辑坐标 y 591–626，而画布是 86–683——
它落在画面底部约 40 px 的带内，且水平居中**正好跨越分割线**。

"磁盘上的视频文件已变化，本次会话仍保留其原始标识。"这类提示会持续数秒，
期间用户正在对比的那条分割线被遮住。它是唯一一个"必然覆盖正在审查的像素"的元件，
权重却和常驻状态一样。

**判断**：移到画布上沿或状态栏，或至少避开分割线中点。

### UX-15 图片分割线没有键盘兜底

视频侧有 `Alt+方向键`（`ReviewShortcuts.qml:257-309`），但其 `enabled` 依赖
`reviewActions.wipeEnabled`，而 `Main.qml:3396` 把它绑到 `root.wipeMode`，
后者是 `effectiveViewMode === ComparisonSurface.Wipe`——**视频会话的模式**。
`ImageWorkspace.qml` 不写 `preferences.viewMode`（全仓核对为空），
所以图片工作区的 `wipePosition` 只由拖动写入，没有任何键盘路径。

分割线手柄本身的键盘缺口是已知的，并被一条禁用测试记录着：
`MainQmlContractTests.cpp:7686`（`DISABLED_WipeHandleIsKeyboardAdjustable`）。
问题在于：视频侧至少还有快捷键兜底，图片侧连兜底都没有。

### UX-16 禁用态文字对比度不足

按 WCAG 2.x 相对亮度公式计算（基于令牌十六进制值）：

| 前景 / 背景 | 对比度 | 4.5:1 |
|---|---|---|
| `disabledText` `#6c6c70` on `window` `#1c1c1e` | **3.25** | 不达标 |
| `disabledText` on `panel` `#242426` | **2.96** | 不达标 |
| `disabledText` on `menu` `#343438` | **2.37** | 不达标 |
| `mutedText` `#a5a5ab` on `window` | 8.57 | 达标 |
| `mutedText` on `menu` | 5.06 | 达标 |

而"对比"菜单灰掉时，禁用文字正是"你需要 2 个以上视频"这句解释的唯一载体（UX-09）。
`tst_vcs_theme.qml:89-98` 的对比度用例覆盖了 `mutedText`、`accentText`、`inverseText`，
**没有**覆盖 `disabledText`。

### UX-17 字号阶梯名存实亡

`VcsTheme.js` 定义阶梯 10/11/12/13/14/16，并在 `tst_vcs_theme.qml:141` 起断言了顺序。

全仓计数：

- `font.pixelSize` 字面量：**156** 处；令牌：167 处
- 落在阶梯外：**10** 处（15×4、17×3、20×1、24×1、28×1）
- 写死 10 px：**31** 处
- 单文件最集中：`ImageWorkspace.qml` 49 处字面量 vs 36 处令牌

**门禁为什么没拦**：`tools/quality/check-qml-token-literals.ps1` 是这轮审查里最值得说明的
发现之一。它确实覆盖 `font.pixelSize`，但它把**字面量等于某个令牌量级**的情况算作
"already at a token magnitude"（本次输出：156 处中 146 处属于此类），只有**阶梯外**的
10 处受基线约束，且基线本身允许其存在（`'15'=4`、`'17'=3`、`'20'=1`、`'24'=1`、`'28'=1`，
脚本注释写明这是"ratchet, not a migration"，量级相同的替换属于"引用卫生"而非漂移）。

也就是说：门禁的职责是**防止新漂移**，它从未打算强制 156 处字面量全部改成令牌。
这一点与本文的批评不矛盾——本文批评的不是"字面量存在"，而是**内容与尺寸错配**
（必须读的结论用了装饰尺寸）。这个维度门禁结构上无法覆盖，因为它只看数字不看语义。

其中 10 px 的站点包含**必须读**的内容：

| 位置 | 内容 |
|---|---|
| `ComparisonViewport.qml:1259` | "规范化帧值 YCbCr"表头 |
| `ComparisonViewport.qml:1269` | "· Δ 相对 A"（差值基准） |
| `SampleExportDialog.qml:255` | "生成帧 / GT" |
| `NearbyFramesCard.qml:240` | "缩略图源：未知"（**警告**） |
| `TabbedInspector.qml:942` | "实时采样 / 就绪"（状态） |

主题注释自己写着 `fontMicro`(10) "must never carry a key action or a must-read
conclusion"，`fontCaption`(11) 才是"可读但非关键标签"的下限。这些站点恰好违反了规则。
**判断**：需要的是"这条文案的语义等级 → 允许的最小字号"的映射，而不是又一个字面量门禁。

### UX-18 等宽数字令牌几乎未用

`fontFeaturesNumeric`（`tnum`）在 `VcsTheme.js:195` 定义，注释说明它服务于
"zoom badges, metric readouts"这类原地跳动的数字。实际只有 **1 个消费点**：
`TimelineTracks.qml:441`（另 1 处是注释）。

像素读数表、帧计数、缩放徽章、指标数值都没有用。拖动走带时这些数字会因字宽变化横向抖动——
这正是该令牌存在的理由。

### UX-19 "智能抠图"是必然失败的常驻入口

工具栏按钮在编辑模式下**始终可见**：

```qml
// ImageWorkspace.qml:3876-3883
ImageCommandButton {
    objectName: "imageEditToolSmartSelectButton"
    visible: control.editModeActive
    text: qsTr("智能抠图")
```

可用性只在**选中该工具之后**才提示（`ImageWorkspace.qml:4335`：
"AI 模型未就绪 (将 onnxruntime.dll 与 sam2 模型放入 models 目录后自动使能)"）。

而发布包实测不含这些载荷：

- ZIP（27.0 MB）内**无** `onnxruntime` 条目、**无** `models/` 目录
  （只有 Qt 自带的 `qml/QtQml/Models/`，与 SAM2 无关）
- 本机 `out/models/` 合计 **138.6 MB**（`sam2_encoder.onnx` 104.4 MB + `sam2_decoder.onnx` 15.8 MB），
  未进入任何安装规则（`install.*models` 命中数 0）

即：用户拿到的包永远无法使用这个按钮，而它占据工具栏第二的位置（仅次于"裁剪"）。
对一个强调"结论可信"的工具，常驻一个必然失败的功能是可信度成本。

**判断**：要么按可用性隐藏/置灰并在悬停说明原因，要么把它移到"更多工具"下拉里。

### UX-20 提示延迟不一致

- `VcsToolButton.qml:16`：`toolTipDelay: 500`（工具栏按钮默认）
- `TransportBar.qml:116`：`toolTipDelay: 650`；`ToolTip.delay: 650`（芯片，`:187`、`:241`）

同一类提示两种手感，且 650 ms 已接近"以为没有提示"的阈值。

---

## 4. 已经做对的部分（避免只列缺陷）

审查也应当记录成立的设计，否则修复时容易把对的改坏：

- **颜色令牌的组织方式**：表面分层（canvas → window → panel → menu）、半透明白描边
  代替实线、单一强调蓝，这套结构是可维护的。`VcsTheme.js` 的注释解释了每条规则的
  理由（含"为什么 information 从 #38bdf8 改成绿色"这类历史决策）。
- **源身份色系统**：A/B/C 三色与时间线标记色的分离有专门测试
  （`tst_vcs_theme.qml:101-131`），并在注释里写明"rail 只有 4-11 px，像某个源就是错的"。
  这是少见的、把可辨识性当成契约来测的做法。
- **对比度已有测试**：`tst_vcs_theme.qml:76-99` 对 8 组前景/背景做了 ≥4.5 断言，
  只是覆盖面漏了 `disabledText`。
- **键盘焦点契约有文档**：`VcsTheme.js:49-65` 明确写了"鼠标点击不留焦点环、
  只有键盘导航显示 2 px 环"，并指出 `visualFocus` 在非 Control 上绑定的陷阱。
- **术语与快捷键同源**：`ShortcutCatalog.js` 单一来源，菜单显示、帮助表、实际绑定
  由契约测试互相校验，防止三处漂移。
- **窄窗可达性有专门测试**：`NarrowWindowKeepsViewportCornerControlsReachable` 等用例
  锁住了"不能重叠、不能靠缩小文字换简洁"。
- **空态首屏**：一屏给出四条工作流入口 + 拖放提示，文案克制，是好的第一印象。

---

## 5. 建议修复顺序（按杠杆，不按工作量）

**第一批（改完即可感知，约 1.5 天）**

1. **UX-01 + UX-02 颜色语义复位**：未知/未测量 → 中性色；黄红只留给失败。
   改 `ImageWorkspace.qml:6515/6532` 与 `PlayerOsc.qml:49` 的告警集合。
   补一条测试：未知状态不得使用 `success`/`information`/`warning`。
2. **UX-03 + UX-04 字体接线**：`fontFamilyUi` 接到根窗口 `font.family`；
   菜单字号 `fontMono → fontBody`；箭头字号不得大于标签。
   补一条"令牌必须有消费方"的断言——`fontFamilyUi` 由 `8c90005e` 引入时就带着只验证值的
   测试，所以接线前没有任何环节会因它未生效而失败。

**第二批（结构性问题，约 2 天）**

3. **UX-07 + UX-08 帮助与设置归位**：新增"帮助"顶层菜单（快捷键 / 版本 / 支持格式）；
   `?` 的启用条件从"有视频会话"改为"无模态"；把分散的偏好项收进"设置"，
   并去掉"服务缺失就整菜单消失"的绑定。
4. **UX-05 + UX-06 + UX-15 键盘可达**：`Alt`/`F10` 进入菜单栏；
   `RangeChip`/`ContinuityOption` 改为可聚焦控件；图片分割线补 `Alt+方向键`。
   同时处理那条 `DISABLED_WipeHandleIsKeyboardAdjustable`。

**第三批（一致性收尾，约 1 天）**

5. **UX-10 + UX-11 + UX-12**：术语统一为"分割线"（含测试断言同步）、
   空态与菜单共用动作名、去掉重复的应用名。
6. **UX-16 + UX-17 + UX-18**：提 `disabledText` 到 ≥4.5:1（或改用 `mutedText` 的降级策略）；
   把"必须读"的 10 px 提到 `fontCaption`；给读数类数字接上 `tnum`。
7. **UX-09 + UX-19 + UX-20**：菜单禁用下放到条目级；智能抠图按可用性隐藏；
   提示延迟统一到 500 ms。

**第四批（需要设计取舍，非纯修复）**

8. **UX-13 + UX-14**：960 档的 chrome 折叠策略（模式芯片分两组、走带条随高度收缩）
   与提示横幅位置。这两项决定"最小窗口还能不能用于审查"，建议先出一版原型再定。

---

## 6. 证据与复现

截图与几何：

```powershell
$env:QT_QUICK_CONTROLS_STYLE='Basic'
$env:QSG_RENDER_LOOP='basic'
$env:DVS_REVIEW_EVIDENCE_DIR='out\verification\ui-audit'
out\build\dev\bin\dvs_main_qml_contract_tests.exe `
  --gtest_filter='*ChromeGeometryAndScreenshotEvidence*'
```

产物：21 张 PNG（`welcome-1440/960`、`video-default-1440`、`video-range-1440`、
`video-roi-triplet-1440`、`video-min-960`、`image-side-by-side-1440`、`image-diff-1440`、
`image-edit-1440`、`image-min-960`、`compare-toolbar-960/1280/1440/1920-{0,3}` 等）
与 `chrome-geometry.txt`（逻辑像素 + 设备像素比 1.75，屏幕 `S2719DGF 1920x1152@1.75`）。

窗口标题复现：启动 `out\build\dev\bin\CompareStation.exe`，用 `EnumWindows` 读可见窗口文本，
得到 `CompareStation 2.5.1 — 视觉审查 - CompareStation`。

发布包载荷复现：直接读
`out\package\zip\CompareStation-2.5.1-windows-x64.zip` 的条目表，
检索 `onnx` 与 `models/` 均为零命中（Qt 自带的 `qml/QtQml/Models/` 除外）。

**未覆盖**：真实影片素材下的界面观感、4K/HDR/多屏 DPI、读屏软件实测朗读、
触摸与手写笔输入、高对比度系统主题下的表现。这些需要独立环境，本文不宣称已验收。
