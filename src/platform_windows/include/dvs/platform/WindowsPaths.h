#pragma once

#include "dvs/platform/PlatformResult.h"

#include <filesystem>

namespace dvs::platform {

struct ApplicationDataPaths final {
    std::filesystem::path userDataDirectory;
    std::filesystem::path settingsFile;
    std::filesystem::path proxyCacheDirectory;
};

// Windows-only path discovery. These functions intentionally use paths rather than Qt or Win32
// public types so persistence and job adapters remain independent of the shell API.
class WindowsPaths final {
public:
    [[nodiscard]] static PlatformResult<std::filesystem::path>
    absolutePath(const std::filesystem::path& path);

    // Resolves %LOCALAPPDATA%\CompareStation but does not create it. Call ensureDirectory at
    // the owning adapter boundary so tests can inject a temporary root without touching user data.
    [[nodiscard]] static PlatformResult<ApplicationDataPaths> applicationDataPaths();

    [[nodiscard]] static PlatformStatus ensureDirectory(const std::filesystem::path& path);
};

} // namespace dvs::platform
