#pragma once

#include <filesystem>
#include <optional>

namespace dvs::app {

[[nodiscard]] std::optional<std::filesystem::path> playbackTracePathFromEnvironment();

} // namespace dvs::app
