#include "dvs/application/MediaPaths.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <span>
#include <string>
#include <string_view>

namespace dvs::application {
namespace {

// PNM (PBM/PGM/PPM/PAM) is a first-class decoded input, not a fallback, so it belongs in the
// routing list as much as in the folder scan: a .ppm handed to the command line used to reach the
// video probe while the same file dropped on the window opened as an image.
constexpr std::array<std::wstring_view, 13U> kStillImageExtensions{
    L".png",
    L".jpg",
    L".jpeg",
    L".bmp",
    L".gif",
    L".webp",
    L".tif",
    L".tiff",
    L".pnm",
    L".ppm",
    L".pgm",
    L".pbm",
    L".pam",
};

constexpr std::array<std::wstring_view, 5U> kVideoExtensions{
    L".mp4",
    L".mkv",
    L".mov",
    L".avi",
    L".m4v",
};

[[nodiscard]] bool hasExtension(const std::filesystem::path& path,
                                const std::span<const std::wstring_view> extensions) {
    std::wstring extension = path.extension().wstring();
    for (wchar_t& character : extension) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return std::ranges::find(extensions, std::wstring_view{extension}) != extensions.end();
}

} // namespace

std::span<const std::wstring_view> stillImageExtensions() {
    return kStillImageExtensions;
}

bool isStillImagePath(const std::filesystem::path& path) {
    return hasExtension(path, kStillImageExtensions);
}

bool isVideoPath(const std::filesystem::path& path) {
    return hasExtension(path, kVideoExtensions);
}

bool isStillImageInvocation(const std::span<const std::filesystem::path> paths) {
    if (paths.empty() || paths.size() > 2U) {
        return false;
    }
    return std::ranges::all_of(
        paths, [](const std::filesystem::path& path) { return isStillImagePath(path); });
}

} // namespace dvs::application
