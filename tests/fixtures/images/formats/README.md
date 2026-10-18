# Image-format fixtures (MEDIA1)

These are intentional binary fixtures. They are generated and must not be hand-edited.

`manifest.json` records each file's repository-relative path, SHA-256, format, codec,
geometry, bit depth, alpha, colour type, generation command, current decoder/entry behaviour
and (for the videos) the real ffprobe result. Paths are relative to the repository root.

## Contents

- Still images: 8-bit PNG, 16-bit grayscale PNG, JPEG, BMP, static single-frame GIF,
  lossless WebP, 8-bit RGB TIFF, binary P6 PNM.
- Negative fixtures: a truncated PNG (incomplete IDAT, no IEND) and JPEG bytes carrying a
  `.png` extension.
- Videos under `tests/fixtures/media/`: AV1 in Matroska (`.mkv`) and VP9 in WebM (`.webm`),
  each 12 frames, 64x48, 30 fps, 8-bit SDR, yuv420p, under 2 KB.

## Provenance

- Generator: `tools/fixtures/generate-image-format-fixtures.py`.
- Tooling: Pillow 12.3.0 (PNG/JPEG/BMP/GIF/WebP/TIFF encoders) and FFmpeg
  2025-01-15-git-4f3c9f2f03 full build with libaom-av1 and libvpx-vp9.
- Images contain no wall-clock metadata. The ffmpeg commands pass
  `-fflags +bitexact -flags:v +bitexact` as output options, which removes the random
  Matroska segment UID, so two runs with the same FFmpeg build are byte-identical.
- All content is a generated test pattern; no third-party media is copied.

## Regeneration

```powershell
python tools/fixtures/generate-image-format-fixtures.py --ffmpeg <path-to-ffmpeg>
```

`ffprobe` must sit next to `ffmpeg`. The script prints the fixture count (10 images + 2
videos). Image bytes are byte-identical with the same Pillow/zlib/libjpeg/libwebp/libtiff
builds; video bytes are byte-identical with the same FFmpeg build and may change with a
different FFmpeg version.

Verify the committed files, manifests, ICC profiles, PNM dimensions and video probes:

```powershell
python tools/fixtures/verify-fixtures.py --ffmpeg <path-to-ffmpeg>
```

## Current negative behaviour

The truncated PNG must fail through both Pillow and FFmpeg. The fake-extension file is
routed by its `.png` extension but sniffed as JPEG by the still-image decoder; tests should
assert that recorded behaviour rather than assume extension equals content. AV1 and VP9 are
decoded by FFmpeg but rejected by the application's current explicit codec gate; `.webm` is
also outside the current video extension list.
