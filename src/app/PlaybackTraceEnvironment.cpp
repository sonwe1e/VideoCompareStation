#include "PlaybackTraceEnvironment.h"

#include <cstdlib>
#include <memory>

namespace dvs::app {

std::optional<std::filesystem::path> playbackTracePathFromEnvironment() {
    wchar_t* tracePath = nullptr;
    std::size_t tracePathLength = 0U;
    if (_wdupenv_s(&tracePath, &tracePathLength, L"DVS_PLAYBACK_TRACE") != 0) {
        return std::nullopt;
    }
    const std::unique_ptr<wchar_t, decltype(&std::free)> ownedTracePath{tracePath, &std::free};
    if (tracePathLength <= 1U) {
        return std::nullopt;
    }
    return std::filesystem::path{ownedTracePath.get()};
}

} // namespace dvs::app
