#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO

#include "dvs/ui/StillImageDecoder.h"

#include "../third_party/stb_image.h"

#include <cstring>

namespace dvs::ui {

bool decodeStillImageBytes(const std::uint8_t* data,
                           const std::size_t size,
                           StillImage* image,
                           std::string* error) {
    if (image == nullptr || data == nullptr || size == 0U || size > 0x7FFFFFFFU) {
        if (error != nullptr) {
            *error = "Invalid still-image request.";
        }
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        data, static_cast<int>(size), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        if (error != nullptr) {
            const char* reason = stbi_failure_reason();
            *error = reason != nullptr ? reason : "Could not decode image.";
        }
        if (pixels != nullptr) {
            stbi_image_free(pixels);
        }
        return false;
    }
    if (width > 16384 || height > 16384) {
        stbi_image_free(pixels);
        if (error != nullptr) {
            *error = "Image dimensions are too large.";
        }
        return false;
    }

    const auto byteCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    image->width = width;
    image->height = height;
    image->rgba.resize(byteCount);
    std::memcpy(image->rgba.data(), pixels, byteCount);
    stbi_image_free(pixels);
    return true;
}

} // namespace dvs::ui
