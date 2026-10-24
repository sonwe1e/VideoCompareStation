# 视频任务标签实施设计

状态：已评审定稿（lead 已拍板 §11 全项，决议见 §11 末尾「决议记录」）

- 范围：把图片 D-022「一次审查任务一个标签」扩展到视频工作区。只读调查基于当前 HEAD；文中行号会漂移，以符号名为准。
- 产出物：本设计文档。不含任何代码改动。
- 关联决策：D-022（图片标签现行决策，`docs/engineering/decision-register.md:45`）、D-014（解码帧缓存不分档、播放池 224 MiB，`docs/engineering/decision-register.md:37`）。
- 验收视角：文档需可评审、可拆片实施；每条用户提案要么落入章节，要么列入 §11 待拍板。

## 0. 背景与两张镜像的架构差异

图片侧现状（D-022 已实现）：`ImageTaskHost`（`src/ui_qml/include/dvs/ui/ImageTaskHost.h:22`）每任务持有一个独立 `ImageReviewController`，切标签只换活跃控制器，解码缓冲、差异缓存、观察状态全部随任务驻留。标签条 `ImageTaskTabs.qml` 纯展示；打开/切换/关闭路由在 `Main.qml`（`openImageInNewTab` :2703、`adoptActiveImageTab` :2732、`activateImageTab` :2749、`closeImageTab` :2758、`cycleImageTab` :2774、`closeCurrentTask` 图片分支 :1506-1556；Ctrl+Tab 接线 :3823-3837，Ctrl+W :3815-3821）。上限 12（`ImageTaskHost.cpp:15`），重复素材聚焦已有标签（`openImages` :259-318），关闭活动标签选相邻（`removeTask` :579-585）。

视频侧现状：整个应用只有一个解码/渲染运行时 `ReviewRuntime`（`src/app/ReviewRuntime.h:35`，组合根单实例），GUI 投影是单个 `ReviewController`，打开意图经 `ReviewShellController` 串行队列（`src/ui_qml/include/dvs/ui/ReviewShellController.h:21`）。换素材组 = `openSources`/`reopenSources` 重建会话（`ReviewController.h:280-284`）；「当前只有一个会话」是运行时事实，不是 UI 选择。

因此视频标签不能复制图片的「每标签一个 controller」：**视频标签 = 同一运行时上的任务记录集合 + 重建式切换**。核心差异对照：

| 维度 | 图片（D-022 现状） | 视频（本设计） |
|---|---|---|
| 每任务解码状态 | 独立 controller 驻留 | 单运行时，切任务即重建（reopen） |
| 后台任务成本 | 像素与差异缓存驻留（有工作集预算） | 纯数据任务记录，零解码资源 |
| 切换代价 | 立即（换绑定） | 重开定位（冷 seek，见 §7） |
| 身份匹配 | URL 列表有序、大小写不敏感（`ImageTaskHost.cpp:45-55`） | 素材组 + 打开配置，见 §2 |
| 状态恢复 | 无需恢复（状态一直在） | 分阶段恢复协议，见 §4 |

## 1. 架构决策：VideoTaskHost 放哪层

**推荐：C++ `ui_qml` 新类 `VideoTaskHost`（镜像 `ImageTaskHost` 的对外形状）负责任务列表、身份匹配、打开路由判定、关闭选相邻与 tabs 模型；每标签的视图态以纯数据（`QVariantMap` 快照包）存在任务记录里但不被 C++ 解释；快照采集与恢复编排在 `Main.qml`，复用问题记录恢复的分阶段状态机模式。**

理由（不留「两者皆可」）：

1. **可测性落在最难的地方**。身份匹配（AB 与 AC 不得合并）、路由判定（§5 的每一条）、关闭选相邻，是纯逻辑、纯数据，正是 `ImageTaskHostTests` 6/6 与 `MainQmlContractTests.ImageTabs*`（`tests/component/ui/MainQmlContractTests.cpp:16401`、:16493）已经覆盖过并证明值得单测的层。放 QML/JS 则这些规则散进已 5700 行的 `Main.qml`，只能靠窗口级契约测试兜底，且无法在宿主单测里穷举反例。
2. **异步终态本来就在 C++**。任务切换协议（§4）要绑定 `sessionEpoch`（`ReviewController.h:135`）、`commandFinished`/`foregroundCommandFinished`（:383-387）与 shell 的 `intentFinished`（`ReviewShellController.h:232`）。`VideoTaskHost` 与 `ReviewShellController`、`ReviewController` 同层同线程，关联 intentId↔任务记录不需要跨 QML 桥；QML 方案要在 JS 里维护这张映射，正是仓库反复修过的「迟到结果串任务」类缺陷（见 §4 身份门）。
3. **视图态留在视图层**。缩放/平移/ROI 在 `ComparisonSurface`（`src/ui_qml/include/dvs/ui/ComparisonSurface.h:85-89`、`restoreViewport` :301-308），wipe 位置、差异阈值开关在 `Main.qml` 属性（`resetReviewVisualState` :1116-1130 枚举了它们），显示模式/差异参数在 `ReviewPreferencesController`（`ReviewPreferencesController.h:26-39`）。C++ 不应触碰像素与视口；因此 host 只存取快照包，QML 负责采集（读哪里）与重放（调什么），对照 §3。
4. **与图片宿主同构降低实现与评审成本**：Q_PROPERTY（count/activeIndex/tabs）、Q_INVOKABLE（openTask/activate/closeAt/closeActive/cycle）、信号（taskOpened/openFailed/allTasksClosed/activeChanged）与 `ImageTaskHost.h:24-81` 一一对应，`ImageTaskTabs.qml` 可参数化复用（§6）。

明确不做的变体：不为每标签建 `ReviewController`（违反单运行时组合根，`ReviewRuntime.h:32-35`）；不把任务列表放 QML/JS 单例（理由 1、2）；不在 `src/app` 层加任务概念（任务/标签是 UI 会话语义，架构上归 `ui_qml`，`src/app` 只组合）。

## 2. 任务身份 key 与匹配流程

### 2.1 key 的精确定义

一个视频任务的 key 由三部分组成：

1. **素材组**：按打开槽位顺序的 URL 身份列表 `[id(A), id(B), id(C)]`。
2. **参考槽身份**：GT 候选的 URL 身份（打开时即 `referenceSourceIndex` 指向的那一路）。
3. **任务形态**：单视频（组大小 1）或比较（组大小 2–3）。形态由组大小自然区分，不单独编码。

URL 身份 `id(u)` 的取法分两阶段：

- **匹配阶段（打开前）**：规范化本地路径 = `QFileInfo(url.toLocalFile()).absoluteFilePath()`，比较时大小写不敏感——与 `ImageTaskHost.cpp:29-55` 的 `identityForUrl`/`sameIdentityList` 完全一致。这覆盖冗余路径段、大小写差异、`file:///` 拼写差异；非本地文件退回 `url.toString()`。`ReviewController::handleDroppedUrls`（`ReviewController.h:296`）已是仓库统一的本地文件规范化验证器（`Main.qml:3198-3211` 的注释确认它是唯一 canonicalizer），打开路由一律先过它。
- **记录阶段（打开成功后）**：任务记录内升级保存 `ReviewController::frozenSourceIdentity`（`ReviewController.h:355-357`，path|size|mtime，快照冻结）。它用于后续「文件在会话期间被改动」的检测与 GT 身份追踪（`ReviewShellController` 的 C2/V02 状态全部按 frozen identity 键控，`ReviewShellController.h:53-66`），**不用**于跨任务匹配——匹配时拿不到对方的冻结身份，且文件被替换后路径身份仍应命中同一标签（重开按当前文件内容失败则点名报错，见 §4.6）。

匹配规则（`openTask` 判定「聚焦已有」时逐任务比对）：

- **单视频任务**：组大小 1 且 `id` 相等（大小写不敏感）→ 同一任务。
- **比较任务**：组大小相等（2 或 3）且**组的身份多重集相等**（顺序无关）且**参考槽身份相等**。顺序无关的理由：`A↔B` 与 `B↔A`（同一 GT）是同一审查任务的不同槽位排列；命中后按任务记录中保存的槽位顺序重开，不采用新打开的顺序。若产品希望严格有序匹配，见 §11-e。
- **交叉不合并的保证**：AB = {A,B} 与 AC = {A,C} 多重集不等 → 不同任务，各自标签；A 单视频（大小 1）与 AB（大小 2）形态不同 → 不同任务。这直接满足「同一个 A 出现在 AB 与 AC 中不得合并」。
- **三源任务内部切比较对**（AB/AC/BC，经 `applyComparisonPairFromEdge`，`ReviewController.h:350`）只改每标签状态里的「当前比较对」，组不变 → key 不变，永不拆标签。「复制当前任务到新标签」是未来增项，不在本设计内（仅在 §11-h 预留）。

### 2.2 重开同任务 = 聚焦已有标签的匹配流程

`VideoTaskHost::openTask(urls, referenceIndex, purpose)` 流程：

1. 规范化 urls（调用方已过 `handleDroppedUrls`）；构造待匹配 key。
2. 遍历任务记录比对（§2.1 规则）。命中：
   - 若该任务 loading/restoring 中 → 置 `activateWhenLoaded`，返回该索引（镜像 `ImageTaskHost.cpp:273-279`）；
   - 否则 `activate(index)` 聚焦，**任务记录中的进度与状态快照原样保留**；若该任务已是当前运行时会话（组与会话事实一致），只做 §4.5 的差量应用，不重开。
3. 未命中：检查上限（对齐图片 12，§11-b）；创建任务记录（loading 态，标题「正在打开…」），按 §5 的路由把打开意图交给 `ReviewShellController` 既有入口（`openStagedSources`/`openVideo`/`openSidebarAppend`），并在 `intentFinished(OpenSourcesIntent)` 成功终态把会话事实（源列表、frozen identities、参考身份）回写任务记录——回写点即 `Main.qml:3915-3925` 现有 commit 的扩展。
4. 打开失败：移除新建任务记录、`openFailed(error)`（镜像 `ImageTaskHost.cpp:310-315`）；若失败发生在「聚焦已有」路径，原任务不动。

## 3. 每标签独立状态清单

快照在「离开标签」时刻采集（§4.2），存入任务记录的 `state` 包；恢复按 §4.5 逐项重放。逐项写「读哪里、恢复调什么」：

| # | 状态 | 读哪里（快照） | 恢复调什么 |
|---|---|---|---|
| 1 | 素材组（槽位顺序） | `shell.activeSources`（`Main.qml:3105-3107`）/ `controller.sourceUrls` | `reopenSources(urls, refSlot)`（`ReviewController.h:284`） |
| 2 | GT / 参考源 | `controller.referenceSourceIndex` + `hasExplicitReference`（`ReviewController.h:42-46`） | 同上第二参；仅 GT 变化时 `changeReference`（:287，保位置语义） |
| 3 | 时间轴主源（canonical） | `controller.canonicalSourceIndex`（:41） | 由重开命令内部决定（现行实现第二参即参考槽，`ReviewController.h:288-289`）；快照仅记录用于位置换算与防御校验 |
| 4 | 对齐方式与偏移 | `controller.alignmentMode`（:124）；`Main.qml` 的 `sourceOffsetValues`（:1117） | `setAlignmentMode`（:320）+ `applySourceOffsets`（:311）。手动锚点/自动对齐分析结果**不跨标签保存**（首版，§11-g） |
| 5 | 播放位置 | `controller.currentMediaTime`/`currentFrame`（:88, :91） | 重开后 `seekFrame(frame)`（§4.5 阶段 3；防御同 `attemptResume` `Main.qml:1265-1273`：帧号 ≥ totalFrames 时放弃定位并提示） |
| 6 | 倍率 | `controller.playbackRate`（:75） | `setPlaybackRate`（:325，暂停态可设，下次 play 生效） |
| 7 | 入出点、循环、往返 | `shell.inFrame/outFrame/rangePlaybackActive`（`ReviewShellController.h:47-52`）与 `controller.playbackRangeIn/Out/Loop/PingPong`（`ReviewController.h:81-86`） | `shell.setRangeIn/setRangeOut/setRangePlaybackState`（`Main.qml:2027-2032` 同款链）+ `controller.setPlaybackRange`（:340）/ `togglePlaybackRangePingPong`（:343）；注意 shell 与 controller 是两份镜像，恢复顺序先 shell 后命令 |
| 8 | 当前比较对 | `preferences.differenceEdge`（`ReviewPreferencesController.h:31`）/ `controller.effectiveDifferenceEdge` | `applyComparisonPairFromEdge(edge)`（`ReviewController.h:350`，是命令，需等终态，§4.5 阶段 2） |
| 9 | 显示模式 | `preferences.viewMode`（:26） | 赋值 `preferences.viewMode`（属性赋值即可；D07 语义：显式设置后开放置，勿在会话事件里重放——`Main.qml:2260-2271` 的先例） |
| 10 | 缩放、平移、ROI | `viewportFrame.surface` 的 zoom/center/roi 族（`ComparisonSurface.h:85-89`） | `surface.restoreViewport(centerX, centerY, zoom, roiEnabled, l, t, r, b)`（:301-308；问题恢复已用，`Main.qml:2298-2299`） |
| 11 | 分割位置与方向 | `Main.qml` 的 `wipePosition`（:1122）+ 视口分割方向 | `viewportFrame.setWipeOrientation(o)` + `wipePosition = p`（`Main.qml:2307-2320` 同款） |
| 12 | 差异参数 | `Main.qml` 的 `differenceThresholdEnabled/Code`（:1124-1126）；`preferences.differenceMetric/Gain/Filter`（`ReviewPreferencesController.h:27-34`）；`pairMetrics.threshold/thresholdPolicy`（`PairMetricsController.h:46-48`） | 对应属性赋值/invokable；阈值开关是 QML 观察态，恢复为直接赋值 |
| 13 | GT 直看（canvasFocus） | `shell.canvasFocusIdentity`（`ReviewShellController.h:65`） | `shell.requestCanvasFocus`（:226）；瞬态 peek（`differencePeekActive`）不保存 |
| 14 | 文件夹导航位置 | `videoFolderModel.folderUrl` + `currentRow`（`VideoFolderModel.cpp:169-191`） | `loadFolder(folder)` 并回显行高亮，不触发打开；按标签还是全局见 §11-c |
| 15 | 恢复元数据 | 任务记录自身：loading/restoring 标志、快照帧、frozen identities | 内部字段，不进 QML |

不进每标签状态的项（显式排除，评审可推翻）：`chromeVisible`/`inspectorVisible`（窗口级，`ReviewShellController.h:41-43`）、`oscMode`、快捷键 preset、播放连续性策略（C-07，全局偏好）、源显示顺序与选中（C2，会话内 shell 状态，随任务记录的组重建后自然回到默认——若要随标签保存列入 §11 追加）。C1 单视频播放位置记忆（`resumeFrameFor`/`rememberResumeFrame`，`ReviewPreferencesController.h:103-109`；触发点 `Main.qml:760-774`）是跨重启的按源全局记忆，与任务标签正交，维持现状不复制进快照。

## 4. 标签切换协议（暂停 → 快照 → 重建 → 分阶段恢复）

以「点击另一个标签」为代表时序；打开新任务复用同一协议的阶段 3-5。所有阶段绑一个恢复上下文 `{taskId, sessionEpochAtReopen, urls, pairCommandId, seekCommandId}`，15 秒总deadline（对齐 `issueRestoreDeadline`，`Main.qml:2356-2362`；阈值待拍板 §11-a）。

### 4.1 阶段 0：暂停

`ensureVideoPaused()`（`Main.qml:1287-1292`）——现有函数，提交 `pause()` 不等终态（命令本身带会话身份，乱序安全）。离开视频工作区去图片也走它（`activateWorkspace` :1463-1464 已如此）。

### 4.2 阶段 1：快照离开标签

QML 采集 §3 表全部字段存入 `host.updateTaskState(index, snapshot)`（新 invokable）；C++ 侧同时记录该任务的 frozen identities。快照必须发生在任何重开命令提交之前。播放位置取 `currentMediaTime` + `currentFrame` 双记（帧号用于 seek，MediaTime 用于日志与防御）。

### 4.3 阶段 2：激活目标任务并判定是否需要重开

- **同组判定**：目标任务的组与会话事实一致（含槽位顺序与参考身份）——典型即「ABC 标签内切比较对后切去又切回、且期间无其他任务占用运行时」：跳过重开，直接进入阶段 5 差量应用。这保证「切 ABC 内比较对复用已打开三源、即时完成」。
- **需要重开**：调 `reopenSources(urls, refSlot)`（= `openSources(..., preserveDisplayedTime=true, ReplaceSources)`，`ReviewController.cpp:2879-2882`）。**关键细节：跨任务切换必须忽略 `preserveDisplayedTime` 映射的便利**——preserve=true 映射的是「当前（上一个任务）时间线」的位置，不是目标任务的位置。因此切换路径使用 preserve=false 语义重开（回帧 0），再在阶段 3 显式 `seekFrame(快照帧)`；只有**同任务内拓扑变化**（「加入当前视频对比」、`changeReference`、槽内替换 `performSourceReplacement` `Main.qml:3213-3245`）继续用 preserve=true 保持当前显示位置。若实现中发现需要新增 `reopenWithoutMapping` 入口，属于 `ReviewController` 的小改（加一个 invokable 包装，`intent=ReplaceSources, preserve=false`），不影响协议形状。
- 提交后等待：`intentFinished(OpenSourcesIntent)` 成功终态（`Main.qml:3902-3925` 现有连接即事件源），且 `controller.sessionEpoch` 已前进、`sourceCount` 与 URL 匹配（`matchesOpenUrls` 式比对，`Main.qml:2239-2242` 先例）。

### 4.4 阶段 3：跳帧并等待呈现

`seekFrame(snapshotFrame)`（不等价于赋值：它是命令，`ReviewController.cpp:778-791`）；记录 `lastSubmittedCommandId`（`ReviewController.h:277`），在 `commandFinished` 匹配终态且 `currentFrame == snapshotFrame`、`!framePending` 后前进（完全复用 `advanceIssueRestore` 的等待骨架，`Main.qml:2276-2296`）。快照帧为 0 或已就位则跳过。**保持暂停**：协议全程不调 `play()`；呈现的是暂停帧，空格继续。

### 4.5 阶段 4-5：应用比较对/模式，再恢复视口

顺序照抄问题恢复（`Main.qml:2252-2329`，其顺序经过变异验证，台账 2026-10-09 修复 1）：

1. 比较对一次：`applyComparisonPairFromEdge(edge)`，记 commandId 等终态，并校验 `effectiveDifferenceEdge` 与期望一致（`Main.qml:2341-2343` 同款）；失败计「部分恢复」，不阻塞后续。
2. `preferences.viewMode = mode`（比较对可携带模式，记录值最后落）。
3. `surface.restoreViewport(...)`、wipe 方向/位置、差异阈值族、范围族（§3 #7/#10-#12）、`requestCanvasFocus`、倍率。
4. 结束：清恢复上下文；期间任何手动导航/另一次任务切换 → 取消恢复（`Main.qml:2245-2250` 的 epoch+URL 双门先例），不回滚已应用的展示态。

### 4.6 身份门、恢复中状态句与失败处理

- **迟到画面/统计不串任务**：每个等待只接受「当前恢复上下文的 sessionEpoch + 匹配 URLs」的终态；`PairMetricsController` 已按 Scope（SessionId+Epoch+源+对齐修订+comparison 指针）丢弃过期批次（`PairMetricsController.h:26-29`、:149-166），重开换 epoch 后旧统计天然失效，无需为标签额外处理——但契约测试要证一次（§9 反例 R1）。QML 侧 `onVideoSessionEpochChanged` 已清 `differencePeek*`（`Main.qml:388-392`），切标签自动解除直看，符合预期。
- **恢复中状态句**：任务 restoring 时标签标题显示「正在恢复…」，画布上方短暂提示（intentMessage/HUD）；定位完成即消。
- **恢复失败分级**：
  - 重开失败（文件缺失/不可读）：`ReplaceSources` 语义保证旧会话（上一个任务）不被拆除（`Main.qml:3168-3171` 注释的现行契约）。宿主回退激活原任务标签，报错**点名失败文件**（errorKey 经 `messageCatalog` + 源名，`Main.qml:3895-3900` 同源）。原任务画面与位置不动——满足「不得悄悄清空或换成另一素材」。
  - 定位/比较对失败：会话已是目标任务，保留目标任务与已恢复部分，报「部分恢复：…」（措辞先例 `Main.qml:2326`），位置回落到重开后的帧 0/当前帧。
  - 超时：放弃剩余阶段并明示（`Main.qml:2361` 同款），不清任务。

### 4.7 与自动播放的边界

三条打开路径保留既有自动播放语义，均不经过 §4 的恢复协议：文件夹侧栏点选/上一项下一项换片后的 `controller.play()`（`VideoFolderModel.cpp:578-597`）；启动双击的 `OpenForPlayback`（`ReviewShellController.cpp:507-523`、:908-913，受 `autoplayOnOpen` 偏好）。**切回已有标签 = 恢复 = 永远暂停落位**，空格继续；「首次从文件夹打开成功后的自动播放」因此只在「换片/新开」时发生，切回不播。

## 5. 打开规则路由表

提案每行 → 具体改动点（文件:行 为当前 HEAD 锚点；「改」均指未来实施，本设计不改代码）：

| # | 提案行为 | 现状入口 | 改动点 |
|---|---|---|---|
| 1 | 文件菜单/合并「打开文件」选单个视频 → 新建并激活单视频标签 | `requestOpenMediaFiles`（`Main.qml:1780`）→ `openMediaFilesSelection`（:1786）视频分支 `openNewReviewUrls(files)`（:1810-1814）→ `reviewUrls(urls,false)`（:2890）→ `requestDestructiveAction(openVideos)`（:2993）→ `performVideoReview`（:1928）单视频分支 `shell.openStagedSources(false)`（:1935） | `performVideoReview` 单视频分支改为 `host.openTask([url], 0, Review)`，由 host 决定新建/聚焦（§2.2）；shell 提交链不变 |
| 2 | 一次打开 2–3 个视频比较 → 新建一个比较标签 | 同上多视频分支：`openComparison` 对话框选参考（:1962）→ `openDroppedComparison`（:3074）→ `shell.openStagedSources(preserve)`（:3082） | `openDroppedComparison` 改为 `host.openTask(urls, refIdx, Review)`；参考选择对话框保留（它决定 key 的参考槽身份） |
| 3 | 「加入当前视频对比」→ 添加到当前标签任务（≤3 源） | `requestAddVideo`（:1824）→ `openAddVideo`（`ReviewInputDialogs.qml:46`）→ `addVideoAccepted` → `reviewUrls([url], true)`（:2951-2988 append 分支）→ `stageSidebarAppend`/`setDroppedVideoOrder(existing)` + `openComparison` 确认 → `openSidebarAppend`/`openStagedSources(true)`（:3082） | 保持整条链；成功终态后把当前任务记录的组扩为 3 源（key 随之更新，AB→ABC 是**同一标签任务的演化**，不新建标签）；preserve=true 保持位置（§4.3） |
| 4 | 再次打开同一个任务 → 聚焦已有标签并保留进度 | 无对应（现状总是重开替换唯一会话） | `host.openTask` 匹配（§2.2）；行 1/2/5/6/7 的入口全部经此收敛 |
| 5 | 文件夹侧栏点选 → 在当前单视频标签内换片（不堆积标签） | `openFolderVideo`（:1631）/`openSidebarVideo`（:1721）→ `videoFolderModel.openAt/openFile`（`VideoFolderModel.cpp:470-492`）→ `shell.openVideo`（`ReviewShellController.cpp:664-672`） | 路由进 host：当前活动任务形态=单视频 → **就地换片**（该任务记录的组替换为新 URL，视图态按换片重置——`resetReviewVisualState` :1116 语义，位置不恢复），成功后保留自动播放（§4.7）；不是单视频形态（比较任务或无任务）→ 行 6 规则 |
| 6 | 当前是比较标签却从侧栏打开单视频 → 新开单视频标签、保留原比较任务 | 现状 `shell.openVideo` 直接替换唯一会话（原任务丢失） | host 路由：新任务记录 + 重开；原比较任务记录保留为后台标签 |
| 7 | 侧栏右键「在新标签打开」→ 明确新建 | `VideoFolderSidebar.qml:69`「打开为新单视频任务」→ `contextOpenRequested` → `openSidebarVideo`（:1721） | 文案已符合；`openSidebarVideo` 拆出 `openSidebarVideoInNewTab` 变体（或加参数），右键走强制新建，左键点选走行 5/6 路由 |
| 8 | 上一项/下一项 → 同行 5（单视频标签内换片） | `stepFolderVideo`（:1762）→ `openFolderVideo` | 随行 5 实现，无额外改动 |
| 9 | 拖入（含双击资源管理器启动） | `reviewDroppedUrls`（:3042）→ `reviewUrls(urls,true)`；启动参数 `enqueueStartupRequest`（`Main.cpp:405-418`，kind1=PlaySingle 单视频自动播放、Compare 2-3 路，`ReviewShellController.cpp:507-523`） | 拖入多视频/单视频非 append 场景 → 行 1/2 规则；拖单视频且当前任务可 append（≤2 源）→ 行 3 规则（现有 :2951-2988 判定保留）；启动打开 → host.openTask，PlaySingle 的 purpose 原样传给 shell intent（自动播放语义不变） |

统一原则：**所有打开入口收敛到 `host.openTask(urls, refIdx, purpose)`，host 决定「新建 / 聚焦 / 就地演化」三选一，再调用既有 shell 入口提交**。shell 队列、`handleDroppedUrls` 验证、destructive-action 确认弹窗（`requestDestructiveAction` :1139）全部不动。

## 6. 标签标题、徽标与关闭/切换边缘情况

- **标题规则**（镜像 `ImageTaskHost::titleFor` `ImageTaskHost.cpp:516-532`）：加载中「正在打开…」；恢复中「正在恢复…」；单视频＝文件名（`片段 D.mp4`）；两源＝`A.mp4 ↔ B.mp4`；三源＝`A.mp4 + 2`（主源文件名 + 剩余源数，避免三条长名）。detail（tooltip）＝逐行完整路径。三源任务内切比较对不改标题。
- **徽标**：源数徽标（2/3）标示比较任务；后台标签无「解码中」徽标（后台不解码，§7）；图片侧的 dirty 点在视频侧无对应（无编辑会话）。
- **标签条归属**：视频标签条放视频工作区内部（viewport 区上方），与图片标签条在 `ImageWorkspace` 内（`Main.qml:4721-4723`）对称；`ImageTaskTabs.qml` 参数化复用（tabsModel/activeIndex 信号已通用，改 objectName 与文案）。图片工作区激活时视频标签条随工作区整体不可见——两套标签不同栏（§11-d）。
- **关闭活动标签选相邻**：与图片同款 `removeTask` 语义（`ImageTaskHost.cpp:579-585`：优先右侧，末尾取左侧）；随后对该标签走 §4 恢复协议（若相邻任务与会话同组则差量应用）。
- **最后一个视频标签关闭**：经 `closeSources`（`ReviewController.cpp:720-738`）回到空态；若图片工作区有已提交任务则 `showWorkspace(imageMedia)`（镜像 `handleAllImageTasksClosed` `Main.qml:2828-2844` 的反向路径），否则留在视频空态（EmptyReviewView）。
- **素材右键「关闭此视频」**：现状 `removeSelectedSource`/`removeActiveSourceByIdentity`（`Main.qml:3109-3116`、`ReviewShellController.h:192`）移除任务中的一路。与「关闭当前任务」（Ctrl+W）的区分：文案保持「移除此视频/关闭此任务」两级；组 3→2 标题徽标更新；组 2→1 时任务演化为单视频形态（身份迁移见 §11-f）。
- **打开中的标签**：loading 期间禁用切换/关闭（镜像 `imageTabOpenPending` 门，`Main.qml:2752-2764`），避免把终态应用到错误任务（`Main.qml:164-166` 注释的同款竞态）。

## 7. 资源边界与缓存预算

- **仅活动标签解码**由单运行时天然满足：`ReviewRuntime` 只有一套解码/渲染/预取/分析管线，后台标签没有任何解码对象。这是本架构相对图片标签的核心收益（图片后台任务驻留像素，需要 1.5 GiB 工作集硬顶；视频后台任务成本 ≈ 一份 `QVariantMap`）。
- **代价 = 重开定位**：切回后台任务必经 reopen + 冷 seek（阶段 2-3）。已在 §4 用恢复状态句与超时兜底；性能门禁单列（§9）。
- **缓存预算**：解码帧缓存维持应用级 224 MiB（D-014，`ReviewRuntime.cpp:52` `kPlaybackFrameBudgetBytes`），按源分片逻辑不变——**不为每标签复制播放器与缓存，也不需要新预算字段**。标签数量不放大解码内存（后台不占）；唯一随标签数增长的是任务记录与 tabs 模型（可忽略）。差异统计暂存（PairMetrics samples，约 25 MiB 上限，`PairMetricsController.h:58-60`）同样只属于当前会话，切任务即弃，重开重采样——这是恢复延迟的一部分，写入 §10 风险。
- **与 D-022 工作集预算的关系**：图片侧 `withinBudget`（`ImageTaskHost.cpp:599-605`）在视频侧无对应物；若评审要求视频侧也有「上限拦截」，那就是标签数上限（§11-b）而非字节预算。

## 8. 快捷键一致性

- **Ctrl+Tab / Ctrl+Shift+Tab**：现仅图片（`Main.qml:3823-3837`，序列常量 `ShortcutCatalog.js:15-16`）。改法：同一序列新增视频侧 Shortcut，`enabled: videoWorkspaceActive && videoTabCount > 1`，`onActivated: cycleVideoTab(±1)`；两处 enabled 条件互斥（`imageWorkspaceActive` vs `videoActive`），同一组键在各自工作区生效、互不串扰。`ShortcutCatalog` 增加 `nextVideoTab/previousVideoTab`（同序列）；帮助表 `workspaceHelpEntries`（`Main.qml:258`）按工作区行显示——契约测试 `ShortcutBindingsMenuAndHelpStayInSync`（台账 2026-10-09）会强制三方同步，需同步更新其登记表。
- **Ctrl+W**：`closeCurrentTask`（`Main.qml:3815-3821`）已按工作区分支。视频分支（:1559-1587）从「关闭整个会话」改为「关闭当前视频标签」：有 ≥2 标签时 `host.closeActive()`（选相邻，§6）；最后一个标签保留现有 `shell.closeSources()` 路径与「正在关闭视频…」反馈。
- **与图片标签快捷键的一致性**：序列、行为（循环切换、关当前、关闭后选相邻）与图片侧逐项对齐；差异仅在于视频切换走恢复协议（异步），切换期间重复 Ctrl+Tab 应入队或节流（镜像 `cycle` 跳过 loading 的防御，`ImageTaskHost.cpp:347-358`，视频侧还需跳过 restoring）。

## 9. 测试计划

宿主单测（新 `tests/component/ui/VideoTaskHostTests.cpp`，GoogleTest）：

- 身份：AB vs AC 不合并；A 单视频 vs AB 不合并；同组同参考槽（含大小写、冗余路径段、`file:///` 变体）聚焦；顺序无关匹配命中后用记录槽位重开；同组不同参考槽 = 不同任务；上限拦截。
- 打开路由判定：§5 表每行一个「预期决策（新建/聚焦/就地演化/拒绝）」断言。
- 关闭选相邻：中部/末尾/首部/最后一个；loading 中拒绝关闭。
- 快照 API：`updateTaskState`/读取往返；任务记录在重开失败后保留。

`MainQmlContractTests` 契约用例（打开规则每行至少一条，命名延续 `VideoTabs*`）：

- C1 菜单单视频 → 新标签激活；C2 两/三视频 → 一个比较标签 + 参考槽正确；C3 加入当前对比 → 同标签组扩展（标签数不变、preserve 位置）；C4 重复打开同任务 → 聚焦、标签数不变、进度字段保留；C5 侧栏点选在单视频标签内换片（标签数不变、组更新、自动播放发生）；C6 比较标签活动时侧栏单视频 → 新标签、原比较标签存活（后台任务记录仍在）；C7 右键新标签打开；C8 上一项/下一项同 C5；C9 启动参数 kind1/kind2 → 正确任务形态 + 自动播放仅 kind1。
- 切换协议：暂停落位（切回后 playing==false 且帧==快照帧）；同组差量应用不重开（epoch 不前进）；视口/范围/比较对恢复断言。
- **反例 R1（迟到结果不串任务）**：切换期间注入旧任务的 `commandFinished`/统计批次 → 不得应用到新任务（epoch+URL 门）；`PairMetricsController` Scope 反例已有先例（`PairMetricsController.h:26-29`），此处证端到端。
- **反例 R2（恢复失败）**：目标任务一源缺失 → 原任务标签保持活动、画面与位置不动、报错点名缺失文件。
- 关闭：Ctrl+W 关活动视频标签选相邻；最后标签 → 空态或图片工作区；Ctrl+Tab 两工作区各自生效且互不触发对方。

真机/性能门禁单列（WARP/单测不证明）：

- 切换恢复时延：三源 1080p 素材，切回后台标签的 reopen+seek P95（对照 500 ms 冷 seek 既有门禁的可行性，若超出需产品接受或优化预取）。
- 标签数（≤12）下的内存不随标签数增长；D3D11VA 三源切换稳定性（设备/上下文复用路径）。
- 重开后差异统计重采样开销（lane 开启场景）。

## 10. 实施切片与风险

切片（每片可独立交付、独立验收）：

1. **切片 1（首切片）**：`VideoTaskHost` C++ 类 + 宿主单测全绿。任务记录/身份/路由判定/关闭选相邻/tabs 模型，不接 UI（`Main.qml` 不引用），零行为变化。这是可独立交付的最小闭环：规则逻辑全部可测。
2. **切片 2**：Main.qml 接线最小集——文件菜单/拖入/启动打开收敛到 `host.openTask`；标签条 UI；Ctrl+W/Ctrl+Tab 视频侧生效；状态快照最小集（素材组、参考槽、位置、模式）。此时「重复打开聚焦 + 进度保留」已可用。
3. **切片 3**：完整 §3 状态恢复 + §4 分阶段协议（比较对命令链、视口/ROI/wipe、范围族、对齐偏移、恢复状态句、失败分级、deadline）。契约用例 C1-C9 与 R1/R2 随片落地。
4. **切片 4**：文件夹侧栏路由（行 5/6/7/8：就地换片、比较活动时新标签、右键强制新建）、`removeActiveSource` 身份迁移（§11-f）、边缘情况打磨。
5. **切片 5**：真机性能证据、决策登记（新 D-0xx：视频任务标签；D-022 补充关联）、台账条目、帮助/菜单文案核对。

风险清单：

- **切换延迟体感**：冷 seek + 统计重采样可能让三源任务切回 >1 s；缓解：恢复状态句先行、seek P95 门禁、必要时对「最近离开的任务」保留短窗预取（超出 D-014 预算需新决策）。
- **preserveDisplayedTime 误用**：跨任务切换若错用 preserve=true 会把上一任务位置映射进新时间线；以契约测试钉死（切片 3 用例）。
- **shell 双镜像状态**（range 族 shell+controller 两份）恢复顺序错配 → 范围错乱；沿用 `playIntervalRange` 的先 shell 后命令顺序（`Main.qml:2023-2034`）。
- **`Main.qml` 复杂度**：恢复编排再入式状态机已有一份（issue restore），标签恢复复用其骨架而非另写一套，降低两台状态机并存的漂移风险；若评审认为应抽公共 `StagedRestore.js`，列入切片 3 任务。
- **身份迁移边界**（组 2→1、AB→ABC 演化）若实现成「删旧建新」会意外丢进度；设计要求演化原地更新记录并保留快照。
- **自动播放与恢复的混淆**：侧栏换片（播）与切回标签（不播）路径不同源；测试 C5 与「切回保持暂停」各钉一条。

## 11. 待拍板问题

- **a. 恢复超时阈值**：建议沿用 15 s（issue restore 先例）还是收紧到 8 s？切换是高频操作，超时体感比恢复失败更常见；可按「重开成功但定位未确认」单独设短超时。
- **b. 标签数量上限**：对齐图片 12（`ImageTaskHost.cpp:15`）还是更少（视频任务重建成本高、切换延迟可能让多标签堆积无意义）？本设计按 12 起草。
- **c. 文件夹导航位置按标签还是全局**：现状全局唯一 + 单视频自动跟随（`VideoFolderModel.cpp:384-407` `synchronizeSources`/`followCurrentFolder`）。按标签保存需要在切标签时同步模型目录（扫描有成本）；全局则切回标签时行高亮可能错位。建议首版全局、仅记录高亮，目录跟随仍按当前会话源。
- **d. 视频标签与图片标签是否同栏共存**：现状调查结论——不同栏（图片标签条在 `ImageWorkspace` 内部，`Main.qml:4721`；两工作区互斥显示）。建议维持分栏；若产品想要顶部统一任务栏是独立 UX 改版，另立设计。
- **e. 比较任务匹配是否顺序无关**：本设计按「多重集 + 参考身份」匹配（B↔A 聚焦 A↔B 标签）；若产品认为槽位顺序也是任务的一部分（用户刻意区分 A/B 角色），改为严格有序匹配（一行代码的差异，测试不同）。
- **f. 组 2→1 的身份迁移**：「关闭此视频」把比较缩到一路后，任务演化为准视频形态（同标签）还是要求显式确认？本设计按同标签演化起草。
- **g. 自动对齐/手动锚点是否随标签保存**：对齐分析耗时且属会话内计算产物；首版不保存（切回后回到默认 FrameIndex + 手动偏移量照存，#4）。若用户依赖自动对齐结果，需把 `alignmentTimelineMarkers`/序列对齐快照纳入状态包（成本高，建议后续版本）。
- **h. 「复制当前任务到新标签」**：提案中标注的未来项；预留 `host.cloneTask(index)` 的 API 位置，不在本设计展开。
- **i. 后台任务的对齐分析是否允许继续跑**：现状对齐分析绑定当前会话；标签化后后台任务的分析请求会随重开被取消。建议首版明确「后台任务不跑分析」（与 §7 资源边界一致），文档明示该限制。

### 决议记录（lead 评审拍板，实施按此执行）

- **a**：首版单一 15 s 恢复 deadline，复用 issue-restore 骨架与 `issueRestoreDeadline` 先例，不引入分段超时；切片 5 真机时延证据落地后按体感复判（临时假设）。
- **b**：标签上限对齐图片侧 12。
- **c**：首版文件夹导航全局唯一，任务记录仅保存行高亮；目录跟随当前会话源（临时假设，切标签高亮错位的体感差再复判）。
- **d**：维持两工作区分栏，不做顶部统一任务栏。
- **e**：比较任务匹配 = 身份多重集 + 参考槽身份（顺序无关）；命中后按任务记录保存的槽位顺序重开，不采用新打开顺序。
- **f**：组演化（2→3 加入对比、3→2 / 2→1 移除一路）一律同标签原地更新记录并保留快照；不新建标签、不加确认。
- **g**：首版不保存自动对齐分析结果与手动锚点；对齐模式与手动偏移量照存（§3 #4）。该限制需写入用户可见文档。
- **h**：不做「复制当前任务到新标签」；`cloneTask` 仅预留 API 位置。
- **i**：后台任务不跑任何分析/统计；重开即取消，切回重采样。写入文档明示。
