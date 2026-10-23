# 构建与工具定位

适用于当前 Windows x64 工作区。脚本入口是 `tools/build/build.ps1`，工具发现集中在
`tools/build/env.ps1`。所有命令使用 PowerShell 7；无需手工拼接 `cmd /c` 或修改系统 PATH。

默认采用低占用构建：wrapper 在启动工具前，将自身和继承的子进程限制为最多 **4 个逻辑处理器**
（不超过继承可用处理器的一半，至少 1 个）。编译／静态检查使用 `BelowNormal` 优先级，
编译最多 2 个任务；测试在同一 CPU 范围内使用 `Normal` 优先级，为 Qt/WARP 启动和关闭
保留调度余量，启动／关闭门禁时限不变。构建显式传入并行数；
CTest 默认仍为 1，依赖构建 preset 上限为 2。`-TestJobs` 只改变同时运行的测试数量，
不会解除 CPU 限制。此策略覆盖配置、编译、格式检查、静态分析和测试；退出或失败后恢复调用者的
环境、PATH、亲和性及优先级。单逻辑处理器环境只能靠低优先级让出资源。

所有 preset 共用仓库构建锁，避免 dev、release、lint、test 叠加抢资源及同时修改依赖树。
生成的 Ninja 图还为编译／链接设置共享的 2 槽任务池，Qt 自动生成器内部并行上限也为 2，
防止直接 `ninja` 使用默认的“逻辑处理器数 + 2”并发。**完整进程树的 CPU／优先级限制依赖 wrapper**；
任务池只保护其对应步骤，不替代入口。日常与代理验证都使用下方 wrapper，不手动运行裸 Ninja，
也不通过拼 PATH 或临时指定其他编译器修复失败。正式硬件性能测量仍走 CI 的 Interactive 门禁，
不把受限环境测得的帧率当作硬件性能结果。

项目当前没有 C++20 modules，因此关闭每个翻译单元的额外模块扫描；MSVC `/showIncludes`
头文件跟踪和相应依赖验证不变。以后引入 modules 时再针对对应目标启用扫描。

本地存在 SAM2 模型时，智能抠图集成测试包含冷加载和 CPU 推理；两核环境实测约 17–18 秒，
该单项使用 60 秒超时，其他图片编辑测试仍为 10 秒。测试名称、命令、断言及性能门禁未改变，
CTest 发现列表必须恰好包含这一项，不能通过过滤掉模型测试来适应资源限制。

## 日常使用

```powershell
pwsh tools/build/build.ps1 -Preset dev -Doctor
pwsh tools/build/build.ps1 -Preset dev -Test -TestGroup Sidebar -TestJobs 2
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex '^ui[.]ImageReviewControllerTests[.]'
pwsh tools/build/build.ps1 -Preset dev -Test -TestJobs 2
pwsh tools/build/build.ps1 -Preset dev -Target format-check
pwsh tools/build/build.ps1 -Preset dev -Target lint
pwsh tools/quality/check-build-scripts.ps1
pwsh tools/quality/check-script-integrity.ps1
pwsh tools/quality/check-hardcoded-paths.ps1
```

`-Doctor` 只读检查所选工具、缓存路径、已有生产对象的头文件依赖和下方环境自检结论；没有对象时
仍需首次构建。`-Doctor` 在环境自检发现错误时以非 0 退出并打印修复指令——这是**这台机器当前
不可增量构建**的结论，不是源码问题。缓存仍指向无法解析 manifest 的 vcpkg 根时 `-Doctor` 也会
失败，此时按提示改用 `-UseInstalledDependencies`，不需要重建。
默认构建所有目标。定向测试用 `-Test -TestRegex 'ui.ImageReviewControllerTests'`，
零项匹配会失败。`-TestOnly -TestRegex ...` 只用于源码与二进制已一致的情况。
包装器以仓库为工作目录；从其他目录调用时使用脚本的完整路径。

### 开发循环与完整验收

编辑时先选择受影响的组，或用 `-TestRegex` 精确到套件／用例。组不是自动影响分析：跨层修改
仍需沿 [Agent 路由表](agent-guide.md) 补跑其他受影响入口；`Ui` 是较宽的 UI 回归，通常不应
用于单个按钮的每次修改。组与正则不能同时传入，避免误以为两个范围会取交集。

| `-TestGroup` | 覆盖范围 |
|---|---|
| `Core` | domain、application、presentation 契约 |
| `Media` | media 解码／导出组件与 platform 渲染组件，沿用 preset 的硬件／性能排除项 |
| `Images` | 图片解码、查看／编辑／配对控制器、图片工作区及文件夹侧栏接线 |
| `Playback` | application、播放解码链、走带／Jog／缩略图控制器与主窗播放入口 |
| `Sidebar` | 文件夹模型、历史／侧栏控制器、菜单交互与主窗侧栏接线 |
| `Ui` | 全部 UI 控制器、组件与主窗契约 |
| `Build` | `quality.*` 构建、脚本和发布契约检查 |
| `Shell` | Explorer 接口及应用启动／smoke |

命名组默认每套 QML 只跑 Basic／100% 代表配置。修改主题、弹窗、布局或 DPI 时加
`-FullMatrix`，同一套件的 Windows／125%／150%／200% 组合仍全部保留。显式 `-TestRegex`
和无过滤器的 `-Test` 保持完整矩阵，不会因为新增开发分组而缩小原有门禁。

```powershell
pwsh tools/build/build.ps1 -Preset dev -Test -TestGroup Images -TestJobs 2
pwsh tools/build/build.ps1 -Preset dev -Test -TestGroup Sidebar -FullMatrix -TestJobs 2
```

`-TestJobs N` 只改变本次 CTest 并行度；默认仍遵循 preset 的单线程设置。真实窗口输入测试
保持 `RUN_SERIAL`，不会同时争抢焦点。不要自行并发启动同一构建目录的多个 wrapper。

自 2026-10-22 起，**小改动允许以相关验证完成交付**，不再要求每项修改最后追加全套。
按行为、依赖和风险选择，不能以改动行数代替影响分析。以下规则同时适用于开发循环和最终交付：

| 改动范围 | 最终交付所需验证 |
|---|---|
| 仅文档、注释，未改变脚本或可执行行为 | 差异／链接／规则一致性检查；涉及脚本指引时跑相应脚本契约，不构建媒体／UI，不跑 C++ lint |
| 隔离的 QML 文案或局部交互 | 增量构建、受影响 QML／主窗接线测试、`dvs_format_check_qml` 与 `dvs_lint_qml`；不运行全仓 C++ lint |
| 布局、弹窗、主题、DPI | 上一行加受影响套件的完整样式／DPI 配置及视觉检查；使用组时加 `-FullMatrix`，不扩大到无关 UI 套件 |
| 隔离的低风险 C++ 实现，不改公共接口、资源所有权／生命周期、并发或数据完整性 | warning-fatal 增量构建、受影响套件及直接调用者测试、`dvs_format_check_cpp`；可将全仓 clang-tidy 留给 CI／发布，交付说明未跑 |
| 局部资源生命周期、异步身份、完整帧组、时序、缓存边界、文件写入 | 相关跨层测试、适用 format/lint，以及每项改变的关键行为所需反例；适用性能证据照常保留，不能按“小改动”豁免。影响可界定时不强制无关全套 |
| 公共契约／共享头文件的语义变化、广泛跨层重构、全局构建／工具链／依赖变更、影响不能界定 | 稳定后一次完整 `-Test` 和适用 format/lint；必要的集成／性能证据 |
| 局部测试脚本／路由变更 | 对应契约测试及正反例，证明非零选择和必需门禁不会被静默遗漏；改变全局执行语义则按上一行 |
| 发布验收、用户明确要求全量 | 完整测试及现有 CI／发布门禁，不以开发子集替代 |

“相关测试”须覆盖修改行为及直接受影响的调用者；先核对 CTest 注册名，结果核对实际执行数量，
零项匹配不是通过。工作区有其他人的未提交改动时，说明本次验证边界；它们若影响本次依赖链，
也要纳入选择。没有证据证明某个既有测试重复时，不删测试、断言或超时门禁来换速度。

**不用增加并行度也能减少等待：**

- 格式整理和资源释放／类型转换自查在昂贵 lint 前完成；代码稳定后再启动收尾检查。
- 按语言选择已有目标。纯 QML 不需要触发 C++ 分析；纯 C++ 不需要附带无关 QML 检查。
  `dvs_format_check_cpp`／`dvs_format_check_qml` 仍扫描该语言的全仓文件，并非按 diff 增量。
- 当前 `lint` 在 `cmake/Quality.cmake` 中是一个 custom target，依次执行全部纳入范围的生产
  C++ 翻译单元，且依赖生产构建和 QML lint；每次调用都会重新分析。增大 `-TestJobs` 不会
  加速它。它尚无可靠的按改动文件／头文件依赖缓存，不能把它称为增量 lint。
- 默认 `-Test` 先构建全部目标；已明确测试二进制及其依赖时，可用 `-Target` 限定构建目标后
  搭配 `-TestRegex`。必须确认所选测试依赖均已重建，不能为省时运行旧二进制。
- 已经对最终源码构建通过，仅重跑测试时可用 `-TestOnly`；源码或依赖变化后先重新构建。
  不例行 `-Fresh`、清空依赖树或重复 `-Doctor`，仅在对应环境／缓存问题出现时使用。
- 失败先重跑失败范围；相同输入上的成功结果不重复。一轮完整 CTest 已包含脚本检查时，
  不再逐条运行相同脚本。关键行为的相关断言可以共享变异证据。

例如仅改 QML 时，在 PowerShell 7 内把相关目标合并到一次 wrapper 调用，避免重复环境初始化：

```powershell
# 正则须替换为当前改动实际影响的已注册套件；目标名来自 cmake/Quality.cmake。
& ./tools/build/build.ps1 -Preset dev -Target @('dvs_format_check_qml', 'dvs_lint_qml')
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex '^ui[.]player_osc$'
```

慢检查应读取**本次任务**日志，比较进程 CPU 时间／I/O 在两个采样点之间是否变化，并核对当前
命令和子进程；不能仅凭进程存在或累计 CPU 时间断言正常推进，也不能把 Ninja 的总步骤数当作
当前 C++ 文件的分析进度。持续无输出时先诊断，不反复盲等或叠加新的 lint。交付简述实际范围、
结果和未运行项即可；不要把定向通过写成全量通过。CI、无过滤器 `-Test` 及发布门禁保持原义。

整个仓库的包装器构建和测试互斥，避免不同 preset 堆叠负载，以及 smoke 运行 EXE 时重新链接造成 LNK1168。
不要同时绕过包装器运行同目录的 Ninja/CTest。日志实时输出；需要保存时重定向到 `out/`。
静态分析会建立所需生产目标，因此也应与测试串行执行。

## 移动或重命名工作区

CMake 缓存里存的是**绝对路径**，所以工作区被移动或改名后，旧缓存会继续指向已经不存在的目录，
表现成「代码明明没问题，构建却失败」。恢复只需要丢弃缓存重配一次：

```powershell
pwsh tools/build/build.ps1 -Preset dev -Fresh
```

依赖安装树要慢得多：`out/vcpkg` 一旦不完整（或依赖声明变化），ffmpeg 与 Qt6 就得从源码重建，
本机实测光 Qt6 就要几十分钟；这段时间 configure 之后的 ctest、format-check、lint 全都被挡住。
依赖重建也通过低占用 wrapper；运行期间可以编辑源码，但不要另起构建／测试进程叠加负载。

脚本里不得硬编码机器路径，一律从 `$PSScriptRoot` 推导（工具发现仍集中在 `tools/build/env.ps1`）。
`tools/quality/check-hardcoded-paths.ps1` 在 `ctest` 中拦截这类路径，改名留下的过期默认值不会再
潜伏到下次运行才暴露。

## 工具来源与覆盖

| 工具 | 发现顺序与约束 |
|---|---|
| vcpkg | `-VcpkgRoot`；否则 `DVS_VCPKG_ROOT`、`VCPKG_ROOT`、`VCPKG_INSTALLATION_ROOT`、PATH 中 vcpkg、仓库相邻的 vcpkg 目录。无效显式参数报错，失效的自动候选报告并继续查找。 |
| MSVC | `-VcvarsAll`、`VCVARSALL_PATH`、当前 `VCINSTALLDIR`，或通过 vswhere 查找安装了 C++ 工具的 VS 2022。导入 x64 环境后保留所选 vcpkg。 |
| CMake | `-CMakeExecutable`、`CMAKE_EXECUTABLE`；未覆盖时优先当前 preset 缓存、标准安装目录、VS 内置目录，再遍历 PATH 候选。必须 ≥4.4 且旁边有 CTest；不兼容的自动候选跳过，显式指定不兼容工具则报错。 |
| Ninja | `-NinjaExecutable`、`NINJA_BIN`；未覆盖时优先当前 preset 缓存、所选 VS 内置 Ninja，再遍历 PATH。必须 ≥1.12；配置时显式写入 `CMAKE_MAKE_PROGRAM`。 |
| LLVM | 显式 `DVS_LLVM_BIN`；否则当前缓存、所选 VS 的 `VC/Tools/Llvm/x64/bin`、标准 LLVM 安装、PATH。两个工具都必须为 19.1.5，解析出的绝对路径显式传入 CMake。关闭质量工具的 preset 不要求 LLVM。 |
| Qt 工具 | qmlformat、qmllint 只能来自当前 vcpkg 安装树，CMake 不回退到其他 Qt 的 PATH；已有缓存指向别处也报错。 |

导入 vcvars 后重新排列进程内 PATH，并逐项核验 `cl`、`link`、`lib`、CMake／CTest、Ninja、
启用的 LLVM 工具，以及已配置的 Qt 工具。`-Doctor` 输出实际绝对路径和所选工具版本；工具无须
永久加入系统 PATH。已有缓存编译器／归档器必须与当前 MSVC 匹配，不能因旧文件仍存在就混用。
本次选择有效时不会因 PATH 前面多了一个程序而切换工具或触发无谓重编。

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

vcpkg 的 builtin registry 通过 git 读取端口和 baseline，所以 manifest 模式要求 vcpkg 根
**自身**是一个 git 仓库。给根补仓库时**必须全量克隆**：`--filter=blob:none` 的 blobless 克隆
体积小得多，但缺 blob 时会按需联网拉取，一旦网络不可用就变成难以归因的失败，比明确报错更糟。
若根确实没有 `.git`，环境自检会报出该能力缺失并提示本地改用
`-UseInstalledDependencies`（`VCPKG_MANIFEST_INSTALL=OFF`，直接用已装好的 `out/vcpkg` 树）。
当前工作站两者都可用：已为该根补上真实 git 仓库（含 baseline ref），本地仍推荐预装模式，
因为它更快、更确定。这不是 CI 或新机器的默认设置。

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

## ZIP 只打包必要运行时

这是后续所有发布的固定规则：**只随包交付当前产品实际需要的程序、运行时、资源及许可证**。
Qt／CMake 自动复制了某个文件，不等于该文件已获准进入发布包；也不能只为缩小体积删除依赖。

- 保留 GUI／CLI、版本对应的 shell DLL、实际加载的 Qt／FFmpeg DLL 和 QML 模块、Basic 控件
  样式、品牌资源、第三方许可证，以及右键注册入口和安装说明。
- CRT／UCRT 采用程序旁的 DLL，由 `InstallRequiredSystemLibraries` 部署。Qt 使用
  `NO_COMPILER_RUNTIME`，不再附带重复的 `vc_redist.x64.exe` 等运行库安装器；不能连 DLL 一起删。
- 当前 `GraphicsBackend` 固定 Direct3D 11；产品没有 D3D12 路径，不随包交付
  `dxcompiler.dll`／`dxil.dll`。支持的 Windows 10／11 自带 D3DCompiler_47，Qt 使用
  `--no-system-d3d-compiler`／`--no-system-dxc-compiler`，不重复复制系统编译器。
  `HlslHeaderCompiler` 在构建时生成着色器字节码，不是用户侧运行程序。
- 不交付开发符号（PDB／ILK）、调试插件、未启用的控件样式、外部 ffmpeg／ffprobe 工具、
  测试和性能素材、构建缓存或旧版本文件。新增产品功能确实需要其中某项时，先修改对应规则。

实现入口是 `cmake/Install.cmake`；`cmake/VerifyRuntimePayload.cmake` 由 CPack staging 门禁调用，
拒绝多余 EXE、图形编译器、开发符号和未交付插件，同时要求四个关键 app-local CRT／UCRT DLL
非空。`quality.runtime-payload` 用隔离目录验证缺失、空文件、目录伪装、大小写及嵌套路径，
并强制断言 **27** 个检查，防止规则回退或测试静默少跑。

每次发布必须：

1. 从当前候选构建和标准 CPack 流程生成 ZIP，不手工删包内文件后冒充同一候选。
2. 对比上一发布的 ZIP 清单、压缩前／后大小；逐项解释新增文件，不能仅报告总大小。
   新增依赖须附真实调用／动态加载依据；图形后端或最低 Windows 版本变更须同时更新部署策略。
3. 在解压后的包上验证 CLI 启动、媒体打开、QML 导入／对话框和图形渲染；检查包内字节与当前
   构建一致。干净 Windows 环境验证不得借用开发机 PATH、Qt 安装树或预装 VC++ 运行库。
4. 保留既有硬件／性能和其他发布门禁。精简包验证不代替这些门禁，旧候选证据不认证新候选。

本轮 2.0.2 → 2.0.3 草稿的差异证据为 27,908,959 → 62,665,162 bytes；新增的四个文件
`vc_redist.x64.exe`、`dxcompiler.dll`、`d3dcompiler_47.dll`、`dxil.dll` 贡献 34,752,984 bytes
压缩体积，几乎占全部增量。此次规则收紧解决的是部署范围，不是降低产品功能或修改压缩率。

## 修改完成前

按改动运行相关测试、format-check、lint，并检查实际执行数量。
构建脚本回归测试使用隔离的模拟工具，覆盖工具路径、参数透传、stdout/stderr 保留、失败传播和缓存恢复；
真实编译仍由项目构建与头文件依赖门禁验证。脚本测试同时进入 CTest 和 CI。
视频性能验收仍遵循 [runner 门禁](self-hosted-runner.md)，不以构建或 WARP 测试代替硬件证据。

本次构建可靠性修复和验证结果见 [问题台账 E-01](engineering/visual-review-backlog.md#e-01-构建工具与增量依赖可靠性)。
