# 时间轴标记图例与实际颜色一致

日期：2026-10-08。基线：`ba594923aac26891bb998e785cf515d02ca2e57b`。

## 问题与边界

播放检查器原来用文字说明“橙色重复、紫色多余、青色锚点、黄色低置信度”，
但时间轴已通过 `VcsTheme.timelineMarkerColor(kind)` 使用另一套语义颜色。
图例与真正标记不一致，会使用户误读分析结果。

`ReviewController` 实际会发出 `missing`、`duplicate`、`extra`、`anchor`、
`low-confidence`、`review-segment` 和 `rejected-segment`。
后三种共享 `markerOther` 的中性色；低置信度不是独立的黄色分类。

本次只改 `TabbedInspector.qml` 的图例：

- 缺失、重复、多余、锚点、低置信度使用文字标签和实际色块。
- 色块直接调用时间轴已使用的 `Theme.timelineMarkerColor`，不再复制颜色名称或色值。
- 简短说明低置信度、待复核和已拒绝区间共用中性色，具体类型与置信度仍从时间轴悬停提示读取。
- 使用 `Flow` 承载图例，保留其后的标记溢出提示。

不修改标记生产、分类、颜色、播放、对齐、时间轴、源角色或文件侧栏行为。

## 正式回归入口

在现有 `tests/component/ui/MainQmlContractTests.cpp` 新增
`TimelineMarkerLegendUsesThemeColorsAndWraps`，不新增测试注册或平行测试套件。
该用例实例化生产检查器并切到播放页，核对：

- 五项标签与 `Theme.timelineMarkerColor` 的运行时结果一致。
- 低置信度、待复核、已拒绝三类仍使用中性色。
- 标签和色块可见且有尺寸；图例、换行说明、溢出提示不越界或重叠。
- 检查器宽度依次为 300、380、300 个逻辑像素，包含缩窄返回。

后续在正常 Windows 构建环境中可执行：

```powershell
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex '^ui[.]MainQmlContractTests[.]TimelineMarkerLegendUsesThemeColorsAndWraps$'
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex '^ui[.](vcs_theme|timeline_tracks)$'
pwsh tools/build/build.ps1 -Preset dev -Target format-check
pwsh tools/build/build.ps1 -Preset dev -Target lint
```

## 本次云端证据

复用已有 Linux Qt 6.8.2 与 GoogleTest，不启动完整应用，不运行 GitHub Actions。
另在隔离目录准备官方 PyPI 的 clang-format 19.1.5，仅用于本次 C++ 文件格式检查；
没有修改全局配置。
从正式测试文件提取上述一个完整用例，在生产检查器及其六个原样 QML 资源上运行，
使用可预测的控制器数据；这不是 Windows/D3D11 测试。

- 基线运行失败；修改后的精确测试用例 **1/1 通过**。
- 既有主题测试 **6/6**、时间轴测试 **5/5** 通过；这些 QtTest 总数包含各自初始化与清理。
- **32/32** 生产行为变异被断言检出，覆盖颜色、标签、可见性、尺寸、换行与布局。
- **7/7** 缺失对象观测变异被检出；另外 **10/10** 构造/方法调用 guard 故障注入被检出，
  新用例共 **39/39** 处断言都有失败证据；
  不把测试设施故障计入生产行为变异。
- 生成并逐张检查 **8 张**真实组件渲染截图：300/380 逻辑像素宽 ×
  `QT_SCALE_FACTOR=1/1.25/1.5/2`。五个色块与标签、说明和溢出提示均可见，无裁切或重叠。
  截图是 Basic 风格、offscreen/software 的检查器测试数据，文字颜色使用测试夹具值，
  不能当作完整主窗、原生 Windows 或真实显示器 DPI 迁移验收。
- 现有 Qt 6.8.2 `qmlformat` 的只读格式化结果与最终 QML 一致；
  对修改后的单个 QML 文件运行 `qmllint` 返回 0。它们不是仓库聚合格式/静态检查。
  clang-format 19.1.5 对修改后的单个 C++ 文件 `--dry-run --Werror` 返回 0；
  只修正新增测试的换行与缩进，旧代码区间字节不变。格式前后的完整 C++ token 流
  （包括字符串/原始字符串）一致，因此沿用格式前的行为与变异证据，不宣称重新执行。
  无新增尾随空白。

截图在本次审查交付中单独提供；没有把 PNG 绕过 Git LFS 写入仓库。
执行日志、输入哈希、精确测试体、变异与断言映射保存在本轮 `out/verification/`。
早期测试夹具观测和截图辅助程序编译失败记录保留；修正后重跑，未放宽警告或断言。

## 尚未验证

未运行固定 Qt 6.11.1/MSVC、原生 Windows 主窗、真实视频/D3D11、显示器 DPI 迁移、
完整 build/CTest、仓库聚合 format-check/lint、硬件性能门禁或发布包。
单文件 C++/QML 检查通过不代表仓库聚合门禁通过。
无需为这项图例修复扩展为播放器内核或 UI 架构重写。
