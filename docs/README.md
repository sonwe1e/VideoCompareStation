# Documentation

VCStation documentation is grouped by lifecycle and audience:

- [architecture.md](architecture.md), [architecture/](architecture/), and [adr/](adr/) define
  current architecture, dependency rules, state ownership, and accepted decisions.
- [engineering/](engineering/) contains active engineering contracts and diagnostics. Start with
  [maintenance and performance](engineering/maintenance-and-performance.md) for the current
  cleanup and validation boundary, [the behavior baseline](engineering/behavior-baseline.md) for
  asynchronous playback rules, and [the trace schema](engineering/trace-schema.md) for the
  implemented diagnostic format and its current limitations.
- [plans/playback-overhaul/](plans/playback-overhaul/) contains the active playback overhaul plan
  and its independent review reports.
- [releases/](releases/) records shipped behavior and release-specific validation.
- [design/](design/) preserves the historical A/B design overview; the current target design lives
  in [architecture.md](architecture.md).
- [archive/](archive/) preserves superseded designs, implementation plans, and historical reviews;
  archived documents are not current requirements.

Topic guides such as [alignment.md](alignment.md), [media-support.md](media-support.md), and
[self-hosted-runner.md](self-hosted-runner.md) remain at this level for direct access.
