# Review state ownership

> **Status note.** The per-area view-model classes are not yet split out. The capability names in
> the table are exposed by `ReviewSessionFacade`, which currently backs each with an existing
> controller; see the migration note at the end of this page.

CompareStation keeps one immutable `SessionSnapshot` as media truth. UI objects project that snapshot;
they do not query decoder or renderer workers directly.

| State | Owner | Notes |
| --- | --- | --- |
| Sources, reference, generation, source intents | Session view model | Source mutations are serialized and identity based. |
| Presented/requested frame, play/pause, seek, range | Playback view model | Visible frame changes only after render ACK. |
| Offsets, anchors, analysis, markers | Alignment view model | Async results retain full request identity. |
| View mode, pair, difference, wipe, ROI | Comparison view model | Values use `dvs::presentation` contract types. |
| Errors, overlays, toast, changed-on-disk | Notification view model | Technical details remain separate from message keys. |
| Chrome, inspector, input context, pending action | Shell view model | Window visibility/fullscreen remain in QML. |
| Decoder, frame resources, validated media | Application snapshot and adapters | QML never owns these objects. |

The migration facade exposes these capability names now. Existing controllers remain temporary
forwarding implementations until their properties have moved behind narrow view-model classes.
