#pragma once

#include "FrameTimelineIndex.h"

#include <memory>
#include <optional>
#include <vector>

namespace dvs::media::internal {

using PresentationTimestampIndex = std::shared_ptr<const std::vector<std::int64_t>>;

class PresentationIndexCache final {
public:
    [[nodiscard]] static std::optional<PresentationTimestampIndex>
    load(const TimestampIndexRequest& request) noexcept;
    static void store(const TimestampIndexRequest& request,
                      const PresentationTimestampIndex& index) noexcept;
};

} // namespace dvs::media::internal
