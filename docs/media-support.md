# Media Support

Status: **current 1.6 contract** — all three stages (software correctness, format compatibility, D3D11VA hardware decode) are implemented. VP9, AV1, HDR/tone mapping, audio, and additional video
encoding work are intentionally outside this release.

## What opens today

The probe (`src/media_ffmpeg/src/MediaProbe.cpp`) accepts exactly:

| Dimension | Accepted | Rejected with |
|---|---|---|
| Codec | H.264, HEVC, MPEG-4 Part 2 (and a decoder must exist) | `kUnsupportedCodec` (L304-318) |
| Pixel format | 8/10-bit YUV 4:2:0/4:2:2/4:4:4 and 8-bit RGB | unsupported depth/layout → `kUnsupportedPixelFormat` |
| Transfer | SDR only — PQ (SMPTE2084) and HLG are rejected | `kUnsupportedPixelFormat` (L160-166) |
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
the gates above are the complete 1.4 contract.

VCStation does not decode or play audio. It uses video timestamps as the playback clock and is
presented in the UI as visual playback only.

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
