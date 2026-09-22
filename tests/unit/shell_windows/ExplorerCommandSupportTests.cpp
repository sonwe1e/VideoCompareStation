#include "ExplorerCommandSupport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <array>
#include <filesystem>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <vector>
// Windows base declarations must precede shellapi.h.
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on

namespace dvs::shell {
namespace {

[[nodiscard]] std::vector<std::wstring> parseCommandLine(const std::wstring& commandLine) {
    int count = 0;
    LPWSTR* const raw = CommandLineToArgvW(commandLine.c_str(), &count);
    if (raw == nullptr) {
        return {};
    }
    std::vector<std::wstring> arguments;
    arguments.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        arguments.emplace_back(raw[index]);
    }
    LocalFree(raw);
    return arguments;
}

TEST(ExplorerCommandSupportTests, AcceptsSupportedVideoExtensionsCaseInsensitively) {
    EXPECT_TRUE(hasSupportedVideoExtension(L"C:\\clips\\review.MP4"));
    EXPECT_TRUE(hasSupportedVideoExtension(L"C:\\clips\\review.mKv"));
    EXPECT_TRUE(hasSupportedVideoExtension(L"C:\\clips\\review.mov"));
    EXPECT_TRUE(hasSupportedVideoExtension(L"C:\\clips\\review.avi"));
    EXPECT_TRUE(hasSupportedVideoExtension(L"C:\\clips\\review.m4v"));
    EXPECT_FALSE(hasSupportedVideoExtension(L"C:\\clips\\review.txt"));
}

TEST(ExplorerCommandSupportTests, BuildsUnicodeReviewCommandWithWindowsRoundTripQuoting) {
    const std::filesystem::path executable = LR"(C:\Program Files\CompareStation\CompareStation.exe)";
    const std::array<std::filesystem::path, 2U> sources{
        std::filesystem::path{LR"(C:\素材\甲 视频.mp4)"},
        std::filesystem::path{LR"(D:\素材\乙 视频.mkv)"},
    };
    const std::wstring commandLine = buildReviewCommandLine(executable, sources);
    const std::vector<std::wstring> arguments = parseCommandLine(commandLine);
    ASSERT_EQ(arguments.size(), 3U);
    EXPECT_EQ(arguments[0], executable.wstring());
    EXPECT_EQ(arguments[1], sources[0].wstring());
    EXPECT_EQ(arguments[2], sources[1].wstring());
}

TEST(ExplorerCommandSupportTests, AcceptsOneToThreeSourcesAndRejectsOtherCounts) {
    const std::filesystem::path executable = LR"(C:\CompareStation\CompareStation.exe)";
    const std::array<std::filesystem::path, 1U> oneSource{
        std::filesystem::path{LR"(C:\clips\a.mp4)"},
    };
    const std::array<std::filesystem::path, 3U> threeSources{
        oneSource[0],
        std::filesystem::path{LR"(C:\clips\b.mp4)"},
        std::filesystem::path{LR"(C:\clips\c.mp4)"},
    };
    const std::array<std::filesystem::path, 4U> fourSources{
        threeSources[0], threeSources[1], threeSources[2], oneSource[0]};
    EXPECT_FALSE(buildReviewCommandLine(executable, oneSource).empty());
    EXPECT_FALSE(buildReviewCommandLine(executable, threeSources).empty());
    EXPECT_TRUE(
        buildReviewCommandLine(executable, std::span<const std::filesystem::path>{}).empty());
    EXPECT_TRUE(buildReviewCommandLine(executable, fourSources).empty());
}

} // namespace
} // namespace dvs::shell
