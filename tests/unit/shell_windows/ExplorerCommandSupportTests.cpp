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
    const std::filesystem::path executable =
        LR"(C:\Program Files\CompareStation\CompareStation.exe)";
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

TEST(ExplorerCommandSupportTests, AcceptsSupportedImageExtensionsCaseInsensitively) {
    EXPECT_TRUE(hasSupportedImageExtension(LR"(C:\shots\review.PNG)"));
    EXPECT_TRUE(hasSupportedImageExtension(LR"(C:\shots\review.JpEg)"));
    EXPECT_TRUE(hasSupportedImageExtension(LR"(C:\shots\review.tiff)"));
    EXPECT_TRUE(isSupportedMediaExtension(LR"(C:\shots\review.webp)"));
    EXPECT_TRUE(isSupportedMediaExtension(LR"(C:\clips\review.mp4)"));
    EXPECT_FALSE(hasSupportedImageExtension(LR"(C:\shots\review.mp4)"));
    EXPECT_FALSE(hasSupportedVideoExtension(LR"(C:\shots\review.png)"));
    EXPECT_FALSE(isSupportedMediaExtension(LR"(C:\shots\review.txt)"));
}

TEST(ExplorerCommandSupportTests, ClassifiesTheSelectionsTheCommandCanServe) {
    const std::array<std::filesystem::path, 1U> oneVideo{
        std::filesystem::path{LR"(C:\clips\a.mp4)"}};
    const std::array<std::filesystem::path, 1U> oneImage{
        std::filesystem::path{LR"(C:\shots\a.png)"}};
    const std::array<std::filesystem::path, 2U> videoPair{
        oneVideo[0], std::filesystem::path{LR"(C:\clips\b.mkv)"}};
    const std::array<std::filesystem::path, 2U> imagePair{
        oneImage[0], std::filesystem::path{LR"(C:\shots\b.JPG)"}};
    const std::array<std::filesystem::path, 3U> videoTrio{
        videoPair[0], videoPair[1], std::filesystem::path{LR"(C:\clips\c.mov)"}};
    const std::array<std::filesystem::path, 3U> imageTrio{
        imagePair[0], imagePair[1], std::filesystem::path{LR"(C:\shots\c.tif)"}};
    const std::array<std::filesystem::path, 2U> mixed{oneVideo[0], oneImage[0]};
    const std::array<std::filesystem::path, 1U> unknown{
        std::filesystem::path{LR"(C:\docs\notes.txt)"}};

    EXPECT_EQ(classifySelection(oneVideo), SelectionKind::SingleVideo);
    EXPECT_EQ(classifySelection(oneImage), SelectionKind::SingleImage);
    EXPECT_EQ(classifySelection(videoPair), SelectionKind::VideoPair);
    EXPECT_EQ(classifySelection(imagePair), SelectionKind::ImagePair);
    EXPECT_EQ(classifySelection(videoTrio), SelectionKind::VideoTrio);
    // The image workspace has two sides, so a third image has nowhere to go, and mixing a video
    // with an image would send one of them down a path that cannot decode it.
    EXPECT_EQ(classifySelection(imageTrio), SelectionKind::Unsupported);
    EXPECT_EQ(classifySelection(mixed), SelectionKind::Unsupported);
    EXPECT_EQ(classifySelection(unknown), SelectionKind::Unsupported);
    EXPECT_EQ(classifySelection(std::span<const std::filesystem::path>{}),
              SelectionKind::Unsupported);
}

} // namespace
} // namespace dvs::shell
