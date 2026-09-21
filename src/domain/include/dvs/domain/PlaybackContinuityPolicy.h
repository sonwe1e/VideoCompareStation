#pragma once

#include <cstdint>
#include <string_view>

namespace dvs::domain {

// C-07: explicit playback continuity mode. The status bar must show the active mode.
enum class PlaybackContinuityPolicy : std::uint8_t {
    // Never skip whole FrameSets; clock slips under load (review default for multi-source).
    ReviewEveryFrame = 0,
    // Skip complete FrameSets to recover wall-clock anchor after a stall.
    RealTime = 1,
    // Single-source sessions default RealTime; multi-source default ReviewEveryFrame.
    Contextual = 2,
};

[[nodiscard]] constexpr PlaybackContinuityPolicy
resolveContinuityPolicy(const PlaybackContinuityPolicy requested,
                        const std::size_t sourceCount) noexcept {
    if (requested != PlaybackContinuityPolicy::Contextual) {
        return requested;
    }
    return sourceCount <= 1U ? PlaybackContinuityPolicy::RealTime
                             : PlaybackContinuityPolicy::ReviewEveryFrame;
}

[[nodiscard]] constexpr std::string_view
playbackContinuityPolicyName(const PlaybackContinuityPolicy policy) noexcept {
    switch (policy) {
    case PlaybackContinuityPolicy::ReviewEveryFrame:
        return "ReviewEveryFrame";
    case PlaybackContinuityPolicy::RealTime:
        return "RealTime";
    case PlaybackContinuityPolicy::Contextual:
        return "Contextual";
    }
    return "Contextual";
}

} // namespace dvs::domain
