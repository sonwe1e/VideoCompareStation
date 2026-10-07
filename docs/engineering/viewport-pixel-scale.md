# 视频视口倍率与真实尺寸

本轮基线：`main a0a044066345ed0110f87caab56afb5147f615d5`（2026-10-07）。
这是观看体验的小修；Windows 原生渲染、DPI 与完整播放器验收尚未完成。

## 复现与行为

原徽标按「第一个面板宽度 ÷ 槽位 0 视频宽度」计算倍率，有三个问题：

- 竖屏在宽面板中的左右留白被算进了视频宽度。
- 参考聚焦、B/C 分割线的首个显示源可能不是槽位 0。
- 分割线面板是裁切遮罩，移动分割线会改变它的宽度，却不应改变画面倍率。

一个 800 × 600 的独立视口（边缘各 1 像素）显示 1080 × 1920 的竖屏视频时，
实际内容为 336.375 × 598；适应窗口倍率约 31.15%，旧徽标却报约 73.89%。
旧按钮根据这个错误倍率放大后会显示“100% 真实尺寸”，实际内容比例仍只有约 42.15%。
新按钮以实际内容尺寸计算，目标 `viewScale = 1920 / 598`，再点一次恢复适应窗口。

## 实现范围

- `ComparisonSurface::sourcePanelRects()` 复用既有
  `computeSurfacePresentationGeometry()`，保持原 panel x/y/width/height/slot，并增加
  `contentWidth` / `contentHeight`。分割线返回完整合成画面的内容范围。
- 设置、清除、恢复 ROI 时发出已有 `presentationGeometryChanged` 信号，保证裁切改变
  内容宽高后，徽标不会保留旧几何。
- 徽标跟随首个显示源，计算裁切后的原像素宽高、90°/270° 旋转和逻辑到物理像素的 DPR。
- 方形像素仍显示单一百分比；非方形像素分别显示横/纵百分比，不宣称两轴同时为 1:1。
- “100% 真实尺寸”的两轴容差由 2.5 个百分点收紧至 0.001 个百分点。
  现有 64× 上限仍由 surface 保持；达不到 1:1 时显示真实倍率。
- 不改播放、解码、D3D11 绘制、同步、分割线位置、缩放/平移算法或比较对象选择。
  纯差异/Fade 模式原本不显示这个源面板徽标，本轮不扩展入口。

## 正式回归

`tests/component/ui/ViewportPixelScaleTests.cpp` 注册到已有
`dvs_main_qml_contract_tests`，测试真实 `ComparisonViewport.qml` 与 `ComparisonSurface`。
不创建窗口、不挂载渲染服务、不开素材或播放协调器。

五个场景：

1. 竖屏留白、实际按钮信号、独立验证 surface 放大倍数及再次点击复位。
2. 非零参考槽位、B/C 分割线，以及 0 / 0.25 / 0.8 / 1 四个分割位置。
3. 旋转视频的 ROI 设置、清除和恢复，含响应式几何更新。
4. 非方形像素两轴读数，以及点击后不误标真实尺寸。
5. 无元数据/无效宽度、接近但未到 1:1 的标签，以及 64× 上限。

## 本轮云端证据

环境为 Linux、GCC 14、现有 Qt 6.8.2 / GoogleTest 1.17.0，无新增依赖：

- 五个正式新测试 **5/5 通过**。生产 `ComparisonSurface.cpp` 整文件编译；
  QML 整组件加载，实际 MouseArea 的 `clicked` 信号执行原处理器。
- 同一测试面对原 main 的生产代码 **5/5 失败**。旧 QML 只加两个中性 objectName，
  使同一测试能够找到旧标签与鼠标区，未改变计算或点击行为。
- 精确提取并复跑既有 geometry/property 的 **17/17** 项，加新测试共 **22/22**。
  这些用例经静态检查均不创建窗口、播放器、线程、文件工作器或设备服务。
- Windows-only renderer 在云端链接为“调用即终止”的保护桩；无任何调用。
  真实 renderer 的纯几何函数按唯一锚点原样提取编译，不替换几何算法。
  这不是 D3D11 渲染或 Windows ABI 验证。
- **23/23** 编译并成功加载后的实现变异被运行时断言检出。
  新测试的 **41/41 行为断言位置**有失败证据；另有 **7/7** 初始化/观测故障
  单独验证 setup 与信号调用 guard，不混入实现变异数量。
- 直接执行生产 QML 的 DPR 算式，1 / 1.25 / 1.5 / 1.75 / 2 倍 DPR × 三个缩放值，
  **15 组、45 项检查**通过；分别遗漏横/纵 DPR 的 **2/2** 变异被检出。
  这不验证显示器切换、Windows DPI 事件或真实物理像素。
- 生产 surface 与正式测试按仓库非 MSVC 的 warning-fatal 参数编译通过。
  已有 Qt 6.8.2 `qmlformat` 只读输出与最终 QML 一致。

验证副本、日志、输入哈希和逐断言变异映射保存在本轮 `out/verification/pixel-scale/`；
正式用例只有仓库测试文件一份。初次 Fontconfig 报不可写默认缓存目录；指定验证输出目录
作缓存后终态复跑通过，无 QML 加载错误。

## 仍需 Windows 验收

未执行固定 Qt 6.11.1 / MSVC、原生屏幕与 DPI 迁移、真实视频/D3D11、完整 Main、完整
build/CTest、仓库 format-check/lint、性能门禁或 ZIP。云端独立 `qmllint` 缺少运行时
注册的 Dvs.Ui 类型信息，不能视为仓库零警告 lint 通过。本轮没有实窗截图。

```powershell
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex '^ui[.]ViewportPixelScaleTests[.]'
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex '^ui[.]ComparisonSurface(Geometry|Property)Tests[.]'
pwsh tools/build/build.ps1 -Preset dev -Target format-check
pwsh tools/build/build.ps1 -Preset dev -Target lint
```

原生复核还应打开竖屏手机录屏，在 100%/125%/150%/200% 缩放的显示器上切换窗口，
测试单视频、参考聚焦、B/C 分割线、ROI 和两次点击。验证后再补真实素材截图和证据。
当前保持 Draft，不把云端通过当作 Windows 播放流畅度或交付验收。
