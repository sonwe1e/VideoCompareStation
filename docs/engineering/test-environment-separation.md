# 测试环境分离与运行位置（阶段 0）

更新日期：2026-10-17。状态：审计与清单完成，迁移未执行。本页把
[收尾后分阶段改进计划](../plans/CompareStation_收尾后分阶段改进计划_2026-10-17.md) 的
“主力电脑使用约束与测试环境分离”落成可核验的清单；它不修改 workflow、runner 或系统设置，
因此在迁移完成前，相关门禁保持待验，不回退到主力桌面自动执行。

## 1. 当前 CTest 规模

`dev` 预设当前注册 **1433** 项测试（`ctest --test-dir out/build/dev -N`）。分组只是路由起点，
不证明某一项无窗口或低负载；执行前按具体 suite 核对。规模最大的几组：

| 组 | 注册数 | 示例 |
|---|---|---|
| `ui.MainQmlContractTests` | 140 | 真实 `Main.qml`/`QQuickWindow`，部分用例显式 `show()` |
| `application.PlaybackCoordinatorTests` | 100 | 纯 CPU，无窗口 |
| `ui.ReviewControllerTests` | 79 | GUI 线程投影桥，无主窗 |
| `ui.ImageReviewControllerTests` | 59 | `QCoreApplication` + CPU 图片，无窗口 |
| `platform.ComparisonSurface*` | 89 | D3D11/WARP 表面；非真实显示验收 |
| `ui.ImageEditControllerTests` | 42 | CPU 编辑副本，无窗口 |
| `media.*` | 约 250 | FFmpeg 软解/写作，CPU |
| `shell_windows.*` | 约 35 | 注册表/Explorer，需要真实用户上下文 |
| `hardware.*` / `performance.*` | 8 / 6 | 真实 GPU、D3D11VA、呈现与显示协议 |

## 2. 测试分层与默认运行位置

| 层次 | 内容 | 默认运行位置 | 主力机规则 |
|---|---|---|---|
| 轻量静态检查 | `quality.script-integrity`、`quality.hardcoded-paths`、`quality.msvc_dependencies`、`quality.qmltypes_contract`、文档链接 | 任意环境（含主力机），纯读取 | 可执行；不启动 GUI |
| 开发定向验证 | 受影响的最小 CPU 用例，例如 `ui.ImageColorContractTests`、`ui.ImageReviewControllerTests`、`media.StillImageDecoderTests`、`media.SampleExportWriterTests` | 已确认无窗口时可在主力机；其余走独立环境 | 串行使用同一构建目录；不用 `-FullMatrix` |
| 完整软件回归 | 全部测试、Debug/Release、样式/DPI 矩阵、format-check/lint | 普通 Windows 云端 runner 或独立测试机 | 不在主力机自动执行 |
| 可见窗口验收 | `ui.MainQmlContractTests`、`ui.ImageWorkspaceManual`、`app.ui-*-smoke`、`package.popup-pixels-*`、弹窗/焦点/截图 | 独立 Windows 交互桌面 | 需用户明确允许的独占时段与事先列出的范围 |
| GPU 与物理显示 | `hardware.*`、`performance.*`、D3D11VA、跨屏、真实 ICC/HDR 状态 | 符合协议的专用 Windows GPU/显示环境 | 不在主力机执行；WARP/VM 不替代 |

注意：

- “属于单元/组件分组”不等于无窗口。`tests/component/ui` 下多个 suite 会创建真实主窗。
- `ui.ImageWorkspaceManual` 使用仓库自建 QuickTest runner，持有真实控制器和窗口；不能当作后台测试。
- 虚拟显示适配器、最小化窗口、第二显示器或同机 VM 不构成资源隔离证明；VM 可承担适配过的普通测试，
  不自动取得 D3D11VA/真实呈现或物理色彩验收资格。
- 性能正式采样仍按 [runner 与发布门禁](self-hosted-runner.md) 的硬件协议运行。

## 3. workflow 路由审计（2026-10-17）

| 文件 | 作业 | `runs-on` |
|---|---|---|
| `.github/workflows/build-test.yml` | Windows 构建/测试 | `[self-hosted, Windows, X64, dvs-toolchain-4.4]` |
| `.github/workflows/build-test.yml` | Linux core 检查 | `ubuntu-24.04` |
| `.github/workflows/quality.yml` | static/质量任务 | `windows-2022` |
| `.github/workflows/quality.yml` | native-quality | `[self-hosted, Windows, X64, dvs-toolchain-4.4]` |
| `.github/workflows/release.yml` | release 构建/打包/验证（三处） | `[self-hosted, Windows, X64, dvs-toolchain-4.4]` |
| `.github/workflows/hardware-performance.yml` | 硬件性能 | `[self-hosted, Windows, X64, dvs-gpu]` |

结论：Windows 构建、native-quality、release 和硬件作业仍在 self-hosted 标签上。
“已交给 GitHub Actions”不等于离开主力电脑；必须核对实际接单机器身份，不能只给旧 runner
改显示名称而仍让它匹配重型任务标签。

## 4. 迁移验收要求（未完成）

- [ ] 普通 Windows 软件作业优先迁移到兼容云端 Windows 或独立测试机；先验证锁定工具链、依赖缓存、
      LFS、打包与测试环境，再切换路由。Linux/core 检查继续提供反馈，但不替代 Windows UI 验收。
- [ ] 用专用标签/runner group 明确隔离主力机；加入能检测误路由的检查，必须是正例 + 反例、
      固定检查数，并覆盖全部 workflow（漏掉任一文件即失败）。
- [ ] 日志记录实际 runner 名称/机器；一次普通提交不得在主力桌面启动窗口。
- [ ] 同机不可避免的构建与定向测试保守并行、低优先级，并串行使用同一构建目录；`-TestJobs 1`
      只控制测试并行，不是 CPU/内存总限额。
- [ ] 先定向验证、批次稳定后再完整门禁；结果绑定提交、diff、依赖和构建配置；相同输入的成功结果
      不重复跑，相关输入改变后重新判断影响。

在以上完成前：只执行不启动测试进程的代码/文档工作与轻量检查；完整回归或 GUI/硬件任务保留
待验记录，不自行推断“现在空闲”或安排夜间运行。

## 5. 证据与未决项

- 本页的 CTest 计数与 workflow 行号来自 2026-10-17 的只读检查；路由或测试注册变化后必须重测。
- 尚未取得“普通提交在指定环境完成对应检查、且主力机不接单”的运行日志，因此阶段 0 的
  环境分离退出条件仍未满足。
- 硬件/物理显示基线（显示器型号、刷新率、HDR/Advanced Color、ICC 环境）仍未登记，见
  [可复现基线](reproducible-baseline.md) §2。
