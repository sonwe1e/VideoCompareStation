# Maintenance and Performance Notes

This note records the maintenance and hot-path changes in the current work and, just as
importantly, the boundary of the evidence available for them. It is not a claim that the full
playback roadmap or the hardware performance gates have completed.

## Structural maintenance

Repeated test and gate plumbing has been consolidated without moving product responsibilities
across architecture boundaries:

- `tests/support/include/dvs/test/ScopedTemporaryDirectory.h` provides the one owned temporary
  directory implementation used by persistence, platform, and shell tests. The
  `dvs::test_support` interface target exposes it only to test targets.
- `tools/testing/PlaybackTraceGate.psm1` owns playback-gate process launch, environment
  restoration, process affinity/priority inheritance, exit-code capture, and whole-file structural
  trace validation. The navigation and comparison-semantics scripts remain parameter-compatible
  thin entry points.
- `tools/testing/CommandLineArgument.psm1` owns the one CRT/`CommandLineToArgvW` command-line
  quoting implementation. The playback gates, the performance gate, and the popup-pixel smoke
  script all import it instead of carrying local copies, so quoting fixes apply everywhere at
  once.
- The shared trace helper restores the caller's prior `DVS_PLAYBACK_TRACE` value, including the
  distinction between an unset variable and a set variable. Its validation requires a versioned
  header, parses every record, requires at least one event, and fails closed on overflow; it is not
  a semantic trace analyzer.

These helpers remove duplicated lifecycle code while keeping runtime code out of the test and
tooling layers.

## `SourceFrameCache` lookup path

`SourceFrameCache` retains its byte-bounded least-recently-used list and now pairs it with an
`std::unordered_map` from the complete cache key to the corresponding list iterator. Lookup,
promotion, replacement, and index removal are therefore average-case `O(1)` operations
instead of scanning the list. This is not a worst-case complexity guarantee for a hash table.

The key still includes the source fingerprint, source frame, pixel format, normalized width,
and normalized height. Promotion still moves a hit to the most-recently-used end, replacement
updates byte accounting, and eviction removes both the list entry and its index entry. The cache
remains actor-owned and intentionally has no internal synchronization.

`SourceFrameCacheTests` covers key separation, hit promotion and eviction, replacement and byte
accounting, oversize/zero-capacity behavior, clear, and an index rehash/splice stress case. These
tests establish cache semantics; they do not measure decoder throughput or target-GPU behavior.

## GUI projection filesystem checks

The immutable snapshot projection can be published at display cadence. Source `exists`, size, and
last-modified checks now run as generation-scoped background tasks, with at most one current
request for the active comparison and a configured 250 ms minimum launch interval. A revision can
start its task while a stale task from the prior comparison finishes. A single-shot scheduler keeps
polling in event-driven/idle mode and retries a failed probe. The GUI thread reads only cached
results keyed by normalized source path; it neither waits for the task nor performs those
filesystem calls. Workers publish plain data to a shared mailbox and never dereference the
controller. A comparison revision invalidates the cache and generation so a late result cannot
update the replacement session, and a task may safely finish after controller destruction.
`ReviewControllerTests` covers eventual local-file detection, event-driven idle polling and retry,
late-generation rejection, post-destruction completion, and the path-only fallback when an adapter
omits frozen identity.

The launch interval is not a completion deadline. A global-thread-pool queue or slow filesystem,
especially a network path, can delay detection; the current contract is eventual status reporting,
not bounded detection latency. Once the controller stops, runtime shutdown waits for any active
metadata probe within the shared seven-second shutdown budget. If a filesystem call remains stuck,
shutdown returns false and the executable uses its fail-closed immediate-termination path rather
than enter normal Qt/CRT static destruction with a global-pool task still active.

## Playback-trace hot path

Playback tracing remains disabled by default. In the disabled case, the global record path reads
the installed clock atomically and returns when no clock is installed. When enabled:

- multiple producer threads submit to a fixed-capacity queue;
- a producer uses a mutex `try_lock` and never waits for the queue lock;
- contention and full-queue conditions drop the event and increment an atomic overflow count;
- the shutdown consumer copies bounded batches from the queue, releases the queue lock, and only
  then performs sink I/O; and
- failed sink writes conservatively add the affected dequeued events to the loss count; and
- successfully reported loss is emitted as a JSON overflow marker.

This is a bounded MPSC queue with non-blocking producer attempts, not a lock-free queue. The
runtime disables tracing and drains it after playback producers stop during orderly shutdown.
The file sink batches bytes into a fixed 64 KiB buffer, flushes and closes a same-directory
transaction, and only then atomically publishes the final JSONL path. A finalize failure leaves the
capture unavailable instead of exposing a partial new output, so the gate fails closed. There is no
periodic streaming drain, and abnormal process termination can leave the new trace missing (or
retain a prior target). See
[the trace schema](trace-schema.md) for the exact format and gate contract.

`PlaybackTraceTests` covers bounded overflow reporting, sink-failure accounting, producer progress
while sink I/O is blocked, safe sink removal during a drain, and the disabled/enabled global path.
`TraceSinkTests` covers the JSON header, identity fields (including `dev`), event and overflow
records, Unicode output paths, and the bounded memory sink. `PlaybackTraceGateTests.ps1` covers
header/event types, uint64 boundaries, event-kind ranges, nullable commands, malformed input, and
overflow rejection.

## Verification commands

Run the relevant code checks from the repository root:

```powershell
cmake --preset dev
cmake --build --preset dev
ctest --preset dev -R "PlaybackTraceTests|TraceSinkTests|SourceFrameCacheTests" --output-on-failure
ctest --preset dev --output-on-failure
cmake --build --preset dev --target format-check
cmake --build --preset dev --target lint
```

On this workstation, the first command needs the environment overrides from the
[local build environment notes](#local-build-environment-notes-this-workstation) below; the plain
preset would trigger a package re-validation that is destructive here.

The two playback gate entry points are:

```powershell
.\tools\testing\run-navigation-gate.ps1 `
    -Executable .\out\build\dev\bin\VCStation.exe `
    -Fixtures @('D:\media\a.mp4', 'D:\media\b.mp4')
.\tools\testing\run-comparison-semantics-gate.ps1 `
    -Executable .\out\build\dev\bin\VCStation.exe `
    -Fixtures @('D:\media\a.mp4', 'D:\media\b.mp4')
```

They propagate the child result and reject a missing, malformed, empty, or overflowed trace.
Passing either script does not prove the three asynchronous invariants described in the trace
schema.

## Local build environment notes (this workstation)

The checkout lives at `I:\WorkStations\Toy` on an exFAT external drive whose object paths for a
vcpkg qtdeclarative rebuild exceed 260 characters; `cl.exe` then fails with
`fatal error C1083: cannot open compiler-generated file: ""` (the object path is 261 chars).
This is a machine property, not a project defect: the CI runner path is short.

- The Qt 6.11.1 installed tree (including the completed qtdeclarative package) lives at
  `out\vcpkg\x64-windows`; `out\vcpkg\vcpkg\status` records it as installed. Do not run a plain
  `cmake --preset dev` here: its default manifest install re-validates packages with whatever
  `VCPKG_ROOT` is active, and a different vcpkg version can remove the installed qtdeclarative
  before a doomed rebuild. Configure with the standalone clone used for the dependency builds:
  `VCPKG_ROOT=I:\WorkStations\vcpkg`, `-DVCPKG_MANIFEST_INSTALL=OFF`.
- If qtdeclarative is ever missing again, the completed staged package from the last full build is
  at `I:\WorkStations\vcpkg\packages\qtdeclarative_x64-windows`; copying its contents into
  `out\vcpkg\x64-windows` restores the install (Qt 6 CMake configs are relocatable). Rebuilding it
  requires a short `VCPKG_INSTALLED_DIR` (for example `I:\WorkStations\Toy\vd`) so object paths
  stay under 260 characters.
- The machine PATH contains `C:\msys64\ucrt64\bin` (GNU toolchain); cmake must never see it, or it
  picks `g++` and the project's MSVC check fails. Do the PATH surgery in PowerShell before calling
  `cmd /c`: cmd's `set PATH=...;%PATH%` line exceeds its 8191-character limit with the full user
  PATH and fails silently.
- `cmake --build --preset dev` must run with `clang-format`/`clang-tidy` available
  (`...\BuildTools\VC\Tools\Llvm\x64\bin`); the `dev` preset's quality targets require them.

## Outstanding evidence and follow-up

The following work remains open or requires the intended Windows/D3D11VA runner:

- Stale-arrival analysis now compares optional incoming identity (`is`/`ie`/`igen`/`idev`/`ireq`)
  against the live identity and fails a published stale `FrameSetReady`. Further hardening would
  associate provider terminals and commits with the exact request id that produced them.
- Decide separately whether crash-resilient or continuously streamed traces are required. The
  current shutdown-drained diagnostic was not designed to provide either property.
- Measure tracing enabled versus disabled under contention. Loss is observable through overflow
  markers, but the tests do not establish an acceptable hardware-run loss rate or tracing cost.
- On the interactive-desktop D3D11VA runner with a physical 120 Hz display, run the active
  `hardware-d3d11` and `performance-d3d11` presets described in
  [the runner guide](../self-hosted-runner.md). The matrix includes five-minute 1080p60 and
  60-second 1080p120 profiles. For the 2--3 source 1080p60 evidence, exclude the first two seconds
  and seek/pause intervals and verify no partial/split frame-set publication, set-level drop rate
  (at most 0.5%), seek P95 (at most 500 ms), UI response (at most 100 ms), and decoded-frame cache
  use (at most 256 MiB).
- Confirm with target-hardware measurements that the cache index reduces lookup cost without
  regressing decode, memory, or frame-set behavior. Unit/component coverage alone is not that
  performance evidence.
