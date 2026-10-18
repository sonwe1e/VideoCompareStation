# 可复现基线记录（阶段 0 / BASE）

更新日期：2026-10-18。状态：进行中。软件身份与执行环境已测量；可见窗口、物理显示和
GPU 门禁按计划留在独立环境，尚未在主力桌面执行。本页只记录基线与未决项，不把任何一项
写成“已验收”；问题状态仍以 [问题台账](visual-review-backlog.md) 为准。

计划： [收尾后分阶段改进计划](../plans/CompareStation_收尾后分阶段改进计划_2026-10-17.md)。

## 1. 本次基线身份

| 项目 | 值 |
|---|---|
| 计划基线提交 | `8c90005e0f6ba5b71ceb284682259df8d29a307a`（`feat: release CompareStation 2.5.0 review workflow updates`） |
| 计划日工作区 | 17 个已修改文件、24 个未跟踪文件（QML 类型注册、类型化接线、界面修改；详见计划文档与 `docs/engineering/qml-interface-pilot.md`） |
| dev 构建 | Ninja，Debug，MSVC 14.44.35207，vcpkg triplet `x64-windows`，Qt 6.11.1，FFmpeg 8.1.2 |
| 本地 2.5.0 ZIP | `out/package/zip/CompareStation-2.5.0-windows-x64.zip`，28,327,485 bytes，SHA256 `28176C26632573ACE8338D7B817D5780625791EB93E9A0ABCC6B0B8920BEBDE4` — **该路径已于 2026-10-18 被一次本地 `cpack` 覆盖，见 [台账](visual-review-backlog.md) 同日条目；重新取得该包前不要把本行当作磁盘现状** |
| 构建日期/工具链证据 | `docs/engineering/qml-interface-pilot.md` §5；release 全量 1397 项启用 100% 通过 |

### 2026-10-18 整合后的身份

| 项目 | 值 |
|---|---|
| 上游合并提交 | `66475bb2e4f6333f3c6a57fb68933261f00e3686`（PR #52，父提交 `8c90005`） |
| 本地整合结果 | 分支 `main`，`1f5da53`（3 个成果提交 + 1 个合并提交），工作树干净 |
| 入库内容 | 104 个代码/测试/夹具文件（+11,754 / −490）、11 个文档文件、`.gitignore` 与 `.gitattributes` |
| 云端结果 | **无**。合并后提交未运行任何构建、测试或 lint；下表 2.5.0 数字只属于 `8c90005` 的旧候选 |

整合前的 2.5.0 数字（1397 项、ZIP 哈希）不能作为合并后的通过证据：用例数、依赖
（`vcpkg.json` 已被 `66475bb` 加入 `qtbase/thread`）和构建配置都已变化。

公开 2.5.0 ZIP 与本地候选虽然版本号相同，哈希不同。任何结果必须绑定实际构建、提交、
工作区 diff 与二进制/ZIP 哈希，不能只写版本号；2.5.0 曾跳过本地物理门禁的授权不自动
延续到后续发布。

### 2026-10-19 2.5.1 发布身份

| 项目 | 值 |
|---|---|
| 发布标签 | `v2.5.1`（提交即 tag 指向的 release commit） |
| 版本变更 | `CMakeLists.txt` 与 `vcpkg.json` `2.5.0→2.5.1`；`.github/workflows/release.yml` 免签名清单加入 `v2.5.1` |
| 构建 | Ninja Release，MSVC 14.44.35207，Qt 6.11.1，FFmpeg 8.1.2，Windows SDK 10.0.26100.0 |
| 测试 | Release 预设 1449 项：1446 通过；3 项并发超时／偶发失败，串行重跑全过；0 断言失败 |
| 包 | `CompareStation-2.5.1-windows-x64.zip`，272 文件，28,355,788 bytes，SHA256 `46570CA1516EE1FA30B360F19F1553C4A6064ABC6776E0360960F26416AC4C6B` |
| 未验证 | 真实 D3D11VA／性能门禁、物理显示与干净 Windows；按[测试环境分离](test-environment-separation.md)未在主力桌面启动 |

本机 Release 依赖树为 2026-10-17 以相同版本重建的本地 vcpkg 安装；包内 Qt／FFmpeg 版本
与 2.5.0 相同但字节不同，不能把 2.5.0 的单文件哈希当作本版证据。原始日志与包哈希保存在
`out/verification/release-v2.5.1/`。

## 2. 执行环境（本机只读测量）

| 项目 | 实测值 |
|---|---|
| 操作系统 | Windows 11 家庭版 10.0.26300 |
| CPU | 13th Gen Intel Core i7-13700KF |
| GPU | NVIDIA GeForce RTX 4090，驱动 32.0.16.1074 |
| 虚拟显示适配器 | OrayIddDriver Device（1024×768@60）、GameViewer Virtual Display Adapter（2848×1320@144） |
| 文本缩放 | 96 DPI（100%） |
| 物理显示器身份/刷新率 | 未记录（虚拟适配器与物理屏并存，不能用适配器值代替物理屏验收） |
| 系统 HDR / Advanced Color | 未记录 |
| 显示器 ICC 配置文件 | 未记录；系统 color 目录只有 `sRGB Color Space Profile.icm` 等少数配置 |

由于这是日常生活与工作设备，完整回归、可见窗口、GPU 和物理显示验收不得在此自动启动；
见 [测试环境分离](test-environment-separation.md)。上表的虚拟显示适配器也说明：本机截图
不能作为物理显示颜色或跨屏验收证据。

## 3. 样本登记

自动化夹具必须记录哈希、来源/生成方法、容器/编码或图片格式、位深、Alpha、颜色声明与
预期用途。当前清单：

| 清单 | 内容 |
|---|---|
| `tests/fixtures/manifest.json` | 视频真实编码夹具：H.264、HEVC、MPEG-4 Part 2，含 VFR、B 帧、10-bit、非零起始 PTS、损坏输入等；每项含 SHA256、codec、像素格式、帧 PTS 与帧哈希 |
| `tests/fixtures/images/color/manifest.json` | COLOR2 颜色样本：sRGB / Display P3 / Adobe RGB / 无配置 / 损坏、不支持配置 / 透明边缘 / 灰阶 / 16-bit 与低位差异 / 同码异色与异码同色对 / PNM 家族；含独立参考值与容差（生成中，见下文） |
| `tests/fixtures/images/formats/manifest.json` | MEDIA1 真实编码图片格式与 AV1/VP9 调查样本（生成中） |

真实大素材和一次性证据只放 `out/verification/`，不复制测试套件。生成脚本随样本提交，
避免二进制不可追溯。

## 4. 四条可重复任务（当前状态）

| 任务 | 步骤 | 当前状态 |
|---|---|---|
| 日常打开 | 启动 CLI 启动检查；GUI 打开单视频/单图；记录首次预览与完整数据就绪时间 | CLI 启动检查可执行；GUI 计时待独立交互桌面 |
| A/B 局部比较 | 打开一对图片，进入并排/分割线，缩放平移同一区域，保存问题记录 | 自动行为测试已就绪（`ui.ImageReviewControllerTests`、`ui.ImageWorkspaceManual`）；实窗与计时待环境 |
| 记录后重开 | 保存问题记录→关闭→重开→恢复同一素材和观察状态 | FLOW1 恢复实现与端到端测试见台账；Windows 实窗验收待做 |
| 导出后独立重读 | 区间流拷贝、精确 PNG 样本、截图分别导出并用独立工具重读 | 自动测试覆盖规划/写入/事务；真实素材与 Windows 流程待验收 |

频率来自真实任务清单；尚未取得频率时不声称某格式是用户最常用。当前失败、未测和环境
跳过必须分开记录，不能把零项执行当通过。

## 5. 退出条件与未决项

- [x] 构建身份（提交、工作区、工具链、本地 ZIP 哈希）已固定。
- [x] 视频夹具清单与来源规则已具备；图片颜色/格式清单在 COLOR2/MEDIA1 交付时补齐。
- [ ] 同一构建在独立环境重现“打开、比较、记录、导出与恢复”四条任务。
- [ ] 物理显示器、刷新率、HDR/Advanced Color 与 ICC 环境登记。
- [ ] workflow 接单机器身份与“普通提交不在主力桌面启动窗口”的正反例检查（见测试环境分离）。

完成前，任何性能或物理显示结论都只能写成待验；性能正式采样仍按
[runner 与发布门禁](self-hosted-runner.md) 的硬件协议执行。
