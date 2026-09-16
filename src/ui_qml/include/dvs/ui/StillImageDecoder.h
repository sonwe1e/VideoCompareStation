#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dvs::ui {

// CPU RGBA8 still image (PNG/JPEG/BMP/GIF/WebP… via stb_image).
struct StillImage final {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

[[nodiscard]] bool decodeStillImageBytes(const std::uint8_t* data,
                                         std::size_t size,
                                         StillImage* image,
                                         std::string* error);

} // namespace dvs::ui
