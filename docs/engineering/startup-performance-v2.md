# 启动性能基线与归因（v2.0 计划 · Step 0/1）

更新日期：2026-10-04（追加「Step 8 · v2.1.0 当前 SHA 重测」一节）。测量对象：`main @ 8ffa577` + 工作区 Step 0/1 启动埋点改动（未提交）。
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
| `startup-request` | 启动请求被**接受**（dispatcher 提交成功，可能在窗口可见之后） | 启动请求已提交（空启动也标记相位） |
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
   **2026-09-28 修正**：致命退出是当时「被拒即无路可走」的结果，不是必然的失败模式。
   `StartupRequestDispatcher` 现在按 50 ms 重试到 10 s 上限，只在会话就绪后仍被拒才算失败，
   且失败只记日志不再退出（详见 `docs/engineering/visual-review-backlog.md` 同日条目）。
   这条记录描述的是当次实验的观测，不是产品现状。
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

## Step 8 · v2.1.0 当前 SHA 重测（B0，2026-10-04）

上文所有表格都绑定 `8ffa577`（v2.0.0 时期）。Step 3/4/5 之后的代码没有再测过，
「830 ms + 230 ms 空白窗」这两个数字属于 **v2.0.0 时代**，不是产品现状。本节把基线
重新绑定到发布 tag。

- **SHA**：`267adca6114c912796465944a51468598b8f0762`（tag `v2.1.0`），工作树在
  `src/` 上无改动（仓库根有 5 个与本轮无关的未跟踪文件，测量脚本计入 dirty 条目）。
- **构建**：`build.ps1 -Preset release -UseInstalledDependencies` 增量，**零编译目标**
  （只重新生成 dyndep），exe 与该 SHA 一致。
- **环境**：与 2026-09-27 基线同一台机器（i7-13700KF / RTX 4090 / 2 个虚拟显示适配器）。
- **协议**：同脚本、同 release、同素材 `frameid_1080p60_a.mp4`，5 轮 + 1 预热，热缓存。
- **数据**：`out/startup-measurements/b0-baseline-{nofile,onefile}-20261004-1210{53,130}/`。

### 关键指标（中位 / P95，ms）

| 指标 | 无文件 | 带单视频 |
|---|---|---|
| spawn → 窗口可见（外部 1 ms 轮询） | 645.8 / 660.2 | 643.0 / 687.8 |
| spawn → 事件循环（exec，可交互） | 872.9 / 889.7 | 874.3 / 933.0 |
| spawn → 命令接受（kind 0） | — | 906.5 / 967.7 |
| spawn → 首帧集就绪（kind 4） | — | 949.4 / 987.5 |
| spawn → 首帧提交（kind 8） | — | **949.9 / 990.6** |
| **"可见但空白"窗口（窗口 → 首帧提交）** | — | **~307** |

**结论 1：现状比 830/230 更差。** 当前 SHA 首帧提交 949.9 ms、空白窗 ~307 ms；
v2.0.0 记录为 835.1 ms / ~243 ms。但按本文档 L178 的教训（环境日间漂移可达 ~25 ms），
**跨月数字不能直接判定为回归**——`show-enter → sg-initialized` 在 153–196 ms 间波动即为例证。
要判定 v2.1.0 是否真的比 v2.0.0 慢，必须重跑 `8ffa577` 的同日 A/B；本轮不做该结论。

**结论 2：出现两段此前从未被单独归因的开销**（v2.0.0 表中不存在或仅有 0.2 ms）：

| 段 | v2.0.0 记录 | 当前 SHA（中位 / P95） | 说明 |
|---|---|---|---|
| `surface-bound → show-enter` | 0.2 / 0.3 | **23.4 / 25.0** | `DesktopApplication.cpp` 里 `setGraphicsConfiguration`、`preferHighRefreshScreen` 的 `QGuiApplication::screens()` + `setScreen/setPosition`、`applyWindowsNativeChrome` |
| `qml-load-returned → shell-registration` | （里程碑不存在） | **17.8 / 18.3** | `Main.cpp` 启动时的资源管理器命令自愈注册 |

两段合计 **~41 ms**，位于 QML 装载之后、事件循环之前，属于既有串行路径上的新增可见成本，
比继续在 QML 段做延迟加载更便宜可摘。两者都必须先做 A/B 归因再改，不得直接删。

### B0c/B0d · 两段新增开销的细粒度归因（同日，只加埋点不改行为）

在 `surface-bound → show-enter` 之间补三个里程碑（`graphics-config`、`screen-select`、
`native-chrome`），确认 `surface-bound → graphics-config` = 0.2 ms、
`graphics-config → screen-select` = 0.1 ms、`native-chrome → show-enter` = 0.2 ms——
**23.4 ms 全部落在 `applyWindowsNativeChrome()` 这一个函数里**。再把该函数内部拆成
`native-hwnd`（`window->winId()` 之后）与 `native-attrs`（四次 `DwmSetWindowAttribute`
之后）两段，得到决定性结论：

| 段 | 中位 / P95（ms） | 归因 |
|---|---|---|
| `screen-select → native-hwnd` | **22.4 / 23.3** | `window->winId()` 在 `show()` 之前强制物化 HWND |
| `native-hwnd → native-attrs` | 0.2 / 0.3 | 四次 DWM 属性设置，可忽略 |

即**代价不是 DWM 属性，而是「提前造窗口」**。数据：
`out/startup-measurements/b0c-attrib-nofile-20261004-121429/`、
`b0d-chrome-split-20261004-121537/`。

### 本轮实施的两处修复与一处**被实测否决**的修复

1. **【否决并已回退】把 `applyWindowsNativeChrome` 移到 `show()` 之后。**
   假设是"那 22 ms 只是提前物化，挪回去就是净赚"。同日 A/B **证伪了这个假设**：

   | 段 | 修复前（B0d） | 修复后（B0-fix） |
   |---|---|---|
   | `screen-select → native-hwnd`（造 HWND） | **22.4** | （该段已不存在） |
   | `show-enter → sg-initialized` | **168.1** | **213.9 / 235.2** |
   | `post-show → native-hwnd` | （不存在） | **0.2** |
   | spawn → 窗口可见（无文件） | 642.8 | **689.5**（更晚） |

   代价没有消失，只是从 `show()` 之前搬进了 `show()` 内部，而且搬进去的比搬出来的更多
   （+46 ~ +67 ms vs −22 ms）。`post-show → native-hwnd` 只有 0.2 ms 是决定性证据：
   **`show()` 本来就会建 HWND**，提前建并不会替它省掉任何工作。
   窗口出现反而更晚，且引入标题栏晚一帧生效的闪白风险。**已按证据回退，保留原有位置**，
   代码注释里写明这次否决，避免下一个人再犯同样的改动。

2. **【保留】Explorer 自愈注册改为 worker 线程。** `ensureRunningInstallationRegistered`
   原本 ~18 ms 同步注册表工作卡在「首帧已呈现」与「`exec` 起步」之间。现在由 `std::jthread`
   执行，调用、修复规则、失败只记日志的语义不变，`runDesktop` 返回时 join。
   **口径变更**：`shell-registration` 里程碑现在测的是线程派生（约 0.1–0.2 ms），不再测注册耗时；
   注册本身的正确性由 `shell_windows.ExplorerCommandRegistrationTests` 与
   `tools/shell/Test-RegisterCompareStationContextMenu.ps1` 负责，不由启动测量负责。

   这条收益是**同一份运行内的分段对比**，不受跨轮环境漂移影响：
   `qml-load-returned → shell-registration` 由 **18.0 ms → 0.1/0.2 ms**。
   注意：同轮的 spawn→exec 总数不可直接对比（该轮整体更慢，`context-ready → qml-loaded`
   从 560.6 漂到 600.6），所以**只按段认收益，不按总数认收益**。

3. **保留细粒度里程碑**（`graphics-config`、`screen-select`、`native-hwnd`、`native-attrs`、
   `native-chrome`）。它们只在 `DVS_STARTUP_TIMING` 设置时输出，平时零开销，
   且是上面那次否决的唯一依据。`b1-fixed-*` 是被否决版本的数据，**保留作为反例**。

**结论 3：主项结构未变。** `context-ready → qml-loaded` = 560.6 ms 仍是最大单项
（v2.0.0 为 518.2，跨月不作结论）；`show-enter → sg-initialized` = 170.0 ms、
`sg-initialized → post-show` = 52.3 ms 落在历史区间内。Step 5「启动请求提前架构性取消」
的结论仍然成立：`startup-request` 里程碑仍在 `window-shown` 之后。

### Step 8 探针结果 · 延迟加载这条路**不成立**（2026-10-04 同日 A/B）

计划里 Step 8 想「把重组件改延迟加载，砍掉 `context-ready → qml-loaded` 的 550 ms」。
同日 A/B 把它否掉了，数据如下（同一天、同一脚本、warm cache、5 轮 + 1 warmup）。

**实验一 · 摘掉最大的那个组件。** `ImageWorkspace.qml` 是全仓最大的 QML（148.6 KB），
启动时已经用 `Loader` 懒实例化，但内联 `Component { ImageWorkspace { … } }` 在**编译期**
就要解析该类型。把整个 Component 体换成 `Item {}` 再测：

| 变体 | `context-ready → qml-loaded`（中位 / P95） |
|---|---|
| 去掉 `ImageWorkspace`（含 FolderPairSidebar / ImageEditDialogs 的连带编译） | 551.5 / 569.1 |
| 原样 | 563.8 / 604.5 |

差 ~12 ms，落在该段运行间漂移内。**「提前编译大组件」不是这 550 ms 的成因。**

**实验二 · 引擎与 import 预热占多少。** 在 `engine->load()` 前用一个一次性
`QQmlComponent` 加载 `import QtQuick / import QtQuick.Controls / Item {}` 并打点：

| 段 | 中位 / P95 |
|---|---|
| `context-ready → qml-warm`（引擎 + Controls 导入 + 样式初始化） | **12.2 / 12.4** |
| `qml-warm → qml-loaded`（Main.qml 自身编译 + 建树） | **550.7 / 556.6** |

**结论**：这 550 ms 既不是引擎/import 预热（12 ms），也不是被提前拉进来的大组件（~12 ms），
而是 **Main.qml 自身编译 + 实例化**；而其中最大的单文件只占 ~12 ms，说明成本是
**弥散在整个对象树**上的，没有单点热点。

因此按组件逐个改 `Loader` 只能拿到**成比例的小切片**，却要动 objectName 契约、绑定与焦点顺序 ——
**投入产出不成立，本轮不做**。真正对「空白窗」这个用户可见问题有效的，是**覆盖**这 550 ms
而不是压缩它（即启动画面）。工程没有 `qt_add_qml_module` / `qmlcachegen`，全部 QML 运行时编译；
若要压缩，正确的方向是给 QML 加预编译缓存，而不是拆组件。

证据目录：`out/startup-measurements/b1-probe-nostub-nofile-20261004-140340/`、
`b1-probe-withimageworkspace-nofile-20261004-140431/`、`b1-probe-warm-20261004-140529/`。

## Step 9 · B2 启动画面（2026-10-04 实现）

既然 550 ms 压不下去（B1 已证伪），就**盖住**它直到第一帧真正画出来。

**为什么必须是原生窗口而不是 QML。** 这段时间里 GUI 线程一直在 `engine->load()` 里同步阻塞，
事件循环还没跑。`QQuickWindow` 在事件循环启动前画不出任何东西 —— 一个 QML 启动画面自己就是
另一个空白窗。所以 `StartupSplash` 自带**独立线程**和**自己的 HWND**，用 GDI 直接绘制，
在该线程上跑自己的消息循环。

**实现要点**

- `src/ui_qml/src/StartupSplash.cpp`：`WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`
  + `WS_POPUP`。不进任务栏/alt-tab，不抢焦点（主窗口几百毫秒后才 `show()` 并激活）。
- **不阻塞 GUI 线程**：`requestDismiss()` 只置原子标志 + `PostMessage`，任何线程可调；
  只有 `join()` 会等，而它只在 `exec()` 里调用，那时 worker 已空闲。
- **所有提前返回都能撤掉它**：`DesktopApplication::load()` 里 splash 是**守卫对象**，
  每个 `return false` 出栈即撤；成功后所有权移交 `splash_`，由 `sceneGraphInitialized`
  （DirectConnection，可在渲染线程调）撤。
- **smoke 模式不显示**（`options_.smokeMode`）。
- 背景色 `#090d14` 与 `Theme.background` 一致，撤到主窗口不是亮度跳变。
- 失败即降级为「没有启动画面」，绝不阻塞或中断启动。

**实测（同日、release、warm cache、5 轮 + 1 warmup，带 1080p60 素材）**

| 指标 | 值（中位 / P95） |
|---|---|
| `spawn → splash-painted`（启动画面首帧真正上屏） | **57.1 / 68.0 ms** |
| `spawn → window`（工具枚举到的**审阅**窗口） | 642.6 / 646.1 ms |
| `spawn → sg-initialized` | 791.4 / 795.1 ms |
| `spawn → first snapshot committed (kind 8)` | 940.1 / 945.3 ms |

启动画面从 **~57 ms** 一直挂到首帧（`sg-initialized`），覆盖约 **734 ms**。

**踩到并修掉的一个回归（必须记）**：第一版 splash 是**无主**顶层窗口，于是
`Process.CloseMainWindow()` —— 也就是 `measure-startup.ps1` 结束每轮时的关闭动作 —— 按枚举顺序
把 `WM_CLOSE` 发给了**启动画面**而不是审阅窗口。表现为：每轮 `exit=-1`（10 s 等不到退出后被
`Kill(true)` 硬杀）、**播放 trace 一个都不落盘**。发现方式是注意到加 splash 之后所有带文件的
运行都丢失了 trace，回查 `summary.json` 的 `exitCode` 才定位到硬杀。
修法：先建一个**从不显示的隐藏 owner 窗口**，把 splash 作为**被拥有的窗口**创建
（`kSplashOwnerClassName`）。被拥有的窗口会被「进程主窗口」搜索跳过，于是关闭进程关的是审阅窗口，
应用优雅退出、`exit=0`、trace 恢复。回归断言：
`ui.StartupSplashTests.SplashWindowIsOwnedSoItIsNeverTheProcessMainWindow`。

**一个必须说清的度量陷阱**：修复之后 `spawn → window` 从 77 ms **变回** 642 ms。
这不是倒退 —— 被拥有的窗口本来就不该被「找主窗口」的枚举命中，那 77 ms 测的是工具误抓了 splash。
现在 `spawn → window` 如实报告审阅窗口的 642 ms，splash 的上屏时刻改由新增的
`splash-painted` 里程碑记录（它在 splash 自己的线程上打点，不依赖外部窗口枚举）。

**未变**：主窗口时点与各 segment 与 B2 之前一致（`show-enter → sg-initialized` 171.7 ms vs 之前
184.0 ms，属运行间漂移）。按 L178，只有 segment 级差值可归因，这里没有任何 segment 变化。

证据：`out/startup-measurements/b2-splash-onefile-20261004-143150/`（无主，有回归）、
`b3-owned-onefile-20261004-144218/`、`b3-splashmark-onefile-20261004-144428/`（修复后）。
测试：`ui.StartupSplashTests` **5/5**；变异 2/2（去掉 `WM_CLOSE` 的 `DestroyWindow` → 3/4 失败；
去掉 `WS_EX_NOACTIVATE` → 焦点断言失败），产品文件按字节还原。
**未验证**：本环境拿不到屏幕确认，启动画面的**观感**（位置、字号、留白）未经人眼或截图核对。

## Step 10 · B3 worker 侧预探测 —— **收益不足，不做**（2026-10-04 实测）

B3 想让探测与 550 ms 的 QML 段、~172 ms 的设备初始化重叠。先测余量再决定要不要动媒体路径。

当前架构下探测本身**已经是异步的**：`MediaProbe` 有 2 个 worker，`submit` 即发即忘，且
**没有结果缓存** —— 所以「预探测」若不做缓存就是重复劳动，做缓存则要在解码适配器里引入
按文件身份失效的语义（路径 + 字节数 + mtime），是一块新的正确性表面。

**余量实测（当前 SHA，带 1080p60 素材，播放 trace 时间线，以 kind 0 CommandAccepted 为 0）**

| 事件 | 相对时间 |
|---|---|
| kind 0 CommandAccepted | 0 ms |
| kind 2 | +14.6 ms |
| kind 4 FrameSetReady | +42.5 ms |
| kind 8 SnapshotCommitted | +42.7 ms |

**整条「打开」只有 ~42.7 ms**，而 kind 2 → kind 4 的 ~28 ms 里 1080p60 的解码占大头，探测只是其中
一小段。即使预探测做到完美，上限也只有个位数毫秒，占 ~940 ms 总启动的 **<1%**。

这与 Step 5（2026-09-28）在当时基线上独立得到的「打开本身仅 ~38 ms」一致 —— 本次是在当前 SHA
上复现了同一结论，不是新判断。**为 <1% 在媒体路径上加缓存不值得，B3 本轮不做。**

对比之下真正的量级：`context-ready → qml-loaded` 550 ms（需 qmlcachegen，见 Step 8）、
`show-enter → sg-initialized` ~172 ms（设备生命周期架构改动）。这两项都远大于 B3 的全部余量。

证据：`out/startup-measurements/b3-splashmark-onefile-20261004-144428/`（含 trace）。

### 一个真缺陷：splash 的拆卸存在竞态挂起（2026-10-04 发现，**根因是推断，未复现**）

全量 CTest 跑出一次超时：
`ui.StartupSplashTests.DestructionRemovesTheWindowWithoutAnExplicitDismiss (Timeout)`。
再次单跑 8/8 通过，此后再未复现，**根因未确证**。按代码推断出的机制是：

`requestDismiss()` 只在 `hwnd_` 非空时 `PostMessage(WM_CLOSE)`。worker 在
`CreateWindowExW` **之前**重查过标志，但在 `hwnd_` 存储之前还有一段窗口；这段时间里
`requestDismiss()` 读到空句柄、不发任何消息，而 worker 随即进入 `GetMessage` **无超时阻塞**，
`join()` 永不返回。生产里对应「启动期间被拆掉的启动会卡死在那里」。

改为 worker 的消息循环**每轮重读标志并带超时等待**（`MsgWaitForMultipleObjects` +
`kPollMilliseconds = 50`），拆卸不再依赖调用方与句柄在同一瞬间可见；原先
`CreateWindowExW` 之前的那次重查仍然保留，它只是让晚到的 splash 根本不出现。

**但必须说清：这一改动没有测试覆盖。** 写了一个 150 次「建了就扔」的压力测试，并做了两个变异
（去掉轮询恢复成阻塞 `GetMessage`；保留超时等待但不读标志）—— **两个变异都存活**。
原因明确：`std::thread` 的启动开销远大于那段竞态窗口，析构几乎总是先到，worker 走早退分支，
根本进不了窗口。所以
`ui.StartupSplashTests.RepeatedCreateAndDropNeverLeavesAWindowBehind` 是**反复丢弃的冒烟测试
（断言后置条件：无残留窗口），不是该竞态的覆盖**。轮询循环作为**加固**保留，不作为「已验证行为」
记账。若将来要真正覆盖，需要一个测试钩子把 worker 停在标志重查与句柄发布之间 —— 本轮没加。

证据：`out/verification/a4b-foreground-handover/mutate-splash-strand.ps1`（含负面结论）。

### 编译坑 · 含 windows.h 的编译单元不能用多行流式断言

`StartupSplashTests.cpp` 一开始报 `C1903`（`-scanDependencies` 下连底层诊断都看不到）。
绕过 `-scanDependencies` 单独编译才拿到真因：**C1057 unexpected end of file in macro
expansion** —— 只要该 TU 包含 `<windows.h>`，下面这种跨行写法就会让预处理器跟丢宏展开：

```cpp
EXPECT_TRUE(x)
    << "message";          // 断言与消息必须写在同一物理行
```

与 `ASSERT_`/`EXPECT_` 无关，与括号无关（单独测过），与 `windows.h` 的包含顺序也无关。
`MainQmlContractTests.cpp` 能这么写是因为它没有包含 `windows.h`。规则已写进该测试文件头部。

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
8. **当时的 cpack 体积复核（后续结论修正见下）。** `cmake/Install.cmake` 的
   `InstallRequiredSystemLibraries` 只安装 CRT/UCRT DLL，CMake 4.4.0 的该模块全文不含任何
   `.exe` 安装路径；`out/build/release/cmake_install.cmake` 中 `vc_redist` 出现 0 次，仓库
   代码也从不引用它。59.77 MiB 那个包（`out/packages/CompareStation-1.9.0-pre-step2.zip`）
   多出的 24.45 MB `vc_redist.x64.exe` 当时被归因为**从别处手工拷入 staging 的一次性产物**（同一字节数的
   副本还留在 `out/package/zip/VCStation-1.6.0-windows-x64/` 与 `out/migration/…`）；
   它同时还多了 `dxcompiler.dll`(14.3 MB) 与 `d3dcompiler_47.dll`(4.7 MB)，故
   59.77 − 24.45 = 35.3 而非 26.63。正常 `-Preset release` + `cpack --preset release-zip`
   产出的 26.63 MiB 才是基线。原第 8 条"疑似 CMake 版本行为差异"的结论作废。
   **实测复核（2026-09-28）**：干净跑
   `build.ps1 -Preset release -UseInstalledDependencies` 后 `cpack --preset release-zip`，
   产出 `out/package/zip/CompareStation-1.9.0-windows-x64.zip` =
   **26.63 MiB / 310 条目 / 解压 66.42 MiB / 无 vc_redist**，与基线逐项吻合
   （27919730 vs 27923994 bytes，差 0.004 MiB 属正常构建抖动）。体积维度可直接用于前后对照。
   **2026-09-30 修正**：2.0.3 标准 CPack 日志明确记录 `windeployqt` 自动部署
   `vc_redist.x64.exe`、`dxcompiler.dll`、`d3dcompiler_47.dll`、`dxil.dll`，包再次增至
   59.8 MiB；因此“只可能手工拷入／不需要管线修复”的推断不能成立。现在显式关闭 Qt 的重复
   CRT 安装器与系统图形编译器复制，保留必要的 app-local DLL，并通过 staging 门禁拒绝回退。
   后续以 [必要运行时打包规则](../building.md#zip-只打包必要运行时) 为准，不沿用本条旧归因。
