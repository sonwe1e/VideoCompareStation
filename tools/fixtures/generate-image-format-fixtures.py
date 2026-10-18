#!/usr/bin/env python3
"""Generate the MEDIA1 image-format fixtures and the AV1/VP9 probe media.

The image fixtures are tiny real encodings (not injected QImage data) for the still-image
entry and decoder contract.  The two videos live beside the existing video fixtures because
MediaProbeTests consumes video files from tests/fixtures/media; their real codec and pixel
format are recorded from ffprobe in the manifest.

Run with the repository's bundled Python and an FFmpeg that provides libaom-av1 and
libvpx-vp9:

    python tools/fixtures/generate-image-format-fixtures.py --ffmpeg <path-to-ffmpeg>

Image bytes are byte-identical when regenerated with the same Pillow, zlib, libjpeg, libwebp
and libtiff builds.  Video bytes are byte-identical for the same FFmpeg build because the
ffmpeg commands pass bitexact flags; a different FFmpeg build may produce different bytes.
The manifest's generationDate field is the only run-dependent value.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image
from PIL import __version__ as PILLOW_VERSION

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_IMAGE_DIR = REPO_ROOT / "tests" / "fixtures" / "images" / "formats"
DEFAULT_MEDIA_DIR = REPO_ROOT / "tests" / "fixtures" / "media"
IMAGE_PREFIX = "tests/fixtures/images/formats"
MEDIA_PREFIX = "tests/fixtures/media"
GENERATOR_SCRIPT = "tools/fixtures/generate-image-format-fixtures.py"
MANIFEST_NAME = "manifest.json"
MAX_FIXTURE_BYTES = 50 * 1024
VIDEO_SIZE = "64x48"
VIDEO_RATE = "30"
VIDEO_FRAMES = 12
VIDEO_DURATION = "0.4"

SUPPORTED_EXTENSIONS = (
    ".png",
    ".jpg",
    ".jpeg",
    ".bmp",
    ".gif",
    ".webp",
    ".tif",
    ".tiff",
    ".pnm",
    ".ppm",
    ".pgm",
    ".pbm",
    ".pam",
)
VIDEO_EXTENSIONS = (".mp4", ".mkv", ".mov", ".avi", ".m4v")


# --------------------------------------------------------------------------------------
# Small helpers
# --------------------------------------------------------------------------------------


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run(command: list[str], *, label: str) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        command, capture_output=True, text=True, check=False, encoding="utf-8", errors="replace"
    )
    if completed.returncode != 0:
        stderr = (completed.stderr or "").strip()
        raise RuntimeError(f"{label} failed (exit {completed.returncode}): {stderr[:1200]}")
    return completed


def base_image() -> Image.Image:
    """A deterministic 16x16 RGB pattern with obvious geometry for format tests."""
    image = Image.new("RGB", (16, 16))
    pixels = image.load()
    for y in range(16):
        for x in range(16):
            pixels[x, y] = (
                (x * 16 + y * 3) % 256,
                (x * 5 + y * 17) % 256,
                ((x ^ y) * 17) % 256,
            )
    return image


def pnm_p6_rgb8(width: int = 16, height: int = 16) -> bytes:
    output = bytearray(f"P6\n{width} {height}\n255\n".encode("ascii"))
    for y in range(height):
        for x in range(width):
            output += bytes(
                ((x * 16 + y * 3) % 256, (x * 5 + y * 17) % 256, ((x ^ y) * 17) % 256)
            )
    return bytes(output)


def find_ffmpeg(explicit: str | None) -> Path:
    if explicit:
        candidate = Path(explicit).resolve()
        if candidate.is_file():
            return candidate
        raise SystemExit(f"ffmpeg not found at {candidate}")
    from_environment = os.environ.get("FFMPEG")
    if from_environment:
        candidate = Path(from_environment).resolve()
        if candidate.is_file():
            return candidate
    found = shutil.which("ffmpeg")
    if found:
        return Path(found).resolve()
    raise SystemExit(
        "ffmpeg was not found. Pass --ffmpeg <path> or set FFMPEG; the video fixtures "
        "cannot be generated without libaom-av1 and libvpx-vp9."
    )


def find_ffprobe(ffmpeg: Path) -> Path:
    name = "ffprobe.exe" if ffmpeg.name.lower().endswith(".exe") else "ffprobe"
    candidate = ffmpeg.parent / name
    if not candidate.is_file():
        raise SystemExit(f"ffprobe not found next to ffmpeg: {candidate}")
    return candidate


def tool_version(executable: Path) -> str:
    completed = run([str(executable), "-version"], label=f"{executable.name} -version")
    return completed.stdout.splitlines()[0].strip() if completed.stdout else "unknown"


def ffprobe_json(ffprobe: Path, path: Path, *, count_frames: bool = False) -> dict:
    command = [
        str(ffprobe),
        "-v",
        "error",
    ]
    if count_frames:
        command.append("-count_frames")
    command += [
        "-show_entries",
        "stream=codec_name,pix_fmt,width,height,r_frame_rate,avg_frame_rate,nb_read_frames,"
        "color_range",
        "-show_entries",
        "format=format_name,duration",
        "-of",
        "json",
        str(path),
    ]
    completed = run(command, label=f"ffprobe {path.name}")
    return json.loads(completed.stdout)


def ffmpeg_decodes(ffmpeg: Path, path: Path) -> bool:
    completed = subprocess.run(
        [str(ffmpeg), "-hide_banner", "-loglevel", "error", "-i", str(path), "-f", "null", "-"],
        capture_output=True,
        text=True,
        check=False,
        encoding="utf-8",
        errors="replace",
    )
    return completed.returncode == 0


def ffmpeg_command_video(ffmpeg: Path, output: Path, codec: str) -> list[str]:
    """Build a deterministic 12-frame 64x48 SDR test source for the requested codec."""
    command = [
        str(ffmpeg),
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-f",
        "lavfi",
        "-i",
        f"testsrc2=size={VIDEO_SIZE}:rate={VIDEO_RATE}:duration={VIDEO_DURATION}",
        "-frames:v",
        str(VIDEO_FRAMES),
        "-an",
        "-c:v",
        codec,
        "-pix_fmt",
        "yuv420p",
    ]
    if codec == "libaom-av1":
        command += ["-crf", "35", "-b:v", "0", "-cpu-used", "8", "-row-mt", "1"]
    elif codec == "libvpx-vp9":
        command += ["-crf", "40", "-b:v", "0", "-cpu-used", "8", "-row-mt", "1"]
    else:
        raise ValueError(f"Unsupported test codec: {codec}")
    # Output options: bitexact removes the random Matroska segment UID and encoder strings,
    # making a second run byte-identical with the same FFmpeg build.
    command += ["-fflags", "+bitexact", "-flags:v", "+bitexact", str(output)]
    return command


def generation_command_text(ffmpeg_args: list[str], output: Path) -> str:
    """A machine-independent command description for the manifest."""
    relative = output.name
    sanitized = ["ffmpeg" if index == 0 else argument for index, argument in enumerate(ffmpeg_args)]
    sanitized[-1] = f"<output>/{relative}"
    return " ".join(sanitized)


# --------------------------------------------------------------------------------------
# Fixture generation
# --------------------------------------------------------------------------------------


class EntryBuilder:
    def __init__(self) -> None:
        self.entries: list[dict] = []

    def add_image(
        self,
        path: Path,
        *,
        image_format: str,
        codec: str | None,
        color_type: str,
        bit_depth: int,
        alpha: bool,
        generation_command: str,
        expected_behavior: str,
        current_support: str,
        width: int = 16,
        height: int = 16,
        probe: dict | None = None,
        extra: dict | None = None,
    ) -> dict:
        data = path.read_bytes()
        if len(data) > MAX_FIXTURE_BYTES:
            raise RuntimeError(f"{path.name} exceeds {MAX_FIXTURE_BYTES} bytes: {len(data)}")
        entry = {
            "path": f"{IMAGE_PREFIX}/{path.name}",
            "sha256": sha256_bytes(data),
            "bytes": len(data),
            "format": image_format,
            "codec": codec,
            "width": width,
            "height": height,
            "bitDepth": bit_depth,
            "alpha": alpha,
            "colorType": color_type,
            "generationCommand": generation_command,
            "expectedBehavior": expected_behavior,
            "currentSupport": current_support,
        }
        if probe is not None:
            entry["probe"] = probe
        if extra:
            entry.update(extra)
        self.entries.append(entry)
        return entry

    def add_video(
        self,
        path: Path,
        *,
        container: str,
        codec: str,
        generation_command: str,
        expected_behavior: str,
        current_gate: str,
        entry_filter: str,
        probe: dict,
    ) -> dict:
        data = path.read_bytes()
        if len(data) > MAX_FIXTURE_BYTES:
            raise RuntimeError(f"{path.name} exceeds {MAX_FIXTURE_BYTES} bytes: {len(data)}")
        streams = probe.get("streams") or []
        if not streams:
            raise RuntimeError(f"ffprobe reported no video stream for {path.name}")
        stream = streams[0]
        entry = {
            "path": f"{MEDIA_PREFIX}/{path.name}",
            "sha256": sha256_bytes(data),
            "bytes": len(data),
            "format": container,
            "codec": codec,
            "pixelFormat": stream.get("pix_fmt"),
            "width": int(stream.get("width", 0)),
            "height": int(stream.get("height", 0)),
            "bitDepth": 8,
            "alpha": False,
            "colorType": "YUV420P 8-bit SDR",
            "frameCount": int(stream.get("nb_read_frames", 0)),
            "averageFrameRate": stream.get("avg_frame_rate"),
            "durationSeconds": (probe.get("format") or {}).get("duration"),
            "colorRange": stream.get("color_range"),
            "generationCommand": generation_command,
            "expectedBehavior": expected_behavior,
            "currentSupport": "rejected",
            "currentGate": current_gate,
            "entryFilter": entry_filter,
            "probe": {
                "formatName": (probe.get("format") or {}).get("format_name"),
                "codec": stream.get("codec_name"),
                "pixelFormat": stream.get("pix_fmt"),
                "width": stream.get("width"),
                "height": stream.get("height"),
                "frameCount": stream.get("nb_read_frames"),
            },
        }
        self.entries.append(entry)
        return entry


def _image_probe(ffprobe: Path, path: Path) -> dict:
    probe = ffprobe_json(ffprobe, path)
    streams = probe.get("streams") or []
    if not streams:
        raise RuntimeError(f"ffprobe reported no stream for {path.name}")
    stream = streams[0]
    return {
        "formatName": (probe.get("format") or {}).get("format_name"),
        "codec": stream.get("codec_name"),
        "pixelFormat": stream.get("pix_fmt"),
        "width": stream.get("width"),
        "height": stream.get("height"),
    }


def generate_images(image_dir: Path, ffmpeg: Path, ffprobe: Path) -> list[dict]:
    image_dir.mkdir(parents=True, exist_ok=True)
    builder = EntryBuilder()
    base = base_image()

    def probe(path: Path) -> dict:
        return _image_probe(ffprobe, path)

    # 8-bit PNG.
    png_path = image_dir / "png_rgb8_16x16.png"
    base.save(png_path, format="PNG")
    builder.add_image(
        png_path,
        image_format="png",
        codec="png",
        color_type="RGB8",
        bit_depth=8,
        alpha=False,
        generation_command="Pillow Image.save(format='PNG') of a deterministic 16x16 RGB image",
        expected_behavior=(
            "FFmpeg PNG decoder handles 8-bit RGB; StillImageDecoder sniffs the PNG signature; "
            ".png is in the application still-image extension list."
        ),
        current_support="supported",
        probe=probe(png_path),
    )

    # 16-bit grayscale PNG.
    gray16 = Image.new("I;16", (16, 16))
    gray_pixels = gray16.load()
    for y in range(16):
        for x in range(16):
            gray_pixels[x, y] = (x * 4096 + y * 17) % 65536
    gray16_path = image_dir / "png_gray16_16x16.png"
    gray16.save(gray16_path, format="PNG")
    with Image.open(gray16_path) as check:
        check.load()
        if check.mode not in ("I", "I;16"):
            raise RuntimeError(f"16-bit gray PNG decoded to unexpected mode {check.mode}")
    builder.add_image(
        gray16_path,
        image_format="png",
        codec="png",
        color_type="Gray16",
        bit_depth=16,
        alpha=False,
        generation_command=(
            "Pillow Image.save(format='PNG') of a 16x16 I;16 gradient (PNG colour type 0, "
            "bit depth 16)"
        ),
        expected_behavior=(
            "FFmpeg PNG decoder reports gray16be; StillImageDecoder preserves sourceBitDepth=16 "
            "and creates an RGBA64 sidecar, while the display buffer stays RGBA8."
        ),
        current_support="supported",
        probe=probe(gray16_path),
    )

    # JPEG.  subsampling=0 keeps a 4:4:4 source so geometry checks are not confounded by
    # chroma subsampling; the encoded pixels remain lossy and are not asserted exactly.
    jpeg_path = image_dir / "jpeg_rgb8_16x16.jpg"
    base.save(jpeg_path, format="JPEG", quality=90, subsampling=0, optimize=False)
    builder.add_image(
        jpeg_path,
        image_format="jpeg",
        codec="mjpeg",
        color_type="YCbCr 4:4:4",
        bit_depth=8,
        alpha=False,
        generation_command=(
            "Pillow Image.save(format='JPEG', quality=90, subsampling=0, optimize=False)"
        ),
        expected_behavior=(
            "FFmpeg MJPEG decoder handles the JPEG stream; .jpg is in the still-image "
            "extension list. Encoded pixels are lossy, so tests assert dimensions/format, not "
            "exact colours."
        ),
        current_support="supported",
        probe=probe(jpeg_path),
    )

    # BMP.
    bmp_path = image_dir / "bmp_rgb8_16x16.bmp"
    base.save(bmp_path, format="BMP")
    builder.add_image(
        bmp_path,
        image_format="bmp",
        codec="bmp",
        color_type="BGR8",
        bit_depth=8,
        alpha=False,
        generation_command="Pillow Image.save(format='BMP') of a deterministic 16x16 RGB image",
        expected_behavior=(
            "FFmpeg BMP decoder handles uncompressed 24-bit BMP; .bmp is in the still-image "
            "extension list."
        ),
        current_support="supported",
        probe=probe(bmp_path),
    )

    # Static GIF: one frame, fixed palette.
    gif_path = image_dir / "gif_static_16x16.gif"
    base.convert("P", palette=Image.Palette.ADAPTIVE, colors=16).save(gif_path, format="GIF")
    with Image.open(gif_path) as check:
        check.load()
        if getattr(check, "n_frames", 1) != 1:
            raise RuntimeError("GIF fixture unexpectedly contains more than one frame")
    builder.add_image(
        gif_path,
        image_format="gif",
        codec="gif",
        color_type="Palette8",
        bit_depth=8,
        alpha=False,
        generation_command=(
            "Pillow Image.save(format='GIF') of a 16-colour P-mode image; exactly one frame"
        ),
        expected_behavior=(
            "FFmpeg GIF decoder handles the single frame; .gif is in the still-image extension "
            "list. Recognising GIF must not imply animation or image-sequence playback support."
        ),
        current_support="supported",
        probe=probe(gif_path),
    )

    # Lossless WebP.
    webp_path = image_dir / "webp_lossless_16x16.webp"
    base.save(webp_path, format="WEBP", lossless=True, quality=100, method=6)
    builder.add_image(
        webp_path,
        image_format="webp",
        codec="webp",
        color_type="RGB8 (lossless WebP; ffprobe decoder output argb)",
        bit_depth=8,
        alpha=False,
        generation_command=(
            "Pillow Image.save(format='WEBP', lossless=True, quality=100, method=6)"
        ),
        expected_behavior=(
            "FFmpeg WebP decoder handles the lossless still image; .webp is in the still-image "
            "extension list. Animation/multi-frame semantics are out of scope."
        ),
        current_support="supported",
        probe=probe(webp_path),
    )

    # 8-bit RGB TIFF.
    tiff_path = image_dir / "tiff_rgb8_16x16.tiff"
    base.save(tiff_path, format="TIFF", compression="tiff_deflate")
    builder.add_image(
        tiff_path,
        image_format="tiff",
        codec="tiff",
        color_type="RGB8",
        bit_depth=8,
        alpha=False,
        generation_command=(
            "Pillow Image.save(format='TIFF', compression='tiff_deflate') of a 16x16 RGB image"
        ),
        expected_behavior=(
            "FFmpeg TIFF decoder handles 8-bit RGB; .tif/.tiff are in the still-image "
            "extension list. Multi-page semantics are out of scope."
        ),
        current_support="supported",
        probe=probe(tiff_path),
    )

    # Binary P6 PPM.
    pnm_path = image_dir / "pnm_p6_rgb8_16x16.ppm"
    pnm_path.write_bytes(pnm_p6_rgb8(16, 16))
    with Image.open(pnm_path) as check:
        check.load()
        if check.size != (16, 16) or check.mode != "RGB":
            raise RuntimeError(f"P6 fixture decoded as {check.mode} {check.size}")
    builder.add_image(
        pnm_path,
        image_format="pnm",
        codec="ppm",
        color_type="RGB8 (P6)",
        bit_depth=8,
        alpha=False,
        generation_command=(
            "Hand-written P6 PPM: ASCII header 'P6\\n16 16\\n255\\n' plus 768 raster bytes"
        ),
        expected_behavior=(
            "FFmpeg PPM decoder receives AV_CODEC_ID_PPM; StillImageDecoder sniffs the 'P6' "
            "magic; .ppm/.pnm are in the still-image extension list."
        ),
        current_support="supported",
        probe=probe(pnm_path),
    )

    # Corrupt/truncated PNG: present IHDR and part of IDAT, but no IEND.  Both Pillow and
    # FFmpeg must reject it; the generator verifies that negative control before committing.
    valid_png = png_path.read_bytes()
    corrupt_path = image_dir / "corrupt_truncated_16x16.png"
    corrupt_path.write_bytes(valid_png[: int(len(valid_png) * 0.6)])
    if ffmpeg_decodes(ffmpeg, corrupt_path):
        raise RuntimeError("truncated PNG unexpectedly decoded through ffmpeg")
    try:
        with Image.open(corrupt_path) as check:
            check.load()
    except Exception:
        pass
    else:
        raise RuntimeError("truncated PNG unexpectedly decoded through Pillow")
    builder.add_image(
        corrupt_path,
        image_format="png-corrupt",
        codec=None,
        color_type="RGB8 (declared by IHDR)",
        bit_depth=8,
        alpha=False,
        generation_command=(
            f"first {int(len(valid_png) * 0.6)} bytes of png_rgb8_16x16.png (partial IDAT, no IEND)"
        ),
        expected_behavior=(
            "The PNG signature is present, so StillImageDecoder selects the PNG decoder, but "
            "IDAT is incomplete and IEND is absent. FFmpeg and Pillow both fail; the expected "
            "result is a clear load error, and a failed load must not replace the committed "
            "pair/session."
        ),
        current_support="rejected",
        extra={
            "negativeControl": {
                "pillowDecode": "failed",
                "ffmpegDecode": "failed",
                "reason": "incomplete IDAT and missing IEND",
            }
        },
    )

    # Fake extension: real JPEG bytes named .png.  Entry routing is extension-based while
    # StillImageDecoder sniffs content, so the current decoder treats this as MJPEG.
    fake_path = image_dir / "fake-extension_jpeg-bytes_16x16.png"
    fake_path.write_bytes(jpeg_path.read_bytes())
    with Image.open(fake_path) as check:
        check.load()
        if check.format != "JPEG":
            raise RuntimeError(f"fake-extension fixture sniffed as {check.format}, expected JPEG")
    fake_probe = _image_probe(ffprobe, fake_path)
    builder.add_image(
        fake_path,
        image_format="jpeg-bytes-with-png-extension",
        codec=fake_probe.get("codec"),
        color_type="YCbCr 4:4:4 (JPEG content)",
        bit_depth=8,
        alpha=False,
        generation_command=(
            "byte-for-byte copy of jpeg_rgb8_16x16.jpg renamed to a .png extension"
        ),
        expected_behavior=(
            "Entry routing is extension-based and accepts .png, while StillImageDecoder sniffs "
            "the JPEG SOI marker, so the current decoder treats these bytes as MJPEG. The "
            "fixture records extension/content mismatch; tests must assert the chosen "
            "behaviour instead of assuming extension equals content."
        ),
        current_support="extension-mismatch",
        probe=fake_probe,
        extra={
            "extension": ".png",
            "contentFormat": "jpeg",
            "contentCodec": fake_probe.get("codec"),
        },
    )

    return builder.entries


def generate_videos(media_dir: Path, ffmpeg: Path, ffprobe: Path) -> list[dict]:
    media_dir.mkdir(parents=True, exist_ok=True)
    builder = EntryBuilder()

    specs = (
        {
            "filename": "av1_64x48_30fps_12.mkv",
            "codec": "libaom-av1",
            "expected_codec": "av1",
            "container": "matroska",
            "expected_behavior": (
                "FFmpeg decodes AV1/yuv420p (12 frames, 30 fps). CompareStation MediaProbe "
                "rejects AV1 because the accepted codec set is H.264/HEVC/MPEG-4 Part 2, so "
                "the expected result is kUnsupportedCodec. .mkv is an accepted video "
                "extension, so this rejection is reachable through normal entry routing."
            ),
            "current_gate": "MediaProbe.cpp codec gate: AV1 is not H.264/HEVC/MPEG-4 Part 2 "
            "→ kUnsupportedCodec",
            "entry_filter": ".mkv is in the video extension list (.mp4/.mkv/.mov/.avi/.m4v)",
        },
        {
            "filename": "vp9_64x48_30fps_12.webm",
            "codec": "libvpx-vp9",
            "expected_codec": "vp9",
            "container": "webm",
            "expected_behavior": (
                "FFmpeg decodes VP9/yuv420p (12 frames, 30 fps). MediaProbe rejects VP9 with "
                "kUnsupportedCodec. The container is .webm, which is not in the application "
                "video extension list (nor the still-image list), so normal entry routing "
                "rejects the file before the codec gate; the codec gate can be observed by "
                "probing the stream directly."
            ),
            "current_gate": "MediaProbe.cpp codec gate: VP9 is not H.264/HEVC/MPEG-4 Part 2 "
            "→ kUnsupportedCodec; entry routing also rejects .webm",
            "entry_filter": ".webm is not in the video extension list and not a still-image "
            "extension",
        },
    )

    for spec in specs:
        path = media_dir / spec["filename"]
        command = ffmpeg_command_video(ffmpeg, path, spec["codec"])
        run(command, label=f"ffmpeg {spec['filename']}")

        # A second run with the same command must be byte-identical, otherwise the fixture
        # is not deterministic and must be reported rather than committed silently.
        repro_path = path.with_name(f"{path.stem}.repro{path.suffix}")
        repro_command = ffmpeg_command_video(ffmpeg, repro_path, spec["codec"])
        run(repro_command, label=f"ffmpeg {spec['filename']} (determinism check)")
        first = path.read_bytes()
        second = repro_path.read_bytes()
        repro_path.unlink()
        if first != second:
            raise RuntimeError(
                f"{spec['filename']} is not byte-identical across two ffmpeg runs; "
                "the manifest cannot promise deterministic regeneration"
            )

        probe = ffprobe_json(ffprobe, path, count_frames=True)
        stream = (probe.get("streams") or [{}])[0]
        if stream.get("codec_name") != spec["expected_codec"]:
            raise RuntimeError(
                f"{spec['filename']} probed as {stream.get('codec_name')}, "
                f"expected {spec['expected_codec']}"
            )
        if stream.get("pix_fmt") != "yuv420p":
            raise RuntimeError(f"{spec['filename']} probed pix_fmt {stream.get('pix_fmt')}")
        if int(stream.get("nb_read_frames", 0)) != VIDEO_FRAMES:
            raise RuntimeError(
                f"{spec['filename']} has {stream.get('nb_read_frames')} frames, "
                f"expected {VIDEO_FRAMES}"
            )
        if (int(stream.get("width", 0)), int(stream.get("height", 0))) != (64, 48):
            raise RuntimeError(f"{spec['filename']} has unexpected dimensions")
        if not ffmpeg_decodes(ffmpeg, path):
            raise RuntimeError(f"{spec['filename']} does not decode through ffmpeg")

        builder.add_video(
            path,
            container=spec["container"],
            codec=spec["expected_codec"],
            generation_command=generation_command_text(command, path),
            expected_behavior=spec["expected_behavior"],
            current_gate=spec["current_gate"],
            entry_filter=spec["entry_filter"],
            probe=probe,
        )

    return builder.entries


# --------------------------------------------------------------------------------------
# Manifest and command line
# --------------------------------------------------------------------------------------


def build_manifest(
    image_entries: list[dict], video_entries: list[dict], ffmpeg_version: str, ffprobe_version: str
) -> dict:
    entries = image_entries + video_entries
    return {
        "fixtureFormatVersion": 1,
        "fixtureSet": "MEDIA1",
        "fixtureCount": len(entries),
        "imageDirectory": IMAGE_PREFIX,
        "mediaDirectory": MEDIA_PREFIX,
        "pathBase": "repository-root",
        "generatorScript": GENERATOR_SCRIPT,
        "generationDate": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"),
        "tools": {
            "pillowVersion": PILLOW_VERSION,
            "pythonVersion": sys.version.split()[0],
            "ffmpegVersion": ffmpeg_version,
            "ffprobeVersion": ffprobe_version,
        },
        "extensionContract": {
            "stillImageExtensions": list(SUPPORTED_EXTENSIONS),
            "videoExtensions": list(VIDEO_EXTENSIONS),
            "note": "Extension lists mirror src/application/src/MediaPaths.cpp; the manifest "
            "records the current contract so a changed filter is visible against these "
            "fixtures.",
        },
        "reproducibility": (
            "Image encodings use fixed Pillow parameters and contain no wall-clock metadata; "
            "they are byte-identical with the same Pillow/zlib/libjpeg/libwebp/libtiff builds. "
            "The ffmpeg commands pass -fflags +bitexact -flags:v +bitexact as output options; "
            "two runs with the same FFmpeg build were verified byte-identical. A different "
            "FFmpeg build may produce different video bytes. generationDate is the only "
            "run-dependent manifest value."
        ),
        "license": (
            "All fixtures are synthetic test patterns generated by this script; no "
            "third-party image or video content is copied into the repository."
        ),
        "fixtures": entries,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--image-dir",
        type=Path,
        default=DEFAULT_IMAGE_DIR,
        help="Directory for the still-image fixtures and manifest.json (default: %(default)s)",
    )
    parser.add_argument(
        "--media-dir",
        type=Path,
        default=DEFAULT_MEDIA_DIR,
        help="Directory for the AV1/VP9 video fixtures (default: %(default)s)",
    )
    parser.add_argument(
        "--ffmpeg",
        default=None,
        help="Path to ffmpeg (default: FFMPEG environment variable or PATH). ffprobe must be "
        "in the same directory.",
    )
    args = parser.parse_args(argv)

    image_dir = args.image_dir.resolve()
    media_dir = args.media_dir.resolve()
    ffmpeg = find_ffmpeg(args.ffmpeg)
    ffprobe = find_ffprobe(ffmpeg)

    image_entries = generate_images(image_dir, ffmpeg, ffprobe)
    video_entries = generate_videos(media_dir, ffmpeg, ffprobe)

    if len(image_entries) != 10:
        raise RuntimeError(f"Expected 10 image fixtures, wrote {len(image_entries)}")
    if len(video_entries) != 2:
        raise RuntimeError(f"Expected 2 video fixtures, wrote {len(video_entries)}")

    manifest = build_manifest(
        image_entries,
        video_entries,
        tool_version(ffmpeg),
        tool_version(ffprobe),
    )
    manifest_path = image_dir / MANIFEST_NAME
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n"
    )

    total = sum(entry["bytes"] for entry in manifest["fixtures"])
    print(
        f"Wrote {len(manifest['fixtures'])} media/format fixtures ({total} bytes): "
        f"{len(image_entries)} images in {image_dir}, {len(video_entries)} videos in {media_dir}"
    )
    print(f"Wrote {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
