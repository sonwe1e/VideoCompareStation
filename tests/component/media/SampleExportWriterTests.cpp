#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "dvs/application/SampleExport.h"
#include "dvs/application/SampleExportWriter.h"
#include "dvs/media/MediaProbe.h"
#include "dvs/media/SampleExportWriter.h"
#include "dvs/platform/CpuNv12FrameResource.h"
#include "dvs/platform/FrameBudget.h"

#include "SampleExportWriterTestHooks.h"
#include "SoftwareDecoder.h"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

namespace dvs::media {
namespace {

// Coverage note: no B-frame (reordered-picture) fixture exists under tests/fixtures/media. The
// writer addresses members through SoftwareDecoder's display-order ordinals, whose acceptance
// rule (exact indexed PTS) is what makes B-frame streams correct, but that behaviour is not
// exercised here - the B-frame acceptance item of P08 stays unverified until such a fixture is
// added, and these tests make no claim about it.

[[nodiscard]] std::filesystem::path fixture(const char* const name) {
    return std::filesystem::path{DVS_MEDIA_FIXTURE_DIR} / name;
}

class ScopedTempDirectory final {
public:
    explicit ScopedTempDirectory(const std::string& label) {
        static std::atomic<int> sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("dvs_sample_export_" + label + "_" + std::to_string(stamp) + "_" +
                 std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    ScopedTempDirectory(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory(ScopedTempDirectory&&) = delete;
    ScopedTempDirectory& operator=(ScopedTempDirectory&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// Phase-zero 60->30->60 decimation, so output position 2n + 1 is the generated instant bracketed
// by GT frames 2n and 2n + 2 at ratio 1/2 - exactly the shape the planner was built for.
[[nodiscard]] application::SampleTripletPlan
planForOutputFrame(const std::int64_t gtFrameCount,
                   const std::int64_t outputFrame,
                   const std::optional<application::SampleCropRect> crop = std::nullopt,
                   const std::optional<application::SampleFrameSize> frameSize = std::nullopt) {
    const auto protocol = domain::buildDecimatedProtocol(
        domain::DecimationPlan{.gtFrameCount = gtFrameCount, .stride = 2},
        domain::MappingOrigin::kManual,
        domain::TailPolicy::kTrimPartialInterval);
    EXPECT_TRUE(protocol.hasValue());
    const auto planned = application::planSampleTriplet(
        protocol.value(), domain::FrameId{outputFrame}, crop, frameSize);
    EXPECT_TRUE(planned.hasValue()) << planned.error().technicalDetail;
    return planned.value();
}

[[nodiscard]] application::SampleExportJob
makeJob(const std::filesystem::path& gt,
        const std::filesystem::path& candidate,
        const std::filesystem::path& outputDirectory,
        const application::SampleTripletPlan& plan,
        const std::string_view sampleId = "sample_under_test") {
    application::SampleExportJob job;
    job.requestId = 41U;
    job.sessionId = domain::SessionId{7U};
    job.sessionEpoch = domain::SessionEpoch{2U};
    job.sampleId = std::string{sampleId};
    job.plan = plan;
    job.gtAssetPath = gt;
    job.candidateAssetPath = candidate;
    job.outputDirectory = outputDirectory;
    return job;
}

// Independently decodes the named display ordinal and applies the same documented conversion as
// the writer (normalized NV12 -> RGB24 via swscale with the resource's matrix and range, then the
// plan's crop). Deriving the reference outside the exporter is what lets these tests catch a
// frame-ordinal or conversion mutation rather than trusting the code under test.
[[nodiscard]] std::vector<std::uint8_t>
referenceRgb24(const std::filesystem::path& asset,
               const std::int64_t frame,
               const std::optional<application::SampleCropRect>& crop = std::nullopt) {
    const auto descriptor = MediaProbe::inspect(asset, domain::SourceId{9U});
    EXPECT_TRUE(descriptor);
    platform::FrameBudget budget{16U * 1024U * 1024U};
    internal::SoftwareDecoder decoder{domain::SourceId{9U}, descriptor.value(), budget};
    std::atomic<bool> canceled{false};
    const domain::Status opened = decoder.open(canceled);
    EXPECT_TRUE(opened) << opened.error().technicalDetail;
    if (!opened) {
        return {};
    }
    const auto decoded = decoder.decodeExact(domain::FrameId{frame}, canceled);
    EXPECT_TRUE(decoded) << decoded.error().technicalDetail;
    if (!decoded) {
        return {};
    }
    const auto resource = std::dynamic_pointer_cast<const platform::CpuNv12FrameResource>(
        decoded.value().handle.resource());
    EXPECT_NE(resource, nullptr);
    if (resource == nullptr) {
        return {};
    }
    const platform::Nv12FrameLayout& layout = resource->layout();
    const std::size_t rgbStride = static_cast<std::size_t>(layout.width) * 3U;
    std::vector<std::uint8_t> full(static_cast<std::size_t>(layout.height) * rgbStride,
                                   std::uint8_t{0U});

    const std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scaler{
        sws_getContext(static_cast<int>(layout.width),
                       static_cast<int>(layout.height),
                       AV_PIX_FMT_NV12,
                       static_cast<int>(layout.width),
                       static_cast<int>(layout.height),
                       AV_PIX_FMT_RGB24,
                       SWS_BILINEAR,
                       nullptr,
                       nullptr,
                       nullptr),
        &sws_freeContext};
    EXPECT_NE(scaler.get(), nullptr);
    if (scaler.get() == nullptr) {
        return {};
    }
    const int* const coefficients = sws_getCoefficients(
        resource->colorMetadata().matrix == domain::ColorMatrix::kBt709 ? SWS_CS_ITU709
                                                                        : SWS_CS_ITU601);
    EXPECT_NE(coefficients, nullptr);
    const int sourceRange = resource->colorMetadata().range == domain::ColorRange::kFull ? 1 : 0;
    EXPECT_GE(sws_setColorspaceDetails(
                  scaler.get(), coefficients, sourceRange, coefficients, 1, 0, 1 << 16, 1 << 16),
              0);
    const std::array<const std::uint8_t*, 4> sourceSlices{
        resource->yPlane().data(),
        resource->uvPlane().data(),
        nullptr,
        nullptr,
    };
    const std::array<int, 4> sourceStrides{
        static_cast<int>(layout.yStride),
        static_cast<int>(layout.uvStride),
        0,
        0,
    };
    std::array<std::uint8_t*, 4> targetSlices{
        full.data(),
        nullptr,
        nullptr,
        nullptr,
    };
    const std::array<int, 4> targetStrides{
        static_cast<int>(rgbStride),
        0,
        0,
        0,
    };
    EXPECT_EQ(sws_scale(scaler.get(),
                        sourceSlices.data(),
                        sourceStrides.data(),
                        0,
                        static_cast<int>(layout.height),
                        targetSlices.data(),
                        targetStrides.data()),
              static_cast<int>(layout.height));

    if (!crop.has_value()) {
        return full;
    }
    std::vector<std::uint8_t> cropped(static_cast<std::size_t>(crop->height) *
                                          static_cast<std::size_t>(crop->width) * 3U,
                                      std::uint8_t{0U});
    const std::size_t croppedStride = static_cast<std::size_t>(crop->width) * 3U;
    for (std::int64_t row = 0; row < crop->height; ++row) {
        const std::size_t sourceOffset = static_cast<std::size_t>(crop->y + row) * rgbStride +
                                         static_cast<std::size_t>(crop->x) * 3U;
        std::copy_n(full.begin() + static_cast<std::ptrdiff_t>(sourceOffset),
                    croppedStride,
                    cropped.begin() +
                        static_cast<std::ptrdiff_t>(static_cast<std::size_t>(row) * croppedStride));
    }
    return cropped;
}

// Reads one exported member back to tightly packed RGB24 rows through WIC. The repository's
// locked FFmpeg build is configured --disable-zlib, so its avcodec carries no PNG decoder (nor
// encoder), and decodeStillImageBytes cannot verify PNG members in this tree; WIC gives the test
// an independent decode path that never shares code with the exporter under test.
[[nodiscard]] std::vector<std::uint8_t> memberRgbTriples(const std::filesystem::path& png) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        ADD_FAILURE() << "COM could not be initialized for the test.";
        return {};
    }
    const int uninitialize = SUCCEEDED(initialized) ? 1 : 0;

    std::vector<std::uint8_t> rgb;
    // Every WIC interface must be released before CoUninitialize below: tearing down the
    // apartment first leaves the outstanding Release calls touching a dead apartment, which
    // crashes the test process. The inner scope ends - and therefore releases - while the
    // apartment is still alive.
    UINT width = 0U;
    UINT height = 0U;
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        do {
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory,
                                        nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory)))) {
                ADD_FAILURE() << "The WIC imaging factory is unavailable.";
                break;
            }
            if (FAILED(factory->CreateDecoderFromFilename(png.c_str(),
                                                          nullptr,
                                                          GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand,
                                                          &decoder))) {
                ADD_FAILURE() << "The exported member could not be opened for decode.";
                break;
            }
            if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) ||
                width == 0U || height == 0U) {
                ADD_FAILURE() << "The exported member has no decodable pixel frame.";
                break;
            }
            if (FAILED(factory->CreateFormatConverter(&converter)) ||
                FAILED(converter->Initialize(frame.Get(),
                                             GUID_WICPixelFormat24bppBGR,
                                             WICBitmapDitherTypeNone,
                                             nullptr,
                                             0.0,
                                             WICBitmapPaletteTypeCustom))) {
                ADD_FAILURE() << "The exported member could not be converted for comparison.";
                break;
            }
            const std::size_t stride = static_cast<std::size_t>(width) * 3U;
            std::vector<std::uint8_t> bgr(static_cast<std::size_t>(height) * stride,
                                          std::uint8_t{0U});
            if (FAILED(converter->CopyPixels(nullptr,
                                             static_cast<UINT>(stride),
                                             static_cast<UINT>(bgr.size()),
                                             bgr.data()))) {
                ADD_FAILURE() << "The exported member's pixels could not be read.";
                break;
            }
            rgb.reserve(static_cast<std::size_t>(height) * stride);
            for (std::size_t offset = 0U; offset + 2U < bgr.size(); offset += 3U) {
                rgb.push_back(bgr[offset + 2U]);
                rgb.push_back(bgr[offset + 1U]);
                rgb.push_back(bgr[offset]);
            }
        } while (false);
    }

    if (uninitialize != 0) {
        CoUninitialize();
    }
    return rgb;
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
pngIhdrSize(const std::filesystem::path& png) {
    std::ifstream stream{png, std::ios::binary};
    std::array<std::uint8_t, 24> header{};
    stream.read(reinterpret_cast<char*>(header.data()),
                static_cast<std::streamsize>(header.size()));
    EXPECT_TRUE(stream.good());
    const auto bigEndian = [&header](const std::size_t offset) {
        return (static_cast<std::uint32_t>(header[offset]) << 24U) |
               (static_cast<std::uint32_t>(header[offset + 1U]) << 16U) |
               (static_cast<std::uint32_t>(header[offset + 2U]) << 8U) |
               static_cast<std::uint32_t>(header[offset + 3U]);
    };
    return {bigEndian(16U), bigEndian(20U)};
}

[[nodiscard]] nlohmann::json readJsonFile(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    std::string text{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    return nlohmann::json::parse(text);
}

[[nodiscard]] std::vector<std::string> sortedEntryNames(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

void writeTextFileForTest(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream << text;
}

std::atomic_bool gCancelFlag{false};

void cancelOnceGtMemberIsStaged(const std::filesystem::path& member) {
    if (member.filename() == "gt.png") {
        gCancelFlag.store(true, std::memory_order_release);
    }
}

void sabotageGtMemberAfterStaging(const std::filesystem::path& member) {
    if (member.filename() == "gt.png") {
        std::error_code ignored;
        std::filesystem::resize_file(member, 4U, ignored);
    }
}

constexpr std::array<const char*, 4> kMemberNames{"input0", "gt", "input1", "prediction"};

TEST(SampleExportWriterTests, ExportsThePlannedTripletWithExactFrameIdentityAndManifest) {
    const std::filesystem::path gt = fixture("h264_a_320x180_30fps_12.mp4");
    const std::filesystem::path candidate = fixture("h264_b_160x90_30fps_12.mp4");
    const ScopedTempDirectory workspace{"identity"};
    const std::filesystem::path output = workspace.path() / "out";

    const application::SampleTripletPlan plan = planForOutputFrame(12, 1);
    const std::atomic_bool cancel{false};
    SampleExportWriter writer;
    const application::SampleExportReport report =
        writer.perform(makeJob(gt, candidate, output, plan), cancel);

    ASSERT_EQ(report.outcome, application::SampleExportOutcome::kCompleted)
        << report.technicalDetail;
    EXPECT_EQ(report.requestId, 41U);
    EXPECT_TRUE(report.technicalDetail.empty());

    const std::filesystem::path finalDirectory = output / "sample_under_test";
    EXPECT_EQ(sortedEntryNames(output), std::vector<std::string>({"sample_under_test"}));
    EXPECT_EQ(sortedEntryNames(finalDirectory),
              std::vector<std::string>({"frames", "manifest.json"}));
    EXPECT_EQ(sortedEntryNames(finalDirectory / "frames"),
              std::vector<std::string>({"gt.png", "input0.png", "input1.png", "prediction.png"}));

    // Every member must decode back to exactly the RGB pixels of its planned display ordinal.
    // A one-frame slip anywhere in the export chain changes the bytes and fails here.
    const std::vector<std::vector<std::uint8_t>> expected{
        referenceRgb24(gt, 0),
        referenceRgb24(gt, 1),
        referenceRgb24(gt, 2),
        referenceRgb24(candidate, 1),
    };
    // The prediction member comes from the 160x90 candidate asset; the GT members are 320x180.
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> expectedSizes{
        {320U, 180U},
        {320U, 180U},
        {320U, 180U},
        {160U, 90U},
    };
    ASSERT_EQ(report.publishedMembers.size(), 4U);
    for (std::size_t index = 0U; index < kMemberNames.size(); ++index) {
        SCOPED_TRACE(kMemberNames[index]);
        const std::filesystem::path& member = report.publishedMembers[index];
        EXPECT_EQ(member, finalDirectory / "frames" / (std::string{kMemberNames[index]} + ".png"));
        EXPECT_TRUE(std::filesystem::exists(member));
        EXPECT_EQ(pngIhdrSize(member), expectedSizes[index]);
        EXPECT_EQ(memberRgbTriples(member), expected[index]);
    }
    EXPECT_EQ(report.manifestPath, finalDirectory / "manifest.json");

    const nlohmann::json manifest = readJsonFile(report.manifestPath);
    EXPECT_EQ(manifest["schema_version"], 1);
    EXPECT_EQ(manifest["sample_id"], "sample_under_test");
    EXPECT_EQ(manifest["interpolation"]["input0_gt_index"], 0);
    EXPECT_EQ(manifest["interpolation"]["target_gt_index"], 1);
    EXPECT_EQ(manifest["interpolation"]["input1_gt_index"], 2);
    EXPECT_EQ(manifest["interpolation"]["index_base"], 0);
    EXPECT_EQ(manifest["interpolation"]["target_time_ratio"], nlohmann::json::array({1, 2}));
    EXPECT_EQ(manifest["interpolation"]["mapping_origin"], "manual");
    EXPECT_EQ(manifest["members"]["input0"], "frames/input0.png");
    EXPECT_EQ(manifest["members"]["gt"], "frames/gt.png");
    EXPECT_EQ(manifest["members"]["input1"], "frames/input1.png");
    EXPECT_EQ(manifest["members"]["prediction"], "frames/prediction.png");
    EXPECT_TRUE(manifest["crop"].is_null());
    EXPECT_NE(manifest["pixel_conversion_policy"].get<std::string>().find("swscale"),
              std::string::npos);
    // Which encoder serves a build is a probe outcome, not a constant: the repository's FFmpeg is
    // built --disable-zlib (no avcodec png encoder), so this tree answers "windows-wic" while a
    // fuller build answers "ffmpeg-avcodec". Both names are legitimate; anything else is not.
    const std::string selectedEncoder = manifest["png_encoder"].get<std::string>();
    EXPECT_TRUE(selectedEncoder == "ffmpeg-avcodec" || selectedEncoder == "windows-wic")
        << selectedEncoder;
    EXPECT_EQ(manifest["exporter"], "dvs::media::SampleExportWriter/1");
    // Content fingerprints are E04 lineage work; an honest manifest omits them instead of
    // faking digests.
    EXPECT_FALSE(manifest.contains("sha256"));
}

TEST(SampleExportWriterTests, SelectsExactDisplayOrdinalsFromAVfrSource) {
    // Output position 7 of the stride-2 protocol lands on GT 7, the frame after the fixture's
    // doubled 1024-tick PTS gap. Only display-order addressing picks it; a timestamp
    // approximation rounds onto a neighbour.
    const std::filesystem::path gt = fixture("h264_vfr_320x180_12.mp4");
    const ScopedTempDirectory workspace{"vfr"};
    const std::filesystem::path output = workspace.path() / "out";

    const application::SampleTripletPlan plan = planForOutputFrame(12, 7);
    ASSERT_EQ(plan.input0GtFrame.value(), 6);
    ASSERT_EQ(plan.targetGtFrame.value(), 7);
    ASSERT_EQ(plan.input1GtFrame.value(), 8);

    const std::atomic_bool cancel{false};
    SampleExportWriter writer;
    const application::SampleExportReport report =
        writer.perform(makeJob(gt, fixture("h264_b_160x90_30fps_12.mp4"), output, plan), cancel);
    ASSERT_EQ(report.outcome, application::SampleExportOutcome::kCompleted)
        << report.technicalDetail;

    const std::filesystem::path frames = output / "sample_under_test" / "frames";
    EXPECT_EQ(memberRgbTriples(frames / "input0.png"), referenceRgb24(gt, 6));
    EXPECT_EQ(memberRgbTriples(frames / "gt.png"), referenceRgb24(gt, 7));
    EXPECT_EQ(memberRgbTriples(frames / "input1.png"), referenceRgb24(gt, 8));
    // The neighbours of the doubled gap must not have been substituted for the target.
    const std::vector<std::uint8_t> target = memberRgbTriples(frames / "gt.png");
    EXPECT_NE(target, referenceRgb24(gt, 5));
    EXPECT_NE(target, referenceRgb24(gt, 9));
}

TEST(SampleExportWriterTests, DecodesExactMembersAcrossGopBoundariesOfAMultiGopSource) {
    // Keyframes sit at ordinals 0/3/6/9 of this fixture; input1 of output position 5 is GT 6,
    // the first frame of the third GOP, while target 5 decodes from the second GOP's keyframe.
    const std::filesystem::path gt = fixture("h264_nonzero_start_multigop_64x48_30fps_12.mp4");
    const ScopedTempDirectory workspace{"multigop"};
    const std::filesystem::path output = workspace.path() / "out";

    const application::SampleTripletPlan plan = planForOutputFrame(12, 5);
    ASSERT_EQ(plan.input0GtFrame.value(), 4);
    ASSERT_EQ(plan.targetGtFrame.value(), 5);
    ASSERT_EQ(plan.input1GtFrame.value(), 6);

    const std::atomic_bool cancel{false};
    SampleExportWriter writer;
    const application::SampleExportReport report =
        writer.perform(makeJob(gt, fixture("h264_b_160x90_30fps_12.mp4"), output, plan), cancel);
    ASSERT_EQ(report.outcome, application::SampleExportOutcome::kCompleted)
        << report.technicalDetail;

    const std::filesystem::path frames = output / "sample_under_test" / "frames";
    EXPECT_EQ(memberRgbTriples(frames / "input0.png"), referenceRgb24(gt, 4));
    EXPECT_EQ(memberRgbTriples(frames / "gt.png"), referenceRgb24(gt, 5));
    EXPECT_EQ(memberRgbTriples(frames / "input1.png"), referenceRgb24(gt, 6));
}

TEST(SampleExportWriterTests, PreservesDisplayOrdinalsOfANonZeroStartSource) {
    // The stream's first PTS is one second; ordinals must stay display-order based, so the
    // container origin must not shift which frame "target 3" names.
    const std::filesystem::path gt = fixture("h264_nonzero_start_64x48_30fps_12.mp4");
    const ScopedTempDirectory workspace{"nonzero"};
    const std::filesystem::path output = workspace.path() / "out";

    const application::SampleTripletPlan plan = planForOutputFrame(12, 3);
    const std::atomic_bool cancel{false};
    SampleExportWriter writer;
    const application::SampleExportReport report =
        writer.perform(makeJob(gt, fixture("h264_b_160x90_30fps_12.mp4"), output, plan), cancel);
    ASSERT_EQ(report.outcome, application::SampleExportOutcome::kCompleted)
        << report.technicalDetail;

    const std::filesystem::path frames = output / "sample_under_test" / "frames";
    EXPECT_EQ(memberRgbTriples(frames / "gt.png"), referenceRgb24(gt, 3));
}

TEST(SampleExportWriterTests, AppliesThePlanCropToEveryMember) {
    const std::filesystem::path gt = fixture("h264_a_320x180_30fps_12.mp4");
    const std::filesystem::path candidate = fixture("h264_b_160x90_30fps_12.mp4");
    const ScopedTempDirectory workspace{"crop"};
    const std::filesystem::path output = workspace.path() / "out";

    const application::SampleCropRect crop{.x = 40, .y = 20, .width = 64, .height = 48};
    const application::SampleTripletPlan plan =
        planForOutputFrame(12, 1, crop, application::SampleFrameSize{.width = 320, .height = 180});

    const std::atomic_bool cancel{false};
    SampleExportWriter writer;
    const application::SampleExportReport report =
        writer.perform(makeJob(gt, candidate, output, plan), cancel);
    ASSERT_EQ(report.outcome, application::SampleExportOutcome::kCompleted)
        << report.technicalDetail;

    const std::filesystem::path frames = output / "sample_under_test" / "frames";
    for (const char* const name : kMemberNames) {
        SCOPED_TRACE(name);
        EXPECT_EQ(pngIhdrSize(frames / (std::string{name} + ".png")),
                  (std::pair<std::uint32_t, std::uint32_t>{64U, 48U}));
    }
    EXPECT_EQ(memberRgbTriples(frames / "input0.png"), referenceRgb24(gt, 0, crop));
    EXPECT_EQ(memberRgbTriples(frames / "gt.png"), referenceRgb24(gt, 1, crop));
    EXPECT_EQ(memberRgbTriples(frames / "input1.png"), referenceRgb24(gt, 2, crop));
    EXPECT_EQ(memberRgbTriples(frames / "prediction.png"), referenceRgb24(candidate, 1, crop));

    const nlohmann::json manifest = readJsonFile(output / "sample_under_test" / "manifest.json");
    ASSERT_TRUE(manifest["crop"].is_object());
    EXPECT_EQ(manifest["crop"]["x"], 40);
    EXPECT_EQ(manifest["crop"]["y"], 20);
    EXPECT_EQ(manifest["crop"]["width"], 64);
    EXPECT_EQ(manifest["crop"]["height"], 48);
}

TEST(SampleExportWriterTests, CancelsMidFlightWithoutLeavingAHalfPublishedSample) {
    const ScopedTempDirectory workspace{"cancel"};
    const std::filesystem::path output = workspace.path() / "out";
    const application::SampleTripletPlan plan = planForOutputFrame(12, 1);

    gCancelFlag.store(false, std::memory_order_release);
    testing::ScopedSampleExportStagedMemberHook hook{&cancelOnceGtMemberIsStaged};
    SampleExportWriter writer;
    const application::SampleExportReport report =
        writer.perform(makeJob(fixture("h264_a_320x180_30fps_12.mp4"),
                               fixture("h264_b_160x90_30fps_12.mp4"),
                               output,
                               plan),
                       gCancelFlag);

    EXPECT_EQ(report.outcome, application::SampleExportOutcome::kCanceled);
    EXPECT_EQ(report.requestId, 41U);
    EXPECT_TRUE(report.publishedMembers.empty());
    EXPECT_FALSE(std::filesystem::exists(output / "sample_under_test"));
    ASSERT_TRUE(std::filesystem::exists(output));
    EXPECT_TRUE(sortedEntryNames(output).empty());
}

TEST(SampleExportWriterTests, FailsCleanlyWhenTheCandidateAssetIsCorrupt) {
    const ScopedTempDirectory workspace{"corrupt"};
    const std::filesystem::path output = workspace.path() / "out";
    const application::SampleTripletPlan plan = planForOutputFrame(12, 1);

    const std::atomic_bool cancel{false};
    SampleExportWriter writer;
    const application::SampleExportReport report = writer.perform(
        makeJob(fixture("h264_a_320x180_30fps_12.mp4"), fixture("corrupt_h264.mp4"), output, plan),
        cancel);

    EXPECT_EQ(report.outcome, application::SampleExportOutcome::kFailed);
    EXPECT_FALSE(report.technicalDetail.empty());
    EXPECT_TRUE(report.publishedMembers.empty());
    EXPECT_FALSE(std::filesystem::exists(output / "sample_under_test"));
    ASSERT_TRUE(std::filesystem::exists(output));
    EXPECT_TRUE(sortedEntryNames(output).empty());
}

TEST(SampleExportWriterTests, ReplacesStaleStagingAndAnExistingSampleOfTheSameRequest) {
    const ScopedTempDirectory workspace{"restage"};
    const std::filesystem::path output = workspace.path() / "out";
    const application::SampleTripletPlan plan = planForOutputFrame(12, 1);

    // A crashed earlier run of request 41 left its staging tree behind, and a previous export
    // published a sample directory of its own.
    std::filesystem::create_directories(output / ".staging-41" / "frames");
    writeTextFileForTest(output / ".staging-41" / "frames" / "junk.png", "not a png");
    writeTextFileForTest(output / ".staging-41" / "manifest.json", "{}");
    std::filesystem::create_directories(output / "sample_under_test" / "frames");
    writeTextFileForTest(output / "sample_under_test" / "frames" / "junk.png", "an older member");
    writeTextFileForTest(output / "sample_under_test" / "manifest.json", "an older manifest");

    const std::atomic_bool cancel{false};
    SampleExportWriter writer;
    const application::SampleExportReport report =
        writer.perform(makeJob(fixture("h264_a_320x180_30fps_12.mp4"),
                               fixture("h264_b_160x90_30fps_12.mp4"),
                               output,
                               plan),
                       cancel);

    ASSERT_EQ(report.outcome, application::SampleExportOutcome::kCompleted)
        << report.technicalDetail;
    EXPECT_EQ(sortedEntryNames(output), std::vector<std::string>({"sample_under_test"}));
    const std::filesystem::path finalDirectory = output / "sample_under_test";
    EXPECT_EQ(sortedEntryNames(finalDirectory),
              std::vector<std::string>({"frames", "manifest.json"}));
    EXPECT_EQ(sortedEntryNames(finalDirectory / "frames"),
              std::vector<std::string>({"gt.png", "input0.png", "input1.png", "prediction.png"}));
    EXPECT_EQ(readJsonFile(finalDirectory / "manifest.json")["sample_id"], "sample_under_test");
}

TEST(SampleExportWriterTests, FallsBackToWindowsWicWhenTheAvCodecPngEncoderIsUnavailable) {
    const std::filesystem::path gt = fixture("h264_a_320x180_30fps_12.mp4");
    const ScopedTempDirectory workspace{"wic"};
    const std::filesystem::path output = workspace.path() / "out";
    const application::SampleTripletPlan plan = planForOutputFrame(12, 1);

    const testing::ScopedSampleExportWicFallback forcedWic;
    const std::atomic_bool cancel{false};
    SampleExportWriter writer;
    const application::SampleExportReport report =
        writer.perform(makeJob(gt, fixture("h264_b_160x90_30fps_12.mp4"), output, plan), cancel);

    ASSERT_EQ(report.outcome, application::SampleExportOutcome::kCompleted)
        << report.technicalDetail;
    const std::filesystem::path finalDirectory = output / "sample_under_test";
    EXPECT_EQ(readJsonFile(finalDirectory / "manifest.json")["png_encoder"], "windows-wic");
    // The WIC route is lossless too: the member still decodes to the reference pixels.
    EXPECT_EQ(pngIhdrSize(finalDirectory / "frames" / "gt.png"),
              (std::pair<std::uint32_t, std::uint32_t>{320U, 180U}));
    EXPECT_EQ(memberRgbTriples(finalDirectory / "frames" / "gt.png"), referenceRgb24(gt, 1));
}

TEST(SampleExportWriterTests, RejectsSampleIdsThatAreNotSafeDirectoryNames) {
    const ScopedTempDirectory workspace{"ids"};
    const std::filesystem::path output = workspace.path() / "out";
    const application::SampleTripletPlan plan = planForOutputFrame(12, 1);
    const std::atomic_bool cancel{false};
    SampleExportWriter writer;

    for (const std::string_view sampleId : {"", ".", "..", ".hidden", "a/b", "a\\b", "bad name"}) {
        SCOPED_TRACE(sampleId);
        const application::SampleExportReport report =
            writer.perform(makeJob(fixture("h264_a_320x180_30fps_12.mp4"),
                                   fixture("h264_b_160x90_30fps_12.mp4"),
                                   output,
                                   plan,
                                   sampleId),
                           cancel);
        EXPECT_EQ(report.outcome, application::SampleExportOutcome::kFailed);
        EXPECT_FALSE(report.technicalDetail.empty());
    }
    // Nothing may have been created before the identifiers were accepted.
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(SampleExportWriterTests, RemovesStagingWhenReadBackValidationFails) {
    const ScopedTempDirectory workspace{"validate"};
    const std::filesystem::path output = workspace.path() / "out";
    const application::SampleTripletPlan plan = planForOutputFrame(12, 1);

    testing::ScopedSampleExportStagedMemberHook hook{&sabotageGtMemberAfterStaging};
    const std::atomic_bool cancel{false};
    SampleExportWriter writer;
    const application::SampleExportReport report =
        writer.perform(makeJob(fixture("h264_a_320x180_30fps_12.mp4"),
                               fixture("h264_b_160x90_30fps_12.mp4"),
                               output,
                               plan),
                       cancel);

    EXPECT_EQ(report.outcome, application::SampleExportOutcome::kFailed);
    EXPECT_NE(report.technicalDetail.find("read-back"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(output / "sample_under_test"));
    ASSERT_TRUE(std::filesystem::exists(output));
    EXPECT_TRUE(sortedEntryNames(output).empty());
}

} // namespace
} // namespace dvs::media
