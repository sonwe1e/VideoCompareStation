#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dvs::media {

// CPU RGBA8 still image decoded via FFmpeg (PNG/JPEG/WebP/BMP/GIF/PNM/TIFF…).
// Kept free of Qt types so the media adapter stays presentation-agnostic.
struct StillImage final {
    int width = 0;
    int height = 0;
    // Tightly packed row-major RGBA8 display buffer. Alpha is straight (unassociated) by
    // convention: swscale copies source alpha without premultiplication. The source fields
    // below describe the decoded frame the buffer was converted from, so callers can tell
    // display values from original code values (e.g. a 16-bit PGM shows as 8-bit RGBA here).
    std::vector<std::uint8_t> rgba;
    // Original-depth sidecar: tightly packed row-major RGBA64LE (16 bits per component),
    // produced only when the decoded source carries more than 8 bits per component. Empty
    // for 8-bit sources, where the display buffer already holds the original code values.
    std::vector<std::uint16_t> rgba16;
    // Bits per component of the decoded source frame (8/10/12/16…).
    int sourceBitDepth = 8;
    // Decoded source channel count (1 = gray, 3 = RGB, 4 = RGBA).
    int sourceChannels = 4;
    // The decoded source frame carried an alpha channel.
    bool hasAlpha = false;
    // FFmpeg pixel format name of the decoded source frame, e.g. "rgba", "gray16be",
    // "rgb48be", "yuvj420p"; empty when unknown.
    std::string sourceFormat;
    // FFmpeg AVCOL_RANGE of the source frame, or -1 when unknown.
    int colorRange = -1;
};

// Decode from an in-memory container (Unicode-path safe: read the file first).
[[nodiscard]] bool decodeStillImageBytes(const std::uint8_t* data,
                                         std::size_t size,
                                         StillImage* image,
                                         std::string* error);

// Parse container/codec metadata only so callers can reject oversized images before
// allocating decoder output. Returns false without a decoder context being opened twice.
[[nodiscard]] bool probeStillImageBytes(
    const std::uint8_t* data, std::size_t size, int* width, int* height, std::string* error);

[[nodiscard]] bool
decodeStillImageFile(const std::string& path, StillImage* image, std::string* error);

} // namespace dvs::media
