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
| V01 全景+同 ROI 三局部 | 部分已实现 | P04 已交付 `RoiTriplet` 预设：全景走 ThreeUp（三源全呈现）+ 三个同 ROI 局部窗（QML `RoiTripletStrip` 复采样已呈现 FrameSet，烘焙 .qsb 着色器含 d3d11 变体），无新增解码路数；往返/缩放/平移 ROI 保持、半开边界钳制、入口无三源禁用。真机视觉/DPI/截图未验证；跟随轨迹放大镜未做 |
| V02 全画布切换与盲比 | 部分已实现 | P05 已交付全画布直看闭环：domain `CanvasFocusRequest`/`resolveCanvasFocusTarget`（角色/槽位解析，拒绝而不替代）、shell `canvasFocus` 显示状态（零回放命令、焦点按冻结身份失效清理）、模式条 看A/看B/看GT 入口、Main.qml 正式接线与画布渲染（`ComparisonSurface.canvasFocusSlot` → `SurfaceRenderState`/平台几何与渲染：聚焦态单面板全画布、aspect fit+letterbox、缺帧画黑不回退；分屏线/Wipe 手柄/ROI 三联条聚焦期隐藏，像素读数映射聚焦源并在切换时挂起重取）。不变量测试钉死不 seek/不改 GT/不改比较对/ROI 缩放平移保持/effectiveViewMode 不变；含真实接线测试与 WARP 渲染测试，变异 8/8（`out/verification/p05-mutation/`）。真机 D3D11VA 手感未验；盲比随机种子、顺序播放比较未做 |
| V03 双参考差异 | 部分 | A−B 差异/热力图/阈值已有（`TabbedInspector`、`PairMetricsService`）；GT−A/GT−B 成对布局、跨帧锁定标尺未做 |
| V04 编辑副本比较 | 部分 | 编辑副本、撤销、另存副本已有（台账 2026-10-10 图片连续裁剪与整图变换）；"副本作为候选进入比较管线"未接 |
| D01 播放时序证据 | 部分已实现 | 应用层 trace（kind 2/23/24，`PlaybackTrace.h`）与呈现 ACK 已有；P09 `PlaybackEvidenceCollector`（分段延迟/帧号缺口跳帧/身份过滤/分级摘要，13 单测+6 变异）已实现未接线。P10 平台探针 PoC `PresentationStatisticsProbe`（自有离屏 flip 交换链、七态分级能力、QPC/PresentCount 快照、区间级关联声明、10 测试+5 变异）已实现未接线；本机硬件路径 kMediaAvailable。远控虚拟屏会话下传统计数器可能滞后于验证窗口→诚实报 kUnknown（测试按结构性断言，不对机器答案下断言）。PresentMon/ETW 采集未开始 |
| D02 异常标记诊断卡 | 部分 | `IssueLogController` 问题标记、In/Out 区间、缓存已有；5—9 帧胶片条诊断卡未做 |
| D03 运动进度偏差 | 未开始 | OpenCV 依赖未引入 |
| D04 纹理残留/重影 | 未开始 | 时间切片、模型中间结果接入未做 |
| D05 线条抖动 | 未开始 | — |
| D06 自动异常导航 | 未开始 | — |
| Q01 有口径结果卡 | 部分 | MAE/PSNR、阈值复用、异步采样已有（`ComparisonMetrics`、`PairMetricsService`）；通常表现/变差比例/严重问题三段式结果卡未做 |
| Q02 时间指标 | 未开始 | — |
| S01 SampleGroup/配方 | 部分已实现 | P11 已提取 `ImageEditOperations` 纯函数模块（裁剪、水平镜像/垂直翻转/90°/180° 旋转、矩形填充/清除、马赛克 + `EditRecipe` 有序折叠执行器雏形）；控制器全部委托纯函数，撤销/历史/副本身份不动；控制器路径与纯函数路径输出**逐字节一致**有测试证明。画笔/标注/智能抠图刻意不提取（交互态/模型环境）；`EditRecipe` 序列化与批量导出执行器未做 |
| S02 标签几何变换 | 部分已实现 | P12 本次新增 `src/domain` 的 `LabelGeometry`（光流端点通式 `f'(T0(x)) = T1(x+f(x)) - T0(x)`、公共仿射捷径、按网格位裁剪＋终点出界置无效、水平翻转 dx 取反、pad 不制造真值、mask 精确重排；无时间反转、无 mask 重采样）+ 15 项单测与 6/6 变异证据。接入配方执行器（S01）未开始 |
| S03 选区/抠图/AI 分割 | 未开始 | 工作区另有并行未提交的 SAM2 接入尝试（`src/ui_qml/src/Sam2Segmenter.cpp`，当前编译失败），状态以该工作完成时为准 |
| S04 可控合成数据 | 未开始 | — |
| E01 精确样本导出 | 部分已实现 | P07 纯规划层 `planSampleTriplet`（生成帧→三元组计划，整除性/映射验证，固定裁剪）；P08 新增 `SampleExportWriter`（display-order 帧序号精确抽帧、staging→读回校验→rename 原子发布、协作取消、manifest A4 子集 0 起计索引）+ 11 组件测试与 6/6 变异。PNG 实走 Windows WIC（仓库锁定 FFmpeg 构建无 png 编码器，avcodec 路径为未验证回退）；B 帧、10-bit、UI 入口未验证/未接。流拷贝片段导出仍保留（`ClipExportPlanner`/`ClipExportWriter`） |
| E02 批处理 | 仅接口 | `CompareStationCli --startup-check` 存在；配方批处理、断点恢复未做 |
| E03 问题包与回归 | 部分 | `IssueRecordRepository` JSON 持久化与恢复事务已有；区间/协议/采样条件字段扩展与便携问题目录未做 |
| E04 数据谱系 | 未开始 | — |

## 维护规则

- 每完成一个路线工单，更新对应行并链接台账/验证证据；状态只能因真实执行记录而变更为"已真机验证"。
- 本文不复制 [问题台账](visual-review-backlog.md) 的缺陷条目；缺陷仍走问题 ID 流程。
- 性能数字只引用 `out/verification/` 下的真实运行日志，不在本文手写。
