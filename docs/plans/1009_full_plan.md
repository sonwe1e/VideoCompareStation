这份方案来自四路代码审计，我抽查了其中的关键证据。我只读了代码，没有启动应用，所以涉及"手感"的判断都还要上真机确认。

总体判断：工程底子扎实，分层由 CMake 强制，告警按错误处理，QML lint 要求零告警，代码里没有 TODO。帧精确性和"整组同步"这条设计主线，在开源工具里也少见。

短板有三类：

- 审查闭环断了：问题记录保存了视口，却恢复不回来。
- 像素级检查缺少关键件：常规视图始终是双线性采样，视频没有像素读数，也没有放大镜（loupe）。
- 工程上：几个 2000 到 4700 行的上帝对象，加上一个塞满测试 harness 的 Main.cpp。

---

## 落地进度（2026-10-09 更新）

- 第 1 周四组条目已全部完成（`7260471`）并随 `89c819d` 推送 origin/main：交互 P0 的 1–3、高倍率最近邻、ReviewRuntime 自 detach、clang-tidy 扩规则。下文对应条目已打【已落地】标记，正文保留审计时的发现快照。
- 当日核实未动工：视频像素读数（`cursorPixel` 仍只在 ImageReviewController）、放大镜（全仓库无 loupe/magnifier 实现）、模式/视图快捷键（ReviewShortcuts.qml 无数字键与 F/Ctrl+0 绑定）。
- Windows 验收清账：**已通过**（2026-10-09 用户真机走查 `main@6058d19` dev 构建，无回归；交互类悬置项已核销，多容器导出矩阵与 WARP 像素回读仍按各条目原边界，见台账当日条目）。
- 工单 B 模式与视图快捷键：**已落地**（2026-10-09，同日提交）：数字键 1–6、F/Ctrl+0/+−/R、`` ` ``/Shift+`` ` `` 参考原图直看与锁定、双击统一 100%↔适应（全屏只留 F11）、帮助浮层从绑定表生成。偏差三条：Z 留给放大镜（计划 P1-5），避免二次改键；`` ` `` 为点按切换而非按住（Qt ApplicationShortcut 无释放事件，且 RV/Nuke 的单键翻转同为点按）；模式共 6 个故键位 1–6。
- 评审优先级第 4 项（动作与帮助同源）：**已落地**（`10e72a7`）：ShortcutCatalog.js 单源常量供 Main 绑定、菜单文案与帮助表三方引用；M 键补上菜单文案；工作区动作进入两个 preset 的帮助表；契约测试三向交叉 + 重复绑定审计（Esc 为唯一登记过的重叠）。Ctrl+Shift+F 已被图片文件夹比较占用（评审提醒属实），后续帧时序浮层需另选键。
- 行号漂移提示：week-1 改动后 Main.qml 3871→3904、PlaybackCoordinator.cpp 4735→4772、Main.cpp 3124→3132，下文旧行号需按此折算；标注"锚点已核实"的小节除外。

---

一、播放体验（优先级最高，直接影响审查结论）

P0：像素检查保真

1. 【已落地 7260471：>200% Auto 切最近邻 + Smooth/Pixel 徽章；2026-10-09 复审发现口径错误并经 `919d528` 修正——判定从 fit 相对 viewScale 改为徽章同源的物理倍率（含 DPR/ROI），小素材被窗口放大的场景现在正确切最近邻】高倍率改用最近邻采样。现在 drawVideoSlots 固定绑定 linearSampler_（D3d11ComparisonRenderer.cpp:1505），放大后像素会被抹平，看不清块效应和插帧形变。最近邻采样器已经存在（:1055），只有差异视图在用。
   - 方案：缩放超过 200%（可配置）自动切到最近邻，400% 以上叠加像素网格。也提供一个"平滑 / 像素"手动开关，沿用现有的重采样徽章。
   - 参考：OpenRV、DJV、tev 都在高倍率下默认最近邻并显示网格。
2. 视频像素读数。目前 cursorPixel 只存在于 ImageReviewController。
   - 方案：给视频加一个读数，显示 A/B/C 同一坐标的 Y'CbCr 原始码值和转换后 RGB，并标明 8/10-bit 和 limited/full 口径。实现上可以复用 Present 后的 staging 回读，只读 1×1 区域，开销很小。
   - 参考：Nuke 和 RV 的 pixel probe。
3. 4:2:2、4:4:4 和 RGB 素材不再压成 4:2:0。软解路径用 SWS_BILINEAR 压到 NV12/P010（SoftwareDecoder.cpp:855-871）。对比工具压掉色度分辨率，恰好会让被测缺陷消失。
   - 方案：增加 P210/Y410 或 RGBA16 纹理路径，shader 按平面格式分支。上线前至少在 UI 上把"已降采样"标成警告色，而不是一个中性标签。

P1：时钟与流畅度

4. 呈现节奏对齐 vsync。现在由 SteadyDeadlineScheduler 的墙钟驱动（PlaybackCoordinator.cpp:46-49），提前 14 ms 准备，但不看显示器节拍。24p 素材放在 60Hz 屏上会出现不均匀的 3:2 抖动，用户可能误判成"素材卡顿"，这正是 V-01 要区分的问题。
   - 方案：用 IDXGISwapChain::GetFrameStatistics 或 DwmGetCompositionTimingInfo 取 vsync 相位，把截止时间吸附到最近的刷新点上。在 OSC 统计里区分三种情况：素材重复帧、播放器丢帧、显示节拍抖动。
   - 参考：mpv 的 video-sync=display-resample 和 libplacebo 的帧混合。
5. 实时拖动预览。现在拖动进度条只请求缩略图，松手才真正定位（TimelineTracks.qml:254-271）。
   - 方案：拖动中按关键帧近似解码，按"最新请求优先"出图（已有 Exact 槽位，最新请求获胜）。停稳 150 ms 后再补一次精确帧。
   - 参考：Resolve 和 Premiere 的 live scrub。
6. 改 LRU 并加缓存可视化。FrameSet 缓存只有 4 组，并且是 FIFO 淘汰（MultiSourceFrameProvider.cpp:42-44, 914-975），反复在附近几帧来回检查时命中率低。
   - 方案：改成 LRU，按显存预算动态扩容，比如 1080p 三源保留 ±12 帧。在时间线上画一条缓存条，显示哪些帧已就绪。
   - 参考：RV 和 DJV 的缓存条。

P1：走带控制

7. J/K/L 穿梭、反向播放、往返循环。现在倍速只有 0.25× 到 4× 的固定档位。
   - J/K/L：每按一次加速；按住 K 再按 J 或 L 是慢速逐帧。
   - 反向播放：复用已有的反向 GOP 窗口（SourceDecodeActor.cpp:26-30），先支持 ≤1× 的速度。
   - 往返循环：对插帧审查很有用，来回看运动连续性。
8. 跳转到帧号或时间码。点击帧号直接输入，支持 +5、-1s 这样的相对写法。

P2：格式与色彩

9. VP9/AV1 解码。D3D11VA 已经支持这两种格式，主要工作是放开 MediaProbe 白名单并补测试矩阵，成本低。用户目前把这项放在延后池，建议重新排期。
10. BT.2020 和 HDR。至少做到"可以打开，并显式标注未做色调映射"，而不是直接拒绝。之后再考虑 libplacebo 风格的 PQ/HLG 到 SDR 映射。
11. 帧时序浮层。按 Ctrl+Shift+F 打开，显示解码、上传、呈现间隔的直方图，用于给 V-01 和 V-02 取证。数据已经在 JSONL trace 里，只差可视化。参考 mpv 的 Shift+I 统计页。

---

二、交互优化

P0：修复已确认的缺陷

1. 【已落地 7260471：restoreViewport 已接线并带契约测试；2026-10-09 外部复审发现保存端缺 viewScale/参考身份且恢复与打开竞速，经 `12d2774` 补齐：记录保存 viewScale + referenceSourceIndex，恢复改分阶段状态机（等打开→比较对/模式→目标帧呈现→视口），另修复模式重放误写 viewModeCode 的静默失败】问题记录恢复不了视口。IssueLogController.cpp:220-265 保存了 ROI、中心点和缩放，但 applyIssueRestore（Main.qml:1500-1510）只恢复了模式、源对和帧号。ComparisonSurface::restoreViewport（ComparisonSurface.cpp:794）全仓库没有调用方。这违背了产品文档里"回到同一状态复核"的目标。改动只需一行调用，再补一个契约测试。
2. 【已落地 7260471：帮助内容已改为与实际绑定一致；"从绑定表生成"的根治方案仍开放，见第 2–3 周细化 B】快捷键帮助写错了。播放器方案里写"← / → 快退 / 快进 5 秒"、"Ctrl+← / → 30 秒"（ShortcutHelpOverlay.qml:24），但代码在所有方案下都是逐帧（ReviewShortcuts.qml:82 的注释也这么写）。另外，帮助里漏了 Up/Down、Alt+←/→（Wipe）和 Home/End。
   - 建议：帮助内容直接从 ReviewShortcuts 的绑定表生成，不再手写第二份。
3. 【已落地 7260471：M 键 + IssueNoteDialog，回车带备注、Esc 留空；2026-10-09 复审指出备注期间播放推进会漂移记录，经 `8ab6af8` 改为按下即冻结观察，备注经 attachNote 事后补文字】记录问题时可以写备注。现在 captureCurrentIssue("") 永远传空备注，也没有快捷键。
   - 建议：M 键直接记录，并弹出一个可以忽略的备注输入框；回车保存，Esc 留空保存。参考 Frame.io 的"按键留言，自动带时间码"。

P1：键盘优先的比较操作

4. 【已落地 2026-10-09：数字键 1–6（模式共 6 个）、F 适应、Ctrl+0 100%、+/− 缩放、R 重置、` 点按看参考原图 + Shift+` 锁定；Z 留给放大镜】对比模式和视图快捷键。现在模式只能用鼠标点。
   - 数字键 1–8 切换视图。
   - F 适应窗口，Ctrl+0 或 Z 切 100%，+/- 缩放，R 重置。
   - "按住看原图"加一个键盘版：按住 ` 看参考源，`Shift+``` 锁定切换。参考 RV 和 Nuke 的单键 A/B 翻转。
5. 放大镜。Z 键按住时，在光标处显示一个 4–8× 的放大圆窗，A/B/C 同步，叠加像素读数。参考 video-compare 的 Z/C 放大镜和 Photoshop 的导航器。
6. 命令面板。Ctrl+Shift+P 按名称模糊搜索所有命令，并显示对应快捷键。功能已经很多了（视图、指标、对齐、导出、编辑），菜单藏得深，命令面板能一次性解决可发现性问题（U-01）。参考 VS Code 和 Figma。
7. 【部分落地 2026-10-09：双击已统一为 100%↔适应（ROI 框选激活时仍为清除 ROI），全屏只留 F11；Ctrl+A/D 冲突与 JSON 键位表导入导出仍待做】快捷键冲突与重绑。
   - Ctrl+A/D 用来跳 1 秒，和"全选"的肌肉记忆冲突。
   - A/O 在视频和图片里的含义完全不同。
   - 双击在视频里是全屏，在图片里是 100%/适应。
   - 建议：双击统一为"100% ↔ 适应"，全屏只留 F11。之后提供可以导入导出的 JSON 键位表。
8. 会话中切换参考源。现在只能在打开时的确认窗里选。可以在 ActiveSourceStrip 的源卡片右键菜单里加"设为参考"。

P2：状态与记忆

9. 视图操作可撤销。缩放、平移、ROI、偏移这些改动记一条轻量的视图历史，用 Ctrl+Z 或 Backspace 回到上一个视图。参考 Lightroom 的"上一视图"。
10. 记住布局。侧栏和检查器的显隐与宽度、窗口几何，都存进 Settings。
11. 消息历史。5 秒自动消失的提示改为可以在通知中心回看，尤其是导出结果和失败原因。

---

三、UI 设计

设计方向：保持现有的 slate 深色基调，从"颜色已统一"推进到"尺寸、字体、动效也统一"。目标是让视频和图片两个工作区看起来属于同一个产品。

P1：补齐设计 token

1. 颜色和圆角已经收进 VcsTheme.js（主题之外只有 17 处写死的十六进制颜色），但其他尺寸几乎没有 token：
   - font.pixelSize 有 180 处字面量，共 12 种字号。
   - radius: 有 92 处字面量，spacing 和 margins 加起来约 175 处。
   - 动效时长 80 到 160 ms 各写各的。
   - "Consolas" 写死了 14 次。
2. 建议的 token：
   - 字号阶梯：11 / 12 / 13 / 16，外加一个等宽字体 token。
   - 间距：4 / 8 / 12 / 16 / 24。
   - 控件高度：28 / 32 / 38。
   - 动效：fast 100 ms / normal 160 ms。
3. 同时去掉 Main.qml:31-34 把主题颜色转发成属性、再逐层传给子组件的做法（全局共 65 个 property color），避免出现两个事实源。
4. 棋盘格的 #272727/#404040（ImageWorkspace.qml:524）也收进主题。

P1：统一组件

5. 按钮。现在有两套共用按钮：ReviewActionButton 用了 59 处，VcsToolButton 用了 12 处。另外还有 ModeButton、DiffModeButton、ModeChip、EditButton、RangeChip、DarkTabButton 等局部变体。
   - 建议：合并成一个基础按钮，提供 text / icon / chip / tab / toggle 五种变体，统一 hover、pressed、disabled、focus 四种状态。
6. 图标。SVG 只在 TransportBar 里用，其他地方都是 Unicode 字形（⋯ + − × …）。
   - 建议：统一成一套 SVG 图标，比如 Lucide 或 Fluent UI System Icons，都可以开源免费商用，引入时固定版本。

P1：布局骨架

7. 改用命名区域布局。Main.qml 靠手算 anchor 偏移排版，比如 :3019 的 sourceBar.height + comparisonBar.height + 6。
   - 建议：改用 SplitView 或 ColumnLayout，划出 header / sidebar / stage / inspector / transport 五个区域。侧栏和检查器可以拖动改宽，宽度会被记住。
8. 窄窗自适应。在 960 px 最小宽度下，侧栏（260）加检查器（300）只给画面留下约 386 px。
   - 建议：宽度低于 1200 时自动把检查器收成图标栏，低于 1000 时侧栏改为浮层。参考 VS Code 的活动栏。
9. 统一两个工作区的外壳。视频的控件在右侧和底部（检查器、OSC），图片的控件在顶部卡片和底栏，同一个产品里有两套空间逻辑。
   - 建议：图片工作区复用同一个 header、检查器和状态栏。模式芯片放进 header，细节参数放进检查器，和产品文档第 7 节"细节放检查器"的要求一致。

P2：无障碍与高 DPI

10. 无障碍。
    - ImageWorkspace.qml 3131 行里没有任何显式的 Accessible 属性。
    - RangeChip 和 ContinuityOption 只有 MouseArea，键盘到不了。
    - 全局没有一处 KeyNavigation，焦点环在鼠标点击时也会出现。
    - 建议：焦点环改成只在 visualFocus 时显示，每个区域设一个 FocusScope，补齐 Accessible 名称和角色。完整的 WCAG 结论仍需要用读屏器做人工测试。
11. 高 DPI。在 125% 和 150% 缩放下，1 px 边框和 9–10 px 小字会发虚。
    - 建议：设置 HighDpiScaleFactorRoundingPolicy，正文最小字号提到 11 px。mutedText #94a3b8 用在 9–11 px 小字上的对比度偏低，需要重新验算。

---

四、技术债

P0：安全与正确性

1. 【已落地 7260471：GraphicsNotificationPump 不再自 detach，线程状态由 shared_ptr 持有】线程自 detach。ReviewRuntime.cpp:89-91 在 worker 线程里调用 stop() 时会 detach()，线程会比持有者活得更久，可能出现释放后使用（UAF）。
   - 建议：worker 线程自己调用 stop 时只发停止请求，由持有者在析构时 join。或者在投递关闭消息时禁止自调用。补一个 ASan 场景测试。
2. 【部分落地 7260471：bugprone/concurrency/performance/member-init 已加入并清完 ~160 处，HeaderFilterRegex 已去掉 jobs_ffmpeg；platform_windows 仍被 Quality.cmake:86 整体排除，单独 lint 配置待做】lint 覆盖面。
   - .clang-tidy 只开了 clang-analyzer-*，而这个代码库有约 30 处线程和约 31 个使用互斥锁的文件。建议加上 bugprone-*、concurrency-*、performance-* 和 cppcoreguidelines-pro-type-member-init。
   - HeaderFilterRegex 里还写着已经删除的 jobs_ffmpeg。
   - Quality.cmake:86 把整个 platform_windows 排除在 lint 外，D3D 渲染器和 GPU actor 都没有静态分析。建议用一个精简的 include 配置单独为它跑 lint。

P1：拆分上帝对象（按事件族拆，不做大重写）

3. PlaybackCoordinator.cpp（4735 行）。Impl 里混着命令分发、探测、对齐生命周期、截止时间、帧集接收、图形状态和快照发布。
   - 建议：把对齐状态机全部迁进已有的 AlignmentWorkflow.cpp，把发布逻辑全部迁进 CoordinatorPublication.cpp，再抽出 TransportClock 和 FrameAdmission。每次只拆一个事件族，现有的 161 个 application 单测作为回归网。
4. app/Main.cpp（3124 行）。大部分是 smoke、性能和取证 harness：runDesktop 里有约 69 处 smokeMode 引用，图片文件夹取证在 2079–2610 行，像素探针在 2611–2930 行。
   - 建议：把这些搬到独立的 dvs_app_diagnostics 目标。命令行参数改用解析表，替换现在按 argc == N 写的 if 链。
5. ReviewController。它有 76 个 Q_PROPERTY，几乎都挂在 stateChanged 和 frameStateChanged 两个粗粒度信号上，任何变化都会让 QML 全量重新求值。
   - 建议：拆成 transport / sources / alignment / frameInfo 四个子对象，每个有自己的 NOTIFY。ImageReviewController（53 个属性）按同样方式拆。
6. Main.qml（3871 行，189 个属性、112 个函数）和 ImageWorkspace.qml（3131 行）。可以跟第三部分第 7 条的布局骨架改造一起做，按区域拆成独立组件。
7. D3d11ComparisonRenderer.cpp。约 830 行纯布局数学（SurfaceColumnLayout 等）和 D3D 调用混在一起。
   - 建议：把布局数学移到不依赖平台的模块并补单测。渲染器目前没有组件测试，这一步能补上大部分缺口。

P1：测试与流程

8. 积压的待验收项。台账里有 8 条以上"修复草稿，Windows 验收待完成"（10-06 到 10-07），都已合入但没有验收。建议先开一轮专门的 Windows 验收清账，再开新功能，否则台账状态会越来越失真。
9. CI 只有一个机器池。除一个 windows-2022 job 外全部跑在自托管 runner 上。建议至少让 format、lint 和单元测试在 GitHub 托管的 runner 上也跑一份，自托管机器宕机时主线仍有保护。
10. 测试缺口。tests/hardware 目前只有一个 CMakeLists；persistence_json 只有 8 个测试。
11. 【部分处理 2026-10-09：根目录文档类（分析/方案/工单/编译笔记）已按功能分 4 个提交入库；`generated-*.png` 仍留在工作区未跟踪，`.gitignore` 收尾待做】仓库根目录整理。根目录有一堆未跟踪的 HTML、PNG、update.finished，还有 symbolize.obj、vc140.pdb。建议加进 .gitignore，或者把需要保留的报告移到 docs/。

---

推荐落地顺序

┌───────────┬────────────────────────────────────────────────────────────────────────────────────────────────────────┬────────────────────────────────────────┐
│   阶段    │                                                  内容                                                  │                  理由                  │
├───────────┼────────────────────────────────────────────────────────────────────────────────────────────────────────┼────────────────────────────────────────┤
│ 第 1 周   │ 交互 P0 的 1–3、高倍率最近邻、ReviewRuntime 自 detach、clang-tidy 扩规则                               │ 改动小、收益确定，修掉会误导结论的问题 │
├───────────┼────────────────────────────────────────────────────────────────────────────────────────────────────────┼────────────────────────────────────────┤
│ 第 2–3 周 │ 视频像素读数、放大镜、模式和视图快捷键、Windows 验收清账                                               │ 补齐像素级审查闭环                     │
├───────────┼────────────────────────────────────────────────────────────────────────────────────────────────────────┼────────────────────────────────────────┤
│ 第 4–6 周 │ 设计 token、统一按钮和图标、布局骨架、Main.qml 拆分                                                    │ UI 重构和 QML 拆分一起做，只拆一次     │
├───────────┼────────────────────────────────────────────────────────────────────────────────────────────────────────┼────────────────────────────────────────┤
│ 之后      │ vsync 对齐、实时拖动、LRU 缓存条、J/K/L、反向播放；Coordinator 和 Controller 拆分；VP9/AV1、4:4:4 路径 │ 投入较大，按用户反馈排序               │
└───────────┴────────────────────────────────────────────────────────────────────────────────────────────────────────┴────────────────────────────────────────┘

状态（2026-10-09）：第 1 周已完成；当前进入第 2–3 周，执行顺序 A →（B ∥ C）→ D，见下节细化。

---

## 第 2–3 周工单细化（2026-10-09，锚点已对当前 HEAD 核实）

### A. Windows 验收清账（先做，不改代码）

**状态：已通过（2026-10-09 用户真机走查，详见台账当日条目与上方进度区）。**

真机走查并逐条核销 `docs/engineering/visual-review-backlog.md` 的待验收条目。dev 构建 + 启动自检已通过，体验清单 2026-10-09 已发出。范围按台账分四组：

- 渲染类（直接影响比对结论）：Highlight 阈值背景透 A 路、SignedSubtract 阈值拒绝区中灰、差异阈值不吞 Fade 混合。
- 侧栏与历史（V-08）：当前文件夹／最近打开两页、右键"打开为新任务／加入对比"及满三源／重复／不可读时的禁用说明、键盘焦点与 Enter 行为。
- 倍率类：竖屏视频倍率读数与一键真实尺寸（含 PR 49 的 fit 以下原生缩放）、拖动不跳变。
- 导出类（需多容器素材）：MP4 末 GOP 关键帧、MKV 惰性索引、TS 起始关键帧、起始帧号舍入、非零起始 PTS。

验收通过的条目在台账推进状态；发现回归记新条目，不带病开新功能。

### B. 模式与视图快捷键（改动面最小，可与 A 并行）

**状态：已落地（2026-10-09）。** 实现按下述方案，偏差与补充：Z 未占用（留给 D 的放大镜按住键）；`` ` `` 实现为点按切换 + Shift+`` ` `` 锁定；帮助浮层由 ReviewShortcuts 的 helpKeys/helpLabel/helpPlayerLabel/helpPresets 生成，Ctrl+方向键的按 preset 文案也在绑定表内；契约测试经 Shortcut 的 activated 信号驱动（ctest 进程拿不到窗口前台，Qt 的 ApplicationShortcut 只在活动窗口匹配，真键事件会把契约耦合到 runner 的前台权限）。

- 绑定点：`src/ui_qml/qml/ReviewShortcuts.qml`（221 行）是唯一绑定表，统一 `Shortcut{enabled,onActivated}` 结构；动作经 `ReviewActions.qml` 门面转发 controller，新增快捷键不直接摸 controller。
- 内容：数字键 1–8 切 CompareModeBar 模式；F 适应窗口；Ctrl+0 / Z 100%；+/- 走 `ComparisonViewport.qml:558` 的居中 `zoomAt` 路径；R 重置；`` ` `` 按住看参考源，`Shift+`` ` 锁定切换。
- 根治帮助浮层：从 ReviewShortcuts 绑定表生成帮助内容，替代手写第二份（P0-2 只修了内容，单一事实源未建）。
- 顺带统一双击语义为"100% ↔ 适应"，全屏只留 F11；新绑定每条补 MainQmlContractTests 契约断言。

### C. 视频像素读数（B 之外独立，放大镜的前置件）

- 光标锚点：`ComparisonViewport.qml:372` 的 `onPositionChanged` 与 `panelPoint()` 已完成部件坐标 → 源坐标换算，缺的是把源坐标送进回读通道。
- 回读通道已存在：D3d11GpuFrameBacking / GpuTransferActor 已有 staging / CopySubresourceRegion 路径；新增"只读 1×1"最小查询接口，禁止整帧回读进热路径。
- 显示口径：A/B/C 同坐标的 Y'CbCr 原始码值 + 转换后 RGB，标注 8/10-bit 与 limited/full；与图片侧 `cursorPixel` 口径一致。
- 验收：对已知测试图（纯色 + 梯度）逐通道比对回读值；异步查询携带会话／代际身份，不阻塞 GUI／渲染线程。

### D. 放大镜 loupe（依赖 C）

- Z 按住显示 4–8× 放大圆窗（倍率／尺寸可配置），A/B/C 严格同坐标，叠加 C 的像素读数，松开即隐。
- 实现：QML 覆盖层 + 既有放大采样路径，读数复用 C 的查询接口；放大镜内默认最近邻，与 200% 采样策略协同。
- 验收：三源内容同坐标偏差为 0；圆窗内像素读数与 C 的定点读数一致。
