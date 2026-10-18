# CompareStation 未提交成果整合与分批交付计划

日期：2026-10-18。状态：只读调查与实施计划；本轮未修改产品代码、未配置构建、未运行测试、
未启动 GUI。文中的验收条件是未来完成标准，不是当前软件已经通过的记录。

- 工作区基线：`8c90005e0f6ba5b71ceb284682259df8d29a307a` + 未提交改动
- 目标基线：`main @ 66475bb2e4f6333f3c6a57fb68933261f00e3686`（PR #52 合并提交）
- 上游计划：[收尾后分阶段改进计划](CompareStation_收尾后分阶段改进计划_2026-10-17.md)
- 数据契约：[图片像素与颜色数据契约](../engineering/image-color-contract.md)、
  [媒体支持](../media-support.md)、[问题台账](../engineering/visual-review-backlog.md)

## 0. 结论摘要

1. **合并面很小但不可省**：main 相对本地 HEAD 只多 1 个提交（`66475bb`），改 27 个文件；
   其中 4 个与工作区成果重叠、10 个是工作区完全缺失的新文件。这不是"拉一下"，而是一次
   冲突面已可枚举的三方合并。
2. **最容易被漏掉的不是冲突，而是计数型门禁**：main 的
   `tools/quality/check-ci-test-routing.py` 把清单总数固定断言为 1409、软件集 1387，
   自检算术也写死同一组数字。工作区新增 55 个用例后该门禁必然失败。必须按合并后的真实
   枚举更新，**不能沿用 1409、1387 或文档里的 1433 中任何一个**。
3. **两个"当前总数"互相矛盾**：环境文档记 `dev` 注册 1433 项，main 门禁固定 1409 项。
   两者来自不同 SHA、不同预设与不同 QML 矩阵配置，都只能说明各自的构建，不能作为合并后
   的通过证据。
4. **已有成果不存在重复实现风险**：QML 类型化接线、COLOR1–COLOR4、FLOW1、FLOW2、
   MEDIA1 调查都在工作区里；缺口集中在"合并面 + 固定计数 + 文档基线 + 两个具体缺陷"。
5. **两个具体可信度缺陷（本轮只记录，不修）**：
   - 16-bit PNG 经 Qt 回退在 `ImagePairLoader.cpp:233` 被强制转 RGBA8，而该路径没有任何
     代码写 `bitDepth`（只有 FFmpeg 路径在 `src/app/Main.cpp:4147` 写 `sourceBitDepth`），
     所以精度标签会把 16-bit 文件显示成 8-bit。
   - 图片编辑的未保存修改只在"关闭单侧"（`ImageWorkspace.qml:224`）拦截；替换、打开新图、
     文件夹换行、问题恢复都不检查脏状态，恢复会静默丢弃编辑副本。
6. **COLOR4 的转换没有控制器侧缓存**：`displayColorImage()` 每次显示请求都做整图
   `convertedToColorSpace`，转换副本不计入 `estimatePairWorkingSet`。URL 已含颜色开关与
   内容代次，失效语义是对的，但重复转换与内存上限未验证。
7. **FLOW3 不需要新建图片模型**：`VideoFolderModel` 已有自然排序（`naturalLess`）、
   无环绕 `step()`、`isStillImagePath` 分类与 `stageImageOpen`/`finishPendingImageOpen`。
   FLOW3 的缺口在图片工作区的接线与离开规则，不在模型。

## 1. 只读核对结果

### 1.1 仓库事实

| 项目 | 实测值 | 取得方式 |
|---|---|---|
| 本地 HEAD | `8c90005e…`（`feat: release CompareStation 2.5.0 review workflow updates`） | `git log -1` |
| 本地分支 | `main` 跟踪 `origin/main`，两者都停在 `8c90005` | `git branch -vv` |
| 远端 main | 已前进；`FETCH_HEAD` 记录 `8d054ff…`，本地对象库**没有** `66475bb` | `git cat-file -t` |
| PR #52 | 已合入，head `4f2e0dd`，合并时间 2026-10-17T09:33:48Z | `gh pr view 52` |
| 合并提交 | `66475bb` 的父提交即 `8c90005`，因此 main = 基线 + 1 提交 | `gh api compare` |
| 合并面 | 27 个文件（10 新增、17 修改），ahead 1 / behind 0 | `gh api compare` |
| 工作区已修改 | 43 个受跟踪文件，+3091 / −507 | `git diff --stat` |
| 工作区未跟踪 | 31 项，其中 `.sessions/`、`.workbuddy-ai/` 是本地产物，其余 29 项是成果 | `git status --porcelain` |

PR #52 的实际范围是普通软件门禁迁移（`.github/workflows` 三份 + 两个组合 action +
`tools/ci/*` + `tools/quality/check-ci-*` + `docs/engineering/hosted-windows-ci.md`），
并顺带把 `tests/component/ui/CMakeLists.txt` 拆成 model-only 与 adapters 两段、把 5 个
真实硬件用例移回手动 preset。PR 描述明确写了"本分支不包含原工作区其余未提交的 QML 收尾改动"。

### 1.2 合并面分类

| 类别 | 文件 | 处理要点 |
|---|---|---|
| 双方都改 | `cmake/Quality.cmake` | main 加 `DVS_LLVM_BIN` 提示；本地加 qmllint `-I` 与生成依赖。位置相距约 200 行，语义互补，需并存 |
| 双方都改 | `tests/component/media/CMakeLists.txt` | main 加 `TEST_FILTER` 硬件拆分与新注册；本地加 `DVS_IMAGE_FIXTURE_DIR`。必须保留拆分，新图片夹具定义不能被硬件过滤段吞掉 |
| 双方都改 | `tests/component/ui/CMakeLists.txt` | 双方独立做了同一段 model-only 拆分（文本一致，可干净合并）；但 main 在菜单 QML 循环里加了 `-o` 日志参数，本地把 `ui.ImageWorkspaceManual` 换成自建 runner。两处必须都在 |
| 双方都改 | `docs/engineering/visual-review-backlog.md` | main 在顶部加 CI 小节，本地也加阶段小节；合并后顺序与"环境分离未迁移"的表述必须修正 |
| main 新增、本地缺失 | `.github/actions/{setup-windows,save-windows-cache}/action.yml`、`docs/engineering/hosted-windows-ci.md`、`tests/fixtures/ci-ui-routing/trace.cmake`、`tools/ci/{setup-hosted-windows,test-hardware-evidence}.ps1`、`tools/quality/{check-ci-evidence,check-ci-test-routing,check-ci-workflows,check-ui-test-routing}.*` | 必须原样进入合并结果；缺任一项，托管 CI 无法运行或路由检查失效 |
| 仅 main 修改、本地未碰 | `cmake/Testing.cmake`、`tests/component/platform/CMakeLists.txt`、`tests/component/ui/ImageEditControllerTests.cpp`、`tests/component/ui/VideoFolderModelTests.cpp`、`tests/component/ui/qml/tst_active_source_strip.qml`、`tests/unit/shell_windows/FileAssociationRegistrationTests.cpp`、`src/shell_windows/FileAssociationRegistration.cpp`、`vcpkg.json` | 直接取 main 版本；`vcpkg.json` 已被 main 加入 `qtbase/thread` 特性，批次 2 的 zlib 决策要在这一版上叠加 |

### 1.3 计数型门禁（本次调查的关键发现）

| 门禁 | 固定内容 | 合并后的影响 |
|---|---|---|
| `tools/quality/check-ci-test-routing.py` | 8 项检查：清单总数 `1409`、软件集 `1387`、5 个 HARDWARE 用例名、软件/硬件集合互斥；自检构造 `1387 + 5 + 17 = 1409`，4 个反例 | 工作区新增用例后总数与软件集都会变，**该门禁必然失败**；自检算术必须同步，否则自检自己就是假的 |
| `tools/quality/check-ui-test-routing.py` | `MODEL` 9 个源、`ADAPTER` 10 个源、5 个案例 | 工作区 `dvs_ui_component_tests` 源集合与 main 完全一致，**不受影响**；但任何新增源若落进该目标会立刻失败 |
| `tools/fixtures/verify-fixtures.py` | 652 项检查、37/37 字节一致，自断言检查数 | **没有注册进 CTest 或任何 workflow**，目前是手工脚本。依赖 Pillow（含 ImageCms）与 PATH 上的 `ffmpeg`/`ffprobe` |

新增用例统计（用于预估，不作为验收数字）：受跟踪测试文件新增 28 个用例；5 个新测试文件
（`ImageColorContractTests` 12、`ImageColorConversionTests` 3、`ImageColorSampleTests` 2、
`ColorClipboardControllerTests` 2、`QmlTypeRegistrationTests` 8）共 27 个；合计 55 个。
`tst_image_workspace_manual.qml` 被重写但仍是单个 CTest 条目。合并后必须重新枚举实际数量。

## 2. 已有实现盘点（不要重复实现）

| 能力 | 现有实现（入口） | 已有证据 | 状态 |
|---|---|---|---|
| QML 单一注册 + 生成契约 | `registerDvsUiTypes()`（`QmlTypeRegistration.cpp`）、`src/ui_qml/qmltypes/`、`NormalizeQmlTypes.cmake`、`CheckQmlTypesContractTests.cmake` | `QmlTypeRegistrationTests` 8 项、`quality.qmltypes_contract` 正例 + 8 反例 | 工作区实现，未合入 |
| 主窗类型化接线 | `Main.qml:137` `required property ImageReviewController`、`:138` `ColorClipboardController`；`MainWindowDependencies.{h,cpp}` 的 `mainWindowPilotProperties` / `verifyMainWindowPilotServices` | 主窗契约反例五例 | 仅 2 个控制器完成 |
| COLOR1/COLOR3 数据契约与状态 | `ImageColorContract.{h,cpp}`、`StillImage::iccProfile`、`extractEmbeddedIccProfile()`、`primaryColorStatusText()` | `ImageColorContractTests` 12 项 | 工作区实现 |
| COLOR2 样本与独立参考 | `tests/fixtures/images/color/`（27 个文件含 manifest）、`tools/fixtures/*.py` | `verify-fixtures.py` 652 项、`ImageColorSampleTests` 2 项 | 工作区实现 |
| COLOR4 SDR 显示转换 | `ImageReviewController::displayColorImage()`（`:2370`）、`imageColorDisplayBadge`、状态句 | `ImageColorConversionTests` 3 项、controller 3 项、主窗 1 项 | 工作区实现，缺缓存与工作集 |
| FLOW1 观察恢复 | `Main.qml:2241 advanceImageIssueRestore`、`restoreObservationPosition`、`IssueViewContext` 5 个可选字段、`IssueLogController::performRestore`（走 `evaluateIssueRestore`） | repository 3 项、controller 2 项、主窗 2 项 | 工作区实现，缺脏检查 |
| 来源身份核验 | `IssueSourceRef` 的 `byteSize`/`modifiedUtcMilliseconds`/`fingerprintSha256`、`IssueSourceMatch`、`IssueRestoreDecision` | `IssueRecordTests`、`IssueLogControllerTests` | 已在基线 |
| FLOW2 样本交接 | `SampleExportWriter.cpp:756` `member_identity`/`candidate_output_frame`/`sources`；`:615` 写后读回校验；`:764` `color_management` | `SampleExportWriterTests` 12 项 | 工作区实现 |
| FLOW2 片段交接 | `ClipExportReport` 新增 `lastPresentationMicroseconds`/`endOfStreamReached`/`stoppedAtRequestedEnd`/`requestedFrameCount`/`startShiftMicroseconds`；`ClipExportDialog` 显示"时间线主源"+文件名 | `ClipExportWriterTests` 9 项 | 工作区实现 |
| FLOW2 截图语义 | `Main.qml:1938` 标注句"显示结果 · 屏幕像素 · 未色彩管理；非源文件原始码值" | 主窗 `ComparisonCaptionStatesTheDisplayResultContract` | 工作区实现，仅文本 |
| 单目录浏览外壳 | `VideoFolderModel`：`naturalLess`（`:72`）、`step()` 无环绕、`isStillImagePath` 分类（`:294`）、`stageImageOpen`/`finishPendingImageOpen` | `VideoFolderModelTests` | 已在基线，未接图片工作区 |
| MEDIA1 格式调查 | 12 个格式夹具 + `StillImageDecoderTests` 9 个新用例、`MediaProbeTests` 3 个 AV1/VP9 拒绝用例 | 定向通过（工作区记录） | 调查完成，支持决定未做 |

## 3. 具体缺口

### 3.1 批次 1（整合）缺口

| 编号 | 缺口 | 证据 |
|---|---|---|
| G1-1 | 合并面未处理：4 个重叠文件、10 个缺失文件 | §1.2 |
| G1-2 | 计数型门禁固定值失效，且自检算术写死同一组数字 | `check-ci-test-routing.py` 的 `validate()` 与 `--self-test` |
| G1-3 | 两处必须并存但容易互相覆盖：main 的 qmltestrunner `-o` 日志、本地自建 `ui.ImageWorkspaceManual` runner | `tests/component/ui/CMakeLists.txt` |
| G1-4 | 本地未跟踪产物未忽略：`.sessions/`、`.workbuddy-ai/` 会污染提交 | `git status --porcelain` |
| G1-5 | 基线文档全部指向旧 SHA：`test-environment-separation.md`（1433、self-hosted 审计）、`reproducible-baseline.md`（`8c90005`）、`media-support.md`（"working tree on `8c90005`"）、`image-color-contract.md`（同） | 各文档首段 |
| G1-6 | 台账"环境分离未迁移：Windows 构建/native-quality/release/hardware 仍全部选择 self-hosted"已部分过时：普通软件门禁已迁托管，硬件仍手动 | 台账 2026-10-17 小节 vs PR #52 |
| G1-7 | 夹具完整性校验未入门禁：652 项检查只有手工入口，且依赖 PATH 上的 ffmpeg（违反"不能借开发机 PATH 通过"的打包原则） | `verify-fixtures.py` 未出现在任何 CTest/workflow |
| G1-8 | 类型化接线只覆盖 2 个控制器：`imageEditService`、`folderPairModel` 仍是 `typeof` 守卫的 `var` | `Main.qml:142-143` |

### 3.2 批次 1B（交接可信度收口）缺口

| 编号 | 缺口 | 证据 |
|---|---|---|
| G1B-1 | 脏检查只在关闭单侧生效；`requestReplacePrimary/Secondary`（`Main.qml:1758/1761`）、`requestOpenPrimary`（`:2485`）、文件夹换行、问题恢复都无守卫 | `ImageWorkspace.qml:224` 是唯一入口 |
| G1B-2 | `imageEdit` 是 `property var imageEdit: null`，缺依赖时 `closeSideBlockedReason` 返回空串，脏检查**静默放行**（fail-open） | `ImageWorkspace.qml:87`、`Main.qml:142` |
| G1B-3 | 截图语义只有文本，没有输出侧颜色声明；该句目前只覆盖视频对比窗口，若图片将来复用同一入口会与 COLOR4 的"显示已转换"冲突 | `Main.qml:1938` |

### 3.3 批次 2（格式收口）缺口

| 编号 | 缺口 | 证据 |
|---|---|---|
| G2-1 | 当前 FFmpeg 只选 `avcodec/avformat/swscale`，无 zlib → **没有 PNG 解码器**，Deflate TIFF 明确失败 | `vcpkg.json:17-26`、`DeflateTiffFailsExplicitlyInsteadOfClaimingSupport` |
| G2-2 | 16-bit PNG 精度：Qt 回退强制 RGBA8，且无人写 `bitDepth` → 精度标签错误 | `ImagePairLoader.cpp:233-235`、`Main.cpp:4147` |
| G2-3 | 无未压缩 TIFF 夹具；损坏/超限 PNM 拒绝未覆盖；Release ZIP 内解码器可用性未验证 | `media-support.md` 的"Still not verified"列 |
| G2-4 | AV1/VP9 只有拒绝证据：真实夹具证明 `kUnsupportedCodec`；`.webm` 不在入口 | `MediaProbeTests` 3 项、`ExplorerCommandSupport.cpp:13` 只有 `.mkv` |
| G2-5 | 缺"依赖与包体"书面决定：zlib / Qt RGBA64 / 明确拒绝三条路没有 ADR | 无 |

### 3.4 COLOR5 缺口（独立列出）

| 编号 | 缺口 | 证据 |
|---|---|---|
| G3-1 | `displayColorEnvironmentVerified()` 恒为 `false`（当前正确，但也说明没有任何输出链路证据） | `ImageReviewController.cpp:657` |
| G3-2 | 未建立"窗口 → 目标显示器 → 配置文件 → Qt Quick/D3D 输出解释"的解析，无法排除重复转换 | 无实现 |
| G3-3 | 未处理跨屏、运行中配置切换、睡眠唤醒、系统 HDR/Advanced Color 状态变化后的重算或降级 | 无实现 |
| G3-4 | 无物理显示环境证据；本机存在 `OrayIddDriver`、`GameViewer Virtual Display Adapter`，截图不能作物理证据 | `reproducible-baseline.md` §2 |
| G3-5 | COLOR4 的转换缓存/工作集/开关持久化未收口，会先于 COLOR5 成为瓶颈 | `displayColorImage()` 无缓存 |

### 3.5 FLOW3 缺口

| 编号 | 缺口 | 证据 |
|---|---|---|
| G4-1 | 图片工作区没有目录列表与上一张/下一张；`FolderPairSidebar` 只服务双目录同名配对 | `ImageWorkspace.qml` 无目录模型 |
| G4-2 | 未保存编辑规则不稳定（见 G1B-1/2），这是 FLOW3 的硬前置 | 同上 |
| G4-3 | 任务切换语义未定义：单图导航、双目录配对、编辑副本三种工作流之间如何离开、离开时谁负责提示 | 无实现 |
| G4-4 | 边界与坏文件行为未接图片侧：`step()` 已无环绕，但"首尾禁用 + 坏文件保留当前图并提示"未在图片工作区实现 | `VideoFolderModel.cpp` 有 `step`，图片侧未用 |

## 4. 分批计划

### 批次 1A：把工作区成果整合到最新 main（必做，下一批 MDS）

**范围**：只做合并、门禁重绑定与基线文档重写，**不新增功能、不改产品行为**。合并后的
提交必须同时包含 `66475bb` 的全部 27 个文件内容与工作区 43+29 项成果。

**依赖**：无产品依赖；需要 GitHub 托管 `windows-2022` 可用（PR #52 已证明），
以及能读取远端对象（本轮只读，不做 fetch）。

**已有实现**：§2 全部条目；不需要重写任何一项。

**具体缺口**：G1-1 至 G1-8。

**实施要点（不改代码，只列方法）**

1. 先把工作区成果固化为可回退的本地提交（建议按 QML 接线 / 颜色契约 / FLOW1 / FLOW2 /
   夹具与文档分 5 个提交），或至少 `git diff > backup.patch` 并把 29 项未跟踪文件复制到
   仓库外的暂存目录。**禁止**在未固化前直接 `git pull`/`reset`。
2. `git merge origin/main`（不用 rebase，避免重写已固化的提交）；冲突预期只在 §1.2 的 4 个
   文件。逐个确认：
   - `Quality.cmake`：保留 `DVS_LLVM_BIN` 提示 + qmllint `-I`/生成依赖两侧内容；
   - `tests/component/media/CMakeLists.txt`：保留 `TEST_FILTER` 拆分 + `DVS_IMAGE_FIXTURE_DIR`；
   - `tests/component/ui/CMakeLists.txt`：保留菜单 QML 的 `-o` 日志 + 自建
     `dvs_image_workspace_qml_tests` runner + 全部新目标；
   - 台账：CI 小节在上，阶段小节在下；修正"环境分离未迁移"的表述。
3. 补齐 10 个 main 新增文件（若 merge 已带入则核对内容哈希）。
4. 更新 `check-ci-test-routing.py`：用合并后 `ctest --show-only=json-v1` 的真实枚举替换
   1409/1387，并同步 `--self-test` 的构造算术（软件数 + 5 硬件 + 17 packaged）。
   自检的反例数量（4）与检查数（8）保持不变。
5. `.gitignore` 增加 `.sessions/`、`.workbuddy-ai/`（本地产物，不进入提交）。
6. 重写基线文档：合并提交 SHA、ZIP 哈希（若重建）、`dev` 实际注册数、托管 CI 实际
   runner 与作业名；明确"普通软件门禁已迁移，硬件/物理显示仍手动"。

**验收标准**

- 合并结果对 main 的 27 个文件逐一哈希一致（除 4 个已并存的冲突文件按并集核对内容）。
- 工作区成果零丢失：43 个受跟踪文件的改动在合并提交中仍可检出；29 项未跟踪文件全部入库，
  且 `git status` 只剩本地产物。
- 云端（`windows-2022`）在该合并提交上各执行一次：Debug 完整软件回归、Release 完整软件
  回归、`format-check`、`lint`；结果按"实际执行 / 跳过 / 禁用 / 失败"四列记录，**不得**把
  跳过或未执行写成通过。
- `check-ci-test-routing.py --preset dev` 8 项检查通过且数字来自本次枚举；
  `--self-test` 输出 8 检查 + 4 反例。
- `check-ui-test-routing.py` 的 9+10 源集合与 5 个案例通过（`--self-test` 与 `--adapters on/off`）。
- `quality.qmltypes_contract` 的正例与 8 反例通过；qmllint 在托管环境能解析 `Dvs.Ui`
  （证明 `-I` 与生成依赖确实生效，而不是靠本机缓存）。
- 台账只追加证据，不关闭任何 ID（关闭需提交、diff、命令、实际数量、结果）。

**可独立交付条件**：满足。它不改变产品行为，可作为后续所有批次的唯一可信基线先合入。
硬件、D3D11VA、性能、物理显示门禁**不在**本批范围，保持待验。

#### 1A 执行状态（2026-10-18）

本地已完成，**未推送**：

- 备份：`out/verification/batch1a/`（`tracked-changes.patch` 5391 行、`untracked-list.txt`
  76 项、`untracked-files.tar`）。
- 成果固化：`3044f61` chore（忽略 `.workbuddy-ai/`、`.sessions/`）、`9f60e9c` feat
  （104 个代码/测试/夹具文件）、`2154c2b` docs（11 个文档）。
- 合并：`1f5da53`。唯一文本冲突是台账（CI 小节在前、阶段小节在后）；三个 CMake 文件自动
  合并后逐项核对（`DVS_LLVM_BIN` + qmllint `-I`、硬件 `TEST_FILTER` + 图片夹具目录、
  model-only/adapter 拆分 + 自建 runner + 菜单套件 `-o`）。
- 门禁诊断：`79f8a0f`。固定计数抽成 `EXPECTED_TOTAL`/`EXPECTED_SOFTWARE`，失败信息报出实测
  总数/软件/硬件数；自检保留独立字面量作为一侧漏改的报警。本地自检 8 检查 + 4 反例通过。
- 文档重绑：`45664ea`（环境分离、可复现基线、媒体支持、颜色契约、台账状态）。
- 已做的静态核对：相对 `2154c2b` 只变 `66475bb` 的 27 个文件；相对 `origin/main` 无文件被删；
  无残留冲突标记；`tests/fixtures` 索引字节与工作区逐字节一致（68 项），21 个 PNG 的 LFS
  指针哈希与文件一致；UI 测试源集合 9 model + 10 adapter 与路由脚本一致；CMake 引用的
  测试源全部存在。

仍待云端（本机不执行）：

- 用合并后的真实枚举替换 `1409`/`1387` 与自检构造算术。**在此之前该门禁会失败，这是预期
  行为**，不是回归。
- 在 `1f5da53`（或推送后的等价提交）上跑 Debug/Release 完整软件回归、`format-check`、
  `lint`，并分列执行/跳过/禁用/失败。
- 推送属于外部动作，需用户确认后才会触发上述云端作业。

#### 1A 首轮云端结果（PR #53，提交 `44a17fb`）

| 作业 | 结果 | 说明 |
|---|---|---|
| Linux Clang core | 通过 | 1m30s |
| Windows core-only | 通过 | 2m23s |
| Windows UI model-only | 通过 | 4m15s |
| Repository guide and script contracts | 失败 | `add_dependencies command is not scriptable`，见下 |
| Release tests | 失败 | 构建完成，在路由门禁处停止；**测试未执行** |
| Debug tests | 未完成 | 本轮结束时仍在运行 |
| Native format and lint | 未完成 | 同上 |

两个真实缺陷（均已修复，待下一轮云端验证）：

1. **脚本模式探针被新目标打断**。`check-ui-test-routing.py --self-test` 用 `cmake -P`
   求值真实的 `tests/component/ui/CMakeLists.txt`；新目标里的 `add_dependencies` 在脚本模式
   不可用，探针在任何源集合比较之前就退出。`add_dependencies` 不影响目标编译哪些源，
   已加入 `tests/fixtures/ci-ui-routing/trace.cmake` 的桩命令表。本地重跑：
   `5 cases, 9 model and 10 adapter sources`。
2. **清单计数已漂移**。路由门禁报告 `total=1465 software=1443`，比 `4f2e0dd` 的
   1409/1387 多 56：55 个新 gtest 用例 + 1 个 `quality.qmltypes_contract`。已回填常量，
   自检仍用独立字面量（1443）构造合成清单，一侧漏改仍会失败。

注意：首轮的 Release 作业在构建后即被路由门禁拦下，**测试没有执行**；Debug 与
Native format/lint 在本轮没有结果。三者都不能计入通过。

#### 1A 第二轮云端结果（提交 `1c1588b`）

| 作业 | 结果 | 说明 |
|---|---|---|
| Repository guide and script contracts | 通过 | 48s；路由计数 1465/1443 生效 |
| Native format and lint | 通过 | 31m55s；新增 104 个文件通过 format-check、clang-tidy、qmllint |
| Linux Clang core | 通过 | 1m31s |
| Windows core-only | 通过 | 2m58s |
| Windows UI model-only | 通过 | 3m26s |
| Release tests | 失败 | 15m48s；**1439 项执行，1 项失败**（11 项 Skipped、8 项 Disabled） |
| Debug tests | 未执行 | 本轮结束时仍排队 |

唯一失败：`ui.MainQmlContractTests.ImageWorkspaceReloadsViewportSourceAfterImageOpen`。
它断言 `image://vcs-review/2/0/0/0/0`（五段），而 COLOR4 把「颜色转换开关」加进了
provider URL 成为第六段，实际值是 `image://vcs-review/2/0/0/0/0/1`。

这条测试**从未在 COLOR4 之后跑过**：该轮只做了定向验证（173/173），没有完整回归，所以
URL 契约变化没有机会被检出。这正是"旧成绩不能转移到新提交"的实例——不是新写坏的代码，
而是既有断言与新的缓存身份契约脱节。

修正（`a9bd352`）：四处字面量改为六段；把"URL 已改变"的断言从硬编码字面量改为与**实际
呈现过的 URL** 比较（否则契约再增一段时该断言会静默失去意义）；顺带修正
`ReviewImageProvider.h` 里过时的两段式注释。provider 只解析 slot 段，其余是缓存身份，
因此没有产品行为变化。

### 批次 1B：交接可信度收口（可与批次 2A 并行；FLOW3 硬前置）

**范围**：把"未保存编辑"从单点拦截改为全路径一致规则，并把 `imageEdit` 提升为 required
类型化属性（fail-closed）。不新增图片导航。

**依赖**：批次 1A。

**已有实现**：`closeSideBlockedReason` 的文案与拒绝逻辑、`ImageEditController::dirty()`、
类型化接线试点（`MainWindowDependencies`）可直接复用。

**具体缺口**：G1B-1、G1B-2（G1B-3 只做文案归属，不改行为）。

**验收标准**

- 关闭单侧、替换单侧、打开新图、文件夹换行、问题恢复五条路径在编辑副本有未保存修改时
  一致拒绝并给出同一句原因；编辑副本干净时正常继续。
- 依赖缺失时窗口创建失败（fail-closed），不再静默降级为"允许离开"。
- 变异证据：去掉任一守卫、或把 required 属性改回 `var`，对应主窗用例必须失败。
- 截图标注句只从单一颜色状态源生成；若将来图片复用该入口，不得复用"未色彩管理"这句。

**可独立交付条件**：满足，且是 FLOW3 的前置。

### 批次 2A：格式收口决定 + 16-bit 精度标签（不依赖 zlib，可先做）

**范围**：先把"已有格式到底支持什么"写成可核验的支持表，并修掉 16-bit PNG 的精度标签
错误；同时给出 zlib 决策 ADR。不做 AV1/VP9。

**依赖**：批次 1A；真实编码夹具（已有 12 格式 + 27 颜色样本）。

**已有实现**：格式夹具与 9 个解码用例、`verify-fixtures.py` 652 项、扩展名入口、
`ImageHeaderProbe`（可读 PNG IHDR 的位深字节，当前只读尺寸）。

**具体缺口**：G2-2、G2-3、G2-5。

**决策矩阵（需在 ADR 中定稿，不在本轮选）**

| 方案 | 做法 | 收益 | 代价与风险 |
|---|---|---|---|
| A 启用 zlib | `vcpkg.json` 的 ffmpeg 增加 `zlib` 特性并重建 | 原生 PNG 解码 + Deflate TIFF；16-bit PNG 走 FFmpeg 的 RGBA64 sidecar，与现有"转换后 RGBA16"口径一致；单一解码链 | 包体变化（当前 ZIP 272 文件基线），必须逐文件增量说明、干净解压重跑、许可核对；重建耗时长，须在独立环境做 |
| B 保持 Qt 回退、读 16-bit | 不再强制 RGBA8888，按 RGBA64 读取并自建 sidecar，同时记录"解码链 = qt"来源 | 不改依赖与包体；立刻修正精度标签 | 出现第二条解码链，`sourceFormat`/`displayConverted`/16-bit 统计口径都要能区分 swscale 与 qt；Qt 插件在打包环境是否支持 16-bit PNG 需实测 |
| C 明确拒绝 | Deflate TIFF 与 16-bit PNG 在支持表中写"不支持"，UI 给出明确原因 | 成本最低、不引入不确定性 | PNG 是截图/无损比对最常用格式，拒绝 16-bit 会削弱质量比较的可信度；只建议作为 A/B 都不成立时的兜底 |

无论选哪条，**精度标签必须与实际解码链一致**：16-bit 文件要么标 16-bit（并有转换后
RGBA16 统计），要么明确标"已回退为 8-bit 显示值"，不允许继续显示 8-bit 而不说明。

**验收标准**

- 支持表按"可打开 / 可显示 / 可精确比较 / 可导出"分列，容器与编码分列；GIF/WebP/TIFF
  明确写"只读一帧"。
- 每个受支持格式在普通打开、拖入、替换、文件夹发现四个入口行为一致；未支持入口给出原因。
- 16-bit PNG 的精度标签与解码链一致，并有反例（把标签写回 8-bit 时用例必须失败）。
- 未压缩 TIFF 夹具补齐；损坏/超限 PNM 有明确拒绝用例；干净解压 ZIP 上重跑代表样本，
  证明实际包内解码器可用（不借开发机 PATH）。
- `verify-fixtures.py` 要么注册为 `quality.fixture-integrity`（并在托管环境固定
  Pillow 与 ffmpeg 来源），要么在支持表里明确写"样本完整性未入门禁"。不得含糊。

**可独立交付条件**：满足。它只改文档、标签与夹具，不改解码依赖。

### 批次 2B：按 ADR 实施 zlib 或 Qt 16-bit 路线

**范围**：只做 ADR 选定的那一条；另一条写成明确拒绝。

**依赖**：批次 2A 的 ADR 与支持表；独立构建环境（重建 FFmpeg）。

**验收标准**：新增/变更依赖有许可、运行时清单、相对前一 ZIP 的逐文件增量说明；
PNG/Deflate TIFF 真实文件在干净解压包中解码；Qt 回退用例保留为回退路径覆盖；
现有颜色状态（PNG iCCP 容器探测）不回归。

### 批次 2C：AV1 或 VP9（先一个格式，逐个交付）

**范围**：只交付一个编码组合（建议先 AV1 8/10-bit SDR，解码用单一解码器），
另一格式保持明确拒绝。

**依赖**：批次 2A 支持矩阵与颜色/精度标签；真实 8/10-bit 样本；硬件路径另验。

**已有实现**：两个真实夹具（`av1_64x48_30fps_12.mkv`、`vp9_64x48_30fps_12.webm`）目前
只证明拒绝；`MediaProbe` 的准入表、P010 10-bit 通路、`MultiSourceFrameProvider` 可复用。

**验收标准**

- 入口、探测、解码、双源比较、精度标签、部署与说明一致；`.webm` 若支持 VP9 必须同时
  进入扩展名入口（当前只有 `.mkv`）。
- 软件解码是独立可验证路径；硬件（D3D11VA）另验，不以"有解码器"证明可稳定播放。
- 保持完整 FrameSet、异步身份与 256 MiB 解码帧缓存限制；关键行为有针对性变异证据。
- HDR（PQ/HLG）仍拒绝，不得因新增解码器绕过准入被当作 SDR 显示。
- 性能门禁仅在手动触发的专用 GPU 环境执行；未执行就写"未执行"。

### 批次 3：COLOR5 Windows 输出与物理显示（独立列出，独立交付）

**范围**：Windows SDR 输出契约、跨屏与配置切换、系统 HDR/Advanced Color 检测与降级、
物理显示验证。**不实现 HDR 内容或色调映射。**

**依赖**：COLOR4 收口（转换缓存、工作集计入、开关持久化）+ 有真实显示设备的独立环境。

**已有实现**：COLOR1–COLOR4 的数值路径与状态句；`displayColorEnvironmentVerified()`
已正确返回 `false`，不需要"先假装通过"。

**具体缺口**：G3-1 至 G3-5。

**验收标准**

- 支持表写清经过验证的 Windows 版本 / 显示配置 / 输出路径；取不到的设备只限制对应声明，
  不用另一台设备的截图代替。
- 跨显示器移动、运行中切换配置、睡眠唤醒、系统显示模式变化后重新计算或显式降级，
  不得继续展示旧环境的"已验证"状态。
- 数值参考、渲染输出、物理显示三层证据分开留证；无仪器时不得声称显示器已校准或达到
  某个物理色差指标。
- 显示环境变化不改写分析缓冲、统计、当前比较对、视口位置或正在恢复的素材身份。

**可独立交付条件**：满足，但**必须**在具备真实显示设备的独立环境完成；本机
（含虚拟显示适配器）不产生任何物理显示结论。

### 批次 4：FLOW3 单目录图片连续查看

**范围**：打开单图后列出父目录、手动上一张/下一张、自然排序、边界禁用、坏文件保留当前图。
首批只做当前目录，不做递归扫描、媒体库、自动轮播。

**依赖（全部为硬前置）**：
1. 批次 1A 的基线；
2. 来源身份与事务提交规则稳定（`evaluateIssueRestore` 已有，恢复路径已用）；
3. 批次 1B 的未保存编辑规则（`imageEdit` 类型化 + 全路径守卫）。

**已有实现**：`VideoFolderModel` 的自然排序、无环绕 `step()`、图片分类与
`stageImageOpen`/`finishPendingImageOpen`；`ImageFolderPairModel` 的双目录配对不受影响。

**具体缺口**：G4-1 至 G4-4。

**验收标准**

- 从文件选择、拖入、Explorer 打开单图后当前项高亮；自然排序覆盖数字、大小写、中文名称。
- 首尾行为固定为禁用并给出边界；坏文件保留当前图并提示，允许继续手动导航。
- 快速切换、目录刷新、删除、重命名、载入取消，不把旧解码提交给新项。
- 未保存编辑有一致的离开处理；进入单图导航不替换另一个双目录配对任务的某一侧；
  任务切换时先明确离开哪种工作流。
- 窄窗、快捷键焦点与帮助一致；首图与完整数据就绪分开测，不以模型测试代替真实主窗操作。
- 双目录配对与编辑行为无回归。

**可独立交付条件**：满足，但不得早于 1A/1B；不得以"模型已支持"作为交付证据。

## 5. 三项语义一致性专项

### 5.1 显示转换不改变分析数据

| 检查项 | 现有证据 | 结论 |
|---|---|---|
| 分析缓冲只由素材提交写入 | `displayColorImage()` 只返回新图，`primary_`/`secondary_` 不被回写 | 成立 |
| 开关切换后统计不变 | `ColorDisplayConversionChangesOnlyThePresentedImage`、`ColorDisplayToggleDoesNotMoveDifferenceStatistics` | 成立（定向） |
| 探针读解码值而非显示值 | `samplePixel` 读 `primary_`/`secondary_` | 成立 |
| 转换缓存与工作集 | 无控制器侧缓存；`estimatePairWorkingSet` 不计转换副本 | **缺口** |
| 迟到结果身份 | URL 含 `contentGeneration_` 与颜色开关；`StaleLoadResultCannotCommitUnderANewerPendingRequest` | 成立，但转换本身是同步的，取消语义只覆盖载入 |

结论：**数值口径成立，性能与内存口径未验证**。COLOR4 验收条件中的"后台转换不阻塞
GUI/渲染线程、缓存按来源与颜色状态失效、有工作集上限与取消验证"目前只有失效语义的一半。

### 5.2 恢复不串素材

| 检查项 | 现有证据 | 结论 |
|---|---|---|
| 身份核验 | `evaluateIssueRestore` 比对 size/mtime/fingerprint，产出 `Ready/RelocationRequired/Modified/IncompleteIdentity` | 成立 |
| 分阶段恢复 | `Main.qml` 等配对提交 → 回录制行 → 一次性重放；超时与切换明确取消 | 成立 |
| 文件夹非首行 | `openPairAt(rowIndex)`，主窗用例覆盖 | 成立 |
| 部分恢复提示 | 缺字段进 `missing[]` 并提示"部分恢复" | 成立 |
| 恢复前的脏检查 | 无 | **缺口**（G1B-1） |
| 跨 DPI / 实窗 | 未验收 | 待验 |

结论：**素材身份不串，但会静默覆盖未保存的编辑副本**。这条在"恢复不会串素材"之外，
属于同一类可信度问题，必须在 FLOW3 之前收口。

### 5.3 截图、PNG 样本、流拷贝片段的颜色/来源/边界语义

| 输出 | 数据来源 | 颜色语义 | 来源标注 | 边界语义 | 状态 |
|---|---|---|---|---|---|
| 对比截图（复制/保存） | 窗口合成像素 | "显示结果 · 屏幕像素 · 未色彩管理；非源文件原始码值" | 源名 + 模式 + 帧号 | 当前视口帧 | 有标注，**无输出侧颜色声明**；该句只对视频路径成立 |
| PNG 精确样本 | 解码像素（`decoded_rgba8_swscale`） | manifest 写 `source_profile_applied=false`、`display_environment_verified=false` | `member_identity`（每成员 source + frame_index）、`sources`、`candidate_output_frame` | 计划帧与裁剪，写后读回校验 | 实现完整，缺真实素材/解压包人工复核 |
| 流拷贝片段 | 原始码流 | 不做颜色转换 | 对话框写"时间线主源"+文件名 | 关键帧前滚、实际尾帧、EOF/出点停止、请求帧数 | 实现完整，缺真实素材人工复核 |

三者语义**互不重叠**，这一点已经做到：截图是显示结果、样本是解码像素、片段是原始码流。
剩余风险有两个：

1. 截图只有文本声明，接收方无法从文件本身判断颜色；若跨软件使用，需要明确的输出编码/
   颜色声明契约（例如声明 sRGB 或明确"未标记"）。
2. "未色彩管理"这句在图片路径上不成立（COLOR4 会对显示派生结果做配置转换）。目前该句
   只用于视频对比窗口，属于正确；一旦图片复用同一入口，必须改为读取单一颜色状态源，
   不能复制这句文案。

## 6. 建议实施顺序

| 顺序 | 批次 | 为什么在这个位置 | 交付后可独立验证 |
|---|---|---|---|
| 1 | 1A 整合 | 所有后续批次都需要一个"能对上 main 门禁"的基线；现在任何测试数字都无法归属 | 是 |
| 2 | 1B 交接收口 | 小、可并行、是 FLOW3 硬前置；同时修掉"恢复覆盖编辑副本"的静默丢失 | 是 |
| 3 | 2A 格式收口 + 精度标签 | 不引入依赖、不改包体，先让支持表与标签可信 | 是 |
| 4 | 2B zlib / 16-bit 路线 | 依赖 2A 的 ADR；需要独立环境重建 | 是 |
| 5 | 2C 单个 AV1 或 VP9 | 依赖 2A 矩阵；性能门禁手动 | 是 |
| 6 | 3 COLOR5 | 依赖 COLOR4 收口与真实显示环境；本机不可做 | 是（在合格环境） |
| 7 | 4 FLOW3 | 依赖 1A/1B 与来源身份规则稳定 | 是 |

2A 与 1B 可以并行；2B/2C 与 3 可以交错，但 3 不得借用 2 的成绩。

## 7. 下一批最小可交付范围（MDS）

**批次 1A，且只做 1A**：合并到 `66475bb`、重绑定计数型门禁、重写基线文档、
在托管环境跑一次完整软件门禁。**不含** 1B、不含任何格式或颜色新能力、不含夹具校验入门禁。

选择理由：

- 它是唯一能让"当前集成候选"这个说法成立的交付物；在此之前，1433、1409、173/173、
  652/652 这些数字分别属于不同 SHA 或不同环境，不能拼成一份通过记录。
- 它不改变产品行为，风险集中在合并与门禁计数，可由托管 CI 完整验证。
- 它把 10 个缺失的 CI 文件补齐，后续批次才有"云端验证"这个前提。

MDS 的交付物清单：

1. 一个合并提交（含 4 个冲突文件的并存内容、10 个 main 新增文件、29 项工作区成果）。
2. 更新后的 `check-ci-test-routing.py`（真实枚举计数 + 同步自检算术）。
3. `.gitignore` 增加本地产物。
4. 四份文档的基线重绑定（环境分离、可复现基线、媒体支持、颜色契约）+ 台账顺序修正。
5. 一份云端门禁记录：提交 SHA、diff 摘要、各作业实际执行/跳过/禁用/失败数量、
   runner 身份、未执行项清单。

**明确不在 MDS 内**：`imageEdit` 类型化与脏检查（1B）、zlib 决策实施（2B）、
AV1/VP9（2C）、COLOR5、FLOW3、`verify-fixtures.py` 入门禁（可延后到 2A 一并定）。

## 8. 执行位置与门禁纪律

- 本机（日常生活与工作设备）**不执行**产品配置、构建、回归、format/lint、GUI。
  本轮已遵守：只做读取与 `gh api` 查询，未 fetch、未修改工作树。
- 后续所有软件验证在云端 `windows-2022` 托管 runner 执行；GPU、D3D11VA、性能、
  物理显示仅由用户手动触发。
- 不降低任何门禁：1409/1387 之类的固定计数要随真实清单更新，而不是放宽检查或删除反例；
  跳过的用例、禁用的用例、未执行的作业分别记录，不得合并成"通过"。
- 结果必须绑定提交、diff、素材、二进制与 ZIP 哈希；`2.5.0` 跳过本地物理门禁的授权
  不延续到任何后续发布。

## 9. 未决决策与风险

| 项 | 现状 | 需要在哪一步定 |
|---|---|---|
| zlib（PNG/Deflate TIFF） | 三方案并列，未选 | 2A 的 ADR |
| 16-bit PNG 精度路径 | Qt 回退 RGBA8 且标签错误 | 2A（标签）→ 2B（路线） |
| 第一个 AV1/VP9 格式 | 两个夹具都只有拒绝证据 | 2A 的支持表 |
| 截图输出颜色声明 | 只有文本标注 | 1B 或 2A（取决于是否与图片路径合并入口） |
| `verify-fixtures.py` 是否入门禁 | 手工脚本，依赖 PATH ffmpeg | 2A |
| `imageEdit`/`folderPairModel` 类型化 | 仍是 `var` + `typeof` 守卫 | 1B（imageEdit）/ FLOW3（folderPairModel） |
| 合并后真实清单数量 | 1433 与 1409 互相矛盾 | 1A 重新枚举 |

风险提示：`check-ci-test-routing.py` 的自检是"用固定数字构造清单再校验"，如果只改
`validate()` 里的常量而不改 `--self-test` 的构造算术，自检会变成同义反复——它会通过，
但不再证明任何东西。这一处必须同时改，并保留 4 个反例。

## 10. PR 组织：使用 gh stack（2026-10-18 增补）

多批次交付改为栈式 PR，不再每次从 `main` 切一条独立分支。工具是官方扩展
`github/gh-stack`（本机已安装 `gh stack version 0.2.1`）。

### 10.1 栈层映射

| 层 | 分支（建议） | 批次 | 目标 |
|---|---|---|---|
| 1 | `feat/integrate-hosted-ci` | 1A 整合 | PR #53 → `main` |
| 2 | `feat/image-edit-dirty-guard` | 1B 交接收口 | → 层 1 |
| 3 | `feat/media-format-closeout` | 2A 格式收口 + 精度标签 | → 层 2 |
| 4 | `feat/media-dependency-route` | 2B zlib 或 Qt 16-bit | → 层 3 |
| 5 | `feat/media-codec-av1` | 2C 单个新编码 | → 层 4 |
| 6 | `feat/colour-windows-output` | 3 COLOR5 | → 层 5（需独立显示环境） |
| 7 | `feat/image-folder-browse` | 4 FLOW3 | → 层 6 |

栈允许非相邻依赖：COLOR5 与 FLOW3 之间没有代码依赖，但都排在计数与标签收口之后，
所以按栈顺序排列不会造成互相等待。

### 10.2 规则

- 栈至少两层才有意义：`gh stack link` 要求两个以上参数，单分支不构成栈。层 1 已用
  `gh stack init feat/integrate-hosted-ci` 纳入本地跟踪，待层 2 出现后一起 `submit`。
- 每层必须独立可评审、独立可回退；上一层的未验证结果不能当作下一层的通过证据。
- 栈底未绿时不合并，但可以在栈上继续开发与评审。计数修正（`EXPECTED_TOTAL`/
  `EXPECTED_SOFTWARE`）必须落在栈底层，避免下游层替底层背门禁。
- 底层修正后，下游层用 `gh stack sync` / `gh stack rebase` 跟随，不要手工 merge 造成
  分叉。
- 合并用 `gh stack merge`，从栈底开始；不单独 `gh pr merge` 中间层，否则栈结构断裂。

### 10.3 常用命令

```bash
gh stack view                     # 看当前栈与每层的 PR
gh stack add feat/<branch>        # 在栈顶加一层（-Am "msg" 可直接提交暂存改动）
gh stack submit                   # 推所有层并创建/更新 PR，最后把 PR 链成 GitHub Stack
gh stack sync                     # 与远端同步（底层合并或修正后）
gh stack checkout <PR 号或分支>   # 切到某一层
gh stack merge                    # 从栈底合并整个栈
```
