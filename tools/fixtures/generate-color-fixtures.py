#!/usr/bin/env python3
"""Generate the COLOR2 color-management fixtures and their manifest.

The fixtures are deliberately tiny real PNG/PNM files.  Every embedded ICC profile is built
here from public standard primaries, white points and transfer functions; no system profile
and no third-party binary profile data is copied into the repository.

Run with the repository's bundled Python (Pillow with LittleCMS):

    python tools/fixtures/generate-color-fixtures.py

The script writes every fixture byte-identically when run with the same Pillow, zlib and
LittleCMS versions.  The profile header timestamp is fixed, PNG compression is deterministic,
and no wall-clock value is written into any image.  The manifest's ``generationDate`` is the
only run-dependent value.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import io
import json
import struct
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageCms
from PIL import __version__ as PILLOW_VERSION

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT_DIR = REPO_ROOT / "tests" / "fixtures" / "images" / "color"
REPO_PREFIX = "tests/fixtures/images/color"
GENERATOR_SCRIPT = "tools/fixtures/generate-color-fixtures.py"
MANIFEST_NAME = "manifest.json"
MAX_FIXTURE_BYTES = 50 * 1024
MAX_SET_BYTES = 400 * 1024

PROFILE_TIMESTAMP = (2024, 1, 1, 0, 0, 0)
"""Fixed so regenerating the fixtures cannot change their bytes."""

PROFILE_COPYRIGHT = "CompareStation COLOR2 fixture profile (generated; no third-party data)"
LITTLECMS_VERSION = ImageCms.core.littlecms_version

D65 = (0.3127, 0.3290)
D50 = (0.3457, 0.3585)
BRADFORD_D65_TO_D50 = np.array(
    [
        [1.0478112, 0.0228866, -0.0501270],
        [0.0295424, 0.9904844, -0.0170491],
        [-0.0092345, 0.0150436, 0.7521316],
    ]
)

SRGB_PRIMARIES = ((0.640, 0.330), (0.300, 0.600), (0.150, 0.060))
DISPLAY_P3_PRIMARIES = ((0.680, 0.320), (0.265, 0.690), (0.150, 0.060))
ADOBE_RGB_PRIMARIES = ((0.640, 0.330), (0.210, 0.710), (0.150, 0.060))

RELATIVE_COLORIMETRIC = 1
BASE_PROBES = ((0, 0), (8, 8), (15, 0), (15, 15))
SMALL_PROBES = ((0, 0), (1, 0), (2, 0), (3, 0), (7, 7))
ALPHA_PROBES = ((0, 0), (1, 0), (2, 0), (3, 0), (4, 0))
PAIR_PROBES = ((0, 0), (1, 0), (3, 0), (7, 7))
GRAY_PROBES = ((0, 0), (5, 6), (15, 15))

# Palette chosen so the Display P3 conversion to sRGB differs measurably from the sRGB
# identity for at least one probe while the raw codepoints stay identical in both files.
SAME_CODEPOINT_PALETTE = (
    (136, 136, 119),
    (200, 120, 60),
    (50, 180, 90),
    (220, 180, 30),
    (64, 128, 192),
    (255, 0, 0),
    (0, 255, 0),
    (0, 0, 255),
)


# --------------------------------------------------------------------------------------
# ICC profile construction
# --------------------------------------------------------------------------------------


def _s15_fixed16(value: float) -> bytes:
    return struct.pack(">i", int(round(value * 65536.0)))


def _xyz_tag(x: float, y: float, z: float) -> bytes:
    return b"XYZ " + b"\0" * 4 + _s15_fixed16(x) + _s15_fixed16(y) + _s15_fixed16(z)


def _desc_tag(text: str) -> bytes:
    raw = text.encode("ascii") + b"\0"
    return (
        b"desc"
        + b"\0" * 4
        + struct.pack(">I", len(raw))
        + raw
        + struct.pack(">II", 0, 0)  # Unicode language code and count: not used.
        + struct.pack(">H", 0)  # ScriptCode code.
        + b"\0"  # ScriptCode count.
        + b"\0" * 67  # ScriptCode buffer.
    )


def _text_tag(text: str) -> bytes:
    return b"text" + b"\0" * 4 + text.encode("ascii") + b"\0"


def _curve_table_tag(values: list[float]) -> bytes:
    body = b"curv" + b"\0" * 4 + struct.pack(">I", len(values))
    body += b"".join(
        struct.pack(">H", max(0, min(65535, int(round(value * 65535.0))))) for value in values
    )
    return body


def _curve_gamma_tag(gamma: float) -> bytes:
    """ICC u8Fixed8 gamma curve (count 1).  2.2 encodes as 563 = 2.19921875."""
    return b"curv" + b"\0" * 4 + struct.pack(">I", 1) + struct.pack(">H", int(round(gamma * 256)))


def _srgb_linear_table(size: int = 4096) -> list[float]:
    values = []
    for index in range(size):
        encoded = index / (size - 1.0)
        if encoded <= 0.04045:
            values.append(encoded / 12.92)
        else:
            values.append(((encoded + 0.055) / 1.055) ** 2.4)
    return values


def _rgb_to_xyz_d65(primaries: tuple[tuple[float, float], ...]) -> np.ndarray:
    white = np.array([D65[0] / D65[1], 1.0, (1.0 - D65[0] - D65[1]) / D65[1]])
    columns = []
    for x, y in primaries:
        columns.append(np.array([x / y, 1.0, (1.0 - x - y) / y]))
    primaries_matrix = np.array(columns).T
    scale = np.linalg.solve(primaries_matrix, white)
    return primaries_matrix * scale


def _d50_adapted_matrix(primaries: tuple[tuple[float, float], ...]) -> np.ndarray:
    return BRADFORD_D65_TO_D50 @ _rgb_to_xyz_d65(primaries)


def _build_profile(data_space: bytes, tags: list[tuple[bytes, bytes]], pcs: bytes = b"XYZ ") -> bytes:
    """Build a minimal ICC v2 profile with the supplied tag list."""
    header = bytearray(128)
    header[4:8] = b"\0\0\0\0"  # Preferred CMM: none.
    header[8:12] = struct.pack(">I", 0x02100000)  # ICC 2.1.0.
    header[12:16] = b"mntr"  # Display profile.
    header[16:20] = data_space
    header[20:24] = pcs
    header[24:36] = struct.pack(">HHHHHH", *PROFILE_TIMESTAMP)
    header[36:40] = b"acsp"
    header[40:44] = b"\0\0\0\0"  # Primary platform: none.
    header[44:48] = struct.pack(">I", 0)
    header[48:52] = b"\0\0\0\0"  # Device manufacturer.
    header[52:56] = b"\0\0\0\0"  # Device model.
    header[56:64] = struct.pack(">Q", 0)  # Device attributes.
    header[64:68] = struct.pack(">I", 0)  # Rendering intent declared by the profile.
    header[68:80] = _s15_fixed16(0.9642) + _s15_fixed16(1.0) + _s15_fixed16(0.8249)  # D50.
    header[80:84] = b"\0\0\0\0"  # Profile creator.
    # Profile ID and reserved bytes stay zero; ICC v2 does not require an ID.

    ordered = sorted(tags, key=lambda item: item[0])
    offset = 128 + 4 + 12 * len(ordered)
    table = [struct.pack(">I", len(ordered))]
    data = bytearray()
    for signature, body in ordered:
        padding = (-offset) % 4
        data += b"\0" * padding
        offset += padding
        table.append(signature + struct.pack(">II", offset, len(body)))
        data += body
        offset += len(body)

    profile = bytes(header) + b"".join(table) + bytes(data)
    return struct.pack(">I", len(profile)) + profile[4:]


def _matrix_shaper_profile(
    primaries: tuple[tuple[float, float], ...],
    transfer_curves: tuple[bytes, bytes, bytes],
    description: str,
) -> bytes:
    matrix = _d50_adapted_matrix(primaries)
    tags = [
        (b"desc", _desc_tag(description)),
        (b"cprt", _text_tag(PROFILE_COPYRIGHT)),
        (b"wtpt", _xyz_tag(0.9505, 1.0, 1.0891)),  # D65 media white.
        (b"rXYZ", _xyz_tag(*matrix[:, 0])),
        (b"gXYZ", _xyz_tag(*matrix[:, 1])),
        (b"bXYZ", _xyz_tag(*matrix[:, 2])),
        (b"rTRC", transfer_curves[0]),
        (b"gTRC", transfer_curves[1]),
        (b"bTRC", transfer_curves[2]),
    ]
    return _build_profile(b"RGB ", tags)


_SRGB_CURVE = _curve_table_tag(_srgb_linear_table())

SRGB_PROFILE = _matrix_shaper_profile(
    SRGB_PRIMARIES,
    (_SRGB_CURVE, _SRGB_CURVE, _SRGB_CURVE),
    "CompareStation sRGB (ICC v2 matrix-shaper)",
)
DISPLAY_P3_PROFILE = _matrix_shaper_profile(
    DISPLAY_P3_PRIMARIES,
    (_SRGB_CURVE, _SRGB_CURVE, _SRGB_CURVE),
    "CompareStation Display P3 (ICC v2 matrix-shaper)",
)
ADOBE_RGB_PROFILE = _matrix_shaper_profile(
    ADOBE_RGB_PRIMARIES,
    (_curve_gamma_tag(2.2),) * 3,
    "CompareStation Adobe RGB (1998) (ICC v2 matrix-shaper)",
)
GRAY_PROFILE = _build_profile(
    b"GRAY",
    [
        (b"desc", _desc_tag("CompareStation Gray sRGB TRC (ICC v2)")),
        (b"cprt", _text_tag(PROFILE_COPYRIGHT)),
        (b"wtpt", _xyz_tag(0.9505, 1.0, 1.0891)),
        (b"kTRC", _SRGB_CURVE),
    ],
)
# Lab is a data colour space the application does not accept as an RGB source profile.  This
# profile is structurally loadable by LittleCMS (description and white point are readable) but
# deliberately carries no A2B0/B2A0 LUT, so it cannot be used as a transform input either.
LAB_PROFILE = _build_profile(
    b"Lab ",
    [
        (b"desc", _desc_tag("CompareStation Lab (unsupported data space sample)")),
        (b"cprt", _text_tag(PROFILE_COPYRIGHT)),
        (b"wtpt", _xyz_tag(0.9642, 1.0, 0.8249)),
    ],
    pcs=b"Lab ",
)

PROFILE_SPECS = {
    "srgb": {
        "name": "CompareStation sRGB (ICC v2 matrix-shaper)",
        "dataSpace": "RGB ",
        "pcs": "XYZ ",
        "transfer": "sRGB piecewise curve sampled at 4096 16-bit table entries",
        "bytes": SRGB_PROFILE,
    },
    "display-p3": {
        "name": "CompareStation Display P3 (ICC v2 matrix-shaper)",
        "dataSpace": "RGB ",
        "pcs": "XYZ ",
        "transfer": "sRGB piecewise curve sampled at 4096 16-bit table entries",
        "bytes": DISPLAY_P3_PROFILE,
    },
    "adobe-rgb": {
        "name": "CompareStation Adobe RGB (1998) (ICC v2 matrix-shaper)",
        "dataSpace": "RGB ",
        "pcs": "XYZ ",
        "transfer": "gamma 2.2 encoded as ICC u8Fixed8 563 (2.19921875)",
        "bytes": ADOBE_RGB_PROFILE,
    },
    "gray": {
        "name": "CompareStation Gray sRGB TRC (ICC v2)",
        "dataSpace": "GRAY",
        "pcs": "XYZ ",
        "transfer": "sRGB piecewise curve sampled at 4096 16-bit table entries",
        "bytes": GRAY_PROFILE,
    },
    "lab": {
        "name": "CompareStation Lab (unsupported data space sample)",
        "dataSpace": "Lab ",
        "pcs": "Lab ",
        "transfer": "none (no transform tags; data-space rejection sample)",
        "bytes": LAB_PROFILE,
    },
}


# --------------------------------------------------------------------------------------
# Hash and PNG helpers
# --------------------------------------------------------------------------------------


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _png_chunk(kind: bytes, body: bytes) -> bytes:
    return (
        struct.pack(">I", len(body))
        + kind
        + body
        + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    )


def _read_png_chunks(data: bytes) -> list[tuple[bytes, bytes]]:
    position = 8
    chunks = []
    while position < len(data):
        length = struct.unpack(">I", data[position : position + 4])[0]
        kind = data[position + 4 : position + 8]
        chunks.append((kind, data[position + 8 : position + 8 + length]))
        position += 12 + length
    return chunks


def _write_png_chunks(chunks: list[tuple[bytes, bytes]]) -> bytes:
    return b"\x89PNG\r\n\x1a\n" + b"".join(_png_chunk(kind, body) for kind, body in chunks)


def replace_embedded_profile(path: Path, profile: bytes) -> None:
    """Re-embed an arbitrary profile without changing the image data."""
    chunks = _read_png_chunks(path.read_bytes())
    replaced = []
    for kind, body in chunks:
        if kind == b"iCCP":
            name = body.split(b"\0", 1)[0]
            replaced.append((kind, name + b"\0\0" + zlib.compress(profile, 9)))
        else:
            replaced.append((kind, body))
    path.write_bytes(_write_png_chunks(replaced))


def embedded_profile_bytes(path: Path) -> bytes | None:
    with Image.open(path) as image:
        return image.info.get("icc_profile")


# --------------------------------------------------------------------------------------
# Image content
# --------------------------------------------------------------------------------------


def base_rgb_image() -> Image.Image:
    image = Image.new("RGB", (16, 16))
    pixels = image.load()
    for y in range(16):
        for x in range(16):
            pixels[x, y] = (x * 17, y * 17, 255 - x * 17)
    return image


def base_rgb_pixels(x: int, y: int) -> tuple[int, int, int]:
    return (x * 17, y * 17, 255 - x * 17)


def same_codepoint_pixels(x: int, y: int) -> tuple[int, int, int]:
    return SAME_CODEPOINT_PALETTE[(x + y * 8) % len(SAME_CODEPOINT_PALETTE)]


def same_codepoint_image() -> Image.Image:
    image = Image.new("RGB", (8, 8))
    pixels = image.load()
    for y in range(8):
        for x in range(8):
            pixels[x, y] = same_codepoint_pixels(x, y)
    return image


def alpha_edge_image() -> Image.Image:
    image = Image.new("RGBA", (8, 8))
    pixels = image.load()
    alphas = (0, 1, 64, 128, 255, 32, 192, 254)
    for y in range(8):
        for x in range(8):
            pixels[x, y] = (
                (200 - 9 * x) % 256,
                (40 + 13 * y) % 256,
                (90 + 7 * x) % 256,
                alphas[x],
            )
    return image


def gray_image() -> Image.Image:
    image = Image.new("L", (16, 16))
    pixels = image.load()
    for y in range(16):
        for x in range(16):
            pixels[x, y] = (x * 16 + y) % 256
    return image


def gray16_image(value: int) -> Image.Image:
    return Image.new("I;16", (8, 8), value)


def rgb16_gradient_png() -> bytes:
    rows = []
    for y in range(16):
        row = bytearray()
        for x in range(16):
            row += struct.pack(">HHH", x * 4369, y * 4369, ((x + y) * 2184) % 65536)
        rows.append(bytes(row))
    buffer = io.BytesIO()
    write_png_to_buffer(buffer, 16, 16, 16, 2, rows, SRGB_PROFILE)
    return buffer.getvalue()


def write_png_to_buffer(
    buffer: io.BytesIO,
    width: int,
    height: int,
    bit_depth: int,
    color_type: int,
    rows: list[bytes],
    profile: bytes | None = None,
) -> None:
    header = struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, 0)
    raw = b"".join(b"\0" + row for row in rows)
    chunks = [(b"IHDR", header)]
    if profile is not None:
        chunks.append((b"iCCP", b"ICC Profile\0\0" + zlib.compress(profile, 9)))
    chunks.append((b"IDAT", zlib.compress(raw, 9)))
    chunks.append((b"IEND", b""))
    buffer.write(b"\x89PNG\r\n\x1a\n" + b"".join(_png_chunk(k, b) for k, b in chunks))


# --------------------------------------------------------------------------------------
# PNM family
# --------------------------------------------------------------------------------------


def pnm_p1(width: int = 8, height: int = 8) -> bytes:
    rows = [
        " ".join("1" if (x + y) % 3 == 0 else "0" for x in range(width))
        for y in range(height)
    ]
    return f"P1\n{width} {height}\n".encode("ascii") + ("\n".join(rows) + "\n").encode("ascii")


def pnm_p2(width: int = 8, height: int = 8) -> bytes:
    rows = [" ".join(str((x * 16 + y) % 256) for x in range(width)) for y in range(height)]
    return (
        f"P2\n{width} {height}\n255\n".encode("ascii")
        + ("\n".join(rows) + "\n").encode("ascii")
    )


def pnm_p3(width: int = 8, height: int = 8) -> bytes:
    rows = [
        " ".join(
            f"{(x * 17) % 256} {(y * 17) % 256} {((x + y) * 8) % 256}" for x in range(width)
        )
        for y in range(height)
    ]
    return (
        f"P3\n{width} {height}\n255\n".encode("ascii")
        + ("\n".join(rows) + "\n").encode("ascii")
    )


def pnm_p4(width: int = 8, height: int = 8) -> bytes:
    output = bytearray(f"P4\n{width} {height}\n".encode("ascii"))
    for y in range(height):
        bits = 0
        for x in range(width):
            if (x * 7 + y * 3) % 5 < 2:
                bits |= 1 << (7 - x)
        output.append(bits)
    return bytes(output)


def pnm_p5(width: int = 8, height: int = 8, maxval: int = 255) -> bytes:
    output = bytearray(f"P5\n{width} {height}\n{maxval}\n".encode("ascii"))
    for y in range(height):
        for x in range(width):
            value = (x * 16 + y) % 256
            if maxval > 255:
                output += struct.pack(">H", value * 257)
            else:
                output.append(value)
    return bytes(output)


def pnm_p6(width: int = 8, height: int = 8) -> bytes:
    output = bytearray(f"P6\n{width} {height}\n255\n".encode("ascii"))
    for y in range(height):
        for x in range(width):
            output += bytes(
                ((x * 17) % 256, (y * 17) % 256, ((x + y) * 8) % 256)
            )
    return bytes(output)


def pam_rgba(width: int = 8, height: int = 8) -> bytes:
    output = bytearray(
        f"P7\nWIDTH {width}\nHEIGHT {height}\nDEPTH 4\nMAXVAL 255\n"
        "TUPLTYPE RGB_ALPHA\nENDHDR\n".encode("ascii")
    )
    alphas = (0, 1, 64, 128, 255)
    for y in range(height):
        for x in range(width):
            output += bytes(
                (
                    (x * 17) % 256,
                    (y * 17) % 256,
                    ((x + y) * 8) % 256,
                    alphas[x % len(alphas)],
                )
            )
    return bytes(output)


# --------------------------------------------------------------------------------------
# Reference values
# --------------------------------------------------------------------------------------


def _open_cms_profile(profile: bytes) -> ImageCms.ImageCmsProfile:
    return ImageCms.ImageCmsProfile(io.BytesIO(profile))


def _srgb_target() -> ImageCms.ImageCmsProfile:
    return ImageCms.createProfile("sRGB")


def converted_srgb8(path: Path, profile: bytes, probes: tuple[tuple[int, int], ...]) -> dict:
    """Convert the fixture through LittleCMS to built-in sRGB and return probe values."""
    source = _open_cms_profile(profile)
    target = _srgb_target()
    with Image.open(path) as image:
        image.load()
        if image.mode not in ("RGB", "RGBA", "L"):
            raise RuntimeError(f"Unexpected mode for colour reference: {image.mode}")
        # A GRAY source profile requires a single-channel input mode; converting L to RGB
        # first would make LittleCMS see an RGB image against a GRAY profile.
        source_image = image if image.mode == "L" else image.convert("RGB")
        converted = ImageCms.profileToProfile(
            source_image, source, target, renderingIntent=RELATIVE_COLORIMETRIC, outputMode="RGB"
        )
        raw_pixels = []
        expected = []
        for x, y in probes:
            raw = image.getpixel((x, y))
            if image.mode == "L":
                raw_values = [raw]
            else:
                raw_values = list(raw)
            raw_pixels.append({"x": x, "y": y, "raw": raw_values})
            expected.append({"x": x, "y": y, "srgb8": list(converted.getpixel((x, y)))})
    if image.mode == "L":
        source_expression = "image"
    else:
        source_expression = "image.convert('RGB')"
    return {
        "probePixels": raw_pixels,
        "expectedSrgb8": expected,
        "outputProfile": "Pillow ImageCms built-in sRGB",
        "renderingIntent": "relative colorimetric (LittleCMS intent 1)",
        "conversionCall": f"ImageCms.profileToProfile({source_expression}, source, "
        "ImageCms.createProfile('sRGB'), renderingIntent=1, outputMode='RGB')",
    }


def _tolerance(value: int, reason: str, applies_to: str = "each expectedSrgb8 RGB channel") -> dict:
    return {"type": "per-channel-8bit", "value": value, "appliesTo": applies_to, "reason": reason}


def _exact_tolerance(reason: str, applies_to: str) -> dict:
    return {"type": "exact", "value": 0, "appliesTo": applies_to, "reason": reason}


# --------------------------------------------------------------------------------------
# Manifest helpers
# --------------------------------------------------------------------------------------


def _profile_record(
    profile_key: str,
    embedded: bytes,
    *,
    valid: bool = True,
    corrupt_kind: str | None = None,
    note: str | None = None,
) -> dict:
    spec = PROFILE_SPECS[profile_key] if profile_key in PROFILE_SPECS else None
    record = {
        "name": spec["name"] if spec else "corrupt profile derived from CompareStation sRGB",
        "class": "mntr",
        "dataSpace": spec["dataSpace"] if spec else "RGB ",
        "pcs": spec["pcs"] if spec else "XYZ ",
        "bytes": len(embedded),
        "sha256": sha256_bytes(embedded),
        "valid": valid,
    }
    if spec:
        record["transfer"] = spec["transfer"]
    if corrupt_kind:
        record["corruptKind"] = corrupt_kind
    if note:
        record["note"] = note
    return record


def _check_profile_opens(profile: bytes) -> str:
    opened = _open_cms_profile(profile)
    return ImageCms.getProfileDescription(opened).strip()


class FixtureBuilder:
    def __init__(self, output_dir: Path) -> None:
        self.output_dir = output_dir
        self.entries: list[dict] = []
        self.checks = 0

    def add(
        self,
        filename: str,
        *,
        category: str,
        expected_purpose: str,
        generation_method: str,
        color_type: str,
        bit_depth: int,
        alpha: bool,
        png_profile_key: str | None = None,
        reference_values: dict | None = None,
        extra: dict | None = None,
        declared_size: tuple[int, int] | None = None,
    ) -> dict:
        path = self.output_dir / filename
        if not path.is_file():
            raise RuntimeError(f"Fixture was not written: {path}")
        data = path.read_bytes()
        if len(data) > MAX_FIXTURE_BYTES:
            raise RuntimeError(f"{filename} exceeds {MAX_FIXTURE_BYTES} bytes: {len(data)}")

        embedded = None
        if declared_size is not None:
            # Pillow's PPM plugin does not identify PAM (P7); the caller parsed the header.
            width, height = declared_size
        else:
            with Image.open(path) as image:
                image.load()
                width, height = image.size
                embedded = image.info.get("icc_profile")

        entry = {
            "path": f"{REPO_PREFIX}/{filename}",
            "sha256": sha256_bytes(data),
            "bytes": len(data),
            "width": width,
            "height": height,
            "bitDepth": bit_depth,
            "colorType": color_type,
            "alpha": alpha,
            "category": category,
            "embeddedProfile": None,
            "expectedPurpose": expected_purpose,
            "generationMethod": generation_method,
        }

        if embedded:
            if not png_profile_key:
                raise RuntimeError(f"{filename} unexpectedly carries an ICC profile")
            entry["embeddedProfile"] = _profile_record(png_profile_key, embedded)

        if reference_values is not None:
            entry["referenceValues"] = reference_values
        if extra:
            entry.update(extra)
        self.entries.append(entry)
        self.checks += 1
        return entry

    def total_bytes(self) -> int:
        return sum(entry["bytes"] for entry in self.entries)


def _validate_profiles() -> None:
    """Fail before touching output if any hand-built profile is unusable."""
    for key in ("srgb", "display-p3", "adobe-rgb", "gray", "lab"):
        description = _check_profile_opens(PROFILE_SPECS[key]["bytes"])
        if not description:
            raise RuntimeError(f"Profile {key} has no description")
    # The two corrupt variants must be rejected by LittleCMS, otherwise they do not exercise
    # the corrupt-profile fallback.
    broken_acsp = bytearray(SRGB_PROFILE)
    broken_acsp[36:40] = b"BAD!"
    for label, corrupted in (
        ("broken-acsp", bytes(broken_acsp)),
        ("declared-length", SRGB_PROFILE[:200]),
    ):
        try:
            _open_cms_profile(corrupted)
        except Exception:
            pass
        else:
            raise RuntimeError(f"LittleCMS accepted corrupt profile variant: {label}")


def _profile_round_trips() -> dict:
    """Convert in-gamut colours sRGB -> profile -> sRGB and record the 8-bit round-trip error."""
    colours = [(128, 64, 32), (200, 180, 160), (64, 128, 192), (30, 40, 50)]
    image = Image.new("RGB", (len(colours), 1))
    image.putdata(colours)
    source = _srgb_target()
    tolerance = 2
    results = []
    for key in ("srgb", "display-p3", "adobe-rgb"):
        profile = _open_cms_profile(PROFILE_SPECS[key]["bytes"])
        forward = ImageCms.profileToProfile(
            image, source, profile, renderingIntent=RELATIVE_COLORIMETRIC, outputMode="RGB"
        )
        round_tripped = ImageCms.profileToProfile(
            forward, profile, source, renderingIntent=RELATIVE_COLORIMETRIC, outputMode="RGB"
        )
        original_pixels = [image.getpixel((x, 0)) for x in range(len(colours))]
        forward_pixels = [forward.getpixel((x, 0)) for x in range(len(colours))]
        result_pixels = [round_tripped.getpixel((x, 0)) for x in range(len(colours))]
        max_error = max(
            abs(original - result)
            for original_pixel, result_pixel in zip(original_pixels, result_pixels)
            for original, result in zip(original_pixel, result_pixel)
        )
        if max_error > tolerance:
            raise RuntimeError(
                f"Profile round trip for {key} exceeded ±{tolerance}: {max_error}"
            )
        results.append(
            {
                "profile": key,
                "forward": [list(pixel) for pixel in forward_pixels],
                "roundTripped": [list(pixel) for pixel in result_pixels],
                "maxChannelError": max_error,
            }
        )

    # The Lab sample is intentionally not usable as an RGB input profile: LittleCMS opens it
    # (the description is readable) but cannot build a transform.
    lab = _open_cms_profile(PROFILE_SPECS["lab"]["bytes"])
    try:
        ImageCms.profileToProfile(
            image, lab, source, renderingIntent=RELATIVE_COLORIMETRIC, outputMode="RGB"
        )
    except Exception as error:
        lab_result = f"{type(error).__name__}: {error}"
    else:
        raise RuntimeError("Lab data-space profile unexpectedly built an RGB transform")

    return {
        "colours": [list(colour) for colour in colours],
        "tolerance": tolerance,
        "reason": (
            "In-gamut sRGB colours are converted to each hand-built profile and back with "
            "relative colorimetric intent. ±2 8-bit code values covers TRC table "
            "quantization and 8-bit rounding; it is not a claim about out-of-gamut "
            "appearance."
        ),
        "results": results,
        "unsupportedDataSpaceCheck": {
            "profile": "lab",
            "transformResult": lab_result,
            "expected": "rejected: Lab is not an RGB source profile and the sample has no LUT",
        },
    }


# --------------------------------------------------------------------------------------
# Generation
# --------------------------------------------------------------------------------------


def generate(output_dir: Path) -> tuple[list[dict], dict]:
    _validate_profiles()
    round_trips = _profile_round_trips()
    output_dir.mkdir(parents=True, exist_ok=True)
    builder = FixtureBuilder(output_dir)

    # (a)-(d) Four profile variants of the exact same codepoints.
    base = base_rgb_image()
    profile_variants = (
        (
            "srgb_rgb8_16x16.png",
            "srgb",
            SRGB_PROFILE,
            "srgb",
            "sRGB-tagged RGB8 baseline; same codepoints as the Display P3, Adobe RGB and "
            "untagged variants so a profile-ignored pipeline can be detected.",
        ),
        (
            "display-p3_rgb8_16x16.png",
            "display-p3",
            DISPLAY_P3_PROFILE,
            "display-p3",
            "Display P3 primaries with the sRGB transfer curve; identical codepoints to the "
            "sRGB baseline, so only the embedded profile changes the expected conversion.",
        ),
        (
            "adobe-rgb_rgb8_16x16.png",
            "adobe-rgb",
            ADOBE_RGB_PROFILE,
            "adobe-rgb",
            "Adobe RGB (1998) primaries with gamma 2.2; same codepoints as the sRGB baseline.",
        ),
        (
            "no-profile_rgb8_16x16.png",
            "no-profile",
            None,
            None,
            "No embedded profile; same codepoints as the tagged variants.  The application "
            "must report a colour state rather than silently claiming a verified profile.",
        ),
    )
    for filename, category, profile, profile_key, purpose in profile_variants:
        path = output_dir / filename
        if profile is None:
            base.save(path, format="PNG")
        else:
            base.save(path, format="PNG", icc_profile=profile)
        reference = None
        if profile is not None:
            conversion = converted_srgb8(path, profile, BASE_PROBES)
            conversion["tolerance"] = _tolerance(
                1,
                "The embedded profiles are built from public primaries with a 4096-entry "
                "16-bit TRC table; converting to Pillow's built-in sRGB involves table "
                "interpolation and 8-bit rounding, so one code value per channel is allowed. "
                "For Display P3 and Adobe RGB out-of-gamut probes, clipping at 0/255 is part "
                "of the expected LittleCMS result.",
            )
            conversion["referenceEngine"] = (
                f"Pillow {PILLOW_VERSION} ImageCms (LittleCMS {LITTLECMS_VERSION})"
            )
            reference = conversion
        else:
            reference = {
                "probePixels": [
                    {"x": x, "y": y, "raw": list(base_rgb_pixels(x, y))} for x, y in BASE_PROBES
                ],
                "expectedFallback": "raw codepoints are preserved; no colour transform is applied",
                "tolerance": _exact_tolerance(
                    "no usable embedded profile exists, so the fallback is identity",
                    "each probe codepoint",
                ),
            }
        builder.add(
            filename,
            category=category,
            expected_purpose=purpose,
            generation_method=(
                "Pillow Image.save(format='PNG')"
                + (" with a hand-built ICC v2 matrix-shaper profile embedded as iCCP"
                   if profile is not None else " without an ICC profile")
            ),
            color_type="RGB8",
            bit_depth=8,
            alpha=False,
            png_profile_key=profile_key,
            reference_values=reference,
            extra={"comparisonGroup": "same-codepoints-different-profile"},
        )

    # (e) Two corrupt ICC profiles embedded in a still-decodable PNG.
    broken_acsp = bytearray(SRGB_PROFILE)
    broken_acsp[36:40] = b"BAD!"
    corrupt_variants = (
        (
            "corrupt-profile_broken-acsp_16x16.png",
            bytes(broken_acsp),
            "broken-acsp",
            "Valid sRGB profile bytes with the 'acsp' file signature replaced by 'BAD!'; "
            "LittleCMS rejects the profile, but the PNG pixels still decode.",
        ),
        (
            "corrupt-profile_declared-length_16x16.png",
            SRGB_PROFILE[:200],
            "declared-length-mismatch",
            "Profile header still declares 25116 bytes but only the first 200 bytes are "
            "embedded; LittleCMS rejects the truncated profile, but the PNG pixels still "
            "decode.",
        ),
    )
    for filename, corrupt_profile, corrupt_kind, purpose in corrupt_variants:
        path = output_dir / filename
        base.save(path, format="PNG", icc_profile=SRGB_PROFILE)
        replace_embedded_profile(path, corrupt_profile)
        embedded = embedded_profile_bytes(path)
        assert embedded == corrupt_profile
        try:
            _open_cms_profile(embedded)
        except Exception as error:
            rejection = f"{type(error).__name__}: {error}"
        else:
            raise RuntimeError(f"Corrupt profile {filename} was accepted by LittleCMS")
        with Image.open(path) as image:
            image.load()
            assert image.size == (16, 16)
        builder.add(
            filename,
            category="corrupt-profile",
            expected_purpose=purpose,
            generation_method=(
                "Pillow PNG encode of the sRGB baseline, then the iCCP chunk is decompressed, "
                f"the profile is corrupted ({corrupt_kind}) and the chunk is rewritten"
            ),
            color_type="RGB8",
            bit_depth=8,
            alpha=False,
            png_profile_key="srgb",
            reference_values={
                "probePixels": [
                    {"x": x, "y": y, "raw": list(base_rgb_pixels(x, y))} for x, y in BASE_PROBES
                ],
                "expectedFallback": "raw codepoints are preserved; the corrupt profile is ignored",
                "tolerance": _exact_tolerance(
                    "the profile must not influence pixels; the decoder fallback is identity",
                    "each probe codepoint",
                ),
                "corruptProfileRejection": rejection,
            },
            extra={
                "corruptProfile": {
                    "kind": corrupt_kind,
                    "littleCmsOpenResult": rejection,
                    "pixelsStillDecode": True,
                }
            },
        )
        # Record the rejection against the embedded-profile record as well.
        builder.entries[-1]["embeddedProfile"] = _profile_record(
            "srgb",
            embedded,
            valid=False,
            corrupt_kind=corrupt_kind,
            note=f"LittleCMS open failed: {rejection}",
        )

    # (f) Well-formed profiles whose data colour space is not RGB.
    for filename, profile_key, profile, purpose in (
        (
            "unsupported-profile_gray-in-rgb_16x16.png",
            "gray",
            GRAY_PROFILE,
            "An RGB PNG embedding a fully valid GRAY data-space profile; the application "
            "must report unsupported/corrupt profile handling instead of treating it as an "
            "RGB source profile.",
        ),
        (
            "unsupported-profile_lab-in-rgb_16x16.png",
            "lab",
            LAB_PROFILE,
            "An RGB PNG embedding a structurally loadable Lab data-space profile with no "
            "transform tags; the data space is not RGB, so it is an unsupported source "
            "profile.",
        ),
    ):
        path = output_dir / filename
        base.save(path, format="PNG", icc_profile=profile)
        description = _check_profile_opens(embedded_profile_bytes(path))
        builder.add(
            filename,
            category="unsupported-profile",
            expected_purpose=purpose,
            generation_method=(
                "Pillow PNG encode with a hand-built ICC v2 profile whose data colour space "
                "is not RGB embedded as iCCP"
            ),
            color_type="RGB8",
            bit_depth=8,
            alpha=False,
            png_profile_key=profile_key,
            reference_values={
                "probePixels": [
                    {"x": x, "y": y, "raw": list(base_rgb_pixels(x, y))} for x, y in BASE_PROBES
                ],
                "expectedFallback": "raw codepoints are preserved; the unsupported profile is ignored",
                "tolerance": _exact_tolerance(
                    "the unsupported data space must not influence pixels",
                    "each probe codepoint",
                ),
                "profileOpensInLittleCms": description,
            },
            extra={
                "unsupportedProfile": {
                    "dataSpace": PROFILE_SPECS[profile_key]["dataSpace"],
                    "littleCmsDescription": description,
                    "reason": "data colour space is not RGB; not a valid RGB source profile",
                }
            },
        )

    # (g) Straight-alpha RGBA with a fully transparent pixel that still carries RGB data.
    alpha_path = output_dir / "alpha-edges_rgba8_8x8.png"
    alpha_edge_image().save(alpha_path, format="PNG", icc_profile=SRGB_PROFILE)
    with Image.open(alpha_path) as image:
        image.load()
        alpha_pixels = [{"x": x, "y": 0, "rawRgba": list(image.getpixel((x, 0)))} for x in range(5)]
    alpha_conversion = converted_srgb8(alpha_path, SRGB_PROFILE, ALPHA_PROBES)
    for entry, raw in zip(alpha_conversion["expectedSrgb8"], alpha_pixels):
        entry["alpha"] = raw["rawRgba"][3]
    alpha_conversion["tolerance"] = _tolerance(
        1,
        "sRGB profile conversion is identity in intent but passes through a 16-bit TRC table "
        "and 8-bit rounding; alpha is not colour data and must stay bit-exact.",
    )
    alpha_conversion["alphaTolerance"] = _exact_tolerance(
        "alpha is copied, not colour-managed", "each probe alpha value"
    )
    alpha_conversion["alphaProbes"] = [
        {"alpha": 0, "note": "fully transparent with non-zero RGB (200,40,90); hidden RGB retained"},
        {"alpha": 1, "note": "near-zero alpha boundary; strict alpha filtering must not drop it"},
        {"alpha": 64, "note": "low but visible alpha"},
        {"alpha": 128, "note": "half alpha"},
        {"alpha": 255, "note": "opaque"},
    ]
    builder.add(
        "alpha-edges_rgba8_8x8.png",
        category="alpha-edges",
        expected_purpose=(
            "Straight (non-premultiplied) RGBA8 PNG with alpha 0/1/64/128/255; the fully "
            "transparent probe keeps non-zero RGB so transparent RGB cannot be silently "
            "zeroed, and alpha filtering boundaries can be tested."
        ),
        generation_method=(
            "Pillow Image.save(format='PNG') of a generated straight-alpha RGBA image with the "
            "hand-built sRGB profile embedded"
        ),
        color_type="RGBA8",
        bit_depth=8,
        alpha=True,
        png_profile_key="srgb",
        reference_values=alpha_conversion,
    )

    # (h) Grayscale 8-bit with a matching GRAY data-space profile.
    gray_path = output_dir / "gray8_srgb-trc_16x16.png"
    gray_image().save(gray_path, format="PNG", icc_profile=GRAY_PROFILE)
    gray_conversion = converted_srgb8(gray_path, GRAY_PROFILE, GRAY_PROBES)
    gray_conversion["tolerance"] = _tolerance(
        1,
        "The GRAY profile shares the sRGB transfer curve but is a separate hand-built profile; "
        "LittleCMS applies its 4096-entry 16-bit table and rounds the sRGB output to 8 bits.",
    )
    builder.add(
        "gray8_srgb-trc_16x16.png",
        category="gray",
        expected_purpose=(
            "Grayscale 8-bit PNG with a valid GRAY data-space ICC v2 profile; exercises "
            "single-channel profile parsing and gray-to-sRGB reference conversion."
        ),
        generation_method=(
            "Pillow Image.save(format='PNG') of a generated L-mode gradient with the hand-built "
            "GRAY profile embedded"
        ),
        color_type="Gray8",
        bit_depth=8,
        alpha=False,
        png_profile_key="gray",
        reference_values=gray_conversion,
    )

    # (i) RGB48 gradient plus the low-bit-difference gray16 pair.
    rgb16_path = output_dir / "rgb16-gradient_16x16.png"
    rgb16_path.write_bytes(rgb16_gradient_png())
    with Image.open(rgb16_path) as image:
        image.load()
        assert image.size == (16, 16)
    builder.add(
        "rgb16-gradient_16x16.png",
        category="high-bit-depth",
        expected_purpose=(
            "True RGB48 PNG (16 bits per channel) with the sRGB profile embedded; exercises "
            "16-bit source depth and the RGBA64 sidecar path.  The raw file codes are the "
            "authoritative reference; display-buffer conversion may quantize."
        ),
        generation_method=(
            "Hand-written PNG (IHDR colour type 2, bit depth 16, filter 0) with a 16-bit "
            "big-endian gradient and the hand-built sRGB profile embedded; zlib level 9"
        ),
        color_type="RGB16",
        bit_depth=16,
        alpha=False,
        png_profile_key="srgb",
        reference_values={
            "probePixels": [
                {
                    "x": x,
                    "y": y,
                    "rawRgb16": [x * 4369, y * 4369, ((x + y) * 2184) % 65536],
                }
                for x, y in BASE_PROBES
            ],
            "expectedHighByte8": [
                {
                    "x": x,
                    "y": y,
                    "rgb8": [min(255, (x * 4369) >> 8), min(255, (y * 4369) >> 8),
                             min(255, (((x + y) * 2184) % 65536) >> 8)],
                }
                for x, y in BASE_PROBES
            ],
            "tolerance": _tolerance(
                1,
                "A 16-bit PNG stores 0..65535 per channel; an 8-bit display buffer takes (or "
                "rounds) the high byte, so one code value is allowed.  Assertions about "
                "precision must use the raw 16-bit codes, not the display buffer.",
                "each expectedHighByte8 channel",
            ),
            "note": "Pillow decodes this file to 8-bit RGB for display; the committed bytes "
            "remain RGB48 and ffprobe reports rgb48be.",
        },
    )

    for filename, value in (
        ("gray16-lowbit_0x1000_8x8.png", 0x1000),
        ("gray16-lowbit_0x1008_8x8.png", 0x1008),
    ):
        path = output_dir / filename
        gray16_image(value).save(path, format="PNG")
        with Image.open(path) as image:
            image.load()
            assert image.size == (8, 8)
        builder.add(
            filename,
            category="low-bit-difference",
            expected_purpose=(
                "16-bit grayscale pair whose 8-bit display value is 16 for both files while "
                "the 16-bit codes differ by 8; proves a display-only comparison cannot detect "
                "the difference and the 16-bit sidecar can."
            ),
            generation_method=(
                "Pillow Image.save(format='PNG') of a uniform 8x8 I;16 image; PNG colour "
                "type 0, bit depth 16, no ICC profile"
            ),
            color_type="Gray16",
            bit_depth=16,
            alpha=False,
            reference_values={
                "probePixels": [
                    {"x": 0, "y": 0, "rawGray16": value},
                    {"x": 7, "y": 7, "rawGray16": value},
                ],
                "expectedDisplay8": 16,
                "tolerance": _exact_tolerance(
                    "0x1000/257 = 15.94 and 0x1008/257 = 15.97; both truncation and rounding "
                    "produce exactly 16, so the 8-bit display value is identical",
                    "the 8-bit display value",
                ),
                "note": "The 16-bit raw codes differ by 8 (0x1008 - 0x1000); the RGBA64 sidecar "
                "must preserve that difference.",
            },
            extra={"lowBitPairmate": f"{REPO_PREFIX}/"
                   + ("gray16-lowbit_0x1008_8x8.png" if value == 0x1000
                      else "gray16-lowbit_0x1000_8x8.png")},
        )

    # Same codepoints, different profile: two files with identical raw RGB values.
    same_srgb_path = output_dir / "same-codepoints_srgb_8x8.png"
    same_p3_path = output_dir / "same-codepoints_p3_8x8.png"
    same_codepoint_image().save(same_srgb_path, format="PNG", icc_profile=SRGB_PROFILE)
    same_codepoint_image().save(same_p3_path, format="PNG", icc_profile=DISPLAY_P3_PROFILE)
    same_srgb = converted_srgb8(same_srgb_path, SRGB_PROFILE, PAIR_PROBES)
    same_p3 = converted_srgb8(same_p3_path, DISPLAY_P3_PROFILE, PAIR_PROBES)
    same_srgb["tolerance"] = same_p3["tolerance"] = _tolerance(
        1,
        "Relative colorimetric conversion through hand-built matrix/shaper profiles; ±1 "
        "covers 16-bit TRC interpolation and 8-bit output rounding.  The pair is useful only "
        "if the two converted results differ beyond this tolerance for at least one probe.",
    )
    differing_probes = 0
    for srgb_entry, p3_entry in zip(same_srgb["expectedSrgb8"], same_p3["expectedSrgb8"]):
        if any(abs(a - b) > 1 for a, b in zip(srgb_entry["srgb8"], p3_entry["srgb8"])):
            differing_probes += 1
    if differing_probes == 0:
        raise RuntimeError(
            "same-codepoint sRGB/Display P3 pair does not produce different converted values"
        )
    same_srgb["comparisonResult"] = (
        f"{differing_probes} of {len(PAIR_PROBES)} probes convert differently beyond ±1"
    )
    same_p3["comparisonResult"] = same_srgb["comparisonResult"]
    for filename, profile_key, reference, other in (
        ("same-codepoints_srgb_8x8.png", "srgb", same_srgb,
         "same-codepoints_p3_8x8.png"),
        ("same-codepoints_p3_8x8.png", "display-p3", same_p3,
         "same-codepoints_srgb_8x8.png"),
    ):
        builder.add(
            filename,
            category="same-codepoints-different-profile",
            expected_purpose=(
                "Identical raw RGB codepoints as its pairmate, tagged with a different ICC "
                "profile.  A pipeline that ignores the profile returns identical output; a "
                "pipeline that applies it must reproduce the recorded LittleCMS conversion."
            ),
            generation_method=(
                "Pillow Image.save(format='PNG') of the same 8x8 palette with the hand-built "
                f"{profile_key} profile embedded"
            ),
            color_type="RGB8",
            bit_depth=8,
            alpha=False,
            png_profile_key=profile_key,
            reference_values=reference,
            extra={
                "comparisonGroup": "same-codepoints-pair",
                "comparisonCounterpart": f"{REPO_PREFIX}/{other}",
            },
        )

    # Different codepoints, same expected colour: sRGB red vs a P3 code that converts to it.
    srgb_red_path = output_dir / "different-codepoints_srgb-red_8x8.png"
    p3_red_path = output_dir / "different-codepoints_p3-red_8x8.png"
    Image.new("RGB", (8, 8), (255, 0, 0)).save(
        srgb_red_path, format="PNG", icc_profile=SRGB_PROFILE
    )
    Image.new("RGB", (8, 8), (255, 16, 8)).save(
        p3_red_path, format="PNG", icc_profile=DISPLAY_P3_PROFILE
    )
    srgb_red = converted_srgb8(srgb_red_path, SRGB_PROFILE, ((0, 0), (7, 7)))
    p3_red = converted_srgb8(p3_red_path, DISPLAY_P3_PROFILE, ((0, 0), (7, 7)))
    for reference in (srgb_red, p3_red):
        reference["tolerance"] = _tolerance(
            1,
            "Both files are expected to convert to sRGB (255,0,0) within ±1.  The P3 code "
            "(255,16,8) is outside the sRGB gamut; relative colorimetric intent clips it to "
            "the sRGB red primary, and the final 8-bit rounding is allowed one code value.",
        )
        reference["conversionExpectation"] = "sRGB8 (255, 0, 0) within ±1 per channel"
    if any(
        max(abs(channel - target) for channel, target in zip(probe["srgb8"], (255, 0, 0))) > 1
        for probe in p3_red["expectedSrgb8"]
    ):
        raise RuntimeError("P3 red candidate does not convert to sRGB (255,0,0) within ±1")
    if tuple(p3_red["probePixels"][0]["raw"]) == (255, 0, 0):
        raise RuntimeError("P3 red candidate must use different raw codepoints from the sRGB red")
    srgb_red["comparisonResult"] = "raw (255,0,0) converts to sRGB (255,0,0)"
    p3_red["comparisonResult"] = (
        "raw (255,16,8) converts to sRGB " + str(tuple(p3_red["expectedSrgb8"][0]["srgb8"]))
    )
    for filename, profile_key, reference, other, purpose in (
        (
            "different-codepoints_srgb-red_8x8.png",
            "srgb",
            srgb_red,
            "different-codepoints_p3-red_8x8.png",
            "sRGB-coded pure red (255,0,0); the reference member of the different-codepoints, "
            "same-expected-colour pair.",
        ),
        (
            "different-codepoints_p3-red_8x8.png",
            "display-p3",
            p3_red,
            "different-codepoints_srgb-red_8x8.png",
            "Display P3 code (255,16,8) whose LittleCMS relative-colorimetric conversion to "
            "sRGB is (255,0,0) within ±1; same expected colour as the sRGB red, different "
            "raw codepoints.",
        ),
    ):
        builder.add(
            filename,
            category="different-codepoints-same-color",
            expected_purpose=purpose,
            generation_method=(
                "Pillow Image.save(format='PNG') of a uniform 8x8 colour with the hand-built "
                f"{profile_key} profile embedded"
            ),
            color_type="RGB8",
            bit_depth=8,
            alpha=False,
            png_profile_key=profile_key,
            reference_values=reference,
            extra={
                "comparisonGroup": "different-codepoints-same-color-pair",
                "comparisonCounterpart": f"{REPO_PREFIX}/{other}",
            },
        )

    # (k) PNM family, one real file per variant.
    pnm_variants = (
        (
            "pnm_p1_bitmap_8x8.pbm",
            pnm_p1,
            "P1",
            1,
            "PBM",
            "ASCII bitmap (P1); 1 is black, 0 is white; decoder entry is AV_CODEC_ID_PBM.",
            _exact_tolerance("PBM stores one bit per pixel; no colour conversion applies",
                             "decoded dimensions and pixel bits"),
        ),
        (
            "pnm_p2_gray_ascii_8x8.pgm",
            pnm_p2,
            "P2",
            8,
            "PGM",
            "ASCII grayscale (P2) with maxval 255; decoder entry is AV_CODEC_ID_PGM.",
            _exact_tolerance("ASCII samples are exact integers; no colour conversion applies",
                             "decoded dimensions and gray values"),
        ),
        (
            "pnm_p3_rgb_ascii_8x8.ppm",
            pnm_p3,
            "P3",
            8,
            "PPM",
            "ASCII RGB (P3) with maxval 255; decoder entry is AV_CODEC_ID_PPM.",
            _exact_tolerance("ASCII samples are exact integers; no colour conversion applies",
                             "decoded dimensions and RGB values"),
        ),
        (
            "pnm_p4_bitmap_binary_8x8.pbm",
            pnm_p4,
            "P4",
            1,
            "PBM",
            "Binary bitmap (P4), MSB first, 1 is black; decoder entry is AV_CODEC_ID_PBM.",
            _exact_tolerance("PBM stores one bit per pixel; padding bits are zero",
                             "decoded dimensions and pixel bits"),
        ),
        (
            "pnm_p5_gray_8bit_8x8.pgm",
            pnm_p5,
            "P5",
            8,
            "PGM",
            "Binary 8-bit grayscale (P5) with maxval 255; decoder entry is AV_CODEC_ID_PGM.",
            _exact_tolerance("8-bit samples are byte-exact; no colour conversion applies",
                             "decoded dimensions and gray values"),
        ),
        (
            "pnm_p5_gray_16bit_8x8.pgm",
            lambda: pnm_p5(maxval=65535),
            "P5",
            16,
            "PGM",
            "Binary 16-bit grayscale (P5) with maxval 65535 and big-endian samples; decoder "
            "entry is AV_CODEC_ID_PGM and the source depth must be retained.",
            _exact_tolerance("16-bit samples are stored big-endian; the source bit depth must "
                             "remain 16", "decoded dimensions and 16-bit gray codes"),
        ),
        (
            "pnm_p6_rgb_8bit_8x8.ppm",
            pnm_p6,
            "P6",
            8,
            "PPM",
            "Binary 8-bit RGB (P6) with maxval 255; decoder entry is AV_CODEC_ID_PPM.",
            _exact_tolerance("8-bit samples are byte-exact; no colour conversion applies",
                             "decoded dimensions and RGB values"),
        ),
        (
            "pnm_p7_rgba_8x8.pam",
            pam_rgba,
            "P7",
            8,
            "PAM",
            "PAM (P7) RGBA with DEPTH 4 and TUPLTYPE RGB_ALPHA; decoder entry is "
            "AV_CODEC_ID_PAM.  Pillow's PPM plugin does not identify PAM; the application "
            "decodes it through FFmpeg and the verifier uses ffprobe.",
            _exact_tolerance("8-bit samples are byte-exact; alpha is stored straight",
                             "decoded dimensions and RGBA values"),
        ),
    )
    for filename, builder_function, variant, bit_depth, color_type, purpose, tolerance in pnm_variants:
        path = output_dir / filename
        path.write_bytes(builder_function())
        builder.add(
            filename,
            category="pnm",
            expected_purpose=purpose,
            generation_method=(
                f"Hand-written {variant} PNM payload with ASCII header and deterministic "
                "samples; no image library is used for the file bytes"
            ),
            color_type=color_type,
            bit_depth=bit_depth,
            alpha=color_type == "PAM",
            png_profile_key=None,
            reference_values={
                "probePixels": [{"x": 0, "y": 0, "note": "first raster sample"}],
                "tolerance": tolerance,
                "note": "PNM has no ICC mechanism; category 'pnm' exists to test the format "
                "entry contract, not colour management.",
            },
            declared_size=(8, 8) if variant == "P7" else None,
        )

    total = builder.total_bytes()
    if total > MAX_SET_BYTES:
        raise RuntimeError(f"Colour fixture set is too large: {total} > {MAX_SET_BYTES} bytes")
    if len(builder.entries) < 20:
        raise RuntimeError(f"Colour fixture set is incomplete: {len(builder.entries)} < 20")
    return builder.entries, round_trips


# --------------------------------------------------------------------------------------
# Manifest
# --------------------------------------------------------------------------------------


def build_manifest(entries: list[dict], round_trips: dict) -> dict:
    categories: dict[str, int] = {}
    for entry in entries:
        categories[entry["category"]] = categories.get(entry["category"], 0) + 1

    profiles = []
    for key in ("srgb", "display-p3", "adobe-rgb", "gray", "lab"):
        spec = PROFILE_SPECS[key]
        profiles.append(
            {
                "key": key,
                "name": spec["name"],
                "class": "mntr",
                "dataSpace": spec["dataSpace"],
                "pcs": spec["pcs"],
                "transfer": spec["transfer"],
                "bytes": len(spec["bytes"]),
                "sha256": sha256_bytes(spec["bytes"]),
            }
        )

    return {
        "fixtureFormatVersion": 1,
        "fixtureSet": "COLOR2",
        "fixtureCount": len(entries),
        "directory": REPO_PREFIX,
        "pathBase": "repository-root",
        "generatorScript": GENERATOR_SCRIPT,
        "generationDate": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"),
        "referenceTool": {
            "engine": "Pillow ImageCms (LittleCMS 2)",
            "pillowVersion": PILLOW_VERSION,
            "littlecmsVersion": LITTLECMS_VERSION,
            "pythonVersion": sys.version.split()[0],
            "outputProfile": "Pillow ImageCms built-in sRGB",
            "renderingIntent": "relative colorimetric (LittleCMS intent 1)",
            "conversionCall": "ImageCms.profileToProfile(image.convert('RGB'), source, "
            "ImageCms.createProfile('sRGB'), renderingIntent=1, outputMode='RGB')",
            "independenceNote": (
                "LittleCMS is the reference engine here.  If the application later uses "
                "LittleCMS too, these converted values are not an independent-engine check; "
                "the raw codepoints, profile sha256 values and standard primaries/white "
                "points remain the anchors.  A Qt QColorSpace result must be compared to "
                "these values under the stated tolerance, not generated from them."
            ),
        },
        "profiles": profiles,
        "profileRoundTrips": round_trips,
        "profileTimestamp": "2024-01-01T00:00:00",
        "categoryCounts": dict(sorted(categories.items())),
        "reproducibility": (
            "Every image is written without wall-clock metadata.  The ICC header timestamp "
            "is fixed at 2024-01-01T00:00:00 and zlib level 9 is used.  Regeneration is "
            "byte-identical with the same Pillow, zlib and LittleCMS versions; the "
            "generationDate field is the only run-dependent manifest value."
        ),
        "license": (
            "All fixture bytes are generated by this script from public standard primaries, "
            "white points and transfer functions.  No third-party ICC profile or image data "
            "is copied into the repository."
        ),
        "fixtures": entries,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
        help="Directory to write fixtures and manifest.json into (default: %(default)s)",
    )
    args = parser.parse_args(argv)
    output_dir = args.output_dir.resolve()

    entries, round_trips = generate(output_dir)
    manifest = build_manifest(entries, round_trips)
    manifest_path = output_dir / MANIFEST_NAME
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n"
    )

    total = sum(entry["bytes"] for entry in entries)
    print(f"Wrote {len(entries)} colour fixtures ({total} bytes) to {output_dir}")
    print(f"Wrote {manifest_path.relative_to(REPO_ROOT) if manifest_path.is_relative_to(REPO_ROOT) else manifest_path}")
    print(f"Categories: {json.dumps(manifest['categoryCounts'], sort_keys=True)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
