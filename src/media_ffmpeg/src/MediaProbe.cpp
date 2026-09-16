#include "dvs/media/MediaProbe.h"

#include "dvs/platform/SourceIdentityService.h"
#include "dvs/platform/WindowsPaths.h"

#include "AvRaii.h"
#include "FrameTimelineIndex.h"
#include "MediaProbeTestHooks.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dvs::media {
namespace {

constexpr std::size_t kMediaProbeWorkerCount = 2U;

enum class ProbeLifecycle {
    kPending,
    kCanceled,
    kTerminalClaimed,
};

struct ProbeOperation final {
    application::MediaProbeRequest request;
    std::weak_ptr<application::IApplicationEventSink> events;
    std::atomic<ProbeLifecycle> lifecycle = ProbeLifecycle::kPending;

    ProbeOperation(application::MediaProbeRequest requestValue,
                   std::weak_ptr<application::IApplicationEventSink> eventsValue)
        : request(std::move(requestValue)), events(std::move(eventsValue)) {}

    void requestCancellation() noexcept {
        ProbeLifecycle expected = ProbeLifecycle::kPending;
        static_cast<void>(lifecycle.compare_exchange_strong(expected,
                                                            ProbeLifecycle::kCanceled,
                                                            std::memory_order_acq_rel,
                                                            std::memory_order_acquire));
    }

    [[nodiscard]] bool isCanceled() const noexcept {
        return lifecycle.load(std::memory_order_acquire) == ProbeLifecycle::kCanceled;
    }

    [[nodiscard]] bool claimTerminal() noexcept {
        ProbeLifecycle expected = ProbeLifecycle::kPending;
        return lifecycle.compare_exchange_strong(expected,
                                                 ProbeLifecycle::kTerminalClaimed,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
    }

    [[nodiscard]] bool claimCanceledTerminal() noexcept {
        ProbeLifecycle expected = ProbeLifecycle::kCanceled;
        return lifecycle.compare_exchange_strong(expected,
                                                 ProbeLifecycle::kTerminalClaimed,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
    }
};

using WorkerAdmissionHook = testing::WorkerAdmissionHook;

struct WorkerAdmissionHookState final {
    std::mutex mutex;
    WorkerAdmissionHook hook = nullptr;
    void* context = nullptr;
};

[[nodiscard]] WorkerAdmissionHookState& workerAdmissionHookState() {
    static WorkerAdmissionHookState state;
    return state;
}

void invokeWorkerAdmissionHook() noexcept {
    WorkerAdmissionHook hook = nullptr;
    void* context = nullptr;
    {
        WorkerAdmissionHookState& state = workerAdmissionHookState();
        std::scoped_lock lock(state.mutex);
        hook = state.hook;
        context = state.context;
    }
    if (hook != nullptr) {
        hook(context);
    }
}

struct InterruptState final {
    const std::atomic<ProbeLifecycle>* lifecycle = nullptr;
};

[[nodiscard]] int interruptCallback(void* opaque) noexcept {
    const auto* const state = static_cast<const InterruptState*>(opaque);
    if (state == nullptr || state->lifecycle == nullptr) {
        return 0;
    }
    return state->lifecycle->load(std::memory_order_acquire) == ProbeLifecycle::kCanceled ? 1 : 0;
}

[[nodiscard]] bool probeCancellationRequested(const void* const context) noexcept {
    const auto* const lifecycle = static_cast<const std::atomic<ProbeLifecycle>*>(context);
    return lifecycle != nullptr &&
           lifecycle->load(std::memory_order_acquire) == ProbeLifecycle::kCanceled;
}

[[nodiscard]] domain::MediaError probeError(const domain::MediaErrorCode code,
                                            const std::optional<domain::SourceId> sourceId,
                                            std::string technicalDetail,
                                            const bool recoverable = false) {
    return domain::makeMediaError(code,
                                  domain::MediaOperation::kMediaProbe,
                                  sourceId,
                                  recoverable,
                                  std::move(technicalDetail));
}

[[nodiscard]] std::string ffmpegError(const int errorCode) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(errorCode, buffer, sizeof(buffer)) < 0) {
        return "FFmpeg returned error " + std::to_string(errorCode) + ".";
    }
    return std::string{buffer};
}

[[nodiscard]] bool sameIdentity(const domain::SourceFileIdentity& left,
                                const domain::SourceFileIdentity& right) noexcept {
    return left.byteSize == right.byteSize &&
           left.modifiedUtcMilliseconds == right.modifiedUtcMilliseconds &&
           left.fingerprintSha256 == right.fingerprintSha256;
}

[[nodiscard]] domain::Result<domain::ColorMetadata>
normalizeColorMetadata(const AVCodecParameters& parameters,
                       const std::uint32_t height,
                       const domain::SourceId sourceId) {
    if (parameters.color_trc == AVCOL_TRC_SMPTE2084 ||
        parameters.color_trc == AVCOL_TRC_ARIB_STD_B67) {
        return domain::Result<domain::ColorMetadata>::failure(
            probeError(domain::MediaErrorCode::kUnsupportedPixelFormat,
                       sourceId,
                       "HDR transfer characteristics are outside the v1 SDR media contract."));
    }

    domain::ColorMetadata color;
    switch (parameters.color_trc) {
    case AVCOL_TRC_BT709:
    case AVCOL_TRC_SMPTE170M:
    case AVCOL_TRC_BT2020_10:
    case AVCOL_TRC_BT2020_12:
        color.transfer = domain::ColorTransfer::kBt709;
        color.transferInferred = false;
        break;
    case AVCOL_TRC_IEC61966_2_1:
        color.transfer = domain::ColorTransfer::kSrgb;
        color.transferInferred = false;
        break;
    case AVCOL_TRC_LINEAR:
        color.transfer = domain::ColorTransfer::kLinear;
        color.transferInferred = false;
        break;
    case AVCOL_TRC_UNSPECIFIED:
        color.transfer = domain::ColorTransfer::kBt709;
        color.transferInferred = true;
        break;
    default:
        return domain::Result<domain::ColorMetadata>::failure(
            probeError(domain::MediaErrorCode::kUnsupportedPixelFormat,
                       sourceId,
                       "The source transfer characteristic cannot be normalized safely."));
    }
    switch (parameters.color_space) {
    case AVCOL_SPC_RGB:
        color.matrix = height >= 720U ? domain::ColorMatrix::kBt709 : domain::ColorMatrix::kBt601;
        color.matrixInferred = true;
        break;
    case AVCOL_SPC_BT709:
        color.matrix = domain::ColorMatrix::kBt709;
        color.matrixInferred = false;
        break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        color.matrix = domain::ColorMatrix::kBt601;
        color.matrixInferred = false;
        break;
    case AVCOL_SPC_UNSPECIFIED:
        color.matrix = height >= 720U ? domain::ColorMatrix::kBt709 : domain::ColorMatrix::kBt601;
        color.matrixInferred = true;
        break;
    default:
        return domain::Result<domain::ColorMetadata>::failure(
            probeError(domain::MediaErrorCode::kUnsupportedPixelFormat,
                       sourceId,
                       "Only BT.601 and BT.709 colour matrices are supported by the v1 renderer."));
    }

    const AVPixFmtDescriptor* const pixelDescriptor =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(parameters.format));
    const bool rgb =
        pixelDescriptor != nullptr && (pixelDescriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    color.range = !rgb && parameters.color_range == AVCOL_RANGE_JPEG ? domain::ColorRange::kFull
                                                                     : domain::ColorRange::kLimited;
    return domain::Result<domain::ColorMetadata>::success(color);
}

[[nodiscard]] std::optional<std::uint8_t>
supportedPixelBitDepth(const AVPixFmtDescriptor& descriptor) noexcept {
    const int bitDepth = descriptor.comp[0].depth;
    const bool rgb = (descriptor.flags & AV_PIX_FMT_FLAG_RGB) != 0;
    if ((rgb && (descriptor.nb_components < 3 || descriptor.nb_components > 4 || bitDepth != 8)) ||
        (!rgb && (descriptor.nb_components < 3 || descriptor.nb_components > 4 ||
                  descriptor.log2_chroma_w < 0 || descriptor.log2_chroma_w > 1 ||
                  descriptor.log2_chroma_h < 0 || descriptor.log2_chroma_h > 1 ||
                  (bitDepth != 8 && bitDepth != 10)))) {
        return std::nullopt;
    }
    for (int component = 1; component < descriptor.nb_components; ++component) {
        if (descriptor.comp[component].depth != bitDepth) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint8_t>(bitDepth);
}

[[nodiscard]] domain::Result<std::uint16_t> normalizedRotation(const AVCodecParameters& parameters,
                                                               const domain::SourceId sourceId) {
    const AVPacketSideData* const display = av_packet_side_data_get(
        parameters.coded_side_data, parameters.nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (display == nullptr) {
        return domain::Result<std::uint16_t>::success(0U);
    }
    if (display->size < sizeof(std::int32_t) * 9U) {
        return domain::Result<std::uint16_t>::failure(
            probeError(domain::MediaErrorCode::kInvalidMediaDescriptor,
                       sourceId,
                       "The source display matrix is truncated."));
    }
    std::array<std::int32_t, 9U> matrix{};
    std::memcpy(matrix.data(), display->data, sizeof(matrix));
    const double counterClockwise = av_display_rotation_get(matrix.data());
    if (!std::isfinite(counterClockwise)) {
        return domain::Result<std::uint16_t>::failure(
            probeError(domain::MediaErrorCode::kInvalidMediaDescriptor,
                       sourceId,
                       "The source display matrix has no finite rotation."));
    }
    int degrees = static_cast<int>(std::lround(counterClockwise));
    degrees %= 360;
    if (degrees < 0) {
        degrees += 360;
    }
    if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
        return domain::Result<std::uint16_t>::failure(
            probeError(domain::MediaErrorCode::kUnsupportedPixelFormat,
                       sourceId,
                       "Only right-angle source rotation can be normalized."));
    }
    return domain::Result<std::uint16_t>::success(static_cast<std::uint16_t>(degrees));
}

[[nodiscard]] domain::SampleAspectRatio
normalizedSampleAspectRatio(const AVStream& stream, const AVCodecParameters& parameters) noexcept {
    AVRational ratio = stream.sample_aspect_ratio;
    if (ratio.num <= 0 || ratio.den <= 0) {
        ratio = parameters.sample_aspect_ratio;
    }
    if (ratio.num <= 0 || ratio.den <= 0) {
        return {};
    }
    const std::uint64_t numerator = static_cast<std::uint64_t>(ratio.num);
    const std::uint64_t denominator = static_cast<std::uint64_t>(ratio.den);
    const std::uint64_t divisor = std::gcd(numerator, denominator);
    if ((numerator / divisor) > (std::numeric_limits<std::uint32_t>::max)() ||
        (denominator / divisor) > (std::numeric_limits<std::uint32_t>::max)()) {
        return {};
    }
    return domain::SampleAspectRatio{
        .numerator = static_cast<std::uint32_t>(numerator / divisor),
        .denominator = static_cast<std::uint32_t>(denominator / divisor),
    };
}

[[nodiscard]] bool
checkedSubtract(const std::int64_t a, const std::int64_t b, std::int64_t* const result) noexcept {
    if ((b > 0 && a < std::numeric_limits<std::int64_t>::min() + b) ||
        (b < 0 && a > std::numeric_limits<std::int64_t>::max() + b)) {
        return false;
    }
    *result = a - b;
    return true;
}

[[nodiscard]] domain::Result<domain::MediaDescriptor> inspectInternal(
    const std::filesystem::path& sourcePath,
    const domain::SourceId sourceId,
    const std::atomic<ProbeLifecycle>* const lifecycle,
    std::optional<std::shared_ptr<const domain::FrameTimeline>>* const timeline = nullptr) {
    if (timeline != nullptr) {
        *timeline = std::nullopt;
    }

    const auto normalizedPath = platform::WindowsPaths::absolutePath(sourcePath);
    if (!normalizedPath) {
        return domain::Result<domain::MediaDescriptor>::failure(probeError(
            domain::MediaErrorCode::kMediaProbeFailed,
            sourceId,
            "Could not normalize the source path: " + normalizedPath.error().technicalDetail));
    }

    auto identityBefore = platform::SourceIdentityService::fingerprint(
        normalizedPath.value(), sourceId, domain::MediaOperation::kMediaProbe);
    if (!identityBefore) {
        return domain::Result<domain::MediaDescriptor>::failure(identityBefore.error());
    }

    const std::u8string pathUtf8 = normalizedPath.value().u8string();
    const std::string inputUrl{reinterpret_cast<const char*>(pathUtf8.data()), pathUtf8.size()};
    InterruptState interruptState{.lifecycle = lifecycle};

    AVFormatContext* rawFormat = avformat_alloc_context();
    if (rawFormat == nullptr) {
        return domain::Result<domain::MediaDescriptor>::failure(
            probeError(domain::MediaErrorCode::kMediaOpenFailed,
                       sourceId,
                       "FFmpeg could not allocate a format context."));
    }
    rawFormat->interrupt_callback.callback = interruptCallback;
    rawFormat->interrupt_callback.opaque = &interruptState;

    const int openResult = avformat_open_input(&rawFormat, inputUrl.c_str(), nullptr, nullptr);
    if (openResult < 0) {
        if (rawFormat != nullptr) {
            avformat_close_input(&rawFormat);
        }
        return domain::Result<domain::MediaDescriptor>::failure(
            probeError(domain::MediaErrorCode::kMediaOpenFailed,
                       sourceId,
                       "FFmpeg could not open the source: " + ffmpegError(openResult),
                       true));
    }
    internal::AvFormatContextPtr format{rawFormat};

    const int streamInfoResult = avformat_find_stream_info(format.get(), nullptr);
    if (streamInfoResult < 0) {
        return domain::Result<domain::MediaDescriptor>::failure(
            probeError(domain::MediaErrorCode::kMediaProbeFailed,
                       sourceId,
                       "FFmpeg could not read stream information: " + ffmpegError(streamInfoResult),
                       true));
    }

    const int streamIndex =
        av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0 || static_cast<unsigned int>(streamIndex) >= format->nb_streams) {
        return domain::Result<domain::MediaDescriptor>::failure(
            probeError(domain::MediaErrorCode::kMediaProbeFailed,
                       sourceId,
                       "The source has no readable video stream.",
                       true));
    }

    AVStream* const stream = format->streams[streamIndex];
    if (stream == nullptr || stream->codecpar == nullptr || stream->codecpar->width <= 0 ||
        stream->codecpar->height <= 0) {
        return domain::Result<domain::MediaDescriptor>::failure(
            probeError(domain::MediaErrorCode::kMediaProbeFailed,
                       sourceId,
                       "The selected video stream has invalid dimensions."));
    }
    const AVCodecParameters& parameters = *stream->codecpar;

    if (parameters.codec_id != AV_CODEC_ID_H264 && parameters.codec_id != AV_CODEC_ID_HEVC &&
        parameters.codec_id != AV_CODEC_ID_MPEG4) {
        return domain::Result<domain::MediaDescriptor>::failure(
            probeError(domain::MediaErrorCode::kUnsupportedCodec,
                       sourceId,
                       "Only H.264, H.265/HEVC, and MPEG-4 Part 2 source video is supported in v1.",
                       true));
    }
    const AVCodec* const decoder = avcodec_find_decoder(parameters.codec_id);
    if (decoder == nullptr) {
        return domain::Result<domain::MediaDescriptor>::failure(
            probeError(domain::MediaErrorCode::kUnsupportedCodec,
                       sourceId,
                       "FFmpeg has no decoder for this supported source codec.",
                       true));
    }
    bool d3d11VaDecode = false;
    for (int configurationIndex = 0;; ++configurationIndex) {
        const AVCodecHWConfig* const configuration =
            avcodec_get_hw_config(decoder, configurationIndex);
        if (configuration == nullptr) {
            break;
        }
        if (configuration->device_type == AV_HWDEVICE_TYPE_D3D11VA &&
            (configuration->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
            d3d11VaDecode = true;
            break;
        }
    }

    const AVPixelFormat pixelFormat = static_cast<AVPixelFormat>(parameters.format);
    const AVPixFmtDescriptor* const pixelDescriptor = av_pix_fmt_desc_get(pixelFormat);
    const char* const pixelFormatName = av_get_pix_fmt_name(pixelFormat);
    const std::optional<std::uint8_t> bitDepth =
        pixelDescriptor == nullptr ? std::nullopt : supportedPixelBitDepth(*pixelDescriptor);
    if (pixelDescriptor == nullptr || pixelFormatName == nullptr || !bitDepth.has_value()) {
        return domain::Result<domain::MediaDescriptor>::failure(
            probeError(domain::MediaErrorCode::kUnsupportedPixelFormat,
                       sourceId,
                       "Only 8/10-bit YUV and 8-bit RGB source pixel formats are supported.",
                       true));
    }

    const AVRational averageRate = stream->avg_frame_rate;
    const AVRational realRate = stream->r_frame_rate;
    std::optional<domain::RationalRate> frameRate;
    std::shared_ptr<const std::vector<std::int64_t>> timestampIndex;
    domain::TimingConfidence timingConfidence = domain::TimingConfidence::kDeclaredCfr;
    {
        auto indexedTimestamps =
            internal::buildPresentationTimestampIndex(internal::TimestampIndexRequest{
                .sourcePath = normalizedPath.value(),
                .sourceId = sourceId,
                .operation = domain::MediaOperation::kMediaProbe,
                .expectedFrameCount = stream->nb_frames > 0
                                          ? std::optional<std::int64_t>{stream->nb_frames}
                                          : std::nullopt,
                .sourceIdentity = identityBefore.value(),
                .streamIndex = streamIndex,
                .timeBase =
                    internal::TimelineRational{
                        .numerator = stream->time_base.num,
                        .denominator = stream->time_base.den,
                    },
                .cancellation =
                    internal::TimelineCancellation{
                        .isRequested = probeCancellationRequested,
                        .context = lifecycle,
                    },
            });
        if (!indexedTimestamps) {
            return domain::Result<domain::MediaDescriptor>::failure(indexedTimestamps.error());
        }
        timestampIndex = std::move(indexedTimestamps).value();

        const AVRational guessedRate = av_guess_frame_rate(format.get(), stream, nullptr);
        const auto verified =
            internal::verifyConstantFrameRate(
                internal::CfrVerificationRequest{
                    .presentationTimestamps = *timestampIndex,
                    .timeBase =
                        internal::TimelineRational{
                            .numerator = stream->time_base.num,
                            .denominator = stream->time_base.den,
                        },
                    .candidates =
                        {
                            internal::FrameRateCandidate{
                                .rate =
                                    internal::TimelineRational{
                                        .numerator = guessedRate.num,
                                        .denominator = guessedRate.den,
                                    },
                                .guessed = true,
                            },
                            internal::FrameRateCandidate{
                                .rate =
                                    internal::TimelineRational{
                                        .numerator = averageRate.num,
                                        .denominator = averageRate.den,
                                    },
                            },
                            internal::FrameRateCandidate{
                                .rate =
                                    internal::TimelineRational{
                                        .numerator = realRate.num,
                                        .denominator = realRate.den,
                                    },
                            },
                        },
                    .sourceId = sourceId,
                    .operation = domain::MediaOperation::kMediaProbe,
                    .cancellation =
                        internal::TimelineCancellation{
                            .isRequested = probeCancellationRequested,
                            .context = lifecycle,
                        },
                });
        if (!verified) {
            if (verified.error().code == domain::MediaErrorCode::kInvalidCfrTiming) {
                // No candidate matches the display-order timestamps: this is a valid
                // variable-frame-rate classification, not a probe error.
                frameRate = std::nullopt;
                timingConfidence = domain::TimingConfidence::kVariableFrameRate;
            } else {
                return domain::Result<domain::MediaDescriptor>::failure(verified.error());
            }
        } else {
            auto verifiedRate = domain::RationalRate::create(
                verified.value().frameRate.numerator, verified.value().frameRate.denominator);
            if (!verifiedRate) {
                domain::MediaError error = verifiedRate.error();
                error.operation = domain::MediaOperation::kMediaProbe;
                error.source = sourceId;
                return domain::Result<domain::MediaDescriptor>::failure(std::move(error));
            }
            frameRate = std::move(verifiedRate).value();
            timingConfidence = domain::TimingConfidence::kVerifiedCfr;
        }
    }

    if (timingConfidence == domain::TimingConfidence::kVariableFrameRate) {
        // A variable-frame-rate classification still anchors and converts the
        // normalized timeline so the application can play variable-rate sources
        // without depending on a rational frame rate. The conversion runs even
        // when the caller cancels the timeline out; only the assignment is gated
        // on a non-null out pointer.
        if (stream->time_base.num <= 0 || stream->time_base.den <= 0) {
            return domain::Result<domain::MediaDescriptor>::failure(
                probeError(domain::MediaErrorCode::kFrameTimelineInvalid,
                           sourceId,
                           "The variable-frame-rate source has a non-positive stream time base."));
        }
        const AVRational timeBase = stream->time_base;
        const std::int64_t anchor = timestampIndex->front();
        std::vector<domain::MediaTime> displayTimes;
        displayTimes.reserve(timestampIndex->size());
        constexpr AVRational microsecondBase{.num = 1, .den = AV_TIME_BASE};
        for (const std::int64_t presentation : *timestampIndex) {
            if (probeCancellationRequested(lifecycle)) {
                return domain::Result<domain::MediaDescriptor>::failure(
                    probeError(domain::MediaErrorCode::kMediaProbeFailed,
                               sourceId,
                               "The variable-frame-rate presentation-time conversion was canceled.",
                               true));
            }
            std::int64_t offset = 0;
            if (!checkedSubtract(presentation, anchor, &offset)) {
                return domain::Result<domain::MediaDescriptor>::failure(
                    probeError(domain::MediaErrorCode::kArithmeticOverflow,
                               sourceId,
                               "The variable-frame-rate presentation-time offset overflowed the "
                               "int64 range."));
            }
            const std::int64_t microseconds =
                av_rescale_q_rnd(offset,
                                 timeBase,
                                 microsecondBase,
                                 static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            if (microseconds < 0 || microseconds == std::numeric_limits<std::int64_t>::max()) {
                return domain::Result<domain::MediaDescriptor>::failure(
                    probeError(domain::MediaErrorCode::kArithmeticOverflow,
                               sourceId,
                               "A variable-frame-rate presentation time overflowed during the "
                               "microsecond conversion."));
            }
            displayTimes.emplace_back(domain::MediaTime{microseconds});
        }
        auto variableTimeline = domain::FrameTimeline::create(std::move(displayTimes));
        if (!variableTimeline) {
            domain::MediaError error = variableTimeline.error();
            error.operation = domain::MediaOperation::kMediaProbe;
            error.source = sourceId;
            return domain::Result<domain::MediaDescriptor>::failure(std::move(error));
        }
        if (timeline != nullptr) {
            *timeline =
                std::make_shared<const domain::FrameTimeline>(std::move(variableTimeline).value());
        }
    }

    auto colorMetadata =
        normalizeColorMetadata(parameters, static_cast<std::uint32_t>(parameters.height), sourceId);
    if (!colorMetadata) {
        return domain::Result<domain::MediaDescriptor>::failure(colorMetadata.error());
    }
    auto rotation = normalizedRotation(parameters, sourceId);
    if (!rotation) {
        return domain::Result<domain::MediaDescriptor>::failure(rotation.error());
    }

    AVRational durationTimeBase = stream->time_base;
    std::int64_t durationTicks = stream->duration;
    bool hasDuration = durationTicks != AV_NOPTS_VALUE && durationTicks >= 0 &&
                       durationTimeBase.num > 0 && durationTimeBase.den > 0;
    if (!hasDuration && format->duration != AV_NOPTS_VALUE && format->duration >= 0) {
        durationTicks = format->duration;
        durationTimeBase = AV_TIME_BASE_Q;
        hasDuration = true;
    }

    std::int64_t durationMicroseconds = 0;
    if (hasDuration) {
        durationMicroseconds =
            av_rescale_q_rnd(durationTicks,
                             durationTimeBase,
                             AVRational{.num = 1, .den = AV_TIME_BASE},
                             static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        if (durationMicroseconds < 0) {
            return domain::Result<domain::MediaDescriptor>::failure(
                probeError(domain::MediaErrorCode::kInvalidDuration,
                           sourceId,
                           "The stream duration cannot be represented in microseconds."));
        }
    }

    const domain::FrameCountInfo frameCount{
        .value = static_cast<std::int64_t>(timestampIndex->size()),
        .origin = domain::FrameCountOrigin::kIndexed,
    };

    auto identityAfter = platform::SourceIdentityService::fingerprint(
        normalizedPath.value(), sourceId, domain::MediaOperation::kMediaProbe);
    if (!identityAfter) {
        return domain::Result<domain::MediaDescriptor>::failure(identityAfter.error());
    }
    if (!sameIdentity(identityBefore.value(), identityAfter.value())) {
        return domain::Result<domain::MediaDescriptor>::failure(
            probeError(domain::MediaErrorCode::kSourceFingerprintMismatch,
                       sourceId,
                       "The source changed while FFmpeg was probing it.",
                       true));
    }

    domain::MediaDescriptor descriptor{
        .normalizedPath = normalizedPath.value(),
        .extent =
            domain::MediaExtent{
                .width = static_cast<std::uint32_t>(parameters.width),
                .height = static_cast<std::uint32_t>(parameters.height),
            },
        .frameRate = frameRate,
        .frameCount = frameCount,
        .duration = domain::MediaTime{durationMicroseconds},
        .codecId = avcodec_get_name(parameters.codec_id),
        .pixelFormatId = pixelFormatName,
        .bitDepth = *bitDepth,
        .rotationDegrees = rotation.value(),
        .sampleAspectRatio = normalizedSampleAspectRatio(*stream, parameters),
        .colorMetadata = colorMetadata.value(),
        .decodeCapabilities =
            domain::DecodeCapabilities{
                .softwareDecode = true,
                .d3d11VaDecode = d3d11VaDecode,
            },
        .timingConfidence = timingConfidence,
        .sourceIdentity = std::move(identityAfter).value(),
    };
    auto validated = domain::validateMediaDescriptor(std::move(descriptor));
    if (!validated) {
        domain::MediaError error = validated.error();
        error.operation = domain::MediaOperation::kMediaProbe;
        error.source = sourceId;
        return domain::Result<domain::MediaDescriptor>::failure(std::move(error));
    }
    return validated;
}

void postCritical(const std::weak_ptr<application::IApplicationEventSink>& events,
                  application::ApplicationEvent event) noexcept {
    if (const std::shared_ptr<application::IApplicationEventSink> sink = events.lock()) {
        static_cast<void>(sink->postCritical(std::move(event)));
    }
}

void postCanceled(const std::shared_ptr<ProbeOperation>& operation) noexcept {
    if (!operation->claimCanceledTerminal()) {
        return;
    }
    application::EventContext context{operation->request.context};
    postCritical(operation->events,
                 application::ApplicationEvent{application::RequestCanceled{
                     .context = std::move(context),
                     .reason = application::CancellationReason::UserRequested,
                 }});
}

void postFailed(const std::shared_ptr<ProbeOperation>& operation,
                domain::MediaError error) noexcept {
    if (!operation->claimTerminal()) {
        postCanceled(operation);
        return;
    }
    error.requestId = operation->request.context.requestId;
    application::EventContext context{operation->request.context};
    postCritical(operation->events,
                 application::ApplicationEvent{application::RequestFailed{
                     .context = std::move(context),
                     .error = std::move(error),
                 }});
}

void postSucceeded(const std::shared_ptr<ProbeOperation>& operation,
                   domain::MediaDescriptor descriptor,
                   std::optional<std::shared_ptr<const domain::FrameTimeline>> timeline) noexcept {
    if (!operation->claimTerminal()) {
        postCanceled(operation);
        return;
    }
    postCritical(operation->events,
                 application::ApplicationEvent{application::ProbeCompleted{
                     .context = operation->request.context,
                     .sourceId = operation->request.sourceId,
                     .descriptor = std::move(descriptor),
                     .timeline = std::move(timeline),
                 }});
    application::EventContext context{operation->request.context};
    postCritical(operation->events,
                 application::ApplicationEvent{application::RequestSucceeded{
                     .context = std::move(context),
                 }});
}

} // namespace

namespace testing {

ScopedMediaProbeWorkerAdmissionHook::ScopedMediaProbeWorkerAdmissionHook(
    const WorkerAdmissionHook hook, void* const context) noexcept {
    WorkerAdmissionHookState& state = workerAdmissionHookState();
    std::scoped_lock lock(state.mutex);
    previousHook_ = state.hook;
    previousContext_ = state.context;
    state.hook = hook;
    state.context = context;
}

ScopedMediaProbeWorkerAdmissionHook::~ScopedMediaProbeWorkerAdmissionHook() {
    WorkerAdmissionHookState& state = workerAdmissionHookState();
    std::scoped_lock lock(state.mutex);
    state.hook = previousHook_;
    state.context = previousContext_;
}

} // namespace testing

class MediaProbe::Impl final {
public:
    explicit Impl(const std::size_t queueCapacity)
        : queueCapacity_(queueCapacity == 0U ? 1U : queueCapacity) {
        try {
            workers_.reserve(kMediaProbeWorkerCount);
            for (std::size_t workerIndex = 0; workerIndex < kMediaProbeWorkerCount; ++workerIndex) {
                workers_.emplace_back([this] { run(); });
            }
        } catch (...) {
            close();
            throw;
        }
    }

    ~Impl() {
        close();
    }

    [[nodiscard]] application::PortSubmitResult
    submit(const application::MediaProbeRequest& request,
           const std::shared_ptr<application::IApplicationEventSink>& events) {
        if (!events) {
            return application::PortSubmitResult::Closed;
        }

        auto operation = std::make_shared<ProbeOperation>(
            request, std::weak_ptr<application::IApplicationEventSink>{events});
        std::scoped_lock lock(mutex_);
        if (closed_) {
            return application::PortSubmitResult::Closed;
        }
        if (queue_.size() >= queueCapacity_) {
            return application::PortSubmitResult::Busy;
        }
        operations_.push_back(operation);
        queue_.push_back(std::move(operation));
        condition_.notify_one();
        return application::PortSubmitResult::Accepted;
    }

    void cancel(const application::RequestContext& context) noexcept {
        std::scoped_lock lock(mutex_);
        for (const std::shared_ptr<ProbeOperation>& operation : operations_) {
            if (operation->request.context == context) {
                operation->requestCancellation();
            }
        }
    }

    [[nodiscard]] MediaProbeStatistics statistics() const noexcept {
        return MediaProbeStatistics{
            .completedProbes = completedProbes_.load(std::memory_order_acquire),
            .totalProbeIndexMicroseconds =
                totalProbeIndexMicroseconds_.load(std::memory_order_acquire),
            .maximumProbeIndexMicroseconds =
                maximumProbeIndexMicroseconds_.load(std::memory_order_acquire),
        };
    }

private:
    void recordSuccessfulProbe(const std::uint64_t elapsedMicroseconds) noexcept {
        completedProbes_.fetch_add(1U, std::memory_order_release);
        totalProbeIndexMicroseconds_.fetch_add(elapsedMicroseconds, std::memory_order_release);
        std::uint64_t maximum = maximumProbeIndexMicroseconds_.load(std::memory_order_acquire);
        while (maximum < elapsedMicroseconds &&
               !maximumProbeIndexMicroseconds_.compare_exchange_weak(maximum,
                                                                     elapsedMicroseconds,
                                                                     std::memory_order_release,
                                                                     std::memory_order_acquire)) {
        }
    }

    void close() noexcept {
        {
            std::scoped_lock lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
            for (const std::shared_ptr<ProbeOperation>& operation : operations_) {
                operation->requestCancellation();
            }
        }
        condition_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void remove(const std::shared_ptr<ProbeOperation>& operation) noexcept {
        std::scoped_lock lock(mutex_);
        const auto iterator = std::remove(operations_.begin(), operations_.end(), operation);
        operations_.erase(iterator, operations_.end());
    }

    void execute(const std::shared_ptr<ProbeOperation>& operation) noexcept {
        const auto started = std::chrono::steady_clock::now();
        try {
            invokeWorkerAdmissionHook();
            if (operation->isCanceled()) {
                postCanceled(operation);
            } else {
                std::optional<std::shared_ptr<const domain::FrameTimeline>> timeline;
                const auto descriptor = inspectInternal(operation->request.sourcePath,
                                                        operation->request.sourceId,
                                                        &operation->lifecycle,
                                                        &timeline);
                if (operation->isCanceled()) {
                    postCanceled(operation);
                } else if (!descriptor) {
                    postFailed(operation, descriptor.error());
                } else {
                    const auto elapsed = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count());
                    recordSuccessfulProbe(elapsed);
                    postSucceeded(operation, descriptor.value(), std::move(timeline));
                }
            }
        } catch (const std::exception& exception) {
            postFailed(
                operation,
                probeError(domain::MediaErrorCode::kMediaProbeFailed,
                           operation->request.sourceId,
                           "Unexpected media probe exception: " + std::string{exception.what()}));
        } catch (...) {
            postFailed(operation,
                       probeError(domain::MediaErrorCode::kMediaProbeFailed,
                                  operation->request.sourceId,
                                  "Unexpected non-standard media probe exception."));
        }
        remove(operation);
    }

    void run() noexcept {
        for (;;) {
            std::shared_ptr<ProbeOperation> operation;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return closed_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (closed_) {
                        return;
                    }
                    continue;
                }
                operation = std::move(queue_.front());
                queue_.pop_front();
            }
            execute(operation);
        }
    }

    std::size_t queueCapacity_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::shared_ptr<ProbeOperation>> queue_;
    std::vector<std::shared_ptr<ProbeOperation>> operations_;
    bool closed_ = false;
    std::vector<std::thread> workers_;
    std::atomic<std::uint64_t> completedProbes_ = 0U;
    std::atomic<std::uint64_t> totalProbeIndexMicroseconds_ = 0U;
    std::atomic<std::uint64_t> maximumProbeIndexMicroseconds_ = 0U;
};

MediaProbe::MediaProbe(const std::size_t queueCapacity)
    : impl_(std::make_unique<Impl>(queueCapacity)) {}

MediaProbe::~MediaProbe() = default;

domain::Result<domain::MediaDescriptor> MediaProbe::inspect(const std::filesystem::path& sourcePath,
                                                            const domain::SourceId sourceId) {
    try {
        return inspectInternal(sourcePath, sourceId, nullptr);
    } catch (const std::exception& exception) {
        return domain::Result<domain::MediaDescriptor>::failure(
            probeError(domain::MediaErrorCode::kMediaProbeFailed,
                       sourceId,
                       "Unexpected media probe exception: " + std::string{exception.what()}));
    } catch (...) {
        return domain::Result<domain::MediaDescriptor>::failure(
            probeError(domain::MediaErrorCode::kMediaProbeFailed,
                       sourceId,
                       "Unexpected non-standard media probe exception."));
    }
}

application::PortSubmitResult
MediaProbe::submit(const application::MediaProbeRequest& request,
                   std::shared_ptr<application::IApplicationEventSink> events) {
    return impl_->submit(request, events);
}

void MediaProbe::cancel(const application::RequestContext& context) noexcept {
    impl_->cancel(context);
}

MediaProbeStatistics MediaProbe::statistics() const noexcept {
    return impl_->statistics();
}

} // namespace dvs::media
