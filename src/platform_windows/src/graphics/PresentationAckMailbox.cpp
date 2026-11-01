#include "dvs/platform/PresentationAckMailbox.h"

#include <cstdint>
#include <type_traits>

namespace dvs::platform {
namespace {

static_assert(std::is_nothrow_copy_constructible_v<application::FrameSetPresented>);

constexpr std::uint64_t kClosedMask = std::uint64_t{1} << 63U;
constexpr std::uint64_t kProducerActiveMask = std::uint64_t{1} << 62U;
constexpr std::uint64_t kSequenceMask = kProducerActiveMask - 1U;

} // namespace

PresentationAckPushResult
PresentationAckMailbox::tryPush(const application::FrameSetPresented& acknowledgement) noexcept {
    std::uint64_t producerState = producerState_.load(std::memory_order_acquire);
    if ((producerState & kClosedMask) != 0U) {
        return PresentationAckPushResult::Closed;
    }

    const std::uint64_t writeSequence = producerState & kSequenceMask;
    const std::uint64_t readSequence = readSequence_.load(std::memory_order_acquire);
    if (writeSequence - readSequence >= kCapacity || writeSequence == kSequenceMask) {
        return PresentationAckPushResult::Full;
    }

    const std::uint64_t claimedState = producerState | kProducerActiveMask;
    if (!producerState_.compare_exchange_strong(
            producerState, claimedState, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return PresentationAckPushResult::Closed;
    }

    entries_[static_cast<std::size_t>(writeSequence % kCapacity)].emplace(acknowledgement);
    producerState = claimedState;
    for (;;) {
        // An acknowledgement admitted before close remains lossless. Preserve a concurrently set
        // closed bit while advancing the visible sequence and clearing the active marker.
        const std::uint64_t publishedState = (producerState & kClosedMask) | (writeSequence + 1U);
        if (producerState_.compare_exchange_strong(producerState,
                                                   publishedState,
                                                   std::memory_order_release,
                                                   std::memory_order_acquire)) {
            return PresentationAckPushResult::Accepted;
        }
    }
}

std::optional<application::FrameSetPresented> PresentationAckMailbox::tryPop() noexcept {
    const std::uint64_t readSequence = readSequence_.load(std::memory_order_relaxed);
    const std::uint64_t producerState = producerState_.load(std::memory_order_acquire);
    const std::uint64_t writeSequence = producerState & kSequenceMask;
    if (readSequence == writeSequence) {
        return std::nullopt;
    }

    std::optional<application::FrameSetPresented>& entry =
        entries_[static_cast<std::size_t>(readSequence % kCapacity)];
    std::optional<application::FrameSetPresented> result = std::move(entry);
    entry.reset();
    readSequence_.store(readSequence + 1U, std::memory_order_release);
    return result;
}

void PresentationAckMailbox::close() noexcept {
    static_cast<void>(producerState_.fetch_or(kClosedMask, std::memory_order_acq_rel));
}

bool PresentationAckMailbox::isClosed() const noexcept {
    return (producerState_.load(std::memory_order_acquire) & kClosedMask) != 0U;
}

bool PresentationAckMailbox::isDrained() const noexcept {
    const std::uint64_t producerState = producerState_.load(std::memory_order_acquire);
    return (producerState & kClosedMask) != 0U && (producerState & kProducerActiveMask) == 0U &&
           (producerState & kSequenceMask) == readSequence_.load(std::memory_order_acquire);
}

} // namespace dvs::platform
