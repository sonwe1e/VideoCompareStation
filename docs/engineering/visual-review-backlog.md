# 视觉审查问题台账

更新：2026-10-06。需求见 [产品目标](../product/visual-review.md)，代码路由与命令见
[Agent 快速定位指南](../agent-guide.md)。此页是当前任务入口，不是发布完成清单。

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
观察口径，并提供不进菜单的临时隐藏。上轮已核实着色器（`Nv12ToRgb.hlsl`）中 Highlight
指标本就是「保留 A 路原图＋按增益对超阈值区域红色 tint」，与审查描述一致，本轮只做接线。

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
