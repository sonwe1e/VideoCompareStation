#include "StartupFailureReporter.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include <windows.h>

namespace dvs::app {
namespace {

[[nodiscard]] std::filesystem::path localAppDataDirectory() {
    const DWORD requiredLength = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (requiredLength > 1U) {
        std::vector<wchar_t> buffer(requiredLength);
        const DWORD written =
            GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), requiredLength);
        if (written > 0U && written < requiredLength) {
            return std::filesystem::path{buffer.data()};
        }
    }

    std::vector<wchar_t> buffer(MAX_PATH + 1U);
    const DWORD written = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (written > 0U && written < buffer.size()) {
        return std::filesystem::path{buffer.data()};
    }
    return std::filesystem::current_path();
}

[[nodiscard]] std::filesystem::path startupLogPath() {
    return localAppDataDirectory() / L"CompareStation" / L"logs" / L"CompareStation.log";
}

[[nodiscard]] bool appendStartupLog(const std::filesystem::path& logPath,
                                    const std::string_view technicalDetail) {
    std::error_code error;
    std::filesystem::create_directories(logPath.parent_path(), error);
    if (error) {
        return false;
    }

    std::ofstream stream{logPath, std::ios::app};
    if (!stream) {
        return false;
    }

    SYSTEMTIME now{};
    GetSystemTime(&now);
    stream << std::setfill('0') << std::setw(4) << now.wYear << '-' << std::setw(2) << now.wMonth
           << '-' << std::setw(2) << now.wDay << 'T' << std::setw(2) << now.wHour << ':'
           << std::setw(2) << now.wMinute << ':' << std::setw(2) << now.wSecond << '.'
           << std::setw(3) << now.wMilliseconds << "Z fatal " << technicalDetail << '\n';
    stream.flush();
    return stream.good();
}

void showStartupDialog(const std::filesystem::path& logPath, const bool logWritten) noexcept {
    try {
        std::wostringstream message;
        message << L"CompareStation could not start.";
        if (logWritten) {
            message << L"\n\nDetails were written to:\n" << logPath.wstring();
        } else {
            message << L"\n\nThe startup log could not be written.";
        }
        static_cast<void>(MessageBoxW(nullptr,
                                      message.str().c_str(),
                                      L"CompareStation startup error",
                                      MB_OK | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND));
    } catch (...) {
        static_cast<void>(MessageBoxW(nullptr,
                                      L"CompareStation could not start.",
                                      L"CompareStation startup error",
                                      MB_OK | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND));
    }
}

} // namespace

int reportFatalStartup(const std::string_view technicalDetail, const bool suppressDialog) noexcept {
    std::filesystem::path logPath;
    bool logWritten = false;
    try {
        logPath = startupLogPath();
        logWritten = appendStartupLog(logPath, technicalDetail);
    } catch (...) {
        logWritten = false;
    }

    if (!suppressDialog) {
        showStartupDialog(logPath, logWritten);
    }
    return EXIT_FAILURE;
}

} // namespace dvs::app
