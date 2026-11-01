#include "dvs/platform/FrameBudget.h"

#include <atomic>
#include <utility>

namespace dvs::platform {

struct FrameBudget::State final {
    explicit State(const std::size_t capacityBytesIn) noexcept : capacityBytes(capacityBytesIn) {}

    const std::size_t capacityBytes;
    std::atomic<std::size_t> reservedBytes{0U};
};

static_assert(std::atomic<std::size_t>::is_always_lock_free,
              "Frame budget accounting must not block the render thread.");

FrameBudget::Reservation::Reservation(std::shared_ptr<State> state,
                                      const std::size_t bytes) noexcept
    : state_(std::move(state)), bytes_(bytes) {}

FrameBudget::Reservation::~Reservation() {
    reset();
}

FrameBudget::Reservation::Reservation(Reservation&& other) noexcept
    : state_(std::move(other.state_)), bytes_(std::exchange(other.bytes_, 0)) {}

FrameBudget::Reservation& FrameBudget::Reservation::operator=(Reservation&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}

std::size_t FrameBudget::Reservation::bytes() const noexcept {
    return bytes_;
}

FrameBudget::Reservation::operator bool() const noexcept {
    return state_ != nullptr;
}

void FrameBudget::Reservation::reset() noexcept {
    const std::shared_ptr<State> state = std::move(state_);
    const std::size_t bytes = std::exchange(bytes_, 0);
    if (!state) {
        return;
    }

    static_cast<void>(state->reservedBytes.fetch_sub(bytes, std::memory_order_acq_rel));
}

FrameBudget::FrameBudget(const std::size_t capacityBytes)
    : state_(std::make_shared<State>(capacityBytes)) {}

std::optional<FrameBudget::Reservation> FrameBudget::tryReserve(const std::size_t bytes) noexcept {
    std::size_t reserved = state_->reservedBytes.load(std::memory_order_relaxed);
    for (;;) {
        if (bytes > state_->capacityBytes - reserved) {
            return std::nullopt;
        }
        if (state_->reservedBytes.compare_exchange_weak(
                reserved, reserved + bytes, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return Reservation{state_, bytes};
        }
    }
}

std::size_t FrameBudget::capacityBytes() const noexcept {
    return state_->capacityBytes;
}

std::size_t FrameBudget::reservedBytes() const noexcept {
    return state_->reservedBytes.load(std::memory_order_acquire);
}

std::size_t FrameBudget::availableBytes() const noexcept {
    return state_->capacityBytes - state_->reservedBytes.load(std::memory_order_acquire);
}

} // namespace dvs::platform
