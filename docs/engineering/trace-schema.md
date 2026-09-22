# Playback Trace Schema

The playback trace is a versioned diagnostic JSON Lines file. Multiple playback producers submit
events to one bounded in-process buffer; the runtime exports the buffer to one sink during
orderly shutdown. The schema records identity fields for invariant analysis, and the Phase 0
analyzer checks the invariants that the current emission contract can prove. The emission
contract alone is still not a complete proof of every asynchronous identity rejection.

The navigation and comparison-semantics gate scripts validate that the child process succeeds,
produces a structurally valid, fully parseable schema-v1 trace with a header, at least one event,
and no overflow marker, and then run the Phase 0 semantic analyzer (`Test-PlaybackTraceInvariants`).
The analyzer checks command exactly-once terminal delivery, acknowledgment-before-commit for
displayed frames, identity-regression stale commits, and publication of stale async arrivals.

## Enabling and file lifecycle

Tracing is disabled unless the process environment contains `DVS_PLAYBACK_TRACE`. Its value is
the output JSONL path. For example:

```powershell
$env:DVS_PLAYBACK_TRACE = Join-Path $PWD "out\playback-trace.jsonl"
.\out\build\dev\bin\VCStation.exe
Remove-Item Env:DVS_PLAYBACK_TRACE
```

The scripts under `tools/testing` set this variable for the child process and restore the
caller's previous value afterward. There is no `--playback-trace` command-line option.

During normal runtime shutdown, playback producers are stopped first, tracing is disabled, and
the remaining buffer is drained to a same-directory temporary file. The sink flushes and closes
that transaction before atomically publishing the configured path. A write, flush, close, or
publish failure leaves the capture unavailable and never exposes the partial new trace at the final
path; the gate consequently fails closed. A pre-existing target normally remains unchanged until
replacement succeeds. In the rare
unrecoverable partial-replacement case, the publisher preserves the previous target and replacement
as explicit same-directory recovery artifacts rather than silently losing either copy; the final
path may then be absent and the gate fails closed.

The sink is not continuously streamed and a crash or forced termination may therefore leave the
new trace missing (or preserve the previous target), rather than publish the in-progress partial.
If no event or overflow record is drained, the lazily opened transaction is not created.

## JSON Lines format

The first written line is the version header:

```json
{"traceVersion":1}
```

Each normal event is one compact JSON object:

```json
{"t":123456789,"kind":7,"s":42,"e":3,"topo":8,"tl":15,"al":2,"gen":9,"dev":4,"req":27,"cmd":91,"p":1200}
```

| Field | Meaning |
| --- | --- |
| `t` | Monotonic process-local timestamp in microseconds. |
| `kind` | Numeric event kind from the table below. |
| `s` | Session identifier. |
| `e` | Session epoch. |
| `topo` | Topology revision. |
| `tl` | Timeline revision. |
| `al` | Alignment revision. |
| `gen` | Playback generation. |
| `dev` | Device generation. |
| `req` | Provider request identifier. |
| `cmd` | Command identifier, or JSON `null` when the event is not command-scoped. |
| `p` | Event-specific payload interpreted according to `kind`. |

Optional additive incoming-identity fields (present together or absent together) record the
async request context the producer carried at arrival time. Topology/timeline/alignment are
coordinator-owned and therefore are not repeated here.

| Field | Meaning |
| --- | --- |
| `is` | Incoming session identifier. |
| `ie` | Incoming session epoch. |
| `igen` | Incoming playback generation. |
| `idev` | Incoming device generation. |
| `ireq` | Incoming provider request identifier. |

These fields are currently emitted on `FrameSetReady`, `PresentationAcknowledged`, and
`ProviderTerminal` when the coordinator can extract a request/playback/frame context. A mismatch
between the incoming tuple and the live `(s,e,gen,dev)` marks a stale arrival. `ireq` is not
compared because coordinator-owned live `req` is always `0`.

The timestamp is useful for elapsed-time measurements, but it is not a global wall clock. Queue
serialization determines file order. Because producers obtain timestamps before attempting to
enqueue, timestamps from different threads must not be treated as a stronger total ordering than
the trace line order.

An overflow report is a separate JSON object:

```json
{"overflow":17}
```

Its value is the number of previously unreported events lost to queue-lock contention, a full
queue, or sink/export failure after dequeue. For a failed batch export, the count conservatively
includes the failed event and the remaining unattempted suffix of that batch. More than one marker
may occur across drains. Any analyzer that uses the trace as proof must fail closed when it
encounters an overflow marker because the missing events may affect the result. If the sink cannot
write the overflow marker itself, the in-process count remains unreported and the resulting file
must not be treated as complete.

## Event kinds

| Value | Name | Payload |
| --- | --- | --- |
| `0` | `CommandAccepted` | `0`. |
| `1` | `CommandRejected` | `CommandOutcome` value. |
| `2` | `ProviderSubmitted` | Request priority. Reserved; not currently emitted. |
| `3` | `ProviderCanceled` | `CancellationReason` value. Reserved; not currently emitted. |
| `4` | `FrameSetReady` | Completed canonical position. |
| `5` | `ProviderTerminal` | `RequestTerminal` variant index. |
| `6` | `RenderPublished` | Published canonical position. |
| `7` | `PresentationAcknowledged` | Presented canonical position. |
| `8` | `SnapshotCommitted` | Displayed canonical position, or `UINT64_MAX` when absent. |
| `9` | `CommandTerminal` | `CommandOutcome` value. |
| `10` | `DecoderSeek` | Seek target position. |
| `11` | `DecoderReopen` | Frame id whose request forced the reopen. Emitted by the source decode actor when a decoder is reopened before a request; the identity `req` carries the source id. A healthy navigation pattern emits none, because a canceled decode is a clean stop rather than corruption. |
| `12` | `CacheHit` | Cached source-frame index. |
| `13` | `DeviceGenerationChanged` | New device generation. |
| `14` | `QmlGrabRequested` | Canonical frame the UI was showing when a scene-graph image grab was requested. |
| `15` | `QmlGrabCompleted` | Same frame payload when that grab delivered its result. |
| `16` | `RenderDrawStarted` | Canonical frame the render thread began drawing. |
| `17` | `RenderAckPublished` | Canonical frame whose presentation acknowledgement was admitted to the ack mailbox. |
| `18` | `PlaybackRunStarted` | First target frame of a continuous playback run. |
| `19` | `PlaybackRunStopped` | Last committed frame when the run ended, or `UINT64_MAX` when none committed. |
| `20` | `ReverseWindowBuilt` | Frames retained in the built reverse GOP window (ADR-003). |
| `21` | `ReverseWindowHit` | Reverse target frame id served from the built window. |
| `22` | `ReverseExactFallback` | Reverse target frame id that fell back to exact decode. |

`PlaybackRunStarted`/`PlaybackRunStopped` bound exactly one continuous playback run (`play` to
pause/end/stop). One trace also contains the open, seek and step phases, and those phases commit
frames too, so an analyzer that compares display intervals must isolate the bounded running window
before attributing a stall to continuous playback. A run that is stopped by an error path still
emits `PlaybackRunStopped`, so an unmatched start means the capture was truncated.

`ReverseWindowBuilt`/`ReverseWindowHit`/`ReverseExactFallback` (ADR-003) observe reverse GOP
window construction and reuse. `Built` payload is the retained frame count; `Hit`/`Fallback`
payload is the reverse target frame id so gates can correlate seeks with window state.

`QmlGrabRequested`/`QmlGrabCompleted` are UI-originated observation events emitted through the QML
`dvsDiagnostics` bridge (currently the timeline thumbnail cache's `grabToImage`). They carry no
session/playback identity — the identity tuple is all zeros — so they must be correlated with the
pipeline events by timestamp, never by identity. The pair bounds how long a grab's result was
outstanding and lets an analyzer line grabs up against the frames that were late. Correlation alone
is not causation: the T5 gate A/B measured the same stall profile with every playback-time grab
suppressed, so a grab that merely overlaps a late frame is not evidence that it caused the delay.
Payload `UINT64_MAX` means the UI did not know a frame number when the event was recorded.

`RenderDrawStarted`/`RenderAckPublished` are renderer-side observation events emitted from
`D3d11ComparisonRenderer` on the render thread, with the canonical frame id as the payload. They
also carry no playback identity. Joined with `RenderPublished` (kind 6) and
`PresentationAcknowledged` (kind 7) they bisect a late frame's presentation:

| hop | meaning when it dominates a long display interval |
| --- | --- |
| `RenderPublished` → `RenderDrawStarted` | the scene graph never scheduled a render for an already-published frame (window/render-loop scheduling) |
| `RenderDrawStarted` → `RenderAckPublished` | the draw itself was slow (GPU or device contention) |
| `RenderAckPublished` → `PresentationAcknowledged` | the relay thread was late |
| `FrameSetReady` → `RenderPublished` | the producer or the coordinator was late |

Because these events come from different identity scopes, an analyzer must key them by frame id
within a single playback run rather than by the identity tuple.

`CommandAccepted` currently means that a command was nonduplicate and claimed by the coordinator;
it is emitted before the remaining admission checks. A rejected claimed command may therefore
produce `CommandAccepted`, `CommandRejected`, and exactly one `CommandTerminal` record.

Numeric values are append-only. Do not reorder or reuse them within schema version 1. The
defined schema-v1 range is `0`–`22` (`kSchemaV1MaxTraceEventKind` in `PlaybackTrace.h` and
`SchemaV1MaxKind` in `PlaybackTraceGate.psm1`); unknown kinds remain fail-closed. Additive
event fields may be introduced without changing the version; a semantic change to an existing
field or event requires a new trace version and analyzer support for both versions. The version
header remains exactly the one-field object shown above.

## Buffer and export behavior

The implementation is a 65,536-event fixed-capacity MPSC queue guarded by a mutex:

- playback coordination and source-decode actors may both be producers;
- producers serialize briefly on the queue lock for a single-event copy (nanoseconds) and never
  touch the sink or perform I/O, so a short wait cannot stall playback;
- the queue drops only when its fixed capacity is exhausted; 64K fills in under 20 s only at
  three 60 fps sources with pathological event rates, so an overflow marker is a genuine anomaly
  rather than the norm for evidence-length traces;
- lost-event accounting is atomic; and
- the single shutdown consumer copies batches of at most 256 events, releases the mutex, and
  performs sink I/O outside the producer critical section;
- the file sink batches JSONL bytes into bounded 64 KiB writes; and
- failed sink writes are counted as lost, while finalize/transaction failures prevent publication
  so the trace gate fails closed without misclassifying the runtime as a shutdown timeout.

This design bounds producer delay and storage, but it is not lock-free and it does not guarantee
a lossless trace. An overflow marker is the explicit loss signal.

## Analyzer contract and remaining gaps

`Test-PlaybackTraceInvariants` in `tools/testing/PlaybackTraceGate.psm1` uses the identity tuple
`(s, e, topo, tl, al, gen, dev)` plus `cmd` and event payloads to check:

1. every `CommandAccepted` with a non-null `cmd` reaches exactly one `CommandTerminal` for the
   same `(s, e, cmd)`; orphans, duplicates, and accepts without a command id fail closed;
2. every `SnapshotCommitted` with a displayed-frame payload has a prior `PresentationAcknowledged`
   with the same payload and the same identity excluding `req`/`cmd`; `UINT64_MAX` means no
   displayed frame and skips the ACK requirement; republishing a still-acked frame is allowed;
3. a `SnapshotCommitted` whose `gen`/`topo`/`tl`/`dev` is lower than a higher value already
   observed for the same `(s, e)` is treated as a stale commit; and
4. a `FrameSetReady` whose optional incoming identity differs from the live `(s,e,gen,dev)`
   marks that frame payload stale; a subsequent `RenderPublished` for the same payload fails as
   `STALE_ARRIVAL_PUBLISHED`. Stale arrivals that are dropped (followed by a fresh ready) pass.
   Live `req` is coordinator-owned and remains `0`, so `ireq` is exported for correlation but is
   not part of the stale comparison.

The coordinator suppresses a `SnapshotCommitted` whose displayed canonical position is unchanged
since the previous commit (the canvas state publication still fires on every notify). A
re-commit of an already-acked frame after the generation advanced would otherwise be
unrepresentable in the contract: the newer identity has no ACK for the older frame (rule 2) and
the older identity is stale once a higher revision was observed (rule 3).

Overflow markers remain fail-closed. `PartialFrameSetCount` is reported as `0` because the
single-line event stream cannot reconstruct multi-source FrameSet shape; structural FrameSet
atomicity is enforced by the C++ factory, not this analyzer.

Incoming identity is only as complete as the `EventContext` on the arrival. `PlaybackRequestContext`
arrivals leave `idev` as `0`, and `RequestContext` arrivals leave `igen`/`idev` as `0`; the
analyzer still compares those zeros against the live identity, so producers that lack device or
generation in their context must not be treated as carrying a full frame identity.

## Current validation coverage

- `PlaybackTraceTests` exercises the bounded queue, one-time overflow reporting per lost batch,
  sink failure accounting, sink I/O outside the queue lock, sink removal as an ownership fence,
  and the disabled/enabled global record path.
- `TraceSinkTests` exercises the file header, every identity field including `dev`, normal events,
  overflow JSON, Unicode paths, and bounded memory-sink behavior.
- `PlaybackTraceGate.psm1` parses every nonblank trace line, validates the schema-v1 numeric types
  and ranges, rejects malformed records and any overflow marker, runs the Phase 0 invariant
  analyzer, and propagates the child process result. `PlaybackTraceGateTests.ps1` locks down the
  structural boundary, uint64 edge cases, and the analyzer's happy path plus
  terminal/ACK/stale failure modes.

For the wider performance and hardware-validation boundary, see
[Maintenance and Performance Notes](maintenance-and-performance.md).
