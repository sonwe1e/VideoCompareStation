#pragma once

#include "dvs/domain/MediaDescriptor.h"
#include "dvs/domain/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace dvs::application {
class IFrameProvider;
}

namespace dvs::platform {
class FrameBudget;
}

namespace dvs::ui {

struct DirectComparisonFrame final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t accountedBytes = 0;
};

// A UI-facing, synchronous adapter over the serialized playback coordinator. The command-line
// smoke path uses it today; a QML controller can reuse the same command/state protocol later.
struct DirectComparisonResult final {
    domain::FrameId frameId{0};
    DirectComparisonFrame sourceA;
    DirectComparisonFrame sourceB;
    std::size_t reservedBytes = 0;
};

[[nodiscard]] domain::Result<DirectComparisonResult>
compareDirectSources(std::shared_ptr<application::IFrameProvider> provider,
                     platform::FrameBudget& frameBudget,
                     const domain::MediaDescriptor& sourceA,
                     const domain::MediaDescriptor& sourceB,
                     domain::FrameId frameId);

} // namespace dvs::ui
