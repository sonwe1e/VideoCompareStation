#include "dvs/media/ImageColorProfileProbe.h"
#include "dvs/media/StillImageDecoder.h"
#include "dvs/ui/ImageColorContract.h"

#include <QCryptographicHash>
#include <QFile>
#include <QString>

#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// COLOR2 sample checker. It validates the committed manifest (counts, required categories,
// SHA-256, reference fields), extracts every embedded ICC payload through the same
// container probe the application uses, and checks the structural classification and hash
// against the independent LittleCMS-derived manifest. PNM fixtures are additionally decoded
// through the real media entry.
//
// PNG pixels are deliberately not decoded here: the project FFmpeg build disables zlib and
// therefore has no PNG decoder; the app opens PNG through the Qt fallback. Raw PNG pixel
// values are verified independently by tools/fixtures/verify-fixtures.py, and the app path is
// covered by the image-workspace tests.
//
// The checker counts its checks and fails below a floor derived from the required fixture and
// category set, so deleting a fixture, a category, a reference field or a loop cannot silently
// pass. Reference values come from LittleCMS, never from this decoder.
namespace {

using json = nlohmann::json;

using dvs::media::decodeStillImageBytes;
using dvs::media::extractEmbeddedIccProfile;
using dvs::ui::classifyImageColorProfile;
using dvs::ui::ImageColorProfileStatus;
using dvs::ui::ImageColorSourceLayout;

constexpr int kRequiredFixtureCount = 25;
constexpr int kRequiredCategoryCount = 13;
const std::string kColorFixturePrefix = "tests/fixtures/images/color/";

[[nodiscard]] std::string fixtureDirectory() {
    return std::string(DVS_IMAGE_FIXTURE_DIR) + "/color/";
}

[[nodiscard]] json loadManifest() {
    const std::string path = fixtureDirectory() + "manifest.json";
    std::ifstream file{path};
    EXPECT_TRUE(file.good()) << path;
    json manifest;
    file >> manifest;
    return manifest;
}

[[nodiscard]] QByteArray readBytes(const std::string& relativePath) {
    const QString path = QString::fromStdString(fixtureDirectory() +
                                                relativePath.substr(kColorFixturePrefix.size()));
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        ADD_FAILURE() << "could not open fixture " << path.toStdString();
        return {};
    }
    return file.readAll();
}

[[nodiscard]] std::string sha256Hex(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())
        .toLower()
        .toStdString();
}

[[nodiscard]] ImageColorProfileStatus expectedProfileStatus(const std::string& category) {
    if (category == "corrupt-profile") {
        return ImageColorProfileStatus::Corrupt;
    }
    if (category == "unsupported-profile") {
        return ImageColorProfileStatus::EmbeddedUnsupported;
    }
    if (category == "no-profile" || category == "low-bit-difference" || category == "pnm") {
        return ImageColorProfileStatus::None;
    }
    return ImageColorProfileStatus::EmbeddedSupported;
}

[[nodiscard]] const char* statusToken(const ImageColorProfileStatus status) {
    return dvs::ui::imageColorProfileStatusToken(status);
}

[[nodiscard]] bool isGrayColorType(const std::string& colorType) {
    return colorType.rfind("Gray", 0) == 0 || colorType == "PBM" || colorType == "PGM";
}

[[nodiscard]] bool hasExpectationField(const json& referenceValues, const std::string& category) {
    if (referenceValues.contains("expectedSrgb8") || referenceValues.contains("expectedFallback") ||
        referenceValues.contains("expectedDisplay8") ||
        referenceValues.contains("expectedHighByte8")) {
        return true;
    }
    // PNM entries record raw probe codes and a note instead of a colour expectation.
    return category == "pnm" && referenceValues.contains("note");
}

TEST(ImageColorSampleTests, ManifestPinsCountsCategoriesHashesAndReferences) {
    const json manifest = loadManifest();
    ASSERT_FALSE(manifest.is_null());
    EXPECT_EQ(manifest.value("fixtureFormatVersion", 0), 1);
    EXPECT_EQ(manifest.value("fixtureSet", std::string{}), "COLOR2");
    const int declaredCount = manifest.value("fixtureCount", 0);
    EXPECT_GE(declaredCount, kRequiredFixtureCount);
    const json& fixtures = manifest.at("fixtures");
    EXPECT_EQ(static_cast<int>(fixtures.size()), declaredCount);

    std::map<std::string, int> categoryCounts;
    int checkedFixtures = 0;
    int checkedReferences = 0;
    int checks = 0;
    for (const json& entry : fixtures) {
        const std::string path = entry.at("path").get<std::string>();
        const std::string category = entry.at("category").get<std::string>();
        SCOPED_TRACE(path);
        EXPECT_EQ(path.rfind(kColorFixturePrefix, 0), 0U);
        EXPECT_FALSE(entry.at("expectedPurpose").get<std::string>().empty());
        EXPECT_FALSE(entry.at("generationMethod").get<std::string>().empty());
        categoryCounts[category]++;

        const QByteArray bytes = readBytes(path);
        ASSERT_FALSE(bytes.isEmpty());
        EXPECT_EQ(static_cast<qint64>(entry.at("bytes").get<long long>()), bytes.size());
        EXPECT_EQ(entry.at("sha256").get<std::string>(), sha256Hex(bytes));

        const json& referenceValues = entry.at("referenceValues");
        EXPECT_FALSE(referenceValues.at("probePixels").empty());
        EXPECT_FALSE(referenceValues.at("tolerance").is_null());
        EXPECT_TRUE(hasExpectationField(referenceValues, category));

        const json& profile = entry.at("embeddedProfile");
        if (category == "no-profile" || category == "low-bit-difference" || category == "pnm") {
            EXPECT_TRUE(profile.is_null());
        } else {
            ASSERT_FALSE(profile.is_null());
            EXPECT_FALSE(profile.at("sha256").get<std::string>().empty());
        }
        checks += 7;
        checkedFixtures++;
        if (!referenceValues.at("probePixels").empty()) {
            checkedReferences++;
        }
    }

    // Required categories with the minimum count each must keep. Deleting a category or
    // dropping one of the paired/duplicate samples must fail here.
    const std::map<std::string, int> requiredCategories = {
        {"srgb", 1},
        {"display-p3", 1},
        {"adobe-rgb", 1},
        {"no-profile", 1},
        {"corrupt-profile", 2},
        {"unsupported-profile", 2},
        {"alpha-edges", 1},
        {"gray", 1},
        {"high-bit-depth", 1},
        {"low-bit-difference", 2},
        {"same-codepoints-different-profile", 2},
        {"different-codepoints-same-color", 2},
        {"pnm", 8},
    };
    EXPECT_EQ(static_cast<int>(requiredCategories.size()), kRequiredCategoryCount);
    for (const auto& [category, minimum] : requiredCategories) {
        EXPECT_GE(categoryCounts[category], minimum) << category;
    }
    EXPECT_EQ(checkedFixtures, declaredCount);
    EXPECT_EQ(checkedReferences, declaredCount);
    EXPECT_GE(checks, 7 * kRequiredFixtureCount);
}

TEST(ImageColorSampleTests, EmbeddedProfilesMatchTheManifestClassification) {
    const json manifest = loadManifest();
    ASSERT_FALSE(manifest.is_null());
    const json& fixtures = manifest.at("fixtures");
    int checks = 0;
    for (const json& entry : fixtures) {
        const std::string path = entry.at("path").get<std::string>();
        const std::string category = entry.at("category").get<std::string>();
        const std::string colorType = entry.at("colorType").get<std::string>();
        SCOPED_TRACE(path);

        const QByteArray bytes = readBytes(path);
        ASSERT_FALSE(bytes.isEmpty());
        std::vector<std::uint8_t> profileBytes;
        bool profileDeclared = false;
        std::string probeError;
        const bool extracted =
            extractEmbeddedIccProfile(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                                      static_cast<std::size_t>(bytes.size()),
                                      &profileBytes,
                                      &profileDeclared,
                                      &probeError);
        const json& profile = entry.at("embeddedProfile");
        const auto layout =
            isGrayColorType(colorType) ? ImageColorSourceLayout::Gray : ImageColorSourceLayout::Rgb;
        const dvs::ui::ImageColorInfo color =
            classifyImageColorProfile(QByteArray{reinterpret_cast<const char*>(profileBytes.data()),
                                                 static_cast<qsizetype>(profileBytes.size())},
                                      layout);
        EXPECT_EQ(std::string{statusToken(color.profileStatus)},
                  std::string{statusToken(expectedProfileStatus(category))})
            << probeError;
        checks++;

        if (!profile.is_null()) {
            EXPECT_TRUE(profileDeclared) << path;
            EXPECT_TRUE(extracted) << path << ": " << probeError;
            checks += 2;
            if (color.profileStatus != ImageColorProfileStatus::Corrupt) {
                EXPECT_EQ(color.profileSha256.toStdString(),
                          profile.at("sha256").get<std::string>());
                EXPECT_EQ(color.profileDataSpace.toStdString(),
                          profile.at("dataSpace").get<std::string>());
                EXPECT_EQ(color.profileClass.toStdString(), profile.at("class").get<std::string>());
                EXPECT_FALSE(color.conversionApplied);
                checks += 4;
            }
        } else {
            EXPECT_FALSE(profileDeclared) << path;
            EXPECT_FALSE(extracted) << path;
            EXPECT_TRUE(profileBytes.empty());
            checks += 3;
        }

        // PNM has no ICC profile and is decoded by the real media entry; verify the decoder
        // path at least reaches the recorded dimensions.
        if (category == "pnm") {
            dvs::media::StillImage image;
            std::string error;
            ASSERT_TRUE(
                decodeStillImageBytes(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                                      static_cast<std::size_t>(bytes.size()),
                                      &image,
                                      &error))
                << path << ": " << error;
            EXPECT_EQ(image.width, entry.at("width").get<int>());
            EXPECT_EQ(image.height, entry.at("height").get<int>());
            checks += 2;
        }
    }
    EXPECT_GE(checks, 6 * kRequiredFixtureCount);
}

} // namespace
