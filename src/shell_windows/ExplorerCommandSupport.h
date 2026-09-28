#pragma once

#include <filesystem>
#include <span>
#include <string>

namespace dvs::shell {

// What the current Explorer selection can be used for. The command is hidden for anything else,
// which keeps "one to three videos" and "one or two images" the only accepted gestures.
enum class SelectionKind {
    Unsupported,
    SingleVideo,
    SingleImage,
    VideoPair,
    ImagePair,
    VideoTrio,
};

// Extension sets must match dvs::application::isStillImagePath / isVideoPath, which decide how the
// application routes the paths this command line produces, and the defaults of
// tools/shell/RegisterExplorerCommand.ps1, which decides which files offer the command at all.
[[nodiscard]] bool hasSupportedVideoExtension(const std::filesystem::path& path);
[[nodiscard]] bool hasSupportedImageExtension(const std::filesystem::path& path);
[[nodiscard]] bool isSupportedMediaExtension(const std::filesystem::path& path);

// The two extension sets themselves, for the code that has to write the per-user registration. It
// reads these instead of keeping a copy, so the entry can never be offered for a file the handler
// then refuses - which is exactly how the shipped registration script and the handler drifted
// apart.
[[nodiscard]] std::span<const std::wstring_view> supportedVideoExtensions();
[[nodiscard]] std::span<const std::wstring_view> supportedImageExtensions();

[[nodiscard]] SelectionKind classifySelection(std::span<const std::filesystem::path> paths);

[[nodiscard]] std::wstring
buildReviewCommandLine(const std::filesystem::path& executable,
                       std::span<const std::filesystem::path> selectedPaths);

} // namespace dvs::shell
