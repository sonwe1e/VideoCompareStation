#include "dvs/media/ImageColorProfileProbe.h"
#include "dvs/ui/ImageColorContract.h"

#include <QColorSpace>
#include <QGuiApplication>
#include <QImage>
#include <QString>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// COLOR4 engine validation. Qt's QColorSpace/QColorTransform is the candidate application
// engine; the committed COLOR2 manifest carries values produced independently by LittleCMS
// (Pillow ImageCms). This test proves the engine accepts the first-batch SDR profiles and that
// its SDR conversion matches the independent reference inside the manifest's tolerance. It does
// not touch the analysis buffers or statistics: the application will apply the same transform
// only to the display derivative.
namespace {

using json = nlohmann::json;

using dvs::media::extractEmbeddedIccProfile;
using dvs::ui::classifyImageColorProfile;
using dvs::ui::ImageColorProfileStatus;
using dvs::ui::ImageColorSourceLayout;

void ensureGuiApplication() {
    if (QGuiApplication::instance() != nullptr) {
        return;
    }
    static int argumentCount = 1;
    static char applicationName[] = "ImageColorConversionTests";
    static char* arguments[] = {applicationName, nullptr};
    static QGuiApplication application{argumentCount, arguments};
    static_cast<void>(application);
}

[[nodiscard]] std::string fixtureDirectory() {
    return std::string(DVS_IMAGE_FIXTURE_DIR) + "/color/";
}

[[nodiscard]] json loadManifest() {
    std::ifstream file{fixtureDirectory() + "manifest.json"};
    EXPECT_TRUE(file.good());
    json manifest;
    file >> manifest;
    return manifest;
}

[[nodiscard]] QByteArray readBytes(const std::string& repositoryPath) {
    const std::string prefix = "tests/fixtures/images/color/";
    EXPECT_EQ(repositoryPath.rfind(prefix, 0), 0U);
    const QString path =
        QString::fromStdString(fixtureDirectory() + repositoryPath.substr(prefix.size()));
    std::ifstream file{path.toStdString(), std::ios::binary};
    EXPECT_TRUE(file.good()) << path.toStdString();
    const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    return QByteArray::fromStdString(text);
}

// Loads the embedded ICC bytes and returns the Qt colour space the application would use.
// Returns an invalid QColorSpace when the file carries no profile or the profile is outside the
// supported first batch.
[[nodiscard]] QColorSpace sourceColorSpaceFor(const QByteArray& encoded,
                                              const std::string& colorType) {
    std::vector<std::uint8_t> profileBytes;
    bool declared = false;
    std::string error;
    if (!extractEmbeddedIccProfile(reinterpret_cast<const std::uint8_t*>(encoded.constData()),
                                   static_cast<std::size_t>(encoded.size()),
                                   &profileBytes,
                                   &declared,
                                   &error)) {
        return {};
    }
    const auto layout = colorType.rfind("Gray", 0) == 0 ? ImageColorSourceLayout::Gray
                                                        : ImageColorSourceLayout::Rgb;
    const auto info =
        classifyImageColorProfile(QByteArray(reinterpret_cast<const char*>(profileBytes.data()),
                                             static_cast<qsizetype>(profileBytes.size())),
                                  layout);
    if (info.profileStatus != ImageColorProfileStatus::EmbeddedSupported) {
        return {};
    }
    return QColorSpace::fromIccProfile(
        QByteArray(reinterpret_cast<const char*>(profileBytes.data()),
                   static_cast<qsizetype>(profileBytes.size())));
}

TEST(ImageColorConversionTests, QtAcceptsEverySupportedFirstBatchProfileAndRejectsTheRest) {
    ensureGuiApplication();
    const json manifest = loadManifest();
    ASSERT_FALSE(manifest.is_null());
    int accepted = 0;
    int rejected = 0;
    for (const json& entry : manifest.at("fixtures")) {
        const std::string path = entry.at("path").get<std::string>();
        const std::string category = entry.at("category").get<std::string>();
        const std::string colorType = entry.at("colorType").get<std::string>();
        if (category == "pnm" || entry.at("embeddedProfile").is_null()) {
            continue;
        }
        SCOPED_TRACE(path);
        const QByteArray bytes = readBytes(path);
        ASSERT_FALSE(bytes.isEmpty());
        const QColorSpace space = sourceColorSpaceFor(bytes, colorType);
        if (category == "unsupported-profile" || category == "corrupt-profile") {
            EXPECT_FALSE(space.isValid()) << category;
            rejected++;
        } else {
            EXPECT_TRUE(space.isValid()) << category;
            accepted++;
        }
    }
    // sRGB, Display P3, Adobe RGB, gray and the comparison pairs carry usable profiles; both
    // corrupt and both unsupported samples must not reach a transform.
    EXPECT_GE(accepted, 10);
    EXPECT_EQ(rejected, 4);
}

TEST(ImageColorConversionTests, QtSdrConversionMatchesTheIndependentLittleCmsReferences) {
    ensureGuiApplication();
    const json manifest = loadManifest();
    ASSERT_FALSE(manifest.is_null());
    int comparedFixtures = 0;
    int comparedPixels = 0;
    for (const json& entry : manifest.at("fixtures")) {
        const std::string path = entry.at("path").get<std::string>();
        const std::string category = entry.at("category").get<std::string>();
        const std::string colorType = entry.at("colorType").get<std::string>();
        const json& reference = entry.at("referenceValues");
        if (!reference.contains("expectedSrgb8") || !reference.contains("probePixels")) {
            continue;
        }
        SCOPED_TRACE(path);
        const QByteArray bytes = readBytes(path);
        ASSERT_FALSE(bytes.isEmpty());
        QImage image;
        ASSERT_TRUE(image.loadFromData(bytes, "PNG")) << path;
        const QColorSpace space = sourceColorSpaceFor(bytes, colorType);
        if (category == "corrupt-profile" || category == "unsupported-profile") {
            EXPECT_FALSE(space.isValid());
            continue;
        }
        ASSERT_TRUE(space.isValid()) << path;
        const QImage converted = [&image, &space]() {
            QImage tagged = image;
            tagged.setColorSpace(space);
            return tagged.convertedToColorSpace(QColorSpace::SRgb);
        }();
        ASSERT_FALSE(converted.isNull()) << path;
        const int tolerance = reference.at("tolerance").value("value", 1);
        const json& expectedValues = reference.at("expectedSrgb8");
        ASSERT_EQ(expectedValues.size(), reference.at("probePixels").size());
        for (std::size_t index = 0; index < expectedValues.size(); ++index) {
            const json& expected = expectedValues[index];
            const int x = expected.at("x").get<int>();
            const int y = expected.at("y").get<int>();
            const std::vector<int> channels = expected.at("srgb8").get<std::vector<int>>();
            ASSERT_EQ(channels.size(), 3U);
            const QColor actual = converted.pixelColor(x, y);
            SCOPED_TRACE("probe " + std::to_string(x) + "," + std::to_string(y));
            EXPECT_NEAR(actual.red(), channels[0], tolerance);
            EXPECT_NEAR(actual.green(), channels[1], tolerance);
            EXPECT_NEAR(actual.blue(), channels[2], tolerance);
            comparedPixels++;
        }
        comparedFixtures++;
    }
    // Nine first-batch fixtures carry LittleCMS references: sRGB, Display P3, Adobe RGB,
    // alpha edges, gray, both same-codepoint peers and both different-codepoint peers. The
    // 16-bit sample has no 8-bit reference and stays a COLOR4-precision item. A run that
    // silently skips the references must fail instead of reporting a green empty loop.
    EXPECT_EQ(comparedFixtures, 9);
    EXPECT_GE(comparedPixels, 30);
}

TEST(ImageColorConversionTests, SameCodepointsDifferentProfileConvertDifferently) {
    ensureGuiApplication();
    const json manifest = loadManifest();
    ASSERT_FALSE(manifest.is_null());
    const json* srgbEntry = nullptr;
    const json* p3Entry = nullptr;
    for (const json& entry : manifest.at("fixtures")) {
        const std::string path = entry.at("path").get<std::string>();
        if (path.find("same-codepoints_srgb") != std::string::npos) {
            srgbEntry = &entry;
        } else if (path.find("same-codepoints_p3") != std::string::npos) {
            p3Entry = &entry;
        }
    }
    ASSERT_NE(srgbEntry, nullptr);
    ASSERT_NE(p3Entry, nullptr);
    const QByteArray srgbBytes = readBytes(srgbEntry->at("path").get<std::string>());
    const QByteArray p3Bytes = readBytes(p3Entry->at("path").get<std::string>());
    QImage srgbImage;
    QImage p3Image;
    ASSERT_TRUE(srgbImage.loadFromData(srgbBytes, "PNG"));
    ASSERT_TRUE(p3Image.loadFromData(p3Bytes, "PNG"));
    const QColorSpace srgbSpace = sourceColorSpaceFor(srgbBytes, "RGB8");
    const QColorSpace p3Space = sourceColorSpaceFor(p3Bytes, "RGB8");
    ASSERT_TRUE(srgbSpace.isValid());
    ASSERT_TRUE(p3Space.isValid());
    const QImage srgbConverted = [&srgbImage, &srgbSpace]() {
        QImage tagged = srgbImage;
        tagged.setColorSpace(srgbSpace);
        return tagged.convertedToColorSpace(QColorSpace::SRgb);
    }();
    const QImage p3Converted = [&p3Image, &p3Space]() {
        QImage tagged = p3Image;
        tagged.setColorSpace(p3Space);
        return tagged.convertedToColorSpace(QColorSpace::SRgb);
    }();
    // Same file codepoints, different embedded profile: an ignored-profile pipeline would make
    // these identical. At least one probe must differ by more than the reference tolerance.
    bool anyDifference = false;
    for (const json& probe : srgbEntry->at("referenceValues").at("probePixels")) {
        const int x = probe.at("x").get<int>();
        const int y = probe.at("y").get<int>();
        const QColor left = srgbConverted.pixelColor(x, y);
        const QColor right = p3Converted.pixelColor(x, y);
        if (std::abs(left.red() - right.red()) + std::abs(left.green() - right.green()) +
                std::abs(left.blue() - right.blue()) >
            2) {
            anyDifference = true;
        }
    }
    EXPECT_TRUE(anyDifference);
}

} // namespace
