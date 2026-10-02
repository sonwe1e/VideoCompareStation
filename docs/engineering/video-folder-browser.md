# 视频文件夹浏览与手动播放

2026-10-02，基线 `4a67d1b` + 未提交工作区。此页记录日常播放入口，不是媒体库、批量评测
或发布验收；此前指标与打包工作区均保留。

## 用户可见行为

- 首屏「从文件夹选视频…」、文件菜单同名入口、`Ctrl+Alt+O` 打开文件夹选择器。
- 只列所选本地文件夹顶层的 `.mp4/.mkv/.mov/.avi/.m4v`，大小写不敏感，文件名自然排序
  （`clip1 / clip2 / clip10`），保留中文路径。文件名过滤不是可解码格式认证。
- 点击文件以**新的单视频任务**打开，等待生产 shell 的匹配成功终态，再开始静音播放。
  已有视频播放中则先异步暂停并等待传输／旧帧收尾，再打开；不要求用户手动暂停。
  Open 的成功仍以首帧呈现 ACK 为前提，没有绕过播放内核或伪造终态。
- 上一项／下一项为手动操作，不越过端点、不在 EOF 自动连播。快速换选以最新待打开项为导航
  锚点；当前高亮只跟随实际已提交的单视频源，失败不把画面错误标成所点击文件。
- 选择文件夹本身不打开视频或切换工作区。列表可收起、通过视图菜单重新显示；纯净模式隐藏
  列表并归还画面空间。图片画面也可保留到新视频成功，而不是选择目录后立即丢掉。
- 刷新保留播放内容，按 URL 重新寻找当前文件，不用旧行号认领移动后的列表项。空目录、目录
  读取失败、视频打开失败分别显示状态；失败保留上一份完整目录和已提交的媒体任务。

不做递归扫描、缩略图／媒体探测、媒体库、目录监视、会话持久化、音频和字幕。显式 UNC／
网络 URL 被拒绝；映射盘的系统调用仍可能慢，不承诺读取耗时。关闭／换目录抑制旧自动播放，
并取消仍排队的旧 intent；**已经交给内核的活动 Open 不被强行终止**，可完成为暂停的视频。

## 实现边界

- `VideoFolderModel` 是 Qt Core 列表投影：worker 仅持有不可变目录参数、代次、取消 token 和
  独立 mailbox，绝不持有 QObject。文件枚举、可读性检查和排序都在 worker 运行。
- 默认每个浏览器最多一个 worker、一个 latest queued job；不在 GUI 上 join 慢文件系统调用。
  结果一次性 reset，代次／取消 fence 拒绝旧回复；每份目录硬限 100,000 个视频，超限报错而非
  发布截断列表。20 ms 轮询仅在读取期间活动，不是逐帧媒体工作。
- `ReviewShellController::openVideo` 冻结单个 URL、参考槽 0 和 NewReview 意图，返回独立 intent
  身份，继续使用现有有界串行队列和 foreground command 终态。浏览器不改写 staged 比较列表。
- `attachPlayback` 共用于桌面组合根和集成测试：源集同步决定当前行；匹配成功、当前 URL 仍为
  目标、且 play 被接受，才能自动播放。外国终态、重复终态、失败重开同一文件都不能误播放。
  更新的普通文件菜单／Explorer／素材 intent 也会使旧浏览器自动播放失效。
- `VideoFolderSidebar.qml` 懒加载，与原视频／图片视口保持空间分离；选择器加入原 modal gate。
  原有 FrameSet、generation/request 身份、呈现 ACK、精确定位与比较指标没有改动。

## 播放中换项提交失败修复

用户反馈选中 `videos` 目录后出现「无法提交视频打开请求」。已复现其中一个确定原因：
`ReviewController::busy()` 仅表示前台命令，播放中、Play/Pause 在途及旧播放帧收尾时可以为 false，
但 `canOpen()` 仍为 false。原 shell 仅按 busy 判断提交／出队，立即拒绝 Open，浏览器得到 intent
ID 0 后显示该错误；这不是目录枚举失败或扩展名过滤问题。

修复在 `ReviewShellController::submitOrQueue/drainIntentQueue`：

- graphics 已就绪而 canOpen 未就绪的素材 intent 保留在原有有界队列，异步唤醒 drain。
- 仅在 canPause 为 true 时提交一次 Pause；既有 Play/Pause 在途则等待其匹配终态。即使
  requested==displayed，也必须等 requested frame 清空，不能用 framePending 代替 canOpen。
- 旧帧收尾后的 stateChanged 恢复出队；成功 Open 后才沿既有浏览器接线自动播放。快速换选／
  取消仍操作排队 intent，当前高亮仍来自实际提交源；Close 的优先级与前台串行门禁不变。
- Pause 提交被拒绝则结束该排队项并报告失败，不残留永久等待；graphics 未就绪仍拒绝 Open。
  没有循环等待、GUI join、放宽内核命令门禁或修改 ACK-before-commit。

`ReviewControllerTests` 新增 **6 项**，通过生产 `attachPlayback` 与 shell 覆盖上述路径、外国
transport terminal、最新选项、取消、Pause 提交拒绝及 graphics 未就绪。修复前 **5/6 失败**，
修复后相关 controller/model/Main 定向 **113 实际通过＋4 原有 disabled**。新增反例 **10/10**
均成功构建后由运行时 GoogleTest 断言检出；9≠10 缺项 guard 拒绝执行，源字节恢复并回读。

首轮全量再次发现旧 `ImageWorkspaceManual` 的等待假设：窗口 shown，但 RowLayout 的 polish
仍 scheduled，未收到新渲染帧；`waitForPolish`／`waitForRendering` 会超时。这不是本次 shell
产品修改导致的布局变化。测试现在让 text/layout 更新进入事件循环，再有界查询原两条几何
条件，不要求 polish 队列完全空闲；未改变产品布局或宽度上限。强制像素读数越界、状态栏覆盖
提示区的 **2/2 QtTest 产品变异**仍被原几何条件检出，产品源逐字节恢复。

恢复重建后的默认 dev：**859 项：852 实际通过、3 原有 skipped、4 原有 disabled、0 failed**；
完整 lint、format-check 通过。日志、变异脚本与结果、两轮全量、布局复查／探针及源码前置
备份在 `out/verification/video-folder-open-transport/`，不覆盖首批证据。当前测试注入快照／
终态，不是实际用户 `videos` 素材、屏幕 ACK 或硬件长测验收；仍需用户重试确认该具体目录。

## 首批回归、反例与画面

生产测试只在 `tests/component/ui/`：

- `VideoFolderModelTests` **15 项**：异步提交、扩展名／自然排序／中文及未知 Unicode 后缀、
  非递归、旧回复、取消／清空、非法目录、刷新重定位、终态／URL、失败重开、快速选择、
  导航端点、容量、调度与播放拒绝、实际后台 worker 和 owner 销毁后的闭包。
- `MainQmlContractTests` 新增 **4 项**：真实菜单／modal gate、实际 ListView delegate 点击→
  单源 NewReview→匹配终态→Play、失败保留工作区与源集、外国新 intent 抑制旧自动播放。
  集成测试使用生产 UI/controller/shell 接线，但媒体快照／终态由测试注入，**不是实际解码或
  屏幕呈现 ACK 的新性能证据**。
- 新增 **19/19** 控制组通过；**40/40** 故障变异在成功构建后由运行时 GoogleTest 断言检出。
  变异脚本要求唯一锚点和固定 40 项、逐项写入回读、finally 逐字节恢复、JSON 读回；故意少一项
  的 39≠40 guard 已拒绝执行。首次 unused-parameter 编译失败不计为检出，修正变异后重新执行。
- 完整 C++／QML lint 通过，最后 QML 再检与 format-check 通过。恢复重建后默认 dev 选中
  **853 项：846 实际通过、3 原有 skipped、4 原有 disabled、0 failed**（CTest 的 849 包含
  skipped，不写成 849 项实际通过）。
- 全量暴露既有图片测试的检查时序：长读数在 RowLayout polish 前 941 px＞可用 920 px，
  polish 后 762 px。`ImageWorkspace.qml` 与 HEAD 相同；首批在原两条宽度断言前增加
  `verify(waitForPolish(details))`，不改产品布局或放宽宽度条件。删掉等待的 QtTest 变异 **1/1**
  被原宽度断言检出，源字节恢复。首轮失败／两次复查／探针／最终全量分别保留。

证据：`out/verification/video-folder-browser/` 的 `red.log`、`control-before-mutation.log`、
`folder-tests.log`、`mutation-run.log`、`mutation-resume.log`、`mutation-results.json`、
各变异 build/test 日志、质量日志、`full-dev-final.log`、`pixel-layout-probe.txt`、
`layout-wait-mutation.txt` 和 `before/`。`folder-sidebar.png` 是测试窗口实际 Qt 截图，
展示目录列表／高亮／导航／空间分隔；画面是注入会话的空渲染面，不冒充真实素材画质截图。

```powershell
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex 'ui\.(VideoFolderModelTests|MainQmlContractTests)'
pwsh tools/build/build.ps1 -Preset dev -Target format-check
pwsh tools/build/build.ps1 -Preset dev -Target lint
```

未提交、未发布。真实用户目录／素材、五分钟 D3D11VA／UI 响应／解码缓存、覆盖率和 ZIP
发布门禁仍须绑定后续候选验证，开发回归不替代这些门禁。
