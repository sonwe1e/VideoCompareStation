# 连续倒退：先完成当前解码，再预备前一帧

日期：2026-10-07。修复基线：`a0a044066345ed0110f87caab56afb5147f615d5`。
本项是请求调度正确性修复，Windows 实播与硬件性能仍待验证。

## 用户路径与问题

`ReviewShortcuts.qml` 的 Left / A 经 `ReviewActions.previousFrame()`、
`ReviewController::previous()` 提交 `StepFramesCommand{-1}`。控制器明确允许在上一导航
尚未完成时继续提交导航，因此连续按键可能重叠；是否在具体机器上出现取决于调度时序。

在第 8 帧提交倒退到 7，尚未收到 Ready 或 Success 时再倒退一次：原协调器立即提交 6。
真实 provider 的 Reverse 与 Exact 共用一个 latest-wins 槽。新请求会挤掉排队的 7，或取消
仍在解码的 7；协调器随后收到当前请求的取消终态，会撤销整个倒退队列。两个按键可能都
成为 Canceled，画面仍是 8。单纯验证“两个请求使用同一 generation”无法发现这个冲突。

## 最小改动

仅改 `PlaybackCoordinator.cpp`：

- Reverse 当前帧尚无 `providerSucceeded` 时，把后继按键留在已有有界队列。
- 收到当前帧 Success 后立即尝试准备后继，仍可与当前帧的呈现 / ACK 等待重叠。
- Forward 的 Sequential 提交、Exact / seek、provider、decoder、帧组提交条件与身份
  校验不变；不串行等待每帧 ACK，不增加新的队列或状态模型。

真实 provider 在发送 Ready 前已经用 CAS 领取成功终态，所以此处 Success 是协调器可以
观察到的保守安全边界。Ready 本身不设置协调器的 `providerSucceeded`；它不是此修复选择的
放行信号。Success 后即使 provider 尚未清空 active 指针，后来的取消也无法赢过已领取的终态。

## 已执行的验证

云端 Linux、GCC 14.2.0、已有 GoogleTest 1.17.0，C++20 / pthread，警告视为错误。
只编译 application / domain 与正式 Fake-port 单元套件；没有实例化真实媒体 adapter。

- 正式 `PlaybackCoordinatorTests` 89/89 与 `PrefetchSchedulerTests` 3/3，共 **92/92**。
  同一二进制重复 20 轮，**1,840/1,840** 执行通过。
- 保留并加强原同 generation 用例：pending 输入、Ready-only 输入、Success 后 ACK 前预备、
  提升后仍 pending 的新当前帧、连续 7→6→5→4、每个命令恰好一次成功。关键“尚未提交”断言
  用已处理的新按键快照作屏障，不用睡眠猜测线程是否完成。
- 新增边界 / shutdown 和 seek 换代后迟到 Success / Cancel 用例；按 commandId 检查终态，
  不要求取消时 queued / current 的回报顺序。已有正向、逐帧、预取、seek、失败、超时、设备
  失效及停止合同一并执行。
- 独立红例使用不变的真实协调器与 Fake ports，将实际捕获的请求重放到 **7 段逐字提取**
  的 provider admission / lifecycle 策略。排队和 active 两种计划均红：请求数 4 而非 3，
  两个命令各一次 Canceled，画面停在 8。基线红例重复 40/40 同样失败。
- 该独立 probe 只替换本次生产文件后，两红例转绿；另两个“已成功领取、ACK 前准备”对照
  保持绿，共 4/4，重复 100 轮 400/400；其余适用旧 step / prefetch 回归 31/31。
- 原基线在最终正式定向 5 项中 3 项失败；另 **11/11** 可编译生产语义变异检出，覆盖去掉
  门控、错用 Ready / ACK、遗漏 Success 钩子、误门控 Forward、优先级 / 帧号 / generation
  错误以及丢失 / 错报终态。编译错误不作为检出。
- 触及区域 **73/73** 断言观察值故障检出（59 个新增 / 改变断言、14 个保留断言），另有
  2/2 无故障控制通过。这是观察 / setup guard 证据，单列于生产语义变异，不能视作 73 种
  真实生产缺陷。两个固定数量 guard 在故意少一项时都提前拒绝执行。

提取策略 shell 是单线程、同 session / generation、Reverse-only 的验证模型：queued
分支按真实代码立即产生取消，active 分支由测试显式调度取消观察出口。它没有运行真实
`MultiSourceFrameProvider` worker、`SourceDecodeActor`、`MediaProbe` 或 `SoftwareDecoder`，
因此不是媒体集成测试，也不能给出 Windows 上的发生概率或耗时改善。

正式测试放在原 `tests/unit/application/PlaybackCoordinatorTests.cpp`，没有引用一次性 probe
或 `out/verification`。原错误的“解码未完成前必须已有两个 Reverse 请求”预期被替换为安全
放行预期；同 generation、后继帧与 ACK 前预备的覆盖保留并加强。

## Windows 体验与门禁（未执行）

1. 打开视频，暂停在中段。快速连续按 Left / A，或短暂按住 Left；检查帧号逐帧倒退，
   慢解码时不要出现整串按键被同代后继请求取消。
2. 切回 Right、点击时间轴 seek、到第 0 帧再倒退、停止或关闭，检查没有旧倒退队列复活。
3. 对 2–3 源比较确认完整帧组同步，GT / 时间线身份与既有播放行为不变。

原生构建后执行：

```powershell
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex '^application[.](PlaybackCoordinatorTests|PrefetchSchedulerTests)'
pwsh tools/build/build.ps1 -Preset dev -Target format-check
pwsh tools/build/build.ps1 -Preset dev -Target lint
```

本轮未执行 Windows / MSVC / 固定 Qt 6.11.1、真实 Main / shell、真实视频解码与 D3D、
完整构建 / CTest / format-check / lint、硬件帧率 / 延迟门禁或 ZIP 发布。未改变工作流，
未触发或重跑 CI，未合并或部署。纯逻辑测试耗时不代表实际播放 FPS 或流畅度。

另一个只读发现是 reverse-window warmup 的队列中断判定，涉及具体 decoder 调度；本次
没有修改或验证它，不据此宣称所有连续倒退性能问题已经解决。
