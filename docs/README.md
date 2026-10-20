# Documentation

Start here: [Agent 快速定位指南](agent-guide.md) → [产品目标](product/visual-review.md)
→ [当前问题台账](engineering/visual-review-backlog.md)。台账区分已提交基线、工作区实现、
未实现能力和待实机验证风险，并提供代码与测试入口。

CompareStation documentation is grouped by lifecycle and audience:

- [building.md](building.md) defines the build wrapper, tool overrides, dependency setup,
  and recovery from stale caches or missing header dependencies.
- [product/visual-review.md](product/visual-review.md) records current user requirements and
  clearly separates recommended designs from agreed scope. Audio and subtitles are out of scope.
- [engineering/visual-review-backlog.md](engineering/visual-review-backlog.md) is the current
  issue and acceptance index; inspect HEAD and local changes before acting on a dated status.
- [engineering/decision-register.md](engineering/decision-register.md) records which side a
  dispute was settled on, with its status (confirmed, temporary assumption, rejected, not
  triggered). Check it before treating a product doc's default as agreed.
- [architecture.md](architecture.md), [architecture/](architecture/), and [adr/](adr/) define
  current architecture, dependency rules, state ownership, and accepted decisions.
- [engineering/](engineering/) contains active engineering contracts and diagnostics. Start with
  [maintenance and performance](engineering/maintenance-and-performance.md) for the current
  cleanup and validation boundary, [the frozen behavior baseline](engineering/behavior-baseline.md)
  for pre-overhaul observations at `d04339c`, and [the trace schema](engineering/trace-schema.md) for the
  implemented diagnostic format and its current limitations.
- [engineering/product-quality-review-2026-10-19.md](engineering/product-quality-review-2026-10-19.md)
  is the dated product-experience review (`EX-` items) covering reverse-step stutter, smart
  matting, the single-video comparison dead end, spacing tokens and terminology, plus the
  execution order and acceptance for this round.
- [engineering/ui-ux-review-8d937eb.md](engineering/ui-ux-review-8d937eb.md) is the dated UI/UX
  review at `8d937eb`: design-token consumption, typography, menu and keyboard reachability,
  contrast, and minimum-window layout. It records findings against that revision; most items have
  since been implemented, and the ledger entry of 2026-10-19 (整改) carries the per-item status,
  the deliberately deferred ones, and the contract tests that now lock them.
- [plans/cache-first-roadmap-2026-10-20.md](plans/cache-first-roadmap-2026-10-20.md) is the current
  execution order: cache and held-backward first, then comparing an edit against its original, then
  the common light edits, then multiple candidates. It records the settled material baseline
  (1080p 8-bit, PNG and JPG, reverse steps mostly on a single source) and what is deliberately not
  built (16-bit PNG, Deflate TIFF, memory-proportional budgets).
- [plans/playback-overhaul/](plans/playback-overhaul/) preserves the earlier playback overhaul
  plan and review reports. Some changes have landed; use the current ledger and ADRs to identify
  remaining work. Its old priorities and optional audio expansion are not current product scope.
- [releases/](releases/) records shipped behavior and release-specific validation.
- [design/](design/) preserves the historical A/B design overview; the current target design lives
  in [architecture.md](architecture.md).
- [archive/](archive/) preserves superseded designs, implementation plans, and historical reviews;
  archived documents are not current requirements.

Topic guides such as [alignment.md](alignment.md), [media-support.md](media-support.md), and
[self-hosted-runner.md](self-hosted-runner.md) remain at this level for direct access.
