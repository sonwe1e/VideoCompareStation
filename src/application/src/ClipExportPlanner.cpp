#include "dvs/application/ClipExport.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace dvs::application {
namespace {

[[nodiscard]] domain::MediaError clipExportError(const domain::MediaErrorCode code,
                                                 std::string detail) {
    return domain::makeMediaError(code,
                                  domain::MediaOperation::kMediaDescriptorValidation,
                                  std::nullopt,
                                  false,
                                  std::move(detail));
}

// The instant a clip must stop at to include `frame` where `frame` is the frame *after* the
// requested out point. A rational (constant-rate) timeline can name the instant after the last
// frame; a variable-rate timeline only knows the frames it holds, so running past its end means
// "copy to the end of the stream" and the writer is handed no bound.
[[nodiscard]] std::optional<std::int64_t>
exclusiveEndMicroseconds(const domain::CanonicalTimeline& timeline,
                         const std::int64_t frameCount,
                         const domain::FrameId outInclusive) {
    if (outInclusive.value() >= frameCount - 1) {
        if (const auto* const rate = std::get_if<domain::RationalRate>(&timeline);
            rate != nullptr) {
            const auto end = rate->frameStartTime(domain::FrameId{frameCount});
            if (end) {
                return end.value().microseconds();
            }
        }
        return std::nullopt;
    }
    const auto end =
        domain::canonicalFrameStartTime(timeline, domain::FrameId{outInclusive.value() + 1});
    if (!end) {
        return std::nullopt;
    }
    return end.value().microseconds();
}

} // namespace

domain::Result<ClipExportPlan> planClipExport(const domain::CanonicalTimeline& timeline,
                                              const std::int64_t frameCount,
                                              const PlaybackRange requested) {
    if (frameCount <= 0) {
        return domain::Result<ClipExportPlan>::failure(
            clipExportError(domain::MediaErrorCode::kInvalidFrameCount,
                            "A clip export needs a canonical timeline with at least one frame."));
    }
    if (!requested.isValid()) {
        return domain::Result<ClipExportPlan>::failure(clipExportError(
            domain::MediaErrorCode::kInvalidFrameId,
            "The requested export range must be a closed interval with a valid in and out point."));
    }
    if (requested.outInclusive.value() >= frameCount) {
        return domain::Result<ClipExportPlan>::failure(
            clipExportError(domain::MediaErrorCode::kFrameOutOfRange,
                            "The requested export range ends outside the canonical timeline."));
    }

    const auto start = domain::canonicalFrameStartTime(timeline, requested.inInclusive);
    if (!start) {
        return domain::Result<ClipExportPlan>::failure(start.error());
    }

    ClipExportPlan plan;
    plan.requestedRange = requested;
    plan.startMicroseconds = start.value().microseconds();
    plan.endMicroseconds = exclusiveEndMicroseconds(timeline, frameCount, requested.outInclusive);
    plan.firstExportedFrame = requested.inInclusive;
    plan.requestedFrameCount = requested.outInclusive.value() - requested.inInclusive.value() + 1;
    return domain::Result<ClipExportPlan>::success(plan);
}

domain::Result<ClipExportPlan>
alignClipExportStart(const domain::CanonicalTimeline& timeline,
                     ClipExportPlan plan,
                     const std::span<const std::int64_t> keyframeTimes) {
    if (keyframeTimes.empty()) {
        return domain::Result<ClipExportPlan>::failure(clipExportError(
            domain::MediaErrorCode::kMediaProbeFailed,
            "The container reported no keyframes, so a stream copy cannot start anywhere."));
    }

    const auto after =
        std::upper_bound(keyframeTimes.begin(), keyframeTimes.end(), plan.startMicroseconds);
    // No keyframe at or before the requested start: the copy has to begin at the first keyframe
    // the container does have, which moves the clip later than asked for.
    const std::int64_t aligned =
        after == keyframeTimes.begin() ? keyframeTimes.front() : *std::prev(after);

    if (plan.endMicroseconds.has_value() && aligned >= *plan.endMicroseconds) {
        return domain::Result<ClipExportPlan>::failure(clipExportError(
            domain::MediaErrorCode::kFrameOutOfRange,
            "The requested range holds no keyframe, so the stream copy would be empty."));
    }

    plan.startShiftMicroseconds = aligned - plan.startMicroseconds;
    plan.startMicroseconds = aligned;
    const auto frame = domain::canonicalFrameAtOrBefore(timeline, domain::MediaTime{aligned});
    if (frame) {
        plan.firstExportedFrame = frame.value();
        if (const auto* const rate = std::get_if<domain::RationalRate>(&timeline);
            rate != nullptr) {
            // Packet times are rounded to the nearest microsecond, but canonical starts round
            // up. Promote only an exact rounded-boundary match, never an arbitrary time just
            // before a frame. Keep sub-microsecond (ambiguous) and variable-rate timelines alone.
            const auto interval = rate->frameIntervalCeiling();
            const domain::FrameId next{frame.value().value() + 1};
            const auto roundedStart = rate->frameStartTimeRounded(next);
            if (interval && interval.value().microseconds() > 1 && roundedStart &&
                roundedStart.value().microseconds() == aligned) {
                plan.firstExportedFrame = next;
            }
        }
    } else {
        plan.firstExportedFrame.reset();
    }
    return domain::Result<ClipExportPlan>::success(plan);
}

} // namespace dvs::application
