#include "ExplorerCommandSupport.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <string_view>

namespace dvs::shell {
namespace {

constexpr std::array<std::wstring_view, 5U> kSupportedVideoExtensions{
    L".mp4",
    L".mkv",
    L".mov",
    L".avi",
    L".m4v",
};

constexpr std::array<std::wstring_view, 8U> kSupportedImageExtensions{
    L".png",
    L".jpg",
    L".jpeg",
    L".bmp",
    L".gif",
    L".webp",
    L".tif",
    L".tiff",
};

[[nodiscard]] bool hasExtension(const std::filesystem::path& path,
                                const std::span<const std::wstring_view> extensions) {
    std::wstring extension = path.extension().wstring();
    for (wchar_t& character : extension) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return std::ranges::find(extensions, std::wstring_view{extension}) != extensions.end();
}

[[nodiscard]] std::wstring quoteWindowsArgument(const std::wstring_view argument) {
    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t slashes = 0U;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(slashes * 2U + 1U, L'\\');
            quoted.push_back(L'"');
            slashes = 0U;
            continue;
        }
        quoted.append(slashes, L'\\');
        slashes = 0U;
        quoted.push_back(character);
    }
    quoted.append(slashes * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

} // namespace

bool hasSupportedVideoExtension(const std::filesystem::path& path) {
    return hasExtension(path, kSupportedVideoExtensions);
}

bool hasSupportedImageExtension(const std::filesystem::path& path) {
    return hasExtension(path, kSupportedImageExtensions);
}

bool isSupportedMediaExtension(const std::filesystem::path& path) {
    return hasSupportedVideoExtension(path) || hasSupportedImageExtension(path);
}

SelectionKind classifySelection(const std::span<const std::filesystem::path> paths) {
    if (paths.empty() || paths.size() > 3U) {
        return SelectionKind::Unsupported;
    }
    bool allVideo = true;
    bool allImage = true;
    for (const std::filesystem::path& path : paths) {
        allVideo = allVideo && hasSupportedVideoExtension(path);
        allImage = allImage && hasSupportedImageExtension(path);
    }
    if (allVideo) {
        switch (paths.size()) {
        case 1U:
            return SelectionKind::SingleVideo;
        case 2U:
            return SelectionKind::VideoPair;
        default:
            return SelectionKind::VideoTrio;
        }
    }
    if (allImage) {
        // The image workspace has two sides, so a third image has nowhere to go; the command stays
        // hidden instead of opening a pair and silently dropping the rest.
        switch (paths.size()) {
        case 1U:
            return SelectionKind::SingleImage;
        case 2U:
            return SelectionKind::ImagePair;
        default:
            return SelectionKind::Unsupported;
        }
    }
    // Mixing a video with an image would put one of them on a path that cannot decode it.
    return SelectionKind::Unsupported;
}

std::wstring buildReviewCommandLine(const std::filesystem::path& executable,
                                    const std::span<const std::filesystem::path> selectedPaths) {
    if (executable.empty() || selectedPaths.empty() || selectedPaths.size() > 3U) {
        return {};
    }
    std::wstring commandLine = quoteWindowsArgument(executable.wstring());
    for (const std::filesystem::path& path : selectedPaths) {
        commandLine.push_back(L' ');
        commandLine.append(quoteWindowsArgument(path.wstring()));
    }
    return commandLine;
}

} // namespace dvs::shell
