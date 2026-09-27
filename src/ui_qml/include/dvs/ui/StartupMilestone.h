#pragma once

namespace dvs::ui {

// Records one startup milestone on stderr when DVS_STARTUP_TIMING is set: microseconds since the
// first mark plus the steady-clock timestamp, so a launch loop can line the log up with the moment
// it spawned the process. It follows the playback trace's contract - off unless the environment
// asks for it, one environment lookup on entry - which is why it can stay in the release build
// instead of living in a developer-only configuration. Without these marks the launch cost cannot
// be attributed to a phase, and an optimisation cannot be shown to have paid for itself.
void markStartupMilestone(const char* name) noexcept;

} // namespace dvs::ui
