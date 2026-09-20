#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "dvs/media/MediaProbe.h"
#include "dvs/platform/CpuNv12FrameResource.h"
#include "dvs/platform/CpuP010FrameResource.h"
#include "dvs/platform/D3d11DecodedFrameResource.h"
#include "dvs/platform/FrameBudget.h"
#include "dvs/platform/GraphicsDeviceBroker.h"

#include "SoftwareDecoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <wrl/client.h>

namespace dvs::media::internal {
namespace {

[[nodiscard]] std::filesystem::path fixture(const char* const name) {
    return std::filesystem::path{DVS_MEDIA_FIXTURE_DIR} / name;
}

[[nodiscard]] domain::MediaDescriptor probeDescriptor(const std::filesystem::path& path,
                                                      domain::SourceId sourceId) {
    const auto descriptor = MediaProbe::inspect(path, sourceId);
    EXPECT_TRUE(descriptor);
    return descriptor.value();
}

[[nodiscard]] std::uint64_t frameHash(const DecodedFrame& frame) {
    const auto resource =
        std::dynamic_pointer_cast<const platform::CpuNv12FrameResource>(frame.handle.resource());
    EXPECT_NE(resource, nullptr);
    if (resource == nullptr) {
        return 0;
    }

    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffsetBasis;
    const auto append = [&hash](const std::span<const std::uint8_t> bytes) {
        for (const std::uint8_t byte : bytes) {
            hash ^= byte;
            hash *= kPrime;
        }
    };
    append(resource->yPlane());
    append(resource->uvPlane());
    return hash;
}

// Self-contained SHA-256. libavutil's av_hash is not reachable from this test (ffmpeg is linked
// PRIVATE into dvs_media_ffmpeg and is non-transitive), and the project's Sha256 class is defined
// inside an anonymous namespace in the platform adapter. So we hash here directly, modelling the
// on- disk byte layout that `ffmpeg -f framehash -hash sha256` produces: planar yuv420p ordered Y
// (width bytes per row), then U (width/2 bytes per row), then V (width/2 bytes per row).
class Sha256 final {
public:
    void update(const std::span<const std::uint8_t> bytes) noexcept {
        for (const std::uint8_t byte : bytes) {
            buffer_[bufferSize_] = byte;
            ++bufferSize_;
            if (bufferSize_ == buffer_.size()) {
                transform(buffer_);
                bitCount_ += 512U;
                bufferSize_ = 0U;
            }
        }
    }

    [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept {
        bitCount_ += static_cast<std::uint64_t>(bufferSize_) * 8U;
        buffer_[bufferSize_] = 0x80U;
        ++bufferSize_;

        if (bufferSize_ > 56U) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(bufferSize_),
                      buffer_.end(),
                      std::uint8_t{0});
            transform(buffer_);
            bufferSize_ = 0U;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(bufferSize_),
                  buffer_.begin() + 56,
                  std::uint8_t{0});
        for (std::size_t index = 0; index < 8U; ++index) {
            const std::size_t shift = (7U - index) * 8U;
            buffer_[56U + index] = static_cast<std::uint8_t>(bitCount_ >> shift);
        }
        transform(buffer_);

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t index = 0; index < state_.size(); ++index) {
            for (std::size_t byte = 0; byte < 4U; ++byte) {
                const std::size_t shift = (3U - byte) * 8U;
                digest[index * 4U + byte] = static_cast<std::uint8_t>(state_[index] >> shift);
            }
        }
        return digest;
    }

private:
    static constexpr std::uint32_t rotateRight(const std::uint32_t value,
                                               const std::uint32_t shift) noexcept {
        return (value >> shift) | (value << (32U - shift));
    }

    void transform(const std::array<std::uint8_t, 64>& block) noexcept {
        constexpr std::array<std::uint32_t, 64> kRoundConstants{
            0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U,
            0x923F82A4U, 0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
            0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U,
            0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
            0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U,
            0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
            0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
            0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
            0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU,
            0x5B9CCA4FU, 0x682E6FF3U, 0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
            0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
        };

        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const std::size_t offset = index * 4U;
            words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                           (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                           (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                           static_cast<std::uint32_t>(block[offset + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const std::uint32_t lowerSigma0 = rotateRight(words[index - 15U], 7U) ^
                                              rotateRight(words[index - 15U], 18U) ^
                                              (words[index - 15U] >> 3U);
            const std::uint32_t lowerSigma1 = rotateRight(words[index - 2U], 17U) ^
                                              rotateRight(words[index - 2U], 19U) ^
                                              (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + lowerSigma0 + words[index - 7U] + lowerSigma1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t upperSigma1 =
                rotateRight(e, 6U) ^ rotateRight(e, 11U) ^ rotateRight(e, 25U);
            const std::uint32_t choose = (e & f) ^ ((~e) & g);
            const std::uint32_t temporary1 =
                h + upperSigma1 + choose + kRoundConstants[index] + words[index];
            const std::uint32_t upperSigma0 =
                rotateRight(a, 2U) ^ rotateRight(a, 13U) ^ rotateRight(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = upperSigma0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint8_t, 64> buffer_{};
    std::size_t bufferSize_ = 0U;
    std::uint64_t bitCount_ = 0U;
    std::array<std::uint32_t, 8> state_{
        0x6A09E667U,
        0xBB67AE85U,
        0x3C6EF372U,
        0xA54FF53AU,
        0x510E527FU,
        0x9B05688CU,
        0x1F83D9ABU,
        0x5BE0CD19U,
    };
};

[[nodiscard]] std::string lowercaseHexSha256(const std::array<std::uint8_t, 32>& digest) {
    constexpr std::array<char, 16> kHex{
        '0',
        '1',
        '2',
        '3',
        '4',
        '5',
        '6',
        '7',
        '8',
        '9',
        'a',
        'b',
        'c',
        'd',
        'e',
        'f',
    };
    std::string result;
    result.reserve(digest.size() * 2U);
    for (const std::uint8_t byte : digest) {
        result.push_back(kHex[byte >> 4U]);
        result.push_back(kHex[byte & 0x0FU]);
    }
    return result;
}

// The decoder hands back NV12 (Y plane followed by interleaved UV). ffmpeg's framehash muxer hashed
// sws conversion to NV12 is lossless at 1:1 scale, so this recovers the exact bytes ffmpeg hashed.
[[nodiscard]] std::string planarYuv420Sha256(const DecodedFrame& frame) {
    const auto resource =
        std::dynamic_pointer_cast<const platform::CpuNv12FrameResource>(frame.handle.resource());
    if (resource == nullptr) {
        return {};
    }
    const platform::Nv12FrameLayout layout = resource->layout();
    const std::uint32_t width = layout.width;
    const std::uint32_t height = layout.height;
    const std::uint32_t yStride = layout.yStride;
    const std::uint32_t uvStride = layout.uvStride;
    const std::span<const std::uint8_t> yPlane = resource->yPlane();
    const std::span<const std::uint8_t> uvPlane = resource->uvPlane();

    const std::uint32_t chromaWidth = (width + 1U) / 2U;
    const std::uint32_t chromaHeight = (height + 1U) / 2U;

    std::vector<std::uint8_t> planar;
    planar.reserve(static_cast<std::size_t>(width) * height + 2ULL * chromaWidth * chromaHeight);
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint8_t* const begin = yPlane.data() + static_cast<std::size_t>(row) * yStride;
        planar.insert(planar.end(), begin, begin + width);
    }
    std::vector<std::uint8_t> uPlane;
    std::vector<std::uint8_t> vPlane;
    uPlane.reserve(static_cast<std::size_t>(chromaWidth) * chromaHeight);
    vPlane.reserve(static_cast<std::size_t>(chromaWidth) * chromaHeight);
    for (std::uint32_t row = 0; row < chromaHeight; ++row) {
        const std::uint8_t* const begin = uvPlane.data() + static_cast<std::size_t>(row) * uvStride;
        for (std::uint32_t col = 0; col < chromaWidth; ++col) {
            uPlane.push_back(begin[2U * col]);
            vPlane.push_back(begin[2U * col + 1U]);
        }
    }
    planar.insert(planar.end(), uPlane.begin(), uPlane.end());
    planar.insert(planar.end(), vPlane.begin(), vPlane.end());

    Sha256 hasher;
    hasher.update(planar);
    return lowercaseHexSha256(hasher.finish());
}

TEST(SoftwareDecoderTests, DecodesExactFirstAndFinalFramesIntoBudgetedNv12Handles) {
    platform::FrameBudget budget{1024U * 1024U};
    SoftwareDecoder decoder{
        0U, probeDescriptor(fixture("h264_a_320x180_30fps_12.mp4"), 0U), budget};
    std::atomic<bool> canceled = false;

    ASSERT_TRUE(decoder.open(canceled));
    {
        const auto first = decoder.decodeExact(domain::FrameId{0}, canceled);
        ASSERT_TRUE(first);
        EXPECT_TRUE(first.value().handle.isValid());
        EXPECT_EQ(first.value().handle.geometry().width, 320U);
        EXPECT_EQ(first.value().handle.geometry().height, 180U);
        EXPECT_EQ(first.value().presentationTime, domain::MediaTime{0});

        const auto final = decoder.decodeExact(domain::FrameId{11}, canceled);
        ASSERT_TRUE(final);
        EXPECT_TRUE(final.value().handle.isValid());
        EXPECT_GT(final.value().presentationTime.microseconds(), 0);
        EXPECT_GT(budget.reservedBytes(), 0U);
    }
    EXPECT_EQ(budget.reservedBytes(), 0U);
}

TEST(SoftwareDecoderTests, DecodesExactHevcFramesByIndexedDisplayTimestamp) {
    platform::FrameBudget budget{2U * 1024U * 1024U};
    SoftwareDecoder decoder{
        0U, probeDescriptor(fixture("h265_a_320x180_30fps_12.mp4"), 0U), budget};
    std::atomic<bool> canceled = false;

    ASSERT_TRUE(decoder.open(canceled));
    const auto first = decoder.decodeExact(domain::FrameId{0}, canceled);
    ASSERT_TRUE(first) << first.error().technicalDetail;
    const auto middle = decoder.decodeExact(domain::FrameId{6}, canceled);
    ASSERT_TRUE(middle) << middle.error().technicalDetail;
    const auto last = decoder.decodeExact(domain::FrameId{11}, canceled);
    ASSERT_TRUE(last) << last.error().technicalDetail;
    const auto largeStep = decoder.decodeExact(domain::FrameId{10}, canceled);
    ASSERT_TRUE(largeStep) << largeStep.error().technicalDetail;
    const auto firstAgain = decoder.decodeExact(domain::FrameId{0}, canceled);
    ASSERT_TRUE(firstAgain) << firstAgain.error().technicalDetail;
    EXPECT_NE(frameHash(first.value()), frameHash(middle.value()));
    EXPECT_NE(frameHash(middle.value()), frameHash(last.value()));
    EXPECT_NE(frameHash(largeStep.value()), frameHash(firstAgain.value()));
    EXPECT_EQ(frameHash(first.value()), frameHash(firstAgain.value()));
}

TEST(SoftwareDecoderTests, DecodesTenBitHevcIntoBudgetedP010WithoutReducingBitDepth) {
    platform::FrameBudget budget{2U * 1024U * 1024U};
    const domain::MediaDescriptor descriptor =
        probeDescriptor(fixture("h265_10bit_320x180_30fps_12.mp4"), 0U);
    ASSERT_EQ(descriptor.bitDepth, 10U);
    SoftwareDecoder decoder{0U, descriptor, budget};
    std::atomic<bool> canceled = false;

    ASSERT_TRUE(decoder.open(canceled));
    const auto decoded = decoder.decodeExact(domain::FrameId{0}, canceled);
    ASSERT_TRUE(decoded) << decoded.error().technicalDetail;
    const auto resource = std::dynamic_pointer_cast<const platform::CpuP010FrameResource>(
        decoded.value().handle.resource());
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(resource->layout().width, 320U);
    EXPECT_EQ(resource->layout().height, 180U);
    EXPECT_EQ(resource->layout().yStride, 640U);
    ASSERT_GE(resource->yPlane().size(), 2U);
    const std::uint16_t firstSample = static_cast<std::uint16_t>(resource->yPlane()[0U]) |
                                      (static_cast<std::uint16_t>(resource->yPlane()[1U]) << 8U);
    EXPECT_EQ(firstSample & 0x003FU, 0U);
    EXPECT_NE(firstSample, 0U);
}

TEST(SoftwareDecoderTests, NormalizesYuv422Yuv444AndRgbInputsIntoNv12VisualFrames) {
    constexpr std::array<const char*, 3U> kFixtures{
        "h264_yuv422p_64x48_30fps_12.mkv",
        "h264_yuv444p_64x48_30fps_12.mkv",
        "h264_gbrp_64x48_30fps_12.mkv",
    };
    for (std::size_t index = 0U; index < kFixtures.size(); ++index) {
        platform::FrameBudget budget{1024U * 1024U};
        SoftwareDecoder decoder{
            static_cast<domain::SourceId>(index),
            probeDescriptor(fixture(kFixtures[index]), static_cast<domain::SourceId>(index)),
            budget};
        std::atomic<bool> canceled = false;
        ASSERT_TRUE(decoder.open(canceled)) << kFixtures[index];
        const auto decoded = decoder.decodeExact(domain::FrameId{0}, canceled);
        ASSERT_TRUE(decoded) << kFixtures[index] << ": " << decoded.error().technicalDetail;
        const auto resource = std::dynamic_pointer_cast<const platform::CpuNv12FrameResource>(
            decoded.value().handle.resource());
        ASSERT_NE(resource, nullptr) << kFixtures[index];
        EXPECT_EQ(resource->layout().width, 64U);
        EXPECT_EQ(resource->layout().height, 48U);
        EXPECT_NE(frameHash(decoded.value()), 0U);
    }
}

TEST(SoftwareDecoderTests, CarriesRotationAndSampleAspectRatioIntoFrameGeometry) {
    const auto verify = [](const char* const filename,
                           const application::FramePresentation expectedPresentation) {
        platform::FrameBudget budget{1024U * 1024U};
        SoftwareDecoder decoder{0U, probeDescriptor(fixture(filename), 0U), budget};
        std::atomic<bool> canceled = false;
        ASSERT_TRUE(decoder.open(canceled));
        const auto decoded = decoder.decodeExact(domain::FrameId{0}, canceled);
        ASSERT_TRUE(decoded) << decoded.error().technicalDetail;
        EXPECT_EQ(decoded.value().handle.geometry().presentation, expectedPresentation);
    };

    verify("h264_rot90_320x180_30fps_12.mp4",
           application::FramePresentation{.rotationDegrees = 90U});
    verify("h264_sar4x3_64x48_30fps_12.mkv",
           application::FramePresentation{
               .sampleAspectNumerator = 4U,
               .sampleAspectDenominator = 3U,
           });
}

TEST(SoftwareDecoderTests, RejectsOutOfRangeFramesAndReleasesAFailedBudgetReservation) {
    std::atomic<bool> canceled = false;
    const auto descriptor = probeDescriptor(fixture("h264_a_320x180_30fps_12.mp4"), 0U);

    platform::FrameBudget adequateBudget{1024U * 1024U};
    SoftwareDecoder adequateDecoder{0U, descriptor, adequateBudget};
    ASSERT_TRUE(adequateDecoder.open(canceled));
    const auto outOfRange = adequateDecoder.decodeExact(domain::FrameId{12}, canceled);
    ASSERT_FALSE(outOfRange);
    EXPECT_EQ(outOfRange.error().code, domain::MediaErrorCode::kInvalidFrameId);

    platform::FrameBudget constrainedBudget{64U * 1024U};
    SoftwareDecoder constrainedDecoder{0U, descriptor, constrainedBudget};
    ASSERT_TRUE(constrainedDecoder.open(canceled));
    const auto constrained = constrainedDecoder.decodeExact(domain::FrameId{0}, canceled);
    ASSERT_FALSE(constrained);
    EXPECT_EQ(constrained.error().code, domain::MediaErrorCode::kFrameBudgetExceeded);
    EXPECT_EQ(constrainedBudget.reservedBytes(), 0U);
}

TEST(SoftwareDecoderTests, PreservesTheNormalizedDescriptorColorMetadataWithoutReinferringIt) {
    auto descriptor = probeDescriptor(fixture("h264_a_320x180_30fps_12.mp4"), 0U);
    descriptor.colorMetadata = domain::ColorMetadata{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };

    platform::FrameBudget budget{1024U * 1024U};
    SoftwareDecoder decoder{0U, descriptor, budget};
    std::atomic<bool> canceled = false;
    ASSERT_TRUE(decoder.open(canceled));

    const auto decoded = decoder.decodeExact(domain::FrameId{0}, canceled);
    ASSERT_TRUE(decoded);
    const auto resource = std::dynamic_pointer_cast<const platform::CpuNv12FrameResource>(
        decoded.value().handle.resource());
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(resource->colorMetadata().matrix, descriptor.colorMetadata.matrix);
    EXPECT_EQ(resource->colorMetadata().range, descriptor.colorMetadata.range);
    EXPECT_EQ(resource->colorMetadata().matrixInferred, descriptor.colorMetadata.matrixInferred);
}

TEST(SoftwareDecoderTests, DecodesMpeg4Part2ByDisplayOrderOrdinal) {
    platform::FrameBudget budget{1024U * 1024U};
    SoftwareDecoder decoder{0U, probeDescriptor(fixture("mpeg4_64x48_30fps_12.mp4"), 0U), budget};
    std::atomic<bool> canceled = false;

    ASSERT_TRUE(decoder.open(canceled));
    const auto first = decoder.decodeExact(domain::FrameId{0}, canceled);
    const auto middle = decoder.decodeExact(domain::FrameId{6}, canceled);
    const auto last = decoder.decodeExact(domain::FrameId{11}, canceled);
    ASSERT_TRUE(first);
    ASSERT_TRUE(middle);
    ASSERT_TRUE(last);
    EXPECT_NE(frameHash(first.value()), frameHash(middle.value()));
    EXPECT_NE(frameHash(middle.value()), frameHash(last.value()));
}

TEST(SoftwareDecoderTests, OpensVerifiedCfrAndDecodesFirstMiddleAndLastFrames) {
    platform::FrameBudget budget{2U * 1024U * 1024U};
    const domain::MediaDescriptor descriptor =
        probeDescriptor(fixture("h264_disputed_metadata_320x180_30fps_12.mp4"), 0U);
    ASSERT_EQ(descriptor.timingConfidence, domain::TimingConfidence::kVerifiedCfr);

    SoftwareDecoder decoder{0U, descriptor, budget};
    std::atomic<bool> canceled = false;
    ASSERT_TRUE(decoder.open(canceled));

    const auto first = decoder.decodeExact(domain::FrameId{0}, canceled);
    const auto middle = decoder.decodeExact(domain::FrameId{6}, canceled);
    const auto last = decoder.decodeExact(domain::FrameId{11}, canceled);
    ASSERT_TRUE(first);
    ASSERT_TRUE(middle);
    ASSERT_TRUE(last);
    EXPECT_NE(frameHash(first.value()), frameHash(middle.value()));
    EXPECT_NE(frameHash(middle.value()), frameHash(last.value()));
}

TEST(SoftwareDecoderTests, ContinuesForwardWithoutSeekingAndFallsBackForReverseTargets) {
    platform::FrameBudget budget{2U * 1024U * 1024U};
    SoftwareDecoder decoder{
        0U, probeDescriptor(fixture("h264_a_320x180_30fps_12.mp4"), 0U), budget};
    std::atomic<bool> canceled = false;

    ASSERT_TRUE(decoder.open(canceled));
    const auto first = decoder.decodeExact(domain::FrameId{0}, canceled);
    ASSERT_TRUE(first);
    ASSERT_EQ(decoder.exactSeekCount(), 1U);

    const auto adjacent = decoder.decodeSequential(domain::FrameId{1}, canceled);
    const auto skipped = decoder.decodeSequential(domain::FrameId{6}, canceled);
    ASSERT_TRUE(adjacent);
    ASSERT_TRUE(skipped);
    EXPECT_EQ(decoder.exactSeekCount(), 1U);
    EXPECT_NE(frameHash(first.value()), frameHash(adjacent.value()));
    EXPECT_NE(frameHash(adjacent.value()), frameHash(skipped.value()));

    const auto reverse = decoder.decodeSequential(domain::FrameId{3}, canceled);
    ASSERT_TRUE(reverse);
    EXPECT_EQ(decoder.exactSeekCount(), 2U);
}

// A canceled decode is a clean stop, not corruption: the owning actor must not reopen the
// source for it, so the decoder reports the interruption and stays reusable afterwards. Reopening
// after every cancellation used to add a full demux plus index rebuild to every seek.
TEST(SoftwareDecoderTests, InterruptedDecodeIsReportedAndLeavesTheDecoderReusable) {
    platform::FrameBudget budget{2U * 1024U * 1024U};
    SoftwareDecoder decoder{
        0U, probeDescriptor(fixture("h264_a_320x180_30fps_12.mp4"), 0U), budget};
    std::atomic<bool> canceled = false;

    ASSERT_TRUE(decoder.open(canceled));
    const auto warm = decoder.decodeExact(domain::FrameId{4}, canceled);
    ASSERT_TRUE(warm);
    EXPECT_FALSE(decoder.lastDecodeInterrupted());

    canceled.store(true, std::memory_order_release);
    const auto interrupted = decoder.decodeExact(domain::FrameId{9}, canceled);
    EXPECT_FALSE(interrupted);
    EXPECT_TRUE(decoder.lastDecodeInterrupted());

    canceled.store(false, std::memory_order_release);
    const auto resumed = decoder.decodeExact(domain::FrameId{9}, canceled);
    ASSERT_TRUE(resumed);
    EXPECT_FALSE(decoder.lastDecodeInterrupted());
    EXPECT_EQ(resumed.value().presentationTime, domain::MediaTime{300000});
    EXPECT_NE(frameHash(resumed.value()), frameHash(warm.value()));
}

TEST(SoftwareDecoderTests, SequentialMpeg4DecodePreservesBufferedPacketState) {
    platform::FrameBudget budget{1024U * 1024U};
    SoftwareDecoder decoder{1U, probeDescriptor(fixture("mpeg4_64x48_30fps_12.mp4"), 1U), budget};
    std::atomic<bool> canceled = false;

    ASSERT_TRUE(decoder.open(canceled));
    const auto first = decoder.decodeExact(domain::FrameId{0}, canceled);
    const auto second = decoder.decodeSequential(domain::FrameId{1}, canceled);
    const auto middle = decoder.decodeSequential(domain::FrameId{6}, canceled);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(middle);
    EXPECT_EQ(decoder.exactSeekCount(), 1U);
    EXPECT_NE(frameHash(first.value()), frameHash(second.value()));
    EXPECT_NE(frameHash(second.value()), frameHash(middle.value()));
}

TEST(SoftwareDecoderTests, LazilyIndexesAnEndGapAndReturnsTheExactFinalOrdinal) {
    platform::FrameBudget budget{1024U * 1024U};
    SoftwareDecoder decoder{
        0U, probeDescriptor(fixture("h264_end_pts_gap_64x48_30fps_12.mp4"), 0U), budget};
    std::atomic<bool> canceled = false;

    ASSERT_TRUE(decoder.open(canceled));
    const auto last = decoder.decodeExact(domain::FrameId{11}, canceled);
    ASSERT_TRUE(last);
    EXPECT_EQ(last.value().presentationTime, domain::MediaTime{400000});
    EXPECT_NE(frameHash(last.value()), 0U);
}

TEST(SoftwareDecoderTests, LazilyIndexesAMiddleGapWithoutSubstitutingANeighbour) {
    platform::FrameBudget budget{1024U * 1024U};
    SoftwareDecoder decoder{
        1U, probeDescriptor(fixture("h264_middle_pts_gap_64x48_30fps_12.mp4"), 1U), budget};
    std::atomic<bool> canceled = false;

    ASSERT_TRUE(decoder.open(canceled));
    const auto beforeGap = decoder.decodeExact(domain::FrameId{5}, canceled);
    const auto afterGap = decoder.decodeExact(domain::FrameId{6}, canceled);
    ASSERT_TRUE(beforeGap);
    ASSERT_TRUE(afterGap);
    EXPECT_EQ(beforeGap.value().presentationTime, domain::MediaTime{166667});
    EXPECT_EQ(afterGap.value().presentationTime, domain::MediaTime{233333});
    EXPECT_EQ(frameHash(beforeGap.value()), 12'440'303'095'298'635'557ULL);
    EXPECT_EQ(frameHash(afterGap.value()), 2'672'727'306'961'435'429ULL);
}

// The VFR fixture h264_vfr_320x180_12.mp4 runs a 512-tick cadence for frames 0-6, then opens a
// doubled 1024-tick gap to frame 7 before returning to 512. The decoder addresses every frame by
// its indexed display-order PTS, so each FrameId must map to the exact rounded presentation time
// and reproduce the deterministic planar yuv420p hash recorded in tests/fixtures/manifest.json.
TEST(SoftwareDecoderTests, VfrPresentationTimesAndPlanarHashesMatchIndexedDisplayOrder) {
    platform::FrameBudget budget{1024U * 1024U};
    const auto descriptor = probeDescriptor(fixture("h264_vfr_320x180_12.mp4"), 0U);
    ASSERT_EQ(descriptor.timingConfidence, domain::TimingConfidence::kVariableFrameRate);
    ASSERT_FALSE(descriptor.frameRate.has_value());
    ASSERT_EQ(descriptor.frameCount.value, 12);

    SoftwareDecoder decoder{0U, descriptor, budget};
    std::atomic<bool> canceled = false;
    ASSERT_TRUE(decoder.open(canceled));

    // round(pts * 1'000'000 / 15'360) for pts in
    // {0,512,1024,1536,2048,2560,3072,4096,5120,6144,7168,8192}.
    constexpr std::array<std::int64_t, 12> kExpectedMicroseconds{
        0, 33333, 66667, 100000, 133333, 166667, 200000, 266667, 333333, 400000, 466667, 533333};
    static_assert(kExpectedMicroseconds.size() == 12U);

    constexpr std::array<std::string_view, 12> kExpectedHashes{
        "4e280dda1516f8f787657b37e70a5514ab1c79a702f7052de5273f49fe68d401",
        "9fa77846200b45fed2cbc19b64870176b2c8b65c2fbd86bae4cc519121577cc4",
        "59e52f4c6b3fddb088ac28dc88578af1e73866193ae7214f177ac58fad5ab9b8",
        "dbe635901f14765b24d58d13ee8c843c209b5a28cf9a38bb66c5cc2a8f5043dc",
        "c02d2d3cd4914b61c00a9f7ffb41510647c988313ac59f04f38f130935822b32",
        "4703ad973cfe3aa8ec0a18ebfe2b0821c8f60b97b303f7136ed2d5c2ef9456ef",
        "bf8620ad58b4c2da23fb8584570a9ba047c961be29beec067f4de167534cbe4e",
        "d427a60004402873d5e36aed70e090095f94871af123f0671e8c4ea3dd00f3fc",
        "b064572dcab15dfedb2633890b5c945a73850b174bb2e43b477c85a40c3f690a",
        "dde292ecd3470c8d9d384bbb06dadfa27dd64a8d869d627ef02c6c50e1423f98",
        "6369cc823b713f5d40b5e6e3c52a3cbf2efc1eb251e1e2274e145c7c38e8b012",
        "48821dbe65664dd4ea3dcddde0fae3ca3ce17108b79792212b503e8341e59ce4",
    };

    for (std::int64_t frame = 0; frame < 12; ++frame) {
        const auto decoded = decoder.decodeExact(domain::FrameId{frame}, canceled);
        ASSERT_TRUE(decoded);
        ASSERT_TRUE(decoded.value().handle.isValid());
        EXPECT_EQ(decoded.value().handle.geometry().width, 320U);
        EXPECT_EQ(decoded.value().handle.geometry().height, 180U);
        EXPECT_EQ(decoded.value().presentationTime,
                  domain::MediaTime{kExpectedMicroseconds[static_cast<std::size_t>(frame)]});
        EXPECT_EQ(planarYuv420Sha256(decoded.value()),
                  std::string{kExpectedHashes[static_cast<std::size_t>(frame)]});
    }

    EXPECT_EQ(decoder.exactSeekCount(), 12U);
    EXPECT_EQ(budget.reservedBytes(), 0U);
}

// Exact seek and the adjacent/sequential walk must never drift identity across the long PTS gap. We
// decode every FrameId both ways (one decoder per strategy, sharing one budget), then assert the
// is strictly wider than a regular cadence step, and the walk performs exactly one seek.
TEST(SoftwareDecoderTests, VfrSequentialWalkAcrossLongPtsGapReproducesExactFrames) {
    platform::FrameBudget budget{1024U * 1024U};
    const auto descriptor = probeDescriptor(fixture("h264_vfr_320x180_12.mp4"), 0U);
    ASSERT_EQ(descriptor.timingConfidence, domain::TimingConfidence::kVariableFrameRate);

    SoftwareDecoder exact{0U, descriptor, budget};
    SoftwareDecoder walk{0U, descriptor, budget};
    std::atomic<bool> canceled = false;
    ASSERT_TRUE(exact.open(canceled));
    ASSERT_TRUE(walk.open(canceled));

    std::array<std::string, 12> exactHash{};
    for (std::int64_t frame = 0; frame < 12; ++frame) {
        const auto decoded = exact.decodeExact(domain::FrameId{frame}, canceled);
        ASSERT_TRUE(decoded);
        ASSERT_TRUE(decoded.value().handle.isValid());
        exactHash[static_cast<std::size_t>(frame)] = planarYuv420Sha256(decoded.value());
    }

    std::array<std::string, 12> walkHash{};
    std::array<std::int64_t, 12> walkTime{};
    for (std::int64_t frame = 0; frame < 12; ++frame) {
        const auto decoded = (frame == 0) ? walk.decodeExact(domain::FrameId{0}, canceled)
                                          : walk.decodeSequential(domain::FrameId{frame}, canceled);
        ASSERT_TRUE(decoded);
        ASSERT_TRUE(decoded.value().handle.isValid());
        const auto value = decoded.value();
        walkHash[static_cast<std::size_t>(frame)] = planarYuv420Sha256(value);
        walkTime[static_cast<std::size_t>(frame)] = value.presentationTime.microseconds();
    }

    for (std::size_t i = 0; i < exactHash.size(); ++i) {
        EXPECT_EQ(walkHash[i], exactHash[i]);
    }
    for (std::size_t i = 1; i < walkTime.size(); ++i) {
        EXPECT_GT(walkTime[i], walkTime[i - 1U]);
    }
    EXPECT_GT(walkTime[7] - walkTime[6], walkTime[1] - walkTime[0]);

    EXPECT_EQ(walk.exactSeekCount(), 1U);
    EXPECT_EQ(exact.exactSeekCount(), 12U);
    EXPECT_EQ(budget.reservedBytes(), 0U);
}

TEST(SoftwareDecoderTests, UsesIndexedCountAndOrdinalsForSourcesWithoutNbFrames) {
    platform::FrameBudget budget{1024U * 1024U};
    SoftwareDecoder decoder{
        0U, probeDescriptor(fixture("h264_no_count_64x48_30fps_12.mkv"), 0U), budget};
    std::atomic<bool> canceled = false;

    ASSERT_TRUE(decoder.open(canceled));
    const auto last = decoder.decodeExact(domain::FrameId{11}, canceled);
    ASSERT_TRUE(last);
    EXPECT_NE(frameHash(last.value()), 0U);
}

TEST(SoftwareDecoderTests, PreservesDisplayOrdinalsForANonZeroStreamStart) {
    platform::FrameBudget budget{1024U * 1024U};
    SoftwareDecoder decoder{
        1U, probeDescriptor(fixture("h264_nonzero_start_64x48_30fps_12.mp4"), 1U), budget};
    std::atomic<bool> canceled = false;

    ASSERT_TRUE(decoder.open(canceled));
    const auto first = decoder.decodeExact(domain::FrameId{0}, canceled);
    const auto last = decoder.decodeExact(domain::FrameId{11}, canceled);
    ASSERT_TRUE(first);
    ASSERT_TRUE(last);
    EXPECT_EQ(first.value().presentationTime, domain::MediaTime{1'000'000});
    EXPECT_EQ(last.value().presentationTime, domain::MediaTime{1'366'667});
}

// The original failure report: D:\Videos\Captures\王者荣耀世界 2026-06-01 13-25-17.mp4 failed as
// "The indexed timestamp did not identify decoded frame 57 (target 62062, decoded 63063)" during
// sequential playback after software fallback. Synthetic libx264 fixtures could not reproduce the
// open-GOP structure, so the regression walks every display ordinal of a provided real capture
// through exact seeks; skipped when the media is not configured so CI stays hermetic.
TEST(SoftwareDecoderTests, ExactDecodeWalksEveryOrdinalOfProvidedGameDvrCapture) {
    // std::getenv raises C4996 under /W4 /WX, so read the variable through the secure API.
    char* configuredBuffer = nullptr;
    std::size_t configuredSize = 0U;
    errno_t configuredError =
        _dupenv_s(&configuredBuffer, &configuredSize, "DVS_TEST_GAMEDVR_CAPTURE");
    if (configuredError != 0 || configuredBuffer == nullptr || configuredSize == 0U) {
        std::free(configuredBuffer);
        GTEST_SKIP() << "DVS_TEST_GAMEDVR_CAPTURE not set or missing.";
    }
    const std::unique_ptr<char[]> configured{configuredBuffer};
    if (!std::filesystem::exists(configured.get())) {
        GTEST_SKIP() << "Configured capture does not exist: " << configured.get();
    }
    const std::filesystem::path path{configured.get()};
    const auto descriptor = probeDescriptor(path, 0U);
    platform::FrameBudget budget{64U * 1024U * 1024U};
    SoftwareDecoder decoder{0U, descriptor, budget};
    std::atomic<bool> canceled = false;
    ASSERT_TRUE(decoder.open(canceled));

    const std::int64_t count = static_cast<std::int64_t>(descriptor.frameCount.value);
    // Reverse order maximizes the number of cold seeks across GOP boundaries; the original
    // failure surfaced as overshoot exactly at such a boundary (frame 57, target 62062 us).
    for (std::int64_t frame = count - 1; frame >= 0; --frame) {
        const auto decoded = decoder.decodeExact(domain::FrameId{frame}, canceled);
        ASSERT_TRUE(decoded) << "exact decode failed at display ordinal " << frame << ": "
                             << decoded.error().technicalDetail;
    }
    EXPECT_EQ(budget.reservedBytes(), 0U);
}

TEST(SoftwareDecoderTests, ReportsWhySoftwareFallbackIsActiveWithoutASharedDevice) {
    platform::FrameBudget budget{1024U * 1024U};
    SoftwareDecoder decoder{
        0U, probeDescriptor(fixture("h264_a_320x180_30fps_12.mp4"), 0U), budget};
    std::atomic<bool> canceled = false;

    ASSERT_TRUE(decoder.open(canceled));
    EXPECT_EQ(decoder.backend(), DecoderBackend::Software);
    EXPECT_EQ(decoder.deviceGeneration(), domain::DeviceGeneration{0U});
    EXPECT_FALSE(decoder.fallbackReason().empty());
    const auto decoded = decoder.decodeExact(domain::FrameId{0}, canceled);
    ASSERT_TRUE(decoded);
    EXPECT_NE(std::dynamic_pointer_cast<const platform::CpuNv12FrameResource>(
                  decoded.value().handle.resource()),
              nullptr);
}

TEST(SoftwareDecoderHardwareTests, RetainsNv12AndP010D3d11VaSurfacesOnTheSharedDevice) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel{};
    const HRESULT created =
        D3D11CreateDevice(nullptr,
                          D3D_DRIVER_TYPE_HARDWARE,
                          nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                          nullptr,
                          0U,
                          D3D11_SDK_VERSION,
                          device.GetAddressOf(),
                          &featureLevel,
                          context.GetAddressOf());
    if (FAILED(created)) {
        GTEST_SKIP() << "No hardware D3D11 video device is available.";
    }

    auto broker = std::make_shared<platform::GraphicsDeviceBroker>();
    ASSERT_EQ(broker->adoptQtDevice(device.Get(), context.Get()),
              platform::GraphicsDeviceBrokerResult::Ready);
    platform::FrameBudget budget{8U * 1024U * 1024U};
    SoftwareDecoder decoder{
        0U,
        probeDescriptor(fixture("h264_a_320x180_30fps_12.mp4"), 0U),
        budget,
        nullptr,
        broker,
    };
    std::atomic<bool> canceled = false;

    ASSERT_TRUE(decoder.open(canceled)) << decoder.fallbackReason();
    const auto decoded = decoder.decodeExact(domain::FrameId{0}, canceled);
    ASSERT_TRUE(decoded) << (decoded ? "" : decoded.error().technicalDetail);
    EXPECT_EQ(decoder.backend(), DecoderBackend::D3d11Va) << decoder.fallbackReason();
    const auto resource = std::dynamic_pointer_cast<const platform::D3d11DecodedFrameResource>(
        decoded.value().handle.resource());
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(resource->format(), application::NormalizedFrameFormat::Nv12_8);
    EXPECT_EQ(resource->deviceGeneration(), broker->currentGeneration());
    EXPECT_NE(resource->texture(), nullptr);
    EXPECT_GT(resource->byteCount(), 0U);
    EXPECT_EQ(decoded.value().handle.geometry().width, 320U);
    EXPECT_EQ(decoded.value().handle.geometry().height, 180U);

    SoftwareDecoder tenBitDecoder{
        1U,
        probeDescriptor(fixture("h265_10bit_320x180_30fps_12.mp4"), 1U),
        budget,
        nullptr,
        broker,
    };
    ASSERT_TRUE(tenBitDecoder.open(canceled)) << tenBitDecoder.fallbackReason();
    const auto tenBitDecoded = tenBitDecoder.decodeExact(domain::FrameId{0}, canceled);
    ASSERT_TRUE(tenBitDecoded) << (tenBitDecoded ? "" : tenBitDecoded.error().technicalDetail);
    EXPECT_EQ(tenBitDecoder.backend(), DecoderBackend::D3d11Va) << tenBitDecoder.fallbackReason();
    const auto p010Resource = std::dynamic_pointer_cast<const platform::D3d11DecodedFrameResource>(
        tenBitDecoded.value().handle.resource());
    ASSERT_NE(p010Resource, nullptr);
    EXPECT_EQ(p010Resource->format(), application::NormalizedFrameFormat::P010_10);
    EXPECT_EQ(p010Resource->deviceGeneration(), broker->currentGeneration());
}

} // namespace
} // namespace dvs::media::internal
