#pragma once

#include "dvs/application/RequestContext.h"

#include <cstdint>

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

} // namespace dvs::application
