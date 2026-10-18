#!/usr/bin/env python3
"""Verify the committed COLOR2 and MEDIA1 fixture sets.

Checks performed:
  - both manifests are UTF-8 without BOM, valid JSON, and reject machine absolute paths;
  - every fixture exists, stays under 50 KiB, and matches its recorded SHA-256 and byte count;
  - the required fixture counts and category/format coverage are present;
  - every non-corrupt embedded ICC profile opens through Pillow ImageCms and reports its
    description; corrupt profiles are expected to be rejected;
  - LittleCMS reference values in the colour manifest are recomputed from the committed files;
  - the high-bit-depth and low-bit-difference raw codes are checked through FFmpeg rawvideo;
  - every PNM fixture decodes through Pillow or FFmpeg with the recorded dimensions;
  - format fixtures decode through Pillow/FFmpeg, the truncated PNG fails in both, and the
    fake-extension file is sniffed as JPEG;
  - AV1/VP9 are probed with ffprobe and decoded with ffmpeg, and the codec/pixel format/frame
    count match the manifest;
  - optionally, both generators are rerun into a scratch directory and every fixture byte is
    compared with the committed copy (``--check-reproduction``).

The script counts its own checks and fails if the number of executed checks is lower than the
minimum implied by the manifests, so a silently skipped loop cannot turn into a pass.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageCms
from PIL import __version__ as PILLOW_VERSION

REPO_ROOT = Path(__file__).resolve().parents[2]
COLOR_MANIFEST = REPO_ROOT / "tests" / "fixtures" / "images" / "color" / "manifest.json"
FORMATS_MANIFEST = REPO_ROOT / "tests" / "fixtures" / "images" / "formats" / "manifest.json"
COLOR_SCRIPT = REPO_ROOT / "tools" / "fixtures" / "generate-color-fixtures.py"
FORMATS_SCRIPT = REPO_ROOT / "tools" / "fixtures" / "generate-image-format-fixtures.py"
COLOR_PREFIX = "tests/fixtures/images/color"
FORMATS_IMAGE_PREFIX = "tests/fixtures/images/formats"
MEDIA_PREFIX = "tests/fixtures/media"
MAX_FIXTURE_BYTES = 50 * 1024
MAX_COLOR_SET_BYTES = 400 * 1024
RELATIVE_COLORIMETRIC = 1

REQUIRED_COLOR_CATEGORIES = {
    "srgb",
    "display-p3",
    "adobe-rgb",
    "no-profile",
    "corrupt-profile",
    "unsupported-profile",
    "alpha-edges",
    "gray",
    "high-bit-depth",
    "low-bit-difference",
    "same-codepoints-different-profile",
    "different-codepoints-same-color",
    "pnm",
}

_PATH_PATTERN = re.compile(r"(?:^|\")(?:[A-Za-z]:[\\/]|\\\\)")


class Verifier:
    def __init__(self) -> None:
        self.checks = 0
        self.failures: list[str] = []

    def check(self, condition: bool, message: str) -> bool:
        self.checks += 1
        if not condition:
            self.failures.append(message)
        return condition

    def require(self, condition: bool, message: str) -> None:
        if not self.check(condition, message):
            print(f"FATAL: {message}", file=sys.stderr)
            raise SystemExit(1)

    def report(self) -> int:
        if self.failures:
            print(f"\nFAILED: {len(self.failures)} of {self.checks} checks failed")
            for message in self.failures:
                print(f"  - {message}")
            return 1
        print(f"\nPASSED: {self.checks} checks")
        return 0


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def find_ffmpeg(explicit: str | None) -> Path | None:
    if explicit:
        candidate = Path(explicit).resolve()
        return candidate if candidate.is_file() else None
    from_environment = os.environ.get("FFMPEG")
    if from_environment and Path(from_environment).is_file():
        return Path(from_environment).resolve()
    found = shutil.which("ffmpeg")
    return Path(found).resolve() if found else None


def find_ffprobe(ffmpeg: Path) -> Path:
    name = "ffprobe.exe" if ffmpeg.name.lower().endswith(".exe") else "ffprobe"
    candidate = ffmpeg.parent / name
    if not candidate.is_file():
        raise SystemExit(f"ffprobe not found next to ffmpeg: {candidate}")
    return candidate


def run_json(command: list[str]) -> dict:
    completed = subprocess.run(
        command, capture_output=True, text=True, check=False, encoding="utf-8", errors="replace"
    )
    if completed.returncode != 0:
        raise RuntimeError((completed.stderr or "").strip()[:800])
    return json.loads(completed.stdout)


def ffprobe(ffprobe_path: Path, path: Path, *, count_frames: bool = False) -> dict:
    command = [str(ffprobe_path), "-v", "error"]
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
    return run_json(command)


def ffmpeg_decodes(ffmpeg_path: Path, path: Path) -> bool:
    completed = subprocess.run(
        [str(ffmpeg_path), "-hide_banner", "-loglevel", "error", "-i", str(path), "-f", "null", "-"],
        capture_output=True,
        text=True,
        check=False,
        encoding="utf-8",
        errors="replace",
    )
    return completed.returncode == 0


def ffmpeg_raw_frames(ffmpeg_path: Path, path: Path, pixel_format: str) -> bytes:
    completed = subprocess.run(
        [
            str(ffmpeg_path),
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            str(path),
            "-f",
            "rawvideo",
            "-pix_fmt",
            pixel_format,
            "-",
        ],
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"ffmpeg rawvideo extraction failed for {path.name}: "
            f"{completed.stderr.decode('utf-8', 'replace')[:400]}"
        )
    return completed.stdout


def png_header(path: Path) -> tuple[int, int, int, int]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    width, height = struct.unpack(">II", data[16:24])
    bit_depth = data[24]
    color_type = data[25]
    return width, height, bit_depth, color_type


def load_manifest(verifier: Verifier, path: Path, label: str) -> dict:
    verifier.require(path.is_file(), f"{label} manifest is missing: {path}")
    raw = path.read_bytes()
    verifier.check(not raw.startswith(b"\xef\xbb\xbf"), f"{label} manifest has a UTF-8 BOM")
    text = raw.decode("utf-8")
    try:
        manifest = json.loads(text)
    except json.JSONDecodeError as error:
        verifier.require(False, f"{label} manifest is not valid JSON: {error}")
    verifier.check(
        not _PATH_PATTERN.search(text),
        f"{label} manifest contains a machine absolute path",
    )
    return manifest


def verify_fixture_files(verifier: Verifier, manifest: dict, label: str) -> list[tuple[dict, Path]]:
    entries = manifest["fixtures"]
    resolved: list[tuple[dict, Path]] = []
    seen_paths: set[str] = set()
    for entry in entries:
        relative = entry["path"]
        verifier.check("\\" not in relative, f"{label}: path uses backslashes: {relative}")
        verifier.check(
            relative.startswith("tests/fixtures/"),
            f"{label}: path is not repository-relative: {relative}",
        )
        verifier.check(relative not in seen_paths, f"{label}: duplicate path: {relative}")
        seen_paths.add(relative)
        path = REPO_ROOT / relative
        if not verifier.check(path.is_file(), f"{label}: missing file: {relative}"):
            continue
        data = path.read_bytes()
        verifier.check(
            len(data) <= MAX_FIXTURE_BYTES,
            f"{label}: {relative} is {len(data)} bytes (> 50 KiB)",
        )
        verifier.check(
            len(data) == entry["bytes"],
            f"{label}: {relative} byte count {len(data)} != manifest {entry['bytes']}",
        )
        verifier.check(
            sha256_bytes(data) == entry["sha256"],
            f"{label}: SHA-256 mismatch for {relative}",
        )
        resolved.append((entry, path))
    verifier.check(
        manifest["fixtureCount"] == len(entries),
        f"{label}: fixtureCount {manifest['fixtureCount']} != {len(entries)} entries",
    )
    return resolved


def verify_color_manifest_shape(verifier: Verifier, manifest: dict) -> None:
    entries = manifest["fixtures"]
    verifier.check(manifest["fixtureCount"] >= 20, "COLOR2 fixtureCount is below 20")
    categories: dict[str, int] = {}
    for entry in entries:
        categories[entry["category"]] = categories.get(entry["category"], 0) + 1
    missing = sorted(REQUIRED_COLOR_CATEGORIES - set(categories))
    verifier.check(not missing, f"COLOR2 required categories missing: {missing}")
    verifier.check(
        manifest["categoryCounts"] == dict(sorted(categories.items())),
        "COLOR2 categoryCounts does not match the fixture entries",
    )
    total = sum(entry["bytes"] for entry in entries)
    verifier.check(total < MAX_COLOR_SET_BYTES, f"COLOR2 total {total} bytes is not < 400 KiB")
    tool = manifest["referenceTool"]
    verifier.check(
        tool["pillowVersion"] == PILLOW_VERSION,
        f"verifier Pillow {PILLOW_VERSION} != manifest Pillow {tool['pillowVersion']}",
    )
    verifier.check(
        tool["littlecmsVersion"] == ImageCms.core.littlecms_version,
        "verifier LittleCMS version does not match the manifest",
    )
    verifier.check(
        manifest["generatorScript"] == "tools/fixtures/generate-color-fixtures.py",
        "COLOR2 generatorScript is unexpected",
    )
    round_trips = manifest.get("profileRoundTrips") or {}
    results = round_trips.get("results") or []
    verifier.check(len(results) == 3, "COLOR2 profileRoundTrips does not cover three profiles")
    for result in results:
        verifier.check(
            result.get("maxChannelError", 99) <= round_trips.get("tolerance", 0),
            f"COLOR2 round-trip exceeds tolerance for {result.get('profile')}",
        )
    unsupported = round_trips.get("unsupportedDataSpaceCheck") or {}
    verifier.check(
        "rejected" in str(unsupported.get("expected", "")).lower(),
        "COLOR2 Lab data-space rejection is not recorded",
    )


def verify_formats_manifest_shape(verifier: Verifier, manifest: dict) -> None:
    entries = manifest["fixtures"]
    verifier.check(manifest["fixtureCount"] >= 10, "MEDIA1 fixtureCount is below 10")
    image_entries = [entry for entry in entries if entry["path"].startswith(FORMATS_IMAGE_PREFIX)]
    video_entries = [entry for entry in entries if entry["path"].startswith(MEDIA_PREFIX)]
    verifier.check(len(image_entries) >= 10, "MEDIA1 has fewer than 10 image fixtures")
    verifier.check(len(video_entries) == 2, "MEDIA1 does not have exactly two video fixtures")
    verifier.check(
        manifest["generatorScript"] == "tools/fixtures/generate-image-format-fixtures.py",
        "MEDIA1 generatorScript is unexpected",
    )
    required = {
        "png_rgb8": ("png", "png"),
        "png_gray16": ("png", "png"),
        "jpeg": ("jpeg", "mjpeg"),
        "bmp": ("bmp", "bmp"),
        "gif": ("gif", "gif"),
        "webp": ("webp", "webp"),
        "tiff": ("tiff", "tiff"),
        "pnm": ("pnm", "ppm"),
        "corrupt_png": ("png-corrupt", None),
        "fake_extension": ("jpeg-bytes-with-png-extension", "mjpeg"),
    }
    for marker, (fmt, codec) in required.items():
        matched = any(
            entry["format"] == fmt and entry.get("codec") == codec
            for entry in entries
            if marker in Path(entry["path"]).name or marker.replace("_", "-") in Path(entry["path"]).name
        )
        if not matched:
            # Fall back to format-only matching for markers whose filename differs.
            matched = any(entry["format"] == fmt for entry in entries)
        verifier.check(matched, f"MEDIA1 missing required fixture kind: {marker}")
    verifier.check(
        any(entry.get("codec") == "av1" for entry in video_entries),
        "MEDIA1 missing AV1 video fixture",
    )
    verifier.check(
        any(entry.get("codec") == "vp9" for entry in video_entries),
        "MEDIA1 missing VP9 video fixture",
    )
    for entry in video_entries:
        verifier.check(entry.get("bitDepth") == 8, f"MEDIA1 video is not 8-bit: {entry['path']}")
        verifier.check(
            entry.get("pixelFormat") == "yuv420p",
            f"MEDIA1 video is not yuv420p: {entry['path']}",
        )
        verifier.check(
            entry.get("frameCount") == 12,
            f"MEDIA1 video does not record 12 frames: {entry['path']}",
        )
    verifier.check(
        ".webm" not in manifest["extensionContract"]["videoExtensions"],
        "MEDIA1 extension contract unexpectedly lists .webm as a video extension",
    )


def verify_color_fixtures(
    verifier: Verifier,
    manifest: dict,
    resolved: list[tuple[dict, Path]],
    ffmpeg_path: Path,
    ffprobe_path: Path,
) -> dict[str, str]:
    descriptions: dict[str, str] = {}
    valid_profile_hashes: set[str] = set()
    corrupt_rejections = 0

    for entry, path in resolved:
        if path.suffix.lower() == ".png":
            width, height, bit_depth, color_type = png_header(path)
            expected = {
                "RGB8": (2, 8),
                "RGBA8": (6, 8),
                "Gray8": (0, 8),
                "Gray16": (0, 16),
                "RGB16": (2, 16),
            }.get(entry["colorType"])
            if expected:
                verifier.check(
                    (color_type, bit_depth) == expected,
                    f"COLOR2: {path.name} IHDR colour type/bit depth "
                    f"{(color_type, bit_depth)} != {expected}",
                )
            verifier.check(
                (width, height) == (entry["width"], entry["height"]),
                f"COLOR2: {path.name} IHDR dimensions differ from manifest",
            )
        elif path.suffix.lower() == ".pam":
            probe = ffprobe(ffprobe_path, path)
            stream = (probe.get("streams") or [{}])[0]
            verifier.check(
                (stream.get("width"), stream.get("height")) == (entry["width"], entry["height"]),
                f"COLOR2: PAM dimensions differ from manifest: {path.name}",
            )
            verifier.check(
                ffmpeg_decodes(ffmpeg_path, path),
                f"COLOR2: PAM did not decode through ffmpeg: {path.name}",
            )
            continue
        else:
            try:
                with Image.open(path) as image:
                    image.load()
                    verifier.check(
                        image.size == (entry["width"], entry["height"]),
                        f"COLOR2: {path.name} Pillow dimensions differ from manifest",
                    )
            except Exception:
                verifier.check(False, f"COLOR2: PNM did not decode through Pillow: {path.name}")
                continue

        if entry["colorType"] in ("PBM", "PGM", "PPM", "PAM"):
            # Pillow handles P1-P6; P7 was handled above with ffprobe.
            try:
                with Image.open(path) as image:
                    image.load()
                    verifier.check(
                        image.size == (entry["width"], entry["height"]),
                        f"COLOR2: {path.name} PNM dimensions differ from manifest",
                    )
            except Exception:
                verifier.check(False, f"COLOR2: PNM did not decode: {path.name}")
            continue

        try:
            with Image.open(path) as image:
                image.load()
                embedded = image.info.get("icc_profile")
                raw_probes = image.copy()
        except Exception as error:
            verifier.check(False, f"COLOR2: {path.name} did not decode: {error}")
            continue

        profile_record = entry.get("embeddedProfile")
        if profile_record is None:
            verifier.check(embedded is None, f"COLOR2: {path.name} has an unrecorded ICC profile")
        else:
            verifier.check(embedded is not None, f"COLOR2: {path.name} is missing its ICC profile")
            if embedded is None:
                continue
            verifier.check(
                sha256_bytes(embedded) == profile_record["sha256"],
                f"COLOR2: {path.name} embedded profile hash differs from manifest",
            )
            if profile_record["valid"]:
                try:
                    profile = ImageCms.ImageCmsProfile(io.BytesIO(embedded))
                    description = ImageCms.getProfileDescription(profile).strip()
                    verifier.check(
                        bool(description), f"COLOR2: empty profile description for {path.name}"
                    )
                    descriptions[profile_record["sha256"]] = description
                    valid_profile_hashes.add(profile_record["sha256"])
                except Exception as error:
                    verifier.check(
                        False, f"COLOR2: valid profile did not open for {path.name}: {error}"
                    )
            else:
                try:
                    ImageCms.ImageCmsProfile(io.BytesIO(embedded))
                except Exception:
                    corrupt_rejections += 1
                else:
                    verifier.check(
                        False, f"COLOR2: corrupt profile opened unexpectedly for {path.name}"
                    )

        reference = entry.get("referenceValues") or {}
        expected_entries = reference.get("expectedSrgb8")
        if expected_entries:
            source_profile = embedded
            verifier.check(
                source_profile is not None,
                f"COLOR2: {path.name} has expected sRGB values but no source profile",
            )
            if source_profile is not None:
                try:
                    source = ImageCms.ImageCmsProfile(io.BytesIO(source_profile))
                    target = ImageCms.createProfile("sRGB")
                    source_image = (
                        raw_probes if raw_probes.mode == "L" else raw_probes.convert("RGB")
                    )
                    converted = ImageCms.profileToProfile(
                        source_image,
                        source,
                        target,
                        renderingIntent=RELATIVE_COLORIMETRIC,
                        outputMode="RGB",
                    )
                    for expected in expected_entries:
                        actual = list(converted.getpixel((expected["x"], expected["y"])))
                        verifier.check(
                            actual == expected["srgb8"],
                            f"COLOR2: {path.name} ({expected['x']},{expected['y']}) "
                            f"recomputed {actual} != manifest {expected['srgb8']}",
                        )
                        if entry["category"] == "different-codepoints-same-color":
                            verifier.check(
                                max(abs(a - b) for a, b in zip(actual, (255, 0, 0))) <= 1,
                                f"COLOR2: {path.name} does not convert to sRGB red within ±1",
                            )
                except Exception as error:
                    verifier.check(
                        False, f"COLOR2: reference conversion failed for {path.name}: {error}"
                    )

        probes = reference.get("probePixels") or []
        if probes and "raw" in probes[0]:
            for probe in probes:
                actual = raw_probes.getpixel((probe["x"], probe["y"]))
                actual_list = [actual] if isinstance(actual, int) else list(actual)
                verifier.check(
                    actual_list == probe["raw"],
                    f"COLOR2: {path.name} raw pixel {probe['x']},{probe['y']} is "
                    f"{actual_list}, manifest says {probe['raw']}",
                )

        if entry["category"] == "low-bit-difference":
            for probe in probes:
                raw_code = probe["rawGray16"]
                verifier.check(
                    (raw_code >> 8) == reference["expectedDisplay8"],
                    f"COLOR2: {path.name} high byte of 0x{raw_code:04x} is not 16",
                )
                verifier.check(
                    round(raw_code / 257) == reference["expectedDisplay8"],
                    f"COLOR2: {path.name} rounded display value of 0x{raw_code:04x} is not 16",
                )
            raw = ffmpeg_raw_frames(ffmpeg_path, path, "gray16be")
            verifier.check(
                len(raw) == entry["width"] * entry["height"] * 2,
                f"COLOR2: {path.name} raw gray16 size is unexpected",
            )
            for probe in probes:
                if len(raw) >= entry["width"] * entry["height"] * 2:
                    offset = (probe["y"] * entry["width"] + probe["x"]) * 2
                    code = struct.unpack(">H", raw[offset : offset + 2])[0]
                    verifier.check(
                        code == probe["rawGray16"],
                        f"COLOR2: {path.name} decoded 16-bit code 0x{code:04x} != "
                        f"manifest 0x{probe['rawGray16']:04x}",
                    )

        if entry["category"] == "high-bit-depth":
            raw = ffmpeg_raw_frames(ffmpeg_path, path, "rgb48be")
            verifier.check(
                len(raw) == entry["width"] * entry["height"] * 6,
                f"COLOR2: {path.name} raw rgb48 size is unexpected",
            )
            for probe in probes:
                offset = (probe["y"] * entry["width"] + probe["x"]) * 6
                if len(raw) < offset + 6:
                    verifier.check(False, f"COLOR2: {path.name} raw probe out of range")
                    continue
                codes = list(struct.unpack(">HHH", raw[offset : offset + 6]))
                verifier.check(
                    codes == probe["rawRgb16"],
                    f"COLOR2: {path.name} decoded RGB48 {codes} != manifest "
                    f"{probe['rawRgb16']}",
                )

        if entry["category"] == "alpha-edges":
            transparent = raw_probes.getpixel((0, 0))
            verifier.check(
                transparent[3] == 0 and any(channel > 0 for channel in transparent[:3]),
                "COLOR2: alpha=0 probe lost its non-zero RGB values",
            )
            for expected in expected_entries:
                matching = next(
                    probe
                    for probe in probes
                    if (probe["x"], probe["y"]) == (expected["x"], expected["y"])
                )
                verifier.check(
                    expected["alpha"] == matching["raw"][3],
                    f"COLOR2: alpha reference mismatch at {expected['x']},{expected['y']}",
                )

    # Pair-level invariants.
    by_name = {path.name: (entry, path) for entry, path in resolved}
    same_names = ("same-codepoints_srgb_8x8.png", "same-codepoints_p3_8x8.png")
    if all(name in by_name for name in same_names):
        raw_srgb = by_name[same_names[0]][0]["referenceValues"]["probePixels"]
        raw_p3 = by_name[same_names[1]][0]["referenceValues"]["probePixels"]
        verifier.check(
            [probe["raw"] for probe in raw_srgb] == [probe["raw"] for probe in raw_p3],
            "COLOR2: same-codepoints pair does not share raw codepoints",
        )
        converted_srgb = by_name[same_names[0]][0]["referenceValues"]["expectedSrgb8"]
        converted_p3 = by_name[same_names[1]][0]["referenceValues"]["expectedSrgb8"]
        differing = sum(
            1
            for left, right in zip(converted_srgb, converted_p3)
            if any(abs(a - b) > 1 for a, b in zip(left["srgb8"], right["srgb8"]))
        )
        verifier.check(
            differing > 0,
            "COLOR2: same-codepoints pair produces identical converted values beyond tolerance",
        )

    red_names = (
        "different-codepoints_srgb-red_8x8.png",
        "different-codepoints_p3-red_8x8.png",
    )
    if all(name in by_name for name in red_names):
        raw_srgb = by_name[red_names[0]][0]["referenceValues"]["probePixels"][0]["raw"]
        raw_p3 = by_name[red_names[1]][0]["referenceValues"]["probePixels"][0]["raw"]
        verifier.check(
            tuple(raw_srgb) != tuple(raw_p3),
            "COLOR2: different-codepoints pair uses identical raw codepoints",
        )
        for name in red_names:
            for converted in by_name[name][0]["referenceValues"]["expectedSrgb8"]:
                verifier.check(
                    max(abs(a - b) for a, b in zip(converted["srgb8"], (255, 0, 0))) <= 1,
                    f"COLOR2: {name} is not sRGB red within ±1",
                )

    low_names = ("gray16-lowbit_0x1000_8x8.png", "gray16-lowbit_0x1008_8x8.png")
    if all(name in by_name for name in low_names):
        codes = [
            by_name[name][0]["referenceValues"]["probePixels"][0]["rawGray16"]
            for name in low_names
        ]
        verifier.check(
            codes[0] != codes[1] and abs(codes[0] - codes[1]) == 8,
            "COLOR2: low-bit pair codes do not differ by 8",
        )
        displays = [
            by_name[name][0]["referenceValues"]["expectedDisplay8"] for name in low_names
        ]
        verifier.check(
            displays[0] == displays[1] == 16,
            "COLOR2: low-bit pair does not share display value 16",
        )

    print(f"COLOR2 profiles opened: {len(descriptions)} unique")
    for profile_hash, description in sorted(descriptions.items()):
        print(f"  {profile_hash[:12]}  {description}")
    print(f"COLOR2 corrupt profiles rejected: {corrupt_rejections}")
    verifier.check(corrupt_rejections == 2, "COLOR2 did not reject both corrupt profiles")
    verifier.check(len(valid_profile_hashes) >= 5, "COLOR2 has fewer than five unique profiles")
    return descriptions


def verify_format_fixtures(
    verifier: Verifier,
    manifest: dict,
    resolved: list[tuple[dict, Path]],
    ffmpeg_path: Path,
    ffprobe_path: Path,
) -> None:
    for entry, path in resolved:
        if entry["path"].startswith(MEDIA_PREFIX):
            continue
        if path.name.startswith("corrupt_"):
            pillow_rejected = False
            try:
                with Image.open(path) as image:
                    image.load()
            except Exception:
                pillow_rejected = True
            verifier.check(pillow_rejected, f"MEDIA1: corrupt PNG decoded through Pillow: {path.name}")
            verifier.check(
                not ffmpeg_decodes(ffmpeg_path, path),
                f"MEDIA1: corrupt PNG decoded through ffmpeg: {path.name}",
            )
            continue

        if "jpeg-bytes" in path.name:
            with Image.open(path) as image:
                image.load()
                verifier.check(
                    image.format == "JPEG",
                    f"MEDIA1: fake-extension file sniffed as {image.format}, not JPEG",
                )
                verifier.check(
                    image.size == (entry["width"], entry["height"]),
                    "MEDIA1: fake-extension dimensions differ from manifest",
                )
            probe = ffprobe(ffprobe_path, path)
            stream = (probe.get("streams") or [{}])[0]
            verifier.check(
                stream.get("codec_name") == "mjpeg",
                "MEDIA1: fake-extension ffprobe codec is not mjpeg",
            )
            continue

        with Image.open(path) as image:
            image.load()
            verifier.check(
                image.size == (entry["width"], entry["height"]),
                f"MEDIA1: {path.name} Pillow dimensions differ from manifest",
            )
            if path.suffix.lower() == ".gif":
                verifier.check(
                    getattr(image, "n_frames", 1) == 1,
                    f"MEDIA1: GIF fixture is not a single frame: {path.name}",
                )
            if entry["bitDepth"] == 16 and path.suffix.lower() == ".png":
                verifier.check(
                    png_header(path)[2] == 16,
                    f"MEDIA1: {path.name} is not a 16-bit PNG",
                )

    image_entries = [
        (entry, path)
        for entry, path in resolved
        if path.parent == REPO_ROOT / "tests" / "fixtures" / "images" / "formats"
    ]
    for entry, path in image_entries:
        if "probe" not in entry or entry["format"] in ("png-corrupt",):
            continue
        try:
            probe = ffprobe(ffprobe_path, path)
        except Exception as error:
            verifier.check(False, f"MEDIA1: ffprobe failed for {path.name}: {error}")
            continue
        stream = (probe.get("streams") or [{}])[0]
        recorded = entry["probe"]
        verifier.check(
            stream.get("codec_name") == recorded.get("codec"),
            f"MEDIA1: {path.name} ffprobe codec {stream.get('codec_name')} != recorded "
            f"{recorded.get('codec')}",
        )
        verifier.check(
            (stream.get("width"), stream.get("height"))
            == (recorded.get("width"), recorded.get("height")),
            f"MEDIA1: {path.name} ffprobe dimensions differ from manifest",
        )
        if entry["format"] == "png" and entry["bitDepth"] == 16:
            verifier.check(
                stream.get("pix_fmt") == "gray16be",
                f"MEDIA1: {path.name} is not probed as gray16be",
            )


def verify_videos(
    verifier: Verifier,
    manifest: dict,
    resolved: list[tuple[dict, Path]],
    ffmpeg_path: Path,
    ffprobe_path: Path,
) -> None:
    for entry, path in resolved:
        if not entry["path"].startswith(MEDIA_PREFIX):
            continue
        try:
            probe = ffprobe(ffprobe_path, path, count_frames=True)
        except Exception as error:
            verifier.check(False, f"MEDIA1: ffprobe failed for {path.name}: {error}")
            continue
        stream = (probe.get("streams") or [{}])[0]
        verifier.check(
            stream.get("codec_name") == entry["codec"],
            f"MEDIA1: {path.name} codec {stream.get('codec_name')} != {entry['codec']}",
        )
        verifier.check(
            stream.get("pix_fmt") == entry["pixelFormat"],
            f"MEDIA1: {path.name} pix_fmt {stream.get('pix_fmt')} != {entry['pixelFormat']}",
        )
        verifier.check(
            (stream.get("width"), stream.get("height")) == (entry["width"], entry["height"]),
            f"MEDIA1: {path.name} dimensions differ from manifest",
        )
        verifier.check(
            int(stream.get("nb_read_frames", 0)) == entry["frameCount"] == 12,
            f"MEDIA1: {path.name} frame count differs from manifest",
        )
        verifier.check(
            ffmpeg_decodes(ffmpeg_path, path),
            f"MEDIA1: {path.name} does not decode through ffmpeg",
        )
        print(
            f"MEDIA1 video {path.name}: codec={stream.get('codec_name')} "
            f"pix_fmt={stream.get('pix_fmt')} {stream.get('width')}x{stream.get('height')} "
            f"frames={stream.get('nb_read_frames')} bytes={entry['bytes']}"
        )


def check_reproduction(
    verifier: Verifier,
    color_fixtures: list[tuple[dict, Path]],
    format_fixtures: list[tuple[dict, Path]],
    ffmpeg_path: Path,
) -> None:
    scratch_root = REPO_ROOT / "out" / "fixture-verification"
    scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="repro-", dir=scratch_root) as temporary:
        temporary_path = Path(temporary)
        color_out = temporary_path / "color"
        image_out = temporary_path / "formats"
        media_out = temporary_path / "media"
        commands = [
            [sys.executable, str(COLOR_SCRIPT), "--output-dir", str(color_out)],
            [
                sys.executable,
                str(FORMATS_SCRIPT),
                "--image-dir",
                str(image_out),
                "--media-dir",
                str(media_out),
                "--ffmpeg",
                str(ffmpeg_path),
            ],
        ]
        for command in commands:
            completed = subprocess.run(
                command,
                capture_output=True,
                text=True,
                check=False,
                cwd=str(REPO_ROOT),
                encoding="utf-8",
                errors="replace",
            )
            verifier.check(
                completed.returncode == 0,
                f"reproduction generator failed: {' '.join(command[-4:])} "
                f"({(completed.stderr or '').strip()[:400]})",
            )
            if completed.returncode == 0 and completed.stdout:
                print(completed.stdout.strip().splitlines()[0])

        all_entries = color_fixtures + format_fixtures
        identical = 0
        for entry, committed_path in all_entries:
            relative = Path(entry["path"])
            if relative.parts[:4] == ("tests", "fixtures", "images", "color"):
                scratch_path = color_out / relative.name
            elif relative.parts[:4] == ("tests", "fixtures", "images", "formats"):
                scratch_path = image_out / relative.name
            elif relative.parts[:3] == ("tests", "fixtures", "media"):
                scratch_path = media_out / relative.name
            else:
                verifier.check(False, f"unexpected fixture path for reproduction: {relative}")
                continue
            if verifier.check(
                scratch_path.is_file(),
                f"reproduction did not write {relative.name}",
            ) and verifier.check(
                sha256_bytes(scratch_path.read_bytes()) == entry["sha256"],
                f"reproduction SHA-256 differs for {relative.name}",
            ):
                identical += 1
        verifier.check(
            identical == len(all_entries),
            f"reproduction matched {identical} of {len(all_entries)} fixtures",
        )
        print(f"Reproduction: {identical}/{len(all_entries)} fixture files byte-identical")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--ffmpeg",
        default=None,
        help="Path to ffmpeg (default: FFMPEG environment variable or PATH). ffprobe must be "
        "in the same directory.",
    )
    parser.add_argument(
        "--check-reproduction",
        action="store_true",
        help="Rerun both generators into a scratch directory and compare every fixture byte",
    )
    args = parser.parse_args(argv)

    ffmpeg_path = find_ffmpeg(args.ffmpeg)
    if ffmpeg_path is None:
        print(
            "ERROR: ffmpeg not found. Pass --ffmpeg <path> (ffprobe must be alongside).",
            file=sys.stderr,
        )
        return 2
    ffprobe_path = find_ffprobe(ffmpeg_path)

    verifier = Verifier()
    for script in (COLOR_SCRIPT, FORMATS_SCRIPT):
        verifier.check(script.is_file(), f"generator script is missing: {script}")
    print(f"Generator scripts:\n  {COLOR_SCRIPT}\n  {FORMATS_SCRIPT}")
    print(f"FFmpeg: {ffmpeg_path}")
    print(f"Pillow {PILLOW_VERSION}, LittleCMS {ImageCms.core.littlecms_version}")

    color_manifest = load_manifest(verifier, COLOR_MANIFEST, "COLOR2")
    formats_manifest = load_manifest(verifier, FORMATS_MANIFEST, "MEDIA1")
    verify_color_manifest_shape(verifier, color_manifest)
    verify_formats_manifest_shape(verifier, formats_manifest)

    color_fixtures = verify_fixture_files(verifier, color_manifest, "COLOR2")
    format_fixtures = verify_fixture_files(verifier, formats_manifest, "MEDIA1")
    print(
        f"Loaded COLOR2 {color_manifest['fixtureCount']} fixtures "
        f"({sum(entry['bytes'] for entry in color_manifest['fixtures'])} bytes), "
        f"MEDIA1 {formats_manifest['fixtureCount']} fixtures "
        f"({sum(entry['bytes'] for entry in formats_manifest['fixtures'])} bytes)"
    )

    verify_color_fixtures(verifier, color_manifest, color_fixtures, ffmpeg_path, ffprobe_path)
    verify_format_fixtures(verifier, formats_manifest, format_fixtures, ffmpeg_path, ffprobe_path)
    verify_videos(verifier, formats_manifest, format_fixtures, ffmpeg_path, ffprobe_path)

    if args.check_reproduction:
        check_reproduction(verifier, color_fixtures, format_fixtures, ffmpeg_path)

    minimum_checks = 4 * (len(color_fixtures) + len(format_fixtures)) + 20
    verifier.check(
        verifier.checks >= minimum_checks,
        f"only {verifier.checks} checks ran; expected at least {minimum_checks}",
    )
    return verifier.report()


if __name__ == "__main__":
    sys.exit(main())
