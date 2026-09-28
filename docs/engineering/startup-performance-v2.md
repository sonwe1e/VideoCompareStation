# 启动性能基线与归因（v2.0 计划 · Step 0/1）

更新日期：2026-09-27。测量对象：`main @ 8ffa577` + 工作区 Step 0/1 启动埋点改动（未提交）。
本文是 v2.0 优化计划（启动 / 体积 / 体验 / 清理 / 发布）中启动维度的唯一数据出处：
所有优化的前后对照必须来自同一脚本、同一构建类型（release）、同一素材。
上一轮会话中口头的 663–836 ms 区间已被本基线取代；`baseline2-*` 两轮（16 里程碑粒度）
取代更早的 `baseline-*`（13 里程碑）作为优化前参考值——两次之间只有埋点粒度差异，无行为差异。

## 测量协议（唯一裁判）

- 脚本：[tools/testing/measure-startup.ps1](../../tools/testing/measure-startup.ps1)。
  外部层 = Start-Process 起测后以 1 ms 轮询 `MainWindowHandle` 出现（已用 P/Invoke 探针
  确认该句柄就是主窗口 `Qt6111QWindowIcon` 且出现即可见）；内部层 = 子进程设
  `DVS_STARTUP_TIMING=1`，脚本解析 16 个 `[startup]` 里程碑（见下）。
- 里程碑的绝对时间戳是 MSVC steady_clock（即 QPC），与外部轮询同一时钟，
  因此 spawn→首标记、spawn→exec 可直接对齐（脚本内置 30 s 合理性窗口防时钟错配）。
- 每场景预热 1 轮不计 + 测量 5 轮；表中给中位数与 P95（nearest-rank，5 轮时 P95 即最大值）。
  任一测量轮无效（窗口未出现 / 里程碑缺失 / 提前退出）则整次运行作废。
- 外部时间含 Start-Process 启动开销（各轮一致），只用于本协议内部的前后对比，不与其他测量法混用。
- 缓存状态：顺序循环启动 = 热缓存。冷启动（重启后首轮）流程待定义，见"已知限制"。
- 首帧层（playback trace kind 0/4/8）尚未接入本表，见"已知限制"。

复测命令：

```powershell
pwsh tools/testing/measure-startup.ps1 -Label baseline-nofile -Rounds 5 -Warmup 1
pwsh tools/testing/measure-startup.ps1 -Label baseline-onefile -Rounds 5 -Warmup 1 `
    -LaunchArgument 'out\evidence-fixtures\media\frameid_1080p60_a.mp4'
```

产物（原始 stderr + JSON + Markdown）落盘 `out/startup-measurements/<label>-<时间戳>/`。

## 环境（各次基线运行相同）

| 项 | 值 |
|---|---|
| OS | Windows 11 家庭版 10.0.26200 |
| CPU | 13th Gen Intel Core i7-13700KF |
| GPU0 | OrayIddDriver Device（远程虚拟显示；驱动 17.50.19.949；120 Hz 3840×2160） |
| GPU1 | GameViewer Virtual Display Adapter（虚拟显示；15.6.5.199；144 Hz 2848×1320） |
| GPU2 | NVIDIA GeForce RTX 4090（驱动 32.0.16.1074；120 Hz 3840×2160） |
| 构建 | release 增量；`out/build/release/bin/CompareStation.exe` |
| git | `8ffa577`（工作区含 Step 0/1 埋点，未提交） |
| 素材（带文件场景） | `out/evidence-fixtures/media/frameid_1080p60_a.mp4` |

注意：系统存在 2 个虚拟显示适配器（OrayIddDriver / GameViewer）。DXGI 适配器枚举与
首帧设备初始化可能被它们放大；本机数据归因时必须连同此环境记录一起引用。

## Step 0/1 埋点（16 个里程碑，`DVS_STARTUP_TIMING` 未设时零输出）

| 里程碑 | 位置 | 含义 |
|---|---|---|
| `runDesktop-enter` | `Main.cpp` runDesktop 入口 | 内部时钟原点（进程装载在此之前） |
| `graphics-backend` | configureGraphicsBackend 返回 | 图形后端选择 |
| `qguiapplication` | DesktopApplication 构造返回 | QGuiApplication + Qt 平台插件初始化 |
| `broker` | StartupRequestBroker 选举返回 | 参数解析 + 单实例选举 |
| `runtime-create` | ReviewRuntime::create 返回 | 协调器 / 解码 actor / 时钟装配 |
| `load-enter` | DesktopApplication::load 入口 | QML 装载开始 |
| `context-ready` | engine->load() 调用前 | 引擎 + 全部上下文属性 / 控制器 / image provider 就绪 |
| `qml-loaded` | engine->load() 返回 | QML 编译 + 整棵树实例化完成 |
| `surface-bound` | bindSurface 返回 | ComparisonSurface 找到并接上 D3D11 桥 |
| `show-enter` | window->show() 调用前 | show 之前的装配代码结束 |
| `sg-initialized` | QQuickWindow::sceneGraphInitialized（渲染线程，DirectConnection） | 场景图 / RHI / D3D11 设备就绪 |
| `post-show` | window->show() 返回 | show() 全部完成 |
| `window-shown` | raise/activate/update 之后 | 进入事件循环前的最后一步 |
| `qml-load-returned` | desktop.load() 返回 runDesktop | 装载阶段结束 |
| `startup-request` | applyStartupRequest 返回 | 启动请求已提交（空启动也标记相位） |
| `exec` | desktop.exec() 调用前（含 trace 安装） | 事件循环起点 |

## 基线表（release，热缓存，中位 / P95，单位 ms）

| 段 | 含义 | 无文件 | 带单视频 |
|---|---|---|---|
| (外部) spawn → 窗口 | 用户看到主窗口 | **660.5 / 740.2** | **676.4 / 795.9** |
| (对齐) spawn → runDesktop-enter | 进程创建 + CRT + DLL 装载 | 22.7 / 24.1 | 19.8 / 20.6 |
| runDesktop-enter → graphics-backend | | 0.6 / 0.6 | 0.6 / 0.6 |
| graphics-backend → qguiapplication | QGuiApplication 构造 | 23.9 / 24.7 | 22.6 / 27.3 |
| qguiapplication → broker | 参数解析 + 单实例选举 | 2.2 / 2.5 | 2.2 / 2.6 |
| broker → runtime-create | ReviewRuntime 装配 | 0.9 / 0.9 | 0.8 / 1.0 |
| runtime-create → load-enter | | 0.4 / 0.5 | 0.5 / 0.6 |
| load-enter → context-ready | 引擎 + 上下文 + 控制器 | 2.2 / 2.3 | 2.2 / 2.4 |
| context-ready → qml-loaded | **QML 编译 + 整树实例化** | **577.3 / 646.6** | **608.2 / 713.5** |
| qml-loaded → surface-bound | findChild + surface 绑定 | 0.7 / 0.8 | 0.8 / 1.0 |
| surface-bound → show-enter | show 前装配代码 | 0.2 / 0.3 | 0.2 / 0.2 |
| show-enter → sg-initialized | **show() 内等待场景图/RHI/D3D11 初始化** | **196.2 / 245.0** | **204.2 / 223.7** |
| sg-initialized → post-show | **SG 就绪后 show() 内剩余部分** | **62.8 / 73.2** | **63.0 / 69.6** |
| post-show → window-shown | raise/activate/update | 0.6 / 0.8 | 0.5 / 1.2 |
| window-shown → qml-load-returned | load() 返回 | 0.1 / 0.2 | 0.1 / 0.1 |
| qml-load-returned → startup-request | 启动请求提交 | 0.2 / 0.3 | 2.1 / 2.3 |
| startup-request → exec | trace 安装 | 0.1 / 0.2 | 0.1 / 0.2 |
| (对齐) spawn → exec | 进事件循环（可交互） | **888.6 / 1022.3** | **928.6 / 1062.5** |

数据目录：`out/startup-measurements/baseline2-nofile-20260927-225405/`、
`out/startup-measurements/baseline2-onefile-20260927-225434/`。

## 归因与决策门（Step 1 完成）

进事件循环前的 ~890 ms（无文件，内部钟）拆解为：

| 归属 | 中位 ms | 占比 | 说明 |
|---|---|---|---|
| 进程装载（spawn→runDesktop-enter） | 23 | 3% | CRT + DLL |
| QGuiApplication 构造 | 24 | 3% | Qt 平台插件 |
| 装配杂项（broker/runtime/上下文/绑定/尾部） | ~8 | 1% | 已无归因价值 |
| **QML 编译 + 整树实例化** | **577** | **65%** | Step 2/3/4 的目标 |
| **show() 阻塞（等待场景图初始化 + SG 后剩余）** | **259** | **29%** | 见下 |

1. **决策门：QML 段 577 ms（P95 647）≫ 100 ms。
   Step 2（`qt_add_qml_module` 预编译）全量执行，不降级；Step 3 / Step 4（延迟加载）
   同样全量执行，不做"只弹窗子集"的收缩。**
2. **show() 段归因（本轮细打点完成）**：show 之前代码 0.2 ms；`window->show()` 内部
   先阻塞 ~196–204 ms 等渲染线程完成场景图初始化（RHI 后端 + D3D11 设备创建 + DXGI
   适配器枚举；本机 2 个虚拟显示适配器很可能放大此成本），SG 就绪后 show() 内再走
   ~63 ms（疑为首次同步/首帧准备），随后 raise/activate/update 仅 ~0.6 ms。
   窗口在外部 ~660 ms 已可见，但场景图 ~828 ms 才就绪、事件循环 ~889 ms 才起步——
   **存在 ~230 ms 的"可见但冻结/空白"窗口期**（体验维度候选改进项）。
3. **对 Step 5 的验证**：启动请求目前提交于 show() 之后（带文件 ~910 ms，内部钟）；
   把提交提前到 surface-bound 后（~637 ms）即可让媒体打开与 259 ms 的 GPU 初始化段
   并行——计划预估的 ~250 ms 收益与本段实测吻合。
4. 带文件与无文件结构一致；启动请求本身仅 ~2 ms，媒体打开完全发生在 exec 之后。

## Step 2（QML 预编译）实验结论：取消（2026-09-27 第二轮实测）

按计划把 QML 资源从 qrc 迁到 `qt_add_qml_module`（`TARGET_PATH "qml"` + 每文件
`QT_RESOURCE_ALIAS`，保住 `qrc:/qml/<file>` 全部 URL；独立静态库目标承载生成代码以避开
/W4 /WX 对生成代码的 C4702；架构表加两条），UI 套件 766/767 全绿，机制完全打通。两个实测：

1. **NO_PLUGIN 静态库的 cachegen payload 会被链接器丢弃**：exe 仅 +2 KB，引擎静默回退到
   源码资源（此状态 QML 段 561.3 ms ≈ 基线 577.3 ms，纯噪声）。必须在模块库之外的翻译单元
   显式 `Q_INIT_RESOURCE(qmlcache_dvs_ui_qml_shell)` 才能把 ~2.9 MB 编译单元拉进 exe。
2. **链入缓存单元后 QML 段 582.6 ms vs 基线 577.3 ms——零收益，exe +2.9 MB。**
   561 KB QML 的解析+字节码编译本身只值 ≤20 ms；该段由**整树实例化**主导（数百个对象的
   C++ 构造 + 属性初始化 + 绑定装配）。qmlcachegen 只省掉前者。

按决策门（"QML 编译段 <80 ms → Step 2 取消"）回退全部 Step 2 改动，回到 `0df097d` 状态。
本轮保留：构建脚本对空缓存值的健壮性修复；本节的归因修正。修正后的杠杆排序：

- **Step 3/4（延迟加载）是 577 ms 段唯一真正的削减手段**——少实例化对象；
- Step 5（启动请求提前）的 259 ms GPU 初始化段并行收益不受影响，预估仍 ~250 ms；
- 术语修正：context-ready→qml-loaded 段不应再称"QML 编译段"，应称"QML 装载+整树实例化段"。

实验数据：`out/startup-measurements/step2-nofile-*`（payload 未链入）、
`out/startup-measurements/step2b-nofile-*`（链入后，exe 5191 KB）。

## Step 3（延迟加载 A：弹窗/对话框）完成（2026-09-28 同日 A/B 实测）

前置的 objectName 断言清单结论（清单先于改动，决定实施范围）：

| 组件（急切对象数约计） | 测试断言时机 | 处置 |
|---|---|---|
| ClipExportDialog（~50） | 测试只在点击导出 chip **之后** findChild | 整体 Loader 化 |
| ReviewInputDialogs + DropConfirmationDialog（~50） | **无** Main.qml 级断言（QML 测试自建实例） | 整体 Loader 化 |
| ShortcutHelpOverlay（~35） | 仅断言**根** objectName 与根属性 | 根 Popup 常驻，内容表 Loader 化 |
| ReviewContextMenu（~15） | 契约测试 `InstantiatesRootAndSeparatesManualAlignmentStates` 装载即断言 7 个子 objectName | **维持急切**（见下） |

实现（`Main.qml` + `ShortcutHelpOverlay.qml`，零测试改动）：

- 两个 `Loader { active: false }` + `Component` 包装；打开路径先 `active = true` 再
  `open()`（Loader 同步建项，首次打开付一次实例化，之后常驻，与原行为等价）。
  `inputContext` 改读 `Boolean(reviewInputDialogs && reviewInputDialogs.modalVisible)`；
  导出失败重开走同一入口并加空项守卫。
- `Loader.item` 对 qmllint 只是 QObject：所有成员访问经
  `readonly property <类型> x: loader.item` 类型化视图（声明行带定向
  `qmllint disable incompatible-type`——linter 看不见 Loader 实例化的具体类型——
  下游访问保持类型检查）。
- ShortcutHelpOverlay：根 Popup 常驻（objectName / `visible` / preset 属性继续满足
  装载即断言），`contentItem` 换 `Loader { active: opened || contentReady }`，
  首次打开后常驻。

同日背靠背 A/B（同机、无文件、release、5 轮 + 1 预热）：

| 段（中位 / P95，ms） | 基线（同日） | Step 3 | Δ |
|---|---|---|---|
| context-ready→qml-loaded | 577.8 / 589.9 | 562.7 / 572.7 | **−15.1 / −17.2** |
| spawn→exec（内部钟） | 850.6 / 890.5 | 832.5 / 840.6 | **−18.1 / −49.9** |
| spawn→window（外部） | 633.8 / 650.7 | 622.1 / 637.2 | −11.7 / −13.5 |

- **教训（跨日对比陷阱）**：昨日基线的 sg-init 段 196 ms 在今日基线为 171 ms（环境漂移），
  而 QML 段跨日稳定（577.3 / 577.8）。**归因一律以同日 A/B 为准**，跨日数字只作参考。
- **每对象实例化成本 ≈ 0.11 ms**（~135 个延迟对象 / 15.1 ms）→ ReviewContextMenu
  ~15 个对象 ≈ 2 ms，低于噪声底（±5 ms），不值得为此改契约测试的装载即断言。维持急切。
- 门禁：受影响测试全绿（MainQmlContractTests、DropConfirmationDialog 缩放、app 冒烟）；
  `qmllint --max-warnings 0`、`format-check`、`lint` 通过；随后全量套件复核。
- 数据：`out/startup-measurements/step3-ab-baseline-nofile-20260928-014103`（基线 A）、
  `out/startup-measurements/step3-nofile-20260928-013922`（Step 3 B）。

对 Step 4 的含义：剩余 ~563 ms 段里 ImageWorkspace（`Main.qml` L2751，仅 `visible:`
门控）与 TabbedInspector（L2637，同）仍是急切实例化——两件的断言都发生在切换到
图像工作区/打开检查器**之后**（`imageWorkspaceRoot`、`tabbedInspector` 系列），
契约面比本批更宽松；按 0.11 ms/对象外推是下一个主要削减点，清单方法照搬本节。

## Step 4（延迟加载 B：ImageWorkspace + TabbedInspector）完成（2026-09-28 同日 A/B 实测）

清单结论（契约面比 Step 3 深得多，两件用了**不同**的延迟结构）：

- **ImageWorkspace（2874 行，~150 objectName）**：多个测试在**未激活**工作区的状态下就
  findChild 其子树（#691 直接查 `primaryViewport`，#696/#699/#707 查 `imageWorkspaceRoot`）。
  → 整体 Loader 化（`active: imageWorkspaceActive || keepActive`，图片启动时在初始绑定
  求值中照常构建）+ 4 处测试改为先经 `activateWorkspace` 激活再查找。
- **TabbedInspector（774 行）**：#669 装载即断言根 objectName；`DrawerScrim…` 测试用其
  `parentItem()` 作布局探针并依赖闭合的锚定链。→ **组件内内容延迟**：根 Rectangle
  （objectName / 几何 / 父子链逐字节不变）常驻，TabBar+StackLayout 全部包进
  `Loader { active: visible || contentReady; anchors.fill: parent }`，`effectiveTab` 经
  内容项转发（关闭状态下报 Review 页）。#669 仅把 5 个检查器子项
  （setIn/setOut/clear/loop/mediaInfoRepeater）的查找移到首次打开之后；DrawerScrim 零改动。

本轮实测踩出的两个 Qt 行为（都有逐项证据，后续任何懒加载都适用）：

1. **Loader 不会把已载入项压到自身尺寸**：ImageWorkspace 根无自身几何，包进 Loader 后
   整棵树 0×0——侧栏高度塌成负数、`folderPairList` contentY=-78。修法：包装内显式
   `anchors.fill: parent`。
2. **positioner 的重排是帧驱动的**：懒创建的 Flow 在宽度 0 状态完成首次布局后，后续宽度
   变化（chrome 边距翻转、窗口 resize）在**没有渲染帧的窗口里永远不会触发重排**（实测
   事后 resize 也无效）——命令面板保持竖排 734 px、侧栏 56 px。`forceLayout()` 可同步
   修复 Flow（实测 296→30）；Column 无此方法，其重排同样只在下一帧。修法：4 个 Flow
   `onWidthChanged: forceLayout()` + 完成时统一 forceLayout。**真实应用始终渲染，激活后
   首帧即自愈**；同步 force 的意义是让无渲染环境也确定。
3. **契约测试环境渲染 0 帧**（`frameSwapped` 计数实测为 0，即使 `show()` + 秒级事件泵）：
   一切帧驱动行为在该环境都不发生——#695 因此改为在 folder 流程前先激活空工作区（创建
   发生在布局仍稳定时），并如实注明原因。

同日背靠背 A/B（无文件、release、5 轮 + 1 预热；A = Step 3 提交 `fcd92be`）：

| 段（中位 / P95，ms） | A（Step 3） | B（Step 4） | Δ |
|---|---|---|---|
| context-ready→qml-loaded | 549.7 / 552.7 | 521.4 / 525.5 | **−28.3 / −27.2** |
| spawn→exec（内部钟） | 803.2 / 814.2 | 765.3 / 770.6 | **−37.9 / −43.6** |
| show-enter→sg-initialized | 153.3 / 154.3 | 153.0 / 158.8 | 无漂移（对照组） |
| sg-initialized→post-show | 64.4 / 65.6 | 50.2 / 50.6 | −14.2（更小的首同步，疑真实现） |

- **首开成本**：移出启动路径的实例化工作 ≈ 42 ms（QML 段 28 + 首同步 14），在首次激活
  图片工作区 / 首次打开检查器时一次性支付，之后（keepActive）免费——远低于可感知阈值。
- **Step 3+4 累计**：QML 段 577.3（9-27 基线，跨日稳定段）→ 562.7 → **521.4**
  （**−55.9 ms，−9.7%**）；同日链验证的端到端收益 −18.1 + −37.9 = **−56.0 ms**。
- 门禁：全量 767/767、`qmllint --max-warnings 0`、`format-check`、`lint` 全绿。
- 数据：`out/startup-measurements/step4-nofile-20260928-032720`（B）、
  `out/startup-measurements/step4-ab-baseline-nofile-20260928-032809`（A）。

## Step 5（启动请求提前）实验结论：架构性取消（2026-09-28 实测 + 首帧层落地）

按计划先补齐首帧层：`measure-startup.ps1` 现在为每轮设置 `DVS_PLAYBACK_TRACE`，解析首个
kind 0（CommandAccepted）/ 4（FrameSetReady）/ 8（SnapshotCommitted）事件并与 spawn 钟对齐
（trace 与里程碑同用 steady_clock，零偏移拼接）。然后把启动请求提交移到 show() 之前
（`DesktopApplication::load` 增加 pre-show 钩子）实测：

1. **pre-show 提交被应用层拒绝**：`ReviewController::openSources` 的门是
   `canOpen = graphicsReady && !busy && …`，而 graphicsReady 依赖场景图初始化创建的
   D3D11 设备——show() 之前恒为 false。实测：无 startup-request 里程碑、无 kind 0 事件、
   应用走致命退出路径（错误弹窗挂住直到被杀，exit -1、14/16 里程碑），与代码路径吻合。
2. **"排队到 graphics-ready 再提交"也拿不到收益**：主线程在 show() 内阻塞到 ~749 ms，
   任何主线程机制（含意图队列冲刷）最早也只能在 post-show 处理——而当前提交点就在那里
   （750.5 ms）。真正的并行需要 worker 侧无设备探测（probe 先行、解码器开箱等设备），
   属设备生命周期架构改造——计划明确的非目标。
3. **计划预估的 ~250 ms 并行收益架构上不可得**。首帧层给出的真实串行链：窗口可见
   588.8 → exec 750.7 → 命令接受 793.9 → **首帧提交 832.3 ms**（中位）；打开本身仅
   ~38 ms（接受→首帧），GPU 段（154 ms）与打开按设计顺序执行。

**保留产出：首帧测量层 + 带文件基线**（四格完成定义"启动前后对照"的带文件数据来源，
Step 6 亦复用）：

| 指标（带文件，中位 / P95，ms） | 值 |
|---|---|
| spawn → 窗口可见 | 588.8 / 590.9 |
| spawn → exec | 762.7 / 777.8 |
| spawn → 命令接受（kind 0） | 793.9 / 810.0 |
| spawn → 首帧集就绪（kind 4） | 832.0 / 852.0 |
| spawn → 首帧提交（kind 8） | **832.3 / 852.2** |
| "可见但空白"窗口（窗口→首帧） | ~243 |

- 应用侧改动全部回退（pre-show 钩子随之移除——无提交者即死代码）；测量脚本保留。
- 含钩子版本曾全量 767/767 通过（冒烟即真实带文件启动路径），回退仅撤实现、不改结论。
- 数据：`out/startup-measurements/step5-cancelled-onefile-20260928-035731`（正常流程基线）、
  `out/startup-measurements/step5-onefile-20260928-035027`（pre-show 被拒的失败证据）。

对 Step 6 的含义：本基线"接受→首帧"仅 38 ms（暖缓存、小文件）——首开软解竞争若存在，
需要更大码流/冷缓存才能显形；trace 的 kind 23/24（解码起止）已可取数。

## Step 7（冗余清理）完成：6/7 批执行，1 批按证据拒绝（2026-09-28）

每批独立提交、独立回滚；全量 767/767 + qmllint/clang-tidy/format-check 全绿贯穿。

| # | 批次 | 提交 | 删除/净变化 | 安全证据 |
|---|---|---|---|---|
| 2 | stb 解码器移除 | `a2569f5` | **−8100 行**（276 KB 三方头 + 59 行包装），exe −58 KB | 生产链 FFmpeg 优先（main 注释明言插件缺 qjpeg）；stb 仅在 FFmpeg 失败后可达；测试零 stb 断言；PNG/BMP/PNM 为 QtGui 内建 |
| 1 | ImageWorkspace 几何 ×3 | `0948b82` | 净 −15 行，fit/trueSize/draw-rect + 跑马框单源 | 113 项缩放/wipe/fade/跑马框契约测试；绑定值恒等 |
| 5 | 设置枚举映射 | `9f913a6` | 净 −34 行，读写共用 NamedEnumOption 表 | 读（有效/无效回退）写（逐字符串断言）双向钉死的偏好持久化测试 |
| 7 | 主题字面量 | `7441658` | 5 处语义色转 Token | 全量字面量审计：仅 15 处命中、10 处为正当字面量（白色背景/画笔/保存填充、TransportBar 自持调色板） |
| 4 | 持久化解析阶梯 | `ed214a9` | 净 −14 行，20 处 contains+is_X 链 → 3 个类型化读取器 | IssueRecord 解析往返测试（身份匹配/缺失报告/schema 拒绝）22 项 |
| 6 | QML 标签孪生 | `81f5bec` | 净 +7 行，但消除**已发生的漂移**（URL 解码 vs 裸路径） | SameNamed 父标签 + 文件夹侧栏/图像标题契约测试 92 项 |
| 3 | PlaybackCoordinator ~30 行 | **拒绝** | 0 | 唯一候选（三处 RenderPublished 发射点）跨两种帧表示（shared_ptr vs 值），助手比重复更长；4700 行不变量最密的文件不值得为此冒险 |

原计划的行数估计普遍偏乐观（如批次 1 估 ~150 实际净 −15；批次 6 估 ~65 实际净 +7），
但每批的核心价值（单源化防漂移、死代码删除、测试钉死的行为等价）均已兑现；
拒绝批次 3 同样是证据驱动的决定。

## 终版对照（v2.0.0 发布测量，2026-09-28）

发布构建（2.0.0，含全部 Step 7 清理）复测，与 9-27 基线对照；Step 7 对启动无影响
（QML 段 518.2 vs Step 4 后的 521.4，噪声内）：

| 指标（中位 / P95，ms） | 1.9.0 基线（9-27） | v2.0.0（9-28） | Δ 中位 |
|---|---|---|---|
| QML 装载+实例化段（无文件） | 577.3 / 646.6 | **518.2 / 524.4** | **−59.1** |
| spawn→窗口可见（无文件） | 660.5 / 740.2 | 579.9 / 597.9 | −80.6 |
| spawn→事件循环（无文件） | 888.6 / 1022.3 | 757.9 / 774.1 | −130.7 |
| spawn→首帧提交（带文件，kind 8） | （无首帧层） | 835.1 / 842.7 | 新基线 |
| 包体积 | 26.63 MiB / 310 条目 | 26.60 MiB / 310 条目 | −34 KB |

同日链验证的核心收益 −56.0 ms（Step 3 −18.1 + Step 4 −37.9，exec）；跨日差值含环境
漂移（GPU 段日间波动 ~25 ms）。数据：`out/startup-measurements/v200-final-{nofile,onefile}-*`。

## Step 6（首开软解竞争诊断）结论：未观察到（2026-09-28 实测）

计划的假设是：首次打开时解码器 `tryLease()` 可能发生在场景图 `adoptQtDevice()` 之前，
撞上 Busy/Unavailable 而**静默回退软解**（`SoftwareDecoder::open` 的 fallbackReason 路径）。

代码路径分析 + 两组实测（临时在开箱收尾处打印 backend/fallbackReason，跑完即回退）：

- **结构上不可能竞争**：解码器打开的前提是 `canOpen = graphicsReady && …`（Step 5 已证），
  而 graphicsReady 依赖 `adoptQtDevice`（sg-initialized，~700 ms）；打开发生在 ~800 ms
  （kind 0 之后）。租约窗口（adopt 持锁的微秒级片段）与打开时刻相隔 ~100 ms。
- **单源首开**：主解码器 + 精确解码器均 `backend=d3d11va fallback=""`；trace 无 kind 11
  （DecoderReopen，即无代际竞争重开）。
- **三源并发首开**（竞争最强场景：6 个解码器同时开箱 + 渲染器活跃）：全部
  `backend=d3d11va`、零 fallbackReason、零重开。

按计划决策门（"确认不了就不动"）：**不改代码**，发布说明记"未观察到"。诊断数据：
`out/startup-measurements/step6-diag*.{stderr.txt,trace.jsonl}`（单源/三源）。

## 已知限制与待办

1. **冷启动未测**：本表全部为热缓存。冷启动协议待定义（重启后首轮即测、不预热）。
2. **~~首帧层未接入~~ 已接入（2026-09-28）**：`measure-startup.ps1` 解析
   `DVS_PLAYBACK_TRACE` 的首个 kind 0/4/8 事件并与 spawn 钟对齐；带文件基线见 Step 5 节。
3. P95 = 5 轮最大值，样本小；结论以中位数为主。
4. dev 构建数据只用于机制验证（QML 段 1570 ms），不进基线表。
5. Step 0/1 已提交；全量 dev 套件 767/767 通过（其中 `quality.release-contract` 的存量
   版本串不同步——README 停在 1.7、vcpkg.json 停在 1.7.0——已同步到 1.9.0 修复）。
   vcpkg manifest 版本变化会在下一次 configure 时从二进制缓存重装一次依赖。
6. **~~本机 MSVC 14.44 英文语言资源缺失~~ 已修复（2026-09-28）。** 原始缺陷：工具集
   `bin/Host*/<arch>/` 下只有 `2052/`，而 `cl.exe` 只在 `1033/` 存在时才输出英文，因此
   `VSLANG=1033` 完全失效（逐字比对过：VSLANG=1033 与 2052 的诊断文本一致）。此时 cl 经管道
   输出**中文 GBK 字节**（注意：与"CMake 把 UTF-8 解成乱码"的说法相反——`cl.exe` 在**管道**上
   输出的是 GBK，只有重定向到**文件**时才是 UTF-8），CMake 4.4.0 按控制台代码页解码后再以
   UTF-8 写入 `CMakeFiles/rules.ninja` 的 `msvc_deps_prefix`，于是 Ninja 匹配不到
   `/showIncludes` 行 → 新编译对象全部没有头文件依赖记录 → `quality.msvc_dependencies` 红灯，
   且头文件改动不再触发重编。此前未暴露是因为增量构建沿用 `.ninja_deps` 里的历史记录；任何
   全量重编都会复现（实测 368 个对象中 360 个 `#deps 0`）。
   注意 **`deps = msvc` 本身始终存在**（184 条，位于 `CMakeFiles/rules.ninja`，不在
   `build.ninja`），坏掉的只有前缀值——按"缺少 deps = msvc"排查会走错方向；也**不要**指望
   `-Fresh` 修复它，那只会白跑一次全量重编。
   修复方式：VS Installer 没有"MSVC 语言包"组件（`--add …LangPack.en-US` 会返回 87）
   ——MSVC 的诊断资源是按语言拆分的独立资源包（`Microsoft.VC.<ver>.<vs>.Tools.Host*.Target*.Res.base`
   ，如 `en-US`、`zh-CN`），每个仅约 225 KB，只含该语言的 8 个 `*ui.dll`。用
   `tools/build/InstallMsvcEnglishResources.ps1`（需管理员）从 installer 自身缓存的频道清单
   取这 4 个 en-US 包、校验 SHA256 后解压到工具集。
   修复后实测：`msvc_deps_prefix` 与 cl 实际输出（`Note: including file: `）逐字节一致；
   全量重编后 368 个对象中 362 个有健康记录（其余 6 个为门禁不检查的构建期工具）；
   5 个门禁对象 `#deps` 分别为 7/31/191/170/343；`quality.msvc_dependencies` 与
   `quality.msvc_dependency_contracts` 通过；改动一个 domain 头文件触发 124 步重编、30.5 秒
   （修复前会静默不重编）。`build.ps1 -Doctor` 与每次构建开头的环境自检会持续守卫该契约：
   逐字节比对 cl 实际输出与生成规则，不合即报错并给出修复指令。
7. **~~本机 vcpkg 根无 git 库~~ 已修复（2026-09-28）。** 原始缺陷：`G:\Workspaces\vcpkg` 是
   zip 解压版、没有 `.git`（而 `G:\.git` 是无效仓库），builtin registry 却通过 git 读取端口与
   baseline，所以任何 manifest 指纹变化触发的依赖重解析都会失败（实测报错
   `--git-dir "G:\.git" read-tree … failed` 与 `failed to git show versions/baseline.json`）。
   修复：为该根补上 microsoft/vcpkg 的真实 git 仓库（全量克隆实测 74 秒 / 142.8 MB，本机保留
   121.8 MB 的 `.git`），并为 `vcpkg-configuration.json` 声明的 baseline commit
   `e6ed7c5b…` 建 `refs/dvs/baseline`，避免将来 `git gc` 把它当悬空对象清理。
   注意**必须用全量克隆**：`--filter=blob:none` 的 blobless 克隆虽然只要 7.5 MB，但缺 blob 时
   会按需联网（本机报 `getaddrinfo() thread failed to start` + `could not fetch … from
   promisor remote`），把"明确失败"变成"不可预测地联网后失败"，是这个环境下更糟的状态。
   验证：`vcpkg install --dry-run` 从 fail(exit 1) 变为完整解析出依赖图（exit 0，17.8 秒，
   ffmpeg 8.1.2#3 / qtbase 6.11.1 / gtest / nlohmann-json 等），且无任何 git 或网络错误。
   本地构建**仍推荐** `build.ps1 -UseInstalledDependencies`（`VCPKG_MANIFEST_INSTALL=OFF`，
   直接用已装好的 out/vcpkg 树，实测 12.48 GB / 157 个包，依赖齐全）——它更快也更确定；
   但现在这是**性能选择而非唯一出路**，manifest 变更时多了一条可用路径。
8. **cpack 体积差异已查清，不是管线问题，也不需要修复。** `cmake/Install.cmake` 的
   `InstallRequiredSystemLibraries` 只安装 CRT/UCRT DLL，CMake 4.4.0 的该模块全文不含任何
   `.exe` 安装路径；`out/build/release/cmake_install.cmake` 中 `vc_redist` 出现 0 次，仓库
   代码也从不引用它。59.77 MiB 那个包（`out/packages/CompareStation-1.9.0-pre-step2.zip`）
   多出的 24.45 MB `vc_redist.x64.exe` 是**从别处手工拷入 staging 的一次性产物**（同一字节数的
   副本还留在 `out/package/zip/VCStation-1.6.0-windows-x64/` 与 `out/migration/…`）；
   它同时还多了 `dxcompiler.dll`(14.3 MB) 与 `d3dcompiler_47.dll`(4.7 MB)，故
   59.77 − 24.45 = 35.3 而非 26.63。正常 `-Preset release` + `cpack --preset release-zip`
   产出的 26.63 MiB 才是基线。原第 8 条"疑似 CMake 版本行为差异"的结论作废。
   **实测复核（2026-09-28）**：干净跑
   `build.ps1 -Preset release -UseInstalledDependencies` 后 `cpack --preset release-zip`，
   产出 `out/package/zip/CompareStation-1.9.0-windows-x64.zip` =
   **26.63 MiB / 310 条目 / 解压 66.42 MiB / 无 vc_redist**，与基线逐项吻合
   （27919730 vs 27923994 bytes，差 0.004 MiB 属正常构建抖动）。体积维度可直接用于前后对照。
