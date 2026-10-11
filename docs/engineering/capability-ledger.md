# 能力台账（M0 / P01 基线冻结）

更新：2026-10-10。依据：[长期产品与技术路线](../CompareStation_长期产品与技术路线_2026-10-10.md) 第 10 节 M0
与附录 B 工单 P01。本文回答"每个路线能力现在到底处于什么状态"，避免重复开发与口径混淆。
行为证据以源码与测试为准；性能数字只认真实执行记录，本文不生成新数字。

## 状态定义

| 状态 | 含义 |
|---|---|
| 已实现 | 代码存在、接入主界面/CLI、有回归测试；但不等于真机性能已验证 |
| 仅接口 | 类型/方法/枚举存在，但行为不完整或调用方缺失 |
| 未接 UI | 核心逻辑存在并测试，主界面没有入口 |
| 未真机验证 | 有实现，但缺少真实硬件（D3D11VA/真实刷新率）下的执行记录 |
| 未开始 | 路线中的新能力，当前代码没有对应物 |

## 环境基线（2026-10-10 记录）

- HEAD：`fc630c0`（`fix(ui): keep smoke runs off stored resume positions`），版本 2.4.0。
- 2026-10-11 更新：路线工单 P02—P12 代码与 SAM2 分割已陆续入库（`7604740`…`9b6f500`）；工具链、
  锁定依赖与本机硬件未变，未执行项结论不变。
- 工具链：MSVC 14.44（VS 2022 BuildTools）、CMake presets、Ninja、clang-format 19.1.5；pwsh 7 wrapper。
- 锁定依赖：FFmpeg ≥ 8.1.2（avcodec/avformat/swscale，动态库）、Qt base ≥ 6.11.1、nlohmann-json、GoogleTest。
- 本机：i7-13700KF；NVIDIA GeForce RTX 4090（驱动 32.0.16.1074），4K@143。
- 显示链路含 OrayIddDriver 与 GameViewer 虚拟显示适配器（4K@143/144）：当前桌面经远控虚拟屏呈现。
  这会影响 D01 系统呈现证据与性能门禁的可信度；正式性能基线必须记录实际呈现路径，
  或在直连物理显示器时重新冻结。
- 未执行项：真实 D3D11VA 三源 1080p60 五分钟稳态门禁、1080p120 门禁、ICAT 0.6.17 同机同素材对照
  均未在本次台账更新中运行，状态保持"未真机验证"。既有门禁要求见
  [AGENTS.md](../../AGENTS.md) 与 [self-hosted runner](self-hosted-runner.md)。

## 能力台账（按路线编号）

| 路线项 | 状态 | 现状与证据 |
|---|---|---|
| F01 插帧协议 | 已实现（未真机走查） | P02 domain 纯数据结构；P03 已交付导入（persistence_json `InterpolationProtocolRepository`，decimation/explicit 双 schema、坏输入全拒绝）+ `InterpolationProtocolController` + 走带条"下一/上一生成帧"导航与来源徽标（经 coordinator 命令链、1 起计显示）。真机走带条交互走查与截图未做 |
| F02 色彩/像素/坐标契约 | 部分 | 已有规范化帧值 YCbCr 读数与"已转换"标记（台账 2026-10-09 一致性轮）、`ComparisonExactness`、8/16-bit 差异统计口径。4:2:2/4:4:4 显式警告、解码源域记录完整化未开始 |
| F03 播放预算与缓存 | 已实现（既有） | 256 MiB 解码帧缓存上限、完整 FrameSet 原子发布、呈现 ACK 背压均在门禁内；短区间预缓存容量计算与局部缓存未开始 |
| V01 全景+同 ROI 局部窗 | 部分已实现 | P04 已交付 `RoiTriplet` 预设（全景走三列/两列保源全呈现 + 同 ROI 局部窗、无新增解码路数）。V01 重写局部绘制路径：`RoiTripletStrip` 不再对全景 flattened snapshot 重采样（删除 .qsb 快照 shader），改为 `D3d11ComparisonRenderer` 对同一已呈现 FrameSet 的附加绘制——`SurfaceRenderState.localWindows`（strip 暂存窗口 contentRect 到 surface 坐标）→ 共用纯函数 `surfaceLocalWindowGeometry`（定向 ROI 半开钳制、整数裁剪居 ROI 中心、SAR 横向、物理像素档位 100/200/400 与 DPR 对齐、`alignLocalWindowFootage` 整物理像素对齐）→ 同一 render() 内的 NV12 附加绘制（同一组 SRV、同一呈现 ACK，视频常量缓冲扩到 6 组，逐绘制采样器）。缩放命名：适配ROI（0）/源像素100%/200%/400%，100%=纵向源像素 1:1（横向按 SAR），不以 ROI 适配冒充 100%；QML 不再持有采样公式，全景定位变换与局部观察变换分离（局部采样与全景 viewTransform 无关，WARP 回读钉死）。2 源与 3 源均可用（2 源全景走 SideBySide）。全景 ROI 轮廓改由 surface 用同一套纯函数发布（`roiTripletOutlineRects`）。WARP 像素回读：1px 棋盘 texel 精确、全景缩放不变性、90° 旋转、SAR、奇数宽末列；几何单测 + QML strip 契约 + MainQmlContractTests。真机 D3D11VA、100–200% DPI、主窗缩放、切源走查未验证 |
| V02 全画布切换与盲比 | 部分已实现 | P05 已交付全画布直看闭环：domain `CanvasFocusRequest`/`resolveCanvasFocusTarget`（角色/槽位解析，拒绝而不替代）、shell `canvasFocus` 显示状态（零回放命令、焦点按冻结身份失效清理）、模式条 看A/看B/看GT 入口、Main.qml 正式接线与画布渲染（`ComparisonSurface.canvasFocusSlot` → `SurfaceRenderState`/平台几何与渲染：聚焦态单面板全画布、aspect fit+letterbox、缺帧画黑不回退；分屏线/Wipe 手柄/ROI 三联条聚焦期隐藏，像素读数映射聚焦源并在切换时挂起重取）。不变量测试钉死不 seek/不改 GT/不改比较对/ROI 缩放平移保持/effectiveViewMode 不变；含真实接线测试与 WARP 渲染测试，变异 8/8（`out/verification/p05-mutation/`）。真机 D3D11VA 手感未验；盲比随机种子、顺序播放比较未做 |
| V03 双参考差异 | 部分 | A−B 差异/热力图/阈值已有（`TabbedInspector`、`PairMetricsService`）；GT−A/GT−B 成对布局、跨帧锁定标尺未做 |
| V04 编辑副本比较 | 部分 | 编辑副本、撤销、另存副本已有（台账 2026-10-10 图片连续裁剪与整图变换）；"副本作为候选进入比较管线"未接 |
| D01 播放时序证据 | 部分已实现 | 应用层 trace（kind 2/23/24，`PlaybackTrace.h`）与呈现 ACK 已有；P09 `PlaybackEvidenceCollector`（分段延迟/帧号缺口跳帧/身份过滤/分级摘要，13 单测+6 变异）已实现未接线。P10 平台探针 PoC `PresentationStatisticsProbe`（自有离屏 flip 交换链、七态分级能力、QPC/PresentCount 快照、区间级关联声明、10 测试+5 变异）已实现未接线；本机硬件路径 kMediaAvailable。远控虚拟屏会话下传统计数器可能滞后于验证窗口→诚实报 kUnknown（测试按结构性断言，不对机器答案下断言）。PresentMon/ETW 采集未开始 |
| D02 异常标记诊断卡 | 已实现（未真机走查） | `IssueLogController` 问题标记、In/Out 区间、缓存已有。P06 已完整落地（步骤 1—7）：domain `IssueIntervalContext`（锚定已呈现帧、绑定 sessionEpoch/playbackGeneration）+ `clampIssueInterval` 纯函数（时间线边缘截断、不移锚、无效锚保持未捕获、默认半径 4 覆盖 5-9 帧）+ `IssueRecordRepository` 往返持久化（向前兼容旧记录）；捕获接线 `captureVideoIssue` 从会话快照填充区间与循环接缝判定（`loopSeamAtOut`）并暴露 `intervalAt`；`NearbyFramesCard.qml` 诊断卡组件（5-9 帧胶片条、retained/generated/missing/outside/unknown 五色来源徽章、锚点高亮、循环接缝标识、跨会话 epoch 不匹配告警、单帧步进/锚点跳转/区间循环播放控制）+ `Main.qml` 接线与问题行诊断入口。7 项组件/单元测试 + QML 契约测试通过。真机走查与长序列交互体验未做 |
| D03 运动进度偏差 | 未开始 | OpenCV 依赖未引入 |
| D04 纹理残留/重影 | 未开始 | 时间切片、模型中间结果接入未做 |
| D05 线条抖动 | 未开始 | — |
| D06 自动异常导航 | 未开始 | — |
| Q01 有口径结果卡 | 部分 | MAE/PSNR、阈值复用、异步采样已有（`ComparisonMetrics`、`PairMetricsService`）；通常表现/变差比例/严重问题三段式结果卡未做 |
| Q02 时间指标 | 未开始 | — |
| S01 SampleGroup/配方 | 部分已实现 | P11 已提取 `ImageEditOperations` 纯函数模块（裁剪、水平镜像/垂直翻转/90°/180° 旋转、矩形填充/清除、马赛克 + `effectiveMosaicBlockSize` + `EditRecipe` 五操作有序折叠执行器雏形）；控制器五入口（裁剪/变换/马赛克/填充/清除）全部委托纯函数，撤销/历史/副本身份不动；控制器路径与纯函数路径输出**逐字节一致**有测试证明（8 项组件测试，含 5 步控制器序列经配方重放一致），6/6 变异检出且既有控制器测试随纯模块变异同步变红（证明无内联残留），证据 `out/verification/edit-recipe-ops/`。画笔/标注/智能抠图刻意不提取（交互态/模型环境）；缩放（Resample）与填充画布（Canvas）仍内联、未入 `EditOperation`（配方执行器落地前需补）；`EditRecipe` 序列化与批量导出执行器未做 |
| S02 标签几何变换 | 部分已实现 | P12 本次新增 `src/domain` 的 `LabelGeometry`（光流端点通式 `f'(T0(x)) = T1(x+f(x)) - T0(x)`、公共仿射捷径、按网格位裁剪＋终点出界置无效、水平翻转 dx 取反、pad 不制造真值、mask 精确重排；无时间反转、无 mask 重采样）+ 15 项单测与 6/6 变异证据。接入配方执行器（S01）未开始 |
| S03 选区/抠图/AI 分割 | 部分已实现 | `1e3e04b` 已提交可选 SAM 2.1 矩形框选抠图：`Sam2Segmenter` 进程内 ONNX Runtime 纯 CPU 推理，运行时 LoadLibrary 解析 `onnxruntime.dll`（零编译/链接依赖，vendored 头入格式门禁豁免）；权重由用户手工放入 models 目录、不入库不发包；缺权重/加载失败降级为状态提示与禁用入口，基本编辑不受影响；2 项常跑测试（优雅降级、applyCutout alpha/包围盒）+1 项本机有权重即跑的 live 测试（无权重 GTEST_SKIP）。未达路线要求/未做：独立可选进程、权重下载与版本哈希锁定、多边形/笔刷选区、mask 导入、加减选区、羽化、去边、matting；点提示 `segmentPoints` 已暴露未接；**编码/解码当前在 GUI 线程同步执行（大图可冻结 UI 数秒，待改异步）**，解码异常后缓存 moved-from 投毒问题待修 |
| S04 可控合成数据 | 未开始 | — |
| E01 精确样本导出 | 部分已实现 | P07 纯规划层 `planSampleTriplet`（生成帧→三元组计划，整除性/映射验证，固定裁剪）；P08 新增 `SampleExportWriter`（display-order 帧序号精确抽帧、staging→读回校验→rename 原子发布、协作取消、manifest A4 子集 0 起计索引）+ 11 组件测试与 6/6 变异。PNG 实走 Windows WIC（仓库锁定 FFmpeg 构建无 png 编码器，avcodec 路径为未验证回退）；B 帧、10-bit、UI 入口未验证/未接。流拷贝片段导出仍保留（`ClipExportPlanner`/`ClipExportWriter`） |
| E02 批处理 | 仅接口 | `CompareStationCli --startup-check` 存在；配方批处理、断点恢复未做 |
| E03 问题包与回归 | 部分 | `IssueRecordRepository` JSON 持久化与恢复事务已有；区间/协议/采样条件字段扩展与便携问题目录未做 |
| E04 数据谱系 | 未开始 | — |

## 维护规则

- 每完成一个路线工单，更新对应行并链接台账/验证证据；状态只能因真实执行记录而变更为"已真机验证"。
- 本文不复制 [问题台账](visual-review-backlog.md) 的缺陷条目；缺陷仍走问题 ID 流程。
- 性能数字只引用 `out/verification/` 下的真实运行日志，不在本文手写。
