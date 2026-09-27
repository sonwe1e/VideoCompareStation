#include "dvs/application/MediaPaths.h"

#include <array>
#include <filesystem>
#include <gtest/gtest.h>
#include <span>

namespace dvs::application {
namespace {

TEST(MediaPathsTests, RecognizesStillImageExtensionsCaseInsensitively) {
    EXPECT_TRUE(isStillImagePath(LR"(C:\shots\a.png)"));
    EXPECT_TRUE(isStillImagePath(LR"(C:\shots\b.JPG)"));
    EXPECT_TRUE(isStillImagePath(LR"(C:\shots\c.JpEg)"));
    EXPECT_TRUE(isStillImagePath(LR"(C:\shots\d.bmp)"));
    EXPECT_TRUE(isStillImagePath(LR"(C:\shots\e.gif)"));
    EXPECT_TRUE(isStillImagePath(LR"(C:\shots\f.webp)"));
    EXPECT_TRUE(isStillImagePath(LR"(C:\shots\g.tif)"));
    EXPECT_TRUE(isStillImagePath(LR"(C:\shots\h.tiff)"));
}

TEST(MediaPathsTests, RecognizesVideoExtensionsCaseInsensitively) {
    EXPECT_TRUE(isVideoPath(LR"(C:\clips\a.mp4)"));
    EXPECT_TRUE(isVideoPath(LR"(C:\clips\b.MKV)"));
    EXPECT_TRUE(isVideoPath(LR"(C:\clips\c.Mov)"));
    EXPECT_TRUE(isVideoPath(LR"(C:\clips\d.avi)"));
    EXPECT_TRUE(isVideoPath(LR"(C:\clips\e.M4V)"));
}

TEST(MediaPathsTests, RejectsUnknownExtensionsAndExtensionlessPaths) {
    EXPECT_FALSE(isStillImagePath(LR"(C:\shots\notes.txt)"));
    EXPECT_FALSE(isStillImagePath(LR"(C:\shots\noextension)"));
    EXPECT_FALSE(isStillImagePath(LR"(C:\shots\clip.mp4)"));
    EXPECT_FALSE(isVideoPath(LR"(C:\clips\notes.txt)"));
    EXPECT_FALSE(isVideoPath(LR"(C:\clips\noextension)"));
    EXPECT_FALSE(isVideoPath(LR"(C:\clips\shot.png)"));
    // A dotted directory name must not be mistaken for an extension.
    EXPECT_FALSE(isStillImagePath(LR"(C:\shots.png\frame)"));
}

TEST(MediaPathsTests, KeepsTheTwoKindsDisjoint) {
    // The shell command classifies a whole selection, so an extension that answered both ways
    // would let a mixed pair through a check meant to reject it.
    for (const auto* path : {LR"(a.png)", LR"(a.jpg)", LR"(a.tif)", LR"(a.mp4)", LR"(a.mkv)"}) {
        EXPECT_NE(isStillImagePath(path), isVideoPath(path)) << path;
    }
}

TEST(MediaPathsTests, RoutesOneOrTwoStillImagesToTheImageWorkspace) {
    const std::array<std::filesystem::path, 1U> oneImage{
        std::filesystem::path{LR"(C:\shots\a.png)"}};
    const std::array<std::filesystem::path, 2U> imagePair{
        oneImage[0], std::filesystem::path{LR"(C:\shots\b.JPEG)"}};
    EXPECT_TRUE(isStillImageInvocation(oneImage));
    EXPECT_TRUE(isStillImageInvocation(imagePair));
}

TEST(MediaPathsTests, LeavesEverythingElseOnTheVideoRoute) {
    const std::filesystem::path image{LR"(C:\shots\a.png)"};
    const std::filesystem::path video{LR"(C:\clips\a.mp4)"};
    const std::array<std::filesystem::path, 1U> oneVideo{video};
    const std::array<std::filesystem::path, 2U> videoPair{video,
                                                          std::filesystem::path{LR"(b.mkv)"}};
    const std::array<std::filesystem::path, 2U> mixed{video, image};
    const std::array<std::filesystem::path, 3U> imageTrio{
        image, std::filesystem::path{LR"(b.png)"}, std::filesystem::path{LR"(c.png)"}};
    // A format the extension list does not know still belongs to the review runner, which probes
    // what it is given instead of refusing it by name.
    const std::array<std::filesystem::path, 1U> unknown{
        std::filesystem::path{LR"(C:\clips\a.webm)"}};
    EXPECT_FALSE(isStillImageInvocation(oneVideo));
    EXPECT_FALSE(isStillImageInvocation(videoPair));
    EXPECT_FALSE(isStillImageInvocation(mixed));
    EXPECT_FALSE(isStillImageInvocation(imageTrio));
    EXPECT_FALSE(isStillImageInvocation(unknown));
    EXPECT_FALSE(isStillImageInvocation(std::span<const std::filesystem::path>{}));
}

} // namespace
} // namespace dvs::application
