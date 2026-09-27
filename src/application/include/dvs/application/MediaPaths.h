#pragma once

#include <filesystem>
#include <span>
#include <string_view>

namespace dvs::application {

// What a path looks like by its extension alone. The desktop shell hands the application one to
// three files on its command line, and a still image has to reach the image workspace instead of
// the video session runner, which would hand it to FFmpeg and fail.
//
// This is the application's one still-image list: the folder model and the review controller used
// to carry private copies that had already drifted from the routing rule. The Explorer command
// extension keeps its own copy because it is a leaf COM server with no link to this module; keep it
// and tools/shell/RegisterExplorerCommand.ps1 in sync with stillImageExtensions().
[[nodiscard]] std::span<const std::wstring_view> stillImageExtensions();

[[nodiscard]] bool isStillImagePath(const std::filesystem::path& path);

[[nodiscard]] bool isVideoPath(const std::filesystem::path& path);

// True when the whole command line is still images: one opens as the primary image, two as a
// candidate pair. Every other selection keeps the existing video route - the review runner probes
// what it is given, so rejecting paths by extension here would refuse formats FFmpeg can open.
// Three or more images therefore stay on the video route, which is also why the Explorer command
// hides itself for them.
[[nodiscard]] bool isStillImageInvocation(std::span<const std::filesystem::path> paths);

} // namespace dvs::application
