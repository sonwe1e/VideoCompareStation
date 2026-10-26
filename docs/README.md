# 文档导航

日常修改从 [Agent 路由](agent-guide.md) 定位代码和测试，只读取与任务有关的专题。
产品范围、问题状态和设计取舍分别由下面三份文档维护；历史执行记录不作为当前待办。

| 要回答的问题 | 入口 |
|---|---|
| 产品做什么、哪些行为必须保留 | [产品目标与工作流](product/visual-review.md) |
| 当前还有什么缺口、哪些仅缺验收 | [问题台账](engineering/visual-review-backlog.md) |
| 默认行为与产品取舍是否已确认 | [决策登记册](engineering/decision-register.md) |
| 从哪里改代码、跑哪些测试 | [Agent 快速定位指南](agent-guide.md) |
| 怎么构建、选择验收范围、恢复环境 | [构建指南](building.md) |
| 层次与状态由谁负责 | [架构](architecture.md)、[依赖规则](architecture/dependency-rules.md)、[状态所有权](architecture/state-ownership.md)、[ADR](adr/) |
| 格式、颜色、像素与对齐的边界 | [媒体支持](media-support.md)、[图片颜色契约](engineering/image-color-contract.md)、[对齐规则](alignment.md)、[像素尺度](engineering/viewport-pixel-scale.md) |
| 性能和真实显示如何验证 | [runner 协议](self-hosted-runner.md)、[真实环境走查](engineering/real-environment-verification.md)、[证据记录方法](engineering/reproducible-baseline.md) |
| 测试在哪运行、CI 如何路由 | [测试环境](engineering/test-environment-separation.md)、[托管 Windows CI](engineering/hosted-windows-ci.md) |
| 已发布版本具有什么证据 | [发布记录](releases/)、[版本约定](versioning.md) |

`engineering/` 的其余专题记录具体契约、诊断方法和带版本的测量证据，按路由链接读取。
`plans/` 保留仍有待办或承载当前设计约定的方案；完成状态回写问题台账，不另建第二份状态表。
[长期路线](CompareStation_长期产品与技术路线_2026-10-10.md)与[能力索引](engineering/capability-ledger.md)
用于评估后续扩展，不是每次修改必须执行的工单。

## 维护方式

- 同一规则只保留一个主要出处，其他页面链接过去。不要复制构建命令、支持矩阵和整轮测试数字。
- 当前说明直接描述当前行为；已完成工作的逐轮日志由 Git 和发布记录追溯。
  只保留仍影响后续判断的契约、反例、测量协议与未解决事项。
- 旧问题改写为最新状态，保留 ID；不在页首不断追加相互冲突的“当前工作区”记录。
- 删除被替代的文档时同步修复引用。旧文档可用 `git log -- <路径>` 找到，
  再用 `git show <提交>:<路径>` 读取，不另建重复归档目录。
- 文档清理不代表产品缺陷已修复，也不把旧版本的测试通过转移到新版本。
