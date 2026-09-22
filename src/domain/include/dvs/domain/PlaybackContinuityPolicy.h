#pragma once

#include <cstdint>
#include <string_view>

namespace dvs::domain {

// C-07: explicit playback continuity mode. The status bar must show the active mode.
// D03: continuous playback defaults to smoothness (RealTime). ReviewEveryFrame is an
// explicit review option and never becomes the implicit multi-source default.
enum class PlaybackContinuityPolicy : std::uint8_t {
    // Never skip whole FrameSets; clock slips under load (explicit review option).
    ReviewEveryFrame = 0,
    // Skip complete FrameSets to recover wall-clock anchor after a stall. Continuous default.
    RealTime = 1,
    // Smoothness-first alias of RealTime for every source count (D03).
    Contextual = 2,
};

[[nodiscard]] constexpr PlaybackContinuityPolicy
resolveContinuityPolicy(const PlaybackContinuityPolicy requested,
                        const std::size_t /*sourceCount*/) noexcept {
    if (requested == PlaybackContinuityPolicy::ReviewEveryFrame) {
        return PlaybackContinuityPolicy::ReviewEveryFrame;
    }
    // Contextual and RealTime both prefer cadence; only explicit ReviewEveryFrame retains
    // every FrameSet. Catch-up drops whole FrameSets on the comparison pair together.
    return PlaybackContinuityPolicy::RealTime;
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
