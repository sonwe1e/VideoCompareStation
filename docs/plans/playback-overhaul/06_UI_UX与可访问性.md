# 06｜UI、UX 与可访问性整改方案

## 核心结论

Toy 的 UI 已具备专业工作站雏形，但当前用户仍需要理解 “A/B/C、canonical、Reference、Pair、Alignment、Exactness” 等内部概念才能稳定操作。最佳整改方向不是增加更多常驻工具栏，而是建立清晰的信息层级：

```text
我正在看哪些视频
    ↓
哪一个定义时间线，哪一个是 GT，当前比较哪两路
    ↓
现在处于什么播放/比较/分析状态
    ↓
如何定位问题、查看证据、恢复错误
```

目标 UI 应在不牺牲帧准确性的前提下做到：**默认路径简单、专业能力可发现、状态可解释、键盘可完成、错误可恢复、窄窗口与高 DPI 下不重叠。**

---

## 一、信息架构：从“控件堆叠”转为四个稳定区域

### 1.1 Source Rail：管理身份与角色

顶部 Source Rail 只回答“有哪些源、它们是什么角色”。

每个 Source Chip 显示：

```text
[GT] GroundTruth.mp4     [C1] model_v7.mp4     [C2] model_v8.mp4
```

Chip 内容：

- 用户别名，缺省为文件名；
- Role：Reference / Candidate / Auxiliary；
- Timeline Master 标记，例如时钟图标，不再复用 `R`；
- 活动 Pair 端点高亮；
- changed-on-disk / decode fallback / missing 状态；
- 右键或 overflow 菜单：重命名、设为 Reference、设为 Timeline Master、加入 Pair、移除、媒体信息。

**不要**继续用单个 “R” 同时表示 Reference 和 canonical timeline。

### 1.2 Comparison Bar：只管理当前比较意图

建议布局：

```text
Compare  [GT ▾]  ↔  [C2 ▾]  [Swap]
Mode [Wipe ▾]  Metric [Luma ▾]  Gain [4× ▾]
```

两端选择器显示别名和角色，不只显示 A/B/C。只有需要 Pair 的模式才显示 Pair 控件。Three-up/Overview 模式下可隐藏 Pair，但保留最近 Pair 状态。

### 1.3 Viewer：画面优先

Viewer 负责：

- 视频与差异画面；
- 非遮挡式源标签；
- Wipe/ROI/Pixel Inspector；
- 临时 HUD；
- 阻断性错误覆盖层。

不应在 Viewer 内长期叠加多个状态气泡。正常信息移到 Status Rail。

### 1.4 Timeline + Status Rail：导航与解释

底部稳定区：

```text
Timecode / Frame     Timeline / Markers / Range     Transport
Status: [Frame-accurate] [GT↔C2] [Pixel-exact] [D3D11VA] [Analysis 42%]
```

Status Rail 按优先级显示：

1. 可恢复/阻断错误；
2. 正在进行的操作；
3. 正确性与比较域；
4. 解码/性能等被动诊断。

点击 badge 展开详细信息；默认不展示长技术字符串。

---

## 二、单源与多源使用路径

### 2.1 单源 Player 模式

目标：

- 打开即播放/逐帧；
- Source Rail 默认收起或轻量显示文件名；
- transport 可 auto-hide overlay；
- Viewer 满画布；
- “添加对比视频”作为主要下一步入口；
- 不出现 Pair、Reference、Alignment 专业控件。

### 2.2 多源 Review 模式

当加入第二路：

1. 保持当前 media time；
2. 原子切换为 Review 布局；
3. transport 默认 docked，不遮挡被评审像素；
4. Source Rail 与 Comparison Bar 固定；
5. 自动选择合理 Pair，并明确提示；
6. 如时间线/分辨率不兼容，显示引导式兼容性摘要，而不是直接弹出高级 Alignment 面板。

### 2.3 多候选模式

Source Rail 超过可用宽度时：

- 不强制所有 Chip 横向挤压；
- 使用候选下拉/搜索与最近使用；
- Reference 固定显示；
- 活动 Candidate 固定显示；
- 其余候选进入可搜索列表；
- 支持上一/下一候选快捷操作；
- 切换候选有 loading 状态，但保留旧画面直到新 FrameSet 可见。

---

## 三、打开、拖放与会话操作

### 3.1 Open Dialog

一次打开多源时提供 staging：

| 文件 | 角色 | 别名 | 时间线主源 |
|---|---|---|---|
| gt.mp4 | Reference | GT | ✓ |
| c1.mp4 | Candidate | Model 7 | |
| c2.mp4 | Candidate | Model 8 | |

用户可拖拽排序，但排序只影响显示，不改变身份。确认前做快速探测，展示：

- 时长/帧率/分辨率；
- 可能的对齐问题；
- 不支持原因；
- 是否会走软件回退。

### 3.2 拖放语义

拖入文件时根据当前状态给出明确 Drop Zone：

- 空会话：Open；
- 单源：Add as comparison；
- 多源未达逻辑上限：Add candidate；
- 拖到某 Source Chip：Replace this source；
- 拖到 Reference 区：Add/replace Reference。

危险操作不应靠猜。Replace 必须保留旧会话直到新源验证成功。

### 3.3 最近文件与工作区

短期可以只保存最近文件；中期建议引入可选 Workspace：

- source identity/alias/role；
- Timeline Master/Reference/Pair；
- alignment；
- range/markers；
- comparison settings；
- analysis result references。

Workspace 保存失败不应影响当前会话；写入必须事务化。

---

## 四、引导式对齐与兼容性反馈

### 4.1 默认只显示摘要

```text
Timing differences detected
C2 appears 2 frames early · confidence 98%
[Review] [Apply] [Ignore]
```

“Review” 进入详细页：

- Global offset；
- local anomalies；
- missing/duplicate；
- 建议 mapping；
- confidence；
- frame/time examples。

### 4.2 Advanced 区域

手动 offset、anchors、strict index、sequence mapping 收进 “Advanced correction”。默认用户不需要先理解算法名。

### 4.3 应用与撤销

- Apply 前预览不会修改 authoritative timeline；
- Apply 后 Pair、range、markers 通过 revision 重算；
- 支持 Undo；
- 状态栏明确显示 `Strict Index` / `Auto Aligned 98%` / `Manual`;
- 低置信度不得默认自动应用。

---

## 五、播放与时间轴体验

### 5.1 Transport 层级

核心按钮保持有限：

```text
First  Prev Frame  Play/Pause  Next Frame  Last
```

±5 frame、±1 second、速度、loop 进入二级或可配置区域，避免九个等权按钮挤在一起。专业用户仍可通过快捷键使用。

### 5.2 时间轴 Scrub

拖动流程：

- 按下：进入 Scrub，播放暂停；
- 移动：缩略图/代理画面、时间码、帧号即时变化；
- 主 Viewer 可选择保持旧帧或展示标记为 Preview 的画面；
- 松开：精确 seek；
- Esc/失焦：取消并回到旧帧；
- 长任务显示“Preview only”，不伪装成已提交帧。

### 5.3 Marker

Marker 分轨显示：

- alignment；
- missing/duplicate；
- metric anomaly；
- user marker；
- range In/Out。

颜色不是唯一编码；同时使用形状/图标/tooltip。密集时聚合，放大后展开。

### 5.4 时间码与帧号

同屏显示：

```text
00:01:12:18 DF   ·   Canonical frame 2188
GT frame 2188    ·   C2 frame 2186
```

VFR 下显示媒体时间 + frame，不伪造固定帧率 timecode。

---

## 六、比较交互

### 6.1 Pair 必须始终可识别

在 Wipe/Diff/Blink/Blend 中，画面角标使用别名：

```text
LEFT · GT
RIGHT · Model v8
```

空间不足时先保留 alias，文件名放 tooltip。沉浸模式也不能只留下 A/B。

### 6.2 Wipe 控件

目标行为：

- Tab 可聚焦；
- 读屏名称：“Wipe position”；
- value 0～100%；
- Left/Right 1%，Shift 5%；
- Home/End 0/100%，Space/Enter 50%；
- 双击重置；
- drag hit target ≥ 44×44 logical px；
- 高 DPI 下 rail 与实际分割位置严格一致。

### 6.3 Difference 控件

根据 metric 动态显示相关参数：

- Exact Plane：禁用 filter，说明要求；
- Heatmap：显示 legend；
- Threshold：数值输入 + slider；
- Gain：仅对适用 metric；
- Exactness badge 永远可见；
- 不可用时说明具体原因并提供可行替代，例如“切换到 Display-space Diff”。

### 6.4 Pixel Inspector

- 鼠标 hover 临时查看；
- 点击锁定；
- 键盘方向移动 1 像素；
- Shift 移动 10 像素；
- Esc 解锁；
- 复制当前样本；
- 显示源坐标、映射帧、domain 与 resampling 状态。

---

## 七、错误、进度与恢复

### 7.1 错误分级

```cpp
enum class UserErrorSeverity {
    Informational,
    Recoverable,
    BlockingCanvas,
    FatalApplication,
};
```

- Informational：toast/status；
- Recoverable：Status Rail + action；
- BlockingCanvas：中央 overlay；
- Fatal：退出前提供日志路径/复制诊断。

### 7.2 技术细节分离

用户信息：

```text
C2 could not decode this frame.
[Retry] [Use software decode] [Details]
```

Details：

```text
SourceKey, frame, codec, backend, HRESULT/FFmpeg error,
session/epoch/generation/request, recent event trace
```

技术细节不能挤进主提示，也不能完全丢失。

### 7.3 进度

长任务必须区分：

- Probing；
- Opening decoder；
- Building index；
- Analyzing alignment；
- Computing metrics；
- Exporting。

有确定工作量时显示百分比；否则使用不确定进度和已完成数量。取消按钮只在后端真正支持取消时出现。

### 7.4 自动恢复

- 硬件解码失败可尝试软件回退；
- GPU device lost 重建期间保留最后帧/明确遮罩；
- source changed-on-disk 提供 Reload/Keep current；
- topology change 失败回滚；
- range/alignment remap 失败清除相关状态并说明；
- 不要用“Something went wrong”。

---

## 八、快捷键与输入上下文

### 8.1 命令注册表

所有菜单、按钮、快捷键、命令面板共享一个 `ActionRegistry`：

```cpp
struct ActionDescriptor {
    ActionId id;
    QString titleKey;
    QString descriptionKey;
    std::vector<KeySequence> defaultBindings;
    ActionContext context;
    CapabilityPredicate enabledWhen;
};
```

这样避免 QML、菜单和帮助页各写一份快捷键。

### 8.2 默认映射原则

- Space：Play/Pause；
- Left/Right：±1 frame；
- Shift+Left/Right：±5；
- Ctrl+Left/Right：±1 second；
- Home/End：First/Last；
- I/O：Range In/Out；
- `\`：Play Range；
- Tab：Chrome；
- F11：Fullscreen；
- Esc：按层级取消当前临时状态；
- `?`：Shortcut Help。

不要让同一个键在不同 preset 下从“1 帧”变成“150 帧”而无显著提示。

### 8.3 Esc 层级

优先级固定：

1. 取消 Pixel Inspector lock/ROI draw/Scrub；
2. 关闭 popup/menu/dialog；
3. 退出 immersive；
4. 退出 fullscreen；
5. 不关闭应用。

### 8.4 输入冲突

文本输入、Combo popup、菜单、Dialog 激活时禁用全局媒体快捷键。焦点恢复必须回到 Viewer 或操作发起控件，不能落到不确定对象。

### 8.5 命令面板

中期加入 `Ctrl+K`：

- 搜索所有动作；
- 显示快捷键；
- 支持“Set Reference to… / Compare with… / Go to frame…”；
- 提升专业能力可发现性，而不增加常驻控件。

---

## 九、可访问性

### 9.1 最低要求

每个交互控件必须有：

- `Accessible.role`；
- 本地化 name；
- description/help；
- enabled/checked/value 状态；
- 可见焦点环；
- 键盘等价操作；
- 逻辑焦点顺序；
- 不只靠颜色传达。

### 9.2 时间轴 Slider

当前仅有基础角色时，需补齐：

- minimum = 0；
- maximum = totalFrames - 1；
- value = preview/current frame；
- valueText = timecode + frame；
- increment/decrement；
- PageUp/PageDown；
- Home/End；
- 拖动状态描述；
- Marker 不应被读屏逐一轰炸，改为摘要与可浏览列表。

### 9.3 Viewer

视频本身可标记为 image/graphic，Accessible description 包含：

```text
Synchronized comparison of GT and Model v8,
Wipe at 52 percent, canonical frame 2188,
pixel-exact, paused.
```

高频帧变化不要每帧自动朗读；仅在用户导航停止或请求状态时播报。

### 9.4 动效与闪烁

- Blink 模式必须提示可能闪烁；
- 提供降低闪烁/关闭动画设置；
- 尊重系统 reduced-motion；
- opacity/slide 动画不影响状态提交；
- 错误不得仅靠红色。

---

## 十、响应式布局与 DPI

### 10.1 断点

建议按可用 logical width：

- ≥1440：Inspector docked；
- 1120～1439：紧凑 toolbar，Inspector 可窄栏；
- 960～1119：Inspector drawer；
- <960：不支持窗口继续缩小，或进入明确 compact layout。

断点逻辑集中在 Layout Model，不散落在多个 QML ternary。

### 10.2 DPI 与多屏

必须测试：

- 100/125/150/175/200/250%；
- 窗口跨不同 DPI 显示器；
- 60/120/144/240 Hz；
- SDR/HDR 桌面；
- 独显/核显切换；
- 最大化/全屏/恢复；
- taskbar auto-hide；
- 远程桌面/WARP fallback。

### 10.3 像素正确性

QML logical 坐标、DPR、D3D11 pixel viewport、Wipe split、ROI、hit-test 必须来自同一 geometry contract。UI 视觉 golden 之外，还要做 pixel readback 与 hit coordinate round-trip。

---

## 十一、本地化与术语

建议统一术语：

| 内部术语 | 用户术语 |
|---|---|
| canonical source | Timeline master / 时间线主源 |
| reference role | Reference / GT / 基准 |
| difference edge | Comparison pair / 对比组合 |
| exact code value | Pixel-exact / 像素编码精确 |
| temporally aligned | Time-mapped / 已做时间映射 |
| source topology | Loaded videos / 已加载视频集合 |

所有字符串进入消息目录；不要在 C++/QML 中散落英文技术句。中文、英文和长文件名都需要布局测试。

---

## 十二、UX 验收场景

1. 新用户打开单个视频，五秒内能播放、暂停、逐帧和拖动定位；
2. 添加 GT 与两个候选后，不看文档也能识别当前 Pair；
3. 切换候选保持同一画面时刻、ROI 和缩放；
4. 时间轴拖动有即时预览，松手后精确定位；
5. Wipe 完全可用键盘操作，并被读屏识别为可调值；
6. Difference 不可用时，用户知道原因与替代方案；
7. 解码/GPU 故障后有明确恢复动作，旧 Ready 画面不会无故消失；
8. 960×640 与 200% DPI 下 Viewer、transport、Inspector 不重叠；
9. 所有菜单动作可通过 ActionRegistry/Command Palette 找到；
10. 多候选时 UI 不变成无穷横向小窗，而是围绕 Reference + Active Candidate 工作。

整体原则是：**把复杂性放进可验证的模型与渐进披露，而不是让用户通过记住 A/B/C 和隐藏状态来管理复杂性。**
