#include "dvs/ui/ImageColorContract.h"
#include "dvs/ui/ImagePairLoader.h"
#include "dvs/ui/ImageReviewController.h"

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <chrono>
#include <cstring>
#include <gtest/gtest.h>

namespace {

using dvs::ui::classifyImageColorProfile;
using dvs::ui::ImageColorInfo;
using dvs::ui::ImageColorProfileStatus;
using dvs::ui::imageColorProfileStatusToken;
using dvs::ui::ImageColorSourceLayout;
using dvs::ui::imagePixelPathIsFileValues;
using dvs::ui::ImagePixelValuePath;
using dvs::ui::imagePixelValuePathToken;
using dvs::ui::ImageReviewController;
using dvs::ui::StillImageSourceInfo;

void ensureCoreApplication() {
    if (QCoreApplication::instance() != nullptr) {
        return;
    }
    static int argumentCount = 1;
    static char applicationName[] = "ImageColorContractTests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application{argumentCount, arguments};
    static_cast<void>(application);
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate,
                             const std::chrono::milliseconds timeout = std::chrono::seconds(8)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(1U);
    }
    return predicate();
}

class CoreApplicationEnvironment final : public ::testing::Environment {
public:
    void SetUp() override {
        ensureCoreApplication();
    }
};

[[maybe_unused]] const bool kCoreApplicationEnvironmentRegistered =
    ::testing::AddGlobalTestEnvironment(new CoreApplicationEnvironment) != nullptr;

// Minimal structurally valid ICC header: the test only exercises the contract classifier, so
// the tag table and payload stay absent while the header fields it reads are exact.
[[nodiscard]] QByteArray iccHeader(const char* dataSpace,
                                   const char* profileClass,
                                   const int versionMajor,
                                   const char* pcs = "XYZ ") {
    QByteArray bytes(128, '\0');
    const auto writeSize = [&bytes](const std::uint32_t value) {
        for (int index = 0; index < 4; ++index) {
            bytes[index] = static_cast<char>((value >> (24 - 8 * index)) & 0xffU);
        }
    };
    writeSize(128U);
    bytes[8] = static_cast<char>(versionMajor);
    bytes[9] = static_cast<char>(0x40); // 4.4 style minor/bugfix nibbles
    std::memcpy(bytes.data() + 12, profileClass, 4);
    std::memcpy(bytes.data() + 16, dataSpace, 4);
    std::memcpy(bytes.data() + 20, pcs, 4);
    std::memcpy(bytes.data() + 36, "acsp", 4);
    return bytes;
}

[[nodiscard]] QImage solidImage(const QColor& color) {
    QImage image(4, 4, QImage::Format_ARGB32);
    image.fill(color);
    return image;
}

[[nodiscard]] StillImageSourceInfo rgba8Info() {
    StillImageSourceInfo info;
    info.bitDepth = 8;
    info.channels = 4;
    info.hasAlpha = true;
    info.straightAlpha = true;
    info.sourceFormat = QStringLiteral("rgba");
    info.displayConverted = false;
    return info;
}

[[nodiscard]] StillImageSourceInfo convertedInfo() {
    StillImageSourceInfo info;
    info.bitDepth = 16;
    info.channels = 4;
    info.hasAlpha = true;
    info.sourceFormat = QStringLiteral("rgb48be");
    info.displayConverted = true;
    return info;
}

TEST(ImageColorContractTests, EmptyProfileIsNoneNotCorrupt) {
    const ImageColorInfo info = classifyImageColorProfile(QByteArray{});
    EXPECT_EQ(info.profileStatus, ImageColorProfileStatus::None);
    EXPECT_TRUE(info.profileSha256.isEmpty());
    EXPECT_FALSE(info.conversionApplied);
    EXPECT_EQ(info.profileVersionMajor, 0);
}

TEST(ImageColorContractTests, TruncatedBytesAreCorrupt) {
    const QByteArray shortBytes = iccHeader("RGB ", "mntr", 4).left(127);
    const ImageColorInfo info = classifyImageColorProfile(shortBytes);
    EXPECT_EQ(info.profileStatus, ImageColorProfileStatus::Corrupt);
    EXPECT_TRUE(info.profileSha256.isEmpty());
}

TEST(ImageColorContractTests, DeclaredSizeBeyondBufferIsCorrupt) {
    QByteArray bytes = iccHeader("RGB ", "mntr", 4);
    bytes[3] = static_cast<char>(255); // declared size far beyond the payload
    EXPECT_EQ(classifyImageColorProfile(bytes).profileStatus, ImageColorProfileStatus::Corrupt);
}

TEST(ImageColorContractTests, MissingAcspSignatureIsCorrupt) {
    QByteArray bytes = iccHeader("RGB ", "mntr", 4);
    bytes[36] = 'x';
    const ImageColorInfo info = classifyImageColorProfile(bytes);
    EXPECT_EQ(info.profileStatus, ImageColorProfileStatus::Corrupt);
    EXPECT_TRUE(info.profileDataSpace.isEmpty());
}

TEST(ImageColorContractTests, SupportedRgbAndGrayProfilesKeepSignaturesAndHash) {
    const QByteArray rgb = iccHeader("RGB ", "mntr", 4);
    const ImageColorInfo rgbInfo = classifyImageColorProfile(rgb);
    EXPECT_EQ(rgbInfo.profileStatus, ImageColorProfileStatus::EmbeddedSupported);
    EXPECT_EQ(rgbInfo.profileDataSpace, QStringLiteral("RGB "));
    EXPECT_EQ(rgbInfo.profileClass, QStringLiteral("mntr"));
    EXPECT_EQ(rgbInfo.profileVersionMajor, 4);
    EXPECT_EQ(rgbInfo.profileSha256.size(), 64);
    EXPECT_FALSE(rgbInfo.conversionApplied);

    // A profile being supported says nothing about the display: the status stays "declared".
    const ImageColorInfo grayInfo =
        classifyImageColorProfile(iccHeader("GRAY", "mntr", 2), ImageColorSourceLayout::Gray);
    EXPECT_EQ(grayInfo.profileStatus, ImageColorProfileStatus::EmbeddedSupported);
    EXPECT_EQ(grayInfo.profileDataSpace, QStringLiteral("GRAY"));

    // PCS Lab is a legal v4 profile; support does not depend on a PCS of XYZ.
    const ImageColorInfo labPcs = classifyImageColorProfile(iccHeader("RGB ", "mntr", 4, "Lab "));
    EXPECT_EQ(labPcs.profileStatus, ImageColorProfileStatus::EmbeddedSupported);
}

TEST(ImageColorContractTests, ProfileMustMatchTheSourceLayout) {
    // A structurally valid gray profile on RGB data (and the reverse) is not applicable:
    // it must stay an explicit fallback instead of becoming a "usable source profile".
    const QByteArray grayProfile = iccHeader("GRAY", "mntr", 2);
    EXPECT_EQ(classifyImageColorProfile(grayProfile, ImageColorSourceLayout::Rgb).profileStatus,
              ImageColorProfileStatus::EmbeddedUnsupported);
    EXPECT_EQ(classifyImageColorProfile(grayProfile, ImageColorSourceLayout::Gray).profileStatus,
              ImageColorProfileStatus::EmbeddedSupported);

    const QByteArray rgbProfile = iccHeader("RGB ", "mntr", 4);
    EXPECT_EQ(classifyImageColorProfile(rgbProfile, ImageColorSourceLayout::Gray).profileStatus,
              ImageColorProfileStatus::EmbeddedUnsupported);
    EXPECT_EQ(classifyImageColorProfile(rgbProfile, ImageColorSourceLayout::Rgb).profileStatus,
              ImageColorProfileStatus::EmbeddedSupported);
}

TEST(ImageColorContractTests, UnsupportedSpaceClassAndVersionAreNotCorruptButNotApplied) {
    for (const QByteArray& bytes : {iccHeader("CMYK", "prtr", 2),
                                    iccHeader("RGB ", "zzzz", 4),
                                    iccHeader("RGB ", "mntr", 5),
                                    iccHeader("XYZ ", "mntr", 4)}) {
        const ImageColorInfo info = classifyImageColorProfile(bytes);
        EXPECT_EQ(info.profileStatus, ImageColorProfileStatus::EmbeddedUnsupported)
            << info.profileDataSpace.toStdString() << " " << info.profileClass.toStdString();
        EXPECT_FALSE(info.conversionApplied);
    }
}

TEST(ImageColorContractTests, ProfileHashIsDeterministicAndContentSensitive) {
    const QByteArray profile = iccHeader("RGB ", "mntr", 4);
    const QString first = classifyImageColorProfile(profile).profileSha256;
    EXPECT_EQ(first, classifyImageColorProfile(profile).profileSha256);
    QByteArray changed = profile;
    changed[100] = static_cast<char>(1);
    EXPECT_NE(first, classifyImageColorProfile(changed).profileSha256);
}

TEST(ImageColorContractTests, TokensAreStableAndFileValuePathIsExplicit) {
    EXPECT_STREQ(imageColorProfileStatusToken(ImageColorProfileStatus::NotProbed), "not-probed");
    EXPECT_STREQ(imageColorProfileStatusToken(ImageColorProfileStatus::None), "none");
    EXPECT_STREQ(imageColorProfileStatusToken(ImageColorProfileStatus::EmbeddedSupported),
                 "embedded-supported");
    EXPECT_STREQ(imageColorProfileStatusToken(ImageColorProfileStatus::EmbeddedUnsupported),
                 "embedded-unsupported");
    EXPECT_STREQ(imageColorProfileStatusToken(ImageColorProfileStatus::Corrupt), "corrupt");
    EXPECT_STREQ(imagePixelValuePathToken(ImagePixelValuePath::FileRgba8CodeValues),
                 "file-rgba8-code-values");
    EXPECT_STREQ(imagePixelValuePathToken(ImagePixelValuePath::DecodedDisplayRgba8),
                 "decoded-display-rgba8");
    EXPECT_TRUE(imagePixelPathIsFileValues(ImagePixelValuePath::FileRgba8CodeValues));
    EXPECT_FALSE(imagePixelPathIsFileValues(ImagePixelValuePath::DecodedDisplayRgba8));
    EXPECT_FALSE(imagePixelPathIsFileValues(ImagePixelValuePath::DisplayDerivative));
}

TEST(ImageColorContractTests, ControllerExposesPerSideProfileStateAndAnalysisPath) {
    ImageReviewController controller;
    StillImageSourceInfo primary = convertedInfo();
    primary.color.profileStatus = ImageColorProfileStatus::EmbeddedSupported;
    primary.color.profileSha256 = QStringLiteral("abc123");
    primary.color.profileDataSpace = QStringLiteral("RGB ");
    primary.color.profileClass = QStringLiteral("mntr");
    primary.color.profileVersionMajor = 4;
    primary.color.declaredPrimaries = 1;
    primary.color.declaredTransfer = 13;
    primary.color.declaredColorSpace = 1;
    StillImageSourceInfo secondary = rgba8Info();
    secondary.color.profileStatus = ImageColorProfileStatus::None;

    ASSERT_TRUE(controller.openPairImages(solidImage(QColor(10, 20, 30)),
                                          QStringLiteral("a.png"),
                                          solidImage(QColor(10, 20, 32)),
                                          QStringLiteral("b.png"),
                                          -1,
                                          primary,
                                          secondary));

    EXPECT_EQ(controller.primaryColorProfileStatus(),
              static_cast<int>(ImageColorProfileStatus::EmbeddedSupported));
    EXPECT_EQ(controller.primaryColorProfileSha256(), QStringLiteral("abc123"));
    EXPECT_EQ(controller.primaryColorProfileDataSpace(), QStringLiteral("RGB "));
    EXPECT_EQ(controller.primaryColorProfileClass(), QStringLiteral("mntr"));
    EXPECT_EQ(controller.primaryColorProfileVersionMajor(), 4);
    EXPECT_EQ(controller.primaryDeclaredPrimaries(), 1);
    EXPECT_EQ(controller.primaryDeclaredTransfer(), 13);
    EXPECT_EQ(controller.primaryDeclaredColorSpace(), 1);
    EXPECT_FALSE(controller.primaryColorConversionApplied());

    EXPECT_EQ(controller.secondaryColorProfileStatus(),
              static_cast<int>(ImageColorProfileStatus::None));
    EXPECT_FALSE(controller.secondaryColorConversionApplied());

    // The analysis buffer is the decoded RGBA8 conversion for A (16-bit source) and the
    // file's own codes for B (plain RGBA8). Native sidecar availability is separate.
    EXPECT_EQ(controller.primaryAnalysisValuePath(),
              static_cast<int>(ImagePixelValuePath::DecodedDisplayRgba8));
    EXPECT_EQ(controller.secondaryAnalysisValuePath(),
              static_cast<int>(ImagePixelValuePath::FileRgba8CodeValues));
    EXPECT_FALSE(controller.primaryNativeValuePathAvailable());
    EXPECT_FALSE(controller.secondaryNativeValuePathAvailable());
    EXPECT_FALSE(controller.displayColorEnvironmentVerified());

    // The state sentence is one source: it distinguishes "profile structurally supported but
    // no engine colour space" from "no profile", and it always admits the display environment
    // is unverified. COLOR4 no longer claims a supported profile is simply "not applied".
    const QString supported = controller.primaryColorStatusText();
    EXPECT_TRUE(supported.contains(QStringLiteral("转换引擎未就绪"))) << supported.toStdString();
    EXPECT_FALSE(supported.contains(QStringLiteral("已应用文件配置转换")))
        << supported.toStdString();
    EXPECT_TRUE(supported.contains(QStringLiteral("显示环境未验证"))) << supported.toStdString();
    EXPECT_FALSE(supported.contains(QStringLiteral("已验证转换"))) << supported.toStdString();
    EXPECT_TRUE(controller.secondaryColorStatusText().contains(QStringLiteral("无嵌入配置")))
        << controller.secondaryColorStatusText().toStdString();

    // Swapping sides moves the whole provenance record, including the colour state.
    ASSERT_TRUE(controller.swapSides());
    EXPECT_EQ(controller.primaryColorProfileStatus(),
              static_cast<int>(ImageColorProfileStatus::None));
    EXPECT_EQ(controller.secondaryColorProfileStatus(),
              static_cast<int>(ImageColorProfileStatus::EmbeddedSupported));

    controller.closeAll();
    EXPECT_EQ(controller.primaryColorProfileStatus(),
              static_cast<int>(ImageColorProfileStatus::NotProbed));
    EXPECT_TRUE(controller.primaryColorStatusText().contains(QStringLiteral("未探测")));
}

TEST(ImageColorContractTests, AnalysisDataAndStatisticsDoNotMoveWithPresentationState) {
    ImageReviewController controller;
    ASSERT_TRUE(controller.openPairImages(solidImage(QColor(120, 130, 140, 200)),
                                          QStringLiteral("a.png"),
                                          solidImage(QColor(122, 128, 140, 255)),
                                          QStringLiteral("b.png")));
    controller.setCompareMode(ImageReviewController::AbsDifference);
    ASSERT_TRUE(waitUntil([&controller] {
        return !controller.openPending() && !controller.diffPending() && controller.hasDiffResult();
    }));
    ASSERT_TRUE(controller.hasDiffResult());
    const int peak = controller.maxAbsDifference();
    const double mean = controller.meanAbsDifference();
    const int alphaPeak = controller.peakAlphaDifference();
    const qint64 alphaChanged = controller.alphaChangedPixels();
    const QImage rawPrimaryBefore =
        controller.rawImageForSlot(ImageReviewController::DisplayPrimarySlot);
    const QImage rawSecondaryBefore =
        controller.rawImageForSlot(ImageReviewController::DisplaySecondarySlot);

    // Presentation-only switches: channel observation, alpha filter, numeric format, gain.
    controller.setViewMode(ImageReviewController::AlphaGrayView);
    controller.setAlphaFilterEnabled(true);
    controller.setAlphaThreshold(200);
    controller.setNumericFormat(ImageReviewController::NormalizedFloat);
    controller.setDiffGain(9);
    controller.setWipeOrientation(1);
    controller.setFadePosition(0.25);
    controller.setViewMode(ImageReviewController::RgbOpaqueView);
    ASSERT_TRUE(waitUntil([&controller] { return !controller.diffPending(); }));

    EXPECT_EQ(controller.maxAbsDifference(), peak);
    EXPECT_DOUBLE_EQ(controller.meanAbsDifference(), mean);
    EXPECT_EQ(controller.peakAlphaDifference(), alphaPeak);
    EXPECT_EQ(controller.alphaChangedPixels(), alphaChanged);
    // The committed analysis buffers are byte-identical: a derived view never replaces them.
    const QImage rawPrimaryAfter =
        controller.rawImageForSlot(ImageReviewController::DisplayPrimarySlot);
    const QImage rawSecondaryAfter =
        controller.rawImageForSlot(ImageReviewController::DisplaySecondarySlot);
    ASSERT_EQ(rawPrimaryBefore.size(), rawPrimaryAfter.size());
    ASSERT_EQ(rawSecondaryBefore.size(), rawSecondaryAfter.size());
    EXPECT_EQ(rawPrimaryBefore.convertToFormat(QImage::Format_RGBA8888),
              rawPrimaryAfter.convertToFormat(QImage::Format_RGBA8888));
    EXPECT_EQ(rawSecondaryBefore.convertToFormat(QImage::Format_RGBA8888),
              rawSecondaryAfter.convertToFormat(QImage::Format_RGBA8888));
    EXPECT_EQ(controller.primaryAnalysisValuePath(),
              static_cast<int>(ImagePixelValuePath::FileRgba8CodeValues));
}

TEST(ImageColorContractTests, ContainerProbeFillsColourStateWhenTheDecoderDoesNot) {
    // The real PNG path is the Qt fallback (the FFmpeg build has no PNG decoder), so the
    // decoder leaves the colour state NotProbed. The injected container probe must fill it,
    // and a container that declares an unreadable profile must become Corrupt.
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("colour.png"));
    {
        QFile file{path};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("not-a-real-png-payload", 22), 22);
    }

    dvs::ui::ImagePairLoader loader;
    dvs::ui::ImagePairLoader::DecodePolicy policy;
    policy.loader =
        [](const QByteArray&, QImage* image, QImage*, StillImageSourceInfo* info, std::string*) {
            QImage decoded(2, 2, QImage::Format_ARGB32);
            decoded.fill(QColor(10, 20, 30));
            *image = decoded;
            info->sourceFormat = QStringLiteral("rgba8 (qt)");
            info->channels = 4;
            info->hasAlpha = false;
            return true;
        };
    int probeCalls = 0;
    policy.colorProbe = [&probeCalls](const QByteArray&,
                                      QByteArray* profileBytes,
                                      bool* profileDeclared,
                                      std::string*) {
        ++probeCalls;
        *profileBytes = iccHeader("RGB ", "mntr", 4);
        *profileDeclared = true;
        return true;
    };

    bool finished = false;
    dvs::ui::ImagePairLoader::Result result;
    ASSERT_GT(loader.requestPrimary(QUrl::fromLocalFile(path),
                                    -1,
                                    policy,
                                    [&finished, &result](dvs::ui::ImagePairLoader::Result value) {
                                        result = std::move(value);
                                        finished = true;
                                    }),
              0);
    ASSERT_TRUE(waitUntil([&finished] { return finished; }));
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(probeCalls, 1);
    EXPECT_EQ(result.primaryInfo.color.profileStatus, ImageColorProfileStatus::EmbeddedSupported);
    EXPECT_EQ(result.primaryInfo.color.profileDataSpace, QStringLiteral("RGB "));
    EXPECT_EQ(result.primaryInfo.color.profileSha256.size(), 64);

    // Declared but unreadable: the state must be Corrupt, never None.
    dvs::ui::ImagePairLoader fallbackLoader;
    dvs::ui::ImagePairLoader::DecodePolicy corruptPolicy;
    corruptPolicy.loader = policy.loader;
    corruptPolicy.colorProbe =
        [](const QByteArray&, QByteArray* profileBytes, bool* profileDeclared, std::string* error) {
            profileBytes->clear();
            *profileDeclared = true;
            if (error != nullptr) {
                *error = "declared but unreadable";
            }
            return false;
        };
    bool corruptFinished = false;
    dvs::ui::ImagePairLoader::Result corruptResult;
    ASSERT_GT(fallbackLoader.requestPrimary(
                  QUrl::fromLocalFile(path),
                  -1,
                  corruptPolicy,
                  [&corruptFinished, &corruptResult](dvs::ui::ImagePairLoader::Result value) {
                      corruptResult = std::move(value);
                      corruptFinished = true;
                  }),
              0);
    ASSERT_TRUE(waitUntil([&corruptFinished] { return corruptFinished; }));
    ASSERT_TRUE(corruptResult.succeeded()) << corruptResult.error.toStdString();
    EXPECT_EQ(corruptResult.primaryInfo.color.profileStatus, ImageColorProfileStatus::Corrupt);
}

} // namespace
