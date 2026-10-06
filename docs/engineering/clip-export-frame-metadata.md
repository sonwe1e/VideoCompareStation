# Clip export: report rounded frame starts accurately

Date: 2026-10-06. Baseline: draft PR #34 head
`d6a5f4e6d722390c261790425e95e588f505ac9d`.
Status: focused cloud verification; Windows and release acceptance remain pending.

## Reproduction and scope

At 24000/1001 fps, the exact start of frame 16 is 667333⅓ microseconds. FFmpeg's packet
conversion reports 667333, while the canonical timeline deliberately uses ceiling boundaries
and maps that time to frame 15. The exported bytes already start at frame 16, but
`ClipExportPlan::firstExportedFrame` names frame 15. The same mismatch occurs at 30000/1001,
60000/1001 and some integer frame rates whose frame periods are fractional microseconds.

This correction is to planner metadata. The current Qt completion text does not consume
`firstExportedFrame`, so this is not a visible UI-label change. It does not change requested
ranges, keyframe selection, actual start/end timestamps, packet copying, B-frame references,
progress, frame-zero normalization or transactional publication.

`RationalRate::frameStartTimeRounded` uses the existing checked integer arithmetic to compute
the nearest whole microsecond, rounding exact halves upward. It matches the nonnegative
presentation-time convention of [FFmpeg's av_rescale_q](https://ffmpeg.org/doxygen/7.1/mathematics_8c_source.html).
The canonical `frameStartTime` ceiling and `frameAtOrBefore` floor contracts remain unchanged.
The planner promotes the following frame only when its rounded start exactly equals the
selected keyframe timestamp. Arbitrary times before a boundary retain their preceding frame.
Variable-rate timelines keep their recorded-time lookup, and sub-microsecond frame periods
keep their prior mapping because a whole microsecond cannot uniquely identify such frames.
Invalid/unrepresentable conversions do not invent a frame identity.

## Durable regression

- Planner tests cover 24000/1001, 30000/1001, 60000/1001, equivalent reduced rates, 25/30/60 fps,
  exact-half rounding, zero and a two-billion-frame timestamp. They assert that selection,
  copy span and requested counts do not change
- Boundary controls include 25 fps at 39999 µs (still frame 0), the whole microsecond before a
  valid rounded timestamp, between-frame times, negative time, missing VFR data, recorded VFR
  boundaries, ambiguous sub-microsecond periods and checked overflow
- Domain tests preserve the existing ceiling/floor behavior and verify nearest conversion,
  invalid frames/rates and overflow handling
- Three parameterized real-FFmpeg remux tests exercise 36 exports at four rates and three
  origins (zero, five seconds, and a fractional tick offset). The fixture repeats the existing
  independent IDR packet, requiring no encoder or extra binary fixture. Its frames intentionally
  have identical pixels; packet timestamps establish the ordinal and bytes verify stream copy
- The source remains in the normal repository suites, under `tests/unit/domain`,
  `tests/unit/application` and `tests/component/media`

The original production planner fails seven rounded-boundary subcases. Final GCC14 C++20
builds with `-O2 -Wall -Wextra -Werror`, FFmpeg 7.1.5 and GoogleTest 1.17.0 pass **56/56**
focused tests. This includes prior origin/keyframe tests and the unchanged publication tests
using a POSIX-backed Win32 API model; four native-Windows-only cases are explicitly excluded.

Independent production-linked checks cover **42 candidate and 42 baseline exports** with
actual distinct decoded-frame identities, compressed packet hashes and complete file bytes.
All 42 candidate outputs are byte-identical to baseline; baseline has 12 metadata mismatches,
while the candidate has none. This is direct evidence that this change corrects the reported
frame without changing exported content. Independent UBSan arithmetic/boundary probes add
753 mapping assertions and 152 direct rounded-conversion assertions.

## Boundaries and required acceptance

This is a nearest-microsecond rounding correction, not a general VFR or coarse-clock frame
indexer. Container timestamps quantized more coarsely than microseconds can still map to a
preceding canonical frame. Missing/lazy keyframe indexes and the existing MPEG-TS initial-seek
export failure remain separate; successful key enumeration does not certify negative-origin
TS export. Existing source-origin and atomic publication fixes are preserved byte-for-byte.

No Windows/MSVC, pinned FFmpeg 8.1.2/Qt 6.11.1, full CTest, native file-lock/Unicode/durability,
GUI, format-check/lint targets, GPU performance or packaging acceptance ran in the Linux cloud.
A Win32 API model is not native Windows verification. No workflow change or CI rerun is needed.
The PR remains draft.

Required supported-environment commands include:

```powershell
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex '^(domain.RationalRateTests|application.ClipExportPlannerTests|media.(ClipExportWriterTests|FirstMiddleLast/ClipExportOriginTests))'
pwsh tools/build/build.ps1 -Preset dev -Target format-check
pwsh tools/build/build.ps1 -Preset dev -Target lint
```
