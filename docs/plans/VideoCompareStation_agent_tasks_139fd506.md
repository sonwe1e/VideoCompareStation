# VideoCompareStation · 下一阶段开发工单

审查对象：`https://github.com/sonwe1e/VideoCompareStation`，分支 `agent/image-folder-comparison`，固定 SHA `139fd5067d582a95beb2bcf99d2586099040a000`。

这是规划，不是已实施变更。开始开发前重新记录实际目标 SHA；若分支有更新，先核对受影响函数和已有测试，不把本审查行号直接套到新版本。

推荐顺序：T0 与 T1/T2 并行；T3 最小输入隔离立即处理；随后 T3→T4/T6；T5 必须有 T0 因果证据；最后 T7。

共同边界：保留 C++/Qt/FFmpeg/D3D11、完整 FrameSet/Missing、代际失效与资源拥有关系；不做全栈重写。每个工单独立交付回归用例与验收记录；不得用删测试、吞错误或静默改图换取通过。

性能表述必须包含环境、SHA、素材、命令与原始结果。没有基线不得填虚构阈值或提升百分比。代码静态推断与运行结果分别标记。

### T0 · P0｜建立正确性复现与播放证据基线

| 字段 | 可执行约定 |
|---|---|
| 目标 | 把“卡顿”和“看错对象”变成固定 SHA、素材与环境下可以重放的问题，不先改变播放行为。 |
| 涉及模块 | `src/app/Main.cpp` 已有性能入口；`tests/hardware/`；`tools/testing/`；`PlaybackTrace` 与 backend 诊断。参见 E16–E19、E21、E26–E28、E33–E37。 |
| 前置依赖 | 无；与 T1/T2 并行 |
| 实施边界 | 复用现有 gates；生成带显式源身份/帧号的确定性素材与图片失败夹具；新增提交和呈现的分离观测；先用短 trace，再汇总长播放。禁止同时调队列、解码、UI，以免失去因果。 |
| 验收 | 保存 SHA、Release 构建参数、设备/显示/磁盘、每源 backend、素材 hash、命令、原始输出与指标 JSON；先测 P1–P4。报告 P50/P95/P99、最长停顿、正确帧组延迟和 UI 事件循环间隙，不只报告平均 FPS。 |
| 成本等级 | 中；不是精确工期承诺 |
| 主要风险 | 日志溢出或缺失时该次证据无效；不同线程时间戳不等于可靠全序；合并预览结果不能混作 head 全栈证据。 |
| 回退 | 诊断默认关闭；有界计数与短 trace；诊断开/关对照确认观测开销。可撤销诊断开关，不改变业务结果。 |

**T0 验收记录（2026-09-16）**

- 交付：证据包 `out\evidence\baseline-20260916-163216`（trace 身份修复前，nav gate 失败，作为"修复前"对照）与 `baseline-20260916-170049`（修复后）、`baseline-20260916-172928`（修复后、最终格式化源码构建；两次独立运行作为可复现性对照）。每包含 environment.json（git SHA 139fd5067d582a95beb2bcf99d2586099040a000、Release 构建参数、CPU/内存/GPU/磁盘/显示、exe sha256、夹具 contract sha256）、每 run 的 command.txt/stdout/stderr/metrics.json/trace.jsonl/trace-timing.json、summary.md 与 crosscheck。
- P1–P4 结果：短/长 1–3 路 1080p60 全部 passed、drop_ratio=0、display_interval P50=16–17ms（60fps 节奏）、P95=49–75ms、P99=51–85ms、最长停顿 58–85ms；ui_loop_gap P50=1ms/P95=2ms/P99=2ms（tail 50–74ms 偶发）；seek P95 91–221ms。trace 侧提交/呈现分离：display P50≈16.8ms、ready→commit P50≈16.7ms、publish→ack P50≈0.6–0.8ms、ack→commit P50=4µs。P2 显示路径 side/wipe 通过；diff 模式复现既有问题 `comparison-mode-black-output`（退出码 1，见基线 summary）。P4 图片 17 项夹具行哈希全部匹配契约。
- 导航 gate（P3 前置）最终全绿：`NAVIGATION_GATE_TRACE_OK … command_terminal_mismatch=0 ack_before_commit_violations=0 stale_commit=0 stale_arrival_published=0 partial_frame_set=0`（两次独立运行一致）。
- 观测层修复（仅 trace 事件身份，不改变播放行为）：命令终态改携 `sessionEpoch`（消除 open 后旧命令终态孤儿）；`SnapshotCommitted` 对未变化 displayed 帧去重（消除跨代重提交无法表示的 ack-before-commit）；trace 容量 16K→64K 且 producer 锁改为阻塞（单事件拷贝，纳秒级，仅容量耗尽才丢事件）。文档 `docs/engineering/trace-schema.md` 同步更新。
- 质量门：dev 全量 ctest 512/512 通过；format-check、lint 通过；PlaybackTraceGate PS 单测 42 断言通过。观测开销：诊断默认关闭；本次基线即带诊断运行，未引入 drop。
- 遗留：P2 diff 模式 black-output 属 T0 已复现的既有问题，不阻塞 T0 交付；显示适配器为虚拟/间接（GameViewer/Oray）时 app 上报 120/144Hz 波动，cadence 硬件权威以环境中的 NVIDIA RTX 4090 @120Hz 为准；4K 不在当前矩阵。


### T1 · P0｜图片配对原子加载与选择提交

| 字段 | 可执行约定 |
|---|---|
| 目标 | 无论 A/B 哪一侧损坏，都不会把上一组内容混成新比较；列表选中身份与画布身份始终一致。 |
| 涉及模块 | `DesktopApplication::setPairOpener` 连接；`ImageFolderPairModel::openPairAt`；`ImageReviewController`；`FolderPairSidebar::selectRow`；Main 图片入口。E05、E07–E10。 |
| 前置依赖 | 先定义 pending/committed 身份；无需等 T0 完成 |
| 实施边界 | 用返回实际结果的同步事务或显式异步 completion 取代 void opener；构造 CandidatePair，全部校验完成再一次提交。第一步可保持同步但必须正确，为 T4 留 generation/token。禁止先提交 A 再提交 B。 |
| 验收 | 夹具：旧 A0/B0，换 A1/B1，依次让 A1 损坏、B1 损坏、缺失/超限；验证 committedPairId、两侧路径/像素、当前列表行完全保留上一对或进入明确新缺失状态，不出现混合；新请求成功时身份一次切换；最后候选失败仍有重试/返回路径。 |
| 成本等级 | 中；不是精确工期承诺 |
| 主要风险 | 现有单独替换一侧功能仍需保留，不能通过每次 closeAll 绕过原子语义、丢失全部上下文；错误文本不得被后续 setCompareMode 覆盖而失去失败来源。 |
| 回退 | 必要时关闭文件夹批量入口，保留安全的单图/已提交配对查看；不得回退到忽略加载失败的新旧混图行为。 |


### T2 · P0｜原图不可变、尺寸/像素语义与真实 100%

| 字段 | 可执行约定 |
|---|---|
| 目标 | 明确用户看到的是原图、显示缩放，还是经过用户允许的派生变换。 |
| 涉及模块 | `ImageReviewController::recomputeDifference/loadChecked/displayImage/samplePixel`；`ImageWorkspace` 的 baseScale/drawScale；`StillImageDecoder`；图片测试。E05、E14、E15、E30。 |
| 前置依赖 | 可与 T1 并行；提交时需共用配对身份 |
| 实施边界 | 分别保存 originalA/originalB 与派生结果。默认不等尺寸只保持比例查看，未选择变换不计算逐像素差异。100% 明确为图像像素→物理屏幕像素，适应窗口单独命令；DPR 纳入显示倍率。明确 RGBA8、alpha、RGB 差异统计的现行范围，不在此工单实现完整 ICC/HDR 管线。 |
| 验收 | 使用有网格与不同比例的 A/B：反复换模式/更换 A 后，B 原始尺寸与字节不变；未选变换时无差异数值假装有效；派生重采样有持续标签及原图/派生取样来源；DPI 100/125/150/200% 验证 100% 映射与适应有区别；alpha-only/16-bit 输入不误报为原码值完全相等。 |
| 成本等级 | 中；不是精确工期承诺 |
| 主要风险 | 旧 meanAbsDifference 实际是每像素 RGB 最大差的平均，不可在重命名时悄悄改变含义；改变算法需要单独标明版本与测试；界面枚举/持久化值保兼容。 |
| 回退 | 出问题先禁用不等尺寸差异和未验证精确模式，保留正确并排，不回滚为静默拉伸；高级派生变换可整体隐藏。 |

**T1 验收记录（2026-09-16）**

- 实现：`ImageReviewController::openPairAtomically/openPairImages`（两侧重解码与校验全部完成后一次性提交，成功时 committedPairId 与 generation 单次切换）；`ImageFolderPairModel::openPairAt` 改为 opener 返回失败原因、仅成功才推进 currentPair；`DesktopApplication` 注入新签名 opener；`Main.qml` 双图入口/文件夹首行/追加路径全部走原子语义且仅成功才切模式；`FolderPairSidebar::selectRow` 失败保持选中。`setCompareMode` 失败分支不再覆盖既有错误来源。
- 新增组件测试（`ImageReviewControllerTests` 8 项、`ImageFolderPairModelTests` 1 项）：A1/B1 损坏、缺失、超限、首对失败进显式缺失态、closeAll 重置身份、成功切换 identity 一次（generation +1）、失败后重试路径。`MainQmlContractTests::ImageFolderComparisonLoadsSidebarAndOpensFirstPair` 端到端通过。
- 冒烟（release 实机，`--ui-image-folder`）：pair_count=10、passed:true；单侧行 opened:false 时画布保持上一对（证据即 T1 语义）；cancel/reopen 探针通过。
- 遗留：pending/committed 双身份与 token 化（为 T4 预留的 generation/token 接口）在 T4 时补充；当前 committedPairId 语义为"最后原子提交的行"，单侧替换不改变它（有意为之，已记录）。

**T2 验收记录（2026-09-16）**

- 实现：`recomputeDifference` 不再改写 `secondary_`（副本上换算）；不等尺寸默认不计算逐像素差异（`hasDiffResult=false`、`diffResampled` 标签、错误信息给出双方尺寸），显式 `resampleAllowed` 后才在副本上重采样并持续标注"已重采样对齐"；`diffScopeText` 声明"解码后 RGBA8；差异统计为 RGB（不含 alpha）"；alpha-only 差异单独检测（`alphaDifferenceOnly`）不再误报相等；`samplePixel` 输出 source=original|diff；ImageWorkspace 新增"适应窗口/100% 真实尺寸"双命令（trueSize = 1 图像像素→1 物理像素，baseScale=1/DPR，状态栏显示物理百分比与模式），差异视口仅在 hasDiffResult 时显示并提供未重采样提示；`--ui-image-folder` 探针记录 has_diff_result/diff_resampled/alpha_difference_only。
- 新增组件测试：B 原始尺寸与字节在反复换模式/换 A 后不变；不等尺寸无重采样无假差异值、开启后 diffResampled=true 且峰值>0；alpha-only 不报相等；取样来源标签。全部通过。
- DPI 映射：100%（DPR=1.0）→ baseScale 1；125%→0.8；150%→0.667；200%→0.5，与适应窗口的 fit 值可区分（qmllint/format 已校验）。
- 遗留：16-bit 原码值不保留（解码即 RGBA8，范围声明在 diffScopeText）；meanAbsDifference 语义保持不变（每像素 RGB 最大差的平均），未改名。


### T3 · P0/P1｜工作区状态与命令路由收拢

| 字段 | 可执行约定 |
|---|---|
| 目标 | 打开/取消/切模式有同一语义；当前画布与隐藏任务绝不竞争媒体动作。 |
| 涉及模块 | `Main.qml` 路由、原生选择器、焦点函数；`ReviewShortcuts` / `ReviewActions`；视频/图片控制器的薄适配；可引入小型 WorkspaceSessionState。E10–E14、E35。 |
| 前置依赖 | 最小输入隔离立即做；完整迁移依赖 T1 的提交模型 |
| 实施边界 | 集中 activeMedia、pendingOpen、committedSession、inputContext。选择器打开不覆盖已提交工作区；切离视频先暂停；轻量保留会话位置，不承诺所有纹理常驻；只共享命令语义，不把两种控制器合成万能类。 |
| 验收 | 视频暂停/播放各执行一次：打开图片选择器后取消，模式与来源/位置不变；进入文件夹再返回视频，恢复明确暂停状态；图片画布方向键只换配对，视频画布方向键只逐帧；文本框、模态、菜单内输入不触发隐藏视频；Ctrl+W 仅关闭当前任务；焦点边框与实际按键接收者一致。 |
| 成本等级 | 中；不是精确工期承诺 |
| 主要风险 | 入口包括菜单、Ctrl+O/Shift+F、拖放、最近项、直接替换；不能只修其中一个。导入散图后旧文件夹键盘导航必须清除或显式脱离。 |
| 回退 | 逐入口切到新的 route adapter，保持旧控制器 API；保留完整契约测试。回退 UI 呈现可以，工作区输入隔离和取消不破坏状态不允许撤销。 |


**T3 验收记录（2026-09-16）**

- 实现：`Main.qml` 新增 `workspaceSession`（activeMedia / pendingMedia / committedMedia / committedIdentity / committedRevision），所有入口先 `beginWorkspaceOpen()` 记录意图，只有实际提交成功后才 `commitWorkspace()` 并经 `showWorkspace()` 切换视图；打开或取消选择器不再改写已提交工作区。`activateWorkspace()` 在离开视频时暂停，也在返回视频时强制回到明确暂停态；`commitWorkspace(image)` 不会关闭视频会话，返回后 sourceCount、currentFrame 与暂停后的位置均保留。原生图片/Folder 对话框取消路径接入 `cancelWorkspaceOpen()`，拖放确认框取消会清理 staged 源。

- 输入隔离：`globalMediaShortcutsEnabled` 增加 `workspaceSession.videoActive`，图片工作区不再收到 Space/方向键/I/O 等视频命令；`inputContext` 把原生图片选择器纳入 modal 级（3），文本框/菜单/popup 仍按 1/2/3 分级。根窗口新增 Ctrl+O / Ctrl+Shift+O / Ctrl+I / Ctrl+Shift+I / Ctrl+Alt+I / Ctrl+W / Ctrl+Shift+F 路由，`ReviewShortcuts` 中重复的 Ctrl+O/Ctrl+Shift+O/Ctrl+W 删除。焦点通过 `focusActiveWorkspace()` 跟随可见工作区，ImageWorkspace 可见时主动接管焦点。

- 任务关闭：Ctrl+W 以 activeTaskHasMedia 为门禁；图片任务只 closeAll() 并清理文件夹导航，视频任务才提交 CloseSession；关闭当前任务后若有另一侧已提交任务则切回该任务。针对 Shell 缓存 Active Sources 与媒体真相短暂不同步的竞态，仅在 shell 侧为空而 controller 仍有源时回退到 controller 关闭，避免隐藏视频任务残留。File 菜单关闭项按工作区动态显示为关闭图片工具或关闭视频。

- 文件夹导航脱离：ImageFolderPairModel::clear() 一次性清空行、currentPair、左右路径与错误；直接导入散图成功后 detachFolderSession() 调用它，避免旧文件夹列表继续响应方向键。双击/单图替换、双图原子提交、文件夹首行提交仍走既有校验路径。

- 测试：新增 WorkspaceOpenIntentDoesNotOverrideCommittedWorkspace、WorkspaceSwitchPausesAndRetainsVideoSession、ImageWorkspaceArrowsDoNotDriveHiddenVideo、CloseCurrentTaskOnlyClosesActiveWorkspace 四项 QML 契约测试，ImageFolderPairModelTests::ClearDetachesFolderSessionAndSelection，并扩展现有文件夹端到端用例验证散图导入后方向键导航已脱离。开发全套 ctest --preset dev 519/519 通过（1 项既有 disabled），qmllint --max-warnings 0 与 format-check 通过。

- 遗留：图片打开失败时若此前图片画布已有内容，performImageReview 单图路径仍会先 closeAll() 再加载（此前行为，见 T4 的后台加载/失败保留设计）；选择器取消不会修改任何已提交状态，但最近项入口当前产品尚不存在，未涉及。

### T4 · P1｜图片后台加载与按需派生计算

| 字段 | 可执行约定 |
|---|---|
| 目标 | 读取/解码/整图差异不会占住 UI 主线程，并避免用户没看差异时支付重复整图计算。 |
| 涉及模块 | 图片控制器、解码封装、图像 provider；必要的新 ImagePairLoader/派生缓存；目录模型扫描。E05、E08、E14、E15。 |
| 前置依赖 | T1/T2；先取得图片 T0 阶段耗时与内存峰值 |
| 实施边界 | 有界工作队列；不可变输入/输出；token/generation 拒绝过期结果；Qt QObject 只在所属线程更新。缓存按字节限制，键包含源身份与尺寸/颜色/alpha/变换策略；邻近配对最多少量预读，不预解整个目录。解码前检查头部像素与预算，解码中继续处理异常。 |
| 验收 | 注入可控慢加载器：请求 N→N+1→N+2、取消、关闭、退出；晚到 N 不能污染当前标题/画布；主线程仍可处理取消；仅并排切换不会触发新的整图差异；复用同派生结果有缓存命中；连续循环换图后预算/线程达到稳定区间，延迟与峰值相对基线报告且不回归已约定目标。 |
| 成本等级 | 中；不是精确工期承诺 |
| 主要风险 | 工作线程不可捕获会被销毁的 controller this；避免 provider 被多线程访问时在后台改共享 QImage；缩略预览不得被当成最终精确像素。 |
| 回退 | 可关闭预读和派生缓存；必要时退到尺寸受限、语义仍正确的同步加载，保留 pending/commit 与错误协议；不能恢复原图破坏。 |


**T4 验收记录 (2026-09-16)**

- 交付: 新增 `ImagePairLoader` (`src/ui_qml/include/dvs/ui/ImagePairLoader.h`, `src/ui_qml/src/ImagePairLoader.cpp`) 作为后台加载/派生计算服务. 每控制器一个有界 `QThreadPool` (maxThreadCount=2); 请求带单调 request id 与取消 token; 完成回调经 queued invocation 回到 owner 线程, worker 只接触文件字节与 `QImage` 副本, 不触碰 QObject 状态.
- 原图缓存按字节 LRU (128 MiB), 派生差异缓存按字节 LRU (64 MiB); 键包含文件身份 (路径/大小/mtime/decode policy revision), `QImage` 内容身份, 比较模式, resample 策略与目标尺寸/尺寸差异. 另提供单次邻接预读 `prefetchPair`, 用户请求会取消预读.
- 格式头预检走注入的 FFmpeg header probe, 另加 `ImageHeaderProbe.h` 对 PNG/JPEG/GIF/BMP/WebP 的轻量兜底; 解码前后都检查 8192 边长与 128 MiB 解码预算, 解码器异常被捕获并转为显式失败.
- 控制器 API: 新增 `requestOpenPrimary/requestOpenSecondary/requestOpenPair` (O(1) 接受并返回 request id), `cancelPendingOpen/cancelOpenRequest`, `prefetchPair`, `asyncStats`, `clearAsyncCaches`, `openPending/diffPending` 属性与 `openFinished(requestId,pairId,success,error)` 终态信号. 同步 `openPrimary/openSecondary/openPairAtomically/openPairImages` 保留给测试与非文件来源.
- 每次新候选使旧候选失效; 晚到的 N/N+1 结果在 request id 检查处丢弃; 失败候选不改变 committedPairId, 路径, 原图像素或 generation, 错误来源保留为 "无法打开 A/B: ...".
- 派生差异按需: `openPair*` 与 `SideBySide/Wipe` 不再计算整图差异; 切换差异模式才查派生缓存或提交 worker. 缓存命中同步应用, 未命中时 `diffPending=true`, `hasDiffResult=false` 不伪造结果; `setResampleAllowed` 仅重算差异模式, 尺寸不等未启用重采样时仍给出尺寸提示.
- 原差异算法 (每像素 RGB 最大差, gain=4, alpha-only 标记, RGBA8 范围) 未改名/改义, 仅移入 worker.
- 文件夹路径: `ImageFolderPairModel` 增加 `AsyncPairOpener`/`AsyncPairCancel` 与 `completePairOpen`; `currentPair` 只在匹配 request id 的成功终态推进, 失败/过期终态不改选择. `FolderPairSidebar` 与 `Main.loadFolderComparison` 仍经由该模型.
- Main.qml 引入 pending image request 状态; `performImageReview`, 追加图片, 文件夹首对选择均先 beginOpen, 成功 `openFinished` 后才 commitWorkspace; 取消选择器/关闭任务会调用 `cancelOpenRequest`, 失败保留上一已提交任务. 同步 opener 仍可工作 (`openPending=false` 时立即提交).
- T0 图片探针: `runImageFolderEvidence` 改为异步阶段机 (等待首对, 逐行, 等待差异完成, 取消/重开), 保证 `open_ms` 度量真实完成时间而不是接受时间; 命令与 fixture 不变. 该调整只影响证据采集, 不改变业务逻辑.
- 测试: `dvs_image_review_controller_tests` 新增 6 项 (慢加载 N->N+1->N+2 晚到结果丢弃, 失败保留上一对, 取消/关闭后晚到不发布, 并排不计算/差异缓存命中, 预读命中, 头部预算在解码前拒绝); `dvs_image_folder_pair_model_tests` 新增 2 项 (异步选择只在匹配终态推进, 取消后晚到终态忽略); 既有 T1/T2 差异测试按异步终态等待后语义不变; `MainQmlContractTests` 文件夹端到端改为等待真实提交.
- 开发全套 `ctest --preset dev` 527/527 通过 (7 项既有 disabled); `format-check`, `lint`, `qmllint --max-warnings 0` 通过; Release `--ui-image-folder` 探针 `passed:true`.
- 证据 (Release, 同 T0 fixture; T0 基线见 `out/evidence/baseline-20260916-172928/p4-images/images.json`): T4 原始 stderr/stdout 为 `out/T4-release-image-probe.err` / `.out`. 同一探针对比: `big.png` open_ms 226->76 ms; UI event-loop gap P95 71->2 ms, P99 281->9 ms, max 281->64 ms; peak working set 518,377,472->409,784,320 bytes; diff recompute wall 5->11 ms (包含 worker 调度/等待, UI gap 证明计算不再占用主线程); threads baseline/peak 86/87->86/88; shutdown_ms 49. 结论属于同机同 fixture 的 Release 观测, 不作为硬件 cadence 指标.
- 回退边界: 同步加载 API 与既有原子语义保留; `prefetchPair` 可不用, 缓存可 `clearAsyncCaches`. 未恢复任何直接改写 `secondary_` 或失败混图的旧行为.

**T4 遗留 (供 T6/T5 参考)**

- 邻接预读 API 已有并有缓存命中测试, 但尚未在文件夹换对时自动调度; 自动预读相邻配对与滚动上下文留给 T6 一起处理, 当前每次只加载用户选择的一对.
- 取消运行中的解码只能在其读取/解码边界后丢弃结果; 析构会等待正在执行的 worker 完成, 避免悬空访问. 后续若出现超大图退出时延, 需要在解码循环内加入显式取消检查.
- 未实现 "关闭派生缓存" 的独立用户开关; 当前通过 64 MiB 字节上限和 `clearAsyncCaches` 控制. T5 仍需 T0 因果证据后才允许修改播放调度/缓存参数.
### T5 · P1 条件执行｜视频平滑播放的已定位热点修复

| 字段 | 可执行约定 |
|---|---|
| 目标 | 降低已测得的播放停顿/时间落后，不靠错帧、左右独立丢帧或丢失精确导航命令改善表面 FPS。 |
| 涉及模块 | 只选被证据指向的模块：PlaybackCoordinator 连续策略、actor/cache、TimelineThumbnailCache 或 GPU transfer/render。E16–E23、E26。 |
| 前置依赖 | T0 已证明瓶颈；逐帧/定位正确性基线先通过 |
| 实施边界 | 一次只做一类变化。缩略图若因果成立改成暂停/闲时或低优先级采样；调度若因果成立在现有协调器内区分连续期限与精确命令；大图缓存若因果成立调整字节政策。禁止默认更换 Qt/FFmpeg，禁止延长缓存到无限。 |
| 验收 | 相同机器/素材/显示/构建、至少重复对照，报告帧组显示间隔 P95/P99、最长停顿、时钟落后、整组 drop/duplicate、seek/step 正确性和资源；改进必须超过重复间波动且不破坏既定正确性；接受的单步每个有终态，A/B 来自同一有效映射帧组。 |
| 成本等级 | 中；若需替换隔离模块再评估为大；不是精确工期承诺 |
| 主要风险 | 2000 ms 常量不是已确认根因；render ACK 不是物理显示。连续播放允许整组跳过，但计数必须真实；Exact 不接受最近帧冒充目标。 |
| 回退 | 保留旧策略/模块开关及相同测试 fixture；异常时退旧连续策略。不得把精确导航正确性开关化，也不需要新增用户可见复杂性能模式菜单。 |

**T5 验收记录（2026-09-16）**

- 环境与素材：Release 构建，机器同 T0（i7-13700KF / RTX 4090 / 显示输出为 PCI 卡驱动的 `\\.\DISPLAY1` @120 Hz，exe sha256 见各 `summary.md`；GameViewer/Oray 虚拟适配器均未 attached，不承载窗口）；素材为用户指定视频 `D:\Videos\2026-06-01 23-46-34.mp4`（1080p60 H.264 + AAC，628 帧，10.47 s，sha256 `8c624b3a…`），并以 T0 fixture `frameid_1080p60_a.mp4` 作同机参照。探针要求播放窗口不短于素材，长窗口运行固定 `--seconds 8`（`--ui-performance` 下限 5 s，12 s 会触发 `playback-ended-before-duration`）。原始输出、metrics.json、trace 与对照汇总在 `out\t5-evidence\`。

- 结论：**本轮证据没有指向可归因的播放热点，因此 T5 不修改任何播放调度、缓存或 UI 行为。** 依工单"T5 必须有 T0 因果证据""一次只做一类变化"，未证实的热点不得落地为行为改动。期间实现过一版候选修复（播放期间暂停时间轴缩略图采样），被下述交替对照实验证伪后已完整回退：`git diff` 中不含 `TimelineThumbnailCache.qml` / `Main.qml` 的行为改动。

- 观测层（保留，纯附加、不改变业务结果，与 T0 的 trace 身份修复同类）：trace 新增 `QmlGrabRequested`/`QmlGrabCompleted`(14/15)、`RenderDrawStarted`/`RenderAckPublished`(16/17)、`PlaybackRunStarted`/`PlaybackRunStopped`(18/19)；新增 QML→trace 桥 `DiagnosticsProbe`（未启用 trace 时为一次原子读＋分支）；`TimelineThumbnailCache` 记录抓图请求/完成；`D3d11ComparisonRenderer` 在渲染线程记录起绘与 ACK 入队；`docs/engineering/trace-schema.md` 同步更新。

- 证据工具（`tools/testing/`）：`run-t5-video-evidence.ps1`（视频/fixture 批量运行与归档，支持渲染循环对照与受控环境覆盖）、`run-t5-gate-ab.ps1`（两个可执行文件**逐轮交替**的配对 A/B，用 18/19 号事件切出播放窗口再统计）、`analyze-playback-trace-stages.ps1`（把显示间隔尾部归因到 ready→publish / publish→ack / ack→commit 与 cadence 余量）、`analyze-render-staging.ps1`（归因到 publish→drawStarted / drawStarted→ackPublished）、`compare-t5-evidence.ps1`。

- 关键测量（Release，同素材、同机、重复对照；60 fps 源的显示间隔基准为 16.67 ms）：

  | 观测 | 结果 |
  |---|---|
  | 播放窗口生产速率（`FrameSetReady` 间隔 / 载荷步进） | P50 14.9 ms，载荷步进恒为 +1 → 解码侧按 60 fps 供帧，不是热点 |
  | 播放窗口提交速率 | 59.5–60.1 帧/s（481 帧 / 8.01 s），`drop_ratio=0`，无整组丢失 |
  | 抽帧显示间隔 | P50 14–16 ms、P95 65–155 ms、max 91–236 ms；>40 ms 停顿 1.74–4.24 次/s；各次运行 P95 极差 65–155 ms，**运行间波动大于任何被测效应** |
  | 渲染管线分解 | `published→drawStarted` P95 65–113 ms（全部 303 个 >40 ms 区间的支配项）；`drawStarted→ackPublished` P50 0.01 ms、max 0.38 ms；`ackPublished→acked`、`acked→commit` ≤0.1 ms |
  | 抓图与尖峰的时间相关性 | >30 ms 的 publish→ack 尖峰 43 次中，其后 20 ms 内存在抓图请求者 **0 次** |
  | 渲染循环对照（默认 / `threaded` / `basic` / `QSG_NO_VSYNC=0`） | 提交率 28.3–28.7 帧/s、P95 65–105 ms，四组配置互相落在噪声内 |
  | 交付构建复验（`out\t5-evidence\final-verification`，exe sha256 `e6718644…`） | P50 16 ms（正好一个 60 fps 间隔）、P95 83 ms、P99 86 ms、max 90 ms；`presented_frames=360`、`dropped_frames=0`、`drop_ratio=0`、`render_canonical_gaps=0`、`render_canonical_regressions=0`；14–19 号观测事件全部落盘且无 overflow（14/15=107、16/17=822、18/19 各 1） |

- 证伪实验（本轮核心）：把缩略图采样在播放期间完全关闭，再用 `out\t5-evidence\gate-ab-interleaved` 逐轮交替运行两个可执行文件（sha256 `24aec89b…` 门开 vs `25d18da4…` 门关），各 5 轮。播放窗口内抓图请求 74–80 → **0**（门确实生效），而 >40 ms 停顿为 32/32、33/32、32/14、34/32、32/32，帧率 59.5–60.1 帧/s 双边一致。**抓图被完全移除后停顿剖面不变**，故"播放中抓图造成显示尾部"不成立，该候选改动已回退。

- 剩余瓶颈（未修，转交后续工单）：唯一未被排除的是渲染线程调度——帧已发布、绘制与 ACK 均 <0.4 ms，但场景图在 P95 上 65–113 ms 内没有启动该帧的绘制，且渲染循环配置不是变量。T5 的"改进必须超过重复间波动"验收条件在本轮被测效应面前不成立（见上表：被测效应小于重复间波动）。**追加更正（同日）**：下面这条最初写的"虚拟适配器"理由经复核不成立，特此作废——`.\tools\testing\test-hardware-runner.ps1 -MinimumRefreshRate 120` 在本机输出 `DVS_HARDWARE_RUNNER_READY session=1 physical_adapters=1 refresh_hz=120`；`EnumDisplayDevices` 显示只有 `\\.\DISPLAY1` 处于 attached 状态且由 PCI 卡 NVIDIA RTX 4090 驱动（120 Hz），GameViewer/Oray 的 14 个虚拟输出全部 attached=False，故它们既不出现在 Qt 的 `QGuiApplication::screens()` 中，也不可能承载本机窗口。也就是说**本机本来就是合格门禁环境，全部 T5 测量都是在物理 4090 @120 Hz 上取得的**。因此"必须换权威物理显示复核"不是遗留项，遗留项只有"如何在本机把被测效应做得比噪声更大"（增加轮次、配对统计、把窗口显式钉在 DISPLAY1、跑 AGENTS.md 要求的 5 分钟门禁时长）。

- 正确性与门禁：全部留存运行 `drop=0`、`render_canonical_regressions=0`、`source_split_observations=0`、`warm_step_p95` 64–246 ms，精确导航与逐帧推进终态齐全；held-step 序列错误在门开/门关两种构建间无系统差异。需要单列的两项**既有事实**（与本次观测层改动无关，改动前后同在）：该素材的 `seek_p95` 稳定在 771–1079 ms（52 次运行一致），远超越探针 500 ms 目标，属既有的精确导航长尾；held-step 序列错误在 fixture 上多次为 0，而在该素材上为 1–15，属既有的按素材差异。两者均需单独立项，不在 T5 范围内。`ctest --preset dev` 全量通过、`format-check`、`lint`、`qmllint --max-warnings 0` 通过。新增测试：`DiagnosticsProbeTests` 6 项（事件类别数值固定、载荷编码、未启用 trace 时零开销、阶段名精确匹配）与 `tst_timeline_thumbnail_cache.qml` 5 项（采样网格、缓存命中不重抓、未缓存取样为空、trace 桥上报帧号、generation 重置）。

- 验收工装遗留：`--ui-performance` 的入参解析不还原 `Start-Process -ArgumentList` 写入的引号，含空格路径会被拆成多个源并失败为 `media-error:`；本次用 8.3 短名 `D:\Videos\20BFE9~1.MP4` 指向同一文件（sha256 相同）。应用本身经对话框/拖放打开含空格路径不受影响，故仅记录在证据脚本中，未改动业务代码。

- 明确未做（避免越过实施边界）：未改 `kPlaybackCatchUpTolerance`/`kPlaybackPresentationLead` 等调度常量，未改 actor/cache 字节政策与预读，未改缩略图采样策略，未更换 Qt/FFmpeg 或渲染循环配置，未引入用户可见性能模式菜单。


**T5 后续：用户报告卡顿的定位与修复（2026-09-17）**

- 现象与复现：用户报告播放 `D:\Videos\2026-06-01 23-46-34.mp4` 卡顿/丢帧。先按 T0 口径复现：8 秒播放窗口内 ACK 间隔 P95 78–93ms、最长 169ms、>40ms 停顿 42 次/窗口（`out\t5-evidence\reported-stall-52e4f2975153489aa1d4b73074d048ea`），且停顿以 250ms 周期规律出现（早期帧 7/22/37/52… 间隔 61ms），渲染帧号 0 缺口/回退，FFmpeg 全量解码无错。丢帧计数为 0 属“整组未丢”，不是播放平滑。
- 根因一（探针工装，影响证据与探针本身）：`Main.cpp` 两个性能探针在 GUI 线程每 250ms 调 `sampleCurrentProcessTelemetry()`，其中 `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)` 实测 8–19ms、随系统线程数增长，正好解释 250ms 周期的 ~61ms 停顿。修复：新增 `ProcessTelemetrySampler`（平台层，后台单线程采样，250ms 节奏不变，峰值仅在 join 后合并，异常隔离），探针 GUI 线程不再执行枚举。
- 根因二（渲染活性缺陷，正常 GUI 同样暴露）：`ComparisonSurface::ComparisonRenderNode::render` 丢弃 `renderer_.render(state)` 的返回值；`Contended`（broker/mailbox try-lock 失败）不重试，且该次 update 已被消费，播放只能等 5 秒看门狗或下一次目标推进。修复：`RenderRetry.h` 对 `Contended` 结果 queued `QQuickWindow::update()` 一次；其余结果（含持续失败）不重试。
- 验证（`out\t5-evidence\playback-fix-ab`，interleaved A/B 6 轮×8s，同机同素材：基线 exe `e6718644…` vs 修复 `bf8e86a1…`，配对差值 95%CI 不跨 0）：>40ms 停顿 32,32,33,32,32,32 → 4,5,1,0,0,7（Δmean −29.3，CI [−31.8, −26.8]）；显示间隔 P95 Δmean −66.2ms（CI [−75.8, −56.6]）；最长 ACK 间隔由基线 126–156ms 降至 33–154ms（其中 4/6 轮 ≤93ms），ui_loop_gap_max 129–152ms → 20–35ms；全部 12 次 `drop=0`、`render_canonical_regressions=0`。
- 测试：`platform.ProcessTelemetryTests.*`（4 项，采样线程化/有界性/峰值/关停）、`platform.ComparisonSurfaceWarpTests.RetriesContendedRenderWithoutAnotherPublication`、`ui.ReviewControllerTests` 全量、`ctest --preset dev` 全量通过；format-check、lint 通过。
- 范围与未做：未改协调器调度常量/缓存政策/缩略图采样/渲染循环配置；ReviewController 宽播报（currentTimecode/currentMediaTime 归一化缺失，播放中每帧额外触发 stateChanged）经静态确认存在但本轮不落地（影响面需单独回归与测试，已从本提交剔除）；seek P95 858–1108ms 为既有问题（B4），未动；本机 A/B 在 144Hz 屏选择下完成，环境记录在案。

**T5 后续 2：GameDVR 开放 GOP 素材无法播放的定位与修复（2026-09-17）**

- 现象：用户报告 `D:\Videos\Captures\王者荣耀世界 2026-06-01 13-25-17.mp4` 无法播放。应用报错 `The indexed timestamp did not identify decoded frame 57 (target 62062, decoded 63063)`，失败发生在软件回退后端（`all_d3d11va:false`，broker 忙时回退）；同文件 D3D11VA 路径因帧缓存命中模式不同而时过时不过，非确定性。
- 机理（由真实素材包序列证实）：该 GameDVR 采集为 VFR + 开放 GOP——GOP 边界处 pre-keyframe 帧的 DTS（59560）早于下一 keyframe 的 DTS（61061），而其 PTS（62062）又小于 keyframe PTS（63063）。MP4 按 DTS 定位，`av_seek_frame(62062, BACKWARD)` 落在 61061 的 keyframe 上，其首个解码输出即 63063，越过目标 1001 tick（恰好一帧），严格 PTS 相等检查正确拒绝后硬失败；既有的"重开一次"恢复路径重复同样必然失败的定位。合成 libx264 夹具（bf1/2/3、pyramid、cgop、open-gop 尝试）均无法复现该结构，最终以环境变量门控的真实文件回归测试锁定（`SoftwareDecoderTests.ExactDecodeWalksEveryOrdinalOfProvidedGameDvrCapture`，未设置 `DVS_TEST_GAMEDVR_CAPTURE` 时跳过保持 CI 密封；逆向全序遍历在修复前稳定失败于 ordinal 521）。
- 修复：`SoftwareDecoder` 与 `SignatureDecodeSession` 的精确解码在首次输出越过目标 PTS 时，改为回退到更早的索引显示序号（回退量 1 起倍增，上限 16 个序号）重新定位并向前解码到精确目标；PTS 相等检查仍是唯一接受条件，不引入"最近帧"替代；回退耗尽后保留原有重开与严格报错路径。`kMaximumSeekOrdinalBackOff = 16`。
- 验证：真实素材全序号 exact 遍历由失败转为通过（57 秒→135 秒含回退 seek 的 547 帧全走通）；`media.S*` 全部 ctest 通过；应用探针对该素材 exit=0、`decoder_exact_seeks=2`、`display_interval_max_ms=25`、无 media-error；全量 `ctest --preset dev` 通过、format-check/lint 通过。
- 范围与未做：未改索引构建、队列/缓存政策与调度常量；未把真实素材二进制入库（含个人信息，仅环境变量引用本地路径）；该素材 seek P95 与 B4 长尾问题仍然分开处理。

**T5 后续 3：播放准备提前量随帧间隔自适应（2026-09-20）**

- 背景：`PlaybackCoordinator` 的播放准备提前量是固定 14 ms。帧间隔小于约 28 ms（约 36 fps 以上）时，`due - lead` 落到上一帧边界之前，请求时刻被 `kMinimumPlaybackPreparationDelay` 地板（提交后 1 ms）接管，调度余量归零；120 fps 素材上每帧都在上一帧提交后 1 ms 请求，请求时刻随解码抖动漂移。
- 修复：提前量改为 `min(14 ms, 上一帧到本帧间隔的一半)`；间隔取自 canonical timeline，VFR 按局部间隔逐帧计算；运行首帧（`target == firstTarget`）无窗内前驱，保持原值。**修复了树内一版实现的单位 bug**：间隔用 `time_since_epoch().count()`（本工具链为纳秒）直接当微秒与 14 ms 比较，放大一千倍导致封顶从未生效；现先 `duration_cast` 到微秒再比较。
- 测试（`PlaybackCoordinatorTests`，fake clock + fake scheduler 确定性）：120 fps 第二个 cadence 请求落在边界前半个间隔（anchor+12'501us，固定提前量本会再次贴地）；60 fps 收敛到 8'333us 半间隔；30 fps 保持 14 ms 全量（anchor+19'334us，锚定既有行为）；VFR 第三帧按 19.8 ms 局部间隔封顶到 9'900us（69'800-9'900=59'900us，固定提前量本应为 55'800us）。
- 验证：dev 全量 `ctest` 通过、`format-check`、`lint` 通过；nav gate（1080p60 夹具，12s）drop_ratio=0、display P50 17ms/P95 18ms/max 29ms、无 canonical gap/regression；**1080p120 交替 A/B（4 轮，同机同素材，`out\ab-lead\lead-120fps-dev`）两构型均 120.00 fps、P50 8.2ms、>40ms 停顿 0、gap/regression 0**，配对差值 fps ±0.16、P95 ±0.5ms、max −3.4..+0.3ms，全部在噪声内——即无回归，但该合成素材上无可测收益（其管线本就被 vsync/ACK  pacing，未出现请求贴地导致停顿的剖面）。
- 既有问题（本次排除归因）：nav gate 的 `held_step_presented_frames != held_step_submitted_frames` 严格判据（491a199 引入）在本机对 T0 夹具稳定失败（本次改动 255/300、回退提前量封顶后 260/300、更早通过运行为 254/300），属 B5 同族的探针吸收排队步骤问题，与本改动无关，单独立项。
- 范围与未做：未改 `kPlaybackCatchUpTolerance`、缓存/预读政策、缩略图采样、渲染循环配置。

### T6 · P1｜文件夹审查上下文与多维可信度状态

| 字段 | 可执行约定 |
|---|---|
| 目标 | 用户不需要记住隐藏规则，也不用每换一对重新找观察位置。 |
| 涉及模块 | `FolderPairSidebar`/模型；源标题；ImageWorkspace；比较语义投影、`ComparisonExactness` / `ComparisonValidator`；状态栏。E08、E09、E14、E24、E25、E35。 |
| 前置依赖 | T1/T2/T3 提供正确来源与状态 |
| 实施边界 | 显示当前配对规则和完整/缺失/失败数量；选择缺失行可看存在的一侧；保持模式/兼容视图，尺寸不兼容重置有解释；自动滚到选中行。并列显示时间/空间/像素范围，不改变现有对齐算法，不自动模糊匹配。 |
| 验收 | 100 行列表键盘连续切换，选中行始终可见；检查缺失项无需资源管理器；切差异模式后换三对同尺寸图模式/ROI 不丢；24/30fps、VFR、异尺寸、颜色转换及缺帧夹具均能看见真实映射/限制；滑动两侧 A/B 身份不反向误标；同名大小写冲突明确列出而非静默任选。 |
| 成本等级 | 小至中；不是精确工期承诺 |
| 主要风险 | “已配对”不能等价为“内容相同”；多种不精确原因不可被单枚举最高优先级遮蔽；不要增加四套并排设置栏。 |
| 回退 | 低频详情可折叠或暂隐藏新筛选，但必要可信度警告/来源身份不可隐藏；不改旧序列化枚举数值。 |


### T7 · P2｜只读问题记录与轻量会话恢复

| 字段 | 可执行约定 |
|---|---|
| 目标 | 一个截图能追溯到准确对象与显示条件，一条问题记录可以重新定位。 |
| 涉及模块 | 复用现有源身份与设置持久化边界；新增审查记录 schema、小型列表和导出动作；不改解码引擎。E02、E22–E25、E28。 |
| 前置依赖 | T1–T3/T6 已稳定；不阻挡核心修复发布 |
| 实施边界 | 记录 schema version、源路径+身份/文件变更信息、源帧号/PTS或配对、映射 revision/参数、ROI/视图、差异/重采样/颜色策略、备注；截图明确是显示结果，不伪装原始像素；先手动保存/加载，自动恢复可选。 |
| 验收 | 保存→关闭→重开→选问题，回到同一有效来源和观察上下文；移动/修改源时提示并要求重定位，不能加载错误文件假装恢复；无有效帧/有加载候选时导出不得混淆身份；旧 schema 可拒绝并解释，不损坏原文件。 |
| 成本等级 | 中；不是精确工期承诺 |
| 主要风险 | 不要加入时间线剪辑、音频、批量转码、数据库服务器或质量排名；不把来源改动后的旧截图当当前结果。 |
| 回退 | 禁用自动恢复仍可读取/导出记录；版本化格式、不破坏已有设置；持久化失败不影响当前查看任务。 |


## 证据索引

- **E01** 审查 commit 与提交历史：https://github.com/sonwe1e/VideoCompareStation/commit/139fd5067d582a95beb2bcf99d2586099040a000
  固定目标分支的审查快照；不把默认分支当作目标。
- **E02** README / 产品范围与构建：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/README.md
  Windows、1–3 路、静音视觉审查；性能目标不等于已取得的实测。
- **E03** 依赖锁定：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/vcpkg.json
  Qt 6.11.1；FFmpeg 8.1.2 port 3；GTest、nlohmann-json。
- **E04** CMake 分层与可选构建：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/CMakeLists.txt
  C++20、CMake 4.4；桌面与 Windows 适配层区别于可单独构建的核心。
- **E05** ImageReviewController：加载、原图、差异：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/ui_qml/src/ImageReviewController.cpp#L351-L405
  recomputeDifference 会直接缩放并覆盖 secondary_；同时检查 openPrimaryImage / openSecondaryImage / loadChecked / displayImage。
- **E06** 历史核对：基点已存在相同尺寸处理：https://github.com/sonwe1e/VideoCompareStation/blob/bbb88b3f67051b5c86d6faad8fe8e74355cbb0a2/src/ui_qml/src/ImageReviewController.cpp#L351-L365
  该问题不是最新文件夹提交首次引入；必须区分新增与既有缺陷。
- **E07** DesktopApplication：图片配对连接：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/ui_qml/src/DesktopApplication.cpp
  setPairOpener 的回调顺序打开 A、B，忽略 bool 返回，再设为 SideBySide。
- **E08** ImageFolderPairModel：扫描、配对、打开：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/ui_qml/src/ImageFolderPairModel.cpp
  仅本层、完整文件名含扩展名、大小写折叠；openPairAt 无法接收实际解码结果。
- **E09** FolderPairSidebar：选中与缺失项：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/ui_qml/qml/FolderPairSidebar.qml
  selectRow 依赖 openPairAt 提交选择；单边行不能打开；上下切换跳过缺失行。
- **E10** Main：打开事务与模式路由：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/ui_qml/qml/Main.qml#L280-L650
  performVideoReview / performImageReview / reviewUrls / loadFolderComparison / returnFocusToViewer。
- **E11** Main：输入上下文：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/ui_qml/qml/Main.qml#L241-L279
  globalMediaShortcutsEnabled 未把 workspaceMode 纳入准入条件。
- **E12** Main：模式菜单与原生选择器：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/ui_qml/qml/Main.qml#L650-L1840
  模式进入时间、取消路径和文件夹/图片对话框；各入口对视频会话的处理不同。
- **E13** ReviewShortcuts：应用级快捷键：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/ui_qml/qml/ReviewShortcuts.qml
  Qt.ApplicationShortcut 的视频操作与图片工作区自己的 Keys 处理并存。
- **E14** ImageWorkspace：缩放、图像请求、模式：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/ui_qml/qml/ImageWorkspace.qml
  baseScale × zoom 才是显示比例；百分比显示仅取 zoom；Image asynchronous:false / cache:false。
- **E15** 图片快速解码器：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/ui_qml/src/StillImageDecoder.cpp#L1-L59
  stb_image 的 JPEG/PNG/BMP/GIF 通路输出 RGBA8；尺寸限制在解码后检查。
- **E16** PlaybackCoordinator：播放与精确导航策略：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/application/src/PlaybackCoordinator.cpp#L1-L300
  含 2000 ms 追赶容忍常量、协调线程与有界队列；不能仅凭该常量确认卡顿根因。
- **E17** MultiSourceFrameProvider：源映射与帧组：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/media_ffmpeg/src/MultiSourceFrameProvider.cpp#L900-L1170
  canonicalFrame + resolved offset；显式 Missing；每源并行完成后组装；Exact 帧组缓存。
- **E18** SourceDecodeActor：线程、缓存与请求优先级：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/media_ffmpeg/src/SourceDecodeActor.cpp#L1-L340
  每源一个工作线程；Exact/Sequential/Prefetch；额外解码上下文不是额外源工作线程。
- **E19** SoftwareDecoder：D3D11VA、软件回退和 PTS 定位：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/media_ffmpeg/src/SoftwareDecoder.cpp#L360-L660
  类名虽为 SoftwareDecoder，实际也承载硬件解码；准确定位失败会显式返回错误。
- **E20** FrameSetAssembler：完整帧组：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/media_ffmpeg/src/FrameSetAssembler.cpp
  所有预期槽位完成才生成 FrameSet；Missing 也是有身份的显式状态。
- **E21** D3d11ComparisonRenderer：绘制与 ACK：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/platform_windows/src/D3d11ComparisonRenderer.cpp#L850-L950
  旧 front 保留、队列背压、绘制提交后 acknowledge；ACK 不等于屏幕扫描显示。
- **E22** ComparisonSurface：Qt 场景图接入：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/ui_qml/src/ComparisonSurface.cpp#L1-L380
  QSGRenderNode 接入 D3D11，负责视图几何、DPR、ROI、旋转、SAR 等投影。
- **E23** ReviewRuntime：运行时装配与释放：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/app/ReviewRuntime.cpp#L1-L270
  224 MiB FrameBudget、GpuTransferActor、时钟、定时器、协调器、独立拥有依赖的关闭工作。
- **E24** 比较精确度判定：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/application/src/ComparisonExactness.cpp
  码值/显示转换/空间重采样/时间映射/缺失；单枚举不等于完整多维说明。
- **E25** 兼容性检查与时间基准：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/domain/src/ComparisonValidator.cpp
  帧率、时长、帧数差异已有 AlignmentRequired；参考角色当前参与 canonical 选择。
- **E26** 时间轴缩略图：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/ui_qml/qml/TimelineThumbnailCache.qml
  grabToImage、采样与有界 LRU；是否造成卡顿需要开关对照。
- **E27** Trace schema 与证据边界：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/docs/engineering/trace-schema.md
  16,384 事件、关闭时导出、溢出失败；若干事件未发出，PartialFrameSetCount 不能用作运行时零错配证明。
- **E28** 维护与性能说明：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/docs/engineering/maintenance-and-performance.md
  背景文件检查、缓存、日志限制、已有硬件目标与构建环境警告；不是本次测量结果。
- **E29** 视频格式边界：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/docs/media-support.md
  SDR / NV12 / P010、VFR 与原始码值限制；4K 不在当前主动性能矩阵内。
- **E30** 图片控制器测试：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/tests/component/ui/ImageReviewControllerTests.cpp
  小幅同尺寸图测试；本地硬编码 PNG 路径测试可以跳过；缺失替换和尺寸语义未覆盖。
- **E31** 文件夹模型测试：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/tests/component/ui/ImageFolderPairModelTests.cpp
  临时文件、配对/循环/缺失语义；注入 opener 仅记录 URL，未端到端解码。
- **E32** UI 测试登记：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/tests/component/ui/CMakeLists.txt
  已存在 QML、DPI、菜单、滑动分割、控制器测试；不能概括为没有 UI 测试。
- **E33** 已有性能测量结构：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/app/Main.cpp#L84-L141
  seek、warm/held step 分位数、丢帧、资源、线程、关闭等指标。
- **E34** 已有硬件测试：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/tests/hardware/CMakeLists.txt
  真实 QML 控件下的 wipe/diff、保留帧、旋转/纹理填充和 1–3 路性能目标。
- **E35** 来源条与参考标识：https://github.com/sonwe1e/VideoCompareStation/blob/139fd5067d582a95beb2bcf99d2586099040a000/src/ui_qml/qml/ActiveSourceStrip.qml
  父目录及完整路径提示、pending 身份防重复操作是应保留的设计。
- **E36** CI：Linux 核心测试实际日志：https://github.com/sonwe1e/VideoCompareStation/actions/runs/35059308072/job/104676040553
  读取现有日志，不是本次执行。合并预览 c17332e…：149 执行通过，3 禁用。
- **E37** CI：当前关联工作流：https://github.com/sonwe1e/VideoCompareStation/actions/runs/35059308072
  检查时 Debug/Release 队列等待；不能当作目标分支 Windows 全栈通过证明。
- **E38** 分支与基点比较：https://github.com/sonwe1e/VideoCompareStation/compare/bbb88b3f67051b5c86d6faad8fe8e74355cbb0a2...139fd5067d582a95beb2bcf99d2586099040a000
  此快照相对基点领先两个提交；文件夹与 UI 改动不等于重写视频核心。
- **O01** Beyond Compare：列表状态与详细比较：https://www.scootersoftware.com/kb/differentthensame
  官方区分文件夹快速检查与详细内容比较，详细结果可反馈回列表。
- **O02** Diffchecker：图片比较交互：https://www.diffchecker.com/image-compare/
  官方的左右独立入口、同步缩放/平移、滑动、闪烁、淡入淡出，以及可撤销对齐。
- **O03** PotPlayer 官方更新记录：https://t1.daumcdn.net/potplayer/PotPlayer/v4/Update2/UpdateEng.html
  2026-06-22 条目记录导航栏定位时暂停功能；不据此虚构某版本的默认键位。
- **O04** Qt：QQuickItem::grabToImage：https://doc.qt.io/qt-6/qquickitem.html#grabToImage
  异步离屏抓图仍可能涉及 GPU→CPU 读回；异步不等于零开销。
- **O05** Qt：QQuickWindow::frameSwapped：https://doc.qt.io/qt-6/qquickwindow.html#frameSwapped
  信号代表一帧已排队等待呈现，而非已经完成物理屏幕显示。

## 遗留任务登记（2026-09-16）

本节登记**当前未开工、尚未解决**的事项。每项都注明来源、为什么没做、以及重新开工前必须先满足什么。顺序即建议优先级。

### B1 · P1｜T6 文件夹审查上下文与多维可信度状态

未开工，**这是下一块关键路径**：前置依赖 T1/T2/T3 已全部交付，且它是剩余工单里唯一的 P1（T7 是 P2）。已对照现有代码核过差距，五处具体缺口：

1. `FolderPairSidebar.qml` 的 `MouseArea` 与 `selectRow` 对单侧行直接 `return`（`hasBoth` 为假时行 `opacity 0.55` 且不可选）——"检查缺失项无需资源管理器"这条验收目前过不了；
2. 头部只有文件夹名，没有"完整 N / 仅 A N / 仅 B N / 失败 N"计数——"已配对≠内容相同"这个被点名的风险没有可见防线；
3. 列表是无 `positionViewAtIndex` 的 `ListView`——100 行键盘连切时选中行会滚出视野；
4. 配对规则（仅本层、完整文件名含扩展名、大小写折叠）与同名大小写冲突都不可见，冲突仍是静默处理；
5. 视频侧只透出 `ComparisonExactness` 的单枚举最高优先级，未并列摊开 `CompatibilityFinding` / `AlignmentRequired` 的时间/空间/像素维度。

可复用资产：`ImageFolderPairModel::stepCompleteRow`、T3 的 `workspaceSession`、T4 的异步 opener 与 `completePairOpen`、T1 的原子提交语义。预计主要为 QML + 少量 model 角色/属性扩展。

### B2 · P1｜T5 定论实验：把"证据不足"升级为确证（正或负）

**未完成，T5 因此仍挂在"待定"。** 硬件资格已确认不是障碍（`test-hardware-runner.ps1` 输出 `DVS_HARDWARE_RUNNER_READY session=1 physical_adapters=1 refresh_hz=120`；只有 `\\.\DISPLAY1` attached，由物理 4090 驱动）。真正的障碍是**测量功效**，三项都不符合项目自己的口径：

- 窗口只有 **8 秒**，而 `AGENTS.md` 要求 **300 秒**；
- 进程以 **BelowNormal** 优先级启动，而 `docs/self-hosted-runner.md` 要求硬件门禁以 **Normal 优先级 + 完整 CPU affinity** 运行；
- 电源方案是 `Stable-Balanced-v1`（省电），且机器上同时运行 DoubaoWork / ZCode / chrome / Weixin / cloudmusic 等常驻高 CPU 进程。

现有配对 A/B 的统计量说明为什么给不出定论——`gate-ab-interleaved` 5 轮配对差值：

| 指标 | 均值 | 标准差 | 95% CI |
|---|---|---|---|
| >40 ms 停顿次数 | −3.8 | 8.07 | [−13.8, +6.2] |
| 显示间隔 P95 | −5.4 ms | 59.3 ms | [−79, +68] |

两个区间都跨 0；逐轮 P95 差值为 +42、+21、−103、−18、+31 ms——**一次运气好，其余四次更差**，属噪声而非效应。

重新开工的前置条件与步骤：

1. 静默整机（需人工退出上述常驻进程），电源方案切到"高性能"（`8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c`），以 Normal 优先级启动；
2. 用 `tools/testing/generate-evidence-fixtures.ps1` 生成 300 秒 1080p60 素材，`--seconds 300`；
3. `tools/testing/run-t5-gate-ab.ps1` 交替 A/B 各 6–8 轮，报告**配对 95% CI**；
4. **判定标准先写死：CI 不跨 0 才算定论**，无论正负。若仍跨 0，则"抓图不是热点"从"证据不足"升级为"已确证排除"，T5 可按负结果结案。

### B3 · P2｜渲染线程调度：已发布帧不被及时绘制

T5 唯一未被排除的嫌疑项，也是当前所有 >40 ms 停顿的支配来源。证据（`analyze-render-staging.ps1`）：帧已发布后，`RenderPublished → RenderDrawStarted` 的 P95 为 65–113 ms，**支配全部 303 个 >40 ms 区间**；而 `drawStarted → ackPublished` P50 仅 0.01 ms、max 0.38 ms，`ackPublished → acked` 与 `acked → commit` 均 ≤0.1 ms。即：**帧已经交到渲染线程面前，场景图却在几十毫秒内没有为它排上一次绘制**。

已排除的变量：渲染循环配置（默认 / `threaded` / `basic` / `QSG_NO_VSYNC=0` 四组提交率 28.3–28.7 帧/s，互相落在噪声内）、解码侧（生产速率 59.45 fps、载荷步进恒为 +1）、缩略图抓图（见 T5 证伪实验）。

开工前置：先完成 B2，因为需要先有一个噪声足够低的测量环境，否则无法判断任何候选修复是否真的有效。B2 之后若确认该停顿在静默环境下依旧稳定复现，才值得深入 Qt 场景图调度（`update()` 合并、窗口暴露状态、DWM/DXGI 呈现路径）。

### B4 · P2｜精确导航长尾：真实素材 seek P95 771–1079 ms

T5 期间顺带测出的**既有**问题，不随观测量改动变化。52 次留存运行中该素材 `seek_p95` 稳定在 771–1079 ms，远超探针 500 ms 目标（T0 fixture 上为 124–245 ms）。工单里 `2000 ms` 追赶容忍常量与 prepare 提前量（`kPlaybackPresentationLead = 14 ms`）都不是已确认根因，动手前必须先做定位实验，并且不得把精确导航正确性开关化。

**定位实验（2026-09-20，`out\reported-stall-recheck`，当前构建，同素材 8s 探针 + trace 切片）**：seek P95 818 ms 由两段构成——

1. **派发段** `CommandAccepted → DecoderSeek` 中位 89 ms（20 个采样 seek 中 16 个落在 79–132 ms，最大 835 ms）。协调器侧 `beginSeek` 是同步直排（cancel 旧作用域 → generation+1 → 立即 submit），因此这 89 ms 在 provider/actor 内：`SourceDecodeActor::cancel` 用 `completion.get()` **同步等待** worker 处理取消 job（`SourceDecodeActor.cpp:225-233`），worker 线程优先级为 `THREAD_PRIORITY_BELOW_NORMAL`，取消排在当前在途解码之后；多源时按源串行等待。
2. **解码遍历段** `DecoderSeek → FrameSetReady(目标)` 中位 126 ms、最大 830 ms。素材关键帧极稀：10.47 s 仅 4 个 I 帧（0 / 2.25 / 6.42 / 9.32 s，最大 GOP 4.17 s ≈ 250 帧），后退定位需从关键帧正向遍历；`FrameSetReady → commit` 仅 2–20 ms，排除渲染/ACK。

后续方向（均需在静音测量环境下先验证）：seek 取消改为非阻塞（让新一代请求顶替而不同步 drain，或把 cancel 并入请求队列由 worker 在同一 job 内切换）；遍历段考虑对已遍历窗口的有界复用/更近的关键帧定位。两者都不得放宽严格 PTS 相等接受条件。

### B5 · P2｜held-step 序列错误按素材分化

同为既有问题：held-step 序列错误在 T0 fixture 上多次为 0，而在真实素材上为 1–15。需要先区分这是"素材本身有重复/非单调 PTS"还是"顺序游标在长 GOP 上真的跳帧"，再决定是否修。按工单口径，连续播放允许整组跳过但**计数必须真实**，所以先要把计数与真实丢帧对齐。

**补充（2026-09-20）**：491a199 引入的严格判据（`held_step_presented_frames == held_step_submitted_frames`）在本机稳定失败：1080p60 双夹具 255–260/300、1080p60 真实素材 295/300、1080p120 单夹具同样未满。剖面一致——探针按 cadence 提交 300 步，窗口结束时步进流仍有已接受未呈现的排队帧被静默丢弃（`finalizeHeldStep` 后不再等待 drain）。属探针窗口与协调器队列深度的测量口径问题，不是丢帧（drop_ratio=0、canonical gap/regression=0）。修法二选一：窗口结束后 drain 到非 busy 再 finalize，或把"已接受未呈现"计为 absorbed 而不是 fail；不得通过放宽呈现计数来放行。

### B6 · P2｜选屏策略会选中虚拟/间接适配器

`DesktopApplication` 在 `preferHighRefreshScreen` 下取 `QGuiApplication::screens()` 中**刷新率最高**的屏幕。当前无害（GameViewer/Oray 的 14 个虚拟输出全部 `attached=False`，不进 Qt 屏幕列表），但**一旦这些虚拟输出被挂进扩展桌面**（远程串流场景常见），144 Hz 的 GameViewer 会被选中，呈现节奏立刻失去证据资格——而且不报错，只会静默产出不可信的数字。建议让选屏显式排除虚拟/间接适配器，或在选中非物理适配器时给出可见警告。

### B7 · P3｜`--ui-performance` 入参不还原引号

探针的入参解析不还原 `Start-Process -ArgumentList` 写入的引号，含空格路径会被拆成多个源并失败为 `media-error:`（T5 期间用 8.3 短名绕过）。应用本身经对话框/拖放打开含空格路径不受影响，故只是证据工装的可用性缺陷。

### 已完成（供对照）

T0 证据基线、T1 图片配对原子提交、T2 原图不可变与尺寸语义、T3 工作区状态与命令路由、T4 图片后台加载与按需派生、T5 执行完毕（**观测层已交付，行为修复无落地**：候选缩略图门控被交替 A/B 证伪后完整回退）。
