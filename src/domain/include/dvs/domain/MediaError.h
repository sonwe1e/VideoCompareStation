#pragma once

#include "dvs/domain/Identifiers.h"

#include <optional>
#include <string>
#include <string_view>

namespace dvs::domain {

enum class SessionState {
    kEmpty,
    kLoading,
    kReady,
    kInvalid,
    kError,
};

enum class PlaybackState {
    kPaused,
    kPlaying,
    kSeeking,
    kBuffering,
};

enum class MediaErrorCode {
    kInvalidArgument,
    kInvalidRate,
    kInvalidFrameId,
    kInvalidFrameCount,
    kInvalidDimensions,
    kInvalidDuration,
    kInvalidMediaDescriptor,
    kArithmeticOverflow,
    kSourceFrameRateMismatch,
    kSourceFrameCountMismatch,
    kSourceDurationMismatch,
    kSourceResolutionMismatch,
    kSourceColorMetadataMismatch,
    kFrameOutOfRange,
    kSourceMissing,
    kSourceFingerprintMismatch,
    kFileIo,
    kMediaOpenFailed,
    kMediaProbeFailed,
    kInvalidCfrTiming,
    kUnsupportedCodec,
    kUnsupportedPixelFormat,
    kMediaDecodeFailed,
    kFrameTimelineInvalid,
    kFrameBudgetExceeded,
    kGraphicsUnavailable,
    kGraphicsDeviceLost,
    kFramePresentationTimedOut,
    kInterpolationProtocolInvalid,
};

enum class MediaOperation {
    kRationalConversion,
    kMediaDescriptorValidation,
    kSourcePairValidation,
    kPersistence,
    kMediaProbe,
    kMediaDecode,
    kGraphicsInitialization,
    kFramePresentation,
    kInterpolationProtocol,
};

[[nodiscard]] std::string_view stableId(SessionState state) noexcept;
[[nodiscard]] std::string_view stableId(PlaybackState state) noexcept;
[[nodiscard]] std::string_view stableId(MediaErrorCode code) noexcept;
[[nodiscard]] std::string_view stableId(MediaOperation operation) noexcept;

// The UI maps userMessageKey to localized presentation text. technicalDetail stays diagnostic and
// must never be used as a user-visible label. When the error belongs to one loaded source, its
// session source id is carried; session-wide errors leave it empty.
struct MediaError final {
    MediaErrorCode code = MediaErrorCode::kInvalidArgument;
    MediaOperation operation = MediaOperation::kPersistence;
    std::optional<SourceId> source;
    std::optional<RequestId> requestId;
    bool recoverable = false;
    std::string userMessageKey;
    std::string technicalDetail;
};

[[nodiscard]] MediaError makeMediaError(MediaErrorCode code,
                                        MediaOperation operation,
                                        std::optional<SourceId> source,
                                        bool recoverable,
                                        std::string technicalDetail = {},
                                        std::optional<RequestId> requestId = std::nullopt);

} // namespace dvs::domain
