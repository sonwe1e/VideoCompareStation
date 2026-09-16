#include "SignatureDecodeSession.h"

#include "dvs/platform/SourceIdentityService.h"
#include "dvs/platform/WindowsPaths.h"

#include "AvRaii.h"
#include "FrameTimelineIndex.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dvs::media::internal {
namespace {

struct InterruptState final {
    const std::atomic<bool>* requested = nullptr;
    const std::atomic<bool>* externalRequested = nullptr;
};

struct TimelineIndexCancellationState final {
    const std::atomic<bool>* request = nullptr;
    const std::atomic<bool>* interrupted = nullptr;
};

[[nodiscard]] int interruptCallback(void* const opaque) noexcept {
    const auto* const state = static_cast<const InterruptState*>(opaque);
    if (state == nullptr) {
        return 0;
    }
    const auto requested = [](const std::atomic<bool>* const flag) {
        return flag != nullptr && flag->load(std::memory_order_acquire);
    };
    return requested(state->requested) || requested(state->externalRequested) ? 1 : 0;
}

[[nodiscard]] bool timelineIndexCancellationRequested(const void* const opaque) noexcept {
    const auto* const state = static_cast<const TimelineIndexCancellationState*>(opaque);
    if (state == nullptr) {
        return false;
    }
    const auto requested = [](const std::atomic<bool>* const flag) {
        return flag != nullptr && flag->load(std::memory_order_acquire);
    };
    return requested(state->request) || requested(state->interrupted);
}

[[nodiscard]] domain::MediaError decodeError(const domain::MediaErrorCode code,
                                             const domain::SourceId sourceId,
                                             std::string detail,
                                             const bool recoverable = false) {
    return domain::makeMediaError(
        code, domain::MediaOperation::kMediaDecode, sourceId, recoverable, std::move(detail));
}

[[nodiscard]] std::string ffmpegError(const int errorCode) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(errorCode, buffer, sizeof(buffer)) < 0) {
        return "FFmpeg returned error " + std::to_string(errorCode) + ".";
    }
    return std::string{buffer};
}

[[nodiscard]] std::optional<application::FrameLumaSignature>
signatureFromFrame(const domain::FrameId frameId,
                   const AVFrame& frame,
                   SwsContextPtr& scaleContext,
                   const domain::MediaTime displayTime) noexcept {
    if (frame.width <= 0 || frame.height <= 0) {
        return std::nullopt;
    }
    const auto sourceFormat = static_cast<AVPixelFormat>(frame.format);
    if (sourceFormat == AV_PIX_FMT_NONE || sws_isSupportedInput(sourceFormat) == 0) {
        return std::nullopt;
    }

    std::array<std::uint8_t, application::kAlignmentDetailPixels> values{};
    SwsContext* const scaled =
        sws_getCachedContext(scaleContext.release(),
                             frame.width,
                             frame.height,
                             sourceFormat,
                             static_cast<int>(application::kAlignmentDetailWidth),
                             static_cast<int>(application::kAlignmentDetailHeight),
                             AV_PIX_FMT_GRAY8,
                             SWS_AREA,
                             nullptr,
                             nullptr,
                             nullptr);
    scaleContext.reset(scaled);
    if (scaleContext == nullptr) {
        return std::nullopt;
    }
    std::array<std::uint8_t*, 4U> destinationData{values.data(), nullptr, nullptr, nullptr};
    std::array<int, 4U> destinationLines{
        static_cast<int>(application::kAlignmentDetailWidth), 0, 0, 0};
    const int rows = sws_scale(scaleContext.get(),
                               frame.data,
                               frame.linesize,
                               0,
                               frame.height,
                               destinationData.data(),
                               destinationLines.data());
    if (rows != static_cast<int>(application::kAlignmentDetailHeight)) {
        return std::nullopt;
    }

    return application::makeMultiScaleFrameLumaSignature(frameId, values, displayTime);
}

} // namespace

class SignatureDecodeSession::Impl final {
public:
    Impl(const domain::SourceId sourceIdValue, domain::MediaDescriptor descriptorValue)
        : sourceId(sourceIdValue), descriptor(std::move(descriptorValue)),
          interruptState{.requested = &interrupted} {}

    domain::SourceId sourceId;
    domain::MediaDescriptor descriptor;
    std::atomic<bool> interrupted = false;
    InterruptState interruptState;
    AvFormatContextPtr format;
    AvCodecContextPtr codec;
    AvPacketPtr packet;
    AvFramePtr frame;
    SwsContextPtr scaleContext;
    int streamIndex = -1;
    AVRational timeBase{};
    std::shared_ptr<const std::vector<std::int64_t>> presentationTimestamps;
    std::optional<domain::FrameId> lastReturnedFrame;
    bool packetPending = false;
    bool inputEnded = false;
    bool flushSubmitted = false;
    bool sequentialReady = false;
    bool opened = false;
};

SignatureDecodeSession::SignatureDecodeSession(const domain::SourceId sourceId,
                                               domain::MediaDescriptor descriptor)
    : impl_(std::make_unique<Impl>(sourceId, std::move(descriptor))) {}

SignatureDecodeSession::~SignatureDecodeSession() {
    close();
}

domain::Status SignatureDecodeSession::open(const std::atomic<bool>& cancellationRequested) {
    close();
    impl_->interrupted.store(cancellationRequested.load(std::memory_order_acquire),
                             std::memory_order_release);
    if (cancellationRequested.load(std::memory_order_acquire)) {
        return domain::Status::failure(decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                                                   impl_->sourceId,
                                                   "Signature decoder open was canceled.",
                                                   true));
    }
    if (!impl_->descriptor.sourceIdentity.has_value() ||
        !impl_->descriptor.sourceIdentity->isComplete()) {
        return domain::Status::failure(
            decodeError(domain::MediaErrorCode::kInvalidMediaDescriptor,
                        impl_->sourceId,
                        "A signature decoder requires a complete probed source identity."));
    }

    const auto identity =
        platform::SourceIdentityService::verify(impl_->descriptor.normalizedPath,
                                                *impl_->descriptor.sourceIdentity,
                                                impl_->sourceId,
                                                domain::MediaOperation::kMediaDecode);
    if (!identity) {
        return identity;
    }
    const auto normalizedPath =
        platform::WindowsPaths::absolutePath(impl_->descriptor.normalizedPath);
    if (!normalizedPath) {
        return domain::Status::failure(decodeError(domain::MediaErrorCode::kMediaOpenFailed,
                                                   impl_->sourceId,
                                                   "Could not normalize the source path: " +
                                                       normalizedPath.error().technicalDetail));
    }
    const std::u8string pathUtf8 = normalizedPath.value().u8string();
    const std::string inputUrl{reinterpret_cast<const char*>(pathUtf8.data()), pathUtf8.size()};

    AVFormatContext* rawFormat = avformat_alloc_context();
    if (rawFormat == nullptr) {
        return domain::Status::failure(
            decodeError(domain::MediaErrorCode::kMediaOpenFailed,
                        impl_->sourceId,
                        "FFmpeg could not allocate a signature format context."));
    }
    rawFormat->interrupt_callback.callback = interruptCallback;
    rawFormat->interrupt_callback.opaque = &impl_->interruptState;
    const int openResult = avformat_open_input(&rawFormat, inputUrl.c_str(), nullptr, nullptr);
    if (openResult < 0) {
        if (rawFormat != nullptr) {
            avformat_close_input(&rawFormat);
        }
        return domain::Status::failure(
            decodeError(domain::MediaErrorCode::kMediaOpenFailed,
                        impl_->sourceId,
                        "FFmpeg could not open the signature source: " + ffmpegError(openResult),
                        true));
    }
    AvFormatContextPtr openedFormat{rawFormat};

    const int streamInfoResult = avformat_find_stream_info(openedFormat.get(), nullptr);
    if (streamInfoResult < 0) {
        return domain::Status::failure(decodeError(
            domain::MediaErrorCode::kMediaDecodeFailed,
            impl_->sourceId,
            "FFmpeg could not read signature stream information: " + ffmpegError(streamInfoResult),
            true));
    }
    const int selectedStream =
        av_find_best_stream(openedFormat.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (selectedStream < 0 ||
        static_cast<unsigned int>(selectedStream) >= openedFormat->nb_streams) {
        return domain::Status::failure(
            decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                        impl_->sourceId,
                        "The signature source has no readable video stream.",
                        true));
    }
    const AVStream* const stream = openedFormat->streams[selectedStream];
    if (stream == nullptr || stream->codecpar == nullptr || stream->codecpar->width <= 0 ||
        stream->codecpar->height <= 0) {
        return domain::Status::failure(
            decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                        impl_->sourceId,
                        "The selected signature video stream is incomplete."));
    }
    if (static_cast<std::uint32_t>(stream->codecpar->width) != impl_->descriptor.extent.width ||
        static_cast<std::uint32_t>(stream->codecpar->height) != impl_->descriptor.extent.height) {
        return domain::Status::failure(
            decodeError(domain::MediaErrorCode::kSourceFingerprintMismatch,
                        impl_->sourceId,
                        "Signature source geometry changed after media probing.",
                        true));
    }

    const AVCodec* const decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        return domain::Status::failure(
            decodeError(domain::MediaErrorCode::kUnsupportedCodec,
                        impl_->sourceId,
                        "FFmpeg has no signature decoder for the source codec.",
                        true));
    }
    AvCodecContextPtr openedCodec{avcodec_alloc_context3(decoder)};
    if (openedCodec == nullptr) {
        return domain::Status::failure(
            decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                        impl_->sourceId,
                        "FFmpeg could not allocate a signature codec context."));
    }
    const int parameterResult = avcodec_parameters_to_context(openedCodec.get(), stream->codecpar);
    if (parameterResult < 0) {
        return domain::Status::failure(
            decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                        impl_->sourceId,
                        "FFmpeg could not transfer signature codec parameters: " +
                            ffmpegError(parameterResult)));
    }
    const int codecOpenResult = avcodec_open2(openedCodec.get(), decoder, nullptr);
    if (codecOpenResult < 0) {
        return domain::Status::failure(decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                                                   impl_->sourceId,
                                                   "FFmpeg could not open the signature decoder: " +
                                                       ffmpegError(codecOpenResult),
                                                   true));
    }

    impl_->format = std::move(openedFormat);
    impl_->codec = std::move(openedCodec);
    impl_->streamIndex = selectedStream;
    impl_->timeBase = stream->time_base;
    impl_->opened = true;

    TimelineIndexCancellationState indexCancellation{
        .request = &cancellationRequested,
        .interrupted = &impl_->interrupted,
    };
    auto timestamps = buildPresentationTimestampIndex(TimestampIndexRequest{
        .sourcePath = impl_->descriptor.normalizedPath,
        .sourceId = impl_->sourceId,
        .operation = domain::MediaOperation::kMediaDecode,
        .expectedFrameCount = impl_->descriptor.frameCount.value,
        .sourceIdentity = impl_->descriptor.sourceIdentity,
        .streamIndex = selectedStream,
        .timeBase =
            TimelineRational{
                .numerator = stream->time_base.num,
                .denominator = stream->time_base.den,
            },
        .cancellation =
            TimelineCancellation{
                .isRequested = timelineIndexCancellationRequested,
                .context = &indexCancellation,
            },
    });
    if (!timestamps) {
        close();
        return domain::Status::failure(timestamps.error());
    }
    impl_->presentationTimestamps = std::move(timestamps).value();
    impl_->interrupted.store(false, std::memory_order_release);
    return domain::Status::success();
}

domain::Result<application::FrameLumaSignature>
SignatureDecodeSession::decodeSignature(const domain::FrameId frameId,
                                        const std::atomic<bool>& cancellationRequested) {
    const bool sequential = impl_->sequentialReady && impl_->lastReturnedFrame.has_value() &&
                            frameId.isValid() &&
                            frameId.value() == impl_->lastReturnedFrame->value() + 1;
    auto result = decodeInternal(frameId, cancellationRequested, sequential, true);
    if (!result) {
        impl_->sequentialReady = false;
    }
    return result;
}

domain::Result<std::vector<application::FrameLumaSignature>>
SignatureDecodeSession::decodeRange(const domain::FrameId firstFrame,
                                    const std::int64_t frameCount,
                                    const std::atomic<bool>& cancellationRequested,
                                    Progress progress) {
    if (!firstFrame.isValid() || frameCount <= 0 ||
        firstFrame.value() > impl_->descriptor.frameCount.value - frameCount) {
        return domain::Result<std::vector<application::FrameLumaSignature>>::failure(
            decodeError(domain::MediaErrorCode::kInvalidFrameId,
                        impl_->sourceId,
                        "Requested signature range is outside the source timeline."));
    }
    std::vector<application::FrameLumaSignature> signatures;
    signatures.reserve(static_cast<std::size_t>(frameCount));
    for (std::int64_t offset = 0; offset < frameCount; ++offset) {
        auto signature =
            decodeSignature(domain::FrameId{firstFrame.value() + offset}, cancellationRequested);
        if (!signature) {
            return domain::Result<std::vector<application::FrameLumaSignature>>::failure(
                signature.error());
        }
        signatures.push_back(std::move(signature).value());
        if (progress) {
            progress(1U);
        }
    }
    return domain::Result<std::vector<application::FrameLumaSignature>>::success(
        std::move(signatures));
}

domain::Result<application::FrameLumaSignature>
SignatureDecodeSession::decodeInternal(const domain::FrameId frameId,
                                       const std::atomic<bool>& cancellationRequested,
                                       const bool continueSequentially,
                                       const bool allowTimelineRecovery) {
    impl_->interrupted.store(cancellationRequested.load(std::memory_order_acquire),
                             std::memory_order_release);
    if (cancellationRequested.load(std::memory_order_acquire)) {
        return domain::Result<application::FrameLumaSignature>::failure(
            decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                        impl_->sourceId,
                        "Signature decoding was canceled.",
                        true));
    }
    if (!impl_->opened || impl_->format == nullptr || impl_->codec == nullptr) {
        return domain::Result<application::FrameLumaSignature>::failure(
            decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                        impl_->sourceId,
                        "Signature decoder is not open."));
    }
    if (!frameId.isValid() || frameId.value() >= impl_->descriptor.frameCount.value) {
        return domain::Result<application::FrameLumaSignature>::failure(
            decodeError(domain::MediaErrorCode::kInvalidFrameId,
                        impl_->sourceId,
                        "Requested signature frame is outside the source timeline."));
    }
    if (!impl_->presentationTimestamps) {
        return domain::Result<application::FrameLumaSignature>::failure(
            decodeError(domain::MediaErrorCode::kFrameTimelineInvalid,
                        impl_->sourceId,
                        "Signature decoder has no presentation timestamp index.",
                        true));
    }
    const std::int64_t targetTimestamp =
        (*impl_->presentationTimestamps)[static_cast<std::size_t>(frameId.value())];
    if (!continueSequentially) {
        const int seekResult = av_seek_frame(
            impl_->format.get(), impl_->streamIndex, targetTimestamp, AVSEEK_FLAG_BACKWARD);
        if (seekResult < 0) {
            return domain::Result<application::FrameLumaSignature>::failure(decodeError(
                domain::MediaErrorCode::kMediaDecodeFailed,
                impl_->sourceId,
                "FFmpeg could not seek for signature extraction: " + ffmpegError(seekResult),
                true));
        }
        avcodec_flush_buffers(impl_->codec.get());
        if (impl_->packet != nullptr) {
            av_packet_unref(impl_->packet.get());
        }
        if (impl_->frame != nullptr) {
            av_frame_unref(impl_->frame.get());
        }
        impl_->packetPending = false;
        impl_->inputEnded = false;
        impl_->flushSubmitted = false;
        impl_->lastReturnedFrame.reset();
        impl_->sequentialReady = false;
    }
    if (impl_->packet == nullptr) {
        impl_->packet.reset(av_packet_alloc());
    }
    if (impl_->frame == nullptr) {
        impl_->frame.reset(av_frame_alloc());
    }
    if (impl_->packet == nullptr || impl_->frame == nullptr) {
        return domain::Result<application::FrameLumaSignature>::failure(
            decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                        impl_->sourceId,
                        "FFmpeg could not allocate signature decode buffers."));
    }

    for (;;) {
        if (cancellationRequested.load(std::memory_order_acquire) ||
            impl_->interrupted.load(std::memory_order_acquire)) {
            return domain::Result<application::FrameLumaSignature>::failure(
                decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                            impl_->sourceId,
                            "Signature decoding was interrupted.",
                            true));
        }
        const int receiveResult = avcodec_receive_frame(impl_->codec.get(), impl_->frame.get());
        if (receiveResult == 0) {
            const std::int64_t timestamp = impl_->frame->best_effort_timestamp != AV_NOPTS_VALUE
                                               ? impl_->frame->best_effort_timestamp
                                               : impl_->frame->pts;
            if (timestamp == AV_NOPTS_VALUE) {
                return domain::Result<application::FrameLumaSignature>::failure(
                    decodeError(domain::MediaErrorCode::kFrameTimelineInvalid,
                                impl_->sourceId,
                                "A signature frame has no presentation timestamp.",
                                true));
            }
            if (timestamp < targetTimestamp) {
                av_frame_unref(impl_->frame.get());
                continue;
            }
            if (timestamp > targetTimestamp) {
                if (!continueSequentially && allowTimelineRecovery) {
                    const domain::Status reopened = open(cancellationRequested);
                    if (!reopened) {
                        return domain::Result<application::FrameLumaSignature>::failure(
                            reopened.error());
                    }
                    return decodeInternal(frameId, cancellationRequested, false, false);
                }
                return domain::Result<application::FrameLumaSignature>::failure(
                    decodeError(domain::MediaErrorCode::kFrameTimelineInvalid,
                                impl_->sourceId,
                                "The indexed timestamp did not identify signature frame " +
                                    std::to_string(frameId.value()) + ".",
                                true));
            }
            const auto pixelFormat = static_cast<AVPixelFormat>(impl_->frame->format);
            if (pixelFormat == AV_PIX_FMT_NONE || sws_isSupportedInput(pixelFormat) == 0) {
                return domain::Result<application::FrameLumaSignature>::failure(
                    decodeError(domain::MediaErrorCode::kUnsupportedPixelFormat,
                                impl_->sourceId,
                                "The signature decoder produced a format that cannot be "
                                "normalized to 8-bit luma.",
                                true));
            }
            if (impl_->frame->width <= 0 || impl_->frame->height <= 0 ||
                static_cast<std::uint32_t>(impl_->frame->width) != impl_->descriptor.extent.width ||
                static_cast<std::uint32_t>(impl_->frame->height) !=
                    impl_->descriptor.extent.height) {
                return domain::Result<application::FrameLumaSignature>::failure(
                    decodeError(domain::MediaErrorCode::kSourceFingerprintMismatch,
                                impl_->sourceId,
                                "Decoded signature geometry changed after media probing.",
                                true));
            }
            const std::int64_t firstTimestamp = impl_->presentationTimestamps->front();
            if (firstTimestamp < 0 &&
                targetTimestamp > (std::numeric_limits<std::int64_t>::max)() + firstTimestamp) {
                av_frame_unref(impl_->frame.get());
                return domain::Result<application::FrameLumaSignature>::failure(
                    decodeError(domain::MediaErrorCode::kFrameTimelineInvalid,
                                impl_->sourceId,
                                "Signature display time exceeds the supported range.",
                                true));
            }
            const std::int64_t normalizedTimestamp = targetTimestamp - firstTimestamp;
            const std::int64_t displayMicroseconds =
                av_rescale_q(normalizedTimestamp, impl_->timeBase, AV_TIME_BASE_Q);
            const auto signature = signatureFromFrame(frameId,
                                                      *impl_->frame,
                                                      impl_->scaleContext,
                                                      domain::MediaTime{displayMicroseconds});
            av_frame_unref(impl_->frame.get());
            if (!signature.has_value()) {
                return domain::Result<application::FrameLumaSignature>::failure(
                    decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                                impl_->sourceId,
                                "Decoder-owned luma could not form an alignment signature."));
            }
            impl_->lastReturnedFrame = frameId;
            impl_->sequentialReady = true;
            return domain::Result<application::FrameLumaSignature>::success(*signature);
        }
        if (receiveResult != AVERROR(EAGAIN) && receiveResult != AVERROR_EOF) {
            return domain::Result<application::FrameLumaSignature>::failure(decodeError(
                domain::MediaErrorCode::kMediaDecodeFailed,
                impl_->sourceId,
                "FFmpeg could not receive a signature frame: " + ffmpegError(receiveResult),
                true));
        }
        if (receiveResult == AVERROR_EOF) {
            break;
        }
        if (!impl_->packetPending && !impl_->inputEnded) {
            for (;;) {
                const int readResult = av_read_frame(impl_->format.get(), impl_->packet.get());
                if (readResult == AVERROR_EOF) {
                    impl_->inputEnded = true;
                    break;
                }
                if (readResult < 0) {
                    return domain::Result<application::FrameLumaSignature>::failure(decodeError(
                        domain::MediaErrorCode::kMediaDecodeFailed,
                        impl_->sourceId,
                        "FFmpeg could not read a signature packet: " + ffmpegError(readResult),
                        true));
                }
                if (impl_->packet->stream_index == impl_->streamIndex) {
                    impl_->packetPending = true;
                    break;
                }
                av_packet_unref(impl_->packet.get());
            }
        }
        if (impl_->packetPending) {
            const int sendResult = avcodec_send_packet(impl_->codec.get(), impl_->packet.get());
            if (sendResult == 0) {
                av_packet_unref(impl_->packet.get());
                impl_->packetPending = false;
                continue;
            }
            if (sendResult == AVERROR(EAGAIN)) {
                continue;
            }
            return domain::Result<application::FrameLumaSignature>::failure(decodeError(
                domain::MediaErrorCode::kMediaDecodeFailed,
                impl_->sourceId,
                "FFmpeg could not submit a signature packet: " + ffmpegError(sendResult),
                true));
        }
        if (impl_->inputEnded && !impl_->flushSubmitted) {
            const int flushResult = avcodec_send_packet(impl_->codec.get(), nullptr);
            if (flushResult == 0 || flushResult == AVERROR_EOF) {
                impl_->flushSubmitted = true;
                continue;
            }
            if (flushResult == AVERROR(EAGAIN)) {
                continue;
            }
            return domain::Result<application::FrameLumaSignature>::failure(decodeError(
                domain::MediaErrorCode::kMediaDecodeFailed,
                impl_->sourceId,
                "FFmpeg could not flush the signature decoder: " + ffmpegError(flushResult),
                true));
        }
        if (impl_->inputEnded && impl_->flushSubmitted) {
            return domain::Result<application::FrameLumaSignature>::failure(
                decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                            impl_->sourceId,
                            "Signature decoder requested input after flush.",
                            true));
        }
    }
    return domain::Result<application::FrameLumaSignature>::failure(
        decodeError(domain::MediaErrorCode::kFrameTimelineInvalid,
                    impl_->sourceId,
                    "Signature decoder reached end of stream before the indexed frame.",
                    true));
}

void SignatureDecodeSession::requestInterrupt() noexcept {
    impl_->interrupted.store(true, std::memory_order_release);
    impl_->sequentialReady = false;
}

void SignatureDecodeSession::close() noexcept {
    impl_->interrupted.store(true, std::memory_order_release);
    impl_->frame.reset();
    impl_->scaleContext.reset();
    impl_->packet.reset();
    impl_->codec.reset();
    impl_->format.reset();
    impl_->streamIndex = -1;
    impl_->timeBase = AVRational{};
    impl_->presentationTimestamps.reset();
    impl_->lastReturnedFrame.reset();
    impl_->packetPending = false;
    impl_->inputEnded = false;
    impl_->flushSubmitted = false;
    impl_->sequentialReady = false;
    impl_->opened = false;
}

} // namespace dvs::media::internal
