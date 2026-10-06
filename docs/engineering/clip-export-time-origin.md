# Clip export: frame-zero time origin

Date: 2026-10-06. Baseline: `267adca6114c912796465944a51468598b8f0762`.
Status: source change with focused cloud verification; Windows acceptance pending.

## Failure and fix

The canonical timeline starts at frame zero. The export writer previously returned raw container
PTS to the planner and interpreted its normalized range as raw container time. The existing
`h264_nonzero_start_64x48_30fps_12.mp4` fixture starts at 1 second. Its full 12-frame plan spans
0–400,000 microseconds, while the writer returned a keyframe at 1,000,000 microseconds, making
an ordinary export fail as an empty range.

The adapter now keeps the first video presentation timestamp as its origin. FFmpeg's
`AVStream::start_time` provides that value when known; otherwise a cancellable, constant-memory
packet scan finds the minimum video PTS and rewinds. This fallback runs only on the export
worker. It does not decode or change playback's frame identity.

- Keyframe times and the reported first frame are normalized before crossing the application port
- Seek and exclusive-end bounds add the origin back in the selected video stream's time base
- Progress subtracts that same origin; a missing packet timestamp falls back to stream ticks
- Output PTS/DTS rebasing and the existing GOP pre-roll/reference-frame behavior are unchanged

No architecture rewrite, re-encoding, audio/subtitle handling, rendering change, or new runtime
dependency is included. The separate VFR UI eligibility bug and replace-existing-file transaction
problem remain open.

## Focused verification

The cloud host is Linux, with GCC 14 and FFmpeg 7.1.5. Public headers from FFmpeg tag `n7.1.5`
and GoogleTest tag `v1.17.0` were used to compile the unchanged production translation units
directly with `-std=c++20 -Wall -Wextra -Werror`. These dependencies are verification-only and
are not added to the repository or shipped product.

1. Existing planner tests: **7/7 passed**
2. New parameterized media tests: **3/3 passed**, covering ranges 0–4, 3–7, and 8–11
3. Replacing the writer with unmodified baseline source: **all 3 new tests fail**
4. Independent production-writer driver: **9/9 passed** across zero-origin CFR, +1-second CFR,
   and VFR fixtures, each with first/middle/tail selections. All **78 exported decoded frames**
   matched their source's pixel hashes in presentation order. Expected counts are 5, 9, and 12;
   the middle selection must retain frame 8 as a reference for frame 7
5. The driver also exercised the actual unknown-origin fallback by clearing optional start-time
   metadata on the opened stream, and verified pre-cancellation preserved the existing output

The new media tests compare actual compressed packet identities and relative presentation times,
not merely the writer's reported count. Time differences are calculated in stream ticks before
rescaling, avoiding an artificial one-microsecond difference from separately rounded timestamps.
An initial test-helper rounding failure was fixed this way without relaxing the equality check.

## Independent review and remaining exporter limits

A separate rebuild and driver checked a 90-frame H.264 MP4 with GOP length 15 and frame rate
30,000/1,001. **21/21 exports** passed across seven ranges and presentation origins of zero,
+5 seconds, and +5.000133 seconds. Decoded selected-frame identities and output equivalence across
origins matched, including cuts at the next GOP boundary. **42/42 pre-copy and mid-copy
cancellations** preserved the exact existing target bytes and left no partial file. Evidence is in
`out/verification/review-export-origin/results.json`.

The review also confirmed two pre-existing limits, neither fixed nor hidden by this patch:

- On that multi-GOP MP4, keyframe lookup omits the final keyframe at frame 75. A selection late
  in the final GOP therefore pre-rolls farther than necessary. The unmodified writer reproduces
  it; selected frames remain present. The subsequent [indexed-keyframe correction](clip-export-final-keyframe.md)
  fixes the verified MP4 path separately, with its additional packet-I/O cost documented
- On generated MPEG-TS, backward seeking to the first presentation timestamp can skip the first
  GOP's keyframe and make export report an empty span. The unmodified writer reproduces this on
  the zero-origin control. Fixed-writer probes at zero, +5 and −5 seconds failed equivalently.
  This is not evidence of working negative-PTS or general MPEG-TS export support

The origin fix is therefore accepted only for the verified MP4 cases and internal fixture paths,
not as a blanket certification of every container or timestamp shape. The reviewer found no newly
introduced blocking regression in the scoped change.

## Assertion mutation evidence

Every new assertion site was exercised by a compiled runtime mutation. **13/13 were detected**:
12 production-code faults and one test-input guard fault. A compilation failure was not accepted
as detection. The mutation driver checks its expected count; removing an entry caused the
12-versus-13 guard to fail before mutation. Source bytes were restored and read back, then all
10 tests passed again.

| Assertion | Injected fault |
| --- | --- |
| Normalized keyframe list | Return raw PTS instead of subtracting the origin |
| Planning succeeds | Reject the valid positive frame count |
| Alignment succeeds | Reject a nonempty keyframe list |
| Export completes | Omit the origin from the exclusive-end boundary |
| Reported first presentation is zero | Return the raw first presentation timestamp |
| Fixture contains 12 packets | Remove one item from the test's inspected fixture data |
| Output packet identities/timing match | Extend the end boundary by 100 ms |
| Reported packet count matches | Increment the reported count twice |
| Intermediate progress exists | Disable the packet progress callback |
| Initial progress is zero | Add 0.1 to progress |
| Intermediate progress advances | Report zero for every packet |
| Intermediate progress is below one | Report one after the first packet |
| Completion progress is one | Report 0.9 on completion |

Local logs, mutation results, baseline copies, driver sources and decoded outputs are under
`out/verification/export-origin/`; they are verification artifacts, not a second checked-in suite.
Permanent regression tests are in `tests/component/media/ClipExportOriginTests.cpp` and registered
in the existing media component target.

## Required Windows follow-through

Run the repository-supported wrapper on its configured Windows toolchain:

```powershell
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex '^(application.ClipExportPlannerTests|media.(ClipExportWriterTests|FirstMiddleLast/ClipExportOriginTests))'
pwsh tools/build/build.ps1 -Preset dev -Target format-check
pwsh tools/build/build.ps1 -Preset dev -Target lint
```

The project pins FFmpeg 8.1.2 and Qt 6.11.1. Windows/MSVC compilation, that pinned dependency
combination, full CTest, lint/format targets, GUI interaction, GPU/performance gates and a release
ZIP were **not run** here. No GitHub Actions run was started or rerun for this verification.
The patch should remain a draft until the remaining platform acceptance is complete.
