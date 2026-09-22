# Windows self-hosted runner

CompareStation 的原生 CI 需要一台处于登录状态的 Windows x64 工作站。常规
Debug/Release、format/lint 使用 `dvs-toolchain-4.4` 标签，D3D11VA 与五分钟性能门禁
使用 `dvs-gpu` 标签；同一台工作站可以同时拥有这两个标签。

## 本机基线（2026-07-30）

当前工作站已具备项目编译和硬件门禁所需的软件：

- Windows x64、NVIDIA GeForce RTX 4090，驱动 `32.0.16.1074`；
- Visual Studio 2022 Build Tools 与 MSVC v143；
- CMake 4.4.0、Ninja 1.13.2、Git 2.47.1；
- vcpkg 位于 `G:\Workspaces\vcpkg`（安装树：`G:\Workspaces\Toy\out\vcpkg`）；
- 本地打包另有免管理员安装的 .NET SDK 8.0.423、WiX 4.0.4 与
  `WixToolset.UI.wixext` 4.0.4。

普通 build/test runner 不依赖 .NET SDK 或 WiX；只有生成 MSI 时才需要这两项。本机
原先缺少的 CI 基础设施只有 GitHub Actions runner 本体、runner 注册和九个长时素材的
稳定目录。

若重新配置机器，MSI 工具链使用以下固定版本：

```powershell
winget install --id Microsoft.DotNet.SDK.8 --exact --silent `
  --accept-package-agreements --accept-source-agreements
dotnet tool install wix --tool-path G:\GitHubActions\tools\wix --version 4.0.4
G:\GitHubActions\tools\wix\wix.exe extension add `
  --global WixToolset.UI.wixext/4.0.4
```

把 `G:\GitHubActions\tools\wix` 加入 runner 用户的 `PATH`。`v1.4.5` 的发布合同明确为
无 Authenticode 签名；runner 不需要签名证书或 `signtool.exe`。MSI 是 per-machine，
因此执行 `packaged-smoke` 的交互式 runner 进程仍必须以管理员身份启动；测试会
拒绝覆盖机器上已有的 CompareStation 安装，并在结束时卸载自己的测试安装。
Release workflow 会下载 `CompareStation-1.2.0-windows-x64.msi`，完成启动/关闭、设置保留
和两路有效 A/B Pair 检查后再安装 1.4.5，并验证旧 `.dvsproj` 注册消失，以及 ARP、
快捷方式、文件关联和版本化 Explorer Shell COM 注册正确。

## 固定下载

使用 GitHub Actions runner `2.336.0`：

```text
https://github.com/actions/runner/releases/download/v2.336.0/actions-runner-win-x64-2.336.0.zip
SHA-256 d59123a43003e357b0805b5d0f611d0bd2f65ab67d51bd070dd4e7a0f685c162
```

下载后必须先核对 SHA-256，再解压到仅供此仓库使用的目录，例如
`G:\GitHubActions\Toy-runner`。注册 token 通过仓库 API 临时生成，有效期短，不写入
仓库、脚本或日志：

```powershell
$runnerRoot = 'G:\GitHubActions\Toy-runner'
$token = gh api --method POST `
  repos/sonwe1e/CompareStation/actions/runners/registration-token --jq .token

Set-Location $runnerRoot
.\config.cmd --unattended `
  --url https://github.com/sonwe1e/VideoCompareStation `
  --token $token `
  --name Sonwe-RTX4090 `
  --labels dvs-toolchain-4.4,dvs-gpu `
  --work _work `
  --replace
```

仓库变量保存非敏感、机器相关的固定路径。使用专用名称，避免
`ilammy/msvc-dev-cmd` 用 Visual Studio 内置 vcpkg 覆盖 `VCPKG_ROOT`：

```text
DVS_VCPKG_ROOT=G:\Workspaces\vcpkg
DVS_PERFORMANCE_FIXTURE_ROOT=G:\GitHubActions\Toy-data\performance
```

`DVS_PERFORMANCE_FIXTURE_ROOT` 指向不受 checkout/clean 影响的素材目录。该目录必须
包含：

```text
gate-1080p60-a.mp4
gate-1080p60-b.mp4
gate-1080p60-diff-b.mp4  # 从 a 生成，中心 25% 注入确定性可见差异
gate-1080p60-diff-b.contract.json
gate-1080p60-c.mp4
gate-1080p60-rot90-a.mp4  # 1080p60, 90-degree presentation metadata
gate-1080p120-a.mp4
gate-1080p120-b.mp4
gate-1080p120-c.mp4
```

Diff 门禁不会使用可能与 A 像素相同的通用 B 素材。首次准备 runner，或替换
`gate-1080p60-a.mp4` 后，必须重新生成带来源哈希、输出哈希和解码首帧哈希的专用素材：

```powershell
.\tools\testing\generate-performance-diff-fixture.ps1 `
  -Ffmpeg C:\ffmpeg\bin\ffmpeg.exe `
  -FixtureRoot $env:DVS_PERFORMANCE_FIXTURE_ROOT
```

生成器要求输入和输出均为同 codec、pixel format、frame count 的 1920x1080 60 fps
素材，并在 B 的中心 25% 写入白色块。`hardware.diff-2source` 启动前还会核对 contract
中的文件 SHA-256 与两个不同的解码首帧 SHA-256；最终渲染仍必须通过至少 1% 的亮像素
比例门禁。若通过 `-OutputName` 生成额外素材，其 contract 使用相同文件基名；默认门禁仍
只读取上述固定命名配对。生成、探测与首帧哈希均受 `-NativeTimeoutSeconds` 限制，替换
现有配对失败时生成器会回滚视频和 contract，保留上一组可用文件。

4K 不属于活动性能矩阵，runner 不需要 4K fixture。HEVC Main10/P010 与 D3D11VA
零拷贝正确性继续由仓库内的小分辨率 fixture 覆盖。

## 运行方式与安全边界

不要把该 runner 安装成 Windows 服务。D3D11VA、Qt 渲染和可见窗口性能门禁需要当前
用户的交互桌面；服务运行在 Session 0，不能作为这组门禁的可信证据。用户登录后从
runner 目录启动 `run.cmd`，或创建“用户登录时”启动的计划任务。

交互桌面还必须实际挂载由 PCI 显卡驱动的 120 Hz 或更高刷新率显示输出。只挂载
GameViewer、Oray 等虚拟/间接显示适配器时，即使 D3D11VA 解码成功，Qt 的呈现节奏也
不能作为 60/120 FPS 发布证据。工作流会先执行以下 fail-closed 预检：

```powershell
.\tools\testing\test-hardware-runner.ps1 -MinimumRefreshRate 120
```

预检必须输出 `DVS_HARDWARE_RUNNER_READY`。如果提示没有物理 PCI 显示适配器附着到
桌面，应先在本机物理显示输出上登录，或把物理输出加入当前扩展桌面，再启动 runner；
不得通过删除预检、降低刷新率或继续使用纯虚拟桌面来绕过门禁。

此仓库是 public。self-hosted job 已限制为仓库内分支的 PR，fork PR 不会被发送到
工作站；硬件性能 workflow 仅允许维护者手动 dispatch。不要移除这些限制，也不要在
runner 上保存 GitHub token、签名证书或其他长期密钥。

runner 控制台显示 `Listening for Jobs` 后，GitHub 的 runner API 应显示：

```text
status: online
labels: self-hosted, Windows, X64, dvs-toolchain-4.4, dvs-gpu
```

常规 PR checks 全绿后，再手动运行 `Hardware and Performance`。配置和编译仍以 4 核
BelowNormal 低影响配置运行；D3D11VA 硬件和性能 CTest 则以 Normal 优先级、runner 完整
CPU affinity 运行，同时保持 CTest 串行。这样 500 ms cold-seek P95 和 100 ms UI 响应
门槛衡量的是交互式用户环境，而不是后台构建限速造成的调度尖峰。

阻断门槛包括 1080p60 运行 300 秒，以及 2 路和 3 路 1080p120 各运行 60 秒。所有工作流
日志作为 artifact 保留 14 天。
