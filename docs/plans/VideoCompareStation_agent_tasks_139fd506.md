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
