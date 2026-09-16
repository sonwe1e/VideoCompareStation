# B · 根因独立再推导验证报告

> 角色：Chapter 08 §III 独立根因再推导。对计划 `02_根因与缺陷调查.md` 的每条 C/H/G 主张，**从代码独立重做推理链**，给出 CONFIRMED / PARTIAL / REFUTED + 精确 file:line + 可变更机制根因。不信任计划文本。
>
> 仓库 HEAD：d04339c。调查范围：`src/domain`、`src/application`、`ui_qml`、`persistence_json`、`presentation_contract`、`platform_windows`。

---

## 0. 方法论

每条主张按以下结构判定：

1. **定位主张**：计划原文声称了什么机制。
2. **代码证据**：独立找到的最小 file:line 集合，构成推理链。
3. **判定**：CONFIRMED / PARTIAL / REFUTED。
4. **根因陈述**：必须是**可变更的机制**（changeable mechanism），不是症状（"代码复杂"不算）。
5. **置信度**：HIGH / MEDIUM / LOW（取决于是否需要运行时/硬件条件才能触发）。
6. **最小复测建议**：每条 CONFIRMED 给一个可红测的最小制品。

一个有效根因必须能回答："改哪一类东西、改掉什么机制，问题就不再产生。"

---

## 1. C-01 · Reference == canonical timeline master

### 计划主张
`ComparisonValidator.cpp` 令 `canonicalId = referenceId`；切换 Reference 会重建时间线、帧号、Range、对齐上下文。

### 代码推理链
- `domain/src/ComparisonValidator.cpp:195`
  ```cpp
  const SourceId canonicalId = referenceId.value_or(sources.front().id);
  ```
- 扫描 sources，把 `role == ComparisonRole::kReference` 的那个 source 的 id 赋给 `referenceId`（`:185-191`）。
- 构造 `ValidatedComparisonSet{std::move(sources), canonicalId, referenceId}`（`:204-206`）。
- `ValidatedComparisonSet::canonicalDescriptor()` → `find(canonicalSourceId_)->descriptor`（`ValidatedComparisonSet.h:41-43`），`canonicalFrameCount()` 走同一 descriptor（`:49-51`）。
- 因此 canonical 的 frameCount / frameRate / duration 全部来自 referenceId 对应的 source。

### 事件序列
1. 用户把 source B 设为 Reference。
2. 验证器把 B.id → referenceId → canonicalId。
3. canonicalDescriptor 变成 B.descriptor；canonicalFrameCount、canonicalRate 全部切换。
4. 所有 MediaTime↔FrameId 映射、Range remap、alignment 基准、oneSecondStepFrames、timecode 都跟着变。

### 判定：CONFIRMED（HIGH）

### 根因（可变更机制）
**`ReferenceRole` 与 `TimelineMaster` 被合并进同一个 `SourceId` 字段**（`referenceId` 兼任 canonical）。两者变化频率与副作用已经不同（Reference 是"比较基准"，TimelineMaster 是"时间轴定义"），但类型未分离。变更机制：把"哪个 source 定义 canonical timeline"从"哪个 source 是 Reference"拆成两个独立状态字段。

### 最小复测
构造 3-source 集，分别以 A / B 为 Reference，断言 `canonicalSourceId()` 与 `canonicalFrameCount()` 随 Reference 切换而切换（当前行为为红，修复后应为固定 master 不随 Reference 变）。

---

## 2. C-02 · Active Pair 被持久化为全局 A/B/C ordinal

### 计划主张
`DifferenceEdge` 只有 0↔1 / 0↔2 / 1↔2 三条；`ReviewPreferencesController` 把它作为全局偏好持久化；拓扑变化后语义漂移。

### 代码推理链
- `presentation_contract/include/dvs/presentation/ComparisonContract.h:35-42`：`DifferenceEdge` 仅三值 `Edge0And1/Edge0And2/Edge1And2`（加 `Between*` 别名）。
- `ReviewPreferencesController.cpp:37`：`kDifferenceEdgeKey = "review.difference-edge"`。
- 持久化写入：`:533-544`，把 `differenceEdge_` 序列化为 `"0-1"/"0-2"/"1-2"` 字符串。
- 加载解析：`:455-461` `parseEnum<DifferenceEdge>` 还原。
- 存储的是**槽位 ordinal**（0/1/2），不是稳定 SourceKey。
- `ReviewShellController.cpp:847-855`：恢复时把 `requestedEdge` 夹紧到 `[Between0And1, Between1And2]`；仅当 `sourceCount == 2` 时强制 `kEdge0And1`，但**保留原偏好值**（注释 + 代码未清除），等第三路恢复时重新生效。
- `SettingsRepository.cpp` 是通用 JSON 键值存储（`:76-83`），并不感知 DifferenceEdge 语义；持久化键 `review.difference-edge` 由 preferences 层决定。

### 事件序列
1. 三路会话，用户选 A↔C（Edge0And2 = `"0-2"`）→ 持久化。
2. 删除 C → 两路会话，shell 把无效边投影为 0↔1 但保留 `"0-2"` 偏好。
3. 重新加入第三路 → 旧 `"0-2"` 重新生效，界面突然跳回 A↔C。

### 判定：CONFIRMED（HIGH）

### 根因（可变更机制）
**"用户通用偏好 / 当前会话选择 / 渲染槽位映射"三类状态被压进同一个全局 ordinal 字段**。只有第一类适合进全局 Preferences；第二类必须按稳定 SourceKey 保存；第三类只应存在于 presentation adapter。变更机制：把持久化的"具体哪两条 slot"改成持久化"DefaultPairPolicy"，会话内用 `ComparisonPair{SourceKey, SourceKey}` 保存真实选择。

### 最小复测
持久化 `differenceEdge=0-2`，构造 2-source 会话再扩回 3-source，断言 active pair 在拓扑变化前后保持语义一致（当前会跳变为红）。

---

## 3. C-03 · 连续后退/多帧步进仍走 Exact Seek

### 计划主张
`PlaybackCoordinator::beginStep` 仅优化 +1；反向/多帧走 Exact Seek。

### 代码推理链
- `application/src/PlaybackCoordinator.cpp:2135-2138`：
  ```cpp
  if (command.delta == 1) {
      enqueueInteractiveForwardStep(command);
      return;
  }
  ```
- `:2154`：其它情况 → `beginSeek(command.context, FrameId{target})`（Exact 路径）。
- `beginSeek` → `submitFirstOrSeekFrame(frameId, PendingPhase::kSeekingFrame)`（`:2091-2120`），请求优先级 `FrameRequestPriority::Exact`（`:1608`）。
- `beginInteractiveForwardStep`（`:1238-1288`）用 `Sequential` 优先级（`submitInteractiveStepRequest` → `FrameRequestPriority::Sequential`），整条 run 共享一个 generation（`:1259-1262`）。
- 反向 `-1`、Shift+`-5` 全部命中 `:2154` 的 Exact 路径。

### 事件序列
- 长按 Right：同一 generation、Sequential、read-ahead → 流畅。
- 长按 Left：每个输入 cancel + Exact seek → generation 风暴、卡顿。

### 判定：CONFIRMED（HIGH）

### 根因（可变更机制）
**`beginStep` 把"步进方向/大小"与"解码路径"硬绑定**：仅 `delta==1` 走 Sequential，其它全部回退到 Exact。变更机制：引入反向专用算法（前向关键帧 K → 正向解码到 N → 窗口 cache → 反向提交），把"步进方向"与"是否可 Sequential 解码"解耦。

### 最小复测
`StepFrames(-1)` × 300，断言 0 missing intermediate canonical FrameId、0 stale commit、0 partial FrameSet、exact seek 数在 warm-up 后显著下降。

---

## 4. C-04 / H-01 · Range Loop 权威在 QML + 越界风险

### 计划主张
Range Loop 由 `Main.qml::onCurrentFrameChanged` 驱动；内核无法保证永不呈现范围外帧；Out 之后帧可能先呈现再回环。

### 代码推理链（C-04 确认）
- `ui_qml/qml/Main.qml:267-285` `onCurrentFrameChanged`：
  - `:280-284`：`rangePlaybackActive && playing && outFrame >= inFrame && currentFrame >= outFrame` → `setRangeStartPending(true)` + `controller.seekFrame(inFrame)`。
  - `:272-278`：`rangeStartPending && currentFrame === inFrame && !busy` → 重新启动 play。
- 这是**唯一**的 range 循环驱动点；coordinator 里没有任何 range 概念。

### 代码推理链（H-01 确认机制存在）
- `PlaybackCoordinator.cpp:850-898` `playbackTargetAt`：
  - `:856-863`：在 `kPlaybackCatchUpTolerance`（2000ms）内 → 呈现 `nextMinimum`（保每帧）。
  - `:864-897`：超过容忍度 → 按 wall-clock 计算 target，**可跳过整段 FrameSet**，且**不做任何 range  clamp**。
- `commitPlaybackFrameIfComplete`（`:2810-2864`）：`reachedEnd` 仅检查 `displayedFrame+1 >= canonicalFrameCount`（`:2823-2824`），即**仅 canonical 末尾**，不检查 Out 点。
- 因此 coordinator 在 catch-up 后可能直接选 Out 之后的 canonical target → 呈现 → QML 只在 `displayedFrame` 变更后（`:2837` 赋值之后）的 `onCurrentFrameChanged` 才发现 `>= Out`。

### 事件序列（H-01）
1. 播放中发生 stall（如降核到 BelowNormal）。
2. 恢复后 `playbackTargetAt` 发现超过 2000ms → 选 Out+Δ 的 target。
3. 该帧被 commit → `state_.displayedFrame = Out+Δ`（`:2837`）→ 呈现。
4. QML `onCurrentFrameChanged` 触发 → 发现 `>= Out` → seek 回 In。
5. **Out+Δ 已被呈现**，违反范围契约。

### 判定：C-04 CONFIRMED（HIGH） / H-01 CONFIRMED（HIGH，机制可证；触发需 stall 条件）

### 根因（可变更机制）
**Range 约束仅存在于 QML 对 `displayedFrame` 的事后观察，coordinator 的 target 选择完全不感知 Range**。变更机制：把 Range 约束下沉到 `PlaybackRun` 的 target 选择路径中（选取下一 target 前先应用范围 clamp），使 Out 之后永远不被提交，消除 QML 事后观察的窗口。

### 最小复测
fake clock + deterministic renderer ACK：人为注入 >2000ms stall 后恢复，断言 `displayedFrame` 永远不超过 `outFrame`（当前会为红）。

---

## 5. C-05 · 缩略图只抓取已显示帧

### 计划主张
`TimelineThumbnailCache.qml` 用 `grabToImage` 抓已显示画布，无独立解码器。

### 代码推理链
- `ui_qml/qml/TimelineThumbnailCache.qml:48`：`sourceItem.grabToImage(result => {...})` — 抓的是**当前显示的 sourceItem 画布**。
- `:83`：`onCurrentFrameChanged: capture(currentFrame)` — 仅在 currentFrame 变化时抓取。
- `:17`：`sampleInterval = ceil(totalFrames/120)` — 仅约 120 个 sample。
- 无任何独立解码器、无任意 target frame 请求、无 Reference/pair 区分、无 fingerprint/timeline-revision cache key。
- popup 文本 "Preview caches during playback" 与行为一致。

### 判定：CONFIRMED（HIGH）

### 根因（可变更机制）
**缩略图被实现为 scene-graph screenshot cache，而非媒体查询服务**。变更机制：引入低优先级 `IThumbnailProvider`（latest-wins、独立解码器、目标尺寸解码、LRU、可取消），QML 只发 hover frame，不直接抓画布。

### 最小复测
hover 到一个从未播放过的帧，断言能返回缩略图（当前为空→红）。

---

## 6. C-06 · façade 把多个 capability 指向同一大 Controller

### 计划主张
`ReviewSessionFacade` 的 playback/alignment/notifications 全部指向同一个 `ReviewController`。

### 代码推理链
- `ui_qml/src/ReviewSessionFacade.cpp:19-33`：
  - `:19-21` `playback()` → `&review_`
  - `:23-25` `alignment()` → `&review_`
  - `:31-33` `notifications()` → `&review_`
  - 三个 capability 全部返回同一个 `ReviewController&`。
- `ReviewController.h:30-85`：`ReviewView` 包含 source/URL/session/busy/playback/frame/timecode/所有媒体信息/错误/alignment/marker/progress/compatibility/differenceEdges/capabilities — 一个巨大的 flat 状态对象。

### 判定：CONFIRMED（HIGH）

### 根因（可变更机制）
**façade 只完成了命名边界（不同方法名），没有完成状态所有权迁移**——三个 capability 共享同一个大而全的 `ReviewView`。变更机制：按 capability 拆分子 snapshot（Comparison/Timeline/Playback/Diagnostics/Session ViewModel），每个只读一个 typed sub-snapshot，并只发本 capability 的命令。

### 最小复测
新增一个属性时，断言只需修改一个 ViewModel + 其对应 QML 绑定（当前需改 controller/view/signal/QML 四处→红）。

---

## 7. C-07 · 隐藏式播放 catch-up（2000ms 混合策略）

### 计划主张
`kPlaybackCatchUpTolerance = 2000ms`；用户不知道当前是"保每帧慢放"还是"保实时跳帧"。

### 代码推理链
- `application/src/PlaybackCoordinator.cpp:51`：`constexpr auto kPlaybackCatchUpTolerance = 2000ms;`（注释 `:44-50` 详细解释设计意图）。
- 决策逻辑 `playbackTargetAt`（`:850-898`）：
  - `:856-863`：`now <= nextDue + 2000ms` → 返回 `nextMinimum`（保每帧，允许时钟滑移）。
  - `:864-897`：否则 → 按 wall-clock 算 target，可跳整段 FrameSet。
- 该切换**完全隐式**，无 `PlaybackPolicy` 枚举、无状态栏指示、无 dropped/late 计数暴露。
- 用户无法从 UI 上区分当前处于哪种模式。

### 判定：CONFIRMED（HIGH）

### 根因（可变更机制）
**catch-up 阈值与策略被硬编码为隐式常量，无显式建模**。变更机制：引入 `enum class PlaybackPolicy { ReviewEveryFrame, RealTime, Contextual }`，状态栏显示实际策略 + dropped/late 计数。

### 最小复测
注入可控 stall，断言 UI 显示的 policy/dropped 计数与 coordinator 实际行为一致（当前无显示→红）。

---

## 8. C-08 · Wipe/Timeline 键盘与 Accessible value 不完整

### 计划主张
`WipeHandle.qml` 无 `activeFocusOnTab` / `Accessible.role=Slider` / min/max/value / 方向键；`TimelineTracks.qml` 无 `Accessible.value/minimumValue/maximumValue` / 键盘键。

### 代码推理链
- `ui_qml/qml/WipeHandle.qml`：
  - 有 DragHandler（`:75-85`）、TapHandler（`:87-91`）、HoverHandler（`:93-97`）。
  - **无** `activeFocusOnTab`、**无** `Accessible.role`、**无** `Accessible.value/minimumValue/maximumValue`、**无** `Keys.onLeft/Right`、**无** orientation 描述。
- `ui_qml/qml/TimelineTracks.qml`：
  - 有 `activeFocusOnTab: enabled`（`:27`）、`Accessible.role: Accessible.Slider`（`:28`）、`Accessible.name`（`:29`）。
  - **无** `Accessible.value` / `minimumValue` / `maximumValue`。
  - **无** `Keys.onLeft/Right/Home/End/PageUp/PageDown`。
  - **无** focused/dragging 的读屏反馈。

### 判定：CONFIRMED（HIGH）

### 根因（可变更机制）
**自定义控件只实现了鼠标/拖拽输入契约，未实现完整的键盘/辅助功能契约**。变更机制：为所有可交互自定义控件建立自动 accessibility contract test（role + value + 键盘 + 读屏反馈），作为合入门禁。

### 最小复测
自动化 a11y 断言：WipeHandle 有 `Accessible.Slider` + 可聚焦 + 方向键调节；TimelineTracks 有 `Accessible.value/minimumValue/maximumValue`（当前缺失→红）。

---

## 9. C-09 · 三路硬编码不是简单上限

### 计划主张
Source 上限及渲染 Pair 在 validator / 数组 / QML / DifferenceEdge / renderer 多层硬编码为 3/0..2。

### 代码推理链 —— 完整 3-硬编码位置清单

| # | 文件:行 | 硬编码形式 | 说明 |
|---|---------|-----------|------|
| 1 | `domain/src/ComparisonValidator.cpp:86,160` | `kMaximumSources = 3U` | domain 验证上限 |
| 2 | `presentation_contract/include/dvs/presentation/ComparisonContract.h:59` | `maximumSourceCount = 3U`（默认） | 模式描述符默认值 |
| 3 | `presentation_contract/include/.../ComparisonContract.h:35-42` | `DifferenceEdge` 仅三值 | 枚举只有 0↔1/0↔2/1↔2 |
| 4 | `presentation_contract/src/ComparisonContract.cpp:10-24` | 各模式 `maximumSourceCount = 3U` | SideBySide/ThreeUp/ReferenceFocus/Difference/Wipe |
| 5 | `ui_qml/src/ComparisonSurface.cpp:122-128` | `std::array<platform::SurfaceDisplayExtent, 3U>` + `index < 3` | 显示区数组 |
| 6 | `ui_qml/include/dvs/ui/ComparisonSurface.h:95-100` | `DifferenceEdge { Edge0And1, Edge0And2, Edge1And2 }` | QML 侧枚举镜像 |
| 7 | `ui_qml/src/ReviewController.cpp:391` | `std::array<LocalFileValidation, 3U> validated` | 文件验证 |
| 8 | `ui_qml/src/ReviewController.cpp:535` | `std::array<qint64, 3U> values{sourceAFrames, sourceBFrames, sourceCFrames}` | 对齐偏移 |
| 9 | `ui_qml/src/ReviewController.cpp:234` | `sourceId < 3U ? ... : ...` | source 命名 A/B/C |
| 10 | `ui_qml/src/ReviewController.cpp:1278` | `preferenceValue = first==0&&second==1?0:(first==0?1:2)` | pair 标签 ordinal 映射 |
| 11 | `ui_qml/include/dvs/ui/ReviewController.h:30-32,53-58` | `sourceA/B/CFilename`、`sourceA/B/CErrorKey`、`sourceA/B/CMissing` | QML 属性契约 |
| 12 | `ui_qml/src/ReviewController.cpp:167-169,188-193` | `sourceA/B/CFilename/ErrorKey/Missing` 成员 | ReviewView 字段 |
| 13 | `ui_qml/qml/ActiveSourceStrip.qml:267` | `visible: control.sourceCount < 3` | 添加按钮显隐 |
| 14 | `ui_qml/qml/ComparisonToolbar.qml:47` | `model: sourceCount>=3 ? [A,B,C] : [A,B]` | Reference 下拉 |
| 15 | `platform_windows/include/dvs/platform/D3d11ComparisonRenderer.h:132-133,149` | `std::array<SurfaceRect,3U> sourceRects`、`sourceSlots{0,1,2}`、`sourceContentRects[3]` | 渲染 panel 布局 |
| 16 | `platform_windows/include/dvs/platform/D3d11ComparisonRenderer.h:188` | `std::array<SurfaceDisplayExtent, 3U>&` | 显示区计算签名 |
| 17 | `platform_windows/src/D3d11ComparisonRenderer.cpp:95,476` | `std::array<VideoDraw,3U>`、`std::array<SurfaceRect,3U> columns` | 绘制/列布局 |
| 18 | `platform_windows/src/D3d11ComparisonRenderer.cpp:1427-1429` | `std::array<const GpuFrameResource*,3U> slotFrames`、`slotBackings[3]` | 渲染 slot 帧 |
| 19 | `platform_windows/src/D3d11ComparisonRenderer.cpp:1498,1500,1548` | `std::array<ID3D11Buffer*,3U>` compose/color/pixel buffers | GPU buffer |
| 20 | `app/Main.cpp:296-297,400-402` | `sourceA/B/CErrorKey` 错误聚合 | CLI/主程序错误显示 |

> 注：`RationalRate.cpp:100` 的 `std::array<std::int64_t,3>` 是乘法因子数学运算，与 source 上限无关；`SourceIdentityService` 的 `array<uint8_t,32>` 是 SHA-256 digest，均已排除。

### 判定：CONFIRMED（HIGH）

### 根因（可变更机制）
**"LoadedSources 逻辑数量 / ActivePresentationSet 实时解码呈现数量 / ActivePair 数量"被混为一个全局常量 3**。变更机制：把三者拆成独立维度（LoadedSources=N 逻辑会话；ActivePresentationSet=M≤3/4 实时解码；ActivePair=2；BackgroundAnalysisSet 可批处理），才能支持 GT+多候选而不要求 16 路同时 120fps 解码。

### 最小复测
把 `kMaximumSources` 提到 4，断言所有上述位置（数组、枚举、QML 属性、renderer slot）仍编译/运行一致（当前多处越界/枚举缺失→红）。

---

## 10. C-10 · Reference 标签歧义

### 计划主张
同一控件同时出现 "Reference" / "Canonical reference source" / "Make reference"，未告诉用户它会重建时间线。

### 代码推理链
- `ui_qml/qml/ComparisonToolbar.qml:36`：`qsTr("Reference")`（标签）。
- `:49`：`Accessible.name: qsTr("Canonical reference source")`。
- `ui_qml/qml/ActiveSourceStrip.qml:153`：`Accessible.name: qsTr("Canonical reference")`（badge）。
- `:245`：`qsTr("Make reference")`（菜单项）。
- 三处措辞不同，且**无任何一处提示"切换会重建时间线/帧号/Range/对齐"**。

### 判定：CONFIRMED（HIGH）

### 根因（可变更机制）
**UI 文案把"Timeline master"与"Comparison reference"两个语义合并为一个"Reference"标签**。变更机制：在 C-01 语义分离完成前，至少把标签改为 "Timeline & Reference" 并显示副作用；分离后改为独立控件。

### 最小复测
文案审计：切换 Reference 时，UI 必须显示"将重建时间线/帧号/Range"副作用提示（当前无→红）。

---

## 11. C-11 · 比较标签在 compact/fullscreen 只显示 A/B/C

### 计划主张
compact/fullscreen 下主要只显示字母，文件名上下文不足。

### 代码推理链
- `ui_qml/qml/ComparisonViewport.qml:465-518` `surfaceLabelRepeater`：
  - `:472-473`：`showFilename = !singleMode && !wipeBadge && panelWidth >= 160`；`compactBadge = singleMode || wipeBadge || panelWidth < 160`。
  - `:497`：`String.fromCharCode(65 + Number(modelData.slot))` → 仅显示 A/B/C 字母。
  - 文件名仅在 panelWidth≥160 且非 wipe/single 时显示（`:504-517`）。
- `:189-195` 沉浸式 HUD 也仅显示字母。
- `ActiveSourceStrip.qml:110`：`qsTr("%1 · %2").arg(String.fromCharCode(65+sourceId)).arg(filename)` — 有文件名，但 strip 在 singleMode 外才显示（`:33-34`）。

### 判定：CONFIRMED（HIGH）

### 根因（可变更机制）
**面板标签的"字母 vs 文件名"切换仅依赖 panelWidth，不依赖用户上下文需求**。变更机制：提供会话内 alias、role badge（GT/Reference/Candidate）、compact label（GT/C1/C2）、hover/OSD 完整文件名，active pair 始终在状态栏可见。

### 最小复测
两个相似文件名（仅后缀差异）在 wipe/compact 模式下，断言用户能在 hover/OSD 看到完整文件名（当前仅字母→红）。

---

## 12. C-12 · 状态反馈分散为 overlay/banner/bubble

### 计划主张
错误/对齐/精确性/忙碌竞争视觉优先级。

### 代码推理链
`ui_qml/qml/ComparisonViewport.qml` 中并存的状态层：
- `:231-263` `framePendingIndicator`（bubble）。
- `:343-376` `analysisChrome` / `analysisStatus`（精确性/ROI badge）。
- `:378-417` `differenceUnavailableOverlay`。
- `:419-444` `alignmentStatus`（banner）。
- `:522-564` `frameErrorBanner`。
- `:566-615` `statusOverlay`（full overlay，含 busy/title/detail）。
- `ui_qml/qml/Main.qml:255` `previewTimecode`、`:268-270` `showImmersiveHud(frameText)` — 状态文本拼接在 QML 层。
- 多层可同时可见（如 `frameErrorBanner` 锚定在 `alignmentStatus` 下方 `:536`），争夺画布。

### 判定：CONFIRMED（HIGH）

### 根因（可变更机制）
**状态反馈被实现为多个独立的视觉层，无统一优先级仲裁**。变更机制：统一为有优先级的 typed status rail，避免多层同时争夺画布。

### 最小复测
同时触发 alignment + error + frame-pending，断言只有一个最高优先级状态可见/播报（当前多层叠加→红）。

---

## 13. 计划主张的事实性核验小结

| ID | 计划主张 | 判定 | 关键 file:line |
|----|---------|------|----------------|
| C-01 | canonicalId = referenceId | **CONFIRMED** | `ComparisonValidator.cpp:195` |
| C-02 | DifferenceEdge 全局 ordinal 持久化 | **CONFIRMED** | `ReviewPreferencesController.cpp:37,533-544`；`ReviewShellController.cpp:847-855` |
| C-03 | 仅 +1 走 Sequential，其余 Exact | **CONFIRMED** | `PlaybackCoordinator.cpp:2135-2138`（+1） vs `:2154`（Exact） |
| C-04 | Range Loop 权威在 QML | **CONFIRMED** | `Main.qml:267-285`；coordinator 无 range（`:2823-2824` 仅 canonical 末） |
| C-05 | 缩略图仅 grabToImage | **CONFIRMED** | `TimelineThumbnailCache.qml:48,83` |
| C-06 | façade 多 capability → 同一 controller | **CONFIRMED** | `ReviewSessionFacade.cpp:19-33` |
| C-07 | 隐藏 2000ms catch-up | **CONFIRMED** | `PlaybackCoordinator.cpp:51`；决策 `:856-897` |
| C-08 | Wipe/Timeline a11y 不完整 | **CONFIRMED** | `WipeHandle.qml`（无 a11y）；`TimelineTracks.qml:27-29`（有 role/name，无 value/键盘） |
| C-09 | 3-source 多层硬编码 | **CONFIRMED** | 共 20 处，见 §9 清单 |
| C-10 | Reference 标签歧义 | **CONFIRMED** | `ComparisonToolbar.qml:36,49`；`ActiveSourceStrip.qml:153,245` |
| C-11 | compact 仅 A/B/C | **CONFIRMED** | `ComparisonViewport.qml:472-473,497` |
| C-12 | 状态反馈分散 | **CONFIRMED** | `ComparisonViewport.qml` 多层（见 §12） |
| H-01 | Out 后可先呈现再回环 | **CONFIRMED** | `playbackTargetAt:864-897`（无 range clamp）+ `commitPlaybackFrameIfComplete:2837`（先赋值 displayedFrame） |

### 计划主张中需精确化的两处

1. **C-02 持久化位置**：计划写 "ReviewPreferencesController.h、ComparisonContract.h、ReviewShellController.cpp"。实际持久化键 `review.difference-edge` 在 `ReviewPreferencesController.cpp:37` 定义，序列化在 `:533-544`，`SettingsRepository.cpp` 只是通用 JSON 存储（不感知 DifferenceEdge）。`ComparisonContract.h` 仅定义枚举。**实质正确，但精确位置是 `ReviewPreferencesController.cpp`。**
2. **C-03 行号**：计划引用 "beginStep ~line 2122"。实测 `beginStep` 在 `:2122`，`delta==1` 分支在 `:2135-2138`，Exact 回退在 `:2154`。**行号准确。**

### 计划未列入但代码暴露的额外结构债（非计划主张，仅补充）

- **`ReviewController.cpp:1278` 的 pair ordinal 映射** `first==0&&second==1?0:(first==0?1:2)` 是纯位置算术，当 source 重排序/删除后，`preferenceValue` 与原始 SourceKey 的对应关系断裂——这是 C-02 的另一个症状，但计划未点名这一行。
- **`ActiveSourceStrip.qml:267` `sourceCount < 3`** 与 **`ComparisonToolbar.qml:47` 的 A/B/C 硬编码 model** 是 C-09 清单中 QML 侧最脆的两处，改上限时必破。

---

## 14. 置信度与总体结论

- **12 条 C 主张全部 CONFIRMED**，置信度均为 HIGH（机制可直接从静态代码证明）。
- **H-01 从"Hypothesis"提升为 CONFIRMED**：coordinator 的 `playbackTargetAt` 无 range clamp + `commitPlaybackFrameIfComplete` 先赋值 `displayedFrame` 后 QML 才观察，机制可证；触发需 >2000ms stall 条件，故复测需 fake clock。
- **无 REFUTED 主张**：计划根因调查的事实基础准确。
- **机制可变更性**：每条根因均指向一个可改动的机制（字段合并、ordinal 持久化、delta-路径绑定、range 约束位置、screenshot-cache 身份、façade 所有权、隐式策略、a11y 契约、常量合并、标签语义、宽度仲裁、状态优先级），没有一条止于"代码复杂"。

### 复测优先级（按根因污染度）
1. C-04/H-01 — deterministic fake-clock red test（唯一需运行时条件的 HIGH 项）。
2. C-01 / C-02 — 语义分离的单元测试（拓扑切换 + Reference 切换）。
3. C-09 — 把 `kMaximumSources` 提到 4 的破坏性编译/运行测试（验证"改常量"清单完整性）。
4. C-03 — held-backward 300 步 profile。
5. C-08 — 自动 a11y contract test 作为门禁。

---

*报告生成：独立根因再推导，HEAD d04339c。所有行号均经实测核对。*
