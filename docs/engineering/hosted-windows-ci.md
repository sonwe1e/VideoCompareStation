# Windows 云端 CI 与手动物理验收

2026-10-17。构建、完整软件回归与 lint 使用 GitHub 托管的 `windows-2022`，
个人电脑只承担明确手动触发的硬件与物理显示验收。此变更不降低既有测试和发布门禁。

## 调度边界

| 入口 | 运行位置 | 范围 |
|---|---|---|
| Build and Test 的 PR、main/dev push、手动运行 | GitHub 托管 Windows；core-linux 仍为 Ubuntu | Debug/Release 完整软件 preset、startup check、core-dev 与 ui-dev |
| Quality 的 PR、main/dev push、手动运行 | GitHub 托管 Windows | 仓库脚本与路由契约、完整 format-check/lint |
| Release 的版本标签 push | GitHub 托管 Windows | 校验版本与提交并运行软件质量；不调度本机硬件、不发布草稿 |
| Hardware and Performance 手动运行 | 带 `dvs-gpu` 标签的交互式 Windows runner | 原有 D3D11VA、像素、物理桌面与性能门禁 |
| Release 手动运行 | GitHub 托管 Windows | 核验先前成功的同提交硬件运行后，执行打包、smoke、soak 并发布草稿 |

硬件 workflow 仅接受 `workflow_dispatch`，移除可被标签发布流程间接调用的 `workflow_call`。
普通软件 job 不含 self-hosted 路由；共享准备 action 还会检查 `RUNNER_ENVIRONMENT`，
误路由到个人电脑时在安装、构建和测试前失败。仅创建 PR 不触发硬件测试。

## 迁移前提与固定工具

官方 Windows 2022 镜像具备 VS 2022/v143、PowerShell 7、Git LFS 与 Ninja，但预装
CMake/LLVM 版本不满足本项目要求，所以不只替换 `runs-on`。

- 使用隔离 Python 环境安装 CMake 4.4.0、Ninja 1.13.0，不改全局 Python。
- 在临时目录安装 LLVM 19.1.5，执行前验证固定 SHA256；通过 `DVS_LLVM_BIN` 优先发现固定工具，
  避免 CMake 的 VS 搜索提示优先选中预装的另一版本。缓存恢复后仍核对工具版本。
- vcpkg 检出到 `out/ci/vcpkg`，提交与 manifest baseline
  `e6ed7c5be05eaedc2d06ee3e51bd059409429c5e` 一致；不读取个人电脑的仓库变量路径。
  现有 Qt 6.11.1、FFmpeg 8.1.2#3 overrides 保留。
- 只缓存 vcpkg 二进制 archives，不搬运个人电脑的安装树、CMakeCache 或运行库。
  cache key 包含 MSVC、manifest 和准备脚本身份；vcpkg 自身继续按 ABI 验证包。
- 首次冷缓存仍需编译 Qt/FFmpeg，因此构建 job 允许 240 分钟；失败保留依赖日志。
  后续利用二进制缓存。缓存缺失时可从固定来源重建，不能改回 self-hosted 作为兜底。

来源：[Windows 镜像清单](https://github.com/actions/runner-images/blob/main/images/windows/Windows2022-Readme.md)、
[vcpkg 二进制缓存](https://learn.microsoft.com/en-us/vcpkg/reference/binarycaching)、
[LLVM 19.1.5 校验来源](https://github.com/microsoft/winget-pkgs/blob/master/manifests/l/LLVM/LLVM/19.1.5/LLVM.LLVM.installer.yaml)。

## 完整回归与验收证据

常规 CI 直接运行完整 dev/release test preset，使用 `--no-tests=error`，不再拆成仅 unit 和
component/e2e 标签筛选。preset 已有 hardware/performance/packaged/soak 排除保持原样，
软件检查不充当物理 GPU、显示器颜色或真实刷新节奏证据。core-dev/ui-dev 最小配置继续验证。

不全局启用 Qt offscreen 或 software 环境来掩盖 Windows UI 失败。云端普通窗口和软件渲染
测试结果按原规则记录；需要真实显示硬件的项目仍由手动门禁承担。JUnit、CTest 与依赖构建
日志保留 14 天。跳过和未执行不写成通过。

轻量路由检查解析 YAML，并对四个 workflow 计数。八个反例覆盖自动任务误回 self-hosted、
硬件自动触发/可复用调用、发布漏依赖、完整套件被筛选、漏 workflow、漏准备和漏质量门禁。
硬件证据检查另有正例及五个字段反例；两类检查都在云端 Quality 执行。

## 手动硬件和发布步骤

1. 在已登录、有合格物理显示输出的 runner 上准备现有工具链和素材，只有准备手动测试时
   才启动 runner。运行 Hardware and Performance 时选择待发布标签或分支，确认其完整 SHA。
2. 等待该运行完成，保存 run ID；它必须为 `workflow_dispatch`、`completed/success`，
   `head_sha` 必须与发布标签提交完全一致。不同提交的成功结果不能复用。
3. 手动运行 Release，填写现有版本标签 `tag` 与 `hardware_run_id`。发布仍要求标签指向
   当前 main、版本一致、明确 unsigned 范围，以及原有软件/打包/soak 门禁。
4. Release 通过仓库 API 验证指定运行的 workflow 路径、事件、终态、结论和提交身份，
   之后才允许创建草稿。缺少或不匹配的硬件结果会阻止发布，但不会自动启动个人电脑。

此迁移不暂停或改装个人电脑 runner，也不自动执行任何 GPU 测试。部署前若有旧配置的自动任务
排队，应单独取消；仅合入新 YAML 不会把旧运行转移到云端。
