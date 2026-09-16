#include "SoftwareDecoder.h"

#include "dvs/application/PlaybackTrace.h"
#include "dvs/platform/D3d11DecodedFrameResource.h"
#include "dvs/platform/FrameResourceFactory.h"
#include "dvs/platform/GraphicsDeviceBroker.h"
#include "dvs/platform/SourceIdentityService.h"
#include "dvs/platform/WindowsPaths.h"

#include "AvRaii.h"
#include "FrameTimelineIndex.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}

#include <array>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dvs::media::internal {
namespace {

constexpr std::int64_t kExactFullDecodeTailFrames = 16;

struct InterruptState final {
    const std::atomic<bool>* requested = nullptr;
    const std::atomic<bool>* externalRequested = nullptr;
};

struct TimelineIndexCancellationState final {
    const std::atomic<bool>* request = nullptr;
    const std::atomic<bool>* interrupted = nullptr;
    const std::atomic<bool>* external = nullptr;
};

[[nodiscard]] int interruptCallback(void* opaque) noexcept {
    const auto* const state = static_cast<const InterruptState*>(opaque);
    if (state == nullptr) {
        return 0;
    }
    const bool requested =
        state->requested != nullptr && state->requested->load(std::memory_order_acquire);
    const bool externallyRequested = state->externalRequested != nullptr &&
                                     state->externalRequested->load(std::memory_order_acquire);
    return requested || externallyRequested ? 1 : 0;
}

[[nodiscard]] bool timelineIndexCancellationRequested(const void* const opaque) noexcept {
    const auto* const state = static_cast<const TimelineIndexCancellationState*>(opaque);
    if (state == nullptr) {
        return false;
    }
    const auto requested = [](const std::atomic<bool>* const flag) {
        return flag != nullptr && flag->load(std::memory_order_acquire);
    };
    return requested(state->request) || requested(state->interrupted) || requested(state->external);
}

[[nodiscard]] domain::MediaError decodeError(const domain::MediaErrorCode code,
                                             const domain::SourceId sourceId,
                                             std::string technicalDetail,
                                             const bool recoverable = false) {
    return domain::makeMediaError(code,
                                  domain::MediaOperation::kMediaDecode,
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

[[nodiscard]] bool isSupportedDecodedFormat(const AVFrame& frame) noexcept {
    const auto pixelFormat = static_cast<AVPixelFormat>(frame.format);
    const AVPixFmtDescriptor* const descriptor = av_pix_fmt_desc_get(pixelFormat);
    if (descriptor == nullptr) {
        return false;
    }
    const int bitDepth = descriptor->comp[0].depth;
    const bool rgb = (descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    if ((rgb &&
         (descriptor->nb_components < 3 || descriptor->nb_components > 4 || bitDepth != 8)) ||
        (!rgb && (descriptor->nb_components < 3 || descriptor->nb_components > 4 ||
                  descriptor->log2_chroma_w < 0 || descriptor->log2_chroma_w > 1 ||
                  descriptor->log2_chroma_h < 0 || descriptor->log2_chroma_h > 1 ||
                  (bitDepth != 8 && bitDepth != 10)))) {
        return false;
    }
    for (int component = 1; component < descriptor->nb_components; ++component) {
        if (descriptor->comp[component].depth != bitDepth) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] int swsColorSpace(const domain::ColorMetadata& metadata) noexcept {
    return metadata.matrix == domain::ColorMatrix::kBt709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
}

[[nodiscard]] bool supportsD3d11Va(const AVCodec* const decoder) noexcept {
    for (int index = 0;; ++index) {
        const AVCodecHWConfig* const configuration = avcodec_get_hw_config(decoder, index);
        if (configuration == nullptr) {
            return false;
        }
        if (configuration->device_type == AV_HWDEVICE_TYPE_D3D11VA &&
            (configuration->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
            return true;
        }
    }
}

} // namespace

class SoftwareDecoder::Impl final {
public:
    Impl(const domain::SourceId sourceIdValue,
         domain::MediaDescriptor descriptorValue,
         platform::FrameBudget& frameBudget,
         const std::atomic<bool>* const externalInterrupt,
         std::shared_ptr<platform::GraphicsDeviceBroker> deviceBrokerValue,
         const std::uint32_t softwareThreadCountValue)
        : sourceId(sourceIdValue), descriptor(std::move(descriptorValue)), frameBudget(frameBudget),
          deviceBroker(std::move(deviceBrokerValue)),
          factory(frameBudget,
                  application::FramePresentation{
                      .rotationDegrees = descriptor.rotationDegrees,
                      .sampleAspectNumerator = descriptor.sampleAspectRatio.numerator,
                      .sampleAspectDenominator = descriptor.sampleAspectRatio.denominator,
                  }),
          interruptState{.requested = &interrupted, .externalRequested = externalInterrupt},
          bufferPool(3U), softwareThreadCount(softwareThreadCountValue) {}

    [[nodiscard]] static AVPixelFormat
    selectHardwareFormat(AVCodecContext* const context,
                         const AVPixelFormat* const formats) noexcept {
        auto* const self = static_cast<Impl*>(context->opaque);
        if (self != nullptr && self->hardwareRequested) {
            for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
                if (*format == AV_PIX_FMT_D3D11) {
                    self->backend = DecoderBackend::D3d11Va;
                    return *format;
                }
            }
            self->backend = DecoderBackend::Software;
            self->fallbackReason =
                "The decoder did not offer a D3D11VA output format for this stream.";
        }
        return avcodec_default_get_format(context, formats);
    }

    domain::SourceId sourceId;
    domain::MediaDescriptor descriptor;
    platform::FrameBudget& frameBudget;
    std::shared_ptr<platform::GraphicsDeviceBroker> deviceBroker;
    platform::FrameResourceFactory factory;
    std::atomic<bool> interrupted = false;
    InterruptState interruptState;
    AvFormatContextPtr format;
    AvCodecContextPtr codec;
    AvPacketPtr packet;
    AvFramePtr frame;
    AvBufferRefPtr hardwareDevice;
    std::optional<platform::GraphicsDeviceLease> hardwareLease;
    SwsContextPtr sws;
    platform::Nv12BufferPool bufferPool;
    int streamIndex = -1;
    AVRational timeBase{};
    std::shared_ptr<const std::vector<std::int64_t>> presentationTimestamps;
    std::optional<domain::FrameId> lastReturnedFrame;
    std::uint64_t exactSeekCount = 0;
    bool packetPending = false;
    bool inputEnded = false;
    bool flushSubmitted = false;
    bool sequentialReady = false;
    bool opened = false;
    bool hardwareRequested = false;
    media::DecoderBackend backend = media::DecoderBackend::Software;
    std::string fallbackReason;
    std::uint32_t softwareThreadCount = 0U;
};

SoftwareDecoder::SoftwareDecoder(const domain::SourceId sourceId,
                                 domain::MediaDescriptor descriptor,
                                 platform::FrameBudget& frameBudget,
                                 const std::atomic<bool>* const externalInterrupt,
                                 std::shared_ptr<platform::GraphicsDeviceBroker> deviceBroker,
                                 const std::uint32_t softwareThreadCount)
    : impl_(std::make_unique<Impl>(sourceId,
                                   std::move(descriptor),
                                   frameBudget,
                                   externalInterrupt,
                                   std::move(deviceBroker),
                                   softwareThreadCount)) {}

SoftwareDecoder::~SoftwareDecoder() = default;

domain::Status SoftwareDecoder::open(const std::atomic<bool>& cancellationRequested) {
    close();
    impl_->interrupted.store(cancellationRequested.load(std::memory_order_acquire),
                             std::memory_order_release);
    if (cancellationRequested.load(std::memory_order_acquire)) {
        return domain::Status::failure(decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                                                   impl_->sourceId,
                                                   "Source decoder open was canceled.",
                                                   true));
    }
    if (!impl_->descriptor.sourceIdentity.has_value() ||
        !impl_->descriptor.sourceIdentity->isComplete()) {
        return domain::Status::failure(decodeError(
            domain::MediaErrorCode::kInvalidMediaDescriptor,
            impl_->sourceId,
            "A direct decoder requires a complete source identity captured by media probing."));
    }

    const domain::SourceId sourceId = impl_->sourceId;
    const auto identity =
        platform::SourceIdentityService::verify(impl_->descriptor.normalizedPath,
                                                *impl_->descriptor.sourceIdentity,
                                                sourceId,
                                                domain::MediaOperation::kMediaDecode);
    if (!identity) {
        return identity;
    }

    const auto normalizedPath =
        platform::WindowsPaths::absolutePath(impl_->descriptor.normalizedPath);
    if (!normalizedPath) {
        return domain::Status::failure(decodeError(domain::MediaErrorCode::kMediaOpenFailed,
                                                   sourceId,
                                                   "Could not normalize the source path: " +
                                                       normalizedPath.error().technicalDetail));
    }
    const std::u8string pathUtf8 = normalizedPath.value().u8string();
    const std::string inputUrl{reinterpret_cast<const char*>(pathUtf8.data()), pathUtf8.size()};

    AVFormatContext* rawFormat = avformat_alloc_context();
    if (rawFormat == nullptr) {
        return domain::Status::failure(decodeError(domain::MediaErrorCode::kMediaOpenFailed,
                                                   sourceId,
                                                   "FFmpeg could not allocate a format context."));
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
                        sourceId,
                        "FFmpeg could not open the source: " + ffmpegError(openResult),
                        true));
    }
    AvFormatContextPtr openedFormat{rawFormat};

    const int streamInfoResult = avformat_find_stream_info(openedFormat.get(), nullptr);
    if (streamInfoResult < 0) {
        return domain::Status::failure(decodeError(
            domain::MediaErrorCode::kMediaDecodeFailed,
            sourceId,
            "FFmpeg could not read source stream information: " + ffmpegError(streamInfoResult),
            true));
    }

    const int selectedStream =
        av_find_best_stream(openedFormat.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (selectedStream < 0 ||
        static_cast<unsigned int>(selectedStream) >= openedFormat->nb_streams) {
        return domain::Status::failure(decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                                                   sourceId,
                                                   "The source has no readable video stream.",
                                                   true));
    }
    const AVStream* const stream = openedFormat->streams[selectedStream];
    if (stream == nullptr || stream->codecpar == nullptr || stream->codecpar->width <= 0 ||
        stream->codecpar->height <= 0) {
        return domain::Status::failure(decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                                                   sourceId,
                                                   "The selected video stream is incomplete."));
    }
    if (static_cast<std::uint32_t>(stream->codecpar->width) != impl_->descriptor.extent.width ||
        static_cast<std::uint32_t>(stream->codecpar->height) != impl_->descriptor.extent.height) {
        return domain::Status::failure(
            decodeError(domain::MediaErrorCode::kSourceFingerprintMismatch,
                        sourceId,
                        "The source geometry changed after media probing.",
                        true));
    }

    const AVCodec* const decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        return domain::Status::failure(
            decodeError(domain::MediaErrorCode::kUnsupportedCodec,
                        sourceId,
                        "FFmpeg has no decoder for the probed source codec.",
                        true));
    }
    const auto allocateCodec = [decoder, stream]() {
        AvCodecContextPtr result{avcodec_alloc_context3(decoder)};
        if (result != nullptr &&
            avcodec_parameters_to_context(result.get(), stream->codecpar) < 0) {
            result.reset();
        }
        return result;
    };
    AvCodecContextPtr openedCodec = allocateCodec();
    if (openedCodec == nullptr) {
        return domain::Status::failure(
            decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                        sourceId,
                        "FFmpeg could not allocate or configure a codec context."));
    }
    impl_->backend = DecoderBackend::Software;
    impl_->fallbackReason.clear();
    impl_->hardwareRequested = false;
    impl_->hardwareDevice.reset();
    impl_->hardwareLease.reset();
    if (impl_->softwareThreadCount > 0U) {
        openedCodec->thread_count = static_cast<int>(impl_->softwareThreadCount);
    }

    if (!impl_->deviceBroker) {
        impl_->fallbackReason = "No shared Qt D3D11 device was configured.";
    } else if (!supportsD3d11Va(decoder)) {
        impl_->fallbackReason = "The FFmpeg decoder does not expose D3D11VA for this codec.";
    } else {
        platform::GraphicsDeviceLeaseResult leaseResult = impl_->deviceBroker->tryLease();
        if (leaseResult.status == platform::GraphicsDeviceLeaseStatus::Available &&
            leaseResult.lease) {
            AvBufferRefPtr hardwareDevice{av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA)};
            if (hardwareDevice != nullptr) {
                auto* const deviceContext =
                    reinterpret_cast<AVHWDeviceContext*>(hardwareDevice->data);
                auto* const d3d11Context =
                    static_cast<AVD3D11VADeviceContext*>(deviceContext->hwctx);
                d3d11Context->device = leaseResult.lease->device.Get();
                d3d11Context->device->AddRef();
                d3d11Context->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
                const int hardwareInitResult = av_hwdevice_ctx_init(hardwareDevice.get());
                if (hardwareInitResult >= 0) {
                    openedCodec->opaque = impl_.get();
                    openedCodec->get_format = &Impl::selectHardwareFormat;
                    openedCodec->extra_hw_frames = 8;
                    openedCodec->hw_device_ctx = av_buffer_ref(hardwareDevice.get());
                    if (openedCodec->hw_device_ctx != nullptr) {
                        impl_->hardwareRequested = true;
                        impl_->hardwareDevice = std::move(hardwareDevice);
                        impl_->hardwareLease = std::move(*leaseResult.lease);
                    } else {
                        impl_->fallbackReason =
                            "FFmpeg could not retain the shared D3D11 device context.";
                    }
                } else {
                    impl_->fallbackReason =
                        "FFmpeg could not initialize the shared D3D11 device: " +
                        ffmpegError(hardwareInitResult);
                }
            } else {
                impl_->fallbackReason = "FFmpeg could not allocate a D3D11VA device context.";
            }
        } else if (leaseResult.status == platform::GraphicsDeviceLeaseStatus::Busy) {
            impl_->fallbackReason = "The shared D3D11 device was busy during decoder open.";
        } else if (leaseResult.status == platform::GraphicsDeviceLeaseStatus::Closed) {
            impl_->fallbackReason = "The shared D3D11 device broker is closed.";
        } else {
            impl_->fallbackReason = "The shared D3D11 device is not available.";
        }
    }

    int codecOpenResult = avcodec_open2(openedCodec.get(), decoder, nullptr);
    if (codecOpenResult < 0 && impl_->hardwareRequested) {
        impl_->fallbackReason = "D3D11VA decoder open failed; software fallback is active: " +
                                ffmpegError(codecOpenResult);
        impl_->hardwareRequested = false;
        impl_->backend = DecoderBackend::Software;
        impl_->hardwareDevice.reset();
        impl_->hardwareLease.reset();
        openedCodec = allocateCodec();
        if (openedCodec == nullptr) {
            return domain::Status::failure(decodeError(
                domain::MediaErrorCode::kMediaDecodeFailed,
                sourceId,
                "FFmpeg could not recreate the software decoder after D3D11VA failed."));
        }
        if (impl_->softwareThreadCount > 0U) {
            openedCodec->thread_count = static_cast<int>(impl_->softwareThreadCount);
        }
        codecOpenResult = avcodec_open2(openedCodec.get(), decoder, nullptr);
    }
    if (codecOpenResult < 0) {
        return domain::Status::failure(
            decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                        sourceId,
                        "FFmpeg could not open the source decoder: " + ffmpegError(codecOpenResult),
                        true));
    }
    if (impl_->hardwareRequested) {
        impl_->backend = DecoderBackend::D3d11Va;
    }

    impl_->format = std::move(openedFormat);
    impl_->codec = std::move(openedCodec);
    impl_->streamIndex = selectedStream;
    impl_->timeBase = stream->time_base;
    impl_->opened = true;

    TimelineIndexCancellationState indexCancellation{
        .request = &cancellationRequested,
        .interrupted = &impl_->interrupted,
        .external = impl_->interruptState.externalRequested,
    };
    auto timestamps = buildPresentationTimestampIndex(TimestampIndexRequest{
        .sourcePath = impl_->descriptor.normalizedPath,
        .sourceId = sourceId,
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
    return domain::Status::success();
}

domain::Result<DecodedFrame>
SoftwareDecoder::decodeExact(const domain::FrameId frameId,
                             const std::atomic<bool>& cancellationRequested) {
    auto result = decodeInternal(frameId, cancellationRequested, false, true);
    if (!result) {
        impl_->sequentialReady = false;
    }
    return result;
}

domain::Result<DecodedFrame>
SoftwareDecoder::decodeSequential(const domain::FrameId frameId,
                                  const std::atomic<bool>& cancellationRequested) {
    const bool continueSequentially = impl_->sequentialReady &&
                                      impl_->lastReturnedFrame.has_value() && frameId.isValid() &&
                                      frameId.value() > impl_->lastReturnedFrame->value();
    auto result = decodeInternal(frameId, cancellationRequested, continueSequentially, true);
    if (!result) {
        impl_->sequentialReady = false;
    }
    return result;
}

domain::Result<DecodedFrame>
SoftwareDecoder::decodeInternal(const domain::FrameId frameId,
                                const std::atomic<bool>& cancellationRequested,
                                const bool continueSequentially,
                                const bool allowTimelineRecovery) {
    const domain::SourceId sourceId = impl_->sourceId;
    impl_->interrupted.store(cancellationRequested.load(std::memory_order_acquire),
                             std::memory_order_release);
    if (cancellationRequested.load(std::memory_order_acquire)) {
        return domain::Result<DecodedFrame>::failure(
            decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                        sourceId,
                        "Frame decoding was canceled.",
                        true));
    }
    if (!impl_->opened || impl_->format == nullptr || impl_->codec == nullptr) {
        return domain::Result<DecodedFrame>::failure(decodeError(
            domain::MediaErrorCode::kMediaDecodeFailed, sourceId, "Frame decoder is not open."));
    }
    if (!frameId.isValid() || frameId.value() >= impl_->descriptor.frameCount.value) {
        return domain::Result<DecodedFrame>::failure(
            decodeError(domain::MediaErrorCode::kInvalidFrameId,
                        sourceId,
                        "Requested frame is outside the source timeline."));
    }
    if (impl_->timeBase.num <= 0 || impl_->timeBase.den <= 0) {
        return domain::Result<DecodedFrame>::failure(
            decodeError(domain::MediaErrorCode::kInvalidRate,
                        sourceId,
                        "The decoded source has a stream time base outside FFmpeg limits."));
    }
    if (!impl_->presentationTimestamps) {
        return domain::Result<DecodedFrame>::failure(
            decodeError(domain::MediaErrorCode::kFrameTimelineInvalid,
                        sourceId,
                        "The decoder was opened without a native presentation timestamp index.",
                        true));
    }
    const std::int64_t targetTimestamp =
        (*impl_->presentationTimestamps)[static_cast<std::size_t>(frameId.value())];
    std::optional<std::int64_t> fullDecodeTimestamp;
    if (!continueSequentially) {
        impl_->codec->skip_frame = AVDISCARD_DEFAULT;
        if (frameId.value() > kExactFullDecodeTailFrames) {
            const auto fullDecodeFrame =
                static_cast<std::size_t>(frameId.value() - kExactFullDecodeTailFrames);
            fullDecodeTimestamp = (*impl_->presentationTimestamps)[fullDecodeFrame];
        }
        const int seekResult = av_seek_frame(
            impl_->format.get(), impl_->streamIndex, targetTimestamp, AVSEEK_FLAG_BACKWARD);
        if (seekResult < 0) {
            return domain::Result<DecodedFrame>::failure(decodeError(
                domain::MediaErrorCode::kMediaDecodeFailed,
                sourceId,
                "FFmpeg could not seek to the target keyframe: " + ffmpegError(seekResult),
                true));
        }
        ++impl_->exactSeekCount;
        application::PlaybackTrace::instance().record(
            application::TraceEventKind::DecoderSeek,
            application::TraceIdentity{
                .request = domain::RequestId{static_cast<std::uint64_t>(impl_->sourceId)}},
            static_cast<std::uint64_t>(frameId.value()));
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
        return domain::Result<DecodedFrame>::failure(
            decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                        sourceId,
                        "FFmpeg could not allocate decode buffers."));
    }

    const auto interruptionRequested = [&] {
        const bool external =
            impl_->interruptState.externalRequested != nullptr &&
            impl_->interruptState.externalRequested->load(std::memory_order_acquire);
        return cancellationRequested.load(std::memory_order_acquire) ||
               impl_->interrupted.load(std::memory_order_acquire) || external;
    };
    for (;;) {
        if (interruptionRequested()) {
            return domain::Result<DecodedFrame>::failure(
                decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                            sourceId,
                            "Frame decoding was interrupted.",
                            true));
        }

        const int receiveResult = avcodec_receive_frame(impl_->codec.get(), impl_->frame.get());
        if (receiveResult == 0) {
            const std::int64_t timestamp = impl_->frame->best_effort_timestamp != AV_NOPTS_VALUE
                                               ? impl_->frame->best_effort_timestamp
                                               : impl_->frame->pts;
            if (timestamp == AV_NOPTS_VALUE) {
                return domain::Result<DecodedFrame>::failure(
                    decodeError(domain::MediaErrorCode::kFrameTimelineInvalid,
                                sourceId,
                                "A decoded frame has no presentation timestamp.",
                                true));
            }
            if (timestamp < targetTimestamp) {
                av_frame_unref(impl_->frame.get());
                continue;
            }
            if (timestamp > targetTimestamp) {
                if (!continueSequentially && allowTimelineRecovery) {
                    // An FFmpeg demuxer interrupted by a superseding request can retain a
                    // poisoned read position even after av_seek_frame reports success. Reopening
                    // once restores the source to a deterministic state; a repeated mismatch is
                    // still surfaced as a real timeline error.
                    const std::uint64_t seekAttempts = impl_->exactSeekCount;
                    const domain::Status reopened = open(cancellationRequested);
                    if (!reopened) {
                        return domain::Result<DecodedFrame>::failure(reopened.error());
                    }
                    impl_->exactSeekCount = seekAttempts;
                    return decodeInternal(frameId, cancellationRequested, false, false);
                }
                return domain::Result<DecodedFrame>::failure(
                    decodeError(domain::MediaErrorCode::kFrameTimelineInvalid,
                                sourceId,
                                "The indexed timestamp did not identify decoded frame " +
                                    std::to_string(frameId.value()) + " (target " +
                                    std::to_string(targetTimestamp) + ", decoded " +
                                    std::to_string(timestamp) + ").",
                                true));
            }

            if (impl_->frame->format == AV_PIX_FMT_D3D11) {
                if (!impl_->hardwareLease || !impl_->deviceBroker ||
                    impl_->deviceBroker->currentGeneration() !=
                        impl_->hardwareLease->deviceGeneration) {
                    return domain::Result<DecodedFrame>::failure(decodeError(
                        domain::MediaErrorCode::kMediaDecodeFailed,
                        sourceId,
                        "The shared D3D11 device generation changed during hardware decode.",
                        true));
                }
                auto* const texture = reinterpret_cast<ID3D11Texture2D*>(impl_->frame->data[0]);
                const std::intptr_t sliceValue =
                    reinterpret_cast<std::intptr_t>(impl_->frame->data[1]);
                if (texture == nullptr || sliceValue < 0 ||
                    static_cast<std::uint64_t>(sliceValue) >
                        (std::numeric_limits<std::uint32_t>::max)()) {
                    return domain::Result<DecodedFrame>::failure(
                        decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                                    sourceId,
                                    "FFmpeg returned an invalid D3D11VA texture slice.",
                                    true));
                }
                D3D11_TEXTURE2D_DESC textureDescription{};
                texture->GetDesc(&textureDescription);
                std::optional<application::NormalizedFrameFormat> outputFormat;
                if (textureDescription.Format == DXGI_FORMAT_NV12 &&
                    impl_->descriptor.bitDepth == 8U) {
                    outputFormat = application::NormalizedFrameFormat::Nv12_8;
                } else if (textureDescription.Format == DXGI_FORMAT_P010 &&
                           impl_->descriptor.bitDepth == 10U) {
                    outputFormat = application::NormalizedFrameFormat::P010_10;
                }
                if (!outputFormat) {
                    return domain::Result<DecodedFrame>::failure(decodeError(
                        domain::MediaErrorCode::kUnsupportedPixelFormat,
                        sourceId,
                        "D3D11VA returned a texture outside the normalized NV12/P010 contract.",
                        true));
                }

                AVFrame* const clonedFrame = av_frame_clone(impl_->frame.get());
                if (clonedFrame == nullptr) {
                    return domain::Result<DecodedFrame>::failure(
                        decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                                    sourceId,
                                    "FFmpeg could not retain the decoded D3D11VA frame.",
                                    true));
                }
                std::shared_ptr<const void> lifetimeAnchor{
                    clonedFrame,
                    [](const void* const opaque) noexcept {
                        auto* frame = const_cast<AVFrame*>(static_cast<const AVFrame*>(opaque));
                        av_frame_free(&frame);
                    },
                };
                Microsoft::WRL::ComPtr<ID3D11Texture2D> retainedTexture = texture;
                auto handle = platform::D3d11DecodedFrameResource::create(
                    std::move(retainedTexture),
                    static_cast<std::uint32_t>(sliceValue),
                    static_cast<std::uint32_t>(impl_->frame->width),
                    static_cast<std::uint32_t>(impl_->frame->height),
                    impl_->hardwareLease->deviceGeneration,
                    *outputFormat,
                    impl_->descriptor.colorMetadata,
                    application::FramePresentation{
                        .rotationDegrees = impl_->descriptor.rotationDegrees,
                        .sampleAspectNumerator = impl_->descriptor.sampleAspectRatio.numerator,
                        .sampleAspectDenominator = impl_->descriptor.sampleAspectRatio.denominator,
                    },
                    std::move(lifetimeAnchor),
                    impl_->frameBudget);
                if (!handle) {
                    return domain::Result<DecodedFrame>::failure(
                        decodeError(domain::MediaErrorCode::kFrameBudgetExceeded,
                                    sourceId,
                                    "The shared frame budget could not retain the D3D11VA frame.",
                                    true));
                }
                const std::int64_t presentationMicroseconds = av_rescale_q(
                    timestamp, impl_->timeBase, AVRational{.num = 1, .den = AV_TIME_BASE});
                auto result = domain::Result<DecodedFrame>::success(DecodedFrame{
                    .handle = std::move(*handle),
                    .presentationTime = domain::MediaTime{presentationMicroseconds},
                });
                av_frame_unref(impl_->frame.get());
                impl_->lastReturnedFrame = frameId;
                impl_->sequentialReady = true;
                return result;
            }

            if (!isSupportedDecodedFormat(*impl_->frame)) {
                return domain::Result<DecodedFrame>::failure(decodeError(
                    domain::MediaErrorCode::kUnsupportedPixelFormat,
                    sourceId,
                    "The decoder produced a pixel format outside the normalized 8/10-bit "
                    "4:2:0 contract.",
                    true));
            }

            if (impl_->frame->width <= 0 || impl_->frame->height <= 0 ||
                static_cast<std::uint32_t>(impl_->frame->width) != impl_->descriptor.extent.width ||
                static_cast<std::uint32_t>(impl_->frame->height) !=
                    impl_->descriptor.extent.height) {
                return domain::Result<DecodedFrame>::failure(
                    decodeError(domain::MediaErrorCode::kSourceFingerprintMismatch,
                                sourceId,
                                "Decoded frame geometry changed after media probing.",
                                true));
            }
            const std::uint32_t width = static_cast<std::uint32_t>(impl_->frame->width);
            const std::uint32_t height = static_cast<std::uint32_t>(impl_->frame->height);
            const bool outputP010 = impl_->descriptor.bitDepth == 10U;
            const std::uint64_t alignedWidth = (static_cast<std::uint64_t>(width) + 1U) & ~1ULL;
            const std::uint64_t stride64 = alignedWidth * (outputP010 ? 2U : 1U);
            if (width == 0U || height == 0U || stride64 > static_cast<std::uint64_t>(INT_MAX)) {
                return domain::Result<DecodedFrame>::failure(
                    decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                                sourceId,
                                "Decoded frame dimensions cannot form a valid normalized layout."));
            }
            const std::uint32_t stride = static_cast<std::uint32_t>(stride64);
            try {
                std::optional<platform::WritableCpuNv12Frame> writableNv12;
                std::optional<platform::WritableCpuP010Frame> writableP010;
                if (outputP010) {
                    writableP010 = impl_->factory.acquireWritableCpuP010(
                        platform::P010FrameLayout{
                            .width = width,
                            .height = height,
                            .yStride = stride,
                            .uvStride = stride,
                        },
                        impl_->descriptor.colorMetadata,
                        impl_->bufferPool);
                } else {
                    writableNv12 = impl_->factory.acquireWritableCpuNv12(
                        platform::Nv12FrameLayout{
                            .width = width,
                            .height = height,
                            .yStride = stride,
                            .uvStride = stride,
                        },
                        impl_->descriptor.colorMetadata,
                        impl_->bufferPool);
                }
                if ((!outputP010 && !writableNv12) || (outputP010 && !writableP010)) {
                    return domain::Result<DecodedFrame>::failure(decodeError(
                        domain::MediaErrorCode::kFrameBudgetExceeded,
                        sourceId,
                        "The shared frame budget could not reserve this normalized frame.",
                        true));
                }

                const AVPixelFormat outputFormat = outputP010 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
                SwsContext* const cached =
                    sws_getCachedContext(impl_->sws.release(),
                                         impl_->frame->width,
                                         impl_->frame->height,
                                         static_cast<AVPixelFormat>(impl_->frame->format),
                                         impl_->frame->width,
                                         impl_->frame->height,
                                         outputFormat,
                                         SWS_BILINEAR,
                                         nullptr,
                                         nullptr,
                                         nullptr);
                impl_->sws.reset(cached);
                if (impl_->sws == nullptr) {
                    return domain::Result<DecodedFrame>::failure(
                        decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                                    sourceId,
                                    "FFmpeg could not create a normalized conversion context."));
                }
                const AVPixFmtDescriptor* const sourcePixelDescriptor =
                    av_pix_fmt_desc_get(static_cast<AVPixelFormat>(impl_->frame->format));
                const bool sourceRgb = sourcePixelDescriptor != nullptr &&
                                       (sourcePixelDescriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0;
                const int* const coefficients =
                    sws_getCoefficients(swsColorSpace(impl_->descriptor.colorMetadata));
                const int sourceFullRange =
                    sourceRgb || impl_->frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
                const int destinationFullRange =
                    impl_->descriptor.colorMetadata.range == domain::ColorRange::kFull ? 1 : 0;
                if (coefficients == nullptr || sws_setColorspaceDetails(impl_->sws.get(),
                                                                        coefficients,
                                                                        sourceFullRange,
                                                                        coefficients,
                                                                        destinationFullRange,
                                                                        0,
                                                                        1 << 16,
                                                                        1 << 16) < 0) {
                    return domain::Result<DecodedFrame>::failure(
                        decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                                    sourceId,
                                    "FFmpeg could not configure normalized color conversion."));
                }
                const std::span<std::uint8_t> yPlane =
                    outputP010 ? writableP010->yPlane() : writableNv12->yPlane();
                const std::span<std::uint8_t> uvPlane =
                    outputP010 ? writableP010->uvPlane() : writableNv12->uvPlane();
                std::array<std::uint8_t*, 4> outputPlanes{
                    yPlane.data(),
                    uvPlane.data(),
                    nullptr,
                    nullptr,
                };
                const std::array<int, 4> outputStrides{
                    static_cast<int>(stride),
                    static_cast<int>(stride),
                    0,
                    0,
                };
                const int convertedRows = sws_scale(impl_->sws.get(),
                                                    impl_->frame->data,
                                                    impl_->frame->linesize,
                                                    0,
                                                    impl_->frame->height,
                                                    outputPlanes.data(),
                                                    outputStrides.data());
                if (convertedRows != impl_->frame->height) {
                    return domain::Result<DecodedFrame>::failure(
                        decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                                    sourceId,
                                    "FFmpeg could not convert the decoded frame to the normalized "
                                    "storage format."));
                }
                auto handle =
                    outputP010 ? std::move(*writableP010).seal() : std::move(*writableNv12).seal();
                if (!handle) {
                    return domain::Result<DecodedFrame>::failure(
                        decodeError(domain::MediaErrorCode::kFrameBudgetExceeded,
                                    sourceId,
                                    "The shared frame budget could not seal this normalized frame.",
                                    true));
                }
                const std::int64_t presentationMicroseconds = av_rescale_q(
                    timestamp, impl_->timeBase, AVRational{.num = 1, .den = AV_TIME_BASE});
                auto result = domain::Result<DecodedFrame>::success(DecodedFrame{
                    .handle = std::move(*handle),
                    .presentationTime = domain::MediaTime{presentationMicroseconds},
                });
                av_frame_unref(impl_->frame.get());
                impl_->lastReturnedFrame = frameId;
                impl_->sequentialReady = true;
                return result;
            } catch (const std::exception& exception) {
                return domain::Result<DecodedFrame>::failure(decodeError(
                    domain::MediaErrorCode::kMediaDecodeFailed,
                    sourceId,
                    "Decoded normalized allocation failed: " + std::string{exception.what()},
                    true));
            }
        }

        if (receiveResult != AVERROR(EAGAIN) && receiveResult != AVERROR_EOF) {
            return domain::Result<DecodedFrame>::failure(decodeError(
                domain::MediaErrorCode::kMediaDecodeFailed,
                sourceId,
                "FFmpeg could not receive a decoded frame: " + ffmpegError(receiveResult),
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
                    return domain::Result<DecodedFrame>::failure(decodeError(
                        domain::MediaErrorCode::kMediaDecodeFailed,
                        sourceId,
                        "FFmpeg could not read a source packet: " + ffmpegError(readResult),
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
            impl_->codec->skip_frame = fullDecodeTimestamp.has_value() &&
                                               impl_->packet->pts != AV_NOPTS_VALUE &&
                                               impl_->packet->pts < *fullDecodeTimestamp
                                           ? AVDISCARD_NONREF
                                           : AVDISCARD_DEFAULT;
            const int sendResult = avcodec_send_packet(impl_->codec.get(), impl_->packet.get());
            if (sendResult == 0) {
                av_packet_unref(impl_->packet.get());
                impl_->packetPending = false;
                continue;
            }
            if (sendResult == AVERROR(EAGAIN)) {
                // Preserve the packet. The next receive attempt drains output before retrying it.
                continue;
            }
            return domain::Result<DecodedFrame>::failure(
                decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                            sourceId,
                            "FFmpeg could not submit a source packet: " + ffmpegError(sendResult),
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
            return domain::Result<DecodedFrame>::failure(
                decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                            sourceId,
                            "FFmpeg could not flush the decoder: " + ffmpegError(flushResult),
                            true));
        }

        if (impl_->inputEnded && impl_->flushSubmitted) {
            return domain::Result<DecodedFrame>::failure(
                decodeError(domain::MediaErrorCode::kMediaDecodeFailed,
                            sourceId,
                            "The decoder requested input after its stream had been fully flushed.",
                            true));
        }
    }

    return domain::Result<DecodedFrame>::failure(
        decodeError(domain::MediaErrorCode::kFrameTimelineInvalid,
                    sourceId,
                    "The decoder reached end of stream before the indexed frame timestamp.",
                    true));
}

std::uint64_t SoftwareDecoder::exactSeekCount() const noexcept {
    return impl_->exactSeekCount;
}

media::DecoderBackend SoftwareDecoder::backend() const noexcept {
    return impl_->backend;
}

std::string SoftwareDecoder::fallbackReason() const {
    return impl_->fallbackReason;
}

domain::DeviceGeneration SoftwareDecoder::deviceGeneration() const noexcept {
    return impl_->hardwareLease ? impl_->hardwareLease->deviceGeneration
                                : domain::DeviceGeneration{0U};
}

void SoftwareDecoder::requestInterrupt() noexcept {
    impl_->interrupted.store(true, std::memory_order_release);
    impl_->sequentialReady = false;
}

void SoftwareDecoder::close() noexcept {
    impl_->interrupted.store(true, std::memory_order_release);
    impl_->frame.reset();
    impl_->packet.reset();
    impl_->sws.reset();
    impl_->codec.reset();
    impl_->hardwareDevice.reset();
    impl_->hardwareLease.reset();
    impl_->format.reset();
    impl_->streamIndex = -1;
    impl_->timeBase = AVRational{};
    impl_->presentationTimestamps.reset();
    impl_->lastReturnedFrame.reset();
    impl_->exactSeekCount = 0;
    impl_->packetPending = false;
    impl_->inputEnded = false;
    impl_->flushSubmitted = false;
    impl_->sequentialReady = false;
    impl_->opened = false;
    impl_->hardwareRequested = false;
    impl_->backend = DecoderBackend::Software;
}

} // namespace dvs::media::internal
