> **Status: historical.** This architecture review was the basis for the 1.4.5/1.5 refactoring direction. Some recommendations were implemented (presentation_contract, dependency-rules, state-ownership, feature-change-impact); others remain target state. The current architecture lives in [architecture.md](../../architecture.md).

# 核心结论

**有必要重构，但不应该推翻现有架构重写。**

当前最新 `dev` HEAD 是 `96e5e39c`，仍属于 1.4.4 修复线。项目的宏观架构其实是健康的：Domain、Application、Windows 平台、FFmpeg、持久化、Qt UI、Explorer Shell 和组合根已经分层，CMake 还会校验依赖方向。真正的问题是，几个模块内部逐渐形成了新的“单体”：`PlaybackCoordinator`、`ReviewController`、`ReviewShellController`、`Main.qml`、`ReviewRuntime` 和 `platform_windows` 承担的职责都过多。

因此当前项目更准确的状态是：

> **外层分层正确，内层功能边界开始模糊。继续直接添加播放倍速、音频、导出、新比较模式等能力，修改成本和回归风险会越来越高。**

建议保留现有 Ports-and-Adapters / Clean Architecture 骨架，把后续工作定位为一次**模块化单体内部重构**。

---

# 一、当前结构中值得保留的部分

## 1. 顶层模块划分是合理的

目前生产模块为：

```text
src/domain
src/application
src/platform_windows
src/media_ffmpeg
src/persistence_json
src/ui_qml
src/shell_windows
src/app
```

其中 Domain 不依赖其他模块，Application 只依赖 Domain，外层适配器实现 Application Ports，App 负责组合。根 CMake 会在配置阶段检查非法 target 依赖。

这个方向不应改变。没有必要把整个仓库改成微服务、动态插件或完全按 Feature 顶层分目录。

## 2. 核心异步模型是正确的

项目使用：

* 不可变 `SessionSnapshot`；
* 带 Session/Epoch/Generation/Request Identity 的异步协议；
* 完整 `FrameSet` 原子发布；
* Application Ports 隔离 FFmpeg、D3D11、Settings；
* Render ACK 后才更新可见帧。

这些设计对逐帧审查工具非常重要。`PlaybackCoordinator` 通过明确依赖注入连接 Probe、Frame Provider、Alignment、Scheduler、Clock 和 Render Channel。

重构时应保留单一 Coordinator Worker 和完整 FrameSet 语义，不能为了拆分类而拆出多个互相竞争的线程状态机。

## 3. 测试基础已经较强

项目已有 Unit、Component、Hardware、E2E、Packaged 和 Soak 等不同测试层级，并提供 WARP、D3D11VA、MSI、Popup Pixel、Shutdown 等专项门禁。

所以这次重构不需要重新建设测试体系，而是需要**调整测试组织和测试接缝**。

---

# 二、当前结构已经开始妨碍迭代的地方

## 1. `ReviewController` 已经成为 UI God Object

`ReviewController` 同时向 QML 暴露：

* Source 文件和列表；
* Reference/Canonical；
* 播放状态和帧；
* Timecode；
* 媒体信息；
* Source/Pair 错误；
* Alignment 状态、进度、Marker；
* Difference Edge；
* Capability；
* Open/Close；
* Seek/Step/Play；
* Alignment 操作；
* 拖放校验；
* Source Identity。

它拥有大量 `Q_PROPERTY` 和 `Q_INVOKABLE`，媒体状态投影、命令入口、错误投影和格式转换全部聚集在一个 QObject 中。

这意味着新增一个功能时，开发者往往首先修改 `ReviewController.h/.cpp`，然后继续修改 Main、QML 组件和测试。它已经成为变更冲突和回归的集中点。

## 2. `ReviewShellController` 与 `ReviewController` 的职责重叠

`ReviewShellController` 又负责：

* Active/Staged Sources；
* Canonical Source；
* Generation；
* Effective View/Pair；
* Intent Queue；
* Source Identity；
* Chrome/Inspector；
* Pending Action；
* In/Out；
* Range Loop。

于是 Source 和 Session 状态存在两个层次：

```text
ReviewController
    后端提交状态、媒体真值

ReviewShellController
    Active/Staged、Intent、Generation、Range、窗口状态
```

这种区分在代码作者脑中可能清晰，但对后来添加功能的人并不直观。特别是播放倍速、Range Playback、Reference 切换、关闭会话等功能，很容易不知道应该放入哪一个 Controller。

更明显的是，`ReviewSessionFacade` 目前仍只是：

```cpp
using ReviewSessionFacade = ReviewShellController;
```

它是命名迁移，而不是真正的 Facade 边界。

## 3. `Main.qml` 仍然持有大量业务和派生状态

当前 Main 根对象仍包含大量状态：

* Wipe；
* Diff Threshold；
* Source Offset；
* Range Playback；
* Drawer Mode；
* Timeline Preview；
* Input Context；
* Error 和 Overlay 文案；
* Compatibility 和 Alignment 拼接；
* Source Missing；
* View Mode 可用性；
* Intent 消息；
* Changed-on-disk 通知。

同时还负责 Range Loop、Session 打开、拖放、重置 Viewport、错误映射和组件连接。

因此 Main.qml 虽然已经提取出多个组件，但本质上仍是一个**页面级状态机加服务定位器**。新增 UI 功能时仍需要进入 Main 增加若干 Property、Binding、Handler 和函数。

理想状态下，Main.qml 应只负责：

```text
窗口
布局
组件实例化
少量顶层信号连接
```

不应该决定 Alignment 文案、Comparison 能力、Range 事务或 Source 操作结果。

## 4. 同一组 Presentation 枚举定义了三遍

以下概念在多个层中重复定义：

```text
ViewMode
DifferenceMetric
DifferenceGain
DifferenceEdge
DifferenceFilter
```

它们分别存在于：

* `ReviewPreferencesController`；

* `ComparisonSurface`；

* `D3d11ComparisonRenderer`。

这意味着新增一个比较模式，至少需要同步修改：

```text
Preferences enum
QML enum
Renderer enum
枚举转换
菜单
Toolbar
Context Menu
Effective View State
Renderer switch
像素测试
持久化解析
```

这正是“添加一个看似简单的功能却很费劲”的典型原因。

## 5. Adapter 之间存在直接横向依赖

架构文档将 `media_ffmpeg`、`platform_windows`、`persistence_json` 和 `ui_qml` 都称为外层 Adapter。

但实际依赖是：

```text
media_ffmpeg     → platform_windows
persistence_json → platform_windows
ui_qml           → platform_windows
```

而且这种依赖不只是实现细节。公开头文件中已经出现具体 Platform 类型：

* `MultiSourceFrameProvider` 构造函数接收 `platform::FrameBudget` 和 `GraphicsDeviceBroker`；

* `ComparisonSurface::attachRendererServices()` 接收 `GraphicsDeviceBroker`、`FrameMailbox` 和 `PresentationAckMailbox`。

这使 Peer Adapter 彼此耦合。更换 Renderer、增加软件渲染、加入音频或独立测试 UI Model 时，都会受到影响。

## 6. `platform_windows` 是一个横向大杂烩模块

同一个 Target 中同时包含：

```text
AtomicFilePublisher
WindowsPaths
ProcessTelemetry
SteadyDeadlineScheduler
SourceIdentityService

FrameBudget
GraphicsDeviceBroker
D3D11 Frame Resources
GPU Transfer
Frame Mailbox
D3D11 Renderer
Presentation ACK
```

这些职责的变化频率、消费者和测试方式都不同。

例如：

* `persistence_json` 可能只需要 AtomicFilePublisher；
* `media_ffmpeg` 需要 FrameBudget 和 GraphicsDeviceBroker；
* `ui_qml` 需要 D3D11 Renderer Bridge；
* Telemetry 与上述功能都不完全相同。

把它们放在同一个 Target，会扩大编译依赖、Public API 面和测试耦合。

## 7. `PlaybackCoordinator` 已经承担多个独立用例

当前 Coordinator 内部同时管理：

* Open/Probe；
* Session Rollback；
* Exact Seek；
* Playback Cadence；
* Prefetch；
* Frame Presentation；
* Global Alignment；
* Sequence Alignment；
* Manual Anchor；
* Automatic Alignment Proposal/Undo；
* Snapshot Publication；
* Command Terminal；
* Error Projection。

它还维护 `PendingProbe`、`PendingPlaybackFrame`、`PlaybackRun`、`ReadySessionBackup`、`BackgroundAnalysis` 等多个状态结构。

Coordinator 作为唯一状态机入口没有问题，但这些逻辑不应继续全部实现在同一个 `.cpp` 内。否则加入播放倍速、音频时钟、循环播放或新的 Alignment 算法，都需要修改同一个高风险文件。

## 8. `ReviewRuntime` 同时是组合根和复杂 Shutdown 状态机

`ReviewRuntime` 不只构建依赖，它还包含：

* Graphics Notification Pump；

* Projection Bridge；

* Decoder Backend Cache；

* Render Activity Bridge；

* 所有 Adapter 的构造；

* QML Surface 绑定；

* 有界 Shutdown；

* Detached Control Task；

* 对各个 Worker 的销毁顺序。

如果未来增加 Audio Output、Evidence Export、Subtitle、Cache Service 或第二种 Renderer，开发者必须直接进入这个复杂 Shutdown 文件中修改生命周期关系，风险较高。

---

# 三、测试结构同样存在维护成本

## 1. Component Test 依赖大量私有实现目录

媒体测试显式包含：

```text
src/media_ffmpeg/src
src/platform_windows/include
```

平台测试也包含：

```text
src/platform_windows/src
```

UI 测试包含：

```text
src/ui_qml/src
```

这类 White-box Test 对验证复杂并发实现有价值，但会造成：

> 只是移动一个私有类或拆分一个 `.cpp`，大量测试也必须同步修改。

建议保留少量 Internal Tests，但大部分 Component Tests 应通过公开 Port、Fixture Driver 或 Test Harness 访问功能。

## 2. 测试物理目录与测试层级名称不一致

`Testing.cmake` 定义了：

```text
unit
component
integration
ui
e2e
hardware
performance
packaged
soak
```

但物理目录为：

```text
tests/unit
tests/component
tests/hardware
tests/smoke
```

而 `tests/smoke` 实际包含 E2E、Packaged 和 Soak。

这不会导致功能错误，但会增加测试发现成本。开发者想为一个新功能添加 Integration Test 时，不容易判断应该放在哪里。

## 3. 测试注册方式不统一

项目已经提供 `dvs_add_test()` 统一 Layer 和 Module 标签，但部分 QML 测试仍直接使用 `add_test()` 和多层 CMake 循环。

结果是 Style、DPI、Render Loop、Timeout 和 Label 配置散落在各个 CMakeLists 中。

## 4. 每个模块一个大型 Test Executable

例如 Application Unit Test 和 Media Component Test 都把多个功能测试编译进同一个 Executable。

随着功能增加：

* 修改一个测试可能需要重新链接整个模块测试；
* Test Support 容易变成共享全局 Fixture；
* 单个测试进程的初始化和全局状态更复杂；
* 很难按 Capability 独立运行和定位。

---

# 四、建议的目标架构

不建议立即修改所有顶层目录。第一阶段可以保留现有 Target 名称，只在 Target 内部按 Capability 整理。

建议目标结构：

```text
src/
├── domain/
│   ├── media/
│   ├── comparison/
│   ├── timing/
│   └── validation/
│
├── application/
│   ├── session/
│   │   ├── SessionCommands.h
│   │   ├── SessionEvents.h
│   │   ├── SessionState.h
│   │   └── SessionWorkflow.cpp
│   ├── playback/
│   │   ├── PlaybackCommands.h
│   │   ├── PlaybackCadence.cpp
│   │   ├── PlaybackStateMachine.cpp
│   │   └── PrefetchPolicy.cpp
│   ├── alignment/
│   ├── diagnostics/
│   ├── ports/
│   └── Coordinator.cpp
│
├── presentation_contract/
│   ├── ComparisonViewConfig.h
│   ├── ComparisonModeDescriptor.h
│   ├── ViewportState.h
│   └── DifferenceConfig.h
│
├── platform_windows/
│   ├── support/
│   ├── telemetry/
│   └── scheduling/
│
├── graphics_d3d11/
│   ├── device/
│   ├── resources/
│   ├── transfer/
│   ├── presentation/
│   └── renderer/
│
├── media_ffmpeg/
│   ├── probe/
│   ├── decode/
│   ├── cache/
│   └── alignment/
│
├── persistence_json/
│
├── ui_qml/
│   ├── viewmodels/
│   │   ├── SessionViewModel
│   │   ├── PlaybackViewModel
│   │   ├── AlignmentViewModel
│   │   ├── ComparisonViewModel
│   │   └── NotificationViewModel
│   ├── render_bridge/
│   └── qml/
│       ├── shell/
│       ├── sources/
│       ├── playback/
│       ├── comparison/
│       ├── alignment/
│       └── common/
│
└── app/
    ├── composition/
    ├── startup/
    └── shutdown/
```

最终仍然是单个 Windows 程序，不是微服务。

---

# 五、最优先的五项重构

## 1. 建立真正的 `ReviewSessionFacade`

不要继续使用类型别名。Facade 应暴露几个窄对象：

```cpp
class ReviewSessionFacade : public QObject {
    Q_OBJECT

    Q_PROPERTY(SessionViewModel* session CONSTANT)
    Q_PROPERTY(PlaybackViewModel* playback CONSTANT)
    Q_PROPERTY(AlignmentViewModel* alignment CONSTANT)
    Q_PROPERTY(ComparisonViewModel* comparison CONSTANT)
    Q_PROPERTY(NotificationViewModel* notifications CONSTANT)
};
```

状态所有权应明确为：

| 状态                            | 唯一所有者                        |
| ----------------------------- | ---------------------------- |
| Source、Reference、Intent Queue | SessionViewModel             |
| Frame、Play/Pause、Seek、Range   | PlaybackViewModel            |
| Offset、Anchor、Analysis、Marker | AlignmentViewModel           |
| Mode、Pair、Diff、Wipe、ROI       | ComparisonViewModel          |
| Toast、Overlay、Input Context   | Notification/Shell ViewModel |
| Decoder、FrameSet、Media Truth  | Application Snapshot         |

QML 组件只绑定自己需要的 ViewModel。

## 2. 引入无 Qt、无 D3D 的 Presentation Contract

把重复的 View/Diff 枚举统一为：

```cpp
namespace dvs::presentation {

enum class ViewMode;
enum class DifferenceMetric;
enum class DifferenceGain;
enum class DifferenceEdge;
enum class DifferenceFilter;

struct ComparisonViewConfig;
struct ViewportConfig;

}
```

UI、Settings 和 D3D Renderer 共用这些类型。

再增加一个模式描述表：

```cpp
struct ComparisonModeDescriptor {
    ViewMode mode;
    int minimumSourceCount;
    int maximumSourceCount;
    bool usesPair;
    bool supportsThreshold;
    bool supportsRoi;
};
```

这样 Menu、Toolbar、Inspector 和 Effective State 可以从同一份能力表生成，不再在 Main.qml 中硬编码多个数组和判断。

## 3. 拆分 `PlaybackCoordinator.cpp`，但保留单一事件循环

推荐内部组合：

```text
PlaybackCoordinator
├── SessionWorkflow
├── PlaybackStateMachine
├── AlignmentWorkflow
├── FrameRequestWorkflow
├── SnapshotPublisher
└── CommandTerminalPublisher
```

Coordinator 仍是唯一状态所有者和 Worker，但把纯状态转换和独立用例委托出去。

例如播放倍速只需要主要修改：

```text
PlaybackCommands
PlaybackStateMachine
PlaybackSnapshot
PlaybackViewModel
PlayerOsc
```

不应再同时进入整个 Open/Alignment/Session Rollback 实现。

## 4. 拆分 `platform_windows`

优先拆成：

```text
dvs_windows_support
    AtomicFilePublisher
    WindowsPaths
    ProcessTelemetry
    SteadyDeadlineScheduler
    SourceIdentityService

dvs_graphics_d3d11
    FrameBudget
    GraphicsDeviceBroker
    FrameMailbox
    GPU Resources
    GPU Transfer
    Renderer
    Presentation ACK
```

然后：

```text
persistence_json → windows_support
media_ffmpeg     → graphics contract / graphics_d3d11
ui render bridge → graphics_d3d11
ui models        → 不依赖 graphics_d3d11
```

这会让普通 QML 菜单和 ViewModel 测试不再需要链接整个 D3D11 子系统。

## 5. 将 Main.qml 变成纯 Composition Root

目标是：

```qml
ApplicationWindow {
    ApplicationMenuBar {}
    ReviewWorkspace {}
    ReviewDialogs {}
    NotificationOverlay {}
}
```

以下逻辑应移出 Main：

* Error Key → 用户文案；
* Effective Comparison State；
* Available Modes；
* Range Loop 状态机；
* Source 操作事务；
* Changed-on-disk 通知；
* Alignment Status 拼接；
* Intent Error 映射。

Main 只保留真正的 Window 状态，例如 `visibility` 和系统 Fullscreen。

---

# 六、测试结构的目标方案

建议调整为：

```text
tests/
├── support/
│   ├── FakeMediaProbe
│   ├── FakeFrameProvider
│   ├── FakeRenderChannel
│   ├── DeterministicClock
│   ├── SessionDriver
│   └── QmlTestHost
│
├── unit/
│   ├── domain/
│   └── application/
│       ├── session/
│       ├── playback/
│       └── alignment/
│
├── contract/
│   ├── frame_provider/
│   ├── settings_repository/
│   └── render_channel/
│
├── component/
│   ├── ffmpeg_probe/
│   ├── ffmpeg_decode/
│   ├── d3d11_renderer/
│   ├── ui_models/
│   └── qml_components/
│
├── feature/
│   ├── source_session/
│   ├── playback/
│   ├── comparison/
│   └── alignment/
│
├── e2e/
├── packaged/
├── hardware/
├── performance/
└── soak/
```

关键原则：

1. Unit Test 不依赖 Qt Quick、FFmpeg 或 D3D11。
2. Port 的所有实现运行同一套 Contract Test。
3. QML Component Test 使用窄 ViewModel Fake。
4. 只有少量测试允许包含生产模块的私有 `src/`。
5. 测试目录名、CTest Layer 和 Label 完全一致。
6. 所有测试通过统一 CMake Helper 注册。

---

# 七、增加 Core-only 构建模式

当前根 CMake 在配置开始时就强制：

```text
Windows
MSVC 2022
Ninja
x64-windows vcpkg
```

因此即使只想运行 Domain 或 Application Unit Test，也必须配置完整 Qt、FFmpeg 和 Windows Adapter。

建议增加：

```cmake
option(DVS_BUILD_DESKTOP "Build the Windows desktop app" ON)
option(DVS_BUILD_ADAPTERS "Build platform/media adapters" ON)
```

然后提供：

```text
core-dev
    domain
    application
    unit tests

ui-dev
    core
    Qt ViewModels/QML
    WARP component tests

desktop-dev
    完整 FFmpeg/D3D11/Windows 应用
```

即使不支持 Linux 正式产品，也可以让平台中立的 Domain/Application 在 Clang 或 Linux CI 上快速编译测试。

---

# 八、按未来功能决定重构优先级

| 下一项功能           | 应先完成的重构                                        |
| --------------- | ---------------------------------------------- |
| 播放倍速            | Playback StateMachine、PlaybackViewModel        |
| 新 Compare Mode  | Presentation Contract、Mode Descriptor          |
| Audio           | ReviewRuntime 生命周期拆分、Playback Clock Policy     |
| Bad Case Export | 独立 Use Case、Export Port、Notification ViewModel |
| Alignment 新算法   | AlignmentWorkflow、Alignment Port               |
| 新 Renderer      | UI Model 与 Render Bridge 分离                    |
| 新 Settings      | Typed Preferences Value + Repository Mapping   |
| 新媒体格式           | media_ffmpeg 内部按 Probe/Decode/Cache 拆分         |

如果下一步确实计划添加播放倍速、新比较模式、音频或导出，那么重构应该现在开始，而不是等功能全部加入后再处理。

---

# 九、分阶段执行路线

## 阶段 0：冻结行为

不修改产品行为，只建立：

* 当前测试基线；
* Dependency Graph；
* State Ownership Matrix；
* Feature Change Impact 表；
* Characterization Tests。

## 阶段 1：UI 内部重构

保持现有 CMake Target，完成：

* 真正的 ReviewSessionFacade；
* 拆分 ViewModel；
* Main.qml 减肥；
* Error Catalog；
* Range Controller；
* Comparison Capability Model。

这是收益最高、风险相对最低的一步。

## 阶段 2：Presentation Contract

统一 View/Diff 类型，删除三套重复枚举和映射。

## 阶段 3：Application 内部重构

拆分 Coordinator 内部 Workflow 和协议头文件：

```text
Commands.h
Events.h
Ports.h
SessionSnapshot.h
```

可保留原聚合头文件作为兼容入口，逐步迁移调用者。

## 阶段 4：Adapter Target 拆分

拆分 Windows Support、D3D11 Graphics、UI Model、Qt/D3D Bridge。

## 阶段 5：测试结构重排

建立 Test Support、Port Contract Tests 和 Feature Tests，逐步移除对私有目录的直接依赖。

## 阶段 6：文档和开发模板

增加：

```text
docs/architecture/state-ownership.md
docs/architecture/dependency-rules.md
docs/adr/
docs/features/
```

当前 `docs/architecture.md` 同时包含现状、历史 Phase、验收标准和未来计划，内容充分但职责过多。

---

# 十、不建议做的事情

不建议：

* 一次性重写整个仓库；
* 为每个类创建一个 CMake Target；
* 引入动态插件 ABI；
* 将业务逻辑迁入 QML；
* 同时重构 Coordinator、Renderer、Media Pipeline 和 UI；
* 仅为了目录美观重命名全部顶层路径；
* 删除现有硬件和性能门禁。

后续仓库整理已将旧实现从主线删除；需要追溯时应使用 Git 历史，避免历史源码继续干扰代码搜索和新开发者判断。

---

# 最终判断

当前项目不是“架构差”，恰恰相反，它已经建立了比普通桌面播放器更严格的异步身份、原子 FrameSet 和依赖方向。

真正的问题是：

> **最初的架构边界仍然存在，但复杂度已经集中进几个超大 Coordinator、Controller、Runtime 和 QML Root 中。**

因此最合适的决策是：

> **保留现有宏观分层，立即进行一次以状态所有权、Presentation Contract、UI ViewModel 和测试接缝为核心的渐进式重构。**

完成前两阶段后，添加播放倍速、新比较模式或导出功能的工作量就会明显下降；完成 Adapter 和 Test 拆分后，音频、HDR 或新 Renderer 之类的大功能才会更容易安全落地。
