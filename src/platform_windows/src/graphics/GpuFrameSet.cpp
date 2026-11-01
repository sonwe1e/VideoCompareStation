#include "GpuFrameSet.h"

#include "GpuFrameResource.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace dvs::platform {

std::shared_ptr<const GpuFrameSet> GpuFrameSet::create(application::FrameRequestContext context,
                                                       domain::FrameId frameId,
                                                       std::vector<GpuFrameSlot> slots) noexcept {
    if (!frameId.isValid()) {
        return {};
    }

    // Validate all slots: must have valid frames, unique sourceIds, consistent
    // context/frameId/device
    std::size_t totalBytes = 0U;
    for (std::size_t index = 0U; index < slots.size(); ++index) {
        const GpuFrameSlot& slot = slots[index];
        if (!slot.frame) {
            return {};
        }

        // Check for duplicate sourceIds
        for (std::size_t other = index + 1U; other < slots.size(); ++other) {
            if (slots[other].sourceId == slot.sourceId) {
                return {};
            }
        }

        // Validate context, frameId, and device generation consistency
        if (slot.frame->context() != context || slot.frame->frameId() != frameId) {
            return {};
        }

        // Accumulate accounted bytes with overflow check
        if (slot.frame->accountedBytes() > std::numeric_limits<std::size_t>::max() - totalBytes) {
            return {};
        }
        totalBytes += slot.frame->accountedBytes();
    }

    try {
        return std::shared_ptr<const GpuFrameSet>{
            new GpuFrameSet(std::move(context), std::move(frameId), std::move(slots), totalBytes)};
    } catch (...) {
        return {};
    }
}

GpuFrameSet::GpuFrameSet(application::FrameRequestContext context,
                         domain::FrameId frameId,
                         std::vector<GpuFrameSlot> slots,
                         const std::size_t accountedBytes) noexcept
    : context_(std::move(context)), frameId_(std::move(frameId)), slots_(std::move(slots)),
      accountedBytes_(accountedBytes) {}

const application::FrameRequestContext& GpuFrameSet::context() const noexcept {
    return context_;
}

const domain::FrameId& GpuFrameSet::frameId() const noexcept {
    return frameId_;
}

std::span<const GpuFrameSlot> GpuFrameSet::slots() const noexcept {
    return slots_;
}

const GpuFrameSlot* GpuFrameSet::find(const domain::SourceId sourceId) const noexcept {
    const auto iterator =
        std::find_if(slots_.begin(), slots_.end(), [sourceId](const GpuFrameSlot& slot) {
            return slot.sourceId == sourceId;
        });
    if (iterator == slots_.end()) {
        return nullptr;
    }
    return &*iterator;
}

std::size_t GpuFrameSet::frameCount() const noexcept {
    return slots_.size();
}

std::size_t GpuFrameSet::accountedBytes() const noexcept {
    return accountedBytes_;
}

} // namespace dvs::platform
