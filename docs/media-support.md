# Media Support

Video baseline: **working tree on `8c90005`**, reviewed 2026-10-17. The three implementation
stages below exist in code; actual performance and release claims require evidence for the
tested SHA. VP9, AV1 and HDR/tone mapping are outside the current playback gate. AV1/VP9 have
real investigation fixtures plus explicit-rejection tests; they remain candidates for MEDIA2,
not supported playback. Audio/subtitles are outside the current product scope, not planned
playback requirements.
See [product intent](product/visual-review.md) and the [issue ledger](engineering/visual-review-backlog.md).

## What opens today

The probe (`src/media_ffmpeg/src/MediaProbe.cpp`) accepts exactly:

| Dimension | Accepted | Rejected with |
|---|---|---|
| Codec | H.264, HEVC, MPEG-4 Part 2 (and a decoder must exist) | `kUnsupportedCodec` in `MediaProbe.cpp` |
| AV1 / VP9 | Not supported; real 12-frame 64×48 fixtures assert the explicit rejection | `kUnsupportedCodec`; `.webm` is outside the video-extension entry gate |
| Pixel format | 8/10-bit YUV 4:2:0/4:2:2/4:4:4 and 8-bit RGB | unsupported depth/layout → `kUnsupportedPixelFormat` |
| Transfer | SDR only — PQ (SMPTE2084) and HLG are rejected | `kUnsupportedPixelFormat` in transfer validation |
| Color matrix | BT.709, BT.601, RGB, Unspecified (normalized/inferred as needed) | unsupported matrix → `kUnsupportedPixelFormat` |
| Geometry | right-angle rotation and positive SAR | arbitrary rotation or invalid metadata → rejected |
| Decode | Software fallback plus D3D11VA when the codec and shared Qt D3D11 device support it | backend and fallback reason remain queryable per source |

The decoder re-verifies the decoded layout on every frame to catch changes
between probe and decode. Native 8-bit 4:2:0 becomes NV12; native 10-bit 4:2:0 becomes
P010 without reducing code depth. On D3D11VA, the same NV12/P010 decoder surface goes
straight to the renderer; aligned texture padding is excluded by the visible texture region.
YUV422/YUV444 and RGB are converted by the software fallback to the
semiplanar visual profile and are therefore never labelled source-plane exact.
Rotation is applied in the GPU sampler and SAR participates in aspect fitting.
Containers are whatever FFmpeg opens when the stream passes these gates. VFR,
non-zero start PTS, and B-frame display reordering use the display-order PTS index.

"Encoding format is not fixed" cannot be solved by widening a file-extension filter;
the gates above define the reviewed video baseline.

CompareStation does not decode or play audio. It uses video timestamps as the playback clock and is
presented in the UI as visual playback only.

## Still images and inspection fidelity

The application injects its image loader in `src/app/Main.cpp`; the main decoder is
`src/media_ffmpeg/src/StillImageDecoder.cpp`, with a Qt fallback in `ImagePairLoader.cpp`.
The separate stb implementation under `ui_qml` is not the application's complete support matrix.

| Capability | Verified 2026-10-17 on the dev build | Still not verified |
|---|---|---|
| PNG | Opens through the Qt fallback: this FFmpeg build disables zlib and therefore has **no PNG decoder**. Embedded ICC bytes are read by the container probe, so the colour state is real on this path | 16-bit PNG precision: the Qt fallback yields RGBA8 without the converted RGBA64 sidecar; a zlib-enabled FFmpeg build would need a dependency/payload decision |
| JPEG/BMP/GIF/WebP/PNM | Real encoded fixtures decode through the FFmpeg-backed entry (`media.StillImageDecoderTests.DecodesRealFormatsWithEnabledFfmpegDecoders`, `DecodesEveryRealPnmSubtype`); extension routing includes all families | Release-ZIP decoder availability and real user photographs/screenshots |
| TIFF | Decoder exists and metadata is readable, but this FFmpeg build has zlib disabled: a Deflate-compressed TIFF fails explicitly in the decoder (`DeflateTiffFailsExplicitlyInsteadOfClaimingSupport`). Uncompressed TIFF was not added as a fixture in this round | Any TIFF support claim until an uncompressed fixture passes and the compressed variants are either enabled (zlib) or listed as unsupported |
| PNM/PBM/PGM/PPM/PAM | Real P1–P7 files decode 8×8; routing and dialogs include `.pnm/.ppm/.pgm/.pbm/.pam` | Damaged/oversized PNM rejection and real world samples |
| Pixel representation | Decoded RGBA8 plus an RGBA64 sidecar for >8-bit sources decoded by FFmpeg; source depth/format/alpha provenance is recorded | Original-depth sampling for PNG through the Qt fallback |
| Alpha | Straight (unassociated); the alpha-edge fixture keeps non-zero RGB under alpha 0; exact raw bytes are verified by `tools/fixtures/verify-fixtures.py` | Background-composited appearance on a physical display |
| Color profiles | Embedded ICC bytes are captured, hashed and structurally classified (COLOR1/COLOR3). COLOR4 parses the SDR profiles with Qt and converts **only the display derivative**; raw buffers, statistics, probes and exports stay decoded values | COLOR5 Windows output and physical-display verification; 16-bit colour precision; per-format profile quirks beyond the committed samples |
| Animated/multipage files | Still-image workflow only; GIF/WebP/TIFF are read as one image | Animation, page selection or image-sequence playback are not supported |
| Entry consistency | A JPEG renamed to `.png` decodes by content sniffing (the decoder sees MJPEG bytes); PNG/JPEG/BMP/GIF/WebP/TIFF/PNM share one extension list | Drag/drop, Explorer and folder-entry parity on Windows real sessions |

All working-tree observations must be rechecked against the current diff. The committed fixture
manifests (`tests/fixtures/images/color/manifest.json`,
`tests/fixtures/images/formats/manifest.json`, `tests/fixtures/manifest.json`) record SHA-256,
generation method, bit depth, alpha and colour declarations; `tools/fixtures/verify-fixtures.py`
rechecks hashes, categories, ICC open/reject behavior, PNM dimensions, corrupt/fake-extension
behavior and AV1/VP9 probe facts. Use ledger I-01/I-02/I-03 for acceptance conditions.
Source numeric values, decoded RGBA8 samples and background-composited appearance are distinct.
Straight/premultiplied Alpha interpretation needs format-specific validation before making fidelity claims.

### MEDIA2 dependency decision (2026-10-17, not implemented)

The remaining format gaps are dependency choices, not missing code paths. The current
`vcpkg.json` selects only `avcodec`/`avformat`/`swscale` with default features off, which is why
this FFmpeg has no zlib, AV1 or VP9 decoder.

- **PNG / Deflate TIFF:** add the `zlib` feature to the FFmpeg dependency and rebuild. This would
  give the FFmpeg PNG decoder and Deflate TIFF support, and a 16-bit PNG path with the native
  decoder (still subject to the viewer's RGBA64 sidecar rules). It changes the packaged FFmpeg
  payload and must be followed by real PNG/TIFF decode tests, decoder-availability checks in the
  unpacked package, and the existing Qt-fallback tests re-run as a fallback path.
- **AV1:** add `aom` and/or `dav1d` (and possibly `svt-av1` for encode; decode needs only one) plus
  the extension gate and probe expectations. **VP9:** add `vpx`. Both grow the payload and need
  real decode fixtures (the committed `.mkv`/`.webm` samples currently only prove the explicit
  rejection gate).
- Because a rebuild is long and changes shipped dependencies, implement this on the independent
  test environment with the normal packaging gate, not as an ad-hoc local build. Until then the
  support matrix keeps saying "unsupported" and the app keeps rejecting AV1/VP9 explicitly.

## Still-image difference pipeline

The per-pair difference is split into one analysis and per-variant renders
(`ImagePairLoader::analyzeDifference` / `renderDifference`, 2026-09-25):

- The **analysis** reads both decoded RGBA8 display buffers once and produces a canonical delta
  field (exact per-channel magnitudes plus one sign byte) together with every statistic. It is
  cached per buffer identity and released when the committed pair changes.
- A **render** derives one display variant (absolute, signed, highlight, alpha) at the selected
  gain. Switching mode or gain re-renders from the field: it does not re-read the sources, does
  not convert formats again, and cannot change a statistic.
- The **gain** (1–16, default 4) is display-only. Peak, RGB MAE and alpha statistics are always
  raw 8-bit deltas of the display buffer.
- When both sides carry a same-size converted RGBA64 sidecar and no resampling occurred, the
  result also reports 16-bit statistics, including the pixels whose RGBA8 samples are equal while
  the 16-bit code values differ. Those values are **decoder-converted RGBA16 code values, not raw
  file planes**, and the readout says so. Resampled pairs get no pixel-level 16-bit statistics.
- Memory is accounted as the pair's whole working set (display buffers, sidecars, delta field,
  sign map, derived image, resample copy), not as one `width*height*4` buffer; the estimate and
  budget are reported in `asyncStats()`.

## Capability layering

Input support is split into three independently judged capabilities, replacing the
single whitelist:

1. **Container and decoder capability** — can FFmpeg demux and decode it at all.
2. **Pixel-format normalization capability** — current software output is converted
   to NV12 8-bit or P010 10-bit:

   ```text
   Decoder output
     ├── YUV420 8-bit ─────────────→ NV12 8-bit
     ├── YUV420 10-bit ────────────→ P010 10-bit
     ├── YUV422 / YUV444 ──────────→ NV12/P010 visual profile
     └── RGB ──────────────────────→ NV12 visual profile
   ```

3. **Render and difference capability** — can color space, bit depth, rotation, and
   SAR be interpreted correctly for comparison and diff display.

A decoder succeeding at level 1 must not be read as a promise that the pair is
comparable; levels 2 and 3 decide that, and any resampling or color conversion is
stated in the UI (`Resampled comparison — not pixel-exact`).

## Stages

- **Stage 1 — correctness first (implemented):** keep software decode; formally cover
  H.264/HEVC/MPEG-4 Part 2, 8-bit YUV420, CFR/VFR, non-zero start PTS, B-frame
  ordering, MP4/MOV/MKV/AVI, rotation and SAR metadata, and differing resolutions.
- **Stage 2 — compatibility (implemented):**
  P010/10-bit, 4:2:2/4:4:4, RGB input, transfer metadata, rotation, and SAR are covered by
  real fixtures and WARP pixel-readback tests. A native BGRA storage/render path remains
  optional because RGB input already has a deterministic visual normalization path.
- **Stage 3 — hardware decode (implemented):** FFmpeg D3D11VA with one decode context
  per source sharing a single D3D11 device, NV12/P010 textures going straight to the
  renderer (no GPU→CPU→GPU), device-generation rejection, and software fallback with a
  queryable backend/fallback reason. H.264 NV12, HEVC Main10 P010, direct array-slice
  publication, and 10-bit signature normalization are covered on the local hardware runner.
  The single-/two-/three-source 1080p60 visible-window profiles are release-blocking for five
  continuous minutes. Small-fixture
  P010 and 10-bit tests retain format and hardware-path correctness coverage; 4K is not part
  of the active performance matrix.

## Optional compatibility proxies

Proxies must never be a precondition for playback (the pre-1.0 forced-proxy chain is
retired and remains available through Git history). A proxy may be generated only when
the original cannot seek stably, when a format cannot enter the render pipeline directly,
or when the user explicitly asks for a fast preview cache.
