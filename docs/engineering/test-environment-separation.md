# 测试环境分离与运行位置（阶段 0）

更新日期：2026-10-18。状态：普通软件门禁已迁移到 GitHub 托管环境（合并 `66475bb`）；
合并后的提交尚未取得云端结果。本页把
[收尾后分阶段改进计划](../plans/CompareStation_收尾后分阶段改进计划_2026-10-17.md) 的
“主力电脑使用约束与测试环境分离”落成可核验的清单；它不修改 workflow、runner 或系统设置。

## 1. 当前 CTest 规模

`dev` 预设在本机合并前注册 **1433** 项测试（`ctest --test-dir out/build/dev -N`，2026-10-17）；
main 的托管门禁在 `66475bb` 上固定断言 **1409** 总数 / **1387** 软件集。两个数字对应不同
SHA 与不同矩阵配置，**都不是**合并后的清单。2026-10-18 的合并（`1f5da53`）新增了 55 个用例，
合并后的真实数量必须在云端重新枚举后才可引用；本机不执行 `ctest`，因此本页不再给出合并后
的总数。分组只是路由起点，不证明某一项无窗口或低负载；执行前按具体 suite 核对。规模最大的几组：

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

## 3. workflow 路由审计（2026-10-18，合并 `66475bb` 之后）

| 文件 | 作业 | `runs-on` |
|---|---|---|
| `.github/workflows/build-test.yml` | `build-test`（Debug/Release 完整软件回归） | `windows-2022` |
| `.github/workflows/build-test.yml` | `core-windows`（Windows core-only） | `windows-2022` |
| `.github/workflows/build-test.yml` | `ui-models`（UI model-only） | `windows-2022` |
| `.github/workflows/build-test.yml` | `core-linux` | `ubuntu-24.04` |
| `.github/workflows/quality.yml` | `repository-guide`、`native-quality` | `windows-2022` |
| `.github/workflows/release.yml` | `validate-version`、`quality`、`hardware-performance`、`release` | `windows-2022` |
| `.github/workflows/hardware-performance.yml` | 硬件性能（仅 `workflow_dispatch`） | `[self-hosted, Windows, X64, dvs-gpu]` |

结论：普通软件作业已全部离开 self-hosted，唯一保留的自托管标签是 GPU 作业，且只能由
`workflow_dispatch` 手动启动。`release.yml` 的 `hardware-performance` 作业本身跑在托管
runner 上，只核验同一提交先前成功的硬件证据，不重新占用主力电脑。

PR #52 记录的实际云端结果（绑定 head `4f2e0dd`，不是合并后的 `1f5da53`）：Debug 1387 项
清单中 1373 通过、10 跳过、4 禁用、0 失败；Release 1372 通过、11 跳过、4 禁用、0 失败；
Windows core-only 306、Linux core 306、UI model-only 424 通过。这些数字**不能转移**到本仓库
合并后的提交，只能证明迁移机制在那一 SHA 上成立。

路由检查由 `tools/quality/check-ci-workflows.py`（70 项检查 + 16 反例）、
`tools/quality/check-ci-test-routing.py`（8 项检查 + 4 反例，含 5 个硬件用例身份）、
`tools/quality/check-ui-test-routing.py`（9 个 model-only 源 + 10 个 adapter 源 + 5 案例）
和 `tools/ci/test-hardware-evidence.ps1` 共同约束。

## 4. 迁移验收要求

- [x] 普通 Windows 软件作业迁移到云端 `windows-2022`；锁定工具链、依赖缓存、LFS、打包与测试
      环境由 `tools/ci/setup-hosted-windows.ps1` 与两个组合 action 承担。证据见
      [Windows 云端 CI](hosted-windows-ci.md) 与 PR #52 的运行记录（绑定 `4f2e0dd`）。
- [x] 主力机与重型任务用标签隔离；`check-ci-workflows.py` 以 70 项检查 + 16 反例覆盖全部
      workflow，漏掉任一文件即失败。
- [x] 日志记录实际 runner；托管作业不会在主力桌面启动窗口。
- [x] 同机不可避免的构建与定向测试仍保守并行、低优先级，并串行使用同一构建目录；
      `-TestJobs 1` 只控制测试并行，不是 CPU/内存总限额。
- [ ] 先定向验证、批次稳定后再完整门禁；结果绑定提交、diff、依赖和构建配置。此条是持续规则，
      合并后的提交（`1f5da53`）尚未取得任何云端结果，仍为未完成。

在以上最后一项完成前：只执行不启动测试进程的代码/文档工作与轻量检查；完整回归或 GUI/硬件
任务保留待验记录，不自行推断“现在空闲”或安排夜间运行。

## 5. 证据与未决项

- 本页的 workflow 路由来自 `66475bb` 的实际文件内容；CTest 计数来自合并前状态，
  合并后必须在云端重新枚举，不能用 1433 或 1409 任一数字代替。
- 迁移机制已由 PR #52 在 `4f2e0dd` 上验证；本仓库合并后的提交（`1f5da53`）**尚未运行**
  任何云端作业，阶段 0 的“结果绑定提交”退出条件仍未满足。
- 硬件/物理显示基线（显示器型号、刷新率、HDR/Advanced Color、ICC 环境）仍未登记，见
  [可复现基线](reproducible-baseline.md) §2。
