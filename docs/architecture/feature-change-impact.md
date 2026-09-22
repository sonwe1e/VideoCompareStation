# Feature change impact

Use this map to keep future feature work inside the smallest stable capability boundary.

For actual file/test entry points use the [agent guide](../agent-guide.md); current product
gaps and acceptance conditions live in the [issue ledger](../engineering/visual-review-backlog.md).

| Feature | Primary implementation boundary | Presentation boundary | Runtime impact |
| --- | --- | --- | --- |
| Playback speed | Playback workflow and cadence | Playback capability | Add a clock policy |
| Compare mode | Contract descriptor table and renderer | Comparison capability | None |
| Evidence export | Export use case and output port | Notification capability | Adapter owned by composition |
| Alignment algorithm | Alignment workflow and analysis port | Alignment capability | New worker only if required |
| Renderer | Render-channel port and render bridge | No UI-model dependency | Replace graphics composition |
| Setting | Typed value and repository mapping | Owning capability only | None |
| Media format | FFmpeg probe/decode/cache internals | Media information only | None |

Cross-cutting changes must retain complete FrameSet publication and the full
session/epoch/generation/request identity. A feature must not add another coordinator event loop
or let a UI model query an adapter worker directly.
