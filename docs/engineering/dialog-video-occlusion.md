# 源顺序确认窗横条露底：诊断与待验候选

日期：2026-10-08。状态：**本地候选，Windows / D3D11 未运行，不能宣称修复完成**。
用户确认截图来自 PR #45–#49 全部合入的本地 main。五个 PR 均未修改
`DropConfirmationDialog.qml` 或原生视频节点的深度状态，不能归因于用户未更新。

## 截图观察与已确认的代码、测试事实

- 截图的两条横带在位置与形状上，和标题栏底部、页脚顶部各 10 个逻辑像素的方形
  补角矩形吻合，疑似对应这两块区域；该对应关系仍是高可信推断，尚未原生复现。
  当前静态布局未发现 padding 或 inset 留出缺口。
- [2026-09-15 的旧修复](https://github.com/sonwe1e/VideoCompareStation/commit/1eb3925b42ce98df70f15b78ec415cc4805c274b)
  已将 Dialog 换为 Popup、连续背景与显式页脚。现状仍保留这些改动。
- 旧接缝测试只检查截屏 alpha 为 255。隐藏连续背景后，透出的不透明洋红底仍使旧测试通过。
  新测试同时检查接缝前后 24 个 RGB 点：同一 mutation 变红，原样 Popup 变绿。
  另分别把上下补角条变为洋红的两个颜色 mutation，均为旧测试绿、新测试红；
  这是断言有效性验证，不是原生视频覆写的复现。
- `ComparisonRenderNode` 声明 `DepthAwareRendering`；`D3d11ComparisonRenderer` 却使用
  `DepthEnable=FALSE` / `DepthFunc=ALWAYS`。该组合从至少 2026-07-28 已存在。
  应用与 WARP 测试宿主均启用 `setDepthBufferFor2D(true)`。

## 有源码依据、仍须原生确认的因果链

Qt 6.11.1 的批处理器先绘制不透明矩形并写深度，再绘制包含自定义视频节点的透明批次。
方形补角条可进入前一批，圆角抗锯齿表面进入后一批。视频节点若不读取深度，可能覆盖
已经绘制、实际应位于视频上方的方形 UI。这与截图中仅两条补角带露底的形状吻合。
软件后端不运行这段 D3D11 实现，纯 QML 洋红底通过不能否定此路径。

官方源码依据：

- [启用深度感知与批处理](https://github.com/qt/qtdeclarative/blob/v6.11.1/src/quick/scenegraph/coreapi/qsgbatchrenderer.cpp#L1264-L1265)
- [Z 顺序公式](https://github.com/qt/qtdeclarative/blob/v6.11.1/src/quick/scenegraph/coreapi/qsgbatchrenderer.cpp#L1964-L1968)
- [深度清为 1、Less 比较、透明批次关闭写入但保留测试](https://github.com/qt/qtdeclarative/blob/v6.11.1/src/quick/scenegraph/coreapi/qsgbatchrenderer.cpp#L3871-L3912)
- [传给 RenderNode 的投影深度](https://github.com/qt/qtdeclarative/blob/v6.11.1/src/quick/scenegraph/coreapi/qsgbatchrenderer.cpp#L4137-L4145)
- [Qt 官方自定义节点启用深度测试](https://github.com/qt/qtdeclarative/blob/v6.11.1/examples/quick/scenegraph/customrendernode/customrender.cpp#L151)
- [Qt 的 D3D11 深度状态映射](https://github.com/qt/qtbase/blob/v6.11.1/src/gui/rhi/qrhid3d11.cpp#L4696-L4698)
- [圆角 Rectangle 默认启用抗锯齿](https://github.com/qt/qtdeclarative/blob/v6.11.1/src/quick/items/qquickrectangle.cpp#L345-L352)
- [抗锯齿矩形使用透明混合材质，普通不透明矩形不需要](https://github.com/qt/qtdeclarative/blob/v6.11.1/src/quick/scenegraph/qsgdefaultinternalrectanglenode.cpp#L64-L111)

此路径不是 reversed-Z。候选只将原生视频状态改为 `DepthEnable=TRUE` / `LESS`，继续
`DepthWriteMask=ZERO`；普通与 stencil 状态均从该描述创建。保留 Qt 提供的投影矩阵、
原有 stencil/scissor 与 QML 布局，不通过关闭整窗深度批处理或改控件透明度绕开问题。

## 覆盖与明确未完成项

已执行：Linux Qt 6.8.2、offscreen 软件后端、100% 下完整 DropConfirmationDialog 套件
5/5（含 init/cleanup）；背景隐藏 mutation 证明旧测试假绿、新 RGB 断言变红。
100% 截图人工检查无横缝。测试文件经现有 qmlformat 不带 normalize 检查无差异，
补齐现有 QtTest 导入路径后定向 `qmllint --max-warnings 0` 通过；不等同仓库全量 lint。
两个 C++ 文件仅变更行范围通过 `clang-format 19.1.5 --dry-run --Werror`；新增测试的
格式调整已核对词法 token 不变，没有全文件重排，不替代 Windows 编译或仓库全量格式门禁。

新增但**未编译、未运行**的 Windows 回归：
`ComparisonSurfaceWarpTests.DropConfirmationChromeStaysOpaqueOverVideo`。
它用真正的 ComparisonSurface / D3D11 WARP 呈现白色视频，然后打开生产 Popup；
2→3→2 源及关闭重开，检查两条边界前后 72 个 RGB 样本，并检查关闭后恢复视频。
显式 CTest 入口设置 `QT_SCALE_FACTOR=1/1.25/1.5/2`；它还会乘以系统缩放，只有系统为
100% 时才对应 100/125/150/200%。测试记录实际 DPR 与截图尺寸并检查坐标比例，拒绝
非空 `QSG_NO_DEPTH_BUFFER`，避免禁用深度批处理后出现无意义的绿例。
单渲染通道 fixture 还在 `afterRenderPassRecording` 通过只读 `OMGetRenderTargets` 记录
真实 DSV 是否绑定；渲染线程只写原子计数，GUI 线程断言。连接以局部 context 自动断开，
回调持有共享状态，避免测试中途失败后悬空。该检查也尚未在 Windows 运行。

尚需在授权的 Windows 显示会话执行：

1. 编译、format-check、lint；本地候选尚不能作为已通过 MSVC 的代码。
2. 先在旧深度状态下运行新增原生测试，应在补角条 RGB 断言失败；再启用候选应通过。
   若旧代码也绿，则继续诊断，不能把该测试当作用户回归的有效覆盖。
3. 执行全部 `ComparisonSurfaceWarpTests`，特别是裸视频、透明度、差异、scissor、关闭还原。
   检查目标深度附件实际绑定、D3D 调试层，以及真实 GPU 的视频 UI 叠加。
4. 在 PR #45–#49 组合、用户素材、100/125/150/200% 上查看真实应用的源顺序确认窗。

高 DPI 的 Linux `grabImage` 在此环境存在裁切与坐标比例不一致，未将它计为 DPI 通过。
云端已有 EGL / Xorg 尝试失败后未绕过限制、未新增依赖；未触发 CI、未操作用户电脑。
