# C — Migration-Surface & Implementation-Feasibility Survey

**Repo:** repository root · **HEAD:** d04339c · **Scope:** `src/` + `src/ui_qml/qml/`
**Basis:** plan types `SourceKey`, `SourceSlot`, `RenderSlotMapping`, `ComparisonSelection`,
`TimelineSelection`, `ComparisonPair`, `DefaultPairPolicy` (03_目标架构设计.md §1, §2).

This survey maps **every** call site the migration touches, so the Implementation role knows
true scope and the Architecture role's "identity not slot" claim can be checked for completeness.
All source paths are repository-relative; `src/` entries are relative to the repository root.

---

## Surface 1 — SourceId-as-slot usages

Places `SourceId` is used as an array/slot index (`sources[id]`, `slots[id]`, `validated[0..2]`)
rather than as a stable identity. The plan wants `SourceKey` (stable) vs `SourceSlot` (transient).

### `ValidatedComparisonSet` API (definition)

| File:line | Usage |
|---|---|
| `domain/src/ComparisonValidator.cpp:9` | ctor takes `canonicalSourceId` |
| `domain/src/ComparisonValidator.cpp:11` | stores `canonicalSourceId_` |
| `domain/src/ComparisonValidator.cpp:22` | `find(SourceId id)` — linear scan by id |
| `domain/src/ComparisonValidator.cpp:33` | `canonicalSourceId()` accessor |
| `domain/src/ComparisonValidator.cpp:34` | returns `canonicalSourceId_` |
| `domain/src/ComparisonValidator.cpp:42` | `canonicalDescriptor()` = `find(canonicalSourceId_)->descriptor` |
| `domain/include/dvs/domain/ValidatedComparisonSet.h:22` | `find` declaration |
| `domain/include/dvs/domain/ValidatedComparisonSet.h:33` | ctor declaration |
| `domain/include/dvs/domain/ValidatedComparisonSet.h:37` | `canonicalSourceId_ = 0` member |

### `canonicalSourceId()` consumers (the coupling the plan must break)

| File:line | Usage |
|---|---|
| `application/src/PlaybackCoordinator.cpp:1729` | `.canonicalSourceId = sources_->canonicalSourceId()` |
| `application/src/PlaybackCoordinator.cpp:1912` | `const SourceId canonicalId = set.canonicalSourceId()` |
| `application/src/PlaybackCoordinator.cpp:2185` | `offset.sourceId == sources_->canonicalSourceId()` |
| `application/src/PlaybackCoordinator.cpp:2247` | `.canonicalSourceId = sources_->canonicalSourceId()` |
| `application/src/PlaybackCoordinator.cpp:2299` | `.canonicalSourceId = sources_->canonicalSourceId()` |
| `application/src/PlaybackCoordinator.cpp:2372` | `command.sourceId == sources_->canonicalSourceId()` |
| `application/src/PlaybackCoordinator.cpp:3193` | `estimate.sourceId == sources_->canonicalSourceId()` |
| `application/src/PlaybackCoordinator.cpp:3226` | `result.sourceId == sources_->canonicalSourceId()` |
| `application/src/AlignmentWorkEstimator.cpp:20,22,23` | `findCanonical(..., canonicalSourceId)` |
| `application/src/AlignmentWorkEstimator.cpp:84,97` | canonical identity checks |
| `application/src/AlignmentWorkEstimator.cpp:125,137` | canonical identity checks |
| `application/src/AlignmentCacheIdentity.cpp:106` | `result.sourceId == sources.canonicalSourceId()` |
| `application/src/AlignmentCacheIdentity.cpp:165` | hashes `canonicalSourceId()` |
| `media_ffmpeg/src/AlignmentAnalysisService.cpp:127,506,518,564,575,591,599,636,648` | canonical identity in alignment |
| `media_ffmpeg/src/MultiSourceFrameProvider.cpp:525,760,765,854` | canonical in provider |
| `application/include/dvs/application/Ports.h:45,65,75` | `canonicalSourceId` in 3 request structs |

### Indexed `SourceId` access (id used as 0/1/2 slot literal)

| File:line | Usage |
|---|---|
| `ui_qml/src/ReviewController.cpp:391` | `std::array<LocalFileValidation, 3U> validated` |
| `ui_qml/src/ReviewController.cpp:404-409` | `validated[0U]`,`[1U]`,`[2U]` by slot |
| `ui_qml/src/ReviewController.cpp:429,431,434` | `appendSource(*validated[0/1/2].candidate, 0/1/2)` |
| `ui_qml/src/ReviewController.cpp:535` | `std::array<qint64,3U>{sourceAFrames,sourceBFrames,sourceCFrames}` |
| `ui_qml/src/ReviewController.cpp:979,981,983` | `source.sourceId == 0U/1U/2U` → A/B/C |
| `ui_qml/src/ReviewController.cpp:1214,1216,1218` | `source.sourceId == 0U/1U/2U` → A/B/C error key |
| `ui_qml/src/ReviewController.cpp:1278` | `preferenceValue = first==0&&second==1?0:(first==0?1:2)` (ordinal) |
| `app/CliMain.cpp:55` | `SourceId{0}` |
| `app/CliMain.cpp:60` | `SourceId{1}` |
| `platform_windows/src/D3d11ComparisonRenderer.cpp:1214-1216` | `set.find(0U)`,`find(1U)`,`find(2U)` → slotA/B/C |
| `application/src/PlaybackCoordinator.cpp:2050` | `pending.slots[primary].sourcePath == pending.slots[index].sourcePath` |
| `application/src/PlaybackCoordinator.cpp:2072` | `pendingProbe_->slots[primaryIndex]` |
| `domain/src/ComparisonValidator.cpp:167,200` | `sources[index]`,`sources[other]` |
| `application/src/SessionSnapshot.cpp:25,29` | `sources[index]` |

### `canonicalSourceIdentity` (QML-side projection)

| File:line | Usage |
|---|---|
| `ui_qml/include/dvs/ui/ReviewShellController.h:36` | `Q_PROPERTY(QString canonicalSourceIdentity ...)` |
| `ui_qml/include/dvs/ui/ReviewShellController.h:115` | accessor decl |
| `ui_qml/include/dvs/ui/SourceIdentity.h:13` | `canonicalSourceIdentity(const QUrl&)` free function |
| `ui_qml/include/dvs/ui/SourceIdentity.h:16` | doc comment |
| `ui_qml/src/ReviewShellController.cpp:108` | `canonicalSourceIdentity()` impl |
| `ui_qml/src/SourceIdentity.cpp:17` | free-function impl |
| `ui_qml/src/ReviewController.cpp:707,714` | calls `canonicalSourceIdentity(source)` |
| `ui_qml/src/ReviewController.cpp:861` | `canonicalSourceId()` → `canonicalSourceIndex` |
| `ui_qml/src/ReviewController.cpp:931` | `source.id == canonicalSourceId()` |
| `ui_qml/src/ReviewController.cpp:1242` | `canonicalSourceIdentity(...)` |
| `ui_qml/qml/ActiveSourceStrip.qml:14` | `required property string canonicalSourceIdentity` |
| `ui_qml/qml/ActiveSourceStrip.qml:77` | `isCanonical` computed from it |
| `ui_qml/qml/Main.qml:679-680` | `canonicalSourceId = canonicalSourceIndex>=0?...:0` |
| `ui_qml/qml/Main.qml:1075` | binds `canonicalSourceIdentity` |

**Surface 1 count:** ~95 sites across 18 files. The `canonicalSourceId()` accessor alone has
14 direct C++ call sites; the `find(SourceId)` id-as-slot pattern has ~25; the 0/1/2 literal
chains in `ReviewController.cpp` and the renderer add another ~20.

---

## Surface 2 — DifferenceEdge / differenceEdge usages

The plan wants to replace the ordinal enum with a `ComparisonPair` of `SourceKey`s. Every
reference below must change or gain an adapter.

### Enum definition

| File:line | Usage |
|---|---|
| `presentation_contract/include/dvs/presentation/ComparisonContract.h:35` | `enum class DifferenceEdge : uint8_t` |
| `.../ComparisonContract.h:36` | `Edge0And1 = 0` |
| `.../ComparisonContract.h:37` | `Edge0And2 = 1` |
| `.../ComparisonContract.h:38` | `Edge1And2 = 2` |
| `.../ComparisonContract.h:39-41` | `Between0And1/0And2/1And2` aliases |
| `.../ComparisonContract.h:89` | `DifferenceEdge differenceEdge = Between0And1` field |
| `presentation_contract/src/ComparisonContract.cpp:50` | `enumInRange(differenceEdge, Between1And2)` |

### Preference property (ReviewPreferencesController)

| File:line | Usage |
|---|---|
| `ui_qml/include/dvs/ui/ReviewPreferencesController.h:24` | `Q_PROPERTY(int differenceEdge READ differenceEdgeCode WRITE setDifferenceEdgeCode ...)` |
| `.../ReviewPreferencesController.h:34` | `using DifferenceEdge = presentation::DifferenceEdge` |
| `.../ReviewPreferencesController.h:51,56,65,70` | `differenceEdge()`, `differenceEdgeCode()`, `setDifferenceEdge()`, `setDifferenceEdgeCode()` decls |
| `ui_qml/src/ReviewPreferencesController.cpp:37` | `kDifferenceEdgeKey = "review.difference-edge"` |
| `.../ReviewPreferencesController.cpp:180-181` | `differenceEdge()` impl |
| `.../ReviewPreferencesController.cpp:230-231` | `setDifferenceEdge()` impl |
| `.../ReviewPreferencesController.cpp:455-461` | deserialization `parseEnum<DifferenceEdge>(...kDifferenceEdgeKey, {{"0-1",Edge0And1},{"0-2",Edge0And2},{"1-2",Edge1And2}})` |
| `.../ReviewPreferencesController.cpp:483,490` | change detection + store |
| `.../ReviewPreferencesController.cpp:534-544` | serialization `switch(differenceEdge_)` → edgeName "0-1"/"0-2"/"1-2" |
| `.../ReviewPreferencesController.cpp:572` | `DifferenceEdge differenceEdge_ = Edge0And1` member |
| `.../ReviewPreferencesController.cpp:613-615` | `differenceEdge()` public impl |
| `.../ReviewPreferencesController.cpp:635-636` | `differenceEdgeCode()` |
| `.../ReviewPreferencesController.cpp:667-668` | `setDifferenceEdge()` public |
| `.../ReviewPreferencesController.cpp:687-688` | `setDifferenceEdgeCode()` |

### Persistence (schema migration target)

| File:line | Usage |
|---|---|
| `persistence_json/src/SettingsRepository.cpp:26` | `kSettingsSchemaVersion = 1` (must bump to 2) |
| `persistence_json/src/SettingsRepository.cpp:68,73` | reads `schemaVersion`, requires `==1` |
| `persistence_json/src/SettingsRepository.cpp:162` | writes `schemaVersion` |

### QML that reads/writes differenceEdge

| File:line | Usage |
|---|---|
| `ui_qml/qml/Main.qml:122` | `differenceEdges: controller ? controller.differenceEdges : []` |
| `ui_qml/qml/Main.qml:202` | `differenceEdge: shell ? Number(shell.effectiveDifferenceEdge) : 0` |
| `ui_qml/qml/Main.qml:206-215` | `selectedDifferenceEdge`, `differenceFirstSlot`, `differenceSecondSlot`, `selectedDifferenceExactness` |
| `ui_qml/qml/Main.qml:639-644` | `differenceEdgeIndex()` |
| `ui_qml/qml/Main.qml:1099-1112` | binds `differenceEdges`, `currentEdgeIndex`, `onEdgeRequested` |
| `ui_qml/qml/Main.qml:1154-1178` | binds `differenceEdges`, `differenceEdge`, `onDifferenceEdgeRequested` |
| `ui_qml/qml/Main.qml:1221` | `effectiveDifferenceEdge: root.differenceEdge` |
| `ui_qml/qml/Main.qml:1317-1325` | binds `differenceEdges`, `currentEdgeIndex`, `onEdgeRequested` |
| `ui_qml/qml/ApplicationMenuBar.qml:171-189` | 3 menu items set `preferences.differenceEdge = 0/1/2` |
| `ui_qml/qml/CompareModeBar.qml:15,73,82-83` | `differenceEdges` model, `edgeRequested` |
| `ui_qml/qml/ComparisonToolbar.qml:123-134` | `differenceEdgeCombo` bound to `differenceEdges` |
| `ui_qml/qml/ReviewContextMenu.qml:12,100` | `differenceEdges` model |
| `ui_qml/qml/TabbedInspector.qml:27,29,43,64-66,217-222` | `differenceEdges`, `differenceEdge`, `differenceEdgeRequested`, `differenceEdgeIndex` |
| `ui_qml/qml/ComparisonViewport.qml:39,133` | `effectiveDifferenceEdge` |

### ReviewController / ReviewShellController façade

| File:line | Usage |
|---|---|
| `ui_qml/include/dvs/ui/ReviewController.h:79` | `Q_PROPERTY(QVariantList differenceEdges READ ... NOTIFY frameStateChanged)` |
| `.../ReviewController.h:170` | `differenceEdges()` decl |
| `ui_qml/src/ReviewController.cpp:212,228` | `differenceEdges` field + equality |
| `ui_qml/src/ReviewController.cpp:1276-1294` | **builds** the `differenceEdges` QVariantList (ordinal preferenceValue, firstSourceId, secondSourceId, exactness) |
| `ui_qml/src/ReviewController.cpp:1343` | copies to view |
| `ui_qml/src/ReviewController.cpp:1717-1718` | `differenceEdges()` accessor |
| `ui_qml/include/dvs/ui/ReviewShellController.h:28` | `Q_PROPERTY(int effectiveDifferenceEdge ...)` |
| `.../ReviewShellController.h:108` | accessor decl |
| `.../ReviewShellController.h:164` | `int differenceEdge = 0` in ComparisonState |
| `ui_qml/src/ReviewShellController.cpp:79-80` | `effectiveDifferenceEdge()` impl |
| `ui_qml/src/ReviewShellController.cpp:827-855` | `effectiveComparisonState()` — projects `preferences_->differenceEdge()`, clamps for 2-source |

### ComparisonSurface (UI model)

| File:line | Usage |
|---|---|
| `ui_qml/include/dvs/ui/ComparisonSurface.h:34-35` | `Q_PROPERTY(DifferenceEdge differenceEdge READ ... WRITE ... NOTIFY differenceEdgeChanged)` |
| `.../ComparisonSurface.h:95-100` | `enum DifferenceEdge { Edge0And1, Edge0And2, Edge1And2 }` + Q_ENUM |
| `.../ComparisonSurface.h:136-137` | `differenceEdge()`, `setDifferenceEdge()` decls |
| `.../ComparisonSurface.h:193` | `differenceEdgeChanged()` signal |
| `.../ComparisonSurface.h:216` | `DifferenceEdge differenceEdge_` member |
| `ui_qml/src/ComparisonSurface.cpp:48` | `platform::SurfaceDifferenceEdge differenceEdge = Between0And1` |
| `.../ComparisonSurface.cpp:82-84` | `nativeDifferenceEdge()` cast helper |
| `.../ComparisonSurface.cpp:97` | forwards to renderer |
| `.../ComparisonSurface.cpp:171` | forwards to renderer |
| `.../ComparisonSurface.cpp:279` | `.differenceEdge = presentationOptions_.differenceEdge` |
| `.../ComparisonSurface.cpp:371-381` | `differenceEdge()`, `setDifferenceEdge()` (validates `Edge0And1/0And2/1And2`) |
| `.../ComparisonSurface.cpp:435` | `nativeDifferenceEdge(differenceEdge_)` |
| `.../ComparisonSurface.cpp:554` | `firstDifferenceSlot = differenceEdge_==Edge1And2?1:0` |

### Renderer (D3D11) — consumes the enum as slot selector

| File:line | Usage |
|---|---|
| `platform_windows/include/dvs/platform/D3d11ComparisonRenderer.h:67` | `using SurfaceDifferenceEdge = presentation::DifferenceEdge` |
| `.../D3d11ComparisonRenderer.h:109` | `SurfaceDifferenceEdge differenceEdge` state field |
| `.../D3d11ComparisonRenderer.h:172,186` | params on `makeDifferenceDraw`/`makeComposeConstants` |
| `platform_windows/src/D3d11ComparisonRenderer.cpp:127-130` | `isValid(SurfaceDifferenceEdge)` |
| `.../D3d11ComparisonRenderer.cpp:439` | validates `differenceEdge` |
| `.../D3d11ComparisonRenderer.cpp:547,552` | `computeSurfacePanelLayout(... differenceEdge ...)`; `isValid(differenceEdge)` |
| `.../D3d11ComparisonRenderer.cpp:581-584` | Wipe: `Between0And2`→`{0,2,1}`, `Between1And2`→`{1,2,0}` slot reorder |
| `.../D3d11ComparisonRenderer.cpp:688,698` | `computeSurfacePresentationGeometry` forwards |
| `.../D3d11ComparisonRenderer.cpp:714` | `Between1And2` slot select |
| `.../D3d11ComparisonRenderer.cpp:1266-1274` | Diff: `Between0And2`→frameC as edgeSecond; `Between1And2`→frameB/frameC |
| `.../D3d11ComparisonRenderer.cpp:1312-1320` | Wipe: same frameA/B/C remap |
| `.../D3d11ComparisonRenderer.cpp:1421` | forwards `state.referenceSlot` |

### App-level (Main.cpp)

| File:line | Usage |
|---|---|
| `app/Main.cpp:781-782` | `preferences.differenceEdge() != Edge0And2` |
| `app/Main.cpp:817` | `objectIntPropertyForAutomation("dualVideoSurface","differenceEdge")` |
| `app/Main.cpp:822` | `effectiveEdge != Edge0And1` |

**Surface 2 count:** ~110 sites across 14 files. The enum flows: persistence →
ReviewPreferencesController → ReviewShellController (effectiveDifferenceEdge) + ReviewController
(differenceEdges list) → ComparisonSurface → D3d11ComparisonRenderer, with parallel QML surfaces
in 7 files. The renderer is the deepest consumer: it uses the enum to pick frameA/B/C.

---

## Surface 3 — Hard-coded 3-source / 0..2 assumptions

The plan claims these are in validator/arrays/QML/DifferenceEdge/renderer. Verified and extended below.

### Validator

| File:line | Usage |
|---|---|
| `domain/src/ComparisonValidator.cpp:86` | `constexpr std::size_t kMaximumSources = 3U` |
| `domain/src/ComparisonValidator.cpp:160` | `sources.size() > kMaximumSources` |

### `std::array<*,3>` / `*,3U>` fixed-size arrays

| File:line | Usage |
|---|---|
| `domain/src/RationalRate.cpp:100` | `std::array<std::int64_t,3>` (rate math — **unrelated to source count**) |
| `ui_qml/src/ReviewController.cpp:391` | `std::array<LocalFileValidation,3U> validated` |
| `ui_qml/src/ReviewController.cpp:535` | `std::array<qint64,3U>{sourceAFrames,sourceBFrames,sourceCFrames}` |
| `ui_qml/src/ComparisonSurface.cpp:122,124` | `std::array<SurfaceDisplayExtent,3U>` |
| `platform_windows/include/dvs/platform/D3d11ComparisonRenderer.h:132` | `std::array<SurfaceRect,3U> sourceRects{}` |
| `.../D3d11ComparisonRenderer.h:133` | `std::array<std::uint8_t,3U> sourceSlots{0,1,2}` |
| `.../D3d11ComparisonRenderer.h:149` | `std::array<SurfaceRect,3U> sourceContentRects{}` |
| `.../D3d11ComparisonRenderer.h:188` | `const std::array<SurfaceDisplayExtent,3U>&` param |
| `platform_windows/src/D3d11ComparisonRenderer.cpp:40,42` | `std::array<float,3U> padding/rotationPadding` |
| `.../D3d11ComparisonRenderer.cpp:95` | `std::array<VideoDraw,3U> videoDraws{}` |
| `.../D3d11ComparisonRenderer.cpp:476` | `std::array<SurfaceRect,3U> columns{}` |
| `.../D3d11ComparisonRenderer.cpp:690` | `const std::array<SurfaceDisplayExtent,3U>&` |
| `.../D3d11ComparisonRenderer.cpp:1427-1548` | `slotFrames`,`slotBackings`,`composeBuffers`,`colorBuffers`,`pixelBuffers` arrays of 3 |

### `sourceA/B/C` property façade (ReviewController)

| File:line | Usage |
|---|---|
| `ui_qml/include/dvs/ui/ReviewController.h:30-32` | `sourceAFilename/B/CFilename` Q_PROPERTY |
| `.../ReviewController.h:53-58` | `sourceA/B/CErrorKey`, `sourceA/B/CMissing` Q_PROPERTY |
| `.../ReviewController.h:122-124,146-151` | accessors |
| `.../ReviewController.h:209` | `applyAlignmentOffsets(sourceAFrames,sourceBFrames,sourceCFrames)` |
| `ui_qml/src/ReviewController.cpp:167-169` | `sourceAFilename/B/CFilename` fields |
| `.../ReviewController.cpp:188-193` | `sourceA/B/CErrorKey`, `sourceA/B/CMissing` fields |
| `.../ReviewController.cpp:224-225` | equality |
| `.../ReviewController.cpp:403-409` | slot-indexed error key assignment |
| `.../ReviewController.cpp:527-538` | `applyAlignmentOffsets` impl (array of 3) |
| `.../ReviewController.cpp:843-845,881-885` | per-slot filename |
| `.../ReviewController.cpp:980-984` | `sourceId==0/1/2` → A/B/C missing |
| `.../ReviewController.cpp:1186-1219` | per-slot error key |
| `.../ReviewController.cpp:1297,1337-1339` | sourceCount + missing copy |
| `.../ReviewController.cpp:1529-1642` | all accessors |
| `.../ReviewController.cpp:1860-1863` | `applyAlignmentOffsets` public |

### QML `sourceCount === 3` / `> 2` slot literals

| File:line | Usage |
|---|---|
| `ui_qml/qml/ComparisonToolbar.qml:47` | `model: sourceCount>=3 ? ["Source A","Source B","Source C"] : ["Source A","Source B"]` |
| `ui_qml/qml/CompareModeBar.qml:70,107,116,125` | `sourceCount === 3` gating |
| `ui_qml/qml/ApplicationMenuBar.qml:60,130-131,154,166-167,199,219` | `sourceCount === 3` / `< 3` / `> 2` |
| `ui_qml/qml/ReviewContextMenu.qml:19,96-97,139` | `sourceCount === 3` |
| `ui_qml/qml/TabbedInspector.qml:215` | `sourceCount === 3` |
| `ui_qml/qml/ReviewShortcuts.qml:220` | `sourceCount < 3` |
| `ui_qml/qml/Main.qml:94-97` | `sourceA/B/CErrorKey`, `sourceAMissing` |

### Renderer slot literals (0/1/2)

| File:line | Usage |
|---|---|
| `platform_windows/src/D3d11ComparisonRenderer.cpp:564,586,603,633` | `result.sourceCount = 1U/2U/3U` |
| `.../D3d11ComparisonRenderer.cpp:626-630` | `sourceSlots[0]=referenceSlot`; loop `slot<3U` |
| `.../D3d11ComparisonRenderer.cpp:722-723` | `sourceContentRects[0U]/[1U]` |
| `.../D3d11ComparisonRenderer.cpp:1214-1216` | `set.find(0U)/find(1U)/find(2U)` |
| `presentation_contract/include/dvs/presentation/ComparisonContract.h:95` | `uint8_t referenceSlot = 0U` |
| `presentation_contract/src/ComparisonContract.cpp:54` | `referenceSlot < 3U` |
| `ui_qml/src/ComparisonSurface.cpp:59,803` | `referenceSlot_` (validated `0..2`) |

**Surface 3 count:** ~120 sites across 12 files. The plan's list (validator/arrays/QML/DifferenceEdge/
renderer) is **correct but incomplete**: it omits (a) the `sourceA/B/C` façade property cluster
in ReviewController (~50 sites), (b) the `referenceSlot < 3U` contract in ComparisonSurface +
ComparisonContract, and (c) `RationalRate.cpp`'s unrelated `std::array<int64,3>` (not a source
count, but a grep trap).

---

## Surface 4 — QML media-state ownership

Places QML owns media truth the plan wants moved to Application. The plan (02 §7) lists these;
exact blocks below.

### Range loop authority in QML

| File:line | Usage |
|---|---|
| `ui_qml/qml/Main.qml:76,80` | `outFrame`, `rangeStartPending` properties |
| `ui_qml/qml/Main.qml:267-284` | **`onCurrentFrameChanged`** — the range loop state machine (`setRangeStartPending`, `seekFrame(inFrame)`, `play()`) |
| `ui_qml/qml/Main.qml:337-353` | `playSelectedRange()` |
| `ui_qml/qml/Main.qml:360-366` | `remapReviewRange()` |
| `ui_qml/qml/Main.qml:372-396` | `toggleRangeLoop()`, `stopRangeLoop()` |
| `ui_qml/qml/Main.qml:809,1167,1286` | `outFrame` binding out |
| `ui_qml/include/dvs/ui/ReviewShellController.h:46,50,124,128,151,154,214,218` | `outFrame`, `rangeStartPending`, `remapRange`, `setRangeStartPending` Q_PROPERTY/invokable |
| `ui_qml/src/ReviewShellController.cpp:186-516` | range state + `setRangeStartPending` impl |

### Pair normalization (ordinal edge → label list)

| File:line | Usage |
|---|---|
| `ui_qml/src/ReviewController.cpp:1276-1294` | **builds** `differenceEdges` with ordinal `preferenceValue` and `firstSourceId/secondSourceId` |

### Alignment status composition (string building in QML)

| File:line | Usage |
|---|---|
| `ui_qml/qml/Main.qml:109-114` | `frameMappingStatus`, `alignmentEstimateStatus`, `sequenceAlignmentStatus`, `alignmentAnalysisStatus`, `manualAnchorStatus` properties |
| `ui_qml/qml/Main.qml:125-134` | **composes** combined status string from all alignment parts |
| `ui_qml/qml/Main.qml:239` | `anyManualAlignmentActive` |
| `ui_qml/qml/Main.qml:779` | `alignmentAnalysisRunning` binding |
| `ui_qml/qml/Main.qml:1051-1061` | manual anchor dialog text |
| `ui_qml/include/dvs/ui/ReviewController.h:60-77` | alignment Q_PROPERTY cluster (8 props) |

### Preview cache lifecycle (QML-owned)

| File:line | Usage |
|---|---|
| `ui_qml/qml/TimelineThumbnailCache.qml:6,40-83` | `cache` id, `capture(frame)`, `grabToImage`, LRU eviction, `onCurrentFrameChanged: capture(currentFrame)` |

### Comparison-availability computation (QML-owned)

| File:line | Usage |
|---|---|
| `ui_qml/qml/Main.qml:151-194` | **`availableViewModes`** computed from `sourceCount` (1 / 2 / 3 branching) |
| `ui_qml/qml/Main.qml:195-200` | `analysisGridMode`, `differenceMode`, `wipeMode`, `sideBySideMode`, `threeUpMode` derived props |
| `ui_qml/qml/ComparisonToolbar.qml:68-73` | `availableViewModes` model + index lookup |

### Shortcut action definitions (QML-owned)

| File:line | Usage |
|---|---|
| `ui_qml/qml/ReviewShortcuts.qml:9-14` | `shortcutsEnabled`, `presentationShortcutsEnabled`, `shortcutPreset` |
| `ui_qml/qml/ReviewShortcuts.qml:50-157+` | ~20 `Shortcut` blocks defining play/step/seek/wipe key bindings incl. preset-1 `stepSeconds(±30)` |

### Error-detail composition (QML-owned)

| File:line | Usage |
|---|---|
| `ui_qml/qml/Main.qml:108,138` | `pairErrorKey`, `hasErrors` |
| `ui_qml/qml/Main.qml:512` | `dropError = messageCatalog.droppedUrlError(...)` |
| `ui_qml/qml/Main.qml:736-745` | **`errorDetails()`** — composes "Source A/B/C: ..." + "Comparison: ..." strings |
| `ui_qml/qml/Main.qml:760` | per-finding severity string |
| `ui_qml/qml/Main.qml:877-886` | intent error message composition |
| `ui_qml/qml/Main.qml:1225` | `errorDetail: root.errorDetails()` |

**Surface 4 count:** ~85 sites across 9 files. The heaviest single block is the range-loop
state machine at `Main.qml:267-284` plus its 5 supporting functions.

---

## Surface 5 — Reference/canonical coupling touch points

Every place that reads `canonicalSourceId()` or assumes reference==canonical, **outside**
`ComparisonValidator.cpp` (which defines the coupling). Phase 1 must update all of these.

| File:line | Usage |
|---|---|
| `application/src/PlaybackCoordinator.cpp:1729` | copies `canonicalSourceId()` into FrameSetRequest |
| `application/src/PlaybackCoordinator.cpp:1912` | reads `set.canonicalSourceId()` |
| `application/src/PlaybackCoordinator.cpp:2185` | `offset.sourceId == canonicalSourceId()` (alignment skip) |
| `application/src/PlaybackCoordinator.cpp:2247,2299` | copies into probe/open requests |
| `application/src/PlaybackCoordinator.cpp:2372` | `command.sourceId == canonicalSourceId()` (anchor skip) |
| `application/src/PlaybackCoordinator.cpp:3193,3226` | alignment estimate/result canonical skip |
| `application/src/AlignmentWorkEstimator.cpp:20-23,84,97,125,137` | `findCanonical` + identity checks |
| `application/src/AlignmentCacheIdentity.cpp:106,165` | canonical in cache identity |
| `media_ffmpeg/src/AlignmentAnalysisService.cpp:127,506,518,564,575,591,599,636,648` | canonical in alignment service |
| `media_ffmpeg/src/MultiSourceFrameProvider.cpp:525,760,765,854` | canonical in provider |
| `application/include/dvs/application/Ports.h:45,65,75` | `canonicalSourceId` in 3 structs |
| `ui_qml/src/ReviewController.cpp:860-861` | `canonicalSourceIndex` from `canonicalSourceId()` |
| `ui_qml/src/ReviewController.cpp:878` | `referenceSourceIndex` from `source.role==kReference` |
| `ui_qml/src/ReviewController.cpp:931` | `source.id == canonicalSourceId()` |
| `ui_qml/src/ReviewController.cpp:424-425` | role assignment: `index==referenceSourceIndex ? kReference : kPrediction` |
| `ui_qml/src/ReviewShellController.cpp:67-68` | `canonicalSourceIndex()` |
| `ui_qml/src/ReviewShellController.cpp:110-113` | `identities[canonicalSourceIndex_]` |
| `ui_qml/src/ReviewShellController.cpp:317` | `int reference = canonicalSourceIndex_` (**assumes ref==canonical**) |
| `ui_qml/src/ReviewShellController.cpp:340,349` | `sourceIndex == canonicalSourceIndex_` |
| `ui_qml/src/ReviewShellController.cpp:529-534` | canonical index tracking |
| `ui_qml/src/ReviewShellController.cpp:696,745` | canonical index in intent projection |
| `ui_qml/qml/Main.qml:59-60` | `referenceSourceIndex`, `canonicalSourceIndex` properties |
| `ui_qml/qml/Main.qml:679-680` | `canonicalSourceId = canonicalSourceIndex>=0?...:0` |
| `ui_qml/qml/Main.qml:291-294` | `onCanonicalSourceIndexChanged` → `remapReviewRange` |
| `ui_qml/qml/ActiveSourceStrip.qml:77` | `isCanonical` from `canonicalSourceIndex` |
| `ui_qml/qml/ApplicationMenuBar.qml:203-220` | `canonicalSourceIndex === 0/1/2` |
| `ui_qml/qml/ReviewContextMenu.qml:122-140` | `canonicalSourceIndex === 0/1/2` |
| `ui_qml/qml/ComparisonToolbar.qml:48,52` | `canonicalSourceIndex` combo |
| `ui_qml/qml/ComparisonViewport.qml:140` | `referenceSlot: referenceSourceIndex>=0?...:0` |
| `ui_qml/qml/TabbedInspector.qml:234` | `referenceSourceIndex` combo |
| `ui_qml/qml/AlignmentInspector.qml:82` | `referenceSourceIndex` gating |

**Surface 5 count:** ~75 sites across 14 files. The critical "reference==canonical" assumption is
at `ReviewShellController.cpp:317` (`int reference = canonicalSourceIndex_`) and
`ReviewController.cpp:424-425` (role derived from index equality). These are exactly the sites
that silently break once Reference can differ from canonical.

---

## Surface 6 — Façade surface (Q_PROPERTY enumeration)

The plan wants 6 narrow ViewModels. Every Q_PROPERTY on the two façade controllers is listed
below with its recommended migration target.

### ReviewController Q_PROPERTY (51 properties)

| Line | Property | Target ViewModel |
|---|---|---|
| 30 | `sourceAFilename` | **Session** (or Source list model) |
| 31 | `sourceBFilename` | **Session** |
| 32 | `sourceCFilename` | **Session** |
| 33 | `sourceUrls` | **Session** |
| 34 | `activeSources` | **Session** |
| 35 | `sources` (QAbstractItemModel) | **Session** |
| 36 | `sourceCount` | **Session** |
| 37 | `canonicalSourceIndex` | **Timeline** |
| 38 | `referenceSourceIndex` | **Comparison** |
| 39 | `displayState` | **Shell** |
| 40 | `busy` | **Shell** |
| 41 | `framePending` | **Playback** |
| 42 | `playing` | **Playback** |
| 43 | `graphicsReady` | **Diagnostics** |
| 44 | `currentFrame` | **Playback** |
| 45 | `totalFrames` | **Timeline** |
| 46 | `oneSecondStepFrames` | **Timeline** |
| 47 | `currentMediaTime` | **Playback** |
| 48 | `currentTimecode` | **Timeline** |
| 49 | `rationalFrameRate` | **Timeline** |
| 50 | `timingMode` | **Timeline** |
| 51 | `dropFrameTimecodeAvailable` | **Timeline** |
| 52 | `sourceMediaInfo` | **Session** |
| 53 | `sourceAErrorKey` | **Session** |
| 54 | `sourceBErrorKey` | **Session** |
| 55 | `sourceCErrorKey` | **Session** |
| 56 | `sourceAMissing` | **Session** |
| 57 | `sourceBMissing` | **Session** |
| 58 | `sourceCMissing` | **Session** |
| 59 | `pairErrorKey` | **Comparison** |
| 60 | `frameMappingStatus` | **Comparison** |
| 61 | `alignmentEstimateStatus` | **Alignment** |
| 62 | `sequenceAlignmentStatus` | **Alignment** |
| 63 | `alignmentAnalysisRunning` | **Alignment** |
| 64 | `alignmentAnalysisProgress` | **Alignment** |
| 65 | `alignmentAnalysisStatus` | **Alignment** |
| 66 | `manualAnchorStatus` | **Alignment** |
| 67-69 | `alignmentTimelineMarkerOverflowCount` | **Alignment** |
| 71 | `manualAnchorActive` | **Alignment** |
| 72 | `autoAlignmentActive` | **Alignment** |
| 73 | `alignmentRequired` | **Alignment** |
| 74 | `automaticAlignmentPending` | **Alignment** |
| 75-77 | `canUndoAutomaticAlignment` | **Alignment** |
| 78 | `compatibilityFindings` | **Comparison** |
| 79 | `differenceEdges` | **Comparison** |
| 80 | `canOpen` | **Shell** |
| 81 | `canFirst` | **Playback** |
| 82 | `canPrevious` | **Playback** |
| 83 | `canNext` | **Playback** |
| 84 | `canLast` | **Playback** |
| 85 | `canPlay` | **Playback** |
| 86 | `canPause` | **Playback** |

### ReviewShellController Q_PROPERTY (26 properties)

| Line | Property | Target ViewModel |
|---|---|---|
| 23 | `activeSources` | **Session** |
| 24 | `stagedSources` | **Session** |
| 25 | `canonicalSourceIndex` | **Timeline** |
| 26 | `activeGeneration` | **Diagnostics** |
| 27 | `effectiveViewMode` | **Comparison** |
| 28 | `effectiveDifferenceEdge` | **Comparison** |
| 29 | `comparisonEdgeAvailable` | **Comparison** |
| 30-31 | `stagedReferenceIndex` | **Comparison** |
| 32 | `queuedIntentCount` | **Shell** |
| 33 | `queuedIntents` | **Shell** |
| 34 | `activeIntent` | **Shell** |
| 35 | `activeSourceIdentities` | **Session** |
| 36 | `canonicalSourceIdentity` | **Timeline** |
| 37 | `pendingSourceIdentities` | **Session** |
| 38 | `pendingSourceIndexes` | **Session** |
| 39 | `chromeVisible` | **Shell** |
| 40-41 | `hasPendingAction` | **Shell** |
| 43 | `pendingAction` | **Shell** |
| 44 | `openIntent` | **Shell** |
| 45 | `inFrame` | **Timeline** |
| 46 | `outFrame` | **Timeline** |
| 47 | `inMediaTime` | **Timeline** |
| 48 | `outMediaTime` | **Timeline** |
| 49 | `rangePlaybackActive` | **Playback** |
| 50 | `rangeStartPending` | **Playback** |

**Surface 6 count:** 51 + 26 = **77 Q_PROPERTY** across two controllers. Distribution:
Session 14, Timeline 9, Comparison 10, Playback 9, Alignment 10, Shell 10, Diagnostics 2
(+ 3 unclassified). The plan's proposed 6 ViewModels (Session/Playback/Timeline/Comparison/
Alignment/Diagnostics/Shell) cover the clusters, but note the **sourceA/B/C** properties (7 of
them) are individually-mapped slot projections that must collapse into a single source list
model, not 7 separate properties.

---

## Total counts per surface

| Surface | Sites | Files |
|---|---|---|
| 1. SourceId-as-slot | ~95 | 18 |
| 2. DifferenceEdge | ~110 | 14 |
| 3. Hard-coded 3-source | ~120 | 12 |
| 4. QML media-state ownership | ~85 | 9 |
| 5. Reference/canonical coupling | ~75 | 14 |
| 6. Façade Q_PROPERTY | 77 | 2 |
| **Total** | **~562** | — |

---

## Phase 1 touch estimate

Phase 1 (Comparison Semantics V2) must change:

- **Surface 1 — all of it.** `canonicalSourceId()` is the coupling being broken; `find(SourceId)`
  and every 0/1/2 literal chain must become `SourceKey`/`SourceSlot`. The 14 `canonicalSourceId()`
  C++ call sites in application/ and media_ffmpeg/ are mandatory Phase 1 updates.
- **Surface 2 — all of it.** DifferenceEdge is the ordinal being replaced by `ComparisonPair`.
  The enum can remain as a renderer-internal slot adapter, but the preference property, the
  `differenceEdges` list, the QML surfaces, and the persistence key must all migrate in Phase 1.
- **Surface 3 — validator + arrays + QML + renderer slot logic.** `kMaximumSources` and the
  `std::array<*,3>` storage become the `RenderSlotMapping`/vector equivalents. The renderer's
  frameA/B/C + `sourceSlots[3]` + `differenceEdge` remap is the deepest Phase 1 site.
- **Surface 4 — pair normalization + comparison-availability only.** The `differenceEdges`
  builder (ReviewController.cpp:1276-1294) and `availableViewModes` (Main.qml:151-194) must move
  to C++. Range loop, alignment strings, preview cache, shortcuts, and error composition are
  Phase 2 (state ownership).
- **Surface 5 — all of it.** Every `canonicalSourceId()` consumer and every reference==canonical
  assumption (esp. ReviewShellController.cpp:317, ReviewController.cpp:424-425) is Phase 1.
- **Surface 6 — design only.** Phase 1 adds the new ViewModels alongside the existing façade
  (parity assertions); property deletion is Phase 2.

**Surfaces fully in Phase 1:** 1, 2, 3, 5. **Partially in Phase 1:** 4 (pair + availability).
**Phase 2+:** 4 (range/preview/shortcuts/errors), 6 (narrowing).

---

## Migration hazards the plan does NOT mention

1. **Renderer slot-to-frame resolution is by stable source ID, not vector position.** The
   comment at `D3d11ComparisonRenderer.cpp:1211-1213` explicitly warns: *"GPU transfer omits
   Missing entries, so vector position is not the source slot. Resolve by the stable source IDs
   assigned at open time or a missing source would shift every later panel to the left."*
   The plan's `RenderSlotMapping` must preserve this `set.find(0U/1U/2U)` semantics. If the
   migration naively replaces the slot array with a vector indexed by position, a missing
   source silently shifts panels — a regression the plan does not flag.

2. **Persistence schema migration is string-valued, not integer.** The plan (07 §1.3) says
   "schema 1: differenceEdge = 0/1/2" but the actual stored values are the strings
   `"0-1"`, `"0-2"`, `"1-2"` (ReviewPreferencesController.cpp:458-460, 534-544). A migration
   that reads the integer `static_cast<int>` gets the wrong edge. The schema also currently
   requires `schemaVersion == 1` exactly (SettingsRepository.cpp:70-73); bumping to 2 must
   keep reading v1.

3. **`referenceSlot` is a separate 0..2 ordinal that must NOT be confused with DifferenceEdge.**
   It appears in `ComparisonContract.h:95`, `ComparisonSurface.h:61/232`, and the renderer
   (`referenceSlot < 3U`, `referenceSlot > 2U`). The plan focuses on DifferenceEdge but the
   ReferenceFocus layout (`computeReferenceFocusLayout`, lines 619-634) reorders `sourceSlots`
   by `referenceSlot` independently of the edge. Two distinct slot-orderings coexist; the plan
   does not disambiguate them.

4. **CliMain.cpp hard-codes `SourceId{0}`/`SourceId{1}`** (lines 55, 60) for the CLI compare
   path. This is a parallel entry point that bypasses the QML façade and would need its own
   SourceKey migration — unmentioned in the plan.

5. **`sourceName(sourceRows[x].sourceId)` maps id→"A/B/C" labels** (ReviewController.cpp:1281).
   The label alphabet is tied to slot position, not identity. When the `differenceEdges` list is
   rebuilt from a `ComparisonPair`, the label generation must move to C++ and use stable names
   (filename/alias), else labels still drift after reorder.

6. **The `sourceA/B/C` façade properties are individually projected from a single snapshot
   with `sourceId==0/1/2` chains** (ReviewController.cpp:979-984, 1214-1219). These are not
   independent properties; they are three views of one array. Collapsing them into a source list
   model requires updating every QML binding (`Main.qml:94-97`, `ApplicationMenuBar.qml`,
   `ReviewContextMenu.qml`, `ComparisonToolbar.qml`) — a wider blast radius than "delete
   DifferenceEdge" suggests.

7. **`RationalRate.cpp:100` `std::array<int64,3>` is rate-factor math, not a source count.**
   It is a grep trap for any automated "replace array-of-3" tooling. The plan should call it
   out as out-of-scope to avoid a mechanical refactor breaking rate multiplication.

8. **`availableViewModes` is computed in QML from `sourceCount`** (Main.qml:151-194) using
   `ComparisonSurface.Single/SideBySide/...` C++ enum values. Moving comparison-availability to
   Application requires either exposing the C++ `isViewModeAvailable`/`effectiveViewMode`
   (ComparisonContract.cpp:66-75) to QML or a computed ViewModel property — the plan lists this
   as a move but does not note the QML-side enum dependency.

---

## Verdict — sufficiency of the plan's proposed types

The plan proposes: `SourceKey`, `SourceSlot`, `RenderSlotMapping`, `ComparisonSelection`,
`TimelineSelection`, `ComparisonPair`, `FocusSelection`, `DefaultPairPolicy`,
`ActivePresentationSet`, `FrameSet.visible`, `OperationIdentity`, `ComparisonProvenance`.

**These types are necessary and largely sufficient to express every current usage found**, with
the following gaps:

- **Missing: a `SourceFingerprint` migration for the renderer's stable-ID resolution.** The
  renderer resolves frames by "stable source IDs assigned at open time" (line 1212). The plan
  defines `SourceFingerprint` for cross-session file identity but does not specify how the
  renderer's per-open stable ID (`SourceSlot`) maps to a `GpuFrameSlot` without reintroducing
  a slot index. A `RenderSlotMapping::sourceToSlot` (which the plan does define) covers this,
  but the renderer's `set.find(0U/1U/2U)` must become `mapping.find(sourceKey)` — a non-trivial
  rewrite the plan understates.

- **Missing: explicit handling of the `referenceSlot` vs `DifferenceEdge` dual ordering.**
  `ComparisonSelection` has `reference` + `activePair` but no field for "which slot is the
  reference in the ReferenceFocus layout." The renderer computes this from a separate
  `referenceSlot` (0..2). Either `RenderSlotMapping` must encode reference position, or a
  `LayoutOptions` value type is needed.

- **Missing: a typed status/error DTO.** Surface 4's error-detail composition and alignment
  status strings (Main.qml:125-134, 736-745) are QML string concatenation. The plan's
  `DiagnosticItem` (03 §9) is the right target but is not wired into the ViewModel split or the
  Phase 1/2 roadmap. Without it, "move error composition to Application" has no destination
  type.

- **Sufficient:** `SourceKey`/`SourceSlot`/`RenderSlotMapping` cover all of Surface 1 and 3;
  `ComparisonPair` + `DefaultPairPolicy` cover Surface 2; `ComparisonSelection` +
  `TimelineSelection` + `FocusSelection` cover Surface 5; the ViewModel split covers Surface 6.

**Overall:** the type set is ~90% sufficient. The 3 gaps above (renderer stable-ID resolution,
referenceSlot dual ordering, typed status DTO) are the places where implementation will
discover the plan's types do not yet fully express a current usage. Each is resolvable by
extending `RenderSlotMapping` and adding a `LayoutOptions`/`StatusBadge` value type — neither
requires a new phase, but both should be explicit before Phase 1 implementation begins.
