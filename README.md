# VCStation（VideoCompareStation）

VCStation 是面向 Windows 的 1～3 路逐帧视频工作站，使用 C++20、Qt Quick、
FFmpeg 动态库和 D3D11。单个视频可直接播放和逐帧审查；多路素材会被放在同一个
canonical frame position 上，支持任意两路 Wipe/Diff、显式对齐以及缺帧和重复帧诊断。

当前产品架构、工程说明、活动方案和历史资料统一从
[docs/README.md](docs/README.md) 进入。产品计划和验收范围以
[docs/architecture.md](docs/architecture.md) 与 [docs/releases/](docs/releases/) 为准；旧实现已从主线移除，
需要时可通过 Git 历史查阅。

---

## 主要工作流

启动后可直接拖入 1～3 个视频：单个视频直接以暂停的首帧铺满审查视口，两路或三路
素材会先确认 A/B/C 顺序和 Reference。也可以从 File 菜单选择 Open videos… 打开视频，
或用 Add video… 向当前会话追加。
VCStation 只管理当前打开的 1～3 个视频（会话），不保存项目、没有 `.dvsproj`，
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
`VCStationCli --probe` 都直接使用随包分发的 FFmpeg 动态库。

---

## 构建与验证

要求 Windows x64、Visual Studio 2022 Build Tools/MSVC v143、CMake 4.4、Ninja 和
vcpkg。首次配置会根据 `vcpkg.json` 安装 Qt 6.11、FFmpeg 8.1.2、GoogleTest 与
nlohmann-json。所有生成物都在未跟踪的 `out/` 下。

从 Visual Studio x64 开发者 PowerShell 运行：

```powershell
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset dev --target format-check
cmake --build --preset dev --target lint
```

启动应用与 CLI 诊断：

```powershell
.\out\build\dev\bin\VCStation.exe
.\out\build\dev\bin\VCStation.exe --play .\video.mp4
.\out\build\dev\bin\VCStation.exe --compare .\reference.mp4 .\prediction.mp4
.\out\build\dev\bin\VCStation.exe .\video.mp4
.\out\build\dev\bin\VCStation.exe .\a.mp4 .\b.mp4 .\c.mp4
.\out\build\dev\bin\VCStationCli.exe --startup-check
.\out\build\dev\bin\VCStationCli.exe --probe .\video.mp4
```

确定性的 WARP UI smoke：

```powershell
.\out\build\dev\bin\VCStation.exe --ui-smoke `
  .\tests\fixtures\media\h264_a_320x180_30fps_12.mp4 `
  .\tests\fixtures\media\h264_b_160x90_30fps_12.mp4
```

Release 与安装包：

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
cpack --preset release-zip
cpack --preset release-msi
ctest --preset shutdown-soak --output-on-failure
ctest --preset packaged-smoke --output-on-failure
```

MSI 生成还要求 .NET SDK 8、WiX 4.0.4 与 `WixToolset.UI.wixext` 4.0.4；
`packaged-smoke` 会真实安装 per-machine MSI，因此 runner 必须提升权限且不能已有
VCStation 安装。详细配置见
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

Release workflow 生成明确标注为未签名的 ZIP、MSI、EXE、CLI 与
`VCStationShell-1.6.dll`，并执行真实
安装、`1.2.0→1.6.0` 升级、A/B Pair 设置回归与 shutdown soak 门禁；SHA-256 只用于
校验完整性，不代表
发布者身份。runner 标签、素材清单与发布合同详见
[docs/self-hosted-runner.md](docs/self-hosted-runner.md)。

---

## 代码结构

`src/domain` 只包含规则，`src/application` 只包含用例和 ports；
`platform_windows`、`media_ffmpeg`、`persistence_json` 与 `ui_qml` 是外层适配器，
`shell_windows` 是不依赖 Qt/FFmpeg 的 Explorer COM 入口，`src/app` 负责组合。
测试按层放在 `tests/`，打包定义在 `packaging/`，品牌资源在
`assets/branding/`。架构与贡献约束见
[docs/architecture.md](docs/architecture.md) 和 [AGENTS.md](AGENTS.md)。
