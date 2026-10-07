# Clip export: resolve the final indexed keyframe

Date: 2026-10-06. Baseline: draft PR #34 head `76a17eb1b8671b2aabc10e597c43bab133e7aaa7`.
Status: focused cloud verification complete; Windows acceptance pending.

## Reproduction and narrow correction

A 90-frame H.264 MP4 at 30000/1001 fps has keyframes at frames 0, 15, 30, 45, 60 and 75.
The baseline production writer returns only the first five. Requesting frames 83–89 therefore
starts at frame 60 and writes 30 packets. With the correction it lists all six keyframes, starts
at frame 75 and writes 15 packets. The selected frames remain byte/pixel-identical to the source.

FFmpeg's MP4 index stores decode timestamps, while its seek interprets a presentation timestamp.
Seeking directly to an index timestamp can land one GOP too early. Accepting the first returned
packet shifts the table backward and loses the final GOP's keyframe.

The indexed lookup now copies the index position before seeking, then advances to a key packet
from the selected video stream at or after that byte position. Its actual presentation timestamp
is normalized using the existing frame-zero origin. Positions at or after the index entry are
accepted because a cluster-based index can point before its packet. A failed lookup returns an
empty table instead of inventing a keyframe presentation time from the index timestamp.

If a later seek moves behind the previous resolved video-key position, the lookup stops seeking
and finishes one forward packet scan to EOF. This handles imprecise/sparse seeks without
repeatedly rescanning a growing prefix or rejecting a playable source. Existing collected keys
are retained and the common sort/dedup runs once. Read errors and cancellation return an empty
table even during this fallback; they cannot publish a partial successful lookup.

Only keyframe discovery changes. The planner, copy loop, presentation origin, export clock,
B-frame end references, target replacement, UI and architecture are unchanged. There is no
new runtime dependency, decoding or re-encoding in the export path.

## Permanent regression and mutation checks

`tests/component/media/ClipExportOriginTests.cpp` now remuxes the existing 12-frame closed-GOP
fixture into one or six GOPs at 30000/1001 fps. No test encoder or ffmpeg CLI is required.
The helper preserves origins of 0, 150000 and 150004 ticks in a 1/30000 time base; the test
asserts the actual raw origin, including the fractional offset.

Three parameterized tests each exercise all six GOP-count/origin combinations: **18 exports**.
They verify the entire keyframe list and final-GOP first/middle/tail cuts, including an exact
keyframe start and frame endpoints requiring microsecond rounding. Packet identities and
presentation spacing, actual first presentation and packet counts must match. A middle cut
still includes the reference packet needed by frame 7 (9 packets, not 8).

- Baseline production source: **all 3 new parameterized tests fail**
- Final production source: **13/13 scoped GoogleTests pass** (7 existing planner, 3 existing
  origin, 3 new indexed-keyframe tests)
- **11/11 compiled runtime mutations** are detected at their intended new assertion sites:
  fixture creation, key list, plan, alignment, aligned start, export completion, actual first
  presentation, raw fixture origin, source packet count, output packet identity/spacing and
  output packet count
- The mutation-count guard rejects a deliberately missing mutation. Compile failures are not
  counted as detection. Mutations are built from separate source copies; final source bytes
  remain unchanged and the final 13-test suite passes again

The Linux cloud build uses GCC 14, actual FFmpeg 7.1.5 libraries, official matching 7.1.5 headers
and GoogleTest 1.17.0, with `-std=c++20 -Wall -Wextra -Werror`.

## Independent checks

The independent reviewer rebuilt the production writer and checked **74 export cases** plus
**16 cancellation/target-preservation cases**. Coverage includes zero/+5-second/fractional
origins, ordinary and fragmented MP4, an MP4 with audio listed before video, 24000/1001 fps
with GOP16/B3, 25 fps without B-frames and a one-GOP +1-second fixture. Key lists match
packet-derived key PTS; nearest starts and selected decoded pixel identities match. The
baseline fails 54 of the same cases, mostly at the key-list assertion.

Fourteen additional guarded probes cover positions just before packets, read failure and
cancellation, retaining already-collected keys, deduplication, and ignoring interleaved audio
positions. With every seek deliberately forced to the file prefix, the one-time fallback uses
108/918/9018/90018 reads for 90/900/9000/90000 frames, with only three seeks and every expected
keyframe retained. Normal MP4 still uses 81 reads/six seeks, so equality with the previous key
position does not trigger the fallback. Scan-mode read failure and cancellation both return an
empty table. These interposition probes do not substitute for real container tests. Detailed logs and harnesses are local under
`out/verification/final-keyframe/` and `out/verification/independent-review/`; the permanent
regression remains in the existing test suite.

## I/O cost and bounded performance sanity

The correction can read intervening compressed packets after a backward seek. It is **not
I/O-neutral**: the 90-frame/GOP15 probe makes 81 packet reads instead of 6. No frames are decoded,
work remains on the export worker, and cancellation is checked on every loop iteration.

A bounded cloud sanity check repeats that small 64×48 fixture to 5 and 10 minutes. Seven runs
per case give these median complete keyframe-lookup times:

| Synthetic file | Baseline | Corrected | Corrected packet reads | Maximum reads per seek |
| --- | ---: | ---: | ---: | ---: |
| 5 minutes / 600 keyframes | 10.5 ms | 15.0 ms | 9,585 | 16 |
| 10 minutes / 1,200 keyframes | 22.4 ms | 36.1 ms | 19,185 | 16 |

The read count scales linearly on these controls; it does not rescan from the start for every
keyframe. The baseline is faster but omits one keyframe in each file. Injecting cancellation at
read 100 or 1,000 returns an empty list without one further packet read. These tiny local files
are a sanity check, not high-bitrate, slow-storage or Windows performance acceptance.

## Remaining limits and required platform acceptance

- If the demuxer omits index or packet byte positions, the existing first-keyframe-after-seek
  fallback remains; the final-key omission can still occur in that case
- A lazy/partial Matroska index can still expose only the initial keyframe at open. This patch
  does not claim complete key discovery for such containers
- The previously reproduced MPEG-TS initial-seek export failure is unchanged. Correct TS key
  enumeration alone is not proof of successful TS or negative-origin export
- The planner has a pre-existing rounded frame-label issue at 24000/1001: the key at frame 16
  can report `firstExportedFrame=15` after microsecond conversion, although the actual export
  starts at frame 16. This separate reporting issue was unchanged in that patch; the subsequent
  [frame-metadata correction](clip-export-frame-metadata.md) covers nearest-microsecond rounding
- The VFR UI restriction remains separate. The target remove/rename failure window present at
  that patch's baseline is addressed by the later [publication repair](clip-export-publication.md)

Run the supported Windows wrapper before platform acceptance:

```powershell
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex '^(application.ClipExportPlannerTests|media.(ClipExportWriterTests|FirstMiddleLast/ClipExportOriginTests))'
pwsh tools/build/build.ps1 -Preset dev -Target format-check
pwsh tools/build/build.ps1 -Preset dev -Target lint
```

Windows/MSVC, the project's pinned FFmpeg 8.1.2/Qt 6.11.1 combination, full CTest, GUI,
format/lint targets, GPU/performance gates and packaging were **not run**. The PR stays draft.
No GitHub Actions workflow is changed, started or rerun; skipped CI is not a passed CI result.
