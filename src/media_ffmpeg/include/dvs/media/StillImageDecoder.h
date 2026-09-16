#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dvs::media {

// CPU RGBA8 still image decoded via FFmpeg (PNG/JPEG/WebP/BMP/GIF…).
// Kept free of Qt types so the media adapter stays presentation-agnostic.
struct StillImage final {
    int width = 0;
    int height = 0;
    // Tightly packed row-major RGBA8 (alpha always 255).
    std::vector<std::uint8_t> rgba;
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
