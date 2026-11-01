#include "dvs/ui/StartupMilestone.h"

#include <QtGlobal>

#include <chrono>
#include <cstdio>

namespace dvs::ui {
namespace {

[[nodiscard]] bool startupTimingEnabled() {
    static const bool kEnabled = qEnvironmentVariableIsSet("DVS_STARTUP_TIMING");
    return kEnabled;
}

[[nodiscard]] const std::chrono::steady_clock::time_point& startupTimingOrigin() {
    static const std::chrono::steady_clock::time_point kOrigin = std::chrono::steady_clock::now();
    return kOrigin;
}

} // namespace

void markStartupMilestone(const char* const name) noexcept {
    if (name == nullptr || !startupTimingEnabled()) {
        return;
    }
    // Initialise the origin before sampling "now": on the first call the static origin
    // would otherwise be set after the sample and the first mark would print a negative
    // elapsed time, which parsers treat as corrupt.
    const std::chrono::steady_clock::time_point& origin = startupTimingOrigin();
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - origin);
    const auto absolute =
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch());
    std::fprintf(stderr,
                 "[startup] %-24s +%9lld us  t=%lld us\n",
                 name,
                 static_cast<long long>(elapsed.count()),
                 static_cast<long long>(absolute.count()));
    std::fflush(stderr);
}

} // namespace dvs::ui
