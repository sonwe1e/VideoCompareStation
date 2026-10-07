# Matroska 惰性索引与片段起点

日期：2026-10-07。代码基线：PR #34 `121e3660e09839b020a34e6cad73afc7fff2242c`。
本页记录云端可复现的修复证据，不是 Windows 或发布验收。

## 用户可见问题与根因

对 30 fps、90 帧、每 15 帧一个关键帧的 H.264 Matroska 文件，请求零起计的
第 83–89 帧。流拷贝应从此前的第 75 帧关键帧开始，导出 15 帧。
原 writer 只报告关键帧时间 `{0}`，实际从第 0 帧导出全部 90 帧。

`avformat_find_stream_info` 后，Matroska 暂时只有一个索引项；第一次 seek 再加载文件 cues。
`ClipExportWriter::keyframeTimes` 却把加载前的索引项数作为整个循环的固定上限。
因此后续关键帧没有交给规划器，起点被错误地向前扩到文件开头。

## 最小修复

只把索引循环的上限改为每轮查询 `avformat_index_get_entries_count`，让第一次 seek
载入的后续条目也得到解析。索引项的时间与位置仍在 seek 前复制；seek 后不使用可能已失效的
索引指针。保留原来的按包位置查找、重复前缀的向前扫描、PTS 原点、B 帧参考包、取消与原子
文件提交路径。没有更改界面、播放时钟、帧对齐规则、转码方式或依赖版本。

## 回归与反例

- 新增 3 个参数化 GoogleTest：`FindsKeyframesLoadedByTheFirstMatroskaSeek`。
  复用既有 12 帧闭合 GOP，直接通过 FFmpeg API 构造 1／3／6 GOP 与零／正／分数起点的
  MKV；3 个区间参数共 27 次真实 remux，不需要 CLI 或编码器作为原生测试依赖。
- 同一套新增测试链接原 writer 时 **3/3 失败**；最终生产 writer、规划器、有理数时间、
  FrameTimeline 与原子发布相关测试 **59/59 通过**，分别完成 GCC14 C++20 的
  `-Wall -Wextra -Werror` O2 与 O0 编译。四个依赖原生 Windows 锁／Unicode 行为的
  既有用例明确排除，没有把它们计为通过。
- 断言覆盖完整关键帧列表、选定 GOP 起点、完成状态、实际起始时间、源包数量、成片压缩包
  字节与相对 PTS、B 帧所需参考包和报告包数。**12/12** 个成功编译的运行时变异由指定
  断言拒绝；包含固定旧索引上限、错误起点／原点／出点／包数、包字节及时间间隔。
  构建失败不计检出；减少变异清单的固定数量 guard 单独验证，最终源码字节复核不变。
- 独立探针使用实际变化画面的 90 帧 MKV，修复后得到关键帧
  0／0.5／1／1.5／2／2.5 秒；请求 83–89 实际输出 15 帧，解码 MD5 逐帧等于源 75–89。
- 独立审查另生成 **10 组**非重复画面的 Matroska／MP4 输入，覆盖 90／900 帧、
  GOP7／15／30／60、B 帧 0／2、零／正起点及 live 无 cues 变体，全部成片的解码像素哈希
  与对应源尾段一致。审查确认 seek 前复制索引值，没有悬空指针读取。

- 独立取消控制：索引从 1 项增长后，在第三次 seek 取消（已处理两个关键帧），返回空列表，
  不泄漏部分索引结果；写出第一个包后取消导出，原目标字节不变且没有遗留临时文件。

## 开销与保留边界

独立调用计数中，GOP15 MKV 从 90 到 900 帧时，关键帧查询的调用方包读取／seek
为 6／6 到 60／60；MP4 为 81／6 到 945／60。计数不包含 demuxer 内部读取，
也不是高码率、长视频、磁盘性能或 Windows 播放性能验收。
另一个 9,000 帧／600 关键帧 MKV 控制需要 600 次调用方读包和 600 次 seek；
0.85 MB 源文件额外 AVIO 读取约 1.75 MB，末 GOP 成片 15 包。此计数不含打开／stream-info
阶段，没有观察到重复扫描前缀的增长；不据小分辨率素材推导生产吞吐量。

MPEG-TS 的 PTS／DTS seek 问题仍未修：独立复查发现 B 帧素材的 backward PTS seek
可跳过选定关键包，短区间失败，完整区间也可能漏掉首个 GOP。零 B 帧对照成功。
这与 Matroska 惰性索引是不同原因，本补丁不扩大到该路径。
粗容器时钟、缺少包位置及未知起点仍须按实际素材另行验证。

## 环境与可复查入口

- 云端 Linux／GCC14／FFmpeg7.1.5；实际生产 writer 与 FFmpeg 库。
  既有 AtomicFilePublisher 采用 POSIX 支撑的 Win32 API 模型；仅去掉其 Windows-only
  编译保护，不把模型视为 NTFS、原生文件锁、Unicode 或持久化保证。
- 本轮探针、编译日志、基线失败、变异清单及最终测试 JSON 位于
  `out/verification/seek/`；一次性探针不替代仓库回归套件。
- 原生入口仍是
  `pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex 'media[.].*ClipExportOriginTests'`；
  需核对实际 CTest 注册名与选中数。完整项目仍要求 Windows／MSVC 2022、
  固定 FFmpeg8.1.2／Qt6.11.1；本轮没有运行原生完整构建、CTest、format-check、lint、
  GUI、GPU／性能门禁或 ZIP。既有 main 的 Windows 原生证据原样保留，但不冒充本轮复验。
- 无 workflow 修改、CI 触发／重跑、合并或部署；PR 仍须保持草稿。
