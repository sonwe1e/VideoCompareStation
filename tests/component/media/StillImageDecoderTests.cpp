#include "dvs/media/StillImageDecoder.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using dvs::media::decodeStillImageBytes;
using dvs::media::probeStillImageBytes;

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

TEST(StillImageDecoderTests, RejectsNonPnmGarbage) {
    const std::vector<std::uint8_t> garbage = {'N', 'O', 'T', ' ', 'A', ' ', 'P', 'N', 'M'};
    dvs::media::StillImage image;
    std::string error;
    EXPECT_FALSE(decodeStillImageBytes(garbage.data(), garbage.size(), &image, &error));
    EXPECT_FALSE(error.empty());
}

} // namespace
