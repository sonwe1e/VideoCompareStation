# 构建与工具定位

适用于当前 Windows x64 工作区。脚本入口是 `tools/build/build.ps1`，工具发现集中在
`tools/build/env.ps1`。所有命令使用 PowerShell 7；无需手工拼接 `cmd /c` 或修改系统 PATH。

## 日常使用

```powershell
pwsh tools/build/build.ps1 -Preset dev -Doctor
pwsh tools/build/build.ps1 -Preset dev -Test
pwsh tools/build/build.ps1 -Preset dev -Target format-check
pwsh tools/build/build.ps1 -Preset dev -Target lint
pwsh tools/quality/check-build-scripts.ps1
```

`-Doctor` 只读检查所选工具、缓存路径和已有生产对象的头文件依赖；没有对象时仍需首次构建。
默认构建所有目标。定向测试用 `-Test -TestRegex 'ui.ImageReviewControllerTests'`，
零项匹配会失败。`-TestOnly -TestRegex ...` 只用于源码与二进制已一致的情况。
包装器以仓库为工作目录；从其他目录调用时使用脚本的完整路径。

同一 preset 的包装器构建和测试互斥，避免 smoke 正在运行 EXE 时重新链接造成 LNK1168。
不要同时绕过包装器运行同目录的 Ninja/CTest。日志实时输出；需要保存时重定向到 `out/`。
静态分析会建立所需生产目标，因此也应与测试串行执行。

## 工具来源与覆盖

| 工具 | 发现顺序与约束 |
|---|---|
| vcpkg | `-VcpkgRoot`；否则 `DVS_VCPKG_ROOT`、`VCPKG_ROOT`、`VCPKG_INSTALLATION_ROOT`、PATH 中 vcpkg、仓库相邻的 vcpkg 目录。无效显式参数报错，失效的自动候选报告并继续查找。 |
| MSVC | `-VcvarsAll`、`VCVARSALL_PATH`、当前 `VCINSTALLDIR`，或通过 vswhere 查找安装了 C++ 工具的 VS 2022。导入 x64 环境后保留所选 vcpkg。 |
| CMake | `-CMakeExecutable`、`CMAKE_EXECUTABLE`、PATH、VS 内置 CMake；CTest 必须来自同一目录。项目要求 CMake 4.4。 |
| Ninja | `-NinjaExecutable`、`NINJA_BIN`、PATH、所选 VS 内置 Ninja。配置时显式写入 `CMAKE_MAKE_PROGRAM`。 |
| LLVM | 所选 VS 的 `VC/Tools/Llvm/x64/bin` 和 PATH；CMake 校验 clang-format、clang-tidy 都为 19.1.5。 |
| Qt 工具 | CMake 从当前 vcpkg 安装树发现 qmlformat、qmllint。不要使用其他 Qt 版本的工具。 |

显式参数示例（替换为实际安装路径）：

```powershell
pwsh tools/build/build.ps1 -Preset dev -VcpkgRoot 'D:\dev\vcpkg' -Doctor
```

`-Target` 接受真实 CMake target；PowerShell 内调用脚本时可传数组。
查看目标用已配置目录的 `cmake --build --preset dev --target help`。
不存在 `dvs_ui_qml` 目标，也没有 `-FormatCheck -Lint` 参数；使用上方独立目标命令。

## 依赖安装与缓存恢复

普通首次配置通过 manifest 安装依赖，需要完整 vcpkg checkout、Git 和网络。
只有已准备好对应 `out/vcpkg*` 安装树时才使用显式预装模式：

```powershell
pwsh tools/build/build.ps1 -Preset dev -Configure -UseInstalledDependencies
```

该选项传入 `VCPKG_MANIFEST_INSTALL=OFF`，CMake 仍检查必需包；它不会安装缺失依赖。
后续 `-Configure` 不加此选项会恢复 preset 的自动安装。
当前工作站的 vcpkg 是无 `.git` 的精简目录，已安装 Qt/FFmpeg/GTest，因此本机重配置使用此选项。
这不是 CI 或新机器的默认设置。

移动工作区、切换工具、升级编译器或发现头文件变化不触发重编时：

```powershell
pwsh tools/build/build.ps1 -Preset dev -Fresh -Test
# 已有完整安装树的精简 vcpkg 工作站：
pwsh tools/build/build.ps1 -Preset dev -Fresh -UseInstalledDependencies -Test
```

`-Fresh` 先重新配置，再用新的 Ninja 规则清理构建产物并重编，保留 `out/vcpkg*`。
`-Configure` 仅重配，不能修复因丢失头文件依赖而残留的旧对象。
缓存引用失效工具或旧目录时会指出具体键和路径，避免继续混用。

MSVC `/showIncludes` 的探测和编译使用一致的 `VSLANG=1033`。只有中文资源的 MSVC 仍可能
输出中文，因此不能靠前缀是否英文判断正确性。`quality.msvc_dependencies` 直接检查生产对象
在 Ninja 数据库中记录的头文件；空记录或遗漏关键头文件会失败并提示 `-Fresh`。
检查覆盖 domain，以及已生成的播放协调器、图片／视频控制器和主界面测试对象，防止只有
domain 正常、UI 仍混用旧类布局。尚未构建的可选目标跳过检查，首次构建后再验证。
该检查也接入包装器、format-check 和 lint，避免“构建成功但使用旧 ABI”。
`quality.msvc_dependency_contracts` 使用独立 Ninja 数据库验证健康记录、空记录、缺关键头文件
和无记录的对象，包含带空格路径；不会修改实际应用的对象或依赖库。

## 修改完成前

按改动运行相关测试、format-check、lint，并检查实际执行数量。
构建脚本回归测试使用隔离的模拟工具，覆盖工具路径、参数透传、stdout/stderr 保留、失败传播和缓存恢复；
真实编译仍由项目构建与头文件依赖门禁验证。脚本测试同时进入 CTest 和 CI。
视频性能验收仍遵循 [runner 门禁](self-hosted-runner.md)，不以构建或 WARP 测试代替硬件证据。

本次构建可靠性修复和验证结果见 [问题台账 E-01](engineering/visual-review-backlog.md#e-01-构建工具与增量依赖可靠性)。
