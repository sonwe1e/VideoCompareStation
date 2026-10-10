#include "dvs/domain/MediaError.h"

#include <utility>

namespace dvs::domain {

std::string_view stableId(const SessionState state) noexcept {
    switch (state) {
    case SessionState::kEmpty:
        return "empty";
    case SessionState::kLoading:
        return "loading";
    case SessionState::kReady:
        return "ready";
    case SessionState::kInvalid:
        return "invalid";
    case SessionState::kError:
        return "error";
    }
    return "unknown";
}

std::string_view stableId(const PlaybackState state) noexcept {
    switch (state) {
    case PlaybackState::kPaused:
        return "paused";
    case PlaybackState::kPlaying:
        return "playing";
    case PlaybackState::kSeeking:
        return "seeking";
    case PlaybackState::kBuffering:
        return "buffering";
    }
    return "unknown";
}

std::string_view stableId(const MediaErrorCode code) noexcept {
    switch (code) {
    case MediaErrorCode::kInvalidArgument:
        return "invalid-argument";
    case MediaErrorCode::kInvalidRate:
        return "invalid-rate";
    case MediaErrorCode::kInvalidFrameId:
        return "invalid-frame-id";
    case MediaErrorCode::kInvalidFrameCount:
        return "invalid-frame-count";
    case MediaErrorCode::kInvalidDimensions:
        return "invalid-dimensions";
    case MediaErrorCode::kInvalidDuration:
        return "invalid-duration";
    case MediaErrorCode::kInvalidMediaDescriptor:
        return "invalid-media-descriptor";
    case MediaErrorCode::kArithmeticOverflow:
        return "arithmetic-overflow";
    case MediaErrorCode::kSourceFrameRateMismatch:
        return "source-frame-rate-mismatch";
    case MediaErrorCode::kSourceFrameCountMismatch:
        return "source-frame-count-mismatch";
    case MediaErrorCode::kSourceDurationMismatch:
        return "source-duration-mismatch";
    case MediaErrorCode::kSourceResolutionMismatch:
        return "source-resolution-mismatch";
    case MediaErrorCode::kSourceColorMetadataMismatch:
        return "source-color-metadata-mismatch";
    case MediaErrorCode::kFrameOutOfRange:
        return "frame-out-of-range";
    case MediaErrorCode::kSourceMissing:
        return "source-missing";
    case MediaErrorCode::kSourceFingerprintMismatch:
        return "source-fingerprint-mismatch";
    case MediaErrorCode::kFileIo:
        return "file-io";
    case MediaErrorCode::kMediaOpenFailed:
        return "media-open-failed";
    case MediaErrorCode::kMediaProbeFailed:
        return "media-probe-failed";
    case MediaErrorCode::kInvalidCfrTiming:
        return "invalid-cfr-timing";
    case MediaErrorCode::kUnsupportedCodec:
        return "unsupported-codec";
    case MediaErrorCode::kUnsupportedPixelFormat:
        return "unsupported-pixel-format";
    case MediaErrorCode::kMediaDecodeFailed:
        return "media-decode-failed";
    case MediaErrorCode::kFrameTimelineInvalid:
        return "frame-timeline-invalid";
    case MediaErrorCode::kFrameBudgetExceeded:
        return "frame-budget-exceeded";
    case MediaErrorCode::kGraphicsUnavailable:
        return "graphics-unavailable";
    case MediaErrorCode::kGraphicsDeviceLost:
        return "graphics-device-lost";
    case MediaErrorCode::kFramePresentationTimedOut:
        return "frame-presentation-timed-out";
    case MediaErrorCode::kInterpolationProtocolInvalid:
        return "interpolation-protocol-invalid";
    }
    return "unknown-error";
}

std::string_view stableId(const MediaOperation operation) noexcept {
    switch (operation) {
    case MediaOperation::kRationalConversion:
        return "rational-conversion";
    case MediaOperation::kMediaDescriptorValidation:
        return "media-descriptor-validation";
    case MediaOperation::kSourcePairValidation:
        return "source-pair-validation";
    case MediaOperation::kPersistence:
        return "persistence";
    case MediaOperation::kMediaProbe:
        return "media-probe";
    case MediaOperation::kMediaDecode:
        return "media-decode";
    case MediaOperation::kGraphicsInitialization:
        return "graphics-initialization";
    case MediaOperation::kFramePresentation:
        return "frame-presentation";
    case MediaOperation::kInterpolationProtocol:
        return "interpolation-protocol";
    }
    return "unknown-operation";
}

MediaError makeMediaError(const MediaErrorCode code,
                          const MediaOperation operation,
                          std::optional<SourceId> source,
                          const bool recoverable,
                          std::string technicalDetail,
                          std::optional<RequestId> requestId) {
    return MediaError{
        .code = code,
        .operation = operation,
        .source = source,
        .requestId = requestId,
        .recoverable = recoverable,
        .userMessageKey = std::string{stableId(code)},
        .technicalDetail = std::move(technicalDetail),
    };
}

} // namespace dvs::domain
