# CompareStation：下一步体验与性能改进计划

**审查基线：** `a3a06eea20daf58ef757c03cd9649bb91128b843`。  
**计划日期：** 2026-10-11。  
**范围：** 启动与打开、单/多视频原速播放、长按前后走带、对比可视化、多源选择、交互与 Windows 默认打开、UI 排版。  
**证据边界：** 本次为远程代码与文档审查，没有启动 Windows GUI，没有运行真实性能或 ICAT 同机对照。以下数字是建议目标，不是实测成绩。没有修改仓库。

## 0. 决策摘要

下一阶段是“基础体验收敛”，不是再次扩大算法范围。优先修正四条主线：

1. **不让诊断拖慢启动和播放。** 诊断构造路径同步探测，关闭 monitoring 仍接收事件，需要拆清采集、汇总、显示、能力探测的开关和生命周期。
2. **把局部对比从显示纹理放大改为解码纹理直采样。** 现有三局部有完整交互原型，但无法恢复全景缩小时已丢失的细节；“100%”也不是源像素 1:1。
3. **把键盘重复逐帧升级为真正可控的正反向走带。** 单击一帧、长按连续、松手停止，命令和已呈现帧要闭环；复用已有反向 GOP 能力。
4. **统一源卡片、画面、比较对和 GT 的操作。** 每路都有同等入口，移除“看 A/看 B/看 GT”在 GT=A 时的重复，以及 C 缺少对等入口的问题。

保留 C++20、Qt Quick、FFmpeg 动态库、D3D11/D3D11VA。保持 1–3 路产品范围，Windows 优先。新功能都应能够独立关闭或回滚；不重写播放器，不引入新的 GUI 框架，不以云 CI 代替本地真机验证。

---

## 1. 当前状态：代码证据与不能据此推出的结论

| 编号 | 当前证据 | 判断与下一步 |
|---|---|---|
| B01 | `DesktopApplication.cpp` 在 `engine->load(Main.qml)` 之前创建 `TimingDiagnosticsController`；其构造立即调用 `evaluateProbeCapability()` | 呈现能力探测在启动关键路径上。需测量耗时并移出关键路径，不能断言这已造成某个具体秒数的延迟 |
| B02 | `TimingDiagnosticsController::setMonitoring(false)` 只停 QTimer；observer 仍挂接，`onTraceEvent()` 无 monitoring 过滤 | “停止刷新”不等于“停止采样”。这是明确的开关语义与开销问题 |
| B03 | `PlaybackTrace::record` 持 observer shared lock 回调；collector 的 ingest/summary/reset 由同一 mutex 串行化 | 采样会进入解码/调度事件生产路径。需测量竞争，不把存在锁直接等同于实际卡顿 |
| B04 | `PresentationStatisticsProbe` 创建自己的 8×8 隐藏窗口、D3D 设备和交换链 | 只能探测这条独立路径的能力，不能当作播放器窗口已逐帧上屏的证据；零 PresentCount 也不唯一证明虚拟屏 |
| B05 | `ReviewShortcuts.qml` 明确由 OS auto-repeat 驱动 A/D、左右逐帧 | 当前是重复发命令，不是按媒体时钟驱动的 Jog；不能直接认定长按等于原速连续播放 |
| B06 | `SourceDecodeActor.cpp` 已有最多 48 帧、80ms 软填充预算的反向窗口 | 不是从零开发反放；48 是请求上限，不是实际可缓存帧数或反向速度保证 |
| B07 | `MultiSourceFrameProvider.cpp` 完整帧组表容量 4，按 insertionOrder 淘汰，96MiB/全局预算四分之一双重限制；源缓存常量 12MiB | 当前不是独立 LRU 成品。先记录命中率，必要时小改淘汰策略和预算分配，不能盲目放大缓存 |
| B08 | `RoiTripletStrip.qml` 对已绘制全景的 flattened snapshot 重新采样，textureSize 取视口大小×DPR | 局部细节受全景采样分辨率限制；应保留界面，替换这一段渲染路径 |
| B09 | 局部窗口帮助文案明确“100%=ROI 适配窗口” | 必须区分适配、相对放大和源像素 100%，避免用于像素判断时误导 |
| B10 | `CompareModeBar.qml` 已有 RoiTriplet、DualDifference；两项在“…”内 | 新能力已存在，不重复开发；改为按任务可见的入口，并实测效果 |
| B11 | 直看按钮为 A/B/GT；`ActiveSourceStrip` 普通点击切换管理选择，拖动只改卡片展示顺序 | 管理选择、直看源、比较对的含义不统一；GT=A 时直看重复且 C 没有同等按钮 |
| B12 | `VcsTheme.js` 已有语义颜色和圆角；Main 默认1440×900、最小960×640；左栏固定268，右检查器按整窗1120阈值切抽屉 | 不需要另起主题；补尺寸/字体 token，并按实际可用画布宽度响应布局 |
| B13 | `ExplorerCommandRegistration.cpp` 主要注册 SystemFileAssociations 下的 CompareStation.Compare 右键动词 | 右键集成不等于完整的默认应用注册。需补用户可选择的默认打开流程；不修改 UserChoice |
| B14 | 能力台账明确真实三路1080p60稳态门禁及 ICAT 同机对照未执行，并记录远控虚拟显示环境 | 不能宣称当前已原速无丢帧或已全面超过 ICAT。直连与远控应分别报告 |

文档存在落后于代码的状态，例如台账部分条目仍写未接线，但当前已含对应 UI。更新台账应依据“当前调用链+测试+真机结果”，不能只按旧条目再实现一次。

---

## 2. 本轮必须保留的行为契约

### 2.1 三类“同一”不可混淆

**同一时间**由 canonical timeline 和显式映射决定；**同一源**由冻结 SourceIdentity 决定；**同一观察区域**由源坐标 ROI 与视图变换决定。A/B/C 是会话标签，不充当跨会话文件身份。

改变直看源、比较对、卡片顺序不应偷偷改变 GT、时间轴、播放位置、ROI 或缩放。GT 设置必须是独立明确动作。

### 2.2 播放、浏览、审查三类请求分别调度

- 原速播放：按照媒体时间调度，慢时明确记录整组帧跳过，不让各路自行追赶。
- 拖动时间轴：最新目标优先，允许中间预览合并，停稳后补精确帧。
- 精确逐帧/完整审查：不能悄悄跳过目标帧；处理跟不上时展示实际速度。

不能对三个工作流统一应用“所有新请求都取消旧解码”，否则长按可能持续取消尚未完成的有价值工作。

### 2.3 显示与测量分级

源 PTS、应用发布、渲染提交/ACK、播放器对应交换链/系统呈现证据、最终物理显示不是同一个时刻。各层分开命名。没有证据写“未知”，不能写零丢帧。远控画面的接收端流畅度不等于宿主程序本地呈现。

---

## 3. 工作流与交互目标

### 3.1 单视频

资源管理器双击一个视频 → 主窗尽快可交互 → 首帧确认 → 按打开意图自动播放。保留设置关闭自动播放。首帧前不显示假帧号；切文件失败保留原会话。

应用内“比较/审查”打开继续默认暂停以便检查起点。将打开意图写成明确参数，不用模糊的全局布尔值让系统双击、拖入、添加候选互相覆盖。

### 3.2 两个视频

打开后主操作只显示：源 A/B、当前比较对、并排/划线/局部/差异。两源同样需要联动局部，不应只有三个视频才能看局部细节。

### 3.3 GT＋两个输出

源卡片显示：`A · GT`、`B · 模型1`、`C · 模型2`。GT 只是角色徽标，不重复再生成一个“GT 视频按钮”。

推荐点击语义：卡片主体单击直看该源，再点同卡或执行“返回比较”恢复；画面普通点击只激活该面板，不修改 GT；比较对通过清楚标注的 A↔B / A↔C / B↔C 入口选择。批量移除等管理选择进入显式管理状态，不占用普通卡片点击。

现有双击100%/适应、Tab隐藏界面、F11全屏、C切候选等行为不悄悄改名或换义。新增快捷键先过当前 ShortcutCatalog/ReviewShortcuts 冲突审计；不把已用于模式的1–6又绑定为源选择。

---

## 4. 工单：效率与可靠播放

### E01｜启动关键路径减负（P0）

**问题：** 新能力可以增加启动初始化成本；显示了启动图不代表软件已可用。当前同步构造呈现探针是可定位的关键路径工作。

**实现：** 复用 StartupMilestone，分别记录进程进入、可交互主窗、目标素材首帧三个节点。创建 TimingDiagnosticsController 时只建立轻量状态；平台探测改为用户展开诊断时或首帧后按需启动的受控任务。窗口/设备的创建与销毁保持在所属线程，不把已创建 HWND 直接跨线程使用。失败、超时均显示未确认，不阻塞播放。保留现有已后台处理的 shell 注册，不重新引入同步注册。

高级检查器、样本导出对话框、图片编辑区域按实际耗时选择性延迟实例化；不要一次把所有组件改 Loader。轻量状态先存在，重界面晚创建，避免旧控制器绑定变成空对象竞态。目录列表、缩略图与完整文件哈希不得成为首帧硬前置。

**代码落点：** DesktopApplication、TimingDiagnosticsController、PresentationStatisticsProbe、StartupMilestone、Main.qml。

**预期：** 诊断模块不会影响普通播放器启动；UI 可响应与首帧到达分别可测。

**验收：** 空启动、单文件、三文件、无诊断/开诊断、物理屏/远控；启动完成前关闭程序；过期探测结果不得改写新窗口。冷进程与系统磁盘缓存冷启动分开命名。

### E02｜诊断真正可关闭且不干扰生产线程（P0）

**问题：** 目前 monitoring=false 只停止刷新；observer 仍 ingest；汇总 mutex 可能与事件生产竞争。

**实现：** 将采集、汇总、UI刷新、系统能力探测四个状态拆清。第一小补丁让停止监测真的停止新增诊断 ingest，注意并发内存可见性；关闭 observer 时保持生命周期互斥安全。不要误关性能测试或其他 trace sink。默认常驻仅保留必要轻量健康计数；详细采样仅诊断开启时运行。

若基线证明 observer 的同步处理占预算，再用容量有界的事件交接队列：生产者只复制紧凑事件，独立消费者配对与聚合；队列满记丢事件，不阻塞播放器，报告证据不完整。QML每秒2–4次取聚合快照，不每帧分配大量 QVariantMap。

**代码落点：** PlaybackTrace、PlaybackEvidenceCollector、TimingDiagnosticsController、TabbedInspector。

**预期：** 关闭详细诊断后没有残留诊断工作；打开面板可解释地增加少量预算而非改变播放语义。

**验收：** 连续开关100次、关闭时有事件在途、退出销毁竞态、多个trace消费者并存。终端分别输出 observer 调用数、队列丢失数、采样开/关帧时和跳帧差异。

### E03｜原速播放指标与调度守恒（P0）

**问题：** 平均FPS既可能掩盖局部停顿，也可能忽略媒体时间已落后于墙钟。

**实现：** 保持唯一 canonical 媒体时钟；从 PTS/时间基得计划位置，不以“每路sleep 16ms”协调。各源并行解码后原子发布完整FrameSet，不把1路正常、2路旧帧当成功。

增加实际速率（媒体时间增量/墙钟增量）、整组跳过数、迟到帧组数、帧时P50/P95/P99、源解码P95、GUI输入反馈、CPU/显存/缓存预算摘要。分别显示计划时间、已发布时间与可确认呈现层。60/59.94/VFR独立测试，显示器正常重复刷新不算源内容重复。

提供“原速观看”和“完整帧审查”两种明确模式：前者允许必要整组追赶但如实标示，后者不承诺处理能力不足时仍原速。常用素材无超载时，两者都应无额外跳过。

**代码落点：** PlaybackCoordinator、MultiSourceFrameProvider、SourceDecodeActor、FrameMailbox、现有ACK/trace。

**预期：** 可以回答“原速吗、哪一层慢、是否损失了完整帧组”，不是只给60FPS。

**验收：** 三源帧号可视编号合成素材，慢源注入，源更换，连续5分钟，前后台/窗口最小化按定义排除或单列。优化不能用悄悄丢帧换低延迟。

### E04｜长按正反向走带（P0）

**问题：** OS auto-repeat 节奏和解码完成无关，不能作为稳定的连续浏览时钟。

**实现：** 新增输入层 Jog 状态，但继续使用同一个 PlaybackCoordinator。单击立即±1帧并暂停；按住约250ms进入指定方向走带，默认1×，可选0.5×/2×；松手返回暂停。键盘重复事件不重复触发Start。由当前媒体时钟/呈现背压决定下一帧，而不是按键数量。

以明确generation标记一次走带。松手、失焦、打开对话框、切会话、Esc、窗口关闭和按钮取消都结束；旧generation不能继续更新画面。连续模式通常保留一个在途有用请求与有界顺序预取，不能不断取消马上就要完成的帧。快速改变方向则撤销旧方向预取。

反向复用已有GOP窗口，按关键帧顺解并缓存后反序呈现；不为每个倒退帧从头seek。冷GOP处理太慢时报告准备/实际速度，不能假称60fps。

**代码落点：** ReviewShortcuts、ReviewActions、PlayerOsc、ReviewController/PlaybackCoordinator、SourceDecodeActor。

**预期：** 轻点精确、长按连续、松手即停，鼠标按住按钮与键盘一致。

**验收：** 轻点、跨250ms边界、长按10秒、前后交替、首尾边界、鼠标移出再松开、Alt+Tab、弹出文件选择器、按住时换源。检查停止后无旧任务“追跑”。

### E05｜缓存小步改进与反向窗口预算（P1，服务E04）

**问题：** 完整帧组容量4且FIFO，往返5–9帧可能反复淘汰；源缓存12MiB也限制实际可保留帧数。

**实现：** 先给完整帧组缓存、单源缓存、反向窗口分别加命中/淘汰/占用指标。保留全局预算不变的第一轮，将FIFO改为真正的访问LRU；不要仅在插入时更新序号。再依据当前模式在当前帧、邻近帧、反向窗口、缩略图间分配预算。记录实际stride、格式和GPU资源占用，不仅按宽高估算。

预取只在剩余预算且队列空闲时进行，优先级低于打开/精确定位/当前播放。在换向/换文件时取消无价值预取。避免长期固定硬解码器的有限输出表面；必须复制时复制到应用拥有的有界纹理池，保留正确释放时机。

**代码落点：** MultiSourceFrameProvider::cacheSet/cachedSet、SourceDecodeActor、现有FrameBudget。

**预期：** 往返审查缓存命中提升，无无限内存增长，无明显退出停顿。

**验收：** 往返4/9/24帧，短/长GOP，1/2/3源，8/10bit；GPU显存和CPU内存分别统计。不用“最多48帧”冒充“实际缓存48帧”。

---

## 5. 工单：更有效的对比可视化

### V01｜局部窗直接采样解码源纹理（P0）

**问题：** 局部窗放大已缩小的全景，细线、文字、纹理可能在全景阶段被平均掉。增加窗口或nearest不能恢复已丢失信息。

**实现：** 保留RoiTriplet入口和共用ROI交互，将QML flattened snapshot路径换成D3d11ComparisonRenderer对同一已呈现FrameSet的附加绘制。在现有源SRV上分别绘制全景和2–3个局部矩形；ROI使用源坐标，不绕过规范化/色彩契约，不新增解码器。保持AVFrame/纹理生命周期锚点，不能在解码器回收后继续使用SRV。

“直接源纹理”指当前解码/规范化平面，不能据此宣称所有格式均保留了原始4:4:4或未转换码值。继续显示已转换/非精确标记。

分离全景定位变换与局部观察变换：全景保留位置感，局部共享ROI和缩放。优先共用纯坐标转换，不在QML和C++各维护一套易漂移公式。2源与3源都支持。

**缩放命名：** 适配ROI、源像素100%、200%、400%。100%按物理像素定义并考虑DPR，不能再以ROI适配冒充100%。

**预期：** 局部观察不受上方全景缩小比例限制，真实看见插帧边缘/细纹理差异。

**验收：** 1px棋盘/线条/文字、奇数尺寸、90°旋转、SAR、100–200%DPI、主窗缩放、切源。固定源像素倍率时，全景变小不能改变局部的源采样结果；只有一组播放ACK，无额外解码路数。

### V02｜统一源入口与比较选择（P0）

**问题：** A/B/GT按钮语义重复；C缺少对等入口；卡片点击做管理选择，不帮助直接比较。

**实现：** 从active sources模型生成每路同等的源按钮，标注A/B/C和GT角色。卡片主体进入/退出该源直看；独立菜单设置GT、替换文件、移除。管理批量选择进入明确模式或复选框，不再劫持普通点击。

比较对选择显示两个真实源名称；三源可一键切A↔B、A↔C、B↔C，并提供固定GT切候选。明确区分当前直看、当前比较对、参考角色、时间线主源四种状态。原位切换不seek、不改GT、不重置ROI、不重新打开视频。

卡片拖动若继续保留，应同步presentation顺序与画面顺序，但不改SourceIdentity和canonical。暂不能同步则禁用误导性拖动；不做看似排序实际只改标签条的交互。

**代码落点：** ActiveSourceStrip、CompareModeBar、ReviewShellController、ComparisonSelection、presentation contract。

**预期：** 看任意源1次点击、切候选1次、显式选择比较对不超过2次；用户知道自己正在看什么。

**验收：** GT分别为A/B/C/无GT；全部比较对；长文件名；同名不同目录；换源失败；拖动排序；回到原模式保持坐标。双击100%/适应等现有动作不得被无提示改写。

### V03｜把已有高级能力变成任务入口（P1）

**问题：** 8种模式平铺难懂，关键局部对比藏“…”导致发现性差。

**实现：** 顶层按任务组织为并排、划线、局部联动、差异定位。内部继续复用现有枚举，不改播放器域模型。三源差异默认引导到以GT为基准的双差异；无GT只描述源间差异。

三联、参考聚焦、分析网格保留在布局子菜单，而不是与所有任务按钮混排。诊断卡是按需展开的附属区域，不再加第二条主时间轴。2源局部默认两格，3源三格。

**预期：** 常用工作流最多一级入口；新增能力不增加默认界面负担。

**验收：** 不依赖帮助文档，完成选同一局部、在候选间切换、看差异、返回原图四项任务。菜单可达不等于主画布实际生效，必须验收实际渲染。

### V04｜双差异显示口径与附近帧可读性（P1）

**问题：** 有两个差异窗仍可能因为gain/阈值/采样口径不同误导；小缩略图适合定位，不适合直接判缺陷。

**实现：** 在当前DualDifference上确认共同GT、共同阈值/增益/色阶/有效区域/分析域。跨帧固定色阶；显示增益不改变指标。统一图例与缺帧/已转换标签。

NearbyFramesCard保留5–9帧定位用途，选中一格把对应真实帧送到主画布精查；可切为同ROI的连续帧条，不增加常驻解码压力。缩略图显示其源身份；未知协议不当成确定的“区间外”。批量缩略图请求独立低优先级，过期请求取消。

**预期：** 能从异常片段快速定位到同一局部，不被截图/缩略图精度或不同图例误导。

**验收：** 阈值变化、同一固定误差跨不同帧、源交换、无GT、缺帧、未加载协议、窗口快速关闭重开。检查两图同色代表同量。

---

## 6. 工单：交互与系统集成

### I01｜打开、添加、替换三个意图区分（P1）

**问题：** 默认打开应像播放器，审查任务却需要保留现场；同一拖入动作不能不明确地替换所有源。

**实现：** 系统双击单视频为OpenForPlayback；应用比较打开为OpenForReview；增加源为AppendSource；拖到明确源卡片为ReplaceSource。在UI出现清晰落点提示，支持撤销意图前取消，错误不覆盖旧会话。

单文件无需弹A/B/GT顺序确认；两三路比较才出现必要的顺序/参考设置。首次成功帧后才记录历史、选择父目录条目并开始自动播放。主窗加载期间收到外部打开请求沿现有broker排队，有界且过期结果不反向覆盖。

**预期：** 日常双击就播放；外部单文件打开明确进入新单路任务，添加/替换动作不会隐式改错比较组。仍沿用当前单会话范围，不为此引入标签页或复杂项目系统。

**验收：** 单击/双击连续打开、应用未启动/启动中/已播放、坏文件、图片/视频切换、同名文件、含空格中文路径、同时打开多文件。

### I02｜统一鼠标与快捷键（P1）

**问题：** ROI依赖Alt拖动、不一致的按钮状态和文本输入焦点会增加操作负担。

**实现：** 为框选局部提供可见按钮和清楚的光标/状态，同时保留快捷键。默认拖动平移、滚轮以指针为中心缩放、Wipe手柄区域优先拖分割线；透明HUD不拦截画布事件。双击继续100%↔适应，F11专管系统全屏。

快捷键从现有动作/目录统一导出菜单、tooltip、帮助。输入框、弹窗、图片编辑与视频走带上下文互斥；一次事件只消费一次。空格、切源等切换型动作关闭autoRepeat，避免按住反复开关。所有持有型动作都有release/cancel/focus-loss结束路径。

**预期：** 同一个动作在菜单、按钮、键盘上的效果一致；不需要猜键位。

**验收：** 弹窗中输入A/D/C/数字/空格不动视频；隐藏chrome仍可退出；视频与图片切换后无悬挂快捷键；Wipe拖动与ROI框选不会同时触发。

### I03｜Windows默认打开完整闭环（P1，可独立交付）

**问题：** 当前右键Compare动词不能代替系统默认应用注册。ZIP版路径变化也可能留下无效关联。

**实现：** 增加“设置→文件关联”：注册为可选应用、前往系统设置选择默认、修复当前路径、移除本应用注册。按用户写入ProgID、Applications/SupportedTypes、OpenWithProgids、Capabilities/FileAssociations和RegisteredApplications。仅注册经本版本验证的扩展名，视频/图片按真实能力处理。

打开命令以明确引号传路径；打开程序支持单文件，比较动词仍支持一次多选。使用系统支持的 `ms-settings:defaultapps?registeredAppUser=...` 跳本应用页，不支持时退回总页。完成系统选择后读取关联状态，不把“设置页已打开”当成功。

注册页明确说明当前为静音视觉审查工具：设为视频默认打开后不播放音频，不注册音频文件类型。视频与图片关联由用户分别选择。

不改写UserChoice、不尝试绕过其保护、不自动接管全部格式。保留ZIP方案；建议提供稳定位置使用说明与用户主动修复，不因开发目录启动就把正式版本注册抢走。关联主程序与Explorer shell DLL分开维护。

**代码落点：** shell_windows中新建与ExplorerCommandRegistration分工清楚的关联适配器；应用设置入口；既有startup broker/CLI打开路径。无需更换UI框架。

**预期：** 用户可从Windows“打开方式/默认应用”选到CompareStation，双击指定视频首帧后自动播放。

**验收：** Windows受支持版本、普通用户权限、注册失败回滚、中文/空格路径、ZIP移动/升级/删除、已有别的默认应用、两版本并存、重启系统、取消默认选择。卸载只清理本应用拥有的注册，不恢复猜测的旧默认值。

---

## 7. 工单：UI比例、风格和响应式排版

### U01｜围绕画布的响应式结构（P1）

**问题：** 当前已有固定40px素材条/模式条、268px文件栏与整窗1120阈值；更多按钮和诊断会压缩画布。不能仅把默认1440窗口做得好看。

**建议布局（Qt逻辑像素，不是源像素）：**

| 区域 | 建议 | 规则 |
|---|---|---|
| 顶部文件/源行 | 36–40高 | 单源精简，三源来源清楚，长名中间省略 |
| 比较动作行 | 36–40高 | 常用任务保留，次要动作溢出到菜单，不无限横排 |
| 左文件栏 | 默认232，可调200–288 | 可收起，不因自动扫描强制展开 |
| 右检查器 | 默认288，可调260–320 | 默认关闭；窄窗使用已有抽屉机制 |
| 主控制条 | 64–80高 | 一条主时间轴，播放/逐帧/速度/帧号为第一层 |
| 局部区 | 工作区高度约28%–40%，可拖分隔 | 只在局部任务显示，不强制固定占比 |
| 附近帧区 | 约144–184高，可折叠 | 不与大检查器无条件同时常驻 |

按“窗口宽度减去实际展开面板后的可用画布宽度”决定重排，而非只用root.width。建议以主画布至少约600逻辑像素为保护线：不足时优先收次级栏/用抽屉，不压缩文字到不可读。临时收起与持久偏好区分，放大窗口可恢复现场。

**代码落点：** Main.qml、CompareModeBar、ActiveSourceStrip、TabbedInspector、PlayerOsc。只提取布局纯组件，不另造会话状态。

**验收：** 960×640、1366×768、1440×900、1920×1080逻辑窗口；100/125/150/200%DPI；长中文文件名；双栏展开；窗口跨屏。没有按钮重叠、裁断、看不见的可点击区或过宽空白。

### U02｜在现有主题上补设计系统（P1）

**问题：** 已有语义色，但尺寸/字体散落；12px常用按钮、10px诊断文字在复杂面板中不利于辨认。

**实现：** 延续VcsTheme.js，增加字体、行高、间距、图标/命中区、面板宽度token。推荐正文14、次要12、分组标题14–16；紧凑数字可用等宽数字，10px不承载关键操作或必读结论。4px微间距、8px主栅格，常用命中区至少32×32，主播放按钮40–44。

窗口/面板层级保持少量明确层次。审查画布采用中性深色，蓝色保留给焦点和选中；A/B/C色仅用于小徽标/细边，不把整个画面包上高饱和色。GT/当前比较/直看不能仅用相同颜色表达。

不引入重毛玻璃、连续背景动画、大面积阴影或视频交叉淡化。对比窗口间切源不做过渡混帧。hover/选中可使用短小动画，但移动/缩放/走带应直接跟手。

**验收：** 常态、hover、pressed、focused、checked、disabled、loading、error状态矩阵；键盘焦点可见；颜色与标签双编码；对比度与高DPI实际截图验证，不能只检查某个颜色常量是否存在。

### U03｜信息分层与空/错/忙状态（P1）

**问题：** 比较、诊断、导出、样本功能都成熟后，默认界面可能变成调试台。

**实现：** 默认只显示当前任务必要信息。普通单视频隐藏比较对和GT；比较模式显示当前源/ROI；诊断区按需显示采样条件和结论。将源码术语翻译为用户动作，例如“比较对象”“原速播放”“正在准备反向帧”“尚未确认系统呈现”。技术值保留在展开详情中。

区分首次空白、正在打开新文件、旧画面等待下一帧、打开失败、缺少某路帧。新文件打开失败保留旧会话；连续逐帧不全屏遮住旧图；忙状态说明哪项工作在进行并允许取消。普通控件隐藏不应让hover失效后立刻反复出现/消失。

**验收：** 无素材、1/2/3源、没有GT、没有协议、坏路径、源缺帧、GPU不支持、诊断未运行、窗口极小等全部状态都能理解。

---

## 8. 验收与基准：如何证明比以前更好

### 8.1 基准条件先冻结

记录SHA、dirty状态、Release/Debug、硬件、驱动、分辨率、素材编码/位深/帧率/GOP/长度、1–3源、显示器刷新率、物理/远控路径。至少H.264 1080p60覆盖1/2/3源，另加59.94、长GOP、10bit及项目支持的其他编码作为扩展。

真实播放基线优先物理显示器适配刷新率（例如60或120Hz）。台账里的远控虚拟屏条件保留为另一组测试，不能混入直连基线；不将宿主程序计时误当远控客户端呈现时间。

### 8.2 建议初始目标（待基准冻结后确认）

| 项目 | 初始目标与口径 |
|---|---|
| 主窗可交互 | 热启动P95≤1秒；受控冷进程启动P95≤2秒，冷磁盘另列 |
| 首帧 | 本地固定1080p素材，单源热打开P95≤1.5秒，三源≤2.5秒；不以启动图代替首帧 |
| 原速 | 60秒稳态媒体进度/墙钟进度在1.00±1%以内，不含显式暂停/拖动/循环接缝 |
| 多源正确性 | 混帧、无提示回退、错GT、错ROI为零 |
| 稳态播放 | 固定支持配置1/2/3源1080p60各5分钟无额外整组跳过；超载条件单列 |
| 缓存内切源 | 应用侧输入到正确帧绘制P95≤50ms，不能冒称光子延迟 |
| 缓存内逐帧 | P95≤80ms；冷seek按GOP分组，不与缓存命中混算 |
| 长按停止 | 松手后P95≤100ms停止旧走带，过期任务不继续推动画面 |
| 稳态资源 | CPU/显存/缓存受预算约束，无随播放时长增长的历史积累 |
| UI | 所列窗口/DPI下没有主要控件遮挡/截断，关键命中区和文本符合约定 |

不以降低播放精度、跳帧不计数、降低显示分辨率而未提示的方式达到性能目标。

### 8.3 ICAT对照任务

官方页面仍列ICAT 0.6.17，支持最多4路同步分屏、2路覆盖滑块、缩放平移/循环/速度调整。CompareStation这轮的领先目标限定为“用户的1–3路插帧审查工作流”，不宣称源数量或所有视频能力全面领先。

使用相同机器、同源文件、相同媒体区间、显示刷新率、观察倍率与对齐。两程序任务顺序交替，减少练习效应。任务包括：

| 任务 | 记录内容 |
|---|---|
| 从冷启动打开三个文件并原速播放 | 可交互时间、首帧时间、稳态速度、跳过情况 |
| 找到GT/A/B同一处武器边缘 | 操作次数、完成时间、是否选错时间/源/坐标 |
| 在A↔B与A↔C之间复核局部 | 切换次数、观察区域是否漂移、误判/返回成本 |
| 检查细线、纹理残留及附近帧 | 已知缺陷检出、漏检、误报，不只主观“更清晰” |
| 长按前后并回到指定帧 | 连续性、首响应、停止延迟、帧号正确性 |

共同任务中位耗时降低30%可作为研发目标，前提是误判和漏检不增加、原速播放不变差。不能拿ICAT未提供的独有功能耗时做不公平速度倍数。额外诊断/精确样本导出作为独立产品价值展示。

### 8.4 本地验证方式

沿用 `pwsh tools/build/build.ps1`、CTest、QML交互测试、WARP像素正确性测试与真实D3D11VA性能门禁。WARP只能证明相应渲染路径正确性，不能替代硬解码播放速度。一次提交的结果不能自动继承到另一SHA。

新增基准封装应只在终端输出必要摘要；可保留本地诊断文件，但不要求用户上传日志/ZIP。下面是**未来摘要格式示意，不是已实现命令或实测数据**：

```text
BUILD sha=... dirty=... type=Release
DISPLAY path=physical|remote hz=... timing_scope=application_ack|system
OPEN sources=... shell_p95_ms=... first_frame_p95_ms=...
PLAY speed_ratio=... frame_sets_skipped=... mismatch_count=...
JOG direction=... hold_fps=... stop_p95_ms=... stale_updates=...
CACHE set_hit=... source_hit=... cpu_peak_mb=... gpu_peak_mb=...
RESULT PASS|FAIL|UNVERIFIED reason=...
```

---

## 9. 实施顺序与提交边界

| 阶段 | 工单 | 可交付结果 | 门禁 |
|---|---|---|---|
| R0：冻结事实 | B台账清理、E03指标基线、ICAT基准素材 | 一张当前状态表和可重复终端摘要 | 未实测项明确保持未知 |
| R1：减负和正确性 | E01/E02、V01 | 普通启动不承担重诊断；局部真正取源纹理 | 不破坏现有三源原子发布、正确像素和关闭流程 |
| R2：连续浏览 | E04/E05 | 单击/长按/松手闭环；反向窗口与缓存改善 | 缓存内连续、冷GOP有界、过期请求不追跑 |
| R3：比较交互 | V02/V03/V04、I01/I02 | A/B/C对等入口、二三源局部、任务化模式 | GT/比较对/时间/ROI不混淆 |
| R4：UI与Windows | U01/U02/U03、I03 | 响应式布局、设计token、系统默认打开 | 窗口/DPI与实际Explorer验收 |
| R5：对照发布 | 完整本地回归与ICAT任务对照 | 只对实测通过的场景宣称改进 | 性能不回退、效率提升、无新增误判 |

I03是相对独立的小改，可与R2/R3并行。不要在同一提交同时改Input生命周期、PlaybackCoordinator状态所有权和GPU后端。V01只换局部绘制，不重写全播放器。每个工单提交包含问题、源码证据、行为契约、测试、真机状态、开关/回滚说明。

## 10. 本轮明确不做

不新增评分模型或继续扩大SAM能力；不迁移D3D12/Vulkan；不转Electron/WinUI重写；不扩成8路以上；不以4K/8K作为当前主门槛；不添加云协作、复杂媒体库或全项目保存体系。

AI剪裁等已有可选功能仍应遵守“不阻塞UI/播放”的约束，但本轮不扩模型范围。首先把用户每天都操作的“打开—播放—长按—框选—切源”做完整。

## 11. 完成定义

当用户能够：双击一个视频迅速进入原速播放；打开三路后不费力确认源身份；长按前后连续浏览且松手停止；一次框选看到三路真正的源像素局部；一键切换候选而不丢时间和观察位置；在普通窗口与高DPI下清楚操作；通过Windows系统选择默认打开——并且这些行为有当前SHA的真实验证，才算本轮完成。

“加入了代码”“通过契约测试”“显示了一个新面板”不能单独作为完成标准。

---

## 附录A：核查源码索引

以下路径均锁定于本计划SHA；开发前重新获取HEAD和工作区diff，若已有新实现，先合并状态而不是覆盖。

- [docs/engineering/capability-ledger.md](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/docs/engineering/capability-ledger.md)：真机验证边界、虚拟显示环境、已实现与未接线状态。
- [src/ui_qml/src/DesktopApplication.cpp](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/src/ui_qml/src/DesktopApplication.cpp)：诊断控制器在QML加载前创建；startup milestones。
- [src/ui_qml/src/TimingDiagnosticsController.cpp](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/src/ui_qml/src/TimingDiagnosticsController.cpp)：构造探测、monitoring开关、observer回调、500ms刷新。
- [src/application/src/PlaybackTrace.cpp](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/src/application/src/PlaybackTrace.cpp)：observer回调路径与锁。
- [src/application/include/dvs/application/PlaybackEvidence.h](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/src/application/include/dvs/application/PlaybackEvidence.h)：collector线程契约与有界统计。
- [src/platform_windows/src/PresentationStatisticsProbe.cpp](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/src/platform_windows/src/PresentationStatisticsProbe.cpp)：独立隐藏窗口/设备/交换链探测。
- [src/ui_qml/qml/ReviewShortcuts.qml](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/src/ui_qml/qml/ReviewShortcuts.qml)：OS auto-repeat逐帧及既有快捷键。
- [src/media_ffmpeg/src/MultiSourceFrameProvider.cpp](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/src/media_ffmpeg/src/MultiSourceFrameProvider.cpp)：4组帧缓存、12MiB源缓存、FIFO淘汰。
- [src/media_ffmpeg/src/SourceDecodeActor.cpp](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/src/media_ffmpeg/src/SourceDecodeActor.cpp)：反向48帧上限与80ms软填充预算。
- [src/ui_qml/qml/RoiTripletStrip.qml](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/src/ui_qml/qml/RoiTripletStrip.qml)：flattened全景采样、DPR纹理大小、100%=ROI适配。
- [src/ui_qml/qml/CompareModeBar.qml](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/src/ui_qml/qml/CompareModeBar.qml)：A/B/GT直看、局部和双差异入口。
- [src/ui_qml/qml/ActiveSourceStrip.qml](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/src/ui_qml/qml/ActiveSourceStrip.qml)：卡片管理选择和仅卡片展示顺序。
- [src/ui_qml/qml/NearbyFramesCard.qml](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/src/ui_qml/qml/NearbyFramesCard.qml)：附近帧缩略图、协议来源、区间交互。
- [src/ui_qml/qml/Main.qml](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/src/ui_qml/qml/Main.qml)：主窗尺寸、侧栏、模式条与真实组件接线。
- [src/ui_qml/qml/VcsTheme.js](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/src/ui_qml/qml/VcsTheme.js)：既有语义色、圆角和源身份颜色。
- [src/shell_windows/ExplorerCommandRegistration.cpp](https://github.com/sonwe1e/VideoCompareStation/blob/a3a06eea20daf58ef757c03cd9649bb91128b843/src/shell_windows/ExplorerCommandRegistration.cpp)：现有Explorer右键动词注册。


## 附录B：公开技术依据

- [NVIDIA ICAT官方页面](https://www.nvidia.com/en-us/geforce/technologies/icat/)：本次核对的版本入口0.6.17、最多4路分屏与2路滑块覆盖。没有实际运行ICAT。
- [Qt Shortcut](https://doc.qt.io/qt-6/qml-qtquick-shortcut.html)：autoRepeat默认true；不是媒体时钟或KeyRelease状态机。项目仍沿用锁定Qt版本，不要求为计划升级。
- [Qt QQuickWindow](https://doc.qt.io/qt-6/qquickwindow.html)：渲染循环信号语义；提交不能直接等同物理上屏。
- [DXGI GetFrameStatistics](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getframestatistics)：交换链呈现统计的能力与条件。
- [Windows默认应用设置跳转](https://learn.microsoft.com/en-us/windows/apps/develop/launch/launch-default-apps-settings)：registeredAppUser等查询参数和支持系统范围。
- [Windows Application Registration](https://learn.microsoft.com/en-us/windows/win32/shell/app-registration)：应用注册、OpenWith、App Paths及secondary verb区分。
- [Windows File Types](https://learn.microsoft.com/en-us/windows/win32/shell/fa-file-types)：ProgID、OpenWithProgids、按用户Classes、关联变更通知。

本计划中的目标值、布局尺寸、工作顺序为设计建议；源码事实以附录A固定SHA为依据，系统API以官方文档为依据。公开文档的“最新”内容可能变化，实施时按项目支持的Windows/Qt版本再次核实。
