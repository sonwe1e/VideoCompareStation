# CI 与发布流水线效率改进

更新：2026-10-08，源自 v2.3.0 发布轮（#38–#41 合入 + Windows 验证 + 三轮标签流水线）。
本文记录该轮实际发生的低效点与踩坑、当时付出的时间代价、已落实的修复，以及按优先级
排列的改进方案；后续发布轮复核本文，落地一项勾一项。与本文互补：构建诊断见
[building](../building.md)，runner 与发布合同见 [self-hosted-runner](self-hosted-runner.md)，
发布流程约定见 [versioning](../versioning.md)。

## 本轮成本核算（证据）

- 四个 PR 全部带 CI skip 标记合入，main 推送成为它们第一次 CI 暴露：此后**六次 main
  推送**逐层剥出问题（视口测试缺导入路径 → 来源条/契约套件超时 → 侧栏菜单布局竞速 →
  剪贴板抓帧环境失败 → ui-dev `add_test` 未守卫 → ui-dev 适配器头编译失败），每层
  一次完整 CI 轮。
- v2.3.0 标签打了**三次**：第一轮检出未修复代码（主动撤下），第二轮检出剪贴板修复
  前的代码（主动撤下），第三轮才携带全部修复。单轮标签流水线约 2.5 小时（串行）。
- 单台自托管 runner 串行：main 推送作业排在标签流水线前面（v2.2.0、v2.3.0 都因此
  空等约 1 小时）；一次纯文档推送（a0a0440）也占满 runner 约 1 小时。
- 发布期间监控曾有盲区：watcher 只盯 release run，main CI 红了近一小时未察觉
  （后补双通道脚本，见 P1-3）。

## 已落实（本轮内）

- 超时预算按 runner 实测放宽：MainQmlContract 20→120 s、来源条 60→180 s（d137aa1）。
- 真窗口抓帧/剪贴板类测试增加能力自检：仅在生成代码自报捕获失败时显式 skip 并记录
  （3725af9）。
- ui-dev 模型-only 配置的两处破坏修复：侧栏 QML 注册挪入 `if(DVS_BUILD_ADAPTERS)`
  守卫（40c0736）；`IssueLogControllerTests.cpp` 挪入同守卫的 target_sources
  （1abdf43）。
- 文档推送不再触发完整构建矩阵：build-test.yml 增加 `paths-ignore`
  （docs/**、\*\*.md、\*\*.txt）。quality.yml **不**加忽略——它的 guide/script
  contract 检查恰恰校验文档本身，且该作业本来就快。

## 改进方案

### P1 小改动，建议下一轮发布前落地

1. **本地预设扫检**：一次性 `--fresh` 配置 `core-dev` / `dev` / `release` / `ui-dev`
   四个预设（只配置不构建），推送前即可抓住 CMake/register 层破坏——本轮 ui-dev 两处
   破坏在本地各花 1 分钟就能发现，却在 CI 上各花了一轮。可做成
   `tools/ci/check-configure-presets.ps1` 并在 agent-guide 提交清单里点名。
2. **发布期监控清单与工具**：把“标签推送后必须同时盯 release run 与 main 推送 CI”
   写进 versioning.md；把本轮一次性 watcher 脚本产品化为
   `tools/release/watch-release.ps1`（双通道、变红即报）。
3. **草稿验收脚本化**：把人工执行的 下载 → SHA256 → verify-release-package →
   包内启动自检 → 对上一版 ZIP 文件清单对比 串成
   `tools/release/accept-draft.ps1`，发布轮零遗漏、可复核。

### P2 需要资源或讨论，发布轮外推进

4. **第二台 Windows 自托管 runner**：结构性瓶颈。Debug/Release 两个 job 可并行，
   硬件门禁可挪到独立机器不再阻塞其它作业；主流水线墙钟预计近乎减半。
5. **PR 合并前的轻量真窗口 CI 通道**：PR 作者在 Linux offscreen + 旧 Qt 验证，与
   生产环境（Windows/MSVC/固定 Qt/真实窗口）系统性错位，本轮所有超时与环境类红都源于
   此。给 PR 提供一条可触发的 runner 组件套件通道（label 或 workflow_run 触发），
   填补“本地已验”与“发布门禁”之间的空档，避免合并后的第一次 CI 暴露。
6. **分支卫生**：仓库设置开启“合并后自动删除分支”；每次发布验收后清扫一次残留分支
   （本轮清理清单见工程台账 2026-10-08 条目）。

### P3 原则性约定（记录即可，不需要工单）

7. **超时预算以最慢核准环境为准**并留 >2 倍余量；新套件落地时在 runner 上标定一次。
8. **真窗口测试对桌面能力自检**（渲染、剪贴板等），能力缺失时以显式 skip 记录原因，
  不计为通过，也不掩盖真实断言。
9. **候选冻结纪律**：打标签前，目标提交在 main 的 CI 必须已经全绿——本轮第三轮才做
  到，前两轮的撤销本可避免。
10. **小改动也要全量 CI 的政策保持不变**：本轮五个“小”修复各自在 CI 上抓到了本地
    962 项全绿抓不到的环境层问题；省时间的正确方向是 P2-4/P2-5 的资源投入，而不是
    缩小门禁覆盖。唯一例外是纯文档推送（已落地）。
