#include "dvs/platform/WindowsPaths.h"

#ifndef _WIN32
#error "WindowsPaths is only supported on Windows."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

// These Windows SDK headers rely on declarations supplied by windows.h. Keep their order.
// clang-format off
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
// clang-format on

#include <system_error>
#include <utility>

namespace dvs::platform {
namespace {

[[nodiscard]] PlatformError
makeError(const PlatformErrorCode code, std::filesystem::path path, std::string technicalDetail) {
    return PlatformError{
        .code = code,
        .path = std::move(path),
        .technicalDetail = std::move(technicalDetail),
    };
}

[[nodiscard]] std::string systemFailure(const char* const api, const DWORD errorCode) {
    return std::string{api} + " failed with Windows error " + std::to_string(errorCode) + ".";
}

} // namespace

PlatformResult<std::filesystem::path>
WindowsPaths::absolutePath(const std::filesystem::path& path) {
    if (path.empty()) {
        return PlatformResult<std::filesystem::path>::failure(
            makeError(PlatformErrorCode::kInvalidPath, path, "Path must not be empty."));
    }

    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        const DWORD errorCode = GetLastError();
        return PlatformResult<std::filesystem::path>::failure(makeError(
            PlatformErrorCode::kInvalidPath, path, systemFailure("GetFullPathNameW", errorCode)));
    }

    std::wstring buffer(required, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), required, buffer.data(), nullptr);
    if (written == 0 || written >= required) {
        const DWORD errorCode = GetLastError();
        return PlatformResult<std::filesystem::path>::failure(makeError(
            PlatformErrorCode::kInvalidPath, path, systemFailure("GetFullPathNameW", errorCode)));
    }

    buffer.resize(written);
    return PlatformResult<std::filesystem::path>::success(
        std::filesystem::path{buffer}.lexically_normal());
}

PlatformResult<ApplicationDataPaths> WindowsPaths::applicationDataPaths() {
    PWSTR rawPath = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &rawPath);
    if (FAILED(result) || rawPath == nullptr) {
        return PlatformResult<ApplicationDataPaths>::failure(
            makeError(PlatformErrorCode::kKnownFolderUnavailable,
                      {},
                      "SHGetKnownFolderPath(FOLDERID_LocalAppData) failed."));
    }

    const std::filesystem::path userDataDirectory =
        std::filesystem::path{rawPath} / L"CompareStation";
    CoTaskMemFree(rawPath);

    return PlatformResult<ApplicationDataPaths>::success(ApplicationDataPaths{
        .userDataDirectory = userDataDirectory,
        .settingsFile = userDataDirectory / L"settings.json",
        .proxyCacheDirectory = userDataDirectory / L"Cache",
    });
}

PlatformStatus WindowsPaths::ensureDirectory(const std::filesystem::path& path) {
    const auto absoluteResult = absolutePath(path);
    if (!absoluteResult) {
        return PlatformStatus::failure(absoluteResult.error());
    }

    const std::filesystem::path absolute = absoluteResult.value();
    std::error_code errorCode;
    const bool exists = std::filesystem::exists(absolute, errorCode);
    if (errorCode) {
        return PlatformStatus::failure(
            makeError(PlatformErrorCode::kDirectoryUnavailable, absolute, errorCode.message()));
    }

    if (exists) {
        if (std::filesystem::is_directory(absolute, errorCode) && !errorCode) {
            return PlatformStatus::success();
        }

        return PlatformStatus::failure(
            makeError(PlatformErrorCode::kDirectoryUnavailable,
                      absolute,
                      errorCode ? errorCode.message() : "Path exists but is not a directory."));
    }

    std::filesystem::create_directories(absolute, errorCode);
    if (errorCode) {
        return PlatformStatus::failure(
            makeError(PlatformErrorCode::kDirectoryCreateFailed, absolute, errorCode.message()));
    }

    if (!std::filesystem::is_directory(absolute, errorCode) || errorCode) {
        return PlatformStatus::failure(
            makeError(PlatformErrorCode::kDirectoryCreateFailed,
                      absolute,
                      errorCode ? errorCode.message() : "Created path is not a directory."));
    }

    return PlatformStatus::success();
}

} // namespace dvs::platform
