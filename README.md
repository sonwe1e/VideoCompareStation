# CompareStation（CompareStation）

CompareStation 是面向 Windows 的 1～3 路逐帧视频工作站，使用 C++20、Qt Quick、
FFmpeg 动态库和 D3D11。单个视频可直接播放和逐帧审查；多路素材会被放在同一个
canonical frame position 上，支持任意两路 Wipe/Diff、显式对齐以及缺帧和重复帧诊断。

当前产品覆盖静音视频播放、视频对比、图片查看和图片对比，重点是画质、运动连续性与透明度审查。
Agent 从 [快速定位指南](docs/agent-guide.md) 开始；当前需求见
[产品目标](docs/product/visual-review.md)，问题、代码入口与验收条件见
[问题台账](docs/engineering/visual-review-backlog.md)。
产品架构、工程说明和历史资料统一从 [docs/README.md](docs/README.md) 进入。
[docs/architecture.md](docs/architecture.md) 描述技术约束，[docs/releases/](docs/releases/) 记录各版本发布范围；旧实现已从主线移除，
需要时可通过 Git 历史查阅。
[版本号与提交约定](docs/versioning.md)说明开发版本、Git 提交和发布标签的关系。

---

## 主要工作流

启动后可直接拖入 1～3 个视频：单个视频直接以暂停的首帧铺满审查视口，两路或三路
素材会先确认 A/B/C 顺序和 Reference。也可以从 File 菜单选择 Open videos… 打开视频，
或用 Add video… 向当前会话追加。首屏／文件菜单的「从文件夹选视频…」和 `Ctrl+Alt+O`
可列出本地顶层目录中的视频；点击成功后静音播放，支持手动上一项／下一项，不在 EOF 自动连播。
CompareStation 只管理当前打开的 1～3 个视频（会话），不保存项目、没有 `.dvsproj`，
关闭窗口时不会提示保存；OSC 模式、快捷键方案和默认 Diff Filter 等偏好自动保存到
Settings。它是静音视觉审查工具：读取视频画面，但不解码或播放音频。

常用审查能力包括：

- Side by side 使用明确白色分割线，Wipe compare 可拖动分割线并比较 A/B、A/C 或 B/C；
- 单路会话加入 B/C 或移除 B/C 时会暂停并原子重建，在新 canonical timeline 上恢复同一 MediaTime；
- A/D 或左右方向键逐 1 帧，Up/Down 或 Shift+A/D 逐 5 帧，Ctrl+A/D 按 canonical FPS 跳 1 秒；
- `Tab` 隐藏全部工具 chrome，`F11` 独立切换系统全屏；纯画布中快捷键、Wipe、Loading、严重错误和短暂帧号反馈继续有效；
- 连续逐帧期间保留旧画面并接受新请求，最终只呈现最新请求，不用全屏 Loading 遮挡；
- Advanced Alignment Inspector 默认收起，需要时再估计整体偏移、分析缺帧/重复帧或设置手动锚点；

用户包不携带外部 `ffmpeg.exe` 或 `ffprobe.exe`。GUI 与
`CompareStationCli --probe` 都直接使用随包分发的 FFmpeg 动态库。

图片工作流：

- 打开单张图片后，`←`/`→`（以及 `↑`/`↓`）按自然序在同一目录内翻上一张/下一张，`Home`/`End`
  跳到目录首尾；工具栏显示当前序号，到边界时给出提示而不是绕回；
- 翻到无法解码的图片时保留当前画面并说明原因，手动继续即可；目录内新增、删除、重命名会在
  下一次翻页时重新扫描；
- 打开图片对或双文件夹对比后，方向键改为逐行切换配对，不会与单目录翻页混用；
- 编辑副本有未保存修改时，关闭该图会被拒绝并提示先「另存副本…」或「结束编辑」。

---

## 支持的格式

按扩展名进入工作区，实际能否解码由内置 FFmpeg 与 Qt 回退共同决定。这张表是当前构建的
真实边界，不承诺未列出的格式。

| 类别 | 扩展名 | 解码路径 | 状态 |
|---|---|---|---|
| 视频 | `.mp4` `.mkv` `.mov` `.avi` `.m4v` | FFmpeg + D3D11VA | 仅 H.264、HEVC、MPEG-4 Part 2；AV1 / VP9 明确拒绝并报错；PQ/HLG 拒绝 |
| 图片（FFmpeg 解码） | `.pnm` `.ppm` `.pgm` `.pbm` `.pam` `.jpg` `.jpeg` `.bmp` `.gif` `.webp` | FFmpeg | PNM P1–P7 与这些格式均经 FFmpeg 解码 |
| 图片（Qt 回退） | `.png` | Qt | 当前 FFmpeg 构建未启用 zlib，PNG 由 Qt 解码为 RGBA8：**16-bit 精度不保留** |
| 图片（受限） | `.tif` `.tiff` | FFmpeg | 未压缩 TIFF 可读；Deflate 压缩因缺少 zlib 会明确失败 |

不支持音频、字幕、媒体库与项目文件；这些不是尚未完成的缺口，而是**不在产品范围内**。

---

## 验证状态与发布声明

每个版本的声明分三段，缺一段即视为未完成发布：

1. **已验证（软件）**：完整软件测试套件、`format-check` 与 `lint` 在当次提交上通过；证据以
   提交 SHA 和日志为准，不跨提交转移。
2. **未验证（硬件与物理显示）**：真实 D3D11VA 硬件门禁、跨 DPI/物理显示验收、`shutdown-soak`
   与 `packaged-smoke`。这些必须登录交互桌面运行，WARP 或 CPU 测试不能替代。
3. **已知不支持**：上表的格式边界，以及 AV1/VP9、HDR、音频、媒体库、项目文件。

2.5.0 按维护者要求跳过本地物理硬件与性能门禁，属于**一次性例外**；该授权不延续到后续
发布，未运行的门禁不记为通过。2.5.1 的硬件与物理显示仍是未验证项。


---

## 构建与验证

要求 Windows x64、PowerShell 7、Visual Studio 2022 Build Tools/MSVC v143、CMake 4.4、
Ninja、LLVM 19.1.5 和完整的 vcpkg checkout。首次配置会根据 `vcpkg.json` 安装 Qt 6.11、FFmpeg 8.1.2、GoogleTest 与
nlohmann-json。所有生成物都在未跟踪的 `out/` 下。

推荐从普通终端使用包装器；它自动导入 MSVC x64 环境，输出实际工具路径并保留实时诊断：

```powershell
pwsh tools/build/build.ps1 -Preset dev -Doctor
pwsh tools/build/build.ps1 -Preset dev -Test
pwsh tools/build/build.ps1 -Preset dev -Target format-check
pwsh tools/build/build.ps1 -Preset dev -Target lint
```

路径迁移、工具升级或旧头文件依赖损坏时用 `-Fresh` 清理构建产物并重新配置。
工具路径覆盖、预装依赖模式和故障定位见 [构建指南](docs/building.md)。
默认构建全部目标，定向测试使用 `-TestRegex`。

启动应用与 CLI 诊断：

```powershell
.\out\build\dev\bin\CompareStation.exe
.\out\build\dev\bin\CompareStation.exe --play .\video.mp4
.\out\build\dev\bin\CompareStation.exe --compare .\reference.mp4 .\prediction.mp4
.\out\build\dev\bin\CompareStation.exe .\video.mp4
.\out\build\dev\bin\CompareStation.exe .\a.mp4 .\b.mp4 .\c.mp4
.\out\build\dev\bin\CompareStationCli.exe --startup-check
.\out\build\dev\bin\CompareStationCli.exe --probe .\video.mp4
```

确定性的 WARP UI smoke：

```powershell
.\out\build\dev\bin\CompareStation.exe --ui-smoke `
  .\tests\fixtures\media\h264_a_320x180_30fps_12.mp4 `
  .\tests\fixtures\media\h264_b_160x90_30fps_12.mp4
```

Release 与安装包：

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
cpack --preset release-zip
ctest --preset shutdown-soak --output-on-failure
ctest --preset packaged-smoke --output-on-failure
```

只发布免安装 ZIP，不需要 .NET SDK、WiX 或管理员权限。
`packaged-smoke` 验证三种 DPI 下的弹出控件像素，`shutdown-soak` 验证重复打开与关闭。
详细配置见
[docs/self-hosted-runner.md](docs/self-hosted-runner.md)。

---

## 性能与发布门禁

硬件门禁必须在登录到交互桌面的 D3D11VA runner 上运行，不能用 WARP 或 CPU 测试替代。
`performance-d3d11` 包括单路/两路/三路 1080p60 五分钟门禁，以及单路/两路/三路
1080p120 各 60 秒门禁；检查 presentation ACK 连续性、source atomicity、完整
FrameSet drop、帧预算、冷 seek P95、相邻逐帧和有界关闭。
4K 不属于活动性能矩阵；小分辨率 P010/10-bit 正确性测试仍保留。

```powershell
$env:DVS_PERFORMANCE_FIXTURE_ROOT = 'G:\GitHubActions\Toy-data\performance'
ctest --preset hardware-d3d11 --output-on-failure
ctest --preset performance-d3d11 --output-on-failure
```

Release workflow 生成明确标注为未签名的 ZIP，包内包含 EXE、CLI 与
`CompareStationShell-2.5.dll`。历史版本链更新到 `1.2.0→2.5.1`；本次交付单图同目录连续翻页、
循环播放跳帧统计修复，以及启动、退出与设备恢复路径的稳定性修复，并清理已证实无调用者的代码。
首次启动新版时接管当前用户的右键菜单。
标准发布流程执行 Release 测试、硬件与性能门禁、DPI 像素验证和 shutdown soak。
2.5.1 在本地完成 Release 测试、打包、packaged-smoke 与 shutdown-soak；真实 D3D11VA 硬件、
性能门禁与物理显示验收未在主力桌面启动（按测试环境分离执行），不据此宣称播放 FPS、
seek 延迟或硬件性能达标。2.5.0 曾按维护者要求跳过本地物理门禁，该一次性例外不延续。
具体功能、验证结果与边界见 [发布说明](docs/releases/v2.5.1.md)。
SHA-256 只用于校验完整性，不代表发布者身份。
runner 标签、素材清单与发布合同详见
[docs/self-hosted-runner.md](docs/self-hosted-runner.md)。

---

## 代码结构

`src/domain` 只包含规则，`src/application` 只包含用例和 ports；
`platform_windows`、`media_ffmpeg`、`persistence_json` 与 `ui_qml` 是外层适配器，
`shell_windows` 是不依赖 Qt/FFmpeg 的 Explorer COM 入口，`src/app` 负责组合。
测试按层放在 `tests/`，打包定义在 `packaging/`，品牌资源在
`assets/branding/`。架构与贡献约束见
[docs/architecture.md](docs/architecture.md) 和 [AGENTS.md](AGENTS.md)。
