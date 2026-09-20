#pragma once

#include "dvs/application/Events.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace dvs::platform {

enum class PresentationAckPushResult {
    Accepted,
    Full,
    Closed,
};

// Fixed-capacity single-producer/single-consumer queue used between the render thread and its
// relay. Full queues reject the new acknowledgement without overwriting either queued entry, so
// the producer can retain and retry it without waiting. Capacity four absorbs roughly three vsync
// periods of GUI-thread stall on a 120 Hz display before the renderer ever sees Full.
class PresentationAckMailbox final {
public:
    static constexpr std::size_t kCapacity = 4U;

    PresentationAckMailbox() noexcept = default;

    PresentationAckMailbox(const PresentationAckMailbox&) = delete;
    PresentationAckMailbox& operator=(const PresentationAckMailbox&) = delete;
    PresentationAckMailbox(PresentationAckMailbox&&) = delete;
    PresentationAckMailbox& operator=(PresentationAckMailbox&&) = delete;

    [[nodiscard]] PresentationAckPushResult
    tryPush(const application::FrameSetPresented& acknowledgement) noexcept;
    [[nodiscard]] std::optional<application::FrameSetPresented> tryPop() noexcept;

    // Closing is non-blocking. An entry already admitted by the producer may still become visible;
    // the consumer can drain every published entry after close.
    void close() noexcept;
    [[nodiscard]] bool isClosed() const noexcept;
    [[nodiscard]] bool isDrained() const noexcept;

private:
    std::array<std::optional<application::FrameSetPresented>, kCapacity> entries_{};
    std::atomic<std::uint64_t> readSequence_{0U};
    // Bit 63 is the closed marker, bit 62 records an admitted producer, and the lower bits are the
    // monotonically increasing producer sequence. Publishing and closing therefore share one
    // linearization domain, and isDrained cannot race past an admitted push.
    std::atomic<std::uint64_t> producerState_{0U};
};

} // namespace dvs::platform
