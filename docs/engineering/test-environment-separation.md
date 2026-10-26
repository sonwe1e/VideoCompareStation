# 测试环境与运行位置

本页规定运行位置；验证范围以 [构建指南的选择表](../building.md#开发循环与完整验收)
和 [AGENTS.md](../../AGENTS.md) 为准。2026-10-18 的迁移批次已结束，不再以当时
未取得云端结果为由阻塞之后的定向开发验证，也不沿用当时的 CTest 数量。

| 工作 | 默认位置与约束 |
|---|---|
| 文档、脚本契约等轻量检查 | 可在主力机执行；文档修改不触发媒体/UI 全量构建 |
| 日常构建与定向测试 | 按影响范围使用 `tools/build/build.ps1`；每个 preset 串行，CPU/优先级受包装器约束 |
| 完整软件回归、长时间 lint | 默认托管 Windows CI 或独立测试机；不要为小改动自动占用主力机跑全量 |
| 可见窗口、弹窗/焦点/截图验收 | 独立交互桌面，或用户已安排的主力机独占时段；沿用已有任务授权，不重复请求 |
| D3D11VA、真实呈现、跨屏与物理颜色 | 符合 [runner 协议](../self-hosted-runner.md) 的专用 GPU/显示环境；主力机不自动执行 |

`ui.MainQmlContractTests`、`ui.ImageWorkspaceManual` 与部分 smoke 会创建真实窗口，
不能仅凭“单元/组件”标签判断无窗口。布局、弹窗、主题与 DPI 修改必须保留受影响套件的
样式/DPI 矩阵（使用命名组时加 `-FullMatrix`），运行位置不能成为省略矩阵的理由。
未具备环境时，明确留下哪项待验，不用纯 CPU/WARP 结果替代。

同机 VM、第二显示器或最小化窗口不自动构成资源隔离。虚拟/远程显示也不自动具备
硬件呈现与物理颜色验收资格。性能数据记录实际路径，不能只记录显卡型号。

## CI 路由与数量

普通 Windows 软件作业在 `windows-2022`；Linux core 在 `ubuntu-24.04`。
GPU 作业由 `.github/workflows/hardware-performance.yml` 手动触发到
`[self-hosted, Windows, X64, dvs-gpu]`。发布流程核验同一提交的硬件证据；
某次发布的明确例外只适用于该次发布。

路由由 `check-ci-workflows.py`、`check-ci-test-routing.py`、`check-ui-test-routing.py`
以及 `tools/ci/test-hardware-evidence.ps1` 约束。实际作业以 `.github/workflows/`
为准，详细设置见 [托管 Windows CI](hosted-windows-ci.md)。
测试数量从当前构建枚举并与实际结果核对，不在本页维护会随代码漂移的计数副本。

证据按 [可复现记录方法](reproducible-baseline.md) 绑定 SHA、diff、构建、素材和环境。
