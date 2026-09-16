#pragma once

#include <filesystem>
#include <span>
#include <string>

namespace dvs::shell {

[[nodiscard]] bool hasSupportedVideoExtension(const std::filesystem::path& path);
[[nodiscard]] std::wstring
buildReviewCommandLine(const std::filesystem::path& executable,
                       std::span<const std::filesystem::path> selectedPaths);

} // namespace dvs::shell
