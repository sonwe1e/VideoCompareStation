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

`-Doctor` 只读检查所选工具、缓存路径、已有生产对象的头文件依赖和下方环境自检结论；没有对象时
仍需首次构建。`-Doctor` 在环境自检发现错误时以非 0 退出并打印修复指令——这是**这台机器当前
不可增量构建**的结论，不是源码问题。缓存仍指向无法解析 manifest 的 vcpkg 根时 `-Doctor` 也会
失败，此时按提示改用 `-UseInstalledDependencies`，不需要重建。
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

MSVC `/showIncludes` 的探测和编译使用一致的 `VSLANG=1033`。`VSLANG` 只能选择已安装的资源目录，
不能凭空补出资源。VS Installer 里**没有"MSVC 语言包"组件**（`--add …LangPack.en-US` 会以
87/ERROR_INVALID_PARAMETER 失败），MSVC 的编译器诊断资源是按语言拆分的独立资源包
（`Microsoft.VC.<ver>.<vs>.Tools.Host*.Target*.Res.base`，如 `en-US`、`zh-CN`，各约 225 KB，
只含该语言的 8 个 `*ui.dll`）。工具集缺少 `1033/` 时 `cl.exe` 仍输出该语言，`VSLANG=1033` 失效，
`cl.exe` 经管道输出的字节与 CMake 写入 `CMakeFiles/rules.ninja` 的 `msvc_deps_prefix` 不匹配，
Ninja 无法识别 `/showIncludes` 行，所有新编译对象都没有头文件依赖记录，
`quality.msvc_dependencies` 失败。
注意 `deps = msvc` 规则本身始终存在（在 `CMakeFiles/rules.ninja`，不在 `build.ninja`）；
**`-Fresh` 不能修复此项**，只会白跑一次全量重编。修复：

```powershell
# 需要管理员权限的 PowerShell 7：从 installer 自身缓存的频道清单取该语言资源包，
# 校验清单里的 SHA256 后解压到工具集；任何不匹配都中止且不写入。
pwsh tools/build/InstallMsvcEnglishResources.ps1
pwsh tools/build/InstallMsvcEnglishResources.ps1 -VerifyOnly   # 只检查是否已具备 1033/
```

装完必须全量重建一次，让所有对象重新记录头文件依赖：
`pwsh tools/build/build.ps1 -Preset dev -Fresh -UseInstalledDependencies -Test`。
`build.ps1 -Doctor` 和每次构建开始的环境自检都会逐字节比对实际前缀与生成规则并报出该缺陷。

`quality.msvc_dependencies` 直接检查生产对象在 Ninja 数据库中记录的头文件；空记录或遗漏关键头文件
会失败。检查覆盖 domain，以及已生成的播放协调器、图片／视频控制器和主界面测试对象，防止只有
domain 正常、UI 仍混用旧类布局。尚未构建的可选目标跳过检查，首次构建后再验证。
该检查也接入包装器、format-check 和 lint，避免“构建成功但使用旧 ABI”。
`quality.msvc_dependency_contracts` 使用独立 Ninja 数据库验证健康记录、空记录、缺关键头文件
和无记录的对象，包含带空格路径；不会修改实际应用的对象或依赖库。

## 环境自检

包装器在导入 vcvars 之后、执行任何构建动作之前运行只读环境自检。`-Doctor` 与普通构建共用同一套
检查，区别只在错误是否致命：

- vcpkg 候选：无效候选不再逐个警告，仅在全部候选都失败时汇总列出。
- manifest 解析能力：vcpkg 根缺少 `.git` 时 builtin registry 无法读取端口与 baseline，本机必须
  使用 `-UseInstalledDependencies`。
- MSVC 英文资源：缺少 `1033/` 时直接给出语言包修复指令。
- `/showIncludes` 前缀契约：逐字节比对 `cl.exe` 实际输出与生成的 `CMakeFiles/rules.ninja`，
  不一致即判定头文件依赖不可信，并打印两个前缀文本及其 UTF-8 字节数。

`-Doctor` 对上述错误直接失败（退出码非 0）；普通构建只告警，因为构建本身与构建后的头文件依赖
门禁才是权威判断，提前失败会把可恢复的机器状态误报成源码问题。

缓存检查区分两类问题：真正失效的缓存（工具消失、路径指向别处）仍提示 `-Fresh`；而“缓存的 vcpkg
根缺少 `versions/`、无法解析 manifest”属于能力问题，提示改用 `-UseInstalledDependencies`——
`-Fresh` 只会重新配置到同一个根并在安装依赖时失败。若两个 vcpkg 根的 `vcpkg.cmake` 内容与
`versions/baseline.json` 都一致（VS 会附带一份自带 vcpkg），则视为可互换，不触发无谓的全量重编。

## 修改完成前

按改动运行相关测试、format-check、lint，并检查实际执行数量。
构建脚本回归测试使用隔离的模拟工具，覆盖工具路径、参数透传、stdout/stderr 保留、失败传播和缓存恢复；
真实编译仍由项目构建与头文件依赖门禁验证。脚本测试同时进入 CTest 和 CI。
视频性能验收仍遵循 [runner 门禁](self-hosted-runner.md)，不以构建或 WARP 测试代替硬件证据。

本次构建可靠性修复和验证结果见 [问题台账 E-01](engineering/visual-review-backlog.md#e-01-构建工具与增量依赖可靠性)。
