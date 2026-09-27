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
    -LaunchArgument 'G:\Workspaces\Toy\out\evidence-fixtures\media\frameid_1080p60_a.mp4'
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

## 已知限制与待办

1. **冷启动未测**：本表全部为热缓存。冷启动协议待定义（重启后首轮即测、不预热）。
2. **首帧层未接入**：带文件"到首帧"需 playback trace kind 0/4/8，Step 5 动手前补齐。
3. P95 = 5 轮最大值，样本小；结论以中位数为主。
4. dev 构建数据只用于机制验证（QML 段 1570 ms），不进基线表。
5. Step 0/1 改动尚未提交；全量测试套件结果见提交说明。
