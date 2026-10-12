# 视觉审查问题台账

更新：2026-10-11（打开/添加/替换意图区分 I01）。需求见 [产品目标](../product/visual-review.md)，代码路由与命令见
[Agent 快速定位指南](../agent-guide.md)。此页是当前任务入口，不是发布完成清单。

## 2026-10-11 打开、添加、替换意图区分 I01

- **范围**：《CompareStation 下一步体验与性能计划 2026-10-11》§6 I01（P1），R4 阶段工单。
  把"为什么打开"写成每个意图自己的显式参数，并把拖到源卡片做成有清晰落点提示的替换入口。
  不引入标签页/项目系统，沿用单会话；不动 broker 协议、StartupRequestDispatch 的排队/耐心值
  与 GPU 渲染后端。
- **问题（基线 b35b808）**：`enqueueStartupRequest(kind, files)` 收到 PlaySingle/Compare 却把
  kind 丢弃——系统双击单视频与资源管理器比较动词进入完全相同的 OpenSourcesIntent，双击后
  永不自动播放（用户预期"像播放器"），且没有任何设置能改变这一点；替换只能经源菜单
  "替换文件…"的文件选择器，拖拽永远落入工作区级"打开/追加"，拖到哪个卡片都不会替换那一路。
- **交付**：
  - `ReviewShellController.h/.cpp`：新增 `OpenPurpose{OpenForReview,OpenForPlayback}`（Q_ENUM，
    随 intentMap 暴露 `purpose`）。`ReviewIntent` 增加 per-intent `purpose` 字段（默认
    OpenForReview）；`enqueueStartupRequest` 把 kind 1（PlaySingle）映射为 OpenForPlayback、
    kind 2（Compare）映射为 OpenForReview——不是全局布尔。`finishIntent` 在打开成功终端
    （该终端在真实命令链上已含首帧 ACK）后，仅当 `kind==OpenSourcesIntent && purpose==
    OpenForPlayback && autoplayOnOpen()` 才提交 `review_.play()` 并发新信号
    `openPlaybackRequested(id, started)`；追加/替换意图按构造永不进该分支。播放启动失败经
    Main.qml 显示"视频已打开，但未能开始播放。"。
  - `ReviewPreferencesController.h/.cpp`：新持久化偏好 `autoplayOnOpen`（键
    `review.autoplay-on-open`，缺省 true=保持播放器默认；`ApplicationMenuBar` 视图菜单新增
    可勾选"双击打开后自动播放"，经 `Main.qml` 的 `root.autoplayOnOpen` 平面值绑定镜像，
    与 shortcutPreset 同模式）。
  - `ActiveSourceStrip.qml`：每张源卡片新增 DropArea（objectName `sourceChipDropArea-N`）与
    拖拽落点提示 `sourceChipDropHint-N`（"松开替换 B"高亮浮层）；仅接受单个 URL 的拖拽，
    多文件/非 URL 拖拽在 onEntered 拒绝并继续落给工作区 DropArea（保持打开/追加语义）。
    新信号 `replaceDropped(identity, url)`。
  - `Main.qml`：`dropReplaceSource()` 校验拖入物是单个可读视频（`handleDroppedUrls`），随后
    复用 V02 `performSourceReplacement` 路径（保持槽位数/参考角色/显示位置，失败保留旧会话）；
    工作区拖放浮层副文案补"拖到源卡片上替换该源"，与卡片提示互为解释。拖拽中途移出即可
    取消（无任何暂存），队列中的替换意图仍可用既有 queuedIntentRow 取消。
- **行为契约**：系统双击单视频=OpenForPlayback→首帧确认后自动播放（偏好可关）；应用比较打开/
  Explorer 比较动词=OpenForReview→暂停在起点；添加源=AddSourcesIntent（既有 V02 语义不动）；
  拖到明确卡片=ReplaceSource。单文件打开不创建 A/B/GT 顺序确认对话框（Loader 惰性实例化，
  测试钉死）；历史记录/父目录跟随仅在成功终端后发生（既有 recordCommittedVideo 位置不动，
  该时机已含首帧 ACK）；打开失败保留原会话（既有 synchronizeActiveSources 契约）；
  主窗加载期的外部打开请求沿既有 broker/StartupRequestDispatcher 排队，有界（8）且耐心值
  到期上报，过期结果不反向覆盖（既有测试覆盖，未改动）。
- **验证（本地 dev preset，2026-10-11）**：
  - 新增 `MainQmlContractTests` +4：`StartupPlaybackOpenAutoplaysAfterTheFirstConfirmedFrame`
    （目的参数=OpenForPlayback；终端前 0 个 PlayCommand、成功后恰 1 个；顺序对话框未实例化）、
    `StartupPlaybackAutoplayRespectsThePreferenceOffSwitch`（偏好关→0 个 PlayCommand；菜单项
    勾选态镜像且两次触发 off→on→off）、`StartupCompareOpenStaysPausedForReview`（Compare
    打开成功后仍 0 个 PlayCommand）、`VideoDroppedOnASourceChipReplacesThatSourceInPlace`
    （卡片拖放→OpenComparisonCommand intent=ReplaceSources、preserveDisplayedTime、槽 1 换文件、
    参考角色保留、0 个 PlayCommand）。
  - 变异证据：`out/verification/i01-mutation-evidence/`（M1 丢 purpose 门→review 打开也播放=红；
    M2 丢 play() 调用=红；M3 丢偏好门=红；M4 kind 映射反转=双测红；M5 拖放丢目标身份=红；
    M6 菜单触发写常量=红；全部改坏→红→还原后 4/4 绿）。
  - 回归（全绿）：`ui.MainQmlContractTests`、`ui.ReviewControllerTests`、
    `ui.ReviewPreferencesControllerTests`、`ui.VideoFolderModelTests`、
    `ui.ImageReviewControllerTests`、`ui.application_menu_bar.*`、`ui.active_source_strip.*`
    共 281/281（另 4 项原有 Disabled 未计）；`ui.player_osc` 2 例失败为基线 b35b808 干净树上
    同样复现的环境问题（protocol_chips / scrub-release），与本单无关；`format-check`
    （qmlformat+clang-format 393 文件）与 `lint`（qmllint --max-warnings 0 + clang-tidy）通过。
- **UNVERIFIED（真机项，不宣称）**：真实 Explorer 双击→自动播放的端到端时延与前台激活体验
  （StartupRequestBroker/AllowSetForegroundWindow 路径未在本单重测）；真实拖拽手势下卡片
  落点提示的观感（组件测试用信号级驱动，未仿真 OS 拖放事件）；偏好持久化跨进程重启的落盘
  （ClosedSettingsRepository 测试仓储不做磁盘 IO）。
- **回退**：还原 `ReviewShellController` 的 purpose 字段/枚举/finishIntent 分支与信号、
  `ReviewPreferencesController` 的 autoplayOnOpen、`ActiveSourceStrip` 的卡片 DropArea/提示、
  `Main.qml` 的 dropReplaceSource/浮层文案/菜单绑定即回旧行为（旧行为=双击不自动播放、拖拽
  全走工作区打开/追加）；新增测试同步还原。播放域、broker、GPU 路径零改动，回退无连锁。

## 2026-10-11 双差异显示口径与附近帧可读性 V04

- **范围**：《CompareStation 下一步体验与性能计划 2026-10-11》§5 V04（P1），R3 阶段工单。
  双差异两窗共同口径的确认与 WARP 像素级钉死、附近帧诊断卡的定位用途与同 ROI 连续帧条。
  不改 V03 差异定位入口语义、E04 走带、GPU 渲染后端结构（渲染器零改动：两窗本就共用同一
  `SurfaceRenderState`，口径由共享状态保证，本单补的是 QML 投影与缩略图管线）。
- **问题（基线 9f631b9）**：DualDifference 渲染层已同 GT、同阈值/增益/度量/滤镜/ROI/视口，但 QML
  投影仍按"当前比较对"描述——HUD 图例/已转换横幅/缺帧提示可能命名画布上并不显示的一对
  （参考源缺失时单差异口径甚至不提示，双差异两窗全黑却无解释）；NearbyFramesCard 未加载协议时把
  全部帧标成确定的"超界"；缩略图逐帧 `request()` 走"最新者胜"单槽路径，一列 9 帧只有最后一帧
  被解码、且与时间线悬停互相取消；区间可随持久化记录无限宽；缩略图不声明其来源。
- **交付**：
  - `Main.qml` + `ComparisonViewport.qml`：新增 `dualDifferencePaneSlots/PaneEdges`（与渲染器
    `computeSurfacePanelLayout` 同规则：非参考槽升序，各自对参考槽）。双差异模式下
    `differenceUnavailableDetail` 按真实显示的两窗+参考槽报告缺帧（参考缺失也点名"参考（源 C）"）；
    HUD 图例（`analysisStatusText`）一行合并两窗口径：`双差异 A↔GT · B↔GT` + 时间/空间/像素三维
    精确性按两窗取与 + `两窗共用阈值/增益/色阶`；精确性聚合任一窗无证据即"不可用/未知"，不宣称
    精确或零；非精确横幅聚合两窗原因（去重）。
  - `NearbyFramesCard.qml`：定位窗口钉为 ≤9 格、锚点居中（宽区间收缩，协议摘要同窗裁剪）；未加载
    协议回退 `kind:"unknown"`（未知），不再伪称"超界"（协议已加载时超界仍由协议证据支持）；全部
    缩略图走一次可替换的低优先级批量请求，关闭/重开即取消/重发；新增"区间帧 ↔ 同 ROI 连续帧"
    切换（连续帧条把缩略图按主画布共享 ROI 裁剪显示，裁剪尺寸取自控制器缓存
    `thumbnailSourceSize`，读 QML Image 隐式尺寸会与 sourceClipRect 成环）；表头标注缩略图源
    （主时间线源文件名，不可用时如实标注）；点格仍走 `controller.seekFrame` 精确跳转链。
  - `PreviewThumbnailController.h/.cpp`：新增 `requestBatch(QVariantList)/cancelBatch()/
    thumbnailSourceSize(frame)`。批量 FIFO 一次最多一帧在途，仅在无交互请求待发/在途时排水；
    交互 `request()` 经既有 `cancel(context)` 抢占在途批量任务；结果到达后自动续排剩余帧；
    会话切换/停止清空批量并取消在途（复用既有取消与 generation 机制，不新增解码路径）。
- **不变量**：双差异渲染口径（同 GT、阈值/增益/色阶/有效区域/分析域）由单一 `SurfaceRenderState`
  共享，本单零渲染器改动、以 WARP 像素回读钉死"同固定误差两窗同色同量、跨帧同色"；显示增益不进
  任何统计（ADR-0005 未动，`PairMetricsRequest` 仍无显示参数，阈值复用协议未动）；缩略图批量在
  独立预览解码服务内排队，不进播放管线、不新增常驻解码。
- **验证（本地 dev preset，2026-10-11）**：
  - 新增测试：`PreviewThumbnailControllerTests` +3（批量让位于悬停且被其抢占／取消清队且交互仍可用／
    会话切换丢批量）、`ComparisonSurfaceWarpTests` +2（`DualDifferencePaintsBothPanesWithOneSharedCaliber`
    固定误差两窗同色→跨帧同色→阈值同步两窗→增益 16×同缩放且换帧不重置→换参考槽重定键；
    `DualDifferenceMissingPaneStaysBlackWhileTheOtherPaneRenders` 缺帧窗黑、另一窗照常）、
    `MainQmlContractTests` +5（未加载协议=未知+≤9 锚点居中定位窗／点格=SeekFrameCommand 精确跳转／
    同 ROI 连续帧裁剪+批量请求+关闭取消+缩略图源命名／双差异图例按真实两窗+缺 B 命名／缺参考
    命名"参考（源 C）"）。
  - 回归（全绿）：`ui.MainQmlContractTests` 97/97（4 项原有 Disabled 未计）、
    `platform.ComparisonSurface*` 83/83、`ui.PreviewThumbnailControllerTests` 8/8、
    `ui.ReviewControllerTests` 79/79、`ui.PairMetricsControllerTests`+
    `ui.PairMetricsThresholdReuseTests`+`domain.PixelDifferenceTests`+
    `presentation.ComparisonContractTests` 39/39；`format-check`（qmlformat+clang-format 393 文件）、
    `lint`（qmllint --max-warnings 0 + clang-tidy）通过。
  - 变异证据：`out/verification/v04-dual-diff-nearby-frames/`（M1–M13：缺参考不点名／图例丢双窗头／
    精确性只聚合单窗／未知退回超界／窗口钳制丢失退左对齐／批量退逐帧最新者胜／ROI 裁剪禁用／
    精确跳转退相对步进／批量不让位悬停／取消不清在途／会话切换留队／增益映射改 1×／阈值常关，
    13/13 改坏→红→还原；mutate.ps1 自校验行数与字节还原，附逐项日志与 mutation-results.json）。
- **UNVERIFIED（真机项，不宣称）**：真实 GT+双输出素材下两窗同色同量的观感验证；连续帧条在真实
  解码延迟下的逐步填充体验与 ROI 裁剪清晰度；2/3 源真机走查（组件测试覆盖 2 源会话与 3 源会话
  的图例/缺帧/ROI 路径，非真机 D3D11VA 回放）；缩略图批量在长 GOP 素材上的实际排队时延。
- **回退**：还原 `Main.qml` 双差异分支与 ROI 转发、`ComparisonViewport.qml` 图例/横幅聚合与四个新
  属性、`NearbyFramesCard.qml`（恢复 `kind:"outside"` 回退、逐帧 request、无窗口钳制、无切换）、
  `PreviewThumbnailController` 批量 API 即回旧行为；新增测试同步还原。渲染器与统计路径零改动，
  回退无连锁。

## 2026-10-11 任务化模式入口 V03

- **范围**：《CompareStation 下一步体验与性能计划 2026-10-11》§5 V03（P1），R3 阶段工单。
  顶层按任务组织对比入口；内部复用现有 ViewMode 枚举，不改播放器域模型、渲染后端与 E04 走带。
- **问题（基线 147dd54）**：8 种模式平铺，三联/参考聚焦/分析网格与日常任务混排在"…"溢出菜单；
  V01 交付的局部联动（源纹理直采样）藏在"…"里发现性差；三源差异无 GT 基准引导。
- **交付**（`CompareModeBar.qml` 单文件为主）：
  - 顶层四个一键任务：并排、划线（原"分割线"）、局部联动（新顶层按钮 `roiTripletTaskButton`，
    直达既有 RoiTriplet 预设；2 源两格/3 源三格由 V01 既有路径承担）、差异定位（`diffModeButton`，
    plain 标签从"差异 ▾"改"差异定位 ▾"）。
  - 差异菜单：三源且已识别 GT 时，"双参考差异（GT 基准）"列为首项（默认引导）；无 GT 时该项保持
    可见但禁用并说明"需先设置参考源"（不悄悄选伪基准），两格差异风味继续描述源间差异；
    双源会话不显示该项。差异定位按钮 checked 覆盖 Difference+DualDifference 两族，激活时按钮
    名称带"双参考"风味。
  - 布局子菜单：原"…"溢出改为"布局 ▾"（`layoutModesButton`），内含三联/参考聚焦/分析网格
    （objectName `modeBarThreeUpItem`/`modeBarReferenceFocusItem`/`modeBarAnalysisGridItem`，
    与 ApplicationMenuBar 同名项解耦）；激活布局时按钮显示"布局·三联"等。诊断卡仍按需展开，未动。
- **不变量**：所有入口仅发既有 modeRequested 枚举值；进入任何任务/布局均为纯视图切换
  （不提交播放命令，测试钉死）；ApplicationMenuBar 与 TabbedInspector 未改。
- **验证（本地 dev preset，2026-10-11）**：
  - 新增 `MainQmlContractTests.ModeBarOrganizesTasksWithLayoutSubmenu`（四任务一键可达、
    溢出按钮消失、布局子菜单驱动同一枚举且按钮命名激活布局、局部联动直达预设：surface 映射
    ThreeUp、strip 可见、3 窗、零播放命令）、`DualDifferenceGuidanceNamesTheBaseline`（GT 基准
    命名、进入即纯视图切换、任务按钮覆盖双差异族、peek 不跟随）、
    `DualDifferenceWithoutReferenceStaysExplainedAndDisabled`（无 GT 三源：禁用+说明文字、
    识别参考后转为可用）；既有 RoiTriplet 两测更新为顶层按钮引用，2 源测试加双差异不可见断言。
  - 回归：MainQmlContractTests 92/92、ReviewControllerTests+domain ComparisonSelection/Validator+
    RoiTripletStrip+active_source_strip 共 243/243；format-check、lint 通过。
  - 变异证据：`out/verification/v03-task-entry-mutation/`（VM1–VM5：任务按钮错靶/无 GT 放开/
    布局项错枚举/差异按钮丢双差异族/三源门丢失，全部改坏→失败→还原；VM1 附实例 onClicked
    与内联组件 handler 叠加不覆盖的记录）。
- **UNVERIFIED（真机项，不宣称）**：四任务在 960×640 最小宽度下的实际排版余量（U01 另行
  处理响应式）；差异菜单引导措辞的用户理解度；真机 GT+双输出的双差异观感。
- **回退**：CompareModeBar.qml 还原为"…"溢出+三按钮平铺即回旧入口；测试引用同步还原。
  无 C++ 行为改动，回退无连锁。

## 2026-10-11 统一源入口与比较选择 V02

- **范围**：《CompareStation 下一步体验与性能计划 2026-10-11》§5 V02（P0），R3 阶段工单。
  每路源对等的直看入口、显式管理选择、比较对真实源名、替换文件入口；复用 P05 canvasFocus
  链路，不新增渲染/播放状态；不改 E04 走带、GPU 后端与播放器域模型。
- **问题（基线 104d474）**：直看按钮为 A/B/GT，GT=A 时 A 与 GT 重复、C 无对等入口；
  ActiveSourceStrip 普通点击被管理选择劫持；拖动只改标签条顺序、画面顺序不跟随（误导）；
  比较对下拉显示 A/B/C 槽位字母而非真实文件名。
- **交付**：
  - `CompareModeBar.qml`：直看组改为按会话生成 A/B/C 同等按钮（Repeater，objectName
    `canvasFocus{A,B,C}Button`）；承担参考角色的源在自己的按钮上挂 GT 徽标（`referenceBadge`），
    删除独立"看 GT"按钮——GT 是角色不是第四个源，GT=A 时不再重复、C 有对等入口。
    点击发 `fullCanvasFocusRequested(SourceSlot, slot)`，选中项再点发 Off 返回比较视图（P05 语义不变）。
  - `ActiveSourceStrip.qml`：卡片主体单击=进入/退出该源全画布直看（`directLookRequested(slot)`，
    Main.qml `toggleDirectLook` 转 `shell.requestCanvasFocus`，同帧同 ROI/缩放，不 seek/不重开）；
    新增"☰"管理开关进入显式管理模式，普通点击才变多选，离开模式清空选择；拖动重排整体移除
    （画面顺序跟随会话槽位，标签条单独重排属误导交互；shell `moveSourceInDisplayOrder` API 保留
    供程序化重排，其不变量测试仍在）；源菜单新增"替换文件…"（`replaceRequested(identity)`）。
    直看中的卡片显示"直看"标签并与参考/主时间线/普通态可区分。
  - `Main.qml` + `ReviewInputDialogs.qml`：替换文件走新 `replaceSourceFileDialog`；
    `performSourceReplacement` 以冻结身份定位槽位、拒绝重复文件、`stageSources+openStagedSources(true)`
    走 ReplaceSourcesIntent（保显示位置、保参考角色、失败不拆当前会话）。
  - `ReviewController.cpp`：每条 differenceEdge 新增 `pairLabel`/`firstSourceName`/`secondSourceName`
    （真实文件名，同名附父目录）；比较对下拉改用 `pairLabel`，`label`（槽位字母）保留给既有调用方。
- **不变量（§2.1）**：切换直看/比较对不 seek、不改 GT、不改时间轴/播放位置、不重置 ROI/缩放、
  不重开视频；A/B/C 槽位仍不充当文件身份（直看状态按冻结身份清除，P05 语义）。
- **验证（本地 dev preset，2026-10-11）**：
  - 新增/改写：`MainQmlContractTests.SourceChipDirectLookSwitchesWithoutTouchingMediaTruthOrObservation`
    （卡片单击进入/退出/直切 + 命令数/参考/边缘/帧/epoch/视图模式/显示顺序/ROI/缩放全保持）、
    `PairComboNamesTheTwoRealSourceFiles`（下拉与全部三条边显示真实文件名）、
    `ReviewControllerTests.DifferenceEdgesNameRealSourceFilesForThePairSelector`；
    P05 焦点组测试改为断言 A/B/C 对等 + GT 徽标（原"看 GT"按钮断言随之移除：徽标方案取代独立按钮，
    Reference 请求码仍由 shell/domain 测试覆盖）。`tst_active_source_strip.qml` 重写拖动为
    "拖动不再重排"断言、新增直看点击/管理模式/退出清空/替换菜单/直看标签 8 个用例。
  - 回归：MainQmlContractTests 89/89、ReviewControllerTests+domain ComparisonSelection/Validator+
    RoiTripletStrip 150/150、active_source_strip 8 变体全绿；format-check、lint 通过。
  - 变异证据：`out/verification/v02-source-entry-mutation/`（M1–M9：直看进不去/重置视口/GT 顶替/
    偷步进/偷换比较对/点击退回选择/拖动重排回归/真实名退回字母/Off 失效，全部改坏→失败→还原）。
- **UNVERIFIED（真机项，不宣称）**：三源真实素材下按钮排布与长文件名截断的观感（仅跑了
  960px 最小宽度的既有布局断言）；替换文件对大文件的失败回退体验（意图失败路径与移除一致，
  未做真机坏文件验收）；管理模式触屏可用性。
- **回退**：还原 CompareModeBar 焦点组为静态 A/B/GT 三按钮、ActiveSourceStrip 恢复点击=选择与
  DragHandler、Main.qml 移除 toggleDirectLook/替换接线即回到旧行为；`pairLabel` 字段多余数据
  无行为影响，可独立保留。

## 2026-10-11 原速播放指标与调度守恒 E03

- **范围**：《CompareStation 下一步体验与性能计划 2026-10-11》§4 E03（P0），§2.2/§2.3/§8。
  目标是回答"原速吗、哪一层慢、是否损失了完整帧组"，不是给一个平均 FPS。
- **现状核对（不改写已满足的行为）**：唯一 canonical 媒体时钟已存在——`playbackDue`/
  `playbackTargetAt` 从 canonical 时间轴（RationalRate 或 FrameTimeline，即 PTS/时间基的运行时
  形态）推导计划位置，无"每路 sleep 16ms"式协调；"原速观看 vs 完整帧审查"两口径已存在为
  `PlaybackContinuityPolicy::RealTime`（允许整组追赶、如实计 skip）与 `ReviewEveryFrame`（不跳组、
  时基滑移）。本单只补测量与命名落地，未改调度语义。
- **交付**：
  - 稳态实际速率：运行内"稳态段"累积 Σ媒体时间/Σ墙钟。段从（锚帧，墙锚）起算、终于该段最后一次
    commit 的（帧，墙时刻），循环接缝、restart-from-end、倍速重锚前先封段；暂停后到最后 commit 的
    停留墙钟不进分母；<200ms 的碎片整段丢弃（`playbackRunSteadySegmentCount` 为样本数，0=未测量
    而非速率 0）。倍速变化重置累积（跨请求速率的比值无单一解释）。
  - 迟到帧组：发布时刻晚于计划呈现时刻（playbackDue）的完整帧组计数（run 级 + 会话级），与
    skip（从未发布的整组）分开归因；计划→发布时延 P50/P95/P99/最大值按最近秩百分位采样
    （提前发布计 0 延迟=准备余量，非负时延）。
  - 单一 reservoir 实现（`PlaybackLatencyReservoir`，application 公共头）：PlaybackEvidence 各跳、
    协调器计划→发布、SourceDecodeActor 解码耗时（`DecoderBackendStatus` 新增 decode_p50/p95/p99/
    truncated）共用同一最近秩定义与"截断保大"截断口径；`PlaybackLatencyStats` 增 P50/P99。
  - 分层呈现口径：计划时间（canonical 边界）、已发布时间（RenderPublished）、可确认层=应用呈现
    ACK（PresentationAcknowledged）；交换链/系统呈现无每帧证据→报告 `timing_scope=application_ack`、
    `system_presentation=unknown`，不写零丢帧（PlaybackEvidence 的 kUnknown 语义原样保留）。
  - `Main.cpp` 性能报告新增 `play` 对象（§8.4 示意的落点）：mode（continuity 策略名）、speed_ratio
    （未测量时字符串 "unknown"）、steady_media_us/steady_wall_us/steady_segments、frame_sets_skipped
    （run+lifetime）、late_frame_sets（run+lifetime）、presented_frames、plan_to_publish_*、
    input_response_ms（复用 play→playing 既有 UI 响应测量点，无则 unknown）、gpu_memory_bytes=
    unknown（无真实采样来源，不编造）。缓存摘要复用 E05 frameset_cache_*/cache_*/reverse_window_*
    键，per_source_decode 增 decode_p50/p95/p99/truncated。
  - 60/59.94/VFR：速率计算全部经 RationalRate（60000/1001 非整数微秒帧距）或 FrameTimeline（逐帧
    非均匀间隔）推导，测试分别覆盖；显示器重复刷新不参与任何"源内容重复"判断（无此概念路径）。
- **测试（application 单元 + media 组件，注入时钟/假解码）**：`PlaybackCoordinatorTests` +6：
  CFR60 精确比值 1.0（媒体=墙钟=100000µs 整）、59.94 非整数时间基、16 点不规则 VFR、循环接缝+
  暂停停留排除（2 段精确折叠、+5s 停留不进分母）、迟到组归因（40ms 持有→late=1/skip=0/
  p50=p95=p99=max=40000）、追赶下 late 与 skip 并存（skip=10 且 late=1）。`PlaybackEvidenceTests`
  +2：最近秩 P50/P95/P99（开段与闭段两路径）、无 ACK 证据时呈现层保持未测量（count=0 +
  pending=3 + systemPresentation=kUnknown，不显示 0）。`MultiSourceFrameProviderTests` +1：连续两次
  带偏移请求的发布帧组中每个源都恰在其映射帧（1 路新鲜+2 路旧帧不可能以成功发布）。
  `SourceDecodeActorTests` +1：解码 P50/P95/P99 最近秩（7 样本 p95=p99=max 恒等式、未解码全零）。
- **变异证据**：9 组改坏→红→还原，日志 `out/verification/e03-mutations/`（百分位秩取整改 floor、
  稳态段终点改 now()（5s 停留泄漏）、接缝不封段、准时发布计迟到、计划→发布不采样、快照不复制
  新字段、映射偏移 +1、解码不采样、开段百分位不终化）。
- **既有套件**：全量 475/475（application/media/domain/presentation，隔离 worktree dev preset）。
  另发现基线 104d474 上 `PlaybackTraceSingletonTests.ObserverGateStopsDispatch...` 因 39989de 的
  sink 计数算术自身矛盾而必红（干净基线复现），以独立小提交把两处 sink 期望修正为累加口径
  （1+2=3、3+1=4）；红→绿输出在 e03-mutations/ 目录。
- **UNVERIFIED（真机项，不宣称）**：真实 5 分钟三源 1080p60 稳态（speed_ratio 1.00±1%、零额外整组
  跳过、D3D11VA、物理屏）未跑；59.94/长 GOP/10bit 扩展素材未跑；`play` 对象的真实 GUI 运行输出
  （性能 harness 需要真机窗口）未验证——报告键是接线完成但运行时读数 UNVERIFIED；GUI 输入→
  画面变化的光子级时延无测量点，按口径写 unknown。
- **回退**：删除 `PlaybackLatencyReservoir.h`、还原 PlaybackEvidence/SessionSnapshot/
  PlaybackCoordinator/SourceDecodeActor/DecoderBackend/Main.cpp/ReviewRuntime 即可；无调度语义、
  trace schema、GPU 后端、QML 改动。`PlaybackLatencyStats` 新增字段为尾部追加，序列化兼容。

## 2026-10-11 长按正反向走带 E04 与缓存小步改进 E05

- **范围**：《CompareStation 下一步体验与性能计划 2026-10-11》§4 E04（P0）+ E05（P1，服务 E04），
  R2 阶段两个工单。遵守 §9 警告：本提交不改 Input 生命周期所有权、PlaybackCoordinator 状态所有权
  和 GPU 后端三者中的多项重写——协调器本体仅加一处预取守卫，GPU 后端零改动。
- **E04 交付**：
  - 纯输入层 Jog 状态机 `JogStateMachine`（`src/ui_qml`，无 Qt 依赖、注入时钟）：单击立即 ±1 帧并
    保持暂停；按住 ≥250ms 进入连续走带；松手/取消停止 feeding。走带步进由媒体时钟推进（起始帧 +
    elapsed × 速率 × 帧率），并以呈现帧 +2 的 lead 上界做背压限界——下一帧由呈现决定，不由按键数
    决定；同方向重复按键视为 auto-repeat 忽略，反方向先结束旧方向（协调器侧取消旧流并升代）。
  - `JogKeyHandler`（窗口事件过滤器，`Dvs.Ui/JogKeyHandler` 注册）：裸 A/D、←/→（播放预设加 ,/.）
    压下→jogPress、抬起→jogRelease；auto-repeat 按键被消费且不触发；窗口失活（Alt+Tab）释放全部
    按键；未启用时全部放行（对话框/文本框/图片工作区快捷键不受影响）。ReviewShortcuts 中对应
    per-repeat Shortcut 移除（无法感知 key release 是旧实现依赖 OS 重复时钟的根因），帮助行保留
    并注明"长按连续走带"。
  - `ReviewController` 新增 jogPress/jogRelease/jogCancel/jogRate 与 jogging/jogActive/jogDirection
    投影；步进经同一 PlaybackCoordinator 的 StepFramesCommand ±1 链（单 generation、current+prepared
    流水线、输入 lookahead 上界 3、warm cursor 复用、换向取消旧代），协调器语义零改动。
  - TransportBar 上一帧/下一帧按钮改为 pressed/released/canceled 驱动 jog（与键盘一致，幂等守卫），
    键盘激活（Space/Enter）仍走 clicked 单步回退；Esc（非全屏/有 chrome 时专用 Shortcut）、对话框
    （shortcutsEnabled 翻转 → handler 释放）、会话不可步进（refresh 内 poll 自取消）、窗口关闭
    （Main.qml onClosing）均结束走带。
  - 反向走带复用既有 ADR-003 反向 GOP 窗口（关键帧顺解+缓存+反序呈现），请求窗长 24、上限 48，
    未改 SourceDecodeActor 行为。
- **E05 交付**：
  - 完整帧组缓存（provider setTable_）：命中/未命中/淘汰计数 + 真实条目/字节占用进
    `FrameProviderStatistics`；FIFO 改为访问序 LRU（命中刷新 recency，不只插入时）。
  - 单源缓存（SourceFrameCache）：新增淘汰计数；actor `DecoderBackendStatus` 暴露
    cacheEvictCount/cacheEntryCount/cacheRetainedBytes（实际占用，非"最多 N 帧"口径）。
  - 预取守卫：队列空闲优先级由结构性位置承担——provider 取件序为 control→exact→sequential→
    prefetch（预取只在前列为空时解码）、每次新请求取消旧代预取（cancelOutdatedFrameQueuesLocked）、
    actor read-ahead 遇 urgent work 让位；协调器精确导航完成处的 submitPrefetch 注释钉死该契约
    （该点不可能有播放运行或交互步流活跃）。集缓存对预取保持 source-cache-only（不保留完整帧组）。
  - `Main.cpp` 性能报告补 frameset_cache_misses/evictions/entries/bytes 与 per_source_decode 的
    cache_evictions/cache_entries/cache_bytes/reverse_window_* 键。
- **验收对照（§4 E04/E05、§2.2）**：三类请求分别调度未破坏（原速播放/拖动/精确逐帧路径未动）；
  轻点精确、长按连续、松手即停、鼠标按钮与键盘一致；generation 过期不更新画面由协调器既有
  cancelInteractiveStepRun 升代 + 反向终结测试覆盖。
- **验证（本地 dev preset，2026-10-11）**：
  - 新增 GoogleTest：`JogStateMachineTests` 11 项（轻点单步、跨 250ms 阈值、媒体时钟节奏与
    轮询频率无关、背压 lead 上界、松手停止、边界自取消、auto-repeat 忽略、换向重起、速率钳制
    0.5/2.0、无效方向拒绝、提交被拒不取消）；`JogKeyHandlerTests` 9 项（键位映射、auto-repeat 抑制
    与合成 release 不终止、禁用放行、方向门、修饰键放行、播放预设 ,/. 门、失活释放、禁用释放、
    非监视对象过滤）；`ReviewControllerTests` +5 项（tap 单步且重复按忽略、按住按媒体时钟+lead
    节奏、会话失去步进能力自取消、边界/gate 无副作用、速率投影）；`MultiSourceFrameProviderTests`
    +3 项（访问序 LRU 判别：重访 0 后插入 4 淘汰 1、frame 0 仍命中=第二次命中——FIFO 变异下
    hits 停在 1/misses 增至 6；4 帧往返零重解码零淘汰；9 帧往返淘汰有界且尾部 3 帧命中）；
    `SourceDecodeActorTests` +1 项（三帧缓存五次精确解码 → 占用=3 帧、淘汰=2、最近帧命中、
    最旧帧重解码后再淘汰）。
  - QML：`ui.ReviewActions` +4 项（jog 转发与 manualNavigation、边界门无副作用、release/cancel
    转发、投影读取）；`ui.player_osc` +3 项（按住上一帧按钮 jogPress(-1)+单次 jogRelease、下一帧
    同 + 键盘 clicked 恰好单步、事件序恰为 press→release）。
  - 既有套件回归：ui.ReviewControllerTests 98/98、application.PlaybackCoordinatorTests 90/90、
    media.MultiSourceFrameProviderTests + SourceDecodeActorTests 40/40、ui.MainQmlContractTests 87/87、
    ui.player_osc + ui.ReviewActions 全绿。
  - 变异证据：见 `out/verification/e04-jog-mutation/` 与 `out/verification/e05-cache-mutation/`
    （改坏→失败→还原记录）。
- **UNVERIFIED（真机项，不宣称）**：长按 10 秒真实手感与停止延迟 P95≤100ms（需 D3D11VA 真机）；
  反向冷 GOP 实际准备/走带速度（48 帧是请求上限不是缓存实测，实测经新增 reverse_window_* 与
  cache_* 指标读出）；缓存命中率提升幅度（往返 24 帧与 8/10bit、1/2/3 源矩阵未跑真机）；0.5×/2×
  速率的 UI 选择器未做（能力已暴露为 jogRate 属性，默认 1×）。
- **回退**：恢复 ReviewShortcuts 旧 per-repeat Shortcut、移除 JogKeyHandler 实例与 TransportBar
  pressed/released 接线即回到 OS auto-repeat 行为；LRU 还原为 insertionOrder 比较即回 FIFO；
  预取守卫删除不影响其余路径。协调器除 submitPrefetch 守卫外零改动。

## 2026-10-11 启动关键路径减负 E01

- **范围**：《CompareStation 下一步体验与性能计划 2026-10-11》§4 E01（P0）：呈现能力探测移出
  启动关键路径；StartupMilestone 补齐"主窗可交互"与"素材首帧"。
- **代码证据（基线 a3a06ee）**：`TimingDiagnosticsController` 构造即同步调用
  `evaluateProbeCapability()`（`PresentationStatisticsProbe` 自建隐藏窗口 + D3D11 设备 + 交换链
  并 Present 验证），发生在 `engine->load(Main.qml)` 之前；里程碑已有进程进入/窗口序列，
  缺首帧两个节点。
- **交付**：
  - 控制器构造只建轻量状态：呈现状态初始为"未确认 (未探测)"，不再构造期探测。探针的创建、
    采样与销毁全部移入控制器私有的单工作线程（命令合并 + 单槽结果信箱）；隐藏窗口 HWND、
    D3D11 设备与交换链同线程创建、使用、销毁，不跨线程使用。
  - `Q_INVOKABLE requestPresentationProbe()`：TabbedInspector 在面板可见时请求（守卫式属性
    访问，兼容契约测试 mock）；单飞 + 代次标识，重复请求复用缓存答案；超时（4 s）显示
    "未确认 (探测超时)"，均不阻塞播放；`notePresentationTargetReplaced()` 为窗口/会话交接
    提供失效钩子，过期代次结果丢弃、不改写新状态。
  - 探测完成后的 presentCount/presentRefreshCount 采样同样走工作线程（refresh 2 Hz 拉取）。
  - StartupMilestone 新增 `window-first-frame`（QQuickWindow::frameSwapped 首帧，可交互）与
    `source-first-frame`（ReviewController::frameStateChanged 且 currentFrame ≥ 0）；进程进入
    沿用 Main.cpp 既有打点；后台 shell 注册（jthread）未动。
  - Loader 延迟未新增：startup-performance-v2.md 有实测证据的重组件（ClipExportDialog、
    ReviewInputDialogs+DropConfirmationDialog、ShortcutHelpOverlay、SampleExportDialog）已在
    早前 Step 3/4 Loader 化；其余组件无实测耗时证据，按工单要求跳过。
  - 不改 PlaybackCoordinator 状态所有权与 GPU 渲染后端。
- **验收对照**：无诊断场景启动路径零探测工作（构造测试钉死）；失败/超时显示未确认；过期
  结果不覆盖新状态（变异 B）。真机场景（空启动/单/三文件、物理屏/远控、启动中关闭程序）
  未执行，见"未验证"。
- **验证**（隔离 worktree @ a3a06ee + 本工单文件，避开同工作区并行会话的未提交改动）：
  `ui.TimingDiagnosticsControllerTests` 10/10（新增 3）；`ui.MainQmlContractTests` +
  `ui.DiagnosticsProbeTests` 组合 92/92（4 项既有 Disabled）。变异 3/3：A 构造恢复同步探测 →
  轻量测试红（实测机器返回"可用 (Flip Model)"≠期望"未确认 (未探测)"）；B 去掉代次检查 →
  过期测试红（appliedProbeGeneration 1≠0，过期"窗口不可用"被应用）；C worker 结果不送达 →
  异步应用测试红（5 s 截止、generation 0）。
- **未验证**：真机冷/热启动的 DVS_STARTUP_TIMING 里程碑数值对比、启动完成前关闭程序、远控
  虚拟屏下的探测表现；探测超时分支无自动化断言（无事件循环的测试环境不触发 QTimer）。
- **回退**：还原 TimingDiagnosticsController.h/.cpp、DesktopApplication.cpp 与 TabbedInspector
  一处即可回到构造期同步探测；无其他调用方。

## 2026-10-11 诊断真正可关闭 E02

- **范围**：同计划 §4 E02（P0）：拆清采集／汇总／UI 刷新／能力探测四个状态；停止监测真的
  停止新增诊断 ingest；不误伤其他 trace sink；默认不常驻详细采样。
- **代码证据（基线 a3a06ee）**：`setMonitoring(false)` 只停 QTimer；observer 仍挂接且
  `onTraceEvent` 无过滤；`PlaybackTrace::record` 持 observer shared lock 回调，collector 的
  ingest mutex 进入解码/调度事件生产路径；控制器构造经 `installObserver` 自动 enable 全局
  trace 时钟（默认常驻采样）。
- **交付**：
  - `PlaybackTrace::setObserverEnabled(bool)`：原子门，`record()` 在取 observer 锁**之前**
    acquire 检查（release/acquire 配对保证可见性）；只作用于 observer 分发，ring buffer 与
    已装 sink 不受影响；`reset()` 恢复默认开。
  - 控制器四状态拆分：采集（共享门 + `monitoring_` 原子双保险，`onTraceEvent` 入口 acquire
    再查）；汇总（collector 仅监测开启时收到事件）；UI 刷新（500 ms QTimer = 2 Hz 聚合快照，
    仅监测开启时运行，无每帧 QVariantMap 分配）；能力探测（E01 worker，独立于监测开关）。
  - `monitoring_` 默认 false：构造不再 installObserver/enable——普通运行零每事件诊断工作
    （时钟禁用时 record 仅剩一次原子读 + 分支）；首次 `setMonitoring(true)` 才安装 observer
    （沿用其 enable 契约）；关闭时先关门再翻标志，时钟与 sink 不动；析构关门 → 卸载
    （unique_lock 等待在途回调）→ 恢复门默认。
  - 未引入事件交接队列：本轮无实测证据表明同步处理占事件生产预算（未跑真机预算测量），按
    工单"否则保持简单"执行；QML 快照维持 2 Hz。
- **验收对照**：monitoring=false 后诊断事件数不增长（测试 + 变异 D）；sink 并存不受影响
  （测试）；连续开关 100 次、在途事件、monitoring 开启时销毁（并发 stress 测试）。
- **验证**（同一 worktree）：`ui.TimingDiagnosticsControllerTests` 10/10（新增 4）；`application.
  PlaybackTraceTests` 8/8（新增 3）；焦点组合 `ui.TimingDiagnosticsControllerTests |
  application.PlaybackTraceTests | application.PlaybackEvidenceTests` 28/28；format-check
  （386 文件）与 lint 通过。变异：D 移除 record 门 → 单元测试红（分发未停）；E 移除控制器
  回调检查 → "门重开仍拒绝"红；G 卸载不加锁 → "卸载等待在途回调"红（50 ms 超时断言立即
  返回）。**变异 F（控制器析构围栏整体移除）未检出**：非 ASAN debug 构建下销毁后分发落入
  未复用栈内存、虚表与原子字节未被破坏，UB 恰好无害执行；该保证由 `installObserver` 的
  unique_lock 纪律提供（变异 G 的确定性测试覆盖），后续可用 ASAN 变体验证。
- **未验证**：真机关闭诊断前后的帧时/跳帧差异（E02 验收的"采样开/关帧时和跳帧差异"）；
  队列丢失计数（未实现队列，无此指标）。
- **回退**：还原 PlaybackTrace.h/.cpp 与控制器开关语义即可；门默认开，既有 sink 与 CI 门禁
  行为不变。

## 2026-10-11 图片编辑纯操作提取 P11

- **范围**：路线附录 B 工单 P11（S01 第一步"把已有图像编辑能力逐步提取为纯操作+参数"）。
- **交付**：`src/ui_qml` 新增 `ImageEditOperations.h/.cpp`——crop/水平镜像/垂直翻转/±90°/180° 旋转/
  矩形填充/清除/马赛克纯函数 + `effectiveMosaicBlockSize` + `EditRecipe`（五操作 variant）有序折叠
  执行器雏形；`ImageEditController` 五处（CropCommand::redo、transformImage、mosaic/fill/clearImageRect）
  全部委托纯函数，标注几何、撤销历史与编辑副本身份保持控制器所有；CMake 两处登记。解决"预览与未来
  批量导出各写一份像素规则"：同一输入+参数经同一条纯函数得到同一输出，配方重放不依赖控制器内部状态。
- **验收对照**：附录 B"原图不改、预览/导出一致"——原图只读由构造保证（纯函数返回新缓冲）；预览=纯函数
  路径有逐字节对照测试；旋转为精确像素置换而非插值有全像素置换断言。
- **验证**：`ImageEditOperationsTests` 8/8（空图/越界/空交集、格式与 alpha 保持、mosaic 钳制区间、
  recipe 折叠=逐步手折、控制器路径与纯函数逐字节一致、5 步控制器序列经 recipe 重放一致）；本提交前
  同树组合电池 273/273 中 ImageEdit 系列全绿（2026-10-11 早前会话另有 50/50 控制器套件、57/57 QML
  契约回归，见 `out/verification/edit-recipe-ops/`）。变异 6/6 检出（镜像轴、裁剪 off-by-one、
  Source→SourceOver、配方逆序、块钳制、共享缓冲别名；既有控制器测试随新模块变异同步变红，证明委托
  无内联残留），源码 sha256 逐字节还原。
- **刻意不做**：画笔/标注/智能抠图（交互态/模型环境）；缩放（`ResampleCommand`）与填充画布
  （`CanvasCommand`）本轮未提取、`EditOperation` 不含；`EditRecipe` 序列化与批量导出执行器未做
  （纯数据就绪，无半成品调用方）。
- **回退**：单图编辑行为零变化（同一 Qt 原生像素调用），还原控制器委托即可恢复；模块无其他调用方。


## 2026-10-11 全画布候选直看 P05

- **范围**：路线附录 B 工单 P05（V02 同帧候选切换）：全画布 A/B/GT 直看，同帧、同 ROI、零回放命令。
- **交付**：domain `CanvasFocusRequest`/`resolveCanvasFocusTarget`（无参考 GT 请求与越界槽位**拒绝而不替代**）；
  shell `canvasFocusIdentity`/`canvasFocusSlot`/`requestCanvasFocus`（纯呈现态，零回放命令提交，焦点按冻结
  源身份保存，源离开活动集清除而非改指向接槽者）；模式条 看A/看B/看GT（无 GT 隐藏、重按取消、checked
  镜像 shell 槽位）；Main.qml 正式接线（绑定 + 信号转发，契约测试自带 relay 仍独立有效）；画布渲染：
  `ComparisonSurface.canvasFocusSlot`（-1=off，越界拒绝）进入 `PresentationOptions`/`SurfaceRenderState`，
  平台几何（`computeSurfacePresentationGeometry` 聚焦态单面板全画布、清空差异/Wipe 矩形，QML 覆盖层经
  `sourcePanelRects` 自然一致）与渲染路径（render() 最高优先分支，与 GT peek 同构的 aspect fit+letterbox，
  聚焦槽缺帧画显式黑不回退其它源）。聚焦期隐藏分屏线/Wipe 手柄/ROI 三联条（ROI 本体保留）；沉浸字母与
  源标签随单面板自然聚焦；像素读数映射聚焦源并在切换瞬间挂起重取（宽高比/旋转可不同）。
- **验收对照**：附录 B"不 seek、不改 GT"——契约测试钉死切换前后命令数零增量、无 SeekFrame、参考角色/
  比较对/epoch/帧号/视图模式/ROI/缩放/平移全部保持；协调器级零命令位稳定测试（对照注入 seek 立即扰动
  同一身份，证明断言非空转）；两源无 GT 会话仅显 A/B、参考请求拒绝；单源隐藏整条模式栏。
- **验证**：组合电池 273/273（ComparisonSelection 11、PlaybackCoordinator 100、ComparisonSurface 69、
  MainQmlContractTests 85/85+4 既有 Disabled、ImageEdit 系列含 ImageEditOperations 8）；qmllint 通过。
  变异 8/8（`out/verification/p05-mutation/`：setter 守卫、几何 focus 分支、isValid 范围、render 分派、
  Main 三处接线、几何传参；红/绿 16 相全录、恢复后字节一致）。真机 D3D11VA 手感与性能未跑（WARP 已覆盖
  渲染正确性：Wipe 模式聚焦逐槽亮度单调、退出恢复合成、ACK 单发）。
- **未接/未做**：盲比随机种子与同区间顺序播放比较（V02 其余部分）；真机走查与截图。


## 2026-10-11 协议导航 P03、三局部预设 P04 与呈现探测 P10（未提交）

- **范围**：路线附录 B 工单 P03/P04/P10，三代理并行 + 续作收尾（首轮三代理全部中途死亡：
  P03/P04 被取消但留下大部分成果，P10 模型请求失败零痕迹后重派成功）。基线：SAM2 并行
  会话工作完成（未提交），全树构建与全量测试通过后才开工。
- **P03 交付**：`InterpolationProtocolRepository`（persistence_json，schema_version=1，
  decimation/explicit 双形式，JSON 0 起计；未知版本/kind/坏 JSON/域校验失败各自明确拒绝，
  suggestion 来源拒绝导入）+ `InterpolationProtocolController`（加载失败保持既有协议、
  清空即回退）+ 走带条"下一/上一生成帧"与来源徽标（文字："保留帧/生成帧·输入 GT x/y·比例
  n/d/未知/缺帧"，1 起计显示与内部 0 起计分离）+ Main.qml/DesktopApplication/Main.cpp 接线。
  跳帧经现有 coordinator 命令链；无协议时入口不可用；原速播放路径零变化。
- **P04 交付**：`RoiTriplet` 视图模式（ViewMode=8，surface 走 ThreeUp 三列全景保三源全呈现）
  + `RoiTripletStrip` 三个同 ROI 局部窗（同一已呈现 FrameSet 的 QML 复采样，**无新增解码
  路数**；烘焙 .qsb 着色器含 d3d11/HLSL50 变体）。续作修出两个前任真实缺陷：①映射到
  SideBySide 时三源场景 C 源根本不被呈现（固定两列布局）；②Qt 6 RHI 的 ShaderEffect 不接受
  内联 GLSL（解析成 qrc 相对 URL），三个局部窗此前渲染为空——改为 Vulkan 风格源 + qsb 工具
  烘焙签入。模式往返/缩放/平移 ROI 保持，半开边界钳制，无三源入口禁用，进出预设不 seek。
- **V01 交付（局部窗直采样改造）**：删除 P04 的 flattened snapshot 复采样路径（.qsb 快照
  shader 已删），局部窗改为 `D3d11ComparisonRenderer` 对同一已呈现 FrameSet 的附加绘制：
  strip 暂存窗口 contentRect → `SurfaceRenderState.localWindows` → 共用纯函数
  `surfaceLocalWindowGeometry`（半开钳制定向 ROI、整数裁剪、SAR 横向、100/200/400% 物理
  档位）→ 同一 render() 同一组 SRV 的 NV12 附加绘制（同一 ACK；视频常量缓冲 3→6 组、逐绘制
  采样器；`alignLocalWindowFootage` 整物理像素对齐保 1px 棋盘 texel 精确）。缩放命名改为
  适配ROI/源像素100%/200%/400%，100%=纵向源像素 1:1（横向按 SAR）；QML 不再持有采样公式；
  全景 ROI 轮廓改由 surface 经 `surfacePanelRoiRect` 发布；2 源（SideBySide 全景）与 3 源
  （ThreeUp 全景）均可用。WARP 回读覆盖棋盘 texel 精确、全景缩放不变性、90° 旋转、SAR、
  奇数宽末列；变异验证见提交说明。真机 D3D11VA 与高 DPI 走查保持未验证。
- **P10 交付**：`PresentationStatisticsProbe`（platform_windows，pimpl 无 D3D 类型泄漏）：
  自有离屏 flip 交换链 + 硬件优先/WARP 回退设备，七态分级能力 + Media 统计四态细分；
  "必须亲见 PresentCount 递增才报可用"；快照只读（PresentCount/SyncRefresh/QPC）；
  `evidenceScope` 声明与 Qt 场景仅**区间级关联**。本机实测：硬件路径可用、Media 统计
  kMediaAvailable。未接线（回退语义天然满足）。
- **验证**：最终全量 **1155/1155 通过**。分项：P03 39/39（controller 8、导入 17、domain 11、
  player_osc、契约）变异 6/6；P04 MainQmlContractTests 80/80、RoiTripletStrip 14/14，变异
  6/6（自断言脚本 8 项中 7 过——"viewport restore"一项因基线清理+事后 qmllint 修复不可比，
  已在 mutation-summary.md 补记功能证明：六变异检出器全绿）；P10 10/10×3 连跑，变异 5/5。
  证据：out/verification/{protocol-navigation,roi-triplet-preset,presentation-probe}/。
- **主会话修复**：P10 一个环境敏感测试（`CapabilityReportsExplicitGradedStatusAndEvidenceScope`
  断言真实机器上 Media 统计应不可用）——远控虚拟屏下传统计数器滞后于探针验证窗口、Media
  路径却可用，探针诚实报 kUnknown 而测试写死了机器答案；改为结构性断言（media 对内一致、
  事后采样必须带显式分级），三连跑稳定。教训：对真实硬件能力只能断言分级纪律，不能断言
  具体答案。
- **未验证/未接**：P03 真机走带条走查与截图；P04 真机视觉/多 DPI/非 d3d11 RHI 后端、
  跟随轨迹放大镜（后续 V01 增强）；P10 全屏/MPO/HW Flip Queue（DwmFlush 依赖仅文档依据）、
  真无头会话、PresentMon/ETW；P09 收集器与 P10 探针到运行时/UI 的接线属后续 D01 完整工单。
- **状态**：P03/P04 已实现并接线主界面（走查未做）；P10 PoC 已实现未接线。能力对照见
  [能力台账](capability-ledger.md)。

## 2026-10-10 精确样本导出 P08 与播放时序证据 P09（未提交）

- **范围**：路线附录 B 工单 P08（E01 执行层）与 P09（D01 应用层摘要）。实现由并行子代理完成
  （首轮代理在验证前被取消，二轮验收代理接手），主会话完成源码完整性抽查与文档。
- **P08 交付**：`src/application` 新增 `SampleExportWriter.h` 端口（`ISampleExporter` 阻塞式
  perform + 协作取消，仿 `IClipExporter`）；`src/media_ffmpeg` 新增 `SampleExportWriter`
  （display-order 帧序号精确抽帧，复用 `SoftwareDecoder`；YUV→RGB24 如实记录"解码转换值"；
  固定裁剪整组一致；staging→写 4 PNG + manifest→**读回校验**→rename 原子发布，取消/失败/
  校验失败只清理本次 staging；manifest 按 A4 子集，0 起计索引 + index_base，不伪造指纹字段）。
  **PNG 编码实走 Windows WIC**：仓库锁定 FFmpeg 8.1.2 构建不含 png 编码器（vcpkg 构建日志
  证实），avcodec 路径保留为运行时探测回退、在本树未验证。组件测试覆盖 VFR、multigop、
  非零起点、损坏文件、取消、裁剪、manifest 回读、重复请求清理。
- **P09 交付**：`src/application` 新增 `PlaybackEvidence.h/.cpp`——`PlaybackEvidenceCollector`
  增量消费 `TraceEvent`，按 PlaybackRunStarted/Stopped 开合 run，分段延迟（提交→解码完成、
  →帧组就绪、渲染调度、绘制→ACK、发布→呈现确认，count/max/最近秩 P95），完整帧组跳帧
  **按帧号缺口**统计；源 PTS 级输出不可用、系统呈现级输出未知，不伪造零；session/epoch/run
  身份过滤旧事件，run=0 计入非播放窗口；有界内存（约 200 KiB 上界）。被动消费者，
  不 ingest 即关闭采样。
- **验收对照**：P08 验收项 B 帧/VFR/取消中——VFR、multigop、取消已测；**B 帧路径未验证**
  （fixtures 无 B 帧素材，测试内已声明 coverage note）。P09 验收"注入延迟定位正确"——
  各段注入延迟分别归因到正确阶段，旧 epoch/run 排除，单边事件不产生伪延迟。
- **验证**：`media.SampleExportWriterTests` 11/11、`media.(ClipExport|SampleExport)` 回归
  23/23；`application.PlaybackEvidenceTests` 13/13、全 application 189/189 零失败。
  变异证据 P08 6/6、P09 6/6（协议完整，逐字节还原，主会话对还原结果做了 cmp 抽查）。
  证据：`out/verification/sample-export-writer/`、`out/verification/playback-evidence/`。
  代理修复：P08 测试的 COM 公寓生命周期崩溃（`CoUninitialize` 后释放 WIC ComPtr）、两处
  tidy；P09 两处 tidy。格式化后源码与变异基线的差异经主会话逐行核对仅为格式化与声明过的
  tidy 修复。
- **未验证/未接**：P08——B 帧、10-bit 转换分支、avcodec PNG 路径、staged manifest 读回
  破坏分支（记入 mutation-summary 覆盖缺口）、UI 入口；P09——运行时 trace 流接线与
  D01 面板 UI、系统呈现采集（P10）。
- **状态**：P08 执行层已实现（入口未接）；P09 收集器已实现（未接线）。能力对照见
  [能力台账](capability-ledger.md)。

## 2026-10-10 三元组导出规划 P07 与标签几何变换 P12（未提交）

- **范围**：路线附录 B 工单 P07（E01 纯规划层）与 P12（S02 纯几何模块），由两个并行子代理实现、
  主会话验收收尾。P04（同 ROI 三局部预设）因需改 ui_qml（该层被并行 SAM2 会话占用且编译失败）顺延。
- **P07 交付**：`src/application` 新增 `SampleExport.h`／`SampleExportPlanner.cpp`：
  `planSampleTriplet` 把一个插帧生成帧变成可验证的训练三元组计划（input0／GT 目标／input1 的
  GT 索引、最简时间比例、候选帧自身位置、映射来源、可选固定裁剪 ROI 整组一致）。
  拒绝矩阵：非 Generated 来源（保留／未知／缺失各自明确报错）、隐含 GT 位置非整帧（VFR/59.94
  场景拒绝而非取整）、防御性区间断言、ROI 负值／非正宽高／溢出／越界。纯数据计划，不写文件，
  不做 JSON 序列化（P08/E01 范围）。
- **P12 交付**：`src/domain` 新增 `LabelGeometry.h`／`.cpp`：光流端点通式
  `f'(T0(x)) = T1(x+f(x)) - T0(x)` 与公共仿射捷径（向量乘线性部分，测试断言两者一致）；
  按网格位裁剪＋终点出界（半开边界）置无效；水平翻转镜像＋dx 取反（等距论证：不重查终点）；
  pad 新区域零向量＋无效（不制造真值，有效性永不升级）；mask 精确索引重排（类别与连续同规则）；
  落在像素中心之外／出界的样本丢弃而非重采样；塌缩变换明确拒绝。时间反转与 mask 仿射按路线
  红线**不提供**（逐像素取负是错误反向流；mask 重采样是独立决策），注释中写明原因。
- **验收对照**：P07——60→30→60 相位 0 与 60→20→60 的 1/3、2/3 时刻按已知清单逐字段核对，
  起点/终点/映射验证齐备。P12——解析平移/旋转/缩放、非对称裁剪、出界、反向流方向保留、
  单像素目标、平移逆＋双翻转往返全部覆盖。
- **验证**：`application.SampleExportPlannerTests` 7/7、`domain.LabelGeometryTests` 15/15；
  全量 domain 86/86、application 168/168 零回归。变异证据：P07 6/6、P12 6/6（P12 脚本自带
  检查数断言并通过），分别见 `out/verification/sample-export-planner/` 与
  `out/verification/label-geometry/`。六个新文件 clang-format 干净；两个实现文件 clang-tidy
  以 lint 目标同参数零告警。
- **过程记录**：两个子代理在 10 分钟无活动后被终止——P07 源码停留在 m6 变异未还原状态
  （已从 `.orig` 逐字节还原并补跑 m6，DETECTED）；P12 变异脚本已备好但从未运行（含一次
  /WX 下不可能编译的无效变异 m4，替换为"端点忘记流向量"后整脚本重跑通过，历史保留在
  `run-mutations-first-attempt.log`）。并行会话长时间持有构建互斥锁，本轮所有构建经
  wrapper 锁重试完成。
- **状态**：P07 规划层已实现（UI 入口与写入器未接，属 P08/E01）；P12 纯模块已实现
  （配方执行器接线属 S01）。能力对照见 [能力台账](capability-ledger.md)。

## 2026-10-10 插帧协议纯数据结构 P02（未提交）

- **范围**：[长期产品与技术路线](../CompareStation_长期产品与技术路线_2026-10-10.md) 工单 P02／F01 核心。
  新增 `src/domain` 的 `InterpolationProtocol`：GT 与输出索引强类型分离（`GtFrameIndex` 与 `FrameId`
  不可混用）、输出帧来源四态（Retained／Generated／Unknown／Missing）、映射来源标记
  （ImportedManifest／Manual／Suggestion，建议不作为已确认映射）、创建期整表校验（严格递增的 GT
  隐含位置、目标时间比例严格位于两输入之间且约化为最简分数、计数与索引界限）、生成帧导航查询
  （跳过 Missing／Unknown）与声明式降帧构造器（相位／步长／首尾策略：裁剪或标记缺帧）。
  配套错误码 `kInterpolationProtocolInvalid` 与操作 `kInterpolationProtocol`。
- **验收对照**：两种保留相位（60→30→60 相位 0／1）、多生成时刻（60→20→60 的 1/3、2/3）、缺帧
  （尾段 Missing 与中段缺一帧）全部按已知清单逐项核对；59.94／VFR／起点偏移样例属 P03 导入范围。
- **验证**：`domain.InterpolationProtocolTests` 11/11 通过；8 个受控变异全部被检出后源码逐字节恢复
  （`out/verification/interpolation-protocol/`）。`format-check` 对本轮文件干净；clang-tidy 以 lint
  目标同参数对 `InterpolationProtocol.cpp`／`MediaError.cpp` 通过且零告警。全量构建／全量 lint 因
  工作区另一会话的 SAM2 接入（`Sam2Segmenter.cpp` 编译失败、`onnxruntime/` 第三方头未过格式检查）
  当前无法通过，与本轮改动无关，提交前需与该会话协调。
- **构建环境事故**：一次未走 wrapper 的裸 `cmake --build` 在 Git Bash 环境下触发重生成并损坏
  `out/build/dev` 缓存（`CMAKE_AR` 变成 `/bin/Host/lib.exe`），已按诊断提示 `-Fresh` 重建恢复。
  与 AGENTS.md 的要求一致：构建必须经 `tools/build/build.ps1`。
- **状态**：domain 层已实现；协议导入、UI 生成帧导航未接线（P03）。能力对照快照见
  [能力台账](capability-ledger.md)。

## 2026-10-10 单图／裁剪十字线（未提交）

- **范围**：在 `069cb425` 及前两轮未提交修复上继续完善图片观察；默认单图查看、手动 A/B 单图
  与轻编辑画布共用已有十字线，保留并排双图联动。十字线与坐标牌不接管鼠标手势。
- **坐标语义**：左上角为 `(0, 0)`，按像素向下取整，与原图 RGBA 读数一致；仅在已加载图片内显示，
  留白或图片范围外清除悬停。并排不同尺寸图片继续按归一化位置联动，但各侧标签使用自己的像素尺寸。
  线条限定在图片与画布的可见交集，细暗色衬线使亮背景上的位置也可辨认。
- **裁剪与视图更新**：按下与拖动时更新当前位置；图像几何、来源和加载状态变化时合并刷新悬停，
  即使鼠标停住，缩放、平移、连续裁剪、旋转及撤销后也重新映射到眼前图片。
  编辑态底部明确显示「编辑副本 · 像素」，不把不可变原图的 RGBA 值套用到工作副本坐标。
- **回归**：新增两项原生 Qt 主窗鼠标回归，覆盖单图像素与 RGBA 一致、留白／最末像素、静止鼠标下
  缩放平移、图片部分移出画布时的线段范围、手动 B、不同尺寸双图、裁剪起点／拖动、第二次裁剪、
  副本标签与旋转／撤销。窗口输入先呈现最新 Qt Quick 布局，拖动事件携带真实按住的左键状态。
  12/12 个行为变异被检出且源码逐字节恢复；格式检查与 QML 零警告检查通过。
  初步两项按下调用变异被 Qt 的位置更新纠正，没有破坏最终行为；对应日志及说明一并保留，
  未当作被检出的变异计数。
- **证据**：`out/verification/image-single-crosshair/` 保留两张已检查的主窗夹具截图、
  `mutation-results.json`、`mutation-notes.txt`、格式与 QML 日志。Release 全量测试、更新 ZIP 的
  逐文件差异、解压验证与独立启动探针也在同一目录记录；不据此宣称真实 GPU 的长期播放性能。

## 2026-10-10 图片连续裁剪与整图变换（未提交）

- **基线与范围**：HEAD 仍为 `069cb425`，分支 `integrate/pr45-49`，保留上一轮未提交的一致性修复。
  本轮沿图片轻编辑链路诊断连续裁剪，增加水平镜像、垂直翻转、顺／逆时针 90° 和 180° 旋转。
- **诊断**：控制器原本正确裁剪当前 `working` 缓冲；两次内框裁剪的完整像素、坐标累加与撤销／重做正常。
  新 QML 回归在原实现发现留白边界缺陷：起点 `(-5,-4)` 到 `(25,18)` 被记录成起点 `(0,0)`、
  尺寸 `30×22`，应为 `25×18`。失败日志为 `out/verification/image-crop-transforms/crop-boundary-diagnosis.log`。
- **修复**：`updateCropSelection` 对矩形工具先钳制两端到当前副本范围，之后计算宽高；箭头仍保留原始方向。
  历史提交与撤销／重做会清除旧选区。身份牌新增「编辑副本」及当前像素尺寸，避免裁剪后仍显示原图尺寸。
- **整图变换**：`ImageEditController::transformImage` 变换实际工作缓冲，复用有界历史和事务另存入口；
  变换命令同时保存前后缓冲并将两份计入历史预算，失败不提交。文字保留方向矩阵，标注几何跟随像素，
  变换后仍可选中、删除及撤销；未完成的笔画或标注拖动期间拒绝操作。
- **界面**：编辑栏新增「旋转 / 翻转」菜单，五项动作明确标出方向；成功后适应窗口。
  支持裁剪→变换→再次裁剪及逐步撤销，原图文件与比较统计继续使用已提交原图。
  最终走查补上编辑态的临时单图投影：从差异模式进入编辑时立即显示副本，底层比较模式保持不变，
  退出编辑后恢复；编辑 B 时锁定 B，即使审查侧切换 A/B，也不把工具重定向到别的图片。
  编辑期间 A/B 切换入口、徽章及快捷入口共用可用状态，避免画布编辑 A 而审查徽章仍写「当前 B」。
- **验证与边界**：新增像素级、16 位样本／透明度、历史预算、未完成手势、标注方向与命中、连续裁剪和
  菜单接线共 6 项回归。相关回归选中 276 项：270 通过、2 既有跳过、4 既有禁用，零失败；
  29/29 受控变异均触发真实回归失败，源码逐字节恢复；`format-check`、完整 C++ lint 与最终 QML lint
  复检全部通过（零 QML 警告）。最终相关回归在补上编辑态绑定后再次通过。
  证据保存在 `out/verification/image-crop-transforms/`，包括 `final-regression.log`、`mutation-results.json`
  与 960px 工具栏／菜单截图。窗口鼠标回归先呈现更新后的 Qt Quick 布局，再发送事件。
  任意角度旋转与视频编码变换不在本轮范围，超大图变换耗时尚未做真机性能矩阵。

## 2026-10-09 一致性轮：读数对应眼前画面、恢复单入口、GT 直看语义（未提交）

- **评审来源**：对 `main@069cb42` 的代码级复审提出"下一轮从增加功能转向保证功能之间一致"，本轮按最小范围
  修掉其中四项：取色数值来源与帧身份、恢复单入口与终态、GT 直看语义、取色播放预算。评审中的 12 个 Node
  重放场景（同坐标旧读数被接受、播放鼠标路径仍发请求、恢复第二阶段缺少换源保护）在本轮均有对应修复与回归。
- **取色数值来源与标签（包 1）**：读数面板从"源码值 YCbCr"改名为"规范化帧值 YCbCr"，Accessible 名称与
  tooltip 说明读数是当前 NV12/P010 帧平面中的样本、非 4:2:0 来源在解码时已被重采样；逐源在行尾追加
  「已转换」标记（按来源格式判断，并分别展示缓冲位深、来源格式和范围；未知信息明确写未知）。
- **取色帧身份（包 1）**：`D3d11ComparisonRenderer` 新增 `presentedSet()`/`presentedFrameIdentity()`，
  探针只读取**已绘制的 front publication**（呈现确认背压或邮箱清空后仍是屏幕上的那一组），
  从未绘制时返回 no-frame；一次批量锁定一份帧组，跨渲染轮的子集重试也保持首次身份，A/B/C 因此必然同帧。
  `ComparisonSurface` 新增 `presentedFrameValid/FrameId/SessionEpoch/PlaybackGeneration` 四个属性，
  由渲染节点在呈现帧组变化时经队列发布（仅变化时发布）。QML 侧 `acceptPixelProbeResult` 现在校验
  交付结果自带的 frameId/sessionEpoch/playbackGeneration 与**当前**呈现身份一致，迟到结果按帧丢弃；
  成功与失败都携带 requestId 和实际帧身份；UI 同时核对当前请求、帧、会话及播放代际，未标识结果直接拒绝。
  取消或更换渲染服务后，队列里的旧交付也被丢弃。保留下来的旧值改显示
  「冻结于第 N 帧 · …」，不再冒充当前读数。
- **恢复单入口与终态（包 2）**：问题列表点击不再自己调用 `applyIssueRestore`——`IssueLogController::
  restoreIssue` 已发出 `restoreRequested`，Connections 块是唯一执行入口，一次点击因此只打开一次记录来源。
  打开、比较对、seek 都记录各自的命令 ID，消费 `ReviewController::commandFinished` 中匹配的终态；
  每阶段绑定会话 epoch（包括同 URL 重新打开），只提交一次比较对并等待成功终态与已提交配对一致。
  打开或 seek 失败立即结束，未知或过期终态不推进恢复；15 秒无完成回调明确报告超时。
  失败打开不会把先前同 URL 会话当作成功；比较对失败时提示"部分恢复"。图片恢复分支补一条
  "部分恢复"提示：记录里的图片视图状态（模式/缩放/平移）目前不重放。
- **GT 直看语义（包 3）**：`differenceSuppressed` 不再画"当前比较对的第一路"，改为画 `state.referenceSlot`
  （会话的 GT 源，与 ReferenceFocus 主面板同一槽位）；该槽位不可用时画黑而不是画错帧。快捷键、工具栏
  按钮与提示文字统一指向 GT：按钮文案改"按住看 GT 原图"/"GT 原图·已锁定"，`differencePeekAvailable`
  增加"会话确实标识了 GT 槽位"的门槛，无 GT 时快捷键与按钮禁用并说明原因；锁定状态在视口新增常驻
  `differencePeekIndicator`（"直看 GT：B · clip.mp4"/"已锁定直看 GT：…"），不再只靠一条会消失的 HUD 消息；
  GT 身份失效（换会话）时自动解除直看。
  命中映射同时改用 GT 自己的长宽比和旋转；切换直看会撤销旧悬停读数，下一次指针映射后重新取样。
- **取色播放预算（包 4）**：播放开始即取消在途查询，冻结已有值；播放中移动光标不会提交新探针。
  去重依据逐源整数像素坐标及呈现身份，同一像素内的小幅移动不会追加回读；暂停或呈现身份变化可刷新。
  渲染器使用固定三槽、每槽 Y/UV 两张 1×1 staging 纹理，格式不变时复用；同一请求只复制一次，
  每批新复制提交一次异步 Flush，后续以 `D3D11_MAP_FLAG_DO_NOT_WAIT` 检查完成。
  DeviceBusy 以 4 ms 定时更新最多重试 3 次（共 4 次尝试），
  parked render loop 会主动请求下一次更新；取消／完成后释放查询对完整帧组的引用。未新增 GPU 工作线程。
- **顺带修掉的真实缺陷（先由测试暴露）**：`ReviewController::publishProjection()` 的导航可用性查找
  在 `failClosed()` 路径下解引用空 `snapshot_`（源切换导致快照不一致时，控制器构造即崩溃）。
  已加空值保护，回归 `ReviewControllerTests.InconsistentFirstSnapshotFailsClosedWithoutDereferencingIt`，
  变异（去掉保护）1/1 检出。
- **变异证据**：本轮受控变异 23/23 均触发对应回归失败。脚本校验每个源码锚点及总检查数，
  区分测试失败与构建失败，并在每轮后按字节还原源码。证据保留于
  `out/verification/069cb42-consistency/mutations.py`、`mutation-results.json` 及逐项日志：
  - 取色 7 项：请求 ID、帧/会话/播放身份、播放中迟到结果、播放中提交、整数像素去重、冻结标签、转换标签。
  - 恢复 7 项：打开失败终态、比较对等待与失败、seek 失败终态、同 URL 新会话、超时、真实列表点击重复入口。
  - 端到端契约 9 项：命令终态转发、已呈现帧组选择、GT A/B/C 目标、P010 码值右移、跨轮批次锁定、请求替换、
    GT 长宽比、GT 旋转坐标、切换直看撤销旧悬停读数。
- **验证（本机 MSVC + WARP）**：dev 全量 CTest 选择 1046 项，1039 项通过、3 项既有 skipped、
  4 项既有 disabled，0 失败（`full-test.log`）。其后完善非阻塞回读的提交/轮询、取消与光标状态路径，
  最终平台与主窗相关回归 304 项通过、4 项既有 disabled（`final-regression-complete.log`），0 失败，
  包含新增的跨轮批次身份与 GT 几何回归。最终 format-check 通过（347 个 C++ 文件及 QML），lint 通过
  （QML 零告警；C++ 按仓库现有检查配置），见 `final-format-check-complete.log`、`final-lint-complete.log`。
  GT 锁定标识的 960×640 QML 夹具截图已检查；其黑色画布不是实际视频渲染验收。
- **未验收**：真实 GPU 性能（探针批次/资源创建/读取等待对播放路径的影响）、真实素材与全 App 键鼠走查、
  完整图片观察条件恢复、发布门禁。本轮不宣称 FPS 或延迟变化；既有 `6058d19` 用户走查结论及范围保留。

## 2026-10-09 评审优先级第 4 项：动作/菜单/帮助同源（10e72a7）

- ShortcutCatalog.js 成为工作区级快捷键序列的单一来源：Main.qml 绑定、ApplicationMenuBar 文案、
  帮助表工作区行三方引用同一常量；M 键"记录当前问题"补上此前缺失的菜单文案。
- 契约测试 `ShortcutBindingsMenuAndHelpStayInSync` 三向交叉：菜单文案必须对应实际绑定（Alt+F4 为
  登记过的菜单-only 例外，窗口管理器负责投递）、工作区帮助行必须是活绑定且出现在两个 preset、
  同一序列不得被绑定两次（Esc 为登记过的演示/抽屉重叠，二者启用条件互斥）。
- 变异证据：菜单错换常量、Ctrl+O 重复绑定、帮助行缺失均被断言检出后恢复。
- 键位审计确认：Ctrl+Shift+F 已被图片文件夹比较占用，后续帧时序浮层（原计划 P2-11）需另选键。

## 2026-10-09 外部评审三问题修复（记录-恢复闭环、采样口径、M 冻结）

- **评审来源**：对 `main@89c819d` 的代码级复审提出三个"立即重开"问题，逐条对照当前代码核实后全部属实，另发现一个评审未覆盖的潜伏缺陷。
- **修复 1（`12d2774`）**：视频问题记录补存 `viewScale` 与 `referenceSourceIndex`（旧记录 zoom 默认 1.0 会让 `restoreViewport` 把中心合法区塌缩为 0.5、整条拒绝；`openSources` 第二参实为参考槽却收到时间线主源）；恢复改为分阶段状态机——等打开呈现→应用比较对/模式→跳帧并等待呈现→再恢复视口，手动导航或源不匹配即取消。顺带修复：模式重放写的是 C++ 访问器名 `viewModeCode` 而非 QML 属性 `viewMode`，静默失败。
- **修复 2（`919d528`）**：Auto 采样阈值从 fit 相对 `viewScale>=2.0` 改为物理倍率（新增 `physicalScalePercent`，由视口徽章 `effectivePercent` 馈给，含 DPR/ROI/内容），小素材被窗口放大 300% 现在正确切最近邻，大素材缩放到物理 1:1 不再误切。
- **修复 3（`8ab6af8`）**：M 键按下即捕获记录（帧/源/ROI/缩放/比较对全部冻结），备注弹窗只通过新增 `attachNote` 补文字；输入期间播放推进不再改变记录内容。
- **验证**：三修复各带契约/属性测试与变异证据（身份错换、缩放丢失、判定回退、延迟捕获均被断言检出）；MainQmlContract/IssueLog/ViewportPixelScale/ComparisonSurface 相关 80+ 项 0 失败；lint、format-check 绿。既有偶发：`ImageEditModeCropsAWorkingCopyAndKeepsTheOriginal` 在套件顺序下偶发图片 provider 竞速，单测与复跑均绿，与本轮改动路径无关，待单独跟踪。

## 2026-10-09 真机交互验收轮（第 2–3 周启动，用户走查通过）

- **范围**：`main @ 6058d19`（dev 构建，`--startup-check` exit 0）上按当日体验清单全量走查：
  week-1 四项（视口恢复、>200% 最近邻与采样徽章、M 键备注框、快捷键帮助）、差异阈值渲染三项
  （Highlight 透 A 路、SignedSubtract 中灰、阈值不吞 Fade）、文件侧栏两页与右键（含禁用态说明）、
  竖屏倍率与一键真实尺寸（含 PR 49 fit 以下原生缩放）、PR 50/51 交互项（检查器关闭按钮、对话框层序）。
- **结果**：用户确认全部符合预期，未发现回归。上文各条目"留待交互会话／实窗走查"的悬置项据此核销：
  竖屏倍率（实窗/DPI）、侧栏右键（全 App 流程）、同目录与历史侧栏、阈值过滤真机观感。
- **边界**：多容器导出矩阵（TS 起始关键帧、MKV 惰性索引、末 GOP、帧号舍入、非零 PTS）本轮未逐项
  复核，仍按各条目原有证据边界执行；WARP 像素回读原生测试未运行，相关条目状态不变。

## 2026-10-08 PR #42–#44 合入状态（完整 Windows 工作流仍延后）

- 已核对 `main @ ba594923aac26891bb998e785cf515d02ca2e57b`：
  [#42](https://github.com/sonwe1e/VideoCompareStation/pull/42) 的 SignedSubtract 阈值中灰、
  [#43](https://github.com/sonwe1e/VideoCompareStation/pull/43) 的 VFR 区间导出入口、
  [#44](https://github.com/sonwe1e/VideoCompareStation/pull/44) 的 RGBA8 指标口径说明均已合入。
- 下方各条保留原基线与有限验证证据；合入不代表原生渲染、真实素材、完整 Windows
  工作流或发布验收通过，未执行项和延后边界不变。

## 2026-10-08 PR #38–#41 Windows 原生验证轮（四项合并 + 三处测试基建修复）

- **合并**：#38 视口倍率、#39 侧栏键盘目标、#40 来源条拖拽排序、#41 倒退请求放行按
  `chore(merge)` 依次合入 main；#41 与 #38 的同日台账条目在本页顶部冲突，两条都保留解决。
  四个 PR 均带 CI skip 标记，Windows 侧验证由本轮补齐。
- **Windows 全量门禁**：MSVC 19.44 / Qt 6.11.1 / D3D11。dev 预设全量 ctest 962 项注册、
  0 失败（7 skipped/disabled 为既有状态）；四个 PR 的定向集合 107/107（协调器/预取 92、
  视口倍率 5、侧栏 2×19、来源条 8×26）；format-check 346 文件、lint 通过。
- **测试基建修复 ×3**（均先红后绿）：
  1. 来源条套件在 Windows 真实窗口下单轮约 21 s，超出沿用的 20 s 超时，8/8 判 Timeout；
     TIMEOUT 放宽到 60 s（与侧栏套件一致）后 8/8 通过（21–23 s）。
  2. 侧栏上下文菜单 `opened=true` 时内容列首帧布局未完成（探针逐帧采样：contentHeight
     0→70 需约 50 ms，两种控件风格同样发生），菜单项鼠标点击落空——原套件在本机 10/15
     轮固定 4 例红（compare 计数 0 vs 1）。测试内在菜单项点击前等待内容列布局
     （settleContextMenu）后 24/24（12 轮 × Basic/Windows）。探针与红例证据在
     out/verification/sidebar-probe/（一次性，不入套件）。
  3. ViewportPixelScaleTests 缺少 MainQmlContractTests 同款的 `addImportPath(bin/qml)`，
     引擎默认导入路径找不到 QtQuick.Controls（module is not installed，5/5 秒败）；
     补齐后 5/5。
  另：PlaybackCoordinatorTests.cpp 在 #41 的提交里未过 clang-format 19.1.5（当时 CI 被
  skip 未拦下），用仓库 format 目标重排 35 行纯格式后 92/92 复验通过。
- **全 App 交互层限制**：本轮会话为无头桌面——窗口截图全黑、SendInput/PostMessage 与
  UIA Invoke 均无法驱动播放导航（transport 全禁用），而同一打开链路在 `--ui-smoke` 与
  popup-pixels 门禁里全绿，故判定为会话环境限制而非产品回归。真实键鼠的全 App 逐帧
  交互验收留待有显示会话时执行；探针脚本与记录在 out/verification/ui-probe/。
- **结论**：#38/#39/#40/#41 的行为改动在 Windows 上经真实窗口组件与真实协调器测试全部
  验证通过；本轮不宣称交互式全 App 操作与性能门禁（后者由标签触发的 CI 执行）。
- **同日晚间续：自托管 runner 的 CI 余量修复**。四个 PR 带 skip 标记合入，main 推送
  首次在自托管 runner 上跑它们的套件：真实窗口 QML 比开发机慢 3–4 倍，来源条一项实测
  77.8 s、四个 MainQmlContract 弹窗/鼠标用例实测 21.7–27.0 s，先后越过 60 s/20 s 边界
  （本地同套件 962 项 0 失败）。按 runner 实测放宽：MainQmlContract 20→120 s、来源条
  60→180 s（d137aa1）。放宽后 runner 上仅剩 `CopyComparisonButtonPutsLabeledViewportOn
  Clipboard` 一次真失败：断开的桌面会话里 `grabWindow()` 返回空，生产路径自报
  「无法抓取当前画面」而剪贴板保持哨兵图；测试改为仅在该自报捕获失败时 GTEST_SKIP
  （记录状态），自报「已复制」而剪贴板不对仍为硬断言。开发机与附着会话的 runner 上
  全断言照常执行。
- **同日再续：ui-dev 模型-only 配置自 #37 起一直红**。侧栏 QML 测试注册块落在
  `if(DVS_BUILD_ADAPTERS)` 守卫之外，ui-dev（无适配器、无 qmltestrunner）全新配置在
  `add_test` 失败，六个测试 NOT_BUILT——a0a0440 起每次 main 推送的 Debug 作业都有
  这一条，因开发机只跑全量 dev 预设而未暴露。已把侧栏块挪入守卫（与 vcs_tool_tip 等
  同语义：无适配器不注册）；dev 预设 962 项注册不变。该步骤不在发布流水线内，不影响
  标签门禁。守卫修复让配置越过后又暴露第二层：`IssueLogControllerTests.cpp` 包含
  适配器头 `dvs/persistence/IssueRecordRepository.h`，已挪入同守卫的 target_sources
  （1abdf43），适配器开启时 5 项测试照常注册且通过。
- **本轮低效点与改进方案已归档**：skip-CI 合入的连锁暴露、三轮标签重打、单 runner
  串行、监控盲区等成本核算与 P1–P3 改进项见
  [CI 与发布流水线效率改进](ci-release-efficiency.md)；P0（纯文档推送跳过构建矩阵）
  已随文档落地。

## 2026-10-08 小视频真实 100%（候选，Windows 待验）

用户已合入 PR #45–#49 后仍复现点击 100% 无效：旧的 Fit 下限使小视频不能缩小。
本轮候选让 below-Fit 的采样范围与目标矩形分离，保留 ROI、居中显示并一致处理留白命中；
同时处理分数 DPR / 奇数窗口投影取整与原始像素相位。点击只设定当前窗口的 100%，
resize 后继续沿用相对 Fit 的旧行为。详细范围、红例、有限验证和原生待验项目见
[视口倍率记录](viewport-pixel-scale.md)。
当前不关闭问题，不以 Linux 组件通过代替 Windows/D3D11、真实媒体与性能验收。


## 2026-10-07 连续倒退的 pending 请求互相取消（修复草稿，Windows 待验）

- **实锤**：第 7 帧尚未完成解码时，后继 -1 提前提交第 6 帧；provider 的 Reverse 与
  Exact 共用 latest-wins 槽，排队 / active 两条取消分支都能撤销协调器仍需要的当前帧。
  两个输入各一次 Canceled，显示停在 8；同 generation 的旧 Fake 测试未覆盖取消策略。
- **修复**：仅协调器 Reverse 后继等待当前 `providerSucceeded`，Success 后立即预备，
  保留 ACK 前解码重叠；正向、Exact / seek、真实 provider / decoder 与身份校验不改。
- **证据**：正式纯应用 / 预取 92/92，20 轮 1,840/1,840；独立逐字策略重放的两个红例
  转绿，连同 ACK 前对照 4/4、100 轮 400/400。不是运行真实 provider 或视频解码。
- **边界**：Windows / Main / shell / 实播 / GPU / 全构建 / 格式 lint 与性能门禁未验。
  不宣称 FPS 或延迟提升；详见 [倒退请求放行与验证](reverse-step-admission.md)。
- **2026-10-08 Windows 原生验证**：合并后 MSVC/Qt 6.11.1 全量 962 项 0 失败，协调器/
  预取定向 92/92（含本条目的连续 7→6→5→4 与取消终态用例）；全 App 交互步进受无头
  会话限制未执行，见同日验证轮条目。

## 2026-10-07 竖屏视频倍率与一键真实尺寸（观看体验，待 Windows 验收）

- **复现**：旧徽标把面板留白／分割遮罩计入视频宽度，并固定读取槽位 0。800 × 600
  视口显示 1080 × 1920 竖屏时，约 31.15% 被报成 73.89%；点击后仍不是物理 1:1。
- **修复**：复用真实内容几何，按当前首显示源、ROI、旋转及 DPR 计算；移动分割线不
  改变倍率。非方形像素显示横/纵读数，64× 上限与原缩放/平移交互保持。
- **证据**：真实无窗口 Qt 6.8.2 组件新测试 5/5，原 main 5/5 失败；既有几何/属性
  定向 17/17，总计 22/22。23/23 实现变异、7/7 观测 guard、DPR 15 组/45 检查及
  2/2 DPR 变异通过。无设备服务；未验 Windows/MSVC、固定 Qt、实窗/DPI、全套门禁。
- **详细范围与原生复核**：[视频视口倍率](viewport-pixel-scale.md)。不修改 workflow，
  不手动触发或重跑 CI；本地候选待独立审查与 Draft 发布，不能把零 CI 当通过。
- **2026-10-08 Windows 原生验证**：真实视口组件 5/5（含补齐 bin/qml 导入路径后），
  随全量 962 项 0 失败；真实实窗/DPI 切换仍留待交互会话。

## 2026-10-07 文件侧栏右键操作（V-08 增补，待 Windows 验收）

- **同日键盘体验增补**：两页把键盘目标细边框与当前播放项填色分开；委托焦点同步列表
  索引，Enter 与键盘菜单作用到可见目标，失焦保留索引且不打开文件。原 main 新 6 场景
  全失败，最终 Qt 6.8.2 / Basic / offscreen / software 正式套件 19/19（17 行为 +
  init/cleanup）；21/21 实现变异、5/5 观测 guard 检出。Windows 主窗/控件风格、
  完整门禁与真实文件打开未验。见[键盘范围与离屏截图](video-folder-browser.md#2026-10-07-键盘目标与播放项分开显示待-windows-原生验收)。
- **2026-10-08 Windows 原生验证**：Basic/Windows 两种风格真实窗口套件 19/19×2（菜单
  内容列布局竞速已由 settleContextMenu 修复，红例 10/15 → 绿 24/24）；随全量 962 项
  0 失败。真实文件打开的全 App 侧栏流程留待交互会话。

- **入口**：当前文件夹／最近打开两页统一右键，明确区分「打开为新单视频任务」与
  「加入当前视频对比…」；后者在无视频、满三源、重复／不可读文件及忙状态禁用并说明原因。
- **安全语义**：追加预选当前真实 GT，复用顺序／参考确认；Escape／取消不改源。
  shell 冻结已提交源、候选文件和参考身份，在确认及出队时复核；文件改变、新会话、
  旧窗口、重复接受不能默默替换比较组。原来的拖入流程不改。
- **验证**：Linux QtCore 模型／偏好 32/32、controller/shell 69/69；真实 QtQuick 侧栏
  11 项行为场景通过（另有 init/cleanup）。独立真实 Popup 接线探针覆盖接受／Escape／
  普通拖入，完整 Main 新契约只完成语法编译。Windows／完整 build／格式与 lint 门禁未跑。
- **依据**：[右键操作范围与证据](video-folder-browser.md#2026-10-07-侧栏右键打开与加入对比待-windows-原生验收)。
  独立续接 Draft #37；不改 workflow、不触发或重跑 CI，不合并／部署。

## 2026-10-07 打开文件后显示同目录与最近历史（V-08 增补，待 Windows 验收）

- **用户入口**：普通单视频打开成功后自动带出父目录；复用可折叠侧栏，加「当前文件夹 /
  最近打开」两页，当前文件高亮、完整路径提示、点击走既有单视频打开／匹配终态后播放。
- **历史与状态安全**：只保存最近 50 个成功本地单视频；复用异步设置持久化，启动晚加载
  合并历史且不覆盖其他偏好，失败不改历史／当前画面。自动列目录不拆 GT／候选会话；
  A→B→A 旧扫描、更新的排队 intent、缺失文件和扫描／打开错误分别有回归。
- **验证边界**：Linux QtCore 模型／偏好 30 项、QtQuick 隔离组件 4 项行为测试已执行；
  真实 shell/controller 使用仅处理三处既有平台整数差异的验证副本。新增完整 Main 契约
  尚未在 Windows 执行，不能把 Linux 组件截图当作完整播放器实窗或真实素材验收。
- **证据与复核**：[文件与历史侧栏说明](video-folder-browser.md#2026-10-07-文件与历史侧栏增补待-windows-原生验收)。
  未触发／重跑 CI，不修改工作流；后续 Windows build、format-check、lint、定向及完整
  测试绑定最终提交复核后再推进验收状态。

## 2026-10-07 当前帧坏点占比行（V-07，已修复并通过 Windows 原生验证）

- **基线与复现**：main `267adca6114c912796465944a51468598b8f0762`。
  `TabbedInspector.qml::metricsRows()` 将坏点占比的完整标签作为字符串混入二元数组列表，
  实际 Repeater 的 `[0]` / `[1]` 因而得到「坏」/「点」，已经计算的百分比未显示。
- **修复**：仅将该行恢复成 `[完整策略／阈值标签, ratioText]`。不改评分、阈值、策略或
  百分比计算；保持原来零值和小于 0.01% 均显示 `< 0.01%` 的约定，不额外改显示口径。
- **原云端验证（PR 原有记录）**：Node.js 24.19.0 提取并执行生产的两个 JavaScript 函数及两个 delegate 表达式，
  **108 组组合／3,361 项检查**通过；原版在同一探针有 **438 项断言失败**。
  覆盖三种策略、0/16/255 阈值、25%／零／微小值／0.01% 边界／100%／重复切换、
  对齐说明、不可用／采样／不可比及恢复。**15/15** 运行时语义变异检出；语法失败不计检出。
- **Windows 原生回归**：在 MSVC 19.44、固定 Qt 6.11.1 上构建 Debug 和 Release；
  `ui.MainQmlContractTests.PairMetricsMismatchRowPreservesPolicyAndPercentage` 在真实
  Main/Inspector 中验证 18 次投影的行结构、完整标签／百分比及实际 delegate 文本，两种构建均通过。
  定向命令：`pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex '^ui[.]MainQmlContractTests[.]PairMetricsMismatchRowPreservesPolicyAndPercentage$'`。
- **原生变异证据**：15/15 项运行时变异检出，原字符串回退在 `row.isArray()` 断言失败；
  缺值／缺行／重复行、策略／阈值、比例单位／边界／零值／陈旧值、delegate 索引等错误均由
  原生断言拒绝。前后控制组 2/2 通过；故意减少至 14 项时固定数量 guard 在改源前拒绝执行。
  QML 字节恢复并重建后复验通过，构建／语法失败不计检出。
- **门禁**：默认 dev CTest 选中 860 项：853 通过、3 既有 skipped、4 既有 disabled、0 failed
  （233.26s）；Release 的主界面／指标控制器／像素规则相关套件选中 82 项：78 通过、
  4 既有 disabled、0 failed（46.64s）。完整 format-check 和 lint 通过；新增测试三处换行
  按 clang-format 19.1.5 作最小修正，未改变断言。
- **实窗证据**：真实 Qt Main/Inspector 在 1280／960 逻辑宽度、当前显示器 175% 缩放下，
  完整策略／阈值标签与 `25.00%` 均可见。指标是原生测试注入值，截图证明显示／布局，
  不冒充真实视频评分或 GPU 播放证据。见 [原生指标行截图](assets/pr36-inspector-mismatch-percentage.png)。
  临时捕获代码已按字节恢复并重建；详细日志与变异结果保存在
  `out/verification/pr36-20261007/`。
- **构建路径限制**：深隔离目录使格式检查命令达到 39,669 字符，超过 Windows 32,767
  字符上限；改用 `out/p36` 并 `-Fresh` 重建后通过，不改检查范围或放宽门禁。
- **边界**：本轮验收并合并仅覆盖 PR #36 的指标行显示问题；硬件性能与打包发布未运行。
  原云端 `qsTr` / `String.arg` 替身结果与上述 Windows 原生证据分开记录；没有改 workflow。

## 2026-10-07 硬件门禁双源失速：会话恢复移动基线帧（已修复并验证）

- **复现**：`v2.2.0` 标签两次触发发布，Quality 与 Hardware 均红。`hardware.wipe-2source`／
  `diff-2source`／`mode-switch-retained-frame` 在 2-3 秒内以 `comparison-mode-lost-frame`
  失败，`decoder_calls=0`、`presented_frames=0`；单源 `rotated-padded-wipe` 正常通过。
- **根因**：`Main.qml` 的 C1 会话恢复 `attemptResume()` 在打开后把播放头 seek 到设置文档里
  保存的位置。门禁夹具路径固定，此前 300 秒跑批留下非零恢复位置（本机与 runner 均有），
  切换比较模式后 `currentFrame` 从基线 0 跳到 7831。全新机器无存储位置，不会复现——这是
  951 项本地测试全绿仍翻车的原因。诊断插曲：以 `39be3a6` 作二分中点不可靠，该提交在
  GitHub PR36 线上、不含本地功能线的恢复功能；最终以 `3f88d8e`（PR #37 合并）定位。
- **修复**：`DesktopApplicationOptions.performanceAutomation` 由 `runPerformance` 打开并
  发布为 QML 上下文属性 `dvsPerformanceAutomation`；`attemptResume()` 在自动化下直接返回。
  恢复仍是用户会话行为，门禁恢复确定性帧零基线。
- **变异证据**：守卫即差异本体——无守卫的 main 三项双源门禁全红（runner 两次、本地一次，
  症状逐位一致），加守卫后同机同夹具重跑。
- **连带修复**：① 工作流 8 处 checkout 补 `lfs: true`，构建型作业后置 `git lfs checkout`
  防 LFS 指针被当图标编译（`RC2175: comparestation.ico is not in 3.00 format`）及旧工作区
  smudge 竞态；② `dvs_format_check_cpp` 改走生成文件列表 + `cmake/CheckCppFormat.cmake`
  逐文件执行，消除深路径下超 Windows 32,767 字符上限的 "cannot execute" 失败，与 QML 侧
  既有模式对齐；空列表直接失败，缺锚不会静默通过；顺带以 format 目标修正合并带入的
  5 个文件格式违规。
- **门禁**：本机 release 构建通过；`ui.MainQmlContractTests` 53/53（4 disabled 既有）；
  format-check 345 文件通过；硬件门禁加守卫后 **6/6 通过**（wipe 317.9s／diff 318.4s／
  mode-switch 317.8s／rotated 317.6s，300 秒全速播放）；lint 未本地运行，由 CI 执行。

## 2026-10-07 MPEG-TS 定位越过已选关键帧（修复草稿，Windows 验收待完成）

- **复现**：Draft #34 `cfd3c5c` 的 90 帧／GOP30／B 帧 TS，完整 0–89 请求却成功
  导出 30–89；35–49 的短请求失败为空。规划器已正确对齐，DTS seek 却越过目标关键包。
- **修复范围**：仅 MPEG-TS 顺序读取压缩包，严格匹配与规划器相同的归零关键帧时间；
  找不到时失败并保留目标，不选择下一个 GOP。其他容器、B 帧尾部和原子发布行为保持。
- **验证**：定向聚合 65/65；原 writer 的新增 6 项全红；15/15 运行时变异检出；
  独立 36 次新旧导出对照与 18 个最终导出的源帧解码哈希匹配，取消／失败保留原文件。
  详见 [TS 起点回归与代价](clip-export-transport-stream.md)。
- **边界**：晚区间增加一次线性前缀读取；未知原点回退、时间戳不连续等仍有限制。
  Windows／固定依赖、完整构建／CTest／lint／GUI／GPU／ZIP 未验收。下面保留各历史轮次
  当时的限制，本节只推进已经实测的 TS 路径，不宣称所有容器均可用。

## 2026-10-07 Matroska 惰性索引漏报关键帧（修复草稿，Windows 验收待完成）

- **复现**：PR #34 `121e3660` 的 90 帧／GOP15 MKV 请求第 83–89 帧，却从第 0 帧
  导出全部 90 帧。FFmpeg 在首轮 seek 才载入剩余 cues，但 writer 只循环最初的一个索引项。
- **修复范围**：每轮查询当前索引条目数，读取 seek 后新载入的关键帧；保留已有字节位置
  校验、重复前缀的单次向前扫描、实际 PTS、取消检查和原子发布逻辑。无 UI 或播放行为改动。
- **本轮证据与边界**：云端 GCC14／FFmpeg7.1.5 实际 remux 后，同一请求正确从第 75 帧
  关键帧导出 15 帧；解码像素哈希逐帧等于源 75–89 帧。详见
  [惰性索引回归与验证](clip-export-lazy-index.md)。
  Windows／固定 FFmpeg8.1.2 与 Qt6.11.1、完整构建／CTest／lint／GUI／GPU／ZIP 未验收。
  MPEG-TS 的 PTS／DTS seek、粗容器时钟、缺少包位置仍是独立边界，不据此声明所有容器可用。

## 2026-10-06 导出起始帧元数据舍入（修复草稿，Windows 验收待完成）

- **复现**：PR #34 的 `d6a5f4e6` 在 24000/1001 fps 下实际从第 16 帧导出，但规划器的
  `firstExportedFrame` 因 667333 µs 舍入报告第 15 帧；29.97／59.94 等帧率也可触发。
- **修复范围**：仅在下帧的精确有理数时间经最近微秒舍入后等于关键包时间时修正帧号。
  保留规范时间的 ceiling／floor、关键帧选择、导出字节和 VFR 记录时间查找；当前 Qt 完成
  文案未使用该字段，因此不声称修改了可见界面。粗容器时间量化不属于本次修复。
- **验证**：原生产规划器 7 个舍入子例失败；56/56 定向 C++ 测试通过，含 36 次真实 remux。
  独立检查 42 次导出，原版 12 个元数据不符修复为 0，所有输出与原版逐字节一致。
  详见 [帧号舍入证据与边界](clip-export-frame-metadata.md)。
- **未验收**：Windows／固定 FFmpeg8.1.2 与 Qt、原生文件系统、完整构建／lint／GUI／发布门禁。
  不修改 writer 或原子发布器，不扩展 VFR 或 MPEG-TS 支持。

## 2026-10-06 最后 GOP 关键帧漏报（修复草稿，Windows 验收待完成）

- **复现**：PR #34 原 head `76a17eb1` 的 90 帧／GOP15 MP4 漏掉第 75 帧关键帧，
  请求 83–89 帧却从第 60 帧导出 30 帧；修复后从第 75 帧导出 15 帧。
- **修复范围**：仅修正索引关键帧查找；seek 后按索引字节位置继续读取选定视频流的关键包，
  使用真实 PTS。保留时间原点、B 帧结尾参考包、导出时钟与现有交互。
- **验证**：3 个新增参数化回归覆盖 18 次单／多 GOP、零／正／分数起点的首中尾导出；
  原版全部 3 项失败，最终 13/13 定向 GoogleTest、11/11 编译后运行时变异检出。
  独立检查 74 次导出＋16 次取消保留原文件通过。详见 [末 GOP 证据](clip-export-final-keyframe.md)。
- **代价与边界**：增加压缩包读取；5／10 分钟小素材测得线性读量，无逐关键帧重扫前缀。
  不声称高码率或 Windows 性能已验收；缺少字节位置、惰性 Matroska 索引、TS 起始 seek、
  粗容器时钟仍有限制；最近微秒的规划器帧号舍入由上方独立补丁修复。
  完整 Windows／固定依赖／GUI 门禁未运行。

## 2026-10-06 非零起始 PTS 的片段导出（修复草稿，Windows 验收待完成）

- **确认缺陷**：归零的时间轴区间直接对齐容器原始关键帧 PTS。现有 +1 秒起点的 12 帧素材
  请求 0–0.4 秒时被拒绝为空；原 main 的生产 writer 在全部 3 个新增回归中失败。
- **修复**：媒体适配器统一首帧时间原点，关键帧／完成报告归零；seek 与出点边界恢复为选定
  视频流的原始 ticks；进度使用相同原点。未知起点只在导出 worker 上做可取消的 demux 扫描。
- **已验证**：云端 GCC14／FFmpeg7.1.5 下直接编译生产代码，既有规划 7/7＋新增导出 3/3 通过；
  零起点／正起点／VFR 内核的首中尾 9/9，78 个导出解码帧逐帧像素哈希匹配；13/13 运行时
  断言变异检出，源码恢复读回后再通过。详见 [时间原点修复证据](clip-export-time-origin.md)。
- **边界**：未运行 Windows/MSVC、项目锁定 FFmpeg8.1.2、Qt GUI、完整 CTest、lint／format、
  硬件／性能门禁或 ZIP。VFR 的 GUI 入口限制仍待单独处理；当时的旧文件 remove＋rename
  故障窗口已由后续[原子发布补丁](clip-export-publication.md)处理，原生 Windows 验收仍待完成；
  未据内部 VFR 导出探针声称界面已支持。PR 保持草稿，不作为发布验收。
- **独立审查**：90 帧／GOP15／30000/1001 的 MP4，零／+5 秒／+5.000133 秒起点共 21/21
  导出帧身份与结果等价检查通过；42/42 导出前／中途取消保留目标原字节且无 partial。
  当时另确认「最后 GOP 关键帧漏报」（现由上方独立补丁修复已验证的 MP4 路径）与
  「MPEG-TS seek 跳过起始关键帧而导出失败」（仍未修）；不声称负 PTS／所有容器均已验收。
## 2026-10-06 高亮模式的阈值背景（修复草稿，原生渲染验收待完成）

- **复现**：`Nv12ToRgb.hlsl` 的共享阈值分支在进入 Highlight 前返回黑色。
  因而启用正阈值后，未达到阈值的区域丢失 A 路背景，而不是只隐藏红色 tint；
  原来的绝对差异黑底测试未覆盖「原图叠加高亮」。
- **修复范围**：只在 Highlight 的阈值拒绝分支返回经 clamp 和 opacity 预乘的 A 路。
  三种阈值策略、原始差值准入、等于阈值时通过、增益与 tint 保持原样；
  其余差异模式仍返回黑色。不改素材、CPU 评分、解码、接线、布局或片段导出。
- **云端验证**：用 C++ 向量替身编译执行提取的生产 shader 算术主体，
  172,800 组通过，其中 151,200 组验证其余七种模式与旧版相同；
  旧版有 3,720 组 Highlight 期望失败。11 项编译后的运行时变异均被检出。
  此证据不运行 HLSL 编译器、纹理采样、Qt、D3D11 或真实 GPU。
- **原生回归已添加但未运行**：`ComparisonSurfaceWarpTests.HighlightThresholdPreservesFirstSourceAndOpacity`
  覆盖有颜色的 A 背景、三种策略、1.0／0.5 opacity、阈值拒绝／通过／关闭及重复切换；
  共 24 次状态下的像素期望。原生断言的变异验证仍待运行，不能用 CPU 替身冒充。
  Windows 上定向执行：
  `pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex 'platform.ComparisonSurfaceWarpTests.(HighlightThreshold|ThresholdMask)'`。
- **未验收**：Windows/MSVC、项目固定依赖、原生 HLSL/WARP 编译与像素回读、
  format-check／lint／完整 CTest、真实 GPU 性能、实窗截图及发布门禁。
## 2026-10-08 带符号差异的阈值中性值（#42 已合入，原生渲染未验收）

- **基线与改动**：`880d02ad` 已保留 Highlight 的 A 路背景，但 SignedSubtract 的阈值
  拒绝分支仍为黑色，与该模式「中灰表示零差异」冲突。仅让 mode 5 在此分支返回
  0.5 中灰，并保留 opacity 的 clamp、预乘和 alpha；其他模式、准入谓词与增益不变。
- **云端有限验证**：GCC 14.2 编译执行提取的生产 shader 算术主体，172,800 个算术组合
  通过（21,600 个 SignedSubtract 组合；其余 151,200 个与基线一致）。相同输入下基线
  有 3,720 个 SignedSubtract 期望失败；9/9 编译后运行时变异被检出。计数有显式守卫。
  采样由向量替身提供，只有 scalar `.xxx` 改为构造器；不验证 HLSL、纹理或 GPU。
- **原生回归已添加但未运行**：
  `ComparisonSurfaceWarpTests.SignedSubtractThresholdPreservesNeutralGrayAndOpacity`
  覆盖正负两种差值、三种策略、1.0/0.5 opacity、阈值拒绝/通过/关闭，共 36 次回读期望。
  新原生断言的反向控制与变异验证仍待 Windows 执行，不能用上述 CPU 算术结果替代。
- **未验收**：Windows/MSVC、固定依赖、原生 HLSL/WARP 编译与像素回读、仓库
  format-check/lint、完整 CTest、真实 GPU 性能、实窗截图及完整手机录屏工作流。
  本轮不改工作流、不触发或重跑 CI，不扩大差异模式或指标管线。

## 2026-10-08 阈值过滤的模式覆盖与 Fade 守卫（Windows 原生已运行，真机验收未做）

- **背景**：PR 42/43/44 已合并到本地集成分支并快进至本地 main（未推送 origin main）；
  PR 44 的 scope-note 测试缺 `addImportPath(bin/qml)` 在 Windows 失败，修复已作为
  `f616da6` 推到上游 `docs/video-metrics-sample-scope` 分支。本轮补齐阈值过滤残余：
  黑中性模式（0–4）没有逐模式像素断言；且 `D3d11ComparisonRenderer` 把
  `viewMode == Fade` 映射为 Crossfade（7）度量、以 wipePosition 为混合量，差异模式
  遗留的 thresholdEnabled 会让 Fade 视图被阈值过滤成黑——该路径用户可达。
- **改动**：`Nv12ToRgb.hlsl` 阈值分支增加 `differenceMetric != 7U` 守卫
  （Crossfade 读混合而非差异，无差异可滤）。新增
  `ComparisonSurfaceWarpTests.ThresholdFilterKeepsBlackNeutralDifferenceMetricsBlack`
  （模式 0–4 × 三种策略，阈值 1.0 拒绝 → 黑；彩色源对模式 0–3 在阈值 0 下非黑，
  防全黑回归；模式 4 非黑由既有 ExactPlaneDiff 测试覆盖）与
  `ComparisonSurfaceWarpTests.FadeViewIsNotFilteredByTheDifferenceThreshold`
  （阈值 1.0 启用时 Fade 混合仍为 (104,104,104)，与关闭阈值时一致）。
  另用仓库 `format` 目标修复 PR 43 新文件 `ClipExportControllerTests.cpp` 的
  clang-format 违规（该 PR CI 被 skip，本机 format-check 首次发现）。
- **Windows 原生验证（本机 MSVC + WARP）**：`ComparisonSurface` 相关套件 49 通过、
  1 条件跳过（当前屏幕缩放产生不了奇数物理窗口宽度，与改动无关）；
  ClipExportControllerTests 3/3 通过；仓库 format-check 与 lint 通过。
- **变异证据（原生 WARP，恢复后全绿）**：阈值背景 0.0→0.25 由表测试检出；
  移除 mode 7 守卫由 Fade 测试检出；阈值分支整体禁用（恒 false）由表测试检出。
- **未验收**：完整 CTest、真实 GPU 性能、实窗截图、发布门禁与真机素材走查。
  追记：PR 43 的格式修复已作为 `1818325` 推到上游 `fix/indexed-vfr-export-entry`；
  阈值守卫与表驱动断言已合入本地 main（`44de12f`，未推送 origin main）。

## 2026-10-05 区间导出：非零起始源把归一零时间与容器时间混用（P1-A，已修复并验证）

对 `267adca` 的逐条证据核验见 [code-quality-review-267adca.md](code-quality-review-267adca.md)。
本轮修掉其中 P1-A；**P1-B 像素口径、P1-C 阈值高亮、P1-D 导出事务、P2-A VFR 入口、P2-B 坏点占比
均未处理**。

- **缺陷**：区间规划在「归零到 frame 0」的时钟上工作（`RationalRate`/`FrameTimeline` 两条时间线都
  以 frame 0 为 0），而 `ClipExportWriter::keyframeTimes()` 返回**容器原始 PTS**。两者直接进
  `alignClipExportStart()`，于是首 PTS 为 1s 的素材被按「在 0–0.4s 找关键帧」处理，返回
  "The requested range holds no keyframe, so the stream copy would be empty."。播放支持非零起始 PTS
  不代表导出也处理好了它。
- **复现**：仓库自带 `h264_nonzero_start_64x48_30fps_12.mp4`（ffprobe 实测 `start_pts=15360`、
  `time_base=1/15360`、12 帧、仅 1 个 I 帧在 1.000000s）。用仓库自身未修改的 planner 源码编译探针，
  选全部 12 帧得到 `The requested range holds no keyframe`；把关键帧表换成归零后的 `{0}` 立刻对齐成功
  （`shift=-100000`），与零起始 fixture 逐位一致——因果钉死为时间原点，不是规划算法。
- **修法**：容器原点知识只留在适配器。新增 `streamOriginMicroseconds()`/`videoStreamStartTime()`
  从 `stream->start_time` 取原点；`keyframeTimes()` 三处 push 都减去它，使返回表与规划器同一时钟；
  `perform()` 的 seek 目标、`endPts`、进度被减数与 `firstPresentationMicroseconds` 各自加回原点。
  `firstPresentationMicroseconds` 的契约写的是 "source media time"，非零起始源回原始 PTS 就不再是
  source media time，因此一并归一。
- **测试（3 项新增）**：`NormalizesKeyframeTimesOfANonZeroStartStream`（关键帧表归零为 `{0}`，
  覆盖首/中/尾区间）、`ExportsTheRequestedRangeOfANonZeroStartStream`（端到端 12 帧、成片从 0 开始、
  无 partial 残留）、`SeeksANonZeroStartMultiGopSourceOnTheRequestedKeyframe`（多 GOP 证明 seek 落
  frame 3 而非 frame 0）。
- **新 fixture**：`h264_nonzero_start_multigop_64x48_30fps_12.mp4`。关键帧刻意放在 frame 0/3/6/9 ——
  第一版用 0/4/8，归一化时间 133333µs（133333.33 四舍五入）被 `frameAtOrBefore` 向下取整成 frame 3，
  `firstExportedFrame` 差一帧。那是取整伪影不是缺陷，但会掩盖真实语义，故改用整除微秒边界。
- **反向控制**：`git stash` 掉 writer 修复后三个新测试全部失败，失败值正是缺陷本身
  （`{ 1000000 }` vs `{ 0 }`；`{ 1000000, 1100000, 1200000, 1300000 }` vs `{ 0, 100000, 200000, 300000 }`）。
- **变异证据**：`out/verification/p1a-mutation/`，4/4 检出（原点不归一 / seek 忘记原点 /
  end bound 忘记原点 / 报告回原始 PTS），控制组与还原控制组均绿。
- **取证脚本自身的三个坑（已修，留记录）**：① `Write-Output` 诊断被 `$results +=` 收进结果数组，
  计数虚高且诊断不显示，改用 `Write-Host`；② 还原源文件后未重新构建就读还原控制组，读到的是上一个
  变异编出的二进制，误报「还原后失败」——Ninja 按 mtime restat，`Move-Item` 还原的文件可能比目标更旧，
  构建被静默跳过，须强制刷新 mtime；③ `seek-forgets-origin` 一度**存活**：单关键帧 fixture 上 seek 到 0
  会被 FFmpeg 钳到首个关键帧，与 seek 到正确原点等价，这是真实覆盖盲区，补多 GOP fixture 后才可观测。
- **门禁**：media 组件 135 通过 / 1 跳过（GameDVR 用例未设 `DVS_TEST_GAMEDVR_CAPTURE`，既有状态）、
  application 155 通过、`ui.MainQmlContractTests` 48 通过 / 4 禁用（既有）；dev format-check 与 lint 通过。
- **未验收**：Windows GUI 完整导出流程、真实录屏、GPU/D3D11VA 未运行。本轮只闭合时间基准这一条接线。

## 2026-10-05 播放控制条：鼠标在进度条上时控制条淡出（U-03，工作区已修复，未提交）

用户报告「在播放器界面，鼠标在进度条上时进度条也会消失」。复现的不是「悬停被子控件抢走」，
而是**倒计时本身不看光标**：主机每做一次走带调用 `transport.reveal()`（拖动进度条、在进度条上
滚轮），该函数无条件 `hideTimer.restart()`；1.2 秒后计时器把面板淡出，光标还在进度条上。
- **修复**：`PlayerOsc.qml` 新增 `pointerInsidePanel`（面板 `HoverHandler` **或**时间轴
  `pointerInside`），让「光标是否在控件上」成为倒计时的唯一前置条件：
  - `reveal()` 只在光标不在控件上时才重新计票；
  - `hideTimer.onTriggered` 加 `&& !pointerInsidePanel`，倒计时只能把光标已离开的条淡出；
  - `onPointerInsidePanelChanged` 成为唯一的重置点，光标离开控件（移出面板、松手、拖拽中
    掉 grab）都重新计票，避免只停在拖拽释放后卡在可见状态。
  - `TimelineTracks.qml` 新增 `pointerInside`（`containsMouse || dragging`）：拖拽中光标
    被拖出面板时 Qt 会撤销鼠标区 hover，但 grab 仍在，这段时间棒必须算「在控件上」。
  - `Main.qml` 沉浸态 `immersiveOscTimer` 同样改为光标在控件上则延期重检。
- **测试**（`ui.player_osc 12/12`、`ui.timeline_tracks 5/5`）：新增
  `test_hovering_the_progress_bar_keeps_it_visible_under_the_cursor`、
  `test_releasing_a_scrub_outside_the_panel_hides_the_bar_again`、
  `test_pointer_inside_survives_a_scrub_outside_the_bar`，并在原有自动隐藏测试中反证
  光标门没有被彻底关死（无光标时仍会淡出）。
- **变异证据**：`reveal()` 无条件 restart → 新悬停测试 FAIL；
  `pointerInside` 去掉 `|| dragging` → 时间轴拖拽测试 FAIL；
  `onPointerInsidePanelChanged` 去掉 else 分支 → 释放淡出测试 FAIL。三处均单点失效。
- **门禁**：`ui.MainQmlContractTests` CTest **48/48 通过**（`gtest_discover_tests`
  逐例注册，各自 20 s 超时；整二进制一次跑 48 例会出现与本改动无关的跨用例偶发失败）；
  dev `format-check`、`lint`（`--max-warnings 0`）通过。
- **顺带发现**：`tests/component/ui/qml/tst_timeline_tracks.qml` 的 `markers` 数组自 HEAD 起
  就不符合 `qmlformat -w 4`（预先存在的偏差，不在本轮格式门禁范围内，未替它改格式）。

## 2026-10-04 三项收口开工：视觉长尾、启动速度、播放器完备度（工作区，未提交）

用户按三项打分要求修复：UI 视觉完成度 7（token 化完成、布局长尾未清）、启动速度 5
（830 ms + 230 ms 空白窗）、通用播放器完备度 3.5（列表/续播/网络流/滤镜全无）。
本节记录开工当天的核实结果与已完成项；范围决定见下方「范围决定」。

- **范围决定**：用户选定**本轮只做 C-A**（续播、列表增强、播放状态可见化）。C4 网络流与
  C5 滤镜**不在本轮**，作为独立立项记录在文末。产品范围
  [「不要因播放器这一名称而默认扩展媒体库」](../product/visual-review.md) 保持原样，未改写。
- **token 化确实已完成**：`src/ui_qml/qml` 全目录只剩 **19 处**十六进制字面量，集中在
  `ImageWorkspace.qml`(18) 与 `ImageEditDialogs.qml`(1)，均为画笔色板/棋盘格/白底等内容色。
  **A 区的剩余工作是色相与布局，不是 token**，不应再排 token 化任务。

### 本轮验收状态与未完成项（不当作计划已完成）

- **已完成并有证据**：V-09 色相（变异 5/5）、V-10 窄窗遮挡（复现→修复→45/45）、
  S-04 启动基线重绑 + 注册 worker 化（分段 −17.8 ms）、
  **C1 续播**（身份绑定 + 有界存储 + 显式告知，3 条单测 + 1 条全链路契约测试）、
  **C3 未测量 vs 零缺陷**（新 `playbackRunPresentedFrames` 信号，变异 2/2）、
  **B2 启动画面**（自带线程的原生窗口，`spawn → splash-painted` **57.1 ms**，覆盖到首帧约 734 ms，
  测试 5/5 + 变异 2/2；并修掉它自己引入的一个回归，见启动文档 Step 9）、
  **B1 已用 A/B 证伪并撤回**（不是优化）、
  **B3 预探测已用实测判定收益 <1%，撤回**（不是未做）、
  **A4 芯片行溢出已用测试推翻并更正台账说法**（不是修复）。
- **门禁**：dev `format-check` 通过、`lint` 零告警、
  `^(ui\.|shell_windows\.|app\.)` 回归 CTest 报 **100% / 391**
  （注册 395，过滤外 4；391 中 385 实跑通过 + 2 skipped + 4 disabled 为既有状态）。
- **本轮未开始，不要当成已做**：
  - **A3** 通知栈窄窗与右侧面板重叠——未取证。现有 `Main.qml` 的 `stageRight` 已扣除抽屉宽度，
    台账那条「可能重叠」**未复现也未关闭**，只是没动。
  - **A4b** 二次启动前台交接——**已实现授权，但端到端在本环境无法验证**。先更正一条旧记录：
    之前写的「`AllowSetForegroundWindow` 调用点已存在」是错的，代码里**根本没有**。
    实测（`out/verification/a4b-foreground-handover/probe.ps1`）：第二个进程转发成功（exit 0），
    主实例窗口**始终不是前台**。修法：主实例在 accept 时先自报 `PID <pid>`，转发进程在写入请求
    **之前**读这一行并 `AllowSetForegroundWindow(primaryPid)` —— 只有前台进程（或由它启动的进程）
    才有权授予前台资格，而双击启动的转发进程恰好持有这个资格。
    **仍然未验证**：本环境的进程由测试工具拉起、不具前台资格，`AllowSetForegroundWindow` 是空操作，
    修复前后实测都是「窗口没上前台」（诊断日志确认授权已发出、`SetForegroundWindow` 已调用）。
    也就是说**这里的失败是环境造成的，不能当作产品修复成功的证据，也不能当作修复前缺陷的证据**。
    已验证的只有协议半边：握手的 `PID ` 招呼行由
    `app.StartupRequestBrokerTests.PrimaryAnnouncesItsProcessIdBeforeTheForwarderSends` 断言，
    变异（删掉招呼行写入）1/1 失败并按字节还原。真实验收需要在 Explorer 里双击一次。
  - **B1** QML 延迟加载（Step 8）——**已证伪，不要重开**。见
    [startup-performance-v2.md](startup-performance-v2.md) 的 Step 8 探针：摘掉全仓最大的
    `ImageWorkspace.qml`（148.6 KB）只差 ~12 ms；`context-ready → qml-warm` 仅 12.2 ms。
    550 ms 是 Main.qml 自身编译+建树且**弥散分布**，逐个改 `Loader` 只能拿成比例小切片。
  - **B3** worker 侧预探测——**不做**。当前 SHA 实测：命令接受→首帧提交仅 **42.7 ms**，
    其中 1080p60 解码占大头，预探测上限 <1%；而 `MediaProbe` 无结果缓存，要预探测就得在解码
    适配器里引入按文件身份失效的缓存语义。见启动文档 Step 10。
  - **C2 列表增强**——**控制器层与 QML 接线都已实现**。不再等拍板，按建议的安全范围落地：
    重排**只改显示顺序**，多选走**既有操作**。
    - **2026-10-07 真实鼠标补证与修复**：在当前 main `a0a0440` 复现 handler 已 active、
      指针移动约601px，但来源牌位移0、排序信号0；旧属性测试没有覆盖这条真实手势。
      接通 translation，并修正右移索引、取消与拖中身份变化边界。正式 Qt6.8.2 / Basic /
      offscreen / software 在四档缩放各26/26（24行为 + init/cleanup），38/38实现变异、
      3/3观测guard覆盖42个新断言位点。真实组件截图已检查；Windows主窗/输入设备、
      完整Main/shell与播放门禁未验。见[来源条拖拽范围与证据](source-strip-drag.md)。
    - **2026-10-08 Windows 原生验证**：Basic/Windows × 四档缩放 8/8（每轮 21–23 s，
      超时已放宽至 60 s）；随全量 962 项 0 失败。真实鼠标的全 App 拖拽留待交互会话。
    - 新增（`ReviewShellController`）：`displaySourceIdentities` / `selectedSourceIdentities`、
      `moveSourceInDisplayOrder(from,to)`、`setSourceSelected` / `toggleSourceSelection` /
      `clearSourceSelection`、`removeSelectedSources()`。全部以**冻结 source identity** 为键，
      不接受索引寻址，所以重排**碰不到** `sourceId` 驱动的 A/B/C 角色、`Theme.sourceColor`、
      对齐偏移与配对策略；`activeSources` 顺序、`canonicalSourceIndex`、`referenceSourceIndex`、
      `activeGeneration` 一律不动，且重排**不提交任何媒体命令**。拓扑一变，显示顺序复位、选择清空
      （用户没有选中新打开的源）。
    - 测试：`ReviewControllerTests` 3 个（重排不碰角色/参考/active 集合且不提交命令；越界索引被拒、
      二次重排可叠加；选择跨重排保留、拓扑变更后清空、未打开 identity 无法被选中；批量删除按
      identity 走同一条队列，第二个意图排队而非同时提交）。变异 **3/3** 被杀
      （重排吃掉 active 顺序 / 拓扑不变时不复位 / 选择不再限制在已打开的 identity），
      产品文件每次按字节还原。证据 `out/verification/c2/mutate-display-order.ps1`。
    - **QML 接线**：`ActiveSourceStrip.qml` 由 `Row` 自动布局改为**声明式 x 绑定**
      （`chipStartX(position)` / 每个 chip 的 `layoutX`）—— 这是必须的：`Row` 按 model 顺序排布，
      而显示顺序正是可能与它不同的那一维；命令式 `Qt.callLater(repositionChips)` 在实测中**不稳定**
      （芯片宽度由标签算出、要晚一帧才稳定，按钮式重排会按过期宽度定位，测试抓到过一次
      `12 vs 437`），绑定则随顺序或任一宽度变化自动重算，无时机可错。
      芯片体点击 = 切换选中（此前芯片体**根本没有点击目标**，所以没有行为被夺走；源菜单仍在 ⋯ 按钮上，
      不需要修饰键、键盘也可达）；有选中时出现「− 删除所选」按钮；拖拽超过 12 px 阈值后**在抬起时**
      解析目标索引并请求移动（不做拖拽过程中的实时让位动画 —— 本环境无法目视确认其观感）。
      Main.qml 把 `displayOrder` / `selectedIdentities` 接到 shell，并接上 move / toggle / clear /
      removeSelected 四个信号。
    - 测试：`ui.MainQmlContractTests.StripChipsFollowDisplayOrderWithoutChangingTheirSourceId` ——
      芯片 x 沿显示顺序**严格递增**、每个 chip 的 `sourceId` 不变、chip 实例不重建、「删除所选」
      按钮随选择出现/隐藏、选择不提交任何媒体命令。变异 **2/2** 被杀
      （`displayIndex` 退回 `sourceId`；`layoutX` 钉死为常量），QML 按字节还原。
      证据 `out/verification/c2/mutate-strip-qml.ps1`。
    - **踩到的两个取证陷阱（已修，脚本里留了防再犯的断言）**：
      ① 变异脚本最初指向 `dvs_ui_component_tests.exe`，而 `MainQmlContractTests` 在
      `dvs_main_qml_contract_tests.exe` —— 过滤器匹配不到任何测试、gtest 退出 0，于是**每个变异都被
      误读成「存活」**。现在脚本会先断言「被过滤的测试确实跑了」再记结论。
      ② 测试原本用「取 x 最小者」判断顺序；若芯片根本没被定位（x 全为 0），最小者恰好是遍历顺序的
      第一个，也就恰好等于期望值 —— 完全忽略顺序的芯片行也能通过。已改为**成对严格递增**断言。
    - **未验证**：拖拽交互与选中高亮只有几何/属性断言，**没有屏幕确认**，观感（拖拽时的反馈、
      选中态对比度、「−」按钮位置）未经人眼核对。
- **未验收边界**：全部工作区改动**未提交、未发布**；A1/A2 是几何与配色断言，**不是像素或屏幕
  ACK 证据**；B2 启动画面的**生命周期与时序已验证，但观感（位置/字号/留白）未做屏幕确认**；
  A4b 的**前台交接只有协议半边有证据，真实前台行为在本环境无法验证**（需在 Explorer 里双击一次）；
  splash 拆卸竞态的轮询循环是**加固，没有测试覆盖**（两个变异都存活，见启动文档）；
  C2 的拖拽与选中只有几何/属性断言，**观感未经屏幕确认**；
  启动结论绑定本次 release 构建与同一天测量，**不外推到其它 SHA 或机器**。

### C3 ·「无检测结果」不能被读成「零缺陷」（已实现）

- **台账/评分对齐**：C 区「播放丢帧组可见化」的读数（目标/实际倍率、跳过组数、落后、追赶中、
  重复标记、呈现间隙）**本来就存在**，真正缺的是产品文档 §2 那一半：
  「无检测结果应显示未知，不应当作零缺陷」。计数为 0 时这些项被直接省略 →
  用户把「没显示」读成「没问题」。**所以这是新增信号，不是重画。**
- **新增信号**：`SessionSnapshot.playbackRunPresentedFrames` —— 当前 Play 区间**已完成呈现**的
  FrameSet 数。落点是 `PlaybackCoordinator::commitPlaybackFrameIfComplete()`，即
  publish + present + presentationTimer 三者都完成的那一刻，是「已有可测样本」的准确判据；
  在 `startPlaybackRun` 处与 `playbackRunSkippedFrameSets_` 一起清零。
- **读法**：`playbackRunPresentedFrames === 0` → 状态栏显式追加「本次播放尚未测量」；
  一旦有过样本，标记消失，此时的沉默**才是**测量结论。
- **变异证据**：`out/verification/c3-unmeasured-status-mutation/mutate.ps1`
  （2 突变 + 1 对照，自断言 `checks run: 3 (expected 3)`）：
  - `remove-unmeasured-marker`（删掉整行）→ **detected**
  - `always-claim-unmeasured`（`=== 0` 改成恒真，等于永远喊「未测量」）→ **detected**
  - 对照（还原后）→ **通过**；`PlayerOsc.qml` **逐字节恢复**
  - 判别性取证：失败日志精确落在
    `FAIL! qmltestrunner::PlayerOsc::test_status_says_unmeasured_before_the_run_presents_a_frame()`，
    且为 `9 passed, 1 failed` —— **只有目标用例红，没有连带失败**。

### C1 · 续播（已实现）

- **绑定身份而不是文件名**：沿用既有 `SourceIdentity`（`路径|字节数|mtime`，已 case-fold）。
  同名同路径但被替换/编辑过的文件**不会**继承上一份的位置 —— 这正是不能用文件名做键的原因。
  `ReviewController.currentSourceIdentity` 取 canonical 源的**冻结**身份，文件在会话期间被改动
  仍匹配它被打开时的那条记录。
- **存哪**：复用**现有 settings 文档**（`SettingsSnapshot` 本来就是 key/value 边界），
  前缀 `resume.`，值为 `"<递增戳>:<帧>"`。不新建第二个存储层 —— 否则要再加一套 port + adapter。
- **有界**：`kResumeEntryCap = 48`，超出按最久未动淘汰，避免设置文件无限增长。
  这里的戳**不是时钟读数而是单调递增值**：同一毫秒内的连写（正是拖动播放头时的抖动去重行为）
  不会打平，否则淘汰会退化成按 key 字典序，那不叫「最久未动」。
- **何时恢复**：仅当身份变化/源数量变化、且 `currentFrame === 0`（即还停在开头）、
  且记录帧 `< totalFrames`（防止旧版本文件的时间轴把播放头推到界外）。
  **显式告知**：复用既有 `showIntentMessage`，提示「已恢复到上次位置 第 N 帧」——
  静默跳转和「程序丢了我��位置」在界面上无法区分。
- **写入时机**：`currentFrame` 变化后 **1500 ms 去抖**（不是每帧写，否则拖动变成磁盘 IO）；
  关窗时 `onClosing` 同步补记一次，避免「刚拖动就关窗」丢位置。
- **损坏条目按「无记录」处理**，不猜位置：宁可从头开始，也不要跳到一个被猜出来的位置。
- **测试**：
  - `ui.ReviewPreferencesControllerTests` 三条 —— 身份隔离（同路径不同 mtime 互不继承）、
    损坏条目一律读作无记录（5 种畸形形态）、48 条上限淘汰最旧。
  - `ui.MainQmlContractTests.ResumeRestoresRecordedPositionAndStaysOffForeignOrStaleEntries` ——
    真实临时文件 + 真实 `ReviewController`/`ReviewPreferencesController`/Main.qml 全链路：
    匹配身份**恰好 seek 一次**、提示非空、越界条目**不**生效。
- **过程中修掉的两个真缺陷**（都是先由测试暴露，不是先改代码）：
  1. 同毫秒连写使淘汰退化（见上），修在产品侧；
  2. `currentSourceIdentity` 原本只声明为 `Q_INVOKABLE`，QML 读属性拿到空值 —— 必须要是
     带 `NOTIFY` 的 `Q_PROPERTY`，绑定才会随状态更新。
- **未做**：跨进程/重启端到端手工验证；设置文档并发写入的极端情况未压测。

### A4 · 960 px 芯片行溢出 —— **不成立，已用测试推翻**（原台账说法有误）

- 原记录称对比模式芯片行在 960 px 会溢出。**实测不成立**，不予修复：
  `ui.MainQmlContractTests.ActiveSourceStripKeepsEveryChipReachableAtMinimumWidth`
  在真实三源对比 + Main.qml 声明的 960 px 最小宽度下测得
  `chip = 12,31 258.95x28`、`277.95,31 166.95x28`、`451.9,31 166.95x28`，
  添加按钮 `12,31 34x28` —— **全部落在 strip（宽 960）内**。
- **为什么装得下**：芯片宽度是 `Math.min(280, …)`，**单芯片封顶 280 px**，
  3×280 + 34（添加）+ 6×7（间距）+ 2×12（边距）≈ **940 px < 960 px**。
  也就是说封顶值恰好保证了不溢出，台账作者（我）此前**只看了下限 `Math.max(128, …)`，
  没看上限 `Math.min(280, …)`**。
- **边界**：本测试只覆盖 ≤3 源（产品上限）与 ≤960 px 宽度。
  中文文件名会拉大 `chipLabelPlainWidth`，但受 280 封顶约束，仍不会溢出。
  更窄的窗口（<940）本轮未测，Main.qml 也未声明更低的下限。
- **测试保留**：它是「这个说法被推翻」的证据，绿了也不能删。
- 顺带修正两个测试自身缺陷（都不是产品问题）：`QQuickRepeater` 虽是 `QQuickItem`，
  但 delegate 挂在它的 **parent**（chip Row）上；对 Repeater 取 `childItems()` 得到 0。
  以及 `QRectF::contains()` 按定义**拒绝空矩形**，Repeater 自身那个 0×0 记账项必须剔除，
  否则会以「不可达」之名误报。

### V-09 · 时间线标记与信息色同源身份撞色（已修，变异 5/5）

- **缺陷**：`Theme.markerDuplicate` `#fb923c` 与 `sourceB` **逐字节相同**；
  `Theme.information` `#38bdf8` 与 `sourceA` **逐字节相同**；`markerExtra` `#c084fc` 与
  `sourceC` `#a78bfa` 相差约 6°。另自查发现 `markerAnchor` `#22d3ee` 与 `sourceA` 仅差 10°，
  `markerOther` `#facc15` 与 `probe` 相同——两处未被台账记录，同属一类。
- **影响**：时间线标记只有 4–11 px 宽，Alpha 徽标只有 7 px；「看着像源 B」与「这就是源 B」
  在这个尺寸上无法区分，直接违反「红色只给失败」之外的读图约定。
- **修法**：标记族整体移出 A/B/C 三个源色相带（天蓝 198°、橙 27°、紫 255°）——
  `duplicate #a3e635`（青柠 83°）、`extra #f0abfc`（品红 291°）、`anchor #e2e8f0`（近中性亮）、
  `other #a8a29e`（近中性暗）；`information #22c55e`。anchor/other 走**彩度**分离而非色相分离，
  因为灰色本来就落在任意色相位置上。
- **验证**：新增 `tst_vcs_theme.qml`／CTest `ui.vcs_theme`，4 个用例断言「色相距离 ≥30° 或
  彩度差 ≥0.30」，并保留 `missing` 必须仍为 `error` 红的断言。**变异 5/5 检出**（逐条退回
  A1 之前的颜色，每次都被对应用例抓住），对照组通过，`VcsTheme.js` 逐字节恢复。
  脚本自检检查数 6（5 变异 + 1 对照）。
- **过程中的两个坑（都记下来）**：
  1. `build.ps1 -TestOnly` **不能**与 `-UseInstalledDependencies` 同用，wrapper 会直接抛错。
     首版脚本因此把 6 次「wrapper 失败」全部误记成 5/5 检出——**是控制组抓到的**。
  2. `build.ps1` 会把 ctest 失败**重新抛成 `Exception:`**，所以「日志里有没有 Exception」
     不能用来判断 wrapper 是否真的跑了测试；必须看有没有 `Test project` 与结果行。
- **证据**：`out/verification/a1-theme-hue-mutation/`（`mutate.ps1` + 6 份运行日志）。

### S-04 · 启动基线重新绑定当前 SHA，并摘掉两段从未归因的主线程开销

- **为什么重测**：台账与启动文档里所有数字都绑定 `8ffa577`（v2.0.0 时期）。当前 HEAD 是
  `267adca`（tag `v2.1.0`），**从未测过**。「830 ms + 230 ms 空白窗」是 v2.0.0 的数，不是现状。
- **现状实测**（同脚本、同 release、同素材、热缓存、5 轮 + 1 预热）：
  首帧提交 **949.9 / 990.6 ms**，「可见但空白」窗口 **~307 ms**（窗口 643.0 → 首帧 949.9）。
  比记录的 835.1 / ~243 更差；但按启动文档 L178 的日间漂移教训，**不据此判定为回归**，
  要判定必须重跑 `8ffa577` 的同日 A/B。
- **归因**：此前从未单独测过的 `surface-bound → show-enter` = 23.4 ms **全部**在
  `applyWindowsNativeChrome()` 里；再拆开后确认是 `window->winId()`（**22.4 ms**，
  在 `show()` 之前强制物化 HWND），四次 `DwmSetWindowAttribute` 只有 0.2 ms。
  另一段 `qml-load-returned → shell-registration` = 18.0 ms 是启动时同步的资源管理器自愈注册。
- **修复一（保留）：Explorer 自愈注册改为 worker 线程。** 调用、修复规则、失败只记日志的语义
  全部不变，`runDesktop` 返回时 join，会话不会在注册写入进行中退出。
  收益按**同一份运行内的分段**认定：`qml-load-returned → shell-registration` **18.0 → 0.1/0.2 ms**。
  不按总数认定——同轮整体更慢（QML 段漂到 600.6），跨轮总数不可比。
  **口径变更**：该里程碑从此测线程派生，注册正确性改由 `shell_windows.ExplorerCommandRegistrationTests`
  与注册脚本测试负责，不由启动测量负责。
- **修复二（**被实测否决，已回退**）：把 `applyWindowsNativeChrome` 移到 `show()` 之后。
  假设是"22 ms 只是提前物化，挪回去即净赚"。**同日 A/B 证伪**：`show-enter → sg-initialized`
  从 168.1 涨到 **213.9/235.2 ms**，spawn→窗口从 642.8 反而变成 **689.5**（更晚）；
  `post-show → native-hwnd` 仅 0.2 ms 是决定性证据——**`show()` 本来就会建 HWND**，
  提前建不会替它省任何工作，只是把成本搬进更贵的调用并引入标题栏闪白风险。已回退，
  代码注释写明这次否决；`b1-fixed-*` 数据作为反例保留。
- **保留细粒度里程碑**（`graphics-config`/`screen-select`/`native-hwnd`/`native-attrs`/
  `native-chrome`），仅在 `DVS_STARTUP_TIMING` 设置时输出，平时零开销。
- **证据**：`out/startup-measurements/b0-{nofile,onefile}-20261004-1210{53,130}/`（基线）、
  `b0c-attrib-nofile-20261004-121429/`、`b0d-chrome-split-20261004-121537/`（归因）、
  `b1-fixed-{nofile,onefile}-20261004-*`（**否决反例**）、`b1-final-{nofile,onefile}-20261004-1255*/`
  （保留项后的终态）。完整表与口径写入 `startup-performance-v2.md` Step 8。
- **终态复测（保留项落地后）**：

  | 指标（中位 / P95） | 基线 B0 | 终态 B1 |
  |---|---|---|
  | `qml-load-returned → shell-registration` | 18.0 / 18.3 | **0.1 / 0.2** |
  | spawn → 窗口可见（无文件 / 带文件） | 645.8 / 643.0 | 626.2 / 625.7 |
  | spawn → `exec`（无文件 / 带文件） | 872.9 / 874.3 | 846.8 / 852.5 |
  | spawn → 首帧提交（带文件） | 949.9 / 990.6 | 924.9 / 954.1 |
  | 可见但空白窗口（带文件） | ~307 | ~299 |
  | `show-enter → sg-initialized` | 170.0 / 174.2 | 182.8 / 187.2 |

  **只认分段，不认总数**：`shell-registration` 的 −17.8 ms 在两次独立运行里都稳定复现，
  是本次唯一可归因的收益。总数那 −18~−25 ms 里混着运行间漂移（`show-enter → sg-initialized`
  这次反而 182.8 vs 170.0，虽仍落在历史 153–196 区间内），**不作为结论宣称**。
- **下一步**：QML 装载段 ~550 ms 与 `show-enter → sg-initialized` ~180 ms 仍是两个大头；
  前者只能靠继续延迟加载（Step 8），后者属设备生命周期架构改动，本轮不承诺。
  要做 A/B 必须同一天跑同一脚本，跨日数字只作参考。

### 范围外（本轮不做，独立立项）

- **C4 网络流（rtsp/http/smb）**：`ReviewController.cpp:295` 要求 `isLocalFile()`、
  `ReviewShellController.cpp:552` 拒绝非本地，这是**主动拒绝**不是遗漏。放开它要处理超时、
  断线重连、代次失效，以及**不可 seek 的素材**与现有精确导航/帧组语义的冲突——现有整套
  比较语义建立在「可精确定位到帧」上，网络流不满足该前提。
- **C5 滤镜（锐化/去色度/去噪）**：`vcpkg.json` 无 avfilter/encoder。且必须保证滤镜只作用于
  显示路径，**绝不进入比较与统计管线**，否则违反「差异统计始终是原始 8-bit 差值」的既有约定
  （[指标 ADR](adr/0005-pixel-difference-metrics.md)）。

### V-10 · 窄窗悬浮播放条盖住视口「适应窗口/重置视图」（已复现并修复）

- **台账原记录**："悬浮播放条在约 1100 px 以下可能压住视口左下角的适应 / 重置行（既有）"——
  一直标为"可能"，从未复现过。本轮补上复现。
- **确定复现**：新增 `ui.MainQmlContractTests.NarrowWindowKeepsViewportCornerControlsReachable`
  在 `Main.qml` 声明的 960 px 最小宽、单源（唯一的悬浮拓扑）下测几何。修复前实测
  `transport = 14,437 932x203`、`viewportViewCommands = 26,568 126x24`：
  传输条纵向占 437–640，横向占 14–946，把整行 fit/reset **完全覆盖**且不可点击。
- **修法**：不在 `ComparisonViewport` 里猜传输条位置（它拿不到），由 `Main.qml` 测量重叠并把
  高度交给视口——新增 `root.transportOverlayInset`（仅在**悬浮**且可见时非零）与
  `ComparisonViewport.bottomOverlayInset`，`pixelScaleBadge` 的 `bottomMargin` 变为
  `12 + bottomOverlayInset`，`viewCommandRow` 原本就锚在徽标上方，自动跟随上移。
  **停靠与隐藏两种状态下 inset 恒为 0**，停靠布局逐字节不变。
- **回归**：修复前该用例失败、其余 46 条通过；修复后 `ui.MainQmlContractTests` **45/45 通过**
  （注册 49 项，4 项 disabled 为既有状态，与本轮无关）。
- **边界**：这是几何断言，不是像素证据；窄窗下按钮是否被别的元素遮住仍需截图复核。

## 2026-10-02 日常视频文件夹浏览（V-08，工作区已实现，未发布）

- **用户优先级**：先补类似日常播放器的「从文件夹选视频并播放」，暂不推进 CLI 或异常区间评测。
- **交付行为**：首屏／文件菜单／`Ctrl+Alt+O` 选择本地顶层目录；五种视频扩展名自然排序，中文
  路径保留。点击单视频 NewReview，匹配成功终态后播放；上一项／下一项手动、不绕回、不连播。
  列表可收起、纯净模式隐藏；浏览目录不替换工作区，失败不标错当前文件或丢掉已有画面。
- **代码依据**：`VideoFolderModel.h/.cpp`、`ReviewShellController::openVideo`、
  `VideoFolderSidebar.qml`、`ReviewInputDialogs.qml`、`Main.qml`、`DesktopApplication.cpp`。
  默认一个后台 worker＋一个 latest queued job，有界 100,000 个文件；GUI 不做目录 I/O／排序／
  join。取消／代次 fence 与单项 intent 身份隔离；FrameSet 与 ACK-before-commit 未改。
- **回归**：新增 Model 15＋主界面契约 4＝**19/19**；**40/40** 运行时断言变异检出，构建失败不
  计为检出，逐字节恢复与读回通过；39≠40 的缺项 guard 拒绝执行。完整 lint、format-check 通过。
  恢复重建后默认 dev 选中 **853 项：846 实际通过、3 原有 skipped、4 原有 disabled、0 failed**。
- **全量发现的测试时序问题**：既有 `ImageWorkspaceManual` 在 RowLayout polish 前读到像素文字
  941 px＞920 px，布局完成后 762 px。相关产品文件与 HEAD 相同，未改产品布局；在原两条宽度
  断言前等待 `waitForPolish`，不放宽断言。删掉等待的 QtTest 变异 1/1 检出，字节恢复与最终
  全量通过；首次失败、两次复查和探针证据全部保留。
- **证据／边界**：`out/verification/video-folder-browser/`，Qt 测试窗口截图
  `folder-sidebar.png` 展示列表布局，不是实际视频画质／屏幕 ACK 证据。详见
  [协议、范围与验证](video-folder-browser.md)。不做递归、缩略图／媒体库、目录监视、自动连播、
  音频／字幕；已交给内核的活动 Open 不强行终止，取消仅抑制旧自动播放与排队项。
  真实用户素材／硬件长测／覆盖率／发布 ZIP 未验收，未提交／未发布，既有工作区保留。

### V-08 后续：播放中换项「无法提交视频打开请求」（工作区已修复）

- **确定复现**：busy 是前台命令状态，不覆盖 playing、在途 Play/Pause 与旧帧 drain；此时
  canOpen=false，原 shell 按 busy=false 立即提交／出队 Open，浏览器收到 intent ID 0。
- **产品修复**：`ReviewShellController::submitOrQueue/drainIntentQueue` 保留有界素材 intent，
  canPause 就绪时异步暂停，在途 transport 与旧帧收尾后按 canOpen 出队；不能用 framePending
  代替完整门禁。匹配 Open 成功后才播放，取消／最新选项／实际源高亮、Close 优先级保留。
  Pause 提交拒绝结束排队项，graphics 未就绪仍立即拒绝，不绕过呈现 ACK 或阻塞 GUI。
- **回归**：新增生产 folder/controller/shell 路由 **6 项**，修复前 5/6 失败；修复后定向
  **113 实际通过＋4 原有 disabled**；**10/10** 构建成功后由运行时断言检出，9≠10 缺项 guard
  拒绝执行，源码恢复与读回通过。默认 dev 最终 **859 项：852 实际通过、3 skipped、4 disabled、
  0 failed**，完整 lint／format-check 通过，桌面程序已重建。
- **测试时序补正**：首轮全量的旧图片测试因 polish 持续 scheduled 超时；改为先让 text/layout
  更新进入事件循环、再有界检查原两条几何条件，不要求队列空闲或额外 GPU 帧。产品布局未改；
  像素读数越界／状态栏覆盖提示区 **2/2 QtTest 产品变异**检出，产品源逐字节恢复。
- **证据／限制**：`out/verification/video-folder-open-transport/`，首轮失败与复查均保留；详见
  [后续修复协议](video-folder-browser.md#播放中换项提交失败修复)。快照／终态由测试注入，不替代
  用户具体 `videos` 素材重试、屏幕 ACK、硬件长测或发布 ZIP 验收；未提交、未发布。

## 2026-10-01 本机正常入口复核：图片工作区接收视频不切换（S-03，P0，本机已修复）

- **需求与基线**：用户报告另一台机器打不开，要求先测本机。首轮只测试和记录，不修改产品源码。
  源码为 `4a67d1b`，另有既有打包策略工作区；Release 增量构建无待编译目标。
- **明确复现**：先正常打开两张 PNG，再以普通文件参数转发两段视频，或调用当前 HKCU 注册的真实
  `IExplorerCommand::Invoke` 发送两段视频。转发进程退出码／COM HRESULT 均为 0，但目标 HWND
  可访问树仍有 `imageOpenButton` 等图片工具，没有视频走带条；trace 有 `FrameSetReady` 和
  `RenderPublished`，没有 `RenderDrawStarted`／`PresentationAcknowledged`。现有 ZIP 与当前
  Release 构建均复现；不是仅凭窗口存在就判定打开成功。
- **修复前代码依据**：`DesktopApplication::enqueueStartupRequest` 的图片分支调用
  `performImageReview`，视频分支直接入 shell intent。`Main.qml::onIntentFinished` 成功打开视频时
  只重置视口，没有提交视频工作区；`workspaceMode` 不随 sourceCount 自动改变。既有
  `--ui-smoke` 自动化入口额外调用 `activateWorkspace(0)`，所以掩盖这个真实入口遗漏。
- **控制场景**：每个构建记录 12 个正常入口场景，目标 HWND 观察均成功。冷启动的单图／双图、
  单视频／双视频进入对应工作区，视频有呈现 ACK；视频转图片正常。右键场景直接调用已注册的 COM
  服务器，不等同于已经人工检查 Explorer 菜单观感或缓存的历史 DLL。主测试进程关闭后无遗留实例，
  测试用 `DVS_DISABLE_SHELL_REGISTRATION=1` 保留原注册位置，18 个规范入口仍指向同一 CLSID。
- **回归数量**：`pwsh tools/build/build.ps1 -Preset release -Test -TestRegex
  '^(shell_windows\.|app\.|ui\.(ImageReviewControllerTests|ImageFolderPairModelTests|MainQmlContractTests|ReviewControllerTests|ImageWorkspaceManual)|media\.(MediaProbeTests|SoftwareDecoderTests|StillImageDecoderTests))'`
  选中 262 项：**255 实际通过、3 skipped、4 disabled、0 failed**。CTest 的“100% out of 258”
  包含 skipped，不能写成 258 项实际执行通过。未执行完整五分钟硬件／性能门禁。
- **发布包区别**：现有 2.0.3 ZIP SHA-256 为
  `496CD581CDBED41A03CE17E6F6AF6E4CAA660E5E0103458D35C33CCD8C9EEBB6`；静态包校验 15/15。
  ZIP 内 EXE 与当前构建哈希不同，分别实际测试，不将此包冒充当前工作区重新打包结果。
- **证据**：`out/verification/local-open-2026-10-01T06-40-24-786Z/` 的 `target-window/`（ZIP
  正常入口）、`current-window/`（当前 Release）、`release-targeted.log` 和 `summary.md`。
  桌面截图被其他窗口遮挡，已标记为无效，不作为产品画面证据；PowerShell 内直接加载 UIA 的
  探针也因自身程序集解析错误失败，最终以独立目标 HWND UIA helper 的有效结果为准。
  一次性观察脚本的 12 场景数量 guard 控制组通过，故意删一个场景的变异被拒绝；未新增产品断言。
- **修复范围边界**：另一台机器的具体失败仍未复现，需绑定其 ZIP、素材、入口与日志；
  本机这个故障不能解释所有打不开。

### 用户授权后的修复与复验

- **修复**：`Main.qml` 在成功 `OpenSourcesIntent` 结束、shell 已采纳源集后，记录完整视频 URL
  身份，提交视频工作区并恢复其按键接收器。外部 startup 请求提交时不切换图片任务；失败／取消不修改图片
  工作区身份、修订号、焦点或图片内容，关闭后台保留的视频也不替换前台图片任务。
- **首帧等待环**：首次仅增加成功回调提交，控制器回归通过，正常入口仍失败。真实 Open 要等待
  首帧呈现 ACK，隐藏的 viewport 却不能产生 ACK。现仅在视频打开 intent 活动期间让 surface
  在图片任务后方参与渲染，同时禁用其输入；成功终态才切换前台任务并启用视频输入。没有绕过
  ACK-before-commit、伪造成功、清空图片任务或新增 GUI／渲染线程等待。
- **正式回归**：`MainQmlContractTests` 新增 3 项，调用生产 shell 的正常 startup 请求入口，
  覆盖单／双／三视频、成功前 surface 可渲染但不接收输入、成功提交与焦点、失败／取消保留图片、
  关闭后台视频。25 个新增行为检查全部有失败变异证据：15/15 接线／身份／焦点／保留内容／
  首帧可达性变异检出；前后控制组均 3/3，源文件字节恢复校验通过。检查数量 guard 控制组通过，
  故意少一变异时以 14≠15 拒绝执行。
- **最终门禁**：Release 默认测试 preset 选中 815 项，**808 实际通过、3 skipped、4 disabled、
  0 failed**；未包含被 preset 排除的硬件／性能／打包／soak 长测。最终 format-check 与 lint
  通过。CTest 记录解析也检查 815 个唯一用例，删一条记录的变异以 814≠815 拒绝汇总。
- **正常入口复验**：当前 Release 与新 ZIP 各记录 12 个场景，无观察错误或强制结束。原来失败的
  CLI 转发与已注册 `IExplorerCommand::Invoke` 图片→视频场景均切到视频工具，且有真实
  `RenderDrawStarted`、`PresentationAcknowledged` 及 Open 成功终态；反向转图片与冷启动控制
  场景仍正常。不是 `--ui-smoke`，没有自动化额外切工作区。包观察里的 shell 冷启动仍指向
  原注册的 Release；已有 ZIP 主实例时，COM 请求实际转发到该 ZIP 实例，注册位置未改动。
- **测试包**：`out/verification/s03-fix-20261001-162914/package/CompareStation-2.0.3-s03-preview-windows-x64.zip`，
  27,911,491 bytes（26.6 MiB），SHA-256
  `4C8666AB81778298093E362CF930A6194E30D4D16331BFBCAD379214DAD572A6`。
  标准 CPack 必要运行时门禁通过，包静态校验 16/16，解压 EXE 与本轮构建字节相同。
  载荷无新增文件；按既有 P-01 策略去掉四个多余文件，保留所需 app-local CRT／UCRT。
  两个 EXE 与旧 ZIP 不同，旧 ZIP 本就不是当前构建，不能把所有二进制差异归因于 S-03。
  没有覆盖原 ZIP、修改版本／标签或上传发布附件；此包仅用于复测，不是新的正式发布认证。
- **证据与限制**：`out/verification/s03-fix-20261001-162914/summary.md` 与同目录完整日志、
  `mutation-results.json`、`real-open-final/`、`package-real-open/`。`real-open/` 保留首次仅补
  成功回调仍失败的证据。截图尝试因前景 HWND 不匹配被排除，不冒充实际桌面像素证据；结论绑定
  目标 HWND 可访问树与呈现 trace。真实 Explorer 菜单点击、历史 DLL 缓存、另一台机器与完整
  五分钟硬件／性能验证仍未完成。测试结束无残留应用实例，原 shell DLL 注册位置保留。

## 2026-09-30 必要运行时 ZIP 策略（P-01，基线 `4a67d1b` + 本轮工作区）

- **用户要求**：后续打包只交付当前产品必需的运行时；将规则写入文档并持续执行，不仅手工
  精简一次 ZIP。规则见 [构建指南](../building.md#zip-只打包必要运行时) 与根目录 AGENTS。
- **实际体积差异**：2.0.2 为 27,908,959 bytes，2.0.3 草稿为 62,665,162 bytes；只新增
  `vc_redist.x64.exe`、`dxcompiler.dll`、`d3dcompiler_47.dll`、`dxil.dll` 四项，占
  34,752,984 bytes 压缩体积。CPack 日志证实来自 Qt 自动部署，而不是程序本体增长或 PDB。
  `startup-performance-v2.md` 中“只能是手工拷入”的旧归因已标注修正。
- **实现**：`InstallRequiredSystemLibraries` 保留 app-local CRT／UCRT；Qt 显式关闭重复安装器
  和系统图形编译器复制。当前后端固定 D3D11，应用着色器在构建期生成；DXC／DXIL 不属于已交付
  D3D12 路径，D3DCompiler_47 使用 Windows 10／11 系统组件。新增依赖或更换后端时需重新论证。
- **防回退**：CPack 调用 `VerifyRuntimePayload.cmake`，拒绝额外 EXE（包括嵌套路径）、
  开发符号、未交付图形编译器／插件，四个关键 CRT／UCRT DLL 必须非空。新增
  `quality.runtime-payload` 27/27，14 个策略／检查计数故障变异全部检出，恢复控制组 27/27；
  另将 CPack 调用策略的接线移除，证明原门禁拒绝的额外安装器会被接受，接线变异 1/1 检出，
  恢复后完整 staging 控制组通过。
- **本轮验证**：Release 定向 7/7、质量 8/8，format-check／lint／diff-check 通过；标准 CPack 在独立验证目录产出 27,911,207 bytes
  （26.6 MiB）ZIP，包校验 16/16、解压载荷／二进制读回 11/11、CLI 启动／媒体探测和
  单／双／三源 WARP 界面 smoke 通过。证据在 `out/verification/runtime-payload/`。
- **边界**：这是打包工作区预览，不是新的发布候选认证；没有移动已推送的 v2.0.3 标签或替换
  GitHub 草稿附件。干净 Windows 机器和完整硬件／性能验收仍须绑定后续候选，不能以本机 smoke
  或旧版本证据替代。

## 2026-09-30 2.0.3 发布门禁：空闲间隙步进与差异菜单验证（基线 `02dffa7` + 本轮工作区）

- **实际红灯，不归因于远控**：物理 RTX 4090／120 Hz 下，D3D11VA 与 zero-copy 用例通过，
  Wipe、切模式保留帧、旋转 Wipe 的 300 帧均提交／呈现且无序列错误，但 generation delta 为
  300；Diff 只打开下拉菜单、不选择口径，模式验证超时。原始失败证据保存在
  `out/verification/release-2.0.3/failed-02dffa7/`。
- **产品修复**：`PlaybackCoordinator` 的 clean drain 原来清掉整个 step run，下一次按键重新
  进入 `beginInteractiveStepStream` 无条件取消 provider／递增 generation。新增仅包含完整身份
  与显示帧／方向的 warm cursor；空闲相邻步进复用，seek（即使目标为同帧）、方向切换、设备／
  会话／拓扑／时间线／对齐版本变化不复用。没有保留帧引用或隐藏活动任务，request 身份仍独立。
- **验证驱动修复**：先从并排开始，再打开差异菜单选择 `diffPureMenuItem`，避免保存的模式造成
  静默通过；真实 surface 模式／像素／保留帧检查不变。线程基线移到同模式预热完成边界，
  而非未激活菜单／着色器／指标 worker 的初始布局；步进首帧在初始化边界呈现后开始完整
  300 帧稳态计数，generation=0、顺序比例、exact seek／cancel／reopen 与所有性能阈值不变。
- **定向证据**：协调器 87/87（新增空闲正向、空闲反向、同帧 seek、方向切换 4 项）；4 个游标
  变异与 3 个驱动变异均检出，恢复控制组通过。短 Diff 门禁模式／像素／保留帧验证通过、
  300/300 呈现、generation delta=0。变异过程中保留源文件字节备份并在 finally 恢复。
- **发布状态**：短测不是五分钟 Release 通过证据；完整 dev／Release、质量、硬件／性能、
  DPI／关闭 soak 与 ZIP 校验重新绑定修复后的候选 SHA，全部通过才推送并公开 2.0.3。
  日志仍在 `out/verification/release-2.0.3/`，最终结果以发布页为准。

## 2026-09-30 ZIP 升级后的旧右键入口与启动路径（S-01／S-02，基线 `35449f1` + 本轮工作区）

用户反馈：打开视频时 Windows 提示找不到 MP4；解压 2.0.1 与 2.0.2 后右键入口堆叠。
本轮不改视频解码或现有视频项目 Demo 工作区，修复 shell 适配器与对应注册脚本。

- **S-01 · 升级后的启动路径**：`ExplorerCommand::executablePath` 过去只取已加载 DLL 的目录。
  Explorer 持有旧 DLL 时，即使新 ZIP 已接管注册，仍会找旧目录的 EXE；删掉旧目录中的程序会使
  `CreateProcessW` 失败。现在每次调用读取当前用户 CLSID 的 `InprocServer32`，从当前注册目录
  定位 EXE；没有注册时才回退到 DLL 目录。真实 COM 测试先加载旧目录 DLL，再注册新目录，旧目录
  不放 EXE；实际启动探针读回的新 EXE 路径和中文、空格、`&` 文件名均正确。
  此故障形态已复现并验证，**用户那条 Windows 提示的具体入口和原始路径仍待确认**，不能把它
  当作所有“找不到文件”情况的结论。
- **S-02 · 菜单去重**：原清扫按键名排除所有 `CompareStation.Compare`，漏掉别处的同名旧入口。
  现在只保留 18 个规范注册的**完整路径**，扫描扩展名、SystemFileAssociations 和 ProgID 的 shell
  verb，按本命令 CLSID／实际启动的 EXE 判断归属；覆盖版本化键名与旧 `VCStation.exe`。
  普通播放器的参数提到 CompareStation，或 EXE 只是相似名称，都不会被删。规范 verb 上遗留的
  `command` 子键也会被移除，避免保留旧程序路径。卸载走相同清扫，没有“同名”豁免。
- **接管时机与缓存**：启动新版／运行其注册脚本时接管一个稳定 CLSID 与一套菜单，而非每个 ZIP
  一套；仅解压但从未启动不执行注册。旧 ZIP 文件本身不会被删，也不改 Windows 默认打开方式。
  应用在注册修复／清扫改变内容后异步发送 `SHCNE_ASSOCCHANGED`；脚本在清扫和读回之后通知。
  用户关闭标记、环境逃生门、WhatIf、共享父键与其他工具的 verb 保留。
- **验证**：`pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex '(shell_windows\.|app\.)'`
  **66/66**；`-Target format-check` 与 `-Target lint` 通过；脚本真实注册／卸载测试 **85/85**
  （现在强制检查数量），
  脚本完整性与硬编码路径检查通过。变异证据 **13/13**（C++ 8、脚本 5），均让相应断言失败，
  恢复控制组 C++ 27/27、缓存 COM 1/1、脚本 85/85；源码不在变异过程中被替换。
  证据：`out/verification/shell-upgrade/`。真实 Explorer 菜单观感、用户机器上的错误提示与
  HKLM 管理员注册不在本机验证范围内。2.0.3 发布阶段的完整日志另记在
  `out/verification/release-2.0.3/`；最终门禁结果与交付状态以发布页为准。

## 2026-09-29 slate 收口第二轮：选中态、通知栈与余下写死色（分支 `feat/slate-ui-polish`）

收掉下一条「剩余限制」里的写死色、提示压页眉、编辑态三个主按钮和视口身份牌四项，顺带修掉
走查中新发现的重复点击掉选中。一个提交，交互语义与快捷键不变。

- **重复点击掉选中（既有缺陷）**：Qt 在 `onClicked` 之前自己翻转可勾选按钮的 `checked`；当
  `checked` 绑定的是调用方状态时，再点一次已选中的选项，状态没变、绑定不重算，高亮就此消失。
  受影响的有视频对比栏模式键与差异键（打开差异菜单后取消也会残留高亮）、图片模式芯片、
  适应窗口 / 100%、裁剪 / 画笔 / 选择。`ReviewActionButton` 新增 `mirrorsState`，在 `onToggled`
  里再 `toggle()` 一次撤销翻转，由绑定说了算；「更多工具」的 6 个互斥工具改为
  `VcsRadioMenuItem`（互斥项重复点击不会取消）。契约测试用 `invokeMethod("clicked")` 绕开了
  翻转，所以一直没测出来。
- **选中态看得见**：`ReviewActionButton` 原本没有 checked 样式，适应窗口 / 100%、裁剪 / 画笔 /
  选择选没选中看起来一样；现与模式芯片同用 `controlChecked` 填充加强调色边框，悬停、按下时填充
  不变。标签写的是动作的按钮（显示 / 隐藏列表、编辑画面 / 结束编辑）和两个对等选项
  （平滑 / 最近邻）不再着色。
- **一个视图一个实心主按钮**：编辑态「打开图片…」退为普通按钮，「另存副本…」是唯一主按钮；
  裁剪面板里的「应用裁剪」属另一上下文，保留。
- **通知栈**：提示气泡、意图队列、图片打开进度和拖放错误合进一个底部居中的 `notificationStack`，
  落在舞台底部控件（视频的适应 / 重置行，图片的淡化滑块）与悬浮播放条之上，按舞台（抽屉模式
  扣掉检查器）横向居中，最宽 560 px；界面隐藏时移到恢复胶囊下方，避开沉浸 HUD。打开进度原先
  用的是错误红底，现为中性浮板，拖放错误是唯一的红色通知。
- **红色只给失败**：「差异不可用」（缺帧、非精确配对）与「对比非精确对应」横幅改用警告族；
  帧错误横幅、状态浮层错误态和拖放错误改用新增的 `errorPanel / errorBorder / errorText`。
- **写死色收口**：新增画面标记 token（`stageWell`、`stageDivider`、`lineHalo`、`selectionFill`、
  `cropFill / cropBorder`、探针 `probe*`）、`dropScrim`、`successPanel` 和
  `timelineMarkerColor(kind)`；`ComparisonViewport`、`ImageWorkspace`、`Main`、`ClipExportDialog`、
  `TimelineTracks`、`WipeHandle`、`TabbedInspector` 改读 token，半透明一律放进颜色 alpha。剩下
  的字面量只有内容色（画笔色板、棋盘格、JPEG 白底）。沉浸模式的源字母改用各源身份色；视口
  身份牌按文件名收拢（仍以面板宽和 280 px 为上限）；视口的适应 / 重置与缩放徽标补上悬停态。
- **门禁**：dev 构建、`-Target format-check`、`-Target lint`（qmllint 零告警）通过；
  `ctest -R '^ui\.'` **268/268**（新增 1 项；4 项禁用、2 项跳过均为既有状态）；quality 3/3；
  仓库指引检查通过。
- **新增测试与变异证据**：`ui.review_action_button` 用真实鼠标 / 空格输入驱动 5 个用例；删掉
  `onToggled` 修复后，重复点击、空格、仅开菜单 3 个用例失败，切换选中与自持开关 2 个照常通过。
  `ImageEditScaleDialogResizesTheWorkingCopy` 新增编辑态主按钮断言；把「打开图片…」改回常驻
  主按钮后该断言失败，还原后通过。
- **坑**：契约测试的 `settle()` 只排空事件队列、不等时间，取证截图落在 100/120 ms
  `ColorAnimation` 起点，拍到的是过渡前的颜色。编辑态页眉与淡化模式两处取证前改调新增的
  `settleAnimations()`（等 250 ms），其余取证点未改。
- **证据**：`out/evidence/ui-polish-20260929/` 的 12 张按本轮重拍。
- **剩余限制**：悬浮播放条在约 1100 px 以下可能压住视口左下角的适应 / 重置行（既有）；时间线
  「重复」标记与源 B 同为 `#fb923c`，「多余」标记与源 C 的紫相近，`Theme.information` 与源 A
  同为 `#38bdf8`（Alpha 徽标圆点），都待重新分配色相；窄窗口下通知栈可能与右侧问题记录面板
  短暂重叠；二次启动的前台交接仍未跨进程实测（见下一条）。

## 2026-09-29 审查外壳 slate 配色与交互收口（基线 `e5d811e` v2.0.2 + 分支 `feat/slate-ui-polish`）

把视频与图片两个工作区统一到一套 slate 语义色，并在真实窗口里逐屏修掉对齐、截断与误报色，
交互语义与快捷键不变。三个提交：`efab1e6`（配色与逐屏修正）、`1793b02`（原生标题栏）、
`63b0415`（二次启动唤回最小化窗口）。

- **语义色集中**：`VcsTheme.js` 承载整套调色板——表面由 canvas 到 raisedPanel 逐级提亮；控件四态
  （checked 带强调色填充，hover 不带）；强调色拆成边框 / 填充 / 文字三值，白字落在 `accentFill` 上
  保持 AA 对比度；另有成功 / 警告两族、画面上的半透明浮板色与圆角 token。源身份色跟随应用图标
  （A 天蓝、B 橙、C 紫），统一经 `sourceColor/sourceBackground/sourceBorder` 取色，芯片、徽标与
  身份牌不会再给同一槽位不同颜色。菜单栏、源条、对比栏、视口浮层、播放条、时间线、缩略图、
  检查器、图片工作区、文件夹侧栏、拖放确认框、空状态与快捷键浮层都改读 token。
- **逐屏走查修掉的问题**：倍速下拉原先固定 58/68 px 宽，当前倍速被省略成「…」，现按最宽标签
  「0.25×」用 `TextMetrics` 定宽，高度与旁边的连续性开关一致（28/34）；播放条按钮以较高的播放键为
  基准垂直居中，整行共用一条中线；时间映射读数原是压在身份牌上的红框横幅，现为身份牌下方的中性
  浮板（`stageBannerTop` 62/12），红色只留给真正的失败横幅；分析徽标的精确度着色只在差异模式
  生效，并排查看不再出现假警告；时间线入出点区间的透明度改放进颜色 alpha 而不是 `opacity`，
  叠在上面的括号保持完全不透明。
- **原生标题栏**：Windows 上向 DWM 请求沉浸式深色边框与圆角，标题栏底色 / 文字色取
  `Theme.headerBackground`（`#111722`）/ `Theme.primaryText`（`#f1f5f9`），与菜单栏读成同一条
  页眉，`VcsTheme.js` 注明两处须同改。不请求 Mica：窗口自绘不透明背景，Mica 透不出来。冒烟模式
  跳过该调用，UI 桥接库新增链接 `dwmapi`。
- **二次启动唤回窗口**：启动代理把第二次启动的文件交给已运行实例，而它的 `activateWindow()` 只调
  `show()`/`raise()`/`requestActivate()`，最小化的窗口会一直停在最小化状态，文件在看不见的地方
  打开。现只清除最小化标志（最大化 / 全屏保持原样），Windows 上首次显示与交接时另调
  `SetForegroundWindow`。
- **坑**：`Row` 的子项默认顶对齐，高度不同的控件并排时要各自写
  `anchors.verticalCenter: parent.verticalCenter`，否则整行上沿对齐、中线错开；下拉的 contentItem
  是自定义 Text 时，定宽要用 `TextMetrics` 实测最宽标签，不能指望控件自己估宽；截图若落在
  `ColorAnimation`（100/120 ms）起点，拍到的是过渡前的底色，判断状态色要等过渡结束——本轮就因此
  一度数错了编辑态的实心主按钮数量。
- **本轮真实门禁**：`pwsh tools/build/build.ps1 -Preset dev` 通过；`-Target format-check` 通过；
  `-Target lint` 通过（qmllint 零告警）；`ctest -R '^ui\.'` **267/267**（4 项禁用、2 项跳过，均为
  既有状态）；中间提交 `1793b02` 单独编译 `dvs_ui_d3d11_bridge` 通过。`tst_drop_confirmation_dialog`
  改为期待新的页头色 `Theme.panel`。
- **复核 4 项禁用用例**（`--gtest_also_run_disabled_tests` 直接运行）：RangeLoop 用例驱动的是假内核，
  判定 `presentedOutsideRange` 为真；真实保证由 `PlaybackCoordinatorTests` 的 5 个区间用例覆盖
  （5/5 通过），该用例已过时、应删除。WipeHandle 与 Timeline 的无障碍尚未实现（`Accessible.role`
  0 ≠ 8，WipeHandle 不能 Tab 聚焦，Timeline 的 value 0 ≠ 5）。拖动 scrub 合并未实现（`seekCommands`
  19 ≠ `scrubCount` 20），属后续工作。另：连续性开关从来不能 Tab 聚焦，菜单里有等价入口。
- **真实窗口试用**：用户启动构建体验后反馈「之前一些没解决的问题，现在都修好了」。
- **证据**：`out/evidence/ui-polish-20260929/` 12 张——`transport-range-row(-min-width)`、
  `transport-export-chip`、`clip-export-dialog(-min-width)`、`image-edit-header(-min-width)`、
  `image-edit-scale-dialog`、`image-edit-fill-dialog`、`image-fade-mode`、`high-depth-alpha`、
  `image-default-checkerboard`（1280 px 默认宽与 960 px 最小宽）。09-26 的旧图留在 `out/evidence/`
  根目录作前后对照。
- **剩余限制（下一轮）**：QML 里仍有约 100 处写死的十六进制色（`ComparisonViewport`、
  `ImageWorkspace`、`Main`、`ClipExportDialog`、`TimelineTracks` 等；画笔色板与棋盘格是内容色，
  应保留字面量）；提示气泡与意图队列的 `topMargin` 固定为 58，会压住图片工具栏 B 行和视频对比栏；
  图片编辑态「打开图片… / 结束编辑 / 另存副本…」三个同时是实心主按钮；视频视口身份牌宽度只随面板
  （80–280 px），不像图片身份牌那样按内容收拢；二次启动的前台交接未经跨进程实测，第二次启动没有为
  接收进程调用 `AllowSetForegroundWindow`，前台锁仍可能把交接降级为任务栏闪烁。
  前四项（写死色、提示压页眉、编辑态三个主按钮、视口身份牌）已在上方第二轮收口。

## 2026-09-28 资源管理器右键视频出现两个入口，且两个都报「启动动作无法打开」

用户报告：视频文件右键菜单里出现两个 CompareStation 入口，点任意一个都弹出
`fatal the requested startup action could not be opened.`。两件事分别处理，都按产品行为修，不留手工绕过。

- **「无法打开」的根因**：`src/app/Main.cpp` 过去在**一个固定时刻**提交启动请求，提交被拒就当致命错误
  处理——`reportFatalStartup` 弹框 + 写日志 + 进程退出，所以用户看到的是「弹框后应用挂住」。
  被拒的窗口是真实存在的：评审会话要等图形设备就绪（`ReviewView::canOpen` 要求 `graphicsReady`），
  而就绪通知由专用图形泵线程异步送达，本机实测 `sg-initialized +777 ms` → 提交 `+829 ms`，
  52 ms 的余量里胜负全看调度。
- **修法**：新增 `src/app/StartupRequestDispatch.{h,cpp}` 的 `StartupRequestDispatcher`——一次提交，
  被拒且会话会就绪时按 50 ms 重试，上限 10 s；会话就绪后仍拒绝才算失败，且失败只记
  `DVS_STARTUP_PROBLEM` 日志行并向 stderr 报告，**不再终止进程**。同时修正了原代码的顺序错误：
  窗口此前先 `show()` 再提交，现在先提交成功再激活窗口，窗口不会为一次被拒的打开动作先亮出来。
  队列上限 8，溢出时丢最旧的一条并同样只报告不退出。里程碑 `startup-request` 仍打在提交时刻，
  16 个基线里程碑不变。
- **两个入口的根因**：右键菜单按文件关联链拼装，`HKCU\Software\Classes\.<ext>\shell` 与
  `SystemFileAssociations\.<ext>\shell` 都在链上，任何一份遗留的同命令注册都会渲染出第二个同样标题的
  入口。现在每次启动都清扫两处父键下**指向本命令**的 verb（按 `ExplorerCommandHandler` 指向本 CLSID，
  或 `command` 默认值里含 `comparestation.exe`，一律按「指向什么」判断，绝不按键名），保留本命令自己的
  verb，别的工具的 verb 一律不动；清空后的父键按既有规则回收。`-Uninstall` 同样带走遗留副本，
  `tools/shell/RegisterExplorerCommand.ps1` 的注册与卸载两条路径都做同样的清扫并报告 `stale_verbs=`。
- **证据**：`tests/unit/app/StartupRequestDispatchTests.cpp` 10 项（新，`dvs_app_unit_tests`）；
  `tests/unit/shell_windows/ExplorerCommandRegistrationTests.cpp` +5 项（`shell_windows` 18 → 23）。
  判别力：`out/verification/startup-repro/mutate.ps1` **12 个变异体**全部让对应断言失败、控制组全绿
  ——C++ 侧 8 个（永不报告、无重试定时器、越过队列头部就投递、清扫只扫一个父键两遍、认不出遗留副本、
  卸载不清扫、连别人的 verb 一起删、用户已关闭仍然清扫），脚本侧 4 个（注册不清扫、只扫一个父键、
  见 key 就删、`-Uninstall` 不清扫）。`tools/shell/Test-RegisterCompareStationContextMenu.ps1`
  49 → **59 项**，新增 10 项覆盖脚本侧的清扫；其中两项是**先读回刚种下的副本形状**再断言
  （见下）。
- **过程中被抓到的一个静默通过**：清扫用例的种键辅助函数原本用 `[string] $CommandLine = $null`
  区分两种形状，而 PowerShell 会把 `$null` 变成空字符串，于是实际种下的是「空的 `command` 值」——
  一个清扫永远不会匹配的键，用例却因为「种下了、还在」而通过。改为 `[switch] -AsCommandLine`，
  并加两项断言先把种下的形状读回验证（handler 真的是本 CLSID、命令行真的是本程序），
  否则这条用例测的根本不是它声称测的东西。
- **本轮真实门禁**：`pwsh tools/build/build.ps1 -Preset dev` 通过；`-Test` **796/797 通过**（唯一失败是
  与本轮无关的既有红灯，见下）；`-Target format-check` 通过；`-Target lint` 通过；
  `app.*` 端到端 36/36 通过（含走真实带文件启动路径的 `app.ui-shell-smoke`）。
- **真实注册表上的端到端复核**：把一份副本 verb 写到 `HKCU\Software\Classes\.mp4\shell\CompareStation.Open`
  再启动应用，副本被清掉、自有条目仍在；带文件真实启动的里程碑为
  `sg-initialized +1422 ms` → `startup-request +1576 ms`，日志里没有新的 `fatal` 行，进程不再退出。
- **与本轮无关的既有红灯**：`quality.release-contract` 要求 `.github/workflows/release.yml` 发布
  `v2.0.1` 并指向 `docs/releases/v2.0.1.md`，而该工作流仍写着 `v1.9.0`。本轮未触碰
  `.github/`、`docs/releases/`、`packaging/`（`git status` 对这三处 0 项），该红灯在 HEAD 上即存在。
- **仍未在本机验证**：用户机器上的实际观感与注册表实况——本机此前 `HKCU\Software\Classes` 下没有任何
  CompareStation 键，无法复现第二个入口。若注册表干净而菜单仍是两个，那是 Windows 11 把同一个 verb
  同时画进新菜单和「显示更多选项」，属 shell 行为而非产品缺陷。机器级（HKLM）旧注册需要管理员才能清除，
  这一点无法由应用侧自愈。
- **对用户机器的提示**：验证过程中本机的每用户注册项被反复写入与清除，当前 `HKCU` 下已无任何
  CompareStation 注册项；下次启动任意构建会自动重新注册，无需手工处理。

## 2026-09-28 把本轮踩过的弯路变成会失败的检查

同一轮工作的真实成本不在功能，而在三类会复发的操作问题。它们过去只靠文档约定，现已固化进门禁：

- **格式化工具只碰自己负责的文件类型**：clang-format 把未知扩展名当 C++ 处理，据此把一个 470 行的
  `.ps1` 整个重排坏，那还是该测试套件的唯一副本。新检查
  `tools/quality/check-script-integrity.ps1`（CTest 用例 `quality.script-integrity`）要求每个
  `.ps1/.psm1` 都能被 PowerShell 解析、每个 `.cmd/.bat` 都是纯 ASCII + CRLF（cmd.exe 按 OEM 代码页
  读取，裸 LF 会破坏标签扫描——这正是「`-Uninstall` 反而再注册一次」的成因形态）。
- **不硬编码机器路径**：AGENTS.md 早有此要求却无人执行，改名后 `tools/testing/` 里 16 处默认值仍指向
  `G:\Workspaces\Toy`，要等工具真正跑起来才暴露。新检查 `check-hardcoded-paths.ps1`
  （CTest 用例 `quality.hardcoded-paths`）只扫 git 未忽略的仓库文件（`CMakeUserPresets.json` 这类
  本机覆盖文件按设计豁免），要求路径一律从 `$PSScriptRoot` 派生。本轮已改掉那 16 处，并修掉两份文档
  里的过期路径。
- **断言必须可判别**：新断言要附变异证据；检查脚本要断言自己的检查总数，缺锚点不得把断言变成静默
  通过（复验曾用一个「少跑 3 条断言仍打印成功 token」的变体证明了这一风险）。
- AGENTS.md 增补上述规则，并写进指南检查器的必需条款（37 → 44 条）；`docs/building.md` 增补
  「移动或重命名工作区」（`-Fresh`）与「先把依赖重建挂后台，再动代码」。
- **证据**：两道新检查都已在 `ctest` 中通过（`quality.script-integrity` 0.5 s、
  `quality.hardcoded-paths` 0.7 s，遍历时剪枝掉 `out/`，否则单是枚举 vcpkg 树就要 24 s），并且都用
  人为违规验证过判别力：3 条路径违规与 3 条脚本 Encoding/语法违规各自被抓、退出码 1，清理后重新通过。

## 2026-09-28 免安装包自行注册资源管理器右键项（工作区由 `Toy` 改名而来）

发布包没有安装程序，右键项过去只能靠用户手动执行包内脚本。现在应用启动时自己注册并按用户
修复，解压即用；用户显式取消注册后不再自动加回。

- **定位**：注册实现属 shell 适配器（`src/shell_windows/ExplorerCommandRegistration.cpp` 与
  `include/dvs/shell/ExplorerCommandRegistration.h`，新静态库 `dvs_shell_registration`）；
  `src/app/Main.cpp` 只在 `!smokeMode` 时组合调用——所有 smoke/测试模式的参数都以 `smokeMode=true`
  进入 `runDesktop`，故不写注册表；`--open-still`（资源管理器菜单自己发起的真实启动）仍是
  `smokeMode=false`，会走同一段代码。扩展名清单复用 `ExplorerCommandSupport` 的既有 18 项
  （5 视频 + 13 图片），没有第二份拷贝。
- **自愈**：每次启动核对 CLSID 的 `InprocServer32` 与 18 个 verb 的 4 个值；不一致才重写。目录
  移动、注册项被外部清理都属于这一类——正是此前「右键项静默消失」的根因形态。一致时**完全不写**。
  注册前先确认 `CompareStation.exe` 与对应版本的 shell DLL **确实在磁盘上**：否则会写进一个
  Explorer 加载不了的路径，而此后每次比对都「一致」，自愈承诺就永久落空。
- **取消注册闭环**：`-Uninstall` 写 `HKCU\Software\CompareStation\ExplorerContextMenu=0`（DWord），
  启动路径读到 0 就不再加回；重新执行注册脚本写回 1。该值也接受手写文本形式（`0`/`false`/`no`），
  否则用 RegEdit 写下的字符串会被当成「开启」。`DVS_DISABLE_SHELL_REGISTRATION=1` 让某次启动
  完全不碰注册表。
- **只删自己的东西**：共享的 `CLSID` 父键永不删除，只删本命令的 `CLSID` 子键与 18 个 verb；
  清理 `SystemFileAssociations\<ext>` 与 `<ext>\shell` 这两个共享父键前，同时要求**无子键且无值**
  ——`RegDeleteKeyW` 会拒绝有子键的键，却会删掉只带值的键，只看子键会删掉别的工具写在父键上的值。
- **同轮修掉的既有缺陷**：`RegisterExplorerCommand.ps1` 的 `Remove-KeyIfEmpty` 在
  `Set-StrictMode -Version Latest` 下抛异常，`-Uninstall` 只删掉第一个扩展就中断（即「可卸载」
  从未真正成立）；双击入口 `.cmd` 不转发 `%*`，文档里的 `-Uninstall` 会反向再注册一次；`pause`
  被两条错误守卫（带引号的 `if not exist "con"`、`call )` 把 ERRORLEVEL 设成 1）变成不可达代码，
  双击后窗口一闪而过。
- **验证**：`tests/unit/shell_windows/ExplorerCommandRegistrationTests.cpp` 13 项（原先 8 项，按三轮复验
  发现补齐：shell DLL 缺失时拒绝注册、共享父键上「别的工具写的值」不得被牵连、文本形式的关闭标记、
  版本号推导、环境逃生门不改动真实注册项、用户已关闭优先于文件缺失），加同文件 5 项既有支持测试，
  合计 18/18。判别力用变异体现验：8 个变异体（去掉空键判定、只修一半的空键判定、去掉存在性检查、
  忽略文本标记、永不清父键、放宽版本 minor、忽略逃生门、把用户判定挪回文件检查之后）全部让对应断言
  失败，控制组全绿。`tools/shell/Test-RegisterCompareStationContextMenu.ps1` 49 项；跨组件契约
  `out/verification/startup-registration-contract/contract.ps1` 16 项，验证「脚本写的标记 ↔ 应用读的
  标记」一致（含标记必须是 DWord，否则应用会把「已关闭」读成「开启」）以及**卸载后应用不再加回**。
- **本轮真实门禁**（重装依赖、重新 configure 之后）：`pwsh tools/build/build.ps1 -Preset dev` 通过
  （431/431，含架构校验与全部测试目标）；`-Test` 全量 **780/780 通过**，其中 `shell_windows` 20 项
  （含 `ExplorerCommandSmokeTests` 两项在真实 COM 服务器上的组件测试）与 `app.ui-shell-smoke` 均通过；
  `-Target format-check` 通过（C++ clang-format + QML qmlformat + MSVC 依赖检查）；`-Target lint` 通过。
- **证据**：上述 CMake 门禁的真实输出；另有 `out/verification/shell-registration-tests/`（仓库 gtest
  18/18 与顺序变异体，均用项目同款 `/W4 /WX /permissive- /utf-8 /std:c++20` 独立编译运行）、
  `out/verification/startup-probe/`（契约用启动探针，另含只编译不运行的调用形状复现，专门覆盖
  `Main.cpp` 把窄 `DVS_PROJECT_VERSION` 交给版本参数的写法——复验指出这一层此前无人编译）、
  `out/verification/startup-registration-contract/`、`out/verification/self-registration-review/`
  （三轮独立复验报告与变异框架）。独立验证员三轮复验：第一轮发现应用目标版本参数编译不过的
  BLOCKER 与三处自愈/卸载缺陷，第二轮发现 F2 断言没有判别力与校验顺序问题，第三轮全部 PASS。
  仍未在本机验证的部分：真实 GUI 首次启动（会弹出窗口且 dev 目录未部署 `qoffscreen` 平台插件）、
  Explorer 里的实际观感、覆盖率与 cpack 布局。

## 2026-09-26 图片工作区编辑与对比界面整理（交互/审美一轮收口，基线 `02cf55e` + 本轮工作区）

按「使用体验与视觉质量优先」对 `ImageWorkspace.qml` 做一次性整理，不动交互语义与快捷键。

- **命令区两行 Flow**：A 行＝文件与视图（打开图片…/打开图片对▾/关闭图片｜适应窗口/100%/重置视图），
  B 行＝对比模式芯片（手动闪烁/并排/分割线/淡化/差异▾）＋通道（RGB A▾）与背景▾；
  960px 最小宽度下自然换行、不再横向溢出，分隔符用整行高的 `RowSeparator` 且随相邻按钮
  可见性联动（修复无文件夹时 A 行末尾悬空分隔线）。
- **编辑套件成组**：「结束编辑｜工具组（裁剪/画笔/选择）｜缩放…/填充画布…/更多工具▾｜撤销/重做｜
  另存副本…＋编辑模式说明文本」，参数井承载「应用裁剪」等操作，编辑态语义一目了然。
- **视口身份牌**：每个视口左上角半透明浮板（标题＝文件名，副行＝槽位·尺寸·格式·α·数值审查路径），
  悬停显示完整路径 tooltip，文字按内容收拢、过长中间省略，纯展示不拦截画布平移/框选；
  分割线模式左右身份由裸文字升级为同款浮板，淡化模式新增「A · x ↔ B · y」身份浮板——
  各对比模式的身份呈现首次统一。
- **坑**：父项 `visible` 绑定到**子项的** `visible` 时不会随孙级源属性重评估（浮板曾因此永不
  显示）；父/子应各自直接绑定同一源表达式。定位器（Column/Flow）的隐式尺寸跟随子项
  **width/height**，故收拢宽度要设在 Text 上而不是依赖 Text 的 implicitWidth。
- **状态条**：左侧缩放/像素读数，右侧转换/重采样/α 直通徽标与操作提示。
- **验证**：`dev` 全套测试通过；`format-check`、`lint` 通过。不变量（objectName、快捷键、
  缩放/平移/框选/分割线/淡化语义、`imageEditApplyCropButton` 可见性规则）全部保持。
- **证据**：`out/evidence/image-edit-header.png`（并排＋编辑行＋身份牌＋十字准星）、
  `image-edit-header-min-width.png`（960px 换行）、`image-fade-mode.png`（淡化身份浮板）、
  `image-default-checkerboard.png`（棋盘格与 α 身份牌）、`image-edit-scale-dialog.png`、
  `image-edit-fill-dialog.png`。

## 2026-09-26 视频第六增补：区间导出＝无损流拷贝裁剪（P1 · 实施第三步，基线 `02cf55e` + 本轮工作区）

对应产品目标 §3 新增的「区间导出」。用户把视频编辑收窄为「选定起点/终点裁剪 + 缩放」，
第五增补让区间一步可达，本轮补上它的**结果**：把入点到出点导出成一个可独立播放的片段。

- **为什么是流拷贝**：本轮不做解码、不做重新编码，按包（packet）复制，因此片段与源素材
  逐帧一致，分辨率、帧率、编码不变。两条代价必须在界面上讲清楚，不能让人事后才发现：
  视频只能在关键帧处开始拷贝（入点在关键帧之间时片段会比入点早开始，计划里记录这段
  前滚 `startShiftMicroseconds` 与 `firstExportedFrame`）；B 帧素材为了让解码正确，会带上
  显示顺序晚于出点的参考帧（结尾可能多出几帧）。精确成帧裁剪属于第四步重新编码。
- **应用层（纯函数、可单测）**：`application/ClipExport.h` 定义
  `ClipExportPlan`/`planClipExport`/`alignClipExportStart`、`ClipExportJob`、
  `ClipExportReport` 与端口 `IClipExporter`（`keyframeTimes`/`perform`，都带
  `std::atomic_bool` 取消标志）。结束时间是「出点下一帧的起点」（**独占**），到文件末尾时
  为 `nullopt`；对齐取「不大于请求起点的最大关键帧」，若素材没有更早的关键帧则保持原起点
  （此时 shift 为正），对齐后越界则拒绝而不是偷偷延长。
- **适配器**：`media_ffmpeg/ClipExportWriter`（`AvRaii.h` 管 FFmpeg 资源，不外泄类型）。
  `keyframeTimes` 优先读 `stss` 索引（FFmpeg 8 的 `avformat_index_get_entries_count`/
  `avformat_index_get_entry`），没有索引时退回扫包；`perform` 反向 seek 到计划起点，
  在**第一个关键帧包**处才创建输出（仅 mp4/mov 加 `movflags=+faststart`），以首包 pts 归一化
  时间戳，`dts >= 结束时间` 停止、`pts < 结束时间` 才写入，越界参考包先缓存再在结尾补写。
  2026-10-06 后续修正：改用现有 `AtomicFilePublisher` 的独占临时文件，检查 AVIO 关闭与
  flush 后再提交，移除失败时删除旧目标的回退；取消在提交前保留旧文件。普通失败清理临时
  文件；恢复失败则保留原文件备份和新片段并报告路径。云端协议与故障测试通过，Windows
  文件锁、Unicode 与固定依赖验收仍待完成，见 [导出提交保护](clip-export-publication.md)。
- **控制器 `ui_qml/ClipExportController`（纯应用端口，不链 ffmpeg）**：`exportRange(QUrl)`
  在 GUI 线程把整个作业（规范源路径、`CanonicalTimeline`、规范帧数、区间、目标路径）拍成
  `Request` 再交给一个 `jthread`，工作线程**不读快照**；进度与结果用带请求号的 queued 调用
  回到 GUI 线程，过期结果按请求号丢弃；同一时刻只允许一个导出。导出源固定取
  `validatedComparison` 的**规范源**（时间线主源）路径与其规范时间，不用 `sourceFullPaths()`
  ——那只是槽位顺序，可能在用户对调 A/B 后与规范源不一致。
- **界面**：走带条区间行新增「导出」芯片（`transportExportRangeButton`，导出中显示
  「导出中 nn%」并点亮），点击打开 `ClipExportDialog.qml`（`clipExportPopup`）：区间摘要、
  建议文件名（`<源名>_clip_<入>-<出><扩展名>`）、状态行、结果路径、失败原因，以及上面两条
  限制说明；目标位置用原生保存框（`clipExportTargetDialog`），按钮为「停止导出」（仅导出中
  出现）、「关闭」、「选择位置…」。区间不完整时芯片禁用并给沉浸式提示「没有可导出的区间…」；
  没有导出端口时（未来精简构建）芯片整体不出现。
- **坑**：`ClipExportController` 的 `canExport`/`rangeSummary` 只在 `stateChanged` 上发信号，
  而 QML 的区间状态来自 shell，所以 `rangeExportEnabled` 必须同时读**实时的**
  `root.inFrame`/`root.outFrame`，不能只绑 `canExport`，否则设完入点后芯片还停在禁用态。
  对话框同样受影响：设点不会触发 `stateChanged`，首次证据截图抓到的就是残留的
  「未设区间」——`ClipExportDialog` 现在在每次 `open()` 时自增 `rangeRevision`，迫使区间
  摘要与建议文件名的绑定重新读取，契约测试直接断言对话框里显示的是当前区间。
  另外 `CanonicalTimeline` 是 `std::variant<RationalRate, …>`，而 `RationalRate` 没有默认
  构造，所以 `Request::timeline` 用 `std::optional` 承载，不能直接当成员。
- **测试**：
  `application.ClipExportPlannerTests`（7 项：拒绝非法帧数/倒置/越界区间、结束时间独占、
  无关键帧表报 `kMediaProbeFailed`、取前一个关键帧、对齐后越界拒绝）；
  `media.ClipExportWriterTests`（6 项：真实 fixture 导出计划区间、包数与起点前滚断言、
  取消不落盘、临时文件改名、无 `stss` 素材的关键帧回退、越界参考帧补写）；
  `ui.MainQmlContractTests.ExportRangeButtonStartsAClipExport`（无区间时芯片禁用并提示、
  有规范源与区间后芯片可用、点击打开对话框、原生选择框**不自开**、直接走
  `dialog.onAccepted` 调的同一个 `exportRange()` 后由记录型假 exporter 断言收到的作业：
  源＝规范源、起点前滚到 0、`startShiftMicroseconds = -100000`、独占结束 266667 µs、
  请求号非 0、完成态 HUD/状态/输出路径一致）。
- **证据**：`out/evidence/transport-export-chip.png`（1280px：区间行里的「导出」芯片）、
  `clip-export-dialog.png`（1280px：对话框打开，区间摘要 + 建议文件名 + 两条限制说明）、
  `clip-export-dialog-min-width.png`（960px 最小宽度：对话框宽度取 460px 上限并与走带条共存）。
- **剩余限制**：精确成帧需第四步重新编码（`h264_mf` 等编码器运行时探测后再做）；中间切点
  的关键帧数学只有规划层单测覆盖——测试素材都是单关键帧，端到端只验证了「回到文件开头」
  这一种前滚；`vcpkg.json` 未变（依旧没有 encoder/avfilter）。
- **旧账**：「启动时提示『磁盘上的视频文件已变化』」的 tooltip、对比模式芯片行在 960px
  溢出，均为既有现象，本轮未引入亦未处理。

### 2026-10-08 VFR 区间导出入口（P2-A，#43 已合入，Windows 流程待验收）

- **基线与问题**：`880d02ad` 的 `ClipExportController::makeRequest` 用
  `canonicalRate()` 是否存在判断视频；合法 VFR 描述符恰好没有 nominal rate，因此已标记
  的 VFR 区间被入口拒绝，尽管现有 planner 能处理索引时间线和末帧的无上界导出。
- **最小修复**：入口改为使用视频会话的 validated comparison 与 canonical timeline；
  保留源路径、完整入出点，检查可表示帧数、非空且帧数一致的 VFR 索引，并复用 planner
  拒绝负数、倒置、越界区间。图片仍使用独立 `ImageReviewController`，图片-only 的空视频
  快照保持拒绝；不用扩展名判定视频能力，合法无扩展名输入仍能进入。
- **定向证据**：新增 `ClipExportControllerTests.cpp` 并注册现有 UI component target。
  云端 GCC 14.2 / QtCore 6.8.2 / GoogleTest 1.17 下直接编译生产 controller、planner 和
  domain 依赖，20 项 controller + 11 项已有 planner 测试通过；controller 覆盖 VFR/CFR
  中段与末帧、无后缀源、规范源与 reference 分离、快照切换后的请求捕获，以及 15 项拒绝态。
  原基线恰在 3 项 VFR 正例失败；18 个编译后运行时变异均被检出，22 个新增断言点各有
  失败证据。`RecordingExporter` 只记录调用，不读写媒体；这不是 FFmpeg 导出／重新打开验证。
- **提示文案**：功能验证后仅将 Main.qml 和 controller 的两条泛用提示改为检查视频会话与
  区间有效性，不再一律要求先设置入出点。静态反向替换证明只有两条字符串改变；
  既有 MainQmlContractTests 的精确提示预期同步一条字符串，断言和调用不变；
  31 项测试证据针对此前功能代码，该 GUI 合同与最终文案未执行。
- **范围与未验收**：不改 QML 行为、planner、writer、像素管线或播放逻辑；关键帧
  前滚及 B 帧结尾限制不变。未运行 Windows/MSVC、锁定 Qt、完整构建／CTest、format/lint、
  真实素材端到端导出、GUI／D3D11VA、性能与打包门禁；完整工作流按用户要求延后。

## 2026-09-26 视频第五增补：区间标记与循环（P1 · 实施第三步，基线 `02cf55e` + 本轮工作区）

对应产品目标 §3 新增的「区间标记与循环」。用户把视频编辑收窄为「选定起点/终点裁剪 +
缩放」，本轮只做前一半的**可见状态**：区间从此在走带条上一步可达，不再依赖弹窗或记忆。

- **走带条新区间行**：`TransportBar` 增加 `transportRangeRow`，五个芯片
  （`transportMarkInButton` 入点、`transportMarkOutButton` 出点、
  `transportPlayRangeButton` 播放区间、`transportLoopRangeButton` 循环、
  `transportClearRangeButton` 清除）加一个状态文本 `transportRangeLabel`。芯片沿用
  连续性芯片的视觉（`#0f172a`/`#2563eb`/`#26364d`），活动态蓝底，`ToolTip.delay: 650`
  给出快捷键提示；文本三态：`未设区间（I / O 设点）` → `入 42 · 出 —（区间无效）` →
  `入 42 · 出 48 · 7 帧`（循环时追加 ` · 循环`）。帧号一律 1-based，未设端点显示 `—`。
- **区间语义**：端点各自独立可设，闭区间；有效条件是 `inFrame >= 0 && outFrame >= inFrame`，
  所以只设一端或倒置时文本明确写「区间无效」。门控：标记两芯片要 `canMarkRange`
  （`timelineEnabled && currentFrame >= 0`），播放/循环要区间完整，清除只要有任一端点；
  没有媒体时整行隐藏且芯片禁用。无音频、无字幕的产品边界不变。
- **状态来源与单一事实源**：区间真值仍在 `ReviewShellController`
  （`inFrame()`/`outFrame()`/`rangePlaybackActive()`），走带条只读投影；
  新增 `markInRequested`/`markOutRequested`/`playRangeRequested`/`rangeLoopRequested`/
  `clearRangeRequested` 五个信号，`PlayerOsc` 原样转发，`Main.qml` 接到既有
  `setInPoint`/`setOutPoint`/`playSelectedRange`/`toggleRangeLoop`/`clearSelectedRange`。
  循环开关的停止路径复用 `stopRangeLoop()`——它提交 `SetPlaybackRangeCommand`（区间保留、
  只把 loop 置 false），不会悄悄清空用户刚设的端点。键盘 `I`/`O`/`\` 不变，循环仍只有芯片。
- **坑（本轮最大的一个）**：`ReviewController` 的 `projectionTimer_` 每拍用快照重建视图，
  任何只写在视图层的区间状态会被下一拍抹掉。契约测试因此必须先断言**提交的命令**
  （`StartRangePlaybackCommand` / `SetPlaybackRangeCommand`）与 shell 状态表示意图，再把
  已被接受的状态镜像进快照、`refreshProjection()` 后再断言芯片与文本——这与其它播放
  字段一样是投影驱动，不是测试绕过。
- **测试**：`ui.MainQmlContractTests` 新增
  `TransportRangeRowMarksInAndOutFromTheCurrentFrame`（芯片设点 41→47、标签三态流转、
  `inMediaTime()` 与 `mediaTimeForFrame(41)` 一致）、
  `TransportRangeRowPlaysAndLoopsTheMarkedRange`（播放提交 `StartRangePlaybackCommand{41,47,loop=true}`、
  循环芯片活动、停止提交 loop=false 且端点保留）、
  `TransportRangeRowClearsTheRangeAndGatesChipsWithoutMedia`（清除归零、无媒体时隐藏与禁用）。
  定向套件（`ui.MainQmlContractTests|ui.ImageEditControllerTests`，75 项）100% 通过，4 项既有
  禁用未动；`format-check`、`lint` 通过。
- **证据**：`out/evidence/transport-range-row.png`（1280px 默认宽度：区间行在播放按钮下方，
  循环芯片活动态、蓝色区间带落在时间轴上）、`transport-range-row-min-width.png`
  （960px 最小宽度：整行完整不溢出）。
- **剩余限制**：对比模式芯片行在 960px 仍溢出（第三增补已记录的旧问题，非本轮引入）；
  启动时提示「磁盘上的视频文件已变化」的 tooltip 为既有现象，本轮未处理；区间导出
  （无损流拷贝裁剪）已由第六增补落地，本轮只做可见状态与循环播放。

## 2026-09-26 图片轻编辑第四增补：缩放、填充画布与精简工具栏（P1 · 实施第三步，基线 `02cf55e` + 本轮工作区）

对应产品目标 §5 新增的两行。用户收窄了编辑范围——图片编辑就是「裁剪、缩放、填充」，
并要求界面更简洁精炼。本轮把两个整图几何操作落地为各自的撤销步，同时把编辑工具栏
从九个同权按钮压到三个常用按钮加一个下拉菜单。

- **缩放（像素尺寸重采样）**：`resizeImage(w, h, smooth)`，`ResampleCommand` 一步撤销；
  默认 `Qt::SmoothTransformation`，像素级需求可切 `FastTransformation`；标注几何与文字
  字号按比例缩放（下限 1px）；尺寸不变时如实提示且不产生历史步骤。边长限制
  [1, 16384] 由控制器统一把关。
- **填充画布（居中补边）**：`padToCanvas(w, h, color)`，`CanvasCommand` 一步撤销；原图
  居中放入 W×H 画布、余量用指定颜色按 `CompositionMode_Source` 填充（保留 alpha），
  标注随居中偏移平移且不丢弃；画布小于原图时拒绝并提示，透明填充仅当图像有透明
  通道时可用。画布格式按 `working.hasAlphaChannel()` 取 ARGB32/RGB32。
- **历史从 QUndoStack 换为私有有界 `History`**：裁剪/缩放/填充画布会整体保留上一个
  缓冲，只有计数上限会留下大缓冲；新增字节预算（默认 256 MiB，可配置），`prune()` 保证
  刚压入的步骤永不被剪掉。`maximumEdge`/`minimumEdge` 作为 QML 常量属性暴露给对话框。
- **对话框（新 `ImageEditDialogs.qml`）**：缩放对话框——宽/高 SpinBox（`IntValidator`、
  锁定比例联动另一边、50/100/200% 预设、平滑重采样开关）；填充画布对话框——宽/高 +
  黑/白/中灰/透明色样（透明样用棋盘格 data-URI、无透明通道时警示不可保存为不透明
  副本）。对话框故意不预判「画布小于原图」，由控制器拒绝（单一事实源），只预提示
  透明填充。沿用 `ReviewInputDialogs` 的 DialogShell 壳（遮罩居中、底部取消/应用）。
- **工具栏精简**：马赛克/填充/清除/矩形/箭头/文字与标注管理收进「更多工具」
  `VcsMenu`，菜单按钮始终显示当前工具名（隐藏中的工具不会不可见）；新增内联
  `component EditButton` 按文字宽度收缩按钮（突破 `ReviewActionButton` 的 112px 下限）。
  **坑**：`Control.implicitContentWidth` 没有 change 信号，绑定到它在构造期就定格为 0；
  必须绑 `contentItem.implicitWidth`。960px 最小宽度下编辑行完整可见（状态文本允许
  省略号）；对比模式芯片行在 960px 仍溢出，是本轮之前就存在的旧问题。
- **测试**：`ui.ImageEditControllerTests` 增至 35 项（缩放尺寸/平滑/标注随动、无操作
  拒绝、填充居中与余量颜色、拒绝更小画布、透明通道门槛、撤销一步还原、字节预算剪枝）；
  契约测试新增 `ImageEditScaleDialogResizesTheWorkingCopy` 与
  `ImageEditFillDialogPadsTheCanvasAndKeepsTheImage` 走完整链路（开对话框 → 改尺寸 →
  应用 → 逐像素断言 → 撤销还原）。`SpinBox.valueModified` 只对用户输入触发，测试用
  `typeSpinBoxValue` 助手强制信号。定向套件（`ui.ImageEditControllerTests|ui.
  MainQmlContractTests`，68 项）100% 通过；`format-check`、`lint` 通过。
- **证据**：`out/evidence/image-edit-header.png`（默认宽度编辑行）、
  `image-edit-header-min-width.png`（960px 最小宽度）、`image-edit-scale-dialog.png`、
  `image-edit-fill-dialog.png`。
- **剩余限制**：JPEG 编码器接入（第一增补记录）；填充画布不支持任意锚点（产品明确
  不做）；工具栏重组提案 HTML（工作区未跟踪文件）本轮仅作参考、未采纳其多行分组方案。

## 2026-09-25 图片轻编辑第三增补：填充/清除与标注（P1 · 实施第三步，基线 `1f51487` + 本轮工作区）

对应产品目标 §5 的第四、五行，第三步到此功能面收齐。审查的两条边界被正面实现：
「填充先做纯色与透明、不做内容感知」以及「标注可移动和删除，但**不要**完整图层面板」。

- **填充／清除**：`fillImageRect` 用当前颜色填满选区（遮挡敏感内容用不透明色块）、
  `clearImageRect` 清除为透明；两者都是补丁式像素编辑，各自一个撤销步。
- **标注＝扁平几何列表，不是图层**：`addArrow` / `addRectangle` / `addText` 生成对象，
  上限 64、后加的在上；像素编辑在 `working`、标注在其上合成出 `composite`，**显示与另存
  用 composite，差异管线与统计永远只用已提交原图**——这正是审查要求的「编辑与对比分开」。
  新增 `selectAnnotationAt`（命中测试：线段距离／矩形／文字包围盒）、
  `beginAnnotationDrag`/`dragAnnotationTo`/`endAnnotationDrag`（**一次拖动只入栈一步**）、
  `moveSelectedAnnotation`（微调）、`deleteSelectedAnnotation`、`clearAnnotations`。
- **裁剪与标注的关系**：`CropCommand` 现在同时保存标注列表，redo 时平移并把完全落在保留区
  之外的标注丢弃，undo 精确复原——避免「裁完标注飘走」这类几何脱节。
- **QML**：编辑行拆成两行——工具行（裁剪/画笔/马赛克/填充/清除/矩形/箭头/文字/选择＋
  撤销/重做/另存/状态）与参数行（颜色、粗细/线宽、不透明度、块大小、标注文字与字号、
  应用裁剪/马赛克、删除标注/清除全部标注、标注计数），避免窄窗把动作挤掉；
  文字工具是「输入内容 → 点击放置」，Delete/Backspace 删除选中标注。
- **测试发现并修掉的真实缺陷**：矩形选区此前要求两轴都 ≥2px，导致**水平或垂直箭头根本
  建不出来**，而且归一化选区丢掉了箭头方向。现在箭头按拖动端点判定（只要求长度）并使用
  原始端点，方向得以保留。
- **测试**：`ui.ImageEditControllerTests` 增至 22 项（填充/清除与撤销、标注渲染进合成图而
  不进原图、点选/移动/删除/清除全部、拖动一次仅一步、裁剪平移标注并丢弃区外标注、64 上限）；
  契约测试 `ImageEditAnnotationAndFillToolsEditTheWorkingCopy` 通过工作区走完整链路
  （矩形/水平箭头/文字 → 点空清除选中 → 拖动 → 删除 → 撤销 → 填充/清除 → 清除全部 →
  已提交原图逐像素不变）。定向套件、format、lint 见本轮日志。
- **第三步余项**：JPEG 编码器接入（第一增补记录）。

## 2026-09-25 图片轻编辑第二增补：画笔与马赛克（P1 · 实施第三步，基线 `86c53dd` + 本轮工作区）

对应产品目标 §5 的第二行与第三行。第一增补已立好会话/撤销/另存架构，本轮把「顺手处理」
最常用的两个笔刷工具接上，并让历史的内存代价与笔画大小成正比而不是与整图成正比。

- **画笔**：`beginStroke(color, width, opacity)` / `strokeTo(x, y)` / `endStroke()`，端点
  按图像像素传入（QML 侧由 `mapToImage` 换算），宽度 1–256、不透明度 0.05–1；圆头圆角、
  单点画圆点。**一次手势 = 一个撤销步**：`endStroke` 才把整段笔画的脏矩形补丁压栈。
  笔画进行中把工作副本实时画出来，但**预览按 66 ms 节流**刷新提供器 URL——否则每个鼠标
  移动事件都会让 QML Image 重传整幅纹理。
- **马赛克**：`mosaicImageRect(x, y, w, h, blockSize)` 用最近邻「缩小再放大」得到确定性
  像素块，块保持不透明（遮挡敏感内容的语义），块大小 2–64 并按选区尺寸收敛。
- **历史改为补丁式**：新增 `PatchCommand`（脏矩形 + 前后补丁 + `CompositionMode_Source`
  回填，撤销能精确还原 alpha），与既有 `CropCommand`（几何步，保留裁剪前缓冲）混用同一
  `QUndoStack`。补丁只覆盖自己的矩形，测试断言撤销笔画不会把无关像素一并回滚。
- **QML**：编辑行新增工具切换（裁剪／画笔／马赛克）、画笔颜色预设（6 色）、粗细与不透明度
  滑杆、马赛克块大小滑杆；「应用裁剪」按钮按工具改为「应用马赛克」；编辑模式下左键执行
  当前工具，中键仍平移、Shift+拖动仍是放大。
- **测试**：`ui.ImageEditControllerTests` 增至 16 项（新增：笔画落在图像坐标且撤销精确、
  一笔一撤销步、粗细/不透明度生效、马赛克成块且可撤销、画笔与马赛克共享有序历史、
  无会话时拒绝且会话中途退出不留下笔画状态）；契约测试
  `ImageEditBrushAndMosaicToolsEditTheWorkingCopy` 通过工作区走完整流程（切工具 →
  画笔涂抹 → 马赛克选区应用 → 两次撤销回到已提交原图）。定向套件、format、lint 见本轮日志。
- **余项**：第三增补（填充/清除 + 箭头/矩形/文字标注）；JPEG 编码器接入（第一增补记录）。

## 2026-09-25 图片轻编辑第一增补（P1 · 实施第三步，基线 `957ea22` + 本轮工作区）

对应 2026-09-25 审查 §三与产品目标新增的 §5：先立编辑架构（原图不变、撤销/重做、另存
副本），再补画笔/马赛克（第二增补）与填充/箭头文字（第三增补）。

- **`ImageEditController`（新，ui_qml）**：会话从**已提交解码缓冲**开始（新增
  `ImageReviewController::rawImageForSlot`，明确排除派生通道视图作为编辑源），原图深拷贝
  后不再写入；`QUndoStack` 承载有界历史（默认 8 步、可配置），裁剪命令保存裁剪前缓冲以便
  撤销；`editedImageUrl` 是随每次改动变化的属性（`dvs-edit` 图像提供器），`saveCopy` 用
  `QSaveFile` 事务性写盘、**拒绝写入会话来源文件**，并按格式执行透明规则。
- **JPEG 背景规则的现状（重要）**：本仓库的 vcpkg Qt6 **未包含 qjpeg 插件**
  （`out/vcpkg/x64-windows/Qt6/plugins/imageformats` 只有 gif/ico/svg），因此 JPEG 保存
  在此构建中不可用。控制器实现并单测了「JPEG＋含透明 ⇒ 必须先选合成背景」的规则
  （`flattenOntoBackground` 静态可测），编码器缺失时如实返回「当前构建未包含 JPEG 编码器，
  请另存为 PNG」，不写坏文件。**后续项**：接入 qjpeg 或 FFmpeg 编码后再开 JPEG 另存 UI。
- **QML**：`ImageWorkspace` 新增编辑行（编辑画面/应用裁剪/撤销/重做/另存副本…＋状态文本）、
  编辑模式下的裁剪框选（左键框选、中键仍平移、Shift+拖动仍是放大）、裁剪可视化矩形、
  编辑副本通过 `dvs-edit` 提供器显示于原槽位（对比与统计仍用原图）；控制器以可选上下文
  属性 `imageEdit` 注入，缺失时整行隐藏（轻量 QML 夹具不受影响）。
- **测试**：新增 `ui.ImageEditControllerTests`（10 项：原图不可变、裁剪钳制与退化矩形、
  撤销/重做与标签、历史上限、PNG 保透明与不覆盖源、JPEG 背景规则与编码器缺失分支、
  `flattenOntoBackground` 合成、退出会话、URL 失效）；契约测试
  `ImageEditModeCropsAWorkingCopyAndKeepsTheOriginal`（进入编辑→面板切到编辑提供器→
  视口拖动换算成图像像素选区→应用→撤销/重做→已提交原图尺寸不变→退出恢复原面板源）。
  定向套件、format、lint 见本轮日志。
- **余项**：第二增补画笔＋马赛克；第三增补填充/清除＋箭头/矩形/文字标注；JPEG 编码器接入。

## 2026-09-25 一键复制带标注对比图（P1 · 实施第二步「做好比较」余项二，基线 `5b1ced8` + 本轮工作区）

对应 2026-09-25 审查 §五最后一项：「一键复制／导出带 GT、预测名称、局部裁剪和标注的
对比图」。此前的截图能力只存在于问题记录（整窗抓取、随记录存储），没有面向报告粘贴的
对比图出口。

- **新控制器 `ComparisonExportController`**（ui_qml，注入为上下文属性 `comparisonExport`，
  缺省时 QML 全部守卫）：`copyComparison(viewport, captionLines)` 抓取当前窗口并按视口的
  设备像素矩形裁剪（复用 DesktopApplication 自动化截图的映射算法），`composeLabeledCapture`
  在下方拼接**真中性灰标注条**（#1a1a1a/#303030/#ededed——导出图不得给颜色判断引入偏色），
  行按 DPR 缩放、超宽 elide；`saveComparison` 用 `QSaveFile` 事务性写 PNG。捕获是
  **显示结果**（窗口抓取），不是原始码值。
- **入口与标注内容**：对比模式栏「复制对比图」按钮（`copyComparisonButton`）→
  `Main.qml::comparisonCaptionLines()` 生成三行：来源行（三源标 GT 与两个预测，双源列名）、
  观察行（模式·帧号·激活配对·阈值）、`CompareStation · 时间戳`；HUD 反馈结果。
- **测试**：`ComparisonExportComposesCaptionBarInNeutralGray`（纯合成：精确裁剪、
  标注条中性灰 R=G=B 且暗于内容、文字着墨>20px、空标注返回裸裁剪、空裁剪返回空）；
  `CopyComparisonButtonPutsLabeledViewportOnClipboard`（契约：按钮→剪贴板图像尺寸≈视口
  设备像素＋标注条、状态文案、PNG 魔数与事务性另存。Windows OLE 剪贴板异步完成，
  测试在写入间泵事件并重试）。定向套件、format、lint 见本轮日志。
- **边界**：标注文字含观察条件但**不含**结论——它帮助别人看懂图，不替用户判断好坏；
  图片侧的导出（局部裁剪另存等）仍按第四步图片轻编辑推进。
- **第二步余项**：固定 GT 文件夹＋切换预测文件夹的对比实验流程（与第一步待排查项并行，
  等待真实素材验收）。

## 2026-09-25 差异按钮三选项与按住看原图（P1 · 实施第二步「做好比较」余项一，基线 `05b2686` + 本轮工作区）

对应 2026-09-25 审查 §五「差异高亮应该帮助找问题」：让「差异」按钮直接提供审查点名的
观察口径，并提供不进菜单的临时隐藏。Highlight 的目标是「保留 A 路原图＋按增益对
超阈值区域红色 tint」；当时只做接线，正阈值下背景被提前清黑的问题由上方
2026-10-06 阈值背景补丁修正，原生渲染验收仍待完成。

- **「差异」按钮改为下拉**（复用图片工作区 C-02 的既有模式）：`CompareModeBar` 的
  `diffModeButton` 从单一模式按钮改为下拉，提供「纯差异图」（Difference + RgbAbsolute）
  与「原图叠加高亮」（Difference + Highlight）；激活时按钮文字带当前口径，其他指标
  （带符号／热力图等）保持普通「差异」标签并仍从检查器选择，阈值／增益沿用检查器的
  三路共享设置。选口径同时写 `preferences.viewMode` 与 `differenceMetric`。
- **按住看原图（hold-to-peek）**：`ComparisonSurface` 新增瞬态 `differenceSuppressed`
  属性（不持久化、不改布局，契约测试断言不触发 presentationGeometryChanged），经
  `SurfaceRenderState.differenceSuppressed` 进入渲染器；`appendDifference` 在抑制时改为
  按同一画布绘制当前配对第一路原始画面（同 aspect fit 与 letterbox），松开恢复差异。
  入口为差异模式下的「按住看原图」按钮（`onPressed`/`onReleased`/`onCanceled`）。
- **测试**：`DifferenceSuppressedDefaultsFalseAndNotifiesOnlyOnChange`（属性语义）、
  `DifferencePeekReplacesThePassWithTheRawFirstSource`（WARP 真实像素：32/224 灰阶对
  差异≈205、按住≈19、松开逐位还原）、`DifferenceButtonOffersFlavorsAndPeekTogglesSuppression`
  （契约：口径切换写偏好并反映按钮文案；按住/松开切换 surface 抑制态且模式与指标不变；
  离开差异模式隐藏按钮）。定向套件、format、lint 见本轮日志。
- **第二步余项**：带标注的对比图导出已在下一轮落地（见顶部记录）；仍待做的是固定
  GT 文件夹＋切换预测文件夹的对比实验流程。

## 2026-09-25 三源候选快捷切换（P1 · 实施第二步「做好比较」，基线 `220cf67` + 本轮工作区）

对应 2026-09-25 审查 §五与实施顺序第二步：验收标准「找到一次缺陷后，切对象不用重新定位」。
三联（ThreeUp）、参考聚焦（ReferenceFocus）、联动缩放／平移、ROI、高亮与热力图此前已在
基线（见「已有能力」），本轮补齐候选切换的操作面：此前换候选要打开「对比对」下拉两步选择，
且没有快捷键。

- **切候选（固定 GT）**：`Main.qml::switchCandidateEdge()` 在三源会话把当前配对切换到另一条
  参考（GT）锚定的边——只提交新的 `SetActiveComparisonPairCommand` 配对选择，当前帧、
  缩放／平移与分割线位置不动（这些状态本就不随 differenceEdge 变化，本轮用契约测试把该
  语义钉死）。从「双预测互比」（配对不含参考）进入切换时，保留当前主画面槽位并把 GT 带入，
  画面不跳。
- **入口**：`CompareModeBar` 在分割线／差异／分析网格模式（配对相关）且三源时显示「切候选」
  按钮（`switchCandidateButton`）；`ReviewShortcuts` 新增 `C` 快捷键（沿用
  `globalMediaShortcutsEnabled` 的焦点／工作区门控）；两个视频预设的帮助浮层均已列出。
- **测试**：新增 `SwitchingCandidateKeepsReferenceAnchoredPairAndObservation`（三源假会话、
  参考=槽位 1）：按钮与快捷键两条路径都断言配对在 Edge0And1 ↔ Edge1And2 之间翻转、
  `viewScale`／`wipePosition` 逐次保持不变、从 Edge0And2 进入时落到 Edge0And1，并核对提交的
  是 SetActiveComparisonPairCommand。定向 ui.MainQmlContractTests／player_osc／
  ImageWorkspaceManual、format-check、lint 见本轮日志。
- **第二步余项**：差异按钮三选项与按住看原图已在下一轮落地（见顶部记录）；仍待做的是
  带标注的对比图导出、固定 GT 文件夹＋切换预测文件夹的对比实验流程。

## 2026-09-25 图片内容区默认中性灰棋盘格（P1，基线 `04f26c3` + 本轮工作区）

对应 2026-09-25 审查 §二.2 与实施顺序第一步：为「正常 RGBA 偏蓝」排查先落地地面工作——
默认背景不再是有色深底，半透明内容的颜色判断不再被背景染色。**偏蓝根因仍未确认**：
`RgbaView` 直接返回原图；`ReviewImageProvider`、`Main.cpp` 三处 RGBA8888 转换与
`StillImageDecoder` 的 RGBA 包装本轮已静态核对，均无通道重排。深色底 `#090d14` 的合成
影响假设仍待用户提供问题原图与截图验证；「当前录屏的步进/缩放移动」复核也待真实素材。

- **默认背景 0（深色）→ 1（棋盘格）**：`ImageWorkspace.qml` 的 `backgroundMode` 默认值
  改为棋盘格；深色/黑底/白底保持显式可选，应用外框深色主题不变。观察状态浮标的
  「非默认」判定、一键复位目标与提示文案同步改为棋盘格（选深色底现在会点亮浮标）。
- **棋盘格双色改为真正的中性灰**：原 `#22262e`/`#383e4a` 保持亮度但带蓝灰倾向
  （B−R=+12/+18），改为同亮度的 `#272727`/`#404040`（B−R=0）。落实审查建议
  「中性灰棋盘格」，同时修正台账此前把蓝灰棋盘格记作「中性灰」的说法（I-01 已加注）。
- **测试**：`ImageWorkspaceAlphaAndBackgroundSelectionContract` 初始断言改为默认棋盘格；
  显式选择棋盘格不再点亮观察浮标（它就是默认值），选择深色底改为点亮、回到默认后熄灭；
  新增默认态证据截图钩子（`DVS_REVIEW_EVIDENCE_DIR` → `image-default-checkerboard.png`）。
  验证记录见本轮提交说明。

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
| V-07 | MAE/PSNR 能否实际用于视频评估？ | P2 | 独立解码服务、检查器读数、OSC 和指标泳道已接通；源对复用与阈值/通道口径已修复；2026-10-01 工作区改为当前帧先发布＋顺序窗口，合成 1080p60 CPU 评测实测提速；真实素材与硬件验收待做 | `PairMetrics.*`、`PairMetricsController`、`MetricTimelineLane.qml`；[窗口证据](pair-metrics-window-performance.md) |
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
| U-03 | 鼠标放在进度条上，播放控制条还是会淡出 | P1 | 已复现并修复（倒计时不看光标，主机每次走带都重新计票）；光标在控件上即不淡出，移开照旧淡出 | `PlayerOsc.qml`：`pointerInsidePanel`、`reveal`、`hideTimer`；`TimelineTracks.qml`：`pointerInside` |

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

- **基础定义**：`computeRgbAbsoluteMetrics`、`scoreActivePairRgbAbsolute` 提供 MAE/MSE/PSNR
  标量参考，见 [ADR 0005](../adr/0005-pixel-difference-metrics.md)；端到端集成与后续优化见下文。
- **2026-10-08 口径说明（#44 已合入；原基线 `880d02a`，Windows 完整流程待验）**：
  检查器指标标题下新增随列宽换行的说明：统计对象是解码转换后的全帧 RGBA8 RGB 值，
  忽略 Alpha、非原始文件码值；10-bit 输入也先降到 8-bit。ROI、视图缩放／平移、显示旋转
  与显示空间重采样、显示增益不改变统计，CPU 坏点数不保证等于屏幕高亮像素数；
  阈值／通道策略仅查询坏点分布，不改 MAE/MSE/PSNR/最大差。
  源码依据：`PairMetricsDecodeSession::rgbaFromFrame` 由源格式直接转同尺寸 full-range RGBA8，
  播放 `SoftwareDecoder` 则另走 NV12/P010；`PairMetricsService::scoreFrame` 比较等尺寸的
  未旋转整帧缓冲，不使用显示旋转或 SAR 做数值对齐，不把尺寸不匹配的缓冲重采样为可比。
  `PairMetricsRequest` 没有 ROI／视口 scale 输入；现有矩阵／范围处理不承诺 transfer／primaries
  统一或完整色彩管理。本轮仅修说明与相邻失实注释，不改公式、像素管线、缓存、请求或
  `metricsRows`／坏点占比约定；不新增 ROI、原生 RGB 或高位深指标功能。
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
- **2026-10-01 第一批工作区实现（基线 `4a67d1b`，未提交）**：
  1. 新增可选 `priorityFrame`；当前帧立即单样本发布，余下窗口以 suffix／prefix 两个升序区间
     处理，无优先帧则整段升序。GUI 显式传实际播放头或缺口内最近位置，不再用窗口中点冒充。
  2. 工作量计数覆盖实际 seek 与解码前滚，不再把“成功返回帧数”误称全部解码成本。
  3. 同日 Release 双源 H.264 1080p60、GOP=60、301 帧合成样例，1 轮预热＋5 轮测量：
     最终候选完整窗口中位 82737.80 → 7205.01 ms，首批 4356.27 → 346.80 ms；seek 600 → 4，
     实际解码帧 18240 → 662，仍为 301 个 canonical 样本，MAE 汇总一致。
  4. 定向 28 项通过；20/20 mutation 检出后恢复源字节，再跑全量开发测试：821 项选择，
     814 通过、3 原有跳过、4 原有禁用、零失败；format-check／lint 通过。
     [完整协议、数值回归与 mutation 对应证据](pair-metrics-window-performance.md)。
  5. 本批未做阈值缓存、区间 CLI 或异常区间导航；CPU 合成样例不替代真实素材或硬件门禁。
- **2026-10-01 第二批工作区实现（同一基线，未提交）**：
  1. 新增阈值无关 RGB 分析与三种策略的累计分布，保留独立 CPU 标量参考；请求／批次
     不再携带显示阈值。控制器改阈值／策略只查询并通知，在途分析与已缓存结果均可复用。
  2. 缓存以不可变素材／实际映射及对齐输入隔离；提交结果匹配完整当前分析请求身份。
     分析缓存硬限 4096 项、约 25 MiB，淘汰远端并重算范围／峰值，不清空当前读数。
  3. 最终同协议 Release 完整窗口中位 7205.01 → 7923.09 ms（首次分析成本 +9.97%）；
     全 301 帧 × 256 阈值 × 3 策略的 231168 次查询中位 0.8662 ms，seek／解码不增加。
     真实控制器组件确认阈值调整提交次数仍为 1、seek 4 → 4、解码 30 → 30。
  4. 定向 53 项；46/46 mutation 运行时检出；恢复后全量开发选择 834 项，
     827 通过、3 原有跳过、4 原有禁用、零失败，format-check／lint 通过。
     [复用边界、首次成本和逐组反例证据](pair-metrics-threshold-reuse.md)。
  5. 仍未实现区间 CLI／异常区间导航，未验收真实素材、覆盖率、D3D11VA 性能或 release ZIP。
- **退出条件**：同图无限 PSNR、缺帧／错误尺寸 unavailable；不可用项不当作零误差平均；
  换 pair／seek 后陈旧结果被丢弃；显示增益不影响原统计值。组件测试已覆盖以上语义；
  真实素材下的数值对拍与性能（1080p60 窗口采样耗时）待硬件验收。
- **验证入口**：`PairMetricsServiceTests.cpp`（15 项）、`PairMetricsControllerTests.cpp`（13 项）、
  `PairMetricsPerformanceTests.cpp`（显式素材、独立 performance 标签），以及既有
  `PixelDifferenceTests.cpp`、`ComparisonMetricsTests.cpp`。

## 图片：证据与退出条件

### I-01 Alpha 工作流

- **证据**：早期基线只有 RGBA 取样、RGB 差异和 Alpha-only 提示；后续已加入 Alpha 灰度、
  忽略透明度 RGB、Alpha 差异、峰值／均值／变化像素和黑白／棋盘背景。
- **已实现**：
  1. 交互增强：`A` 与 `O` 分别切换 Alpha 灰度和忽略透明度 RGB；背景由下拉菜单直接选择深色、棋盘格、黑底或白底；按 2026-09-23 反馈移除循环背景按钮和 `B` 循环键。保留“α 直通（未预乘）”徽标；
  2. 棋盘格对比度升级：将画布背景 Canvas 替换为专业中性灰阶双色网格（`#22262e` / `#383e4a`），大幅提升半透明边界与镂空细节可辨识度（2026-09-25 注：该双色实为蓝灰倾向，已改为同亮度真中性灰 `#272727`/`#404040` 并成为默认背景，见顶部记录）；
  3. 观察态状态浮标（`imageAlphaObservationBadge`）：在激活非默认观察通道或背景时，于视口顶部实时浮现状态与快捷还原提示，支持点击一键复位；
  4. 快捷键帮助覆盖层：`ShortcutHelpOverlay` 深度整合图片工作区预设，展示图像与透明度检查全套快捷键；
  5. 2026-09-26 界面整理：视口身份牌副行直接显示「α」标记与数值审查路径，「α 直通（未预乘）」徽标保留在状态条，背景/通道选择保留在 B 行下拉。
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
- **2026-09-26 界面整理**：来源尺寸/格式/α/数值审查路径以视口身份牌副行常驻呈现
  （「A · 96×64 · RGBA8 · α · 数值审查路径：原码值」），悬停显示完整路径。
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
- **2026-09-26 界面整理**：「适应窗口 / 100% / 重置视图」在命令区 A 行成组并列，960px 最小宽度下随 Flow 自然换行，缩放读数常驻状态条左侧。
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
  视频侧三源模型（ThreeUp/ReferenceFocus/DifferenceEdge）独立保留（2026-09-25 增加「切候选」
  按钮与 `C` 快捷键，固定参考切换候选且保留观察位置，见顶部记录）。

### C-02 插帧审查与切换模式

- **已有**：视频局部放大、平移、ROI、区间循环、高亮及问题记录，不应重新当作缺失能力实现。
- **2026-09-23 调整**：
  1. 图片单图对比改为手动闪烁：默认 A，点击画面、按 `Space` 或 `T` 在 A/B 间切换；不自动计时交替。顶部 HUD 显示当前源并限制宽度；
  2. 差异模式收为显示当前选项的下拉菜单；淡化入口隐藏。原淡化按钮无效的直接原因是控制器拒绝模式值 `7`（2026-09-25 已修复并恢复入口，点击链路经契约测试验证，见顶部记录）；
  3. 并排模式跨图同步十字准星（Hover 时镜像侧精准投影目标瞄准环、辅助十字线与图像像素坐标，彻底消除并排观察微小伪影时的视线寻找负担）；
  4. 2026-09-26 界面整理：模式芯片迁入命令区 B 行 Flow（960px 自然换行）；分割线左右身份由裸文字升级为半透明浮板；淡化新增「A · x ↔ B · y」身份浮板——各对比模式身份呈现统一（见顶部记录）。
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
  4. 2026-09-26 界面整理：两入口保留在 B 行模式区（「对调 A/B」「换图…▾」）；视口身份牌
     以「A · 文件名」「B · 文件名」呈现当前槽位方向，对调后身份即时可读。
- **边界**：对调是会话内方向，不改写文件夹配对身份。单侧替换后画布可能与文件夹行路径不一致，
  这是有意保留的 T1 语义；文件夹导航仍按行配对。
- **本轮验证**：`ui.ImageReviewControllerTests` 39/39（含 6 项新用例）、
  `ui.MainQmlContractTests` 23/23 通过；`format-check`、`lint` 通过。实窗验收待做。
- **验证入口**：`ImageReviewControllerTests` 的 `SwapSides*`、`Replace*`、`FailedSideReplace*`。

### U-03 鼠标放在进度条上，控制条仍会淡出

- **现象与复现**：悬浮式播放控制条（`controllerState === 1`）下，把光标停在进度条上，
  约 1.2 秒后整块面板淡出，光标下面直接空了。
- **根因**：不是「悬停被进度条子控件抢走」。真正的链路是
  `ComparisonViewport.viewportNavigation.onPositionChanged` → `oscRevealRequested`
  → `Main.revealOsc()` → `PlayerOsc.reveal()`，而旧 `reveal()` 无条件
  `hideTimer.restart()`。拖动进度条或在其上滚轮都会触发走带 → 重新计票 → 光标还在
  进度条上就被淡出。沉浸态（Tab/H）另有 `immersiveOscTimer` 独立倒计时，同样不看光标。
- **修复**：`PlayerOsc.qml`
  - `pointerInsidePanel = panelHover.hovered || tracks.pointerInside`：光标在面板上或时间轴上即真；
  - `reveal()` 只在光标**不在**控件上时才 `hideTimer.restart()`，否则 `stop()`；
  - `hideTimer.onTriggered` 增加 `&& !pointerInsidePanel`，倒计时只能淡出光标已离开的控制条；
  - `onPointerInsidePanelChanged` 作为唯一复位点，移出光标即重新计票，
    拖拽释放到面板外也能再次隐藏（不会卡在常亮）。
  - `TimelineTracks.qml` 新增 `pointerInside = timelineMouse.containsMouse || dragging`：
    拖拽把光标带出面板时 Qt 会撤销鼠标区 hover，但 grab 仍在。
  - `Main.qml` 沉浸计时器改为光标在控件上就顺延重检。
- **测试与变异证据**：`ui.player_osc` 12/12（新增悬停保持、拖拽释放后恢复淡出两条），
  `ui.timeline_tracks` 5/5（新增 `pointerInside` 拖拽语义）。旧自动隐藏测试反证光标门未被关死。
  变异 3/3 单点失效：`reveal()` 恒 restart、`pointerInside` 去掉 `dragging`、
  `onPointerInsidePanelChanged` 去掉 else。
- **门禁**：`ui.MainQmlContractTests` 48/48（CTest 逐例注册）；dev `format-check` 与 `lint` 通过。
  注意整二进制一次跑 48 例会出现**与本改动无关的跨用例偶发失败**，按 CTest 注册方式执行。
- **边界**：未覆盖真实硬件光标行为（合成事件与平台窗口 hover 传递有差异，仓库已有测试注释此点）；
  未改 `wakeArea`/`immersiveWakeStrip` 的唤醒灵敏度。

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
