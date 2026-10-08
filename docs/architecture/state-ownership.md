# Review state ownership

> **Status note.** The table describes logical responsibility areas, not separate implemented
> view-model classes. `ReviewSessionFacade` currently exposes only `playback`, `comparison`, and
> `shell`; see the concrete controller mapping below.

The video review pipeline keeps one immutable `SessionSnapshot` as media truth. `ReviewController`
projects that snapshot rather than querying decoder or renderer workers directly. Image review keeps
its separate `ImageReviewController` / `ImagePairLoader` path.

| State | Logical responsibility area | Notes |
| --- | --- | --- |
| Sources, reference, generation, source intents | Session | Source mutations are serialized and identity based. |
| Presented/requested frame, play/pause, seek, range | Playback | Visible frame changes only after render ACK. |
| Offsets, anchors, analysis, markers | Alignment | Async results retain full request identity. |
| View mode, pair, difference, wipe, ROI | Comparison | Values use `dvs::presentation` contract types. |
| Errors, overlays, toast, changed-on-disk | Notifications | Technical details remain separate from message keys. |
| Chrome, inspector, input context, pending action | Shell | Window visibility/fullscreen remain in QML. |
| Decoder, frame resources, validated media | Application snapshot and adapters | QML never owns these objects. |

The current [facade](../../src/ui_qml/include/dvs/ui/ReviewSessionFacade.h) forwards `playback` to
`ReviewController`, `comparison` to `ReviewPreferencesController`, and `shell` to
`ReviewShellController`; it does not own another state projection. Session, alignment, and
notification responsibilities above are not additional facade properties. Add a narrower property
only together with its first actual reader, rather than restoring unused migration interfaces.
