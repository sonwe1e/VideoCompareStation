# QML 类型注册与类型化接线：验证记录（2026-10-16 / 10-17）

状态：工作区记录（基线 `8c90005` 加未提交改动）。设计决策与反例见
[ADR 0006](../adr/0006-qml-type-registration.md)。本页覆盖 2026-10-16 首轮交付与
2026-10-17 审查修复（R1 真实功能类型化、R2 生产依赖契约、R3 生成与交付验证）。
证据目录：`out/verification/qml-interface-pilot/`（首轮）与
`out/verification/p1-real-controller/`（审查修复轮）。

## 1. 探针结论（pinned Qt 6.11.1，独立小工程）

| 探针 | 结论 |
|---|---|
| `out/verification/qml-wiring-probe/` | 手工 `.qmltypes` 可让 qmllint 报 `channelTxt` 拼写错误（`missing-property` 附修正建议）；`typeof` 守卫不阻止未声明标识符告警 |
| `out/verification/qml-module-probe/` | `qmltyperegistrar --generate-qmltypes` 可从"包装目标 + 被包装目标"的 moc JSON 生成完整 `.qmltypes`；uncreatable 类型不可由 QML 创建；`qt_add_qml_module` 生成的注册 C++ 对 `final` 类触发 MSVC C3246（`QQmlElement<T>`），因此只生成描述、运行期手写注册 |
| 运行期探针 `probe_main` | `createWithInitialProperties`：正确对象可用；**缺 Required 属性使创建硬失败**；错类型只告警并置空 —— 后者就是 R2 需要装载后身份核验的原因 |
| qmltestrunner 编译探针 | `QtObject` 假对象赋给类型化属性是编译期错误 → 假控制器套件必须迁到真实服务（R1） |

## 2. R1：真实功能类型化

- `ImageWorkspace.qml` 的 `controller`／`colorClipboard` 为 **required 类型化属性**，
  `imageReview` 别名保留具体类型；试点函数（`channelText`／`deltaText`／`rgbText`／
  `channelText16`／`hexText`／`captureProbeSample`／`probeColorText`／`copyProbeText` 链路）
  删除 `typeof … === "function"` 防护与重复格式化回退；保留 `numericFloat` 显式读取作为
  QML 绑定依赖（invokable 内部状态读取对绑定不可见），并保留"无样本""素材失效"等业务条件。
- `ui.ImageWorkspaceManual` 从 stock `qmltestrunner` 迁到仓库自建 QuickTest runner
  `tests/component/ui/ImageWorkspaceQmlTestMain.cpp`：注册真实类型、持有真实
  `ImageReviewController`／`ColorClipboardController`、经真实注入/打开路径生成小型素材
  （含 marker 合成 loader 的 16-bit sidecar 高位深对）、提供 `vcs-review` 图像 provider；
  `tst_image_workspace_manual.qml` 全部 35 项在真实控制器上重写并通过（含真实复制断言：
  成功分支读回系统剪贴板内容，失败分支断言显式"复制失败"提示）。
- **变异证据（本轮接受标准）**：`channelText → channelTxt` 与 `copyText → copyTxt` 均使
  `qmllint --max-warnings 0` 失败（`missing-property`）：
  `mutation-channel-typo.log`、`mutation-copy-typo.log`。修复前这两条调用链在抑制区内，
  工具完全不可见。

## 3. R2：生产依赖契约

- `Main.qml` 把 `stillImageController`／`colorClipboardService` 声明为 **required 类型化
  根属性**；组合根经 `engine->setInitialProperties(mainWindowPilotProperties(...))` 传入；
  装载后、显示窗口前由 `verifyMainWindowPilotServices` 核验身份（覆盖"显式 null"与
  "错类型降级为 null"两种绕过 Required 的状态）。
- C++ 主窗测试统一经 `pilotWindowProperties()` / `defaultPilotWindowProperties()` 注入
  （25 处 Main.qml 装载点全部迁移，图片相关 6 处传真实控制器与真实剪贴板）。
- **真实入口反例**（`ui.MainQmlContractTests.MainWindowRequiredPilotServicesAreEnforced`，
  通过）：正确服务 → 创建与核验通过；缺控制器／缺剪贴板 → 创建硬失败（Required 属性未初始
  化）；显式 null → 创建成功但核验拒绝；错类型对象 → 告警降级为 null，核验拒绝。
- **变异证据**：把 `checkService` 的对象比较改为恒用期望值（核验失效）后，该用例的 null 与
  错类型分支失败（`mutation-r2-verify.log`）；恢复后 SHA256 与变异前一致
  （`F0B46CEB…78189`），重测通过。

## 4. R3：生成与交付验证

- `cmake/NormalizeQmlTypes.cmake` 重写为"契约校验 → 拼写归一化 → 发布并读回"三阶段：
  31 项计数校验（组件集各恰一次、总数、两个控制器 uncreatable、三个可创建、试点关键成员
  与枚举、关键 C++ 拼写残留），另 6 项读回校验，共 **37 项**；脚本自断言检查数。拼写映射
  `qreal→double`、`qint64→qlonglong`、`quint64→qulonglong`，不要求替换次数 > 0（未来 Qt
  直接输出 QML 拼写可通过）。
- `quality.qmltypes_contract`（新 CTest）：正例基线 + 8 个反例（缺类型、重复类型、缺关键
  成员、可创建性翻转、残留 `qint32`、空输入、截断、已规范化输入）实际执行 **9 例 + 3 项
  内容检查**，仅正例与已规范化例通过。
- **注册与工具描述一致性**（`GeneratedDescriptionMatchesTheRuntimeRegistration`）：读取
  qmllint 消费的 `.qmltypes`，断言组件名集合与运行期注册一致、每个类型可在运行期解析、
  可创建性一致。变异：运行期把 `ColorClipboardController` 改名后，注册套件 **3/7 失败**
  （`mutation-r3-verify.log`）；恢复后哈希一致（`4A1532EB…CFE8`）并重测通过。
- `qmllint --max-warnings 0 -I <build>/qml-imports`：`Main.qml`、`CompareModeBar.qml`、
  `ComparisonViewport.qml`、`ReviewShortcuts.qml`、`ImageWorkspace.qml`、`TabbedInspector.qml`
  全部通过；`qint64` 等残留拼写会先被生成脚本拒绝。
- **core-dev**（`-Test`）：307/307 通过（`core-dev-gate.log`）。
- **ui-dev**：原先在新依赖树里连编译都不过（GUI/适配器相关测试源未被守卫）；本轮把它们
  移入 `if(DVS_BUILD_ADAPTERS)` 后 configure/build 通过并以 `-Test` 运行（`ui-dev-gate.log`、
  `ui-dev-build-04.log`、`ui-dev-test-05.log`）。**未全绿**：425 项中 402 通过、23 项超时，
  全部集中在"等待后台线程"的两套（`VideoFolderModelTests` 与 `Timelines/ClipExportTimelineTests`）。
  归因证据（同一二进制、同一启动方式、逐项排除）：超时进程挂着 **MSVC CRT 断言对话框**，
  内容为 Qt 内部断言 `ASSERT: "previous > 0" in qcoreapplication.cpp:1902`（
  `sendPostedEvents`）；该二进制直接运行通过（多次）、换成 dev 树的 `Qt6Cored.dll` 后经
  同样的 ctest/cmake 启动也通过，涉及套件与生产代码均未被本轮改动 → 触发点在
  `out/vcpkg-ui` 新构建的 QtCore 调试 DLL 与该等待时序的组合，属依赖构建/环境问题，
  单独立项处理，不在本轮用改测试阈值或屏蔽断言的方式掩盖。
- **发布包**：见下节。

## 5. 发布包验证（R3 交付）

- **release 全量回归**：`-Preset release -Test -TestJobs 8` **100% 通过（1397 项启用）**，
  328.48s；未运行清单 = 9 项环境/素材跳过 + 4 项既有禁用 + 1 项屏幕几何环境跳过
  （`release-tests.log`）。
- **ZIP**：`out/package/zip/CompareStation-2.5.0-windows-x64.zip`，**28,327,485 bytes**、
  272 文件、解压后 **70,418,759 bytes**，SHA256
  `28176C26632573ACE8338D7B817D5780625791EB93E9A0ABCC6B0B8920BEBDE4`。
  与上一 2.5.0 候选（28,316,760 bytes / `6572F050…C1E9A`，其逐文件记录取自
  `out/verification/release-v2.5.0-20261016-01/manifest.json`）相比：**0 新增、0 删除、
  57 变化**（两个入口二进制 + 今日重建后重新部署的 Qt/FFmpeg 运行库），无新增第三方运行时。
  逐文件清单与差异：`manifest.json`、`package-comparison.json`（3 项读回校验）。
- **包校验**：`verify-release-package.ps1` **16/16、0 失败**（`package-verify.log`）。
- **解压 smoke**：解压到独立目录后以"仅系统 PATH、清除全部 Qt/QML 环境覆盖"运行
  **9/9 探针通过**（CLI 启动/媒体探测、单/双/三源 UI smoke、图片 Alpha 渐变对差异探测、
  三档 DPI 弹窗像素）＋ 12 项弹窗像素检查全部通过（`smoke-results.json`、
  `smoke-*.log`、`PackageSmoke.ps1`）。

## 6. 执行过的门禁（本轮，逐条）

| 门禁 | 结果 | 日志 |
|---|---|---|
| 定向批次（MainQml 契约 / 工作区人工 / 注册 / 生成契约） | **143/143 通过**（4 既有禁用、1 环境跳过） | `focused-r1r2-03.log` |
| `ui.ImageWorkspaceManual`（迁移后） | 通过（真实控制器/剪贴板） | `manual-suite-run-02.log` |
| `ui.QmlTypeRegistrationTests` | 7/7 通过（含一致性用例） | `registration-tests-03.log` |
| `quality.qmltypes_contract` | 9 例 + 3 检查通过 | `qmltypes-contract-direct-02.log` |
| core-dev `-Test` | 307/307 | `core-dev-gate.log` |
| ui-dev `-Test` | 402/425；23 超时（Qt 断言 + CRT 对话框，见 §4） | `ui-dev-test-05.log` |
| dev 完整回归（最终树） | 1397 项启用 = **1396 通过 + 1 并行 flake**；flake 串行复测通过、直接重复 20 次全过 | `full-tests-final-02.log`、`splash-serial-recheck.log`、`splash-repeat20.log` |
| `format-check` | 通过（全部 C++/生产 QML；新增文件经 clang-format 19.1.5） | `format-check-final-03.log` |
| `lint` | 通过：clang-tidy 零发现；qmllint（修复 Main.qml 的抑制区间后）零告警 | `lint-final.log`、`lint-qml-fix.log` |
| release 全量 | 1397 项启用 100% 通过 | `release-tests.log` |

补充说明：R2 变异与恢复轮之间曾出现一次依赖树事故——被中断的 vcpkg 安装把
`qtdeclarative` 置为 `purge ok not-installed`，随后单包重建（19 分钟）恢复；恢复后 dev
构建与定向批次按上表通过。

## 7. 仍然开放

- `ImageWorkspace` 之外的控制器（`imageEdit`、`folderPairModel`、`pairMetricsModel` 等）
  仍是 `var`；按功能逐个迁移。
- ui-dev 的 Qt 断言问题（§4）未归因到具体 Qt 变更；若影响面扩大需要提 Qt/vcpkg 问题单。
- 颜色管理、显示路径、性能诊断未开始；样本与口径设计见
  [image-color-samples.md](image-color-samples.md)。
