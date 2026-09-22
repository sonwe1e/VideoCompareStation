# Architecture Overview

> **Status: historical.** Describes the A/B pair contract of the original dual-video
> review slices. The current target design lives in [../architecture.md](../architecture.md).

CompareStation is a Windows-only C++20 modular monolith. The dependency allow-list is explicit: `application` uses `domain`; `platform_windows` implements application ports; outer adapters may use both application contracts and platform services; the composition root selects concrete modules. Platform and adapter types never enter the core.

The playback contract is frame-based and atomic: a render update always contains both A and B for the same canonical `FrameId`. Original-media decoding is available first, while a low-priority merged proxy is prepared in the background. Provider changes occur only through the coordinator's safe-boundary handshake.

Qt Quick owns presentation, FFmpeg owns media decoding and child-process transforms, and `platform_windows` owns D3D11 resources, process supervision, and atomic file publication. Framework/native types do not cross application ports. CompareStation manages only the current open 1–3 video session and does not persist projects or Bad Case captures. Settings and generated output writes remain transactional. Asynchronous work is scoped by session, playback, job, and device identities.

See `AGENTS.md` for the operational invariants and repository commands. The current product direction (session-only, no project persistence) is documented in [`architecture.md`](../architecture.md).
