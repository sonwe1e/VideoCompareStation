#pragma once

#include "dvs/application/RequestContext.h"
#include "dvs/domain/Identifiers.h"

#include <cstdint>
#include <optional>

namespace dvs::application {

struct SeekFrameCommand final {
    CommandContext context;
    domain::FrameId frameId;
};

struct StepFramesCommand final {
    CommandContext context;
    std::int64_t delta = 0;
};

struct FirstFrameCommand final {
    CommandContext context;
};

struct LastFrameCommand final {
    CommandContext context;
};

struct PlayCommand final {
    CommandContext context;
    // Visual playback rate. 1.0 is real time; the UI exposes a fixed ladder so the
    // coordinator can keep its checked timeline arithmetic on integer microseconds.
    double speed = 1.0;
};

struct PauseCommand final {
    CommandContext context;
};

struct SetPlaybackRateCommand final {
    CommandContext context;
    double speed = 1.0;
};

// Closed canonical-frame interval used by kernel-native range playback. Inclusive on both ends.
struct PlaybackRange final {
    domain::FrameId inInclusive{0};
    domain::FrameId outInclusive{0};

    [[nodiscard]] bool isValid() const noexcept {
        return inInclusive.isValid() && outInclusive.isValid() &&
               inInclusive.value() <= outInclusive.value();
    }

    [[nodiscard]] bool operator==(const PlaybackRange&) const noexcept = default;
};

// Installs, replaces, or clears the session playback range. `loop` only applies when a valid
// range is present. Clearing sends `range = nullopt` (loop is then ignored).
struct SetPlaybackRangeCommand final {
    CommandContext context;
    std::optional<PlaybackRange> range;
    bool loop = false;
};

// Atomically install a playback range (with loop intent) and start a playback run under that
// range. Used by UI "play range" so range authority and the run start cannot race.
struct StartRangePlaybackCommand final {
    CommandContext context;
    PlaybackRange range;
    bool loop = true;
    double speed = 1.0;
};

} // namespace dvs::application
