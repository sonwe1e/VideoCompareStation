# 收尾团队 Journal

跨会话唯一真相源。规则见 [团队协作章程](team-charter.md) §5：只追加与更正，
不删历史；先读后写；完成声明 ≠ 效果证据；未知写未知。

记录格式：`日期 | section | 角色 | 动作与证据 | 决策与理由 | 坑 | 下一步`

---

## 2026-10-31 | §0 基建与基线 | 架构师

**状态盘点（只读命令核实）**

- HEAD = `c0dba16` chore(release): prepare CompareStation 2.7.0。
- 工作树有**未提交的大规模重构**：108 文件、+610/−34876，主体是把 `src/ui_qml/src/` 与
  `src/platform_windows/src/` 的平铺文件迁入功能子目录（`image/` `playback/` `comparison/`
  `files/` `outputs/` `workspace/` `graphics/` `support/`），CMakeLists、测试 CMake、
  三个 QML 测试与新增 `docs/architecture/repository-map.md` 同步更新。内容为重排而非重写。
- 构建痕迹：`out/build/dev/build.ninja` 生成于 10-30 13:00，`CompareStation.exe` 构建于
  10-30 13:20；全量 ctest 日志存在（1452 例）。推断：重构当日已构建；**未经本轮复验**。

**决策**：§0 第一动作是复验工作树构建健康度（增量构建），通过后才在其上规划四类润色。
未授权前不 commit、不 stash、不改动重构本身。

**坑（已知）**：`out/build/*.log` 是共享 scratch，读取时注意可能是旧副本；本轮构建结论
以当次命令输出为准。

**坑（缓存失效，2026-10-31 实踩）**：dev preset 的 `CMakeCache.txt` 里
`CMAKE_MAKE_PROGRAM` 指向已卸载的 WinGet 版 ninja；wrapper 缓存校验判 stale，
只有 `-Fresh` 能通过（`-Configure` 会被同一校验拦下）。`-Fresh` 保留 `out/vcpkg*`，
全量重编 dev 预设。无 sccache（本机未安装），编译缓存关闭。

**下一步**：跑 `pwsh tools/build/build.ps1 -Preset dev -Fresh` 全量重建；绿 → 按
`measure-startup.ps1` 协议建启动基线（无文件 + 带 1080p60 素材，release，5 轮 + 1 预热）。
重构本身的内容验证昨日已做（ctest 日志在 `out/build/dev-ctest.log`），本轮 §0 改动为零代码
变更（仅重建），按影响范围（D-021）不默认重跑全量套件；全量回归留到 §5 总验收。

---

## 2026-10-31 | §0 完成 | 助手

- dev 与 release 双双 `-Fresh` 全量重建成功（各 304 步，MSVC 头依赖校验通过）。
  工作树重构（10-30）编译健康，可作为后续工作地面。
- 启动基线（release、热缓存、5+1 轮）落盘：
  `out/startup-measurements/polish-b0-nofile-20261101-020220/`、
  `out/startup-measurements/polish-b0-onefile-20261101-020236/`。
- 基线关键值（中位）：无文件 spawn→窗口 454.6 ms、→exec 735.3 ms；带文件首帧提交
  （kind 8）820.1 ms；QML 装载段（splash-painted→qml-loaded）359.1 ms；
  show-enter→sg-initialized 195.2 ms；sg-init→首帧 85.4 ms；进程装载 23.9 ms。
  与历史 v2.1.0 数字（645.8/949.9）跨月，只作参考，**不作回归判定**。

---

## 2026-10-31 | §1 启动速度 · 研究 | 研究员

**问题**：用户提案「首次打开后常驻后台一小段时间，二次打开更快」。用户所说「每次都要审阅
环境」来自 splash 副标题「正在准备审阅环境」（`DesktopApplication.cpp:223`）——启动路径
**没有**环境审计，耗时在 QML 实例化 + D3D11 设备/场景图初始化。

**已核实的基础设施（读码，非推断）**：

- 单实例交接已完整：`StartupRequestBroker`（QLocalServer/QLocalSocket 命名管道 + 选举锁文件
  + PID 问候 + 前台授权 + `OK\n` 确认）；`Main.cpp` 把转发请求接到
  `StartupRequestDispatcher` → `applyStartupRequest` → `activateWindow()`
  （show+raise+SetForegroundWindow，已支持最小化恢复）。
- 退出路径：`Main.qml onClosing` 仅守卫未保存图片编辑，否则默认 quit-on-last-window-closed。
- broker 仅非 smoke/非 stillImage 模式创建（测量与自动化路径不受影响）。

**候选与结论**：

| 候选 | 评估 |
|---|---|
| A. 关窗后进程驻留（用户提案） | **推荐**。close→hide；转发请求路径已存在；加空闲超时（匹配「一小段时间」）与显式退出入口。预期二次打开 ≈ IPC+show ≈ 数十 ms（冷启动 735 ms）。风险：驻留 RSS 待实测；退出语义偏离 Windows 惯例（需设置开关+提示）；长时间驻留更易遇显示变更/设备丢失（与 ENG-02 设备代际相关）；**测量口径变化**：`measure-startup.ps1` 每轮靠关窗收尾，常驻后第 2 轮起会转发给驻留进程——冷启动 A/B 必须改为轮间 kill 或独立驻留基准，否则数据失真 |
| B. 暖 helper 进程 | 不推荐。Windows 无法跨进程转移 HWND/QML 对象；进程装载仅 23.9 ms 且 DLL 常驻 standby，收益接近零 |
| C. qmlcachegen 预编译 | 已否决（startup-performance-v2.md Step 2：实例化主导，零收益）。仅作对照 |
| D. 冷启动本体再压缩 | 无近期廉价杠杆：Step 8 证明成本弥散（按组件懒加载只有比例切片）；Step 5 证明请求提前架构性不可行；sg-init 195 ms 属设备生命周期架构改动。splash 已覆盖感知（~44 ms 上屏到首帧） |

**待用户决策（charter §8 产品取舍）**：常驻的退出语义与内存代价——(a) 默认开启 +
空闲自动退出（如 15–30 min）+ 菜单「退出」+ 设置可关；(b) 默认关闭、设置内开启；(c) 不做。
模型建议 (a)：用户工作流（反复开关做对比审查）收益最大，代价有界且可关。**未获选择前不动手实现。**

**下一步**：用户决策后，架构师定 A 的设计契约（驻留条件、超时、退出入口、设备丢失行为、
测量协议适配），builder 实现，verifier 补行为测试 + 变异证据 + 驻留 RSS/二次打开实测。
