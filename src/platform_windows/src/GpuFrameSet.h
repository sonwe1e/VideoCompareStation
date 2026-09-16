#pragma once

#include "dvs/application/RequestContext.h"
#include "dvs/domain/ComparisonSource.h"
#include "dvs/domain/Identifiers.h"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace dvs::platform {

class GpuFrameResource;

// One source's GPU-resident frame within a canonical frame set. The slot carries the source
// identity independently of the underlying resource's internal role metadata.
struct GpuFrameSlot final {
    domain::SourceId sourceId = 0;
    std::shared_ptr<const GpuFrameResource> frame;
};

// A complete set of GPU frames for one canonical frame position, published atomically. The set
// owns zero or more source slots; an incomplete set is still published with explicit missing
// entries instead of being dropped or partially advanced.
class GpuFrameSet final {
public:
    [[nodiscard]] static std::shared_ptr<const GpuFrameSet>
    create(application::FrameRequestContext context,
           domain::FrameId frameId,
           std::vector<GpuFrameSlot> slots) noexcept;

    [[nodiscard]] const application::FrameRequestContext& context() const noexcept;
    [[nodiscard]] const domain::FrameId& frameId() const noexcept;
    [[nodiscard]] std::span<const GpuFrameSlot> slots() const noexcept;
    [[nodiscard]] const GpuFrameSlot* find(domain::SourceId sourceId) const noexcept;
    [[nodiscard]] std::size_t frameCount() const noexcept;
    [[nodiscard]] std::size_t accountedBytes() const noexcept;

private:
    GpuFrameSet(application::FrameRequestContext context,
                domain::FrameId frameId,
                std::vector<GpuFrameSlot> slots,
                std::size_t accountedBytes) noexcept;

    application::FrameRequestContext context_;
    domain::FrameId frameId_;
    std::vector<GpuFrameSlot> slots_;
    std::size_t accountedBytes_ = 0U;
};

} // namespace dvs::platform
