#include "dvs/media/ImageColorProfileProbe.h"
#include "dvs/media/StillImageDecoder.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

namespace {

using dvs::media::decodeStillImageBytes;
using dvs::media::extractEmbeddedIccProfile;
using dvs::media::probeStillImageBytes;

[[nodiscard]] std::vector<std::uint8_t> readFixture(const std::string& relativePath) {
    const std::string path = std::string(DVS_IMAGE_FIXTURE_DIR) + "/" + relativePath;
    std::ifstream file{path, std::ios::binary};
    EXPECT_TRUE(file.good()) << path;
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{file},
                                     std::istreambuf_iterator<char>{}};
}

[[nodiscard]] dvs::media::StillImage decodeFixture(const std::string& relativePath) {
    const std::vector<std::uint8_t> bytes = readFixture(relativePath);
    EXPECT_FALSE(bytes.empty()) << relativePath;
    dvs::media::StillImage image;
    std::string error;
    EXPECT_TRUE(decodeStillImageBytes(bytes.data(), bytes.size(), &image, &error))
        << relativePath << ": " << error;
    return image;
}

[[nodiscard]] std::vector<std::uint8_t> iccPayload(const std::size_t size) {
    std::vector<std::uint8_t> payload(size, 0U);
    if (size >= 128U) {
        payload[3] = static_cast<std::uint8_t>(size & 0xFFU);
        payload[2] = static_cast<std::uint8_t>((size >> 8U) & 0xFFU);
        payload[1] = static_cast<std::uint8_t>((size >> 16U) & 0xFFU);
        payload[0] = static_cast<std::uint8_t>((size >> 24U) & 0xFFU);
        std::memcpy(payload.data() + 12, "mntr", 4);
        std::memcpy(payload.data() + 16, "RGB ", 4);
        std::memcpy(payload.data() + 20, "XYZ ", 4);
        std::memcpy(payload.data() + 36, "acsp", 4);
        payload[8] = 4U;
    }
    return payload;
}

void appendBig16(std::vector<std::uint8_t>& bytes, const std::size_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void appendBig32(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void appendLittle32(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

// Builds a P6 binary PPM (8-bit RGB) payload.
[[nodiscard]] std::vector<std::uint8_t>
ppm6Payload(const int width, const int height, const std::vector<std::uint8_t>& samples) {
    const std::string header =
        "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    std::vector<std::uint8_t> payload(header.begin(), header.end());
    payload.insert(payload.end(), samples.begin(), samples.end());
    return payload;
}

TEST(StillImageDecoderTests, ProbesPnmDimensions) {
    const std::vector<std::uint8_t> ppm =
        ppm6Payload(3, 2, {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 128, 64, 32});
    int width = 0;
    int height = 0;
    std::string error;
    ASSERT_TRUE(probeStillImageBytes(ppm.data(), ppm.size(), &width, &height, &error)) << error;
    EXPECT_EQ(width, 3);
    EXPECT_EQ(height, 2);
}

TEST(StillImageDecoderTests, DecodesP6PpmAsDisplayRgbaWithRgb24Provenance) {
    // 2x2: white / red / green / blue.
    const std::vector<std::uint8_t> ppm =
        ppm6Payload(2, 2, {255, 255, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255});
    dvs::media::StillImage image;
    std::string error;
    ASSERT_TRUE(decodeStillImageBytes(ppm.data(), ppm.size(), &image, &error)) << error;
    EXPECT_EQ(image.width, 2);
    EXPECT_EQ(image.height, 2);
    EXPECT_EQ(image.rgba.size(), static_cast<std::size_t>(2 * 2 * 4));
    EXPECT_EQ(image.rgba[0], 255);
    EXPECT_EQ(image.rgba[1], 255);
    EXPECT_EQ(image.rgba[2], 255);
    EXPECT_EQ(image.rgba[3], 255);
    EXPECT_EQ(image.rgba[4], 255);
    EXPECT_EQ(image.rgba[5], 0);
    EXPECT_EQ(image.rgba[6], 0);
    // PNM is now a first-class input: the provenance says what the frame actually was.
    EXPECT_EQ(image.sourceBitDepth, 8);
    EXPECT_EQ(image.sourceChannels, 3);
    EXPECT_FALSE(image.hasAlpha);
    EXPECT_EQ(image.sourceFormat, "rgb24");
}

TEST(StillImageDecoderTests, Decodes16BitPgmAndRetainsSourceBitDepth) {
    // P5 PGM, 2x2, maxval 65535: samples 65535, 32768, 0, 12345 stored big-endian in the
    // file; FFmpeg's pnm decoder hands the frame to us as gray16le, which is what the
    // provenance records — the key contract is that 16-bit never masquerades as 8-bit.
    const std::string header = "P5\n2 2\n65535\n";
    std::vector<std::uint8_t> payload(header.begin(), header.end());
    const std::vector<std::uint8_t> samples = {0xFF, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x30, 0x39};
    payload.insert(payload.end(), samples.begin(), samples.end());

    dvs::media::StillImage image;
    std::string error;
    ASSERT_TRUE(decodeStillImageBytes(payload.data(), payload.size(), &image, &error)) << error;
    EXPECT_EQ(image.width, 2);
    EXPECT_EQ(image.height, 2);
    // Display conversion to RGBA8 is not silent: the 16-bit source is recorded.
    EXPECT_EQ(image.sourceBitDepth, 16);
    EXPECT_EQ(image.sourceChannels, 1);
    EXPECT_FALSE(image.hasAlpha);
    EXPECT_EQ(image.sourceFormat, "gray16le");
}

TEST(StillImageDecoderTests, DecodesPamRgbaWithAlphaProvenance) {
    // P7 PAM, 2x1, DEPTH 4 RGBA: opaque white then half-transparent black.
    const std::string header =
        "P7\nWIDTH 2\nHEIGHT 1\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    std::vector<std::uint8_t> payload(header.begin(), header.end());
    const std::vector<std::uint8_t> samples = {255, 255, 255, 255, 0, 0, 0, 128};
    payload.insert(payload.end(), samples.begin(), samples.end());

    dvs::media::StillImage image;
    std::string error;
    ASSERT_TRUE(decodeStillImageBytes(payload.data(), payload.size(), &image, &error)) << error;
    EXPECT_EQ(image.width, 2);
    EXPECT_EQ(image.height, 1);
    EXPECT_TRUE(image.hasAlpha);
    EXPECT_EQ(image.sourceChannels, 4);
    EXPECT_EQ(image.sourceBitDepth, 8);
    EXPECT_EQ(image.rgba[3], 255);
    EXPECT_EQ(image.rgba[7], 128);
}

TEST(StillImageDecoderTests, DecodesEveryRealPnmSubtype) {
    // I-03/MEDIA1: real P1-P7 files, not injected samples.
    const std::vector<std::string> files = {
        "color/pnm_p1_bitmap_8x8.pbm",
        "color/pnm_p2_gray_ascii_8x8.pgm",
        "color/pnm_p3_rgb_ascii_8x8.ppm",
        "color/pnm_p4_bitmap_binary_8x8.pbm",
        "color/pnm_p5_gray_8bit_8x8.pgm",
        "color/pnm_p5_gray_16bit_8x8.pgm",
        "color/pnm_p6_rgb_8bit_8x8.ppm",
        "color/pnm_p7_rgba_8x8.pam",
    };
    for (const std::string& file : files) {
        SCOPED_TRACE(file);
        const dvs::media::StillImage image = decodeFixture(file);
        EXPECT_EQ(image.width, 8) << file;
        EXPECT_EQ(image.height, 8) << file;
        EXPECT_EQ(image.rgba.size(), static_cast<std::size_t>(8 * 8 * 4)) << file;
    }
}

TEST(StillImageDecoderTests, DecodesRealFormatsWithEnabledFfmpegDecoders) {
    // MEDIA1: the FFmpeg build in this tree enables the JPEG/BMP/GIF/WebP/PNM decoders. PNG is
    // absent because the build disables zlib; the app opens PNG through the Qt fallback and
    // reads its colour state through extractEmbeddedIccProfile(). TIFF is covered separately:
    // the decoder exists, but this fixture is Deflate-compressed and zlib is not compiled in.
    for (const std::string& file : {"formats/jpeg_rgb8_16x16.jpg",
                                    "formats/bmp_rgb8_16x16.bmp",
                                    "formats/gif_static_16x16.gif",
                                    "formats/webp_lossless_16x16.webp",
                                    "formats/pnm_p6_rgb8_16x16.ppm"}) {
        SCOPED_TRACE(file);
        const dvs::media::StillImage image = decodeFixture(file);
        EXPECT_EQ(image.width, 16) << file;
        EXPECT_EQ(image.height, 16) << file;
    }
}

TEST(StillImageDecoderTests, DeflateTiffFailsExplicitlyInsteadOfClaimingSupport) {
    // The TIFF decoder is present, but this FFmpeg build has zlib disabled, so a
    // Deflate-compressed TIFF cannot be submitted. The failure must be explicit: the support
    // table records "uncompressed TIFF only" until a zlib-enabled build is verified.
    const std::vector<std::uint8_t> bytes = readFixture("formats/tiff_rgb8_16x16.tiff");
    ASSERT_FALSE(bytes.empty());
    dvs::media::StillImage image;
    std::string error;
    EXPECT_FALSE(decodeStillImageBytes(bytes.data(), bytes.size(), &image, &error));
    EXPECT_FALSE(error.empty());
}

TEST(StillImageDecoderTests, SniffsFakeExtensionByContentNotByName) {
    // A JPEG renamed to .png must still decode through the content sniff; the entry filters
    // and the decoder must not disagree about what the file is.
    const dvs::media::StillImage image =
        decodeFixture("formats/fake-extension_jpeg-bytes_16x16.png");
    EXPECT_EQ(image.width, 16);
    EXPECT_EQ(image.height, 16);
}

TEST(StillImageDecoderTests, ExtractsEmbeddedSrgbProfileFromRealPng) {
    const std::vector<std::uint8_t> bytes = readFixture("color/srgb_rgb8_16x16.png");
    ASSERT_FALSE(bytes.empty());
    std::vector<std::uint8_t> profile;
    bool declared = false;
    std::string error;
    ASSERT_TRUE(extractEmbeddedIccProfile(bytes.data(), bytes.size(), &profile, &declared, &error))
        << error;
    EXPECT_TRUE(declared);
    ASSERT_EQ(profile.size(), 25148U);
    EXPECT_EQ(std::memcmp(profile.data() + 36, "acsp", 4), 0);
    EXPECT_EQ(std::memcmp(profile.data() + 16, "RGB ", 4), 0);
}

TEST(StillImageDecoderTests, ExtractsCorruptPngProfileBytesForClassification) {
    for (const std::string& file : {"color/corrupt-profile_broken-acsp_16x16.png",
                                    "color/corrupt-profile_declared-length_16x16.png"}) {
        SCOPED_TRACE(file);
        const std::vector<std::uint8_t> bytes = readFixture(file);
        ASSERT_FALSE(bytes.empty());
        std::vector<std::uint8_t> profile;
        bool declared = false;
        std::string error;
        const bool extracted =
            extractEmbeddedIccProfile(bytes.data(), bytes.size(), &profile, &declared, &error);
        EXPECT_TRUE(declared);
        // Both fixtures carry a readable iCCP payload whose header is invalid; the payload must
        // reach the classifier, which then reports Corrupt.
        EXPECT_TRUE(extracted) << error;
        EXPECT_FALSE(profile.empty());
    }
    const std::vector<std::uint8_t> brokenAcsp =
        readFixture("color/corrupt-profile_broken-acsp_16x16.png");
    std::vector<std::uint8_t> profile;
    bool declared = false;
    std::string error;
    ASSERT_TRUE(extractEmbeddedIccProfile(
        brokenAcsp.data(), brokenAcsp.size(), &profile, &declared, &error));
    EXPECT_NE(std::memcmp(profile.data() + 36, "acsp", 4), 0);
}

TEST(StillImageDecoderTests, UntaggedPngReportsNoDeclaredProfile) {
    const std::vector<std::uint8_t> bytes = readFixture("color/no-profile_rgb8_16x16.png");
    ASSERT_FALSE(bytes.empty());
    std::vector<std::uint8_t> profile;
    bool declared = true;
    std::string error;
    EXPECT_FALSE(
        extractEmbeddedIccProfile(bytes.data(), bytes.size(), &profile, &declared, &error));
    EXPECT_FALSE(declared);
    EXPECT_TRUE(profile.empty());
}

TEST(StillImageDecoderTests, ExtractsJpegApp2ProfileSegmentsInOrder) {
    const std::vector<std::uint8_t> first = iccPayload(300U);
    const std::vector<std::uint8_t> second = iccPayload(200U);
    std::vector<std::uint8_t> jpeg{0xFFU, 0xD8U};
    for (const auto& [sequence, chunk] :
         {std::pair{1, std::cref(first)}, std::pair{2, std::cref(second)}}) {
        const std::size_t segmentLength = 2U + 12U + 2U + chunk.get().size();
        jpeg.push_back(0xFFU);
        jpeg.push_back(0xE2U);
        appendBig16(jpeg, segmentLength);
        const char header[] = "ICC_PROFILE";
        jpeg.insert(jpeg.end(), header, header + 11);
        jpeg.push_back(0U);
        jpeg.push_back(static_cast<std::uint8_t>(sequence));
        jpeg.push_back(2U);
        jpeg.insert(jpeg.end(), chunk.get().begin(), chunk.get().end());
    }
    jpeg.push_back(0xFFU);
    jpeg.push_back(0xD9U);

    std::vector<std::uint8_t> profile;
    bool declared = false;
    std::string error;
    ASSERT_TRUE(extractEmbeddedIccProfile(jpeg.data(), jpeg.size(), &profile, &declared, &error))
        << error;
    EXPECT_TRUE(declared);
    ASSERT_EQ(profile.size(), 500U);
    EXPECT_EQ(profile[0], first[0]);
    EXPECT_EQ(profile[300], second[0]);
}

TEST(StillImageDecoderTests, ExtractsWebpIccpChunk) {
    const std::vector<std::uint8_t> payload = iccPayload(256U);
    std::vector<std::uint8_t> webp{'R', 'I', 'F', 'F'};
    appendLittle32(webp, static_cast<std::uint32_t>(4U + 8U + payload.size()));
    const char webpTag[] = "WEBP";
    webp.insert(webp.end(), webpTag, webpTag + 4);
    const char iccpTag[] = "ICCP";
    webp.insert(webp.end(), iccpTag, iccpTag + 4);
    appendLittle32(webp, static_cast<std::uint32_t>(payload.size()));
    webp.insert(webp.end(), payload.begin(), payload.end());

    std::vector<std::uint8_t> profile;
    bool declared = false;
    std::string error;
    ASSERT_TRUE(extractEmbeddedIccProfile(webp.data(), webp.size(), &profile, &declared, &error))
        << error;
    EXPECT_TRUE(declared);
    EXPECT_EQ(profile, payload);
}

TEST(StillImageDecoderTests, RejectsNonPnmGarbage) {
    const std::vector<std::uint8_t> garbage = {'N', 'O', 'T', ' ', 'A', ' ', 'P', 'N', 'M'};
    dvs::media::StillImage image;
    std::string error;
    EXPECT_FALSE(decodeStillImageBytes(garbage.data(), garbage.size(), &image, &error));
    EXPECT_FALSE(error.empty());
}

} // namespace
