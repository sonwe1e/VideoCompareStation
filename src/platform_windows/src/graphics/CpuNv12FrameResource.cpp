#include "dvs/platform/CpuNv12FrameResource.h"

#include <limits>
#include <mutex>
#include <utility>

namespace dvs::platform {
namespace {

[[nodiscard]] std::optional<std::size_t> checkedProduct(const std::size_t left,
                                                        const std::size_t right) noexcept {
    if (left == 0 || right == 0) {
        return std::size_t{0};
    }
    if (left > std::numeric_limits<std::size_t>::max() / right) {
        return std::nullopt;
    }
    return left * right;
}

[[nodiscard]] std::optional<std::size_t> checkedSum(const std::size_t left,
                                                    const std::size_t right) noexcept {
    if (left > std::numeric_limits<std::size_t>::max() - right) {
        return std::nullopt;
    }
    return left + right;
}

[[nodiscard]] std::optional<std::uint32_t> minimumUvStride(const std::uint32_t width) noexcept {
    if ((width % 2U) == 0U) {
        return width;
    }
    if (width == std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return width + 1U;
}

} // namespace

struct Nv12BufferPool::State final {
    explicit State(const std::size_t maximumRetainedBuffersValue)
        : maximumRetainedBuffers(maximumRetainedBuffersValue) {}

    std::mutex mutex;
    std::size_t maximumRetainedBuffers = 0U;
    std::vector<std::vector<std::uint8_t>> available;
};

Nv12BufferPool::Buffer::Buffer(std::shared_ptr<State> state,
                               std::vector<std::uint8_t> storage) noexcept
    : state_(std::move(state)), storage_(std::move(storage)) {}

Nv12BufferPool::Buffer::~Buffer() {
    release();
}

Nv12BufferPool::Buffer::Buffer(Buffer&& other) noexcept
    : state_(std::move(other.state_)), storage_(std::move(other.storage_)) {}

Nv12BufferPool::Buffer& Nv12BufferPool::Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        storage_ = std::move(other.storage_);
    }
    return *this;
}

std::span<std::uint8_t> Nv12BufferPool::Buffer::bytes() noexcept {
    return storage_;
}

std::span<const std::uint8_t> Nv12BufferPool::Buffer::bytes() const noexcept {
    return storage_;
}

Nv12BufferPool::Buffer::operator bool() const noexcept {
    return state_ != nullptr && !storage_.empty();
}

void Nv12BufferPool::Buffer::release() noexcept {
    if (state_ != nullptr && !storage_.empty()) {
        try {
            std::scoped_lock lock{state_->mutex};
            if (state_->available.size() < state_->maximumRetainedBuffers) {
                state_->available.push_back(std::move(storage_));
            }
        } catch (...) {
        }
    }
    storage_.clear();
    state_.reset();
}

Nv12BufferPool::Nv12BufferPool(const std::size_t maximumRetainedBuffers)
    : state_(std::make_shared<State>(maximumRetainedBuffers)) {}

std::optional<Nv12BufferPool::Buffer> Nv12BufferPool::acquire(const std::size_t bytes) noexcept {
    if (bytes == 0U) {
        return std::nullopt;
    }
    try {
        std::vector<std::uint8_t> storage;
        {
            std::scoped_lock lock{state_->mutex};
            if (!state_->available.empty()) {
                storage = std::move(state_->available.back());
                state_->available.pop_back();
            }
        }
        storage.resize(bytes);
        return Buffer{state_, std::move(storage)};
    } catch (...) {
        return std::nullopt;
    }
}

bool Nv12FrameLayout::isValid() const noexcept {
    return byteCounts().has_value();
}

std::optional<Nv12PlaneByteCounts> Nv12FrameLayout::byteCounts() const noexcept {
    if (width == 0 || height == 0 || yStride < width) {
        return std::nullopt;
    }

    const std::optional<std::uint32_t> minimumUv = minimumUvStride(width);
    if (!minimumUv || uvStride < *minimumUv) {
        return std::nullopt;
    }

    const std::size_t chromaRows = (static_cast<std::size_t>(height) + 1U) / 2U;
    const std::optional<std::size_t> yBytes = checkedProduct(yStride, height);
    const std::optional<std::size_t> uvBytes = checkedProduct(uvStride, chromaRows);
    if (!yBytes || !uvBytes || !checkedSum(*yBytes, *uvBytes)) {
        return std::nullopt;
    }

    return Nv12PlaneByteCounts{.y = *yBytes, .uv = *uvBytes};
}

CpuNv12FrameResource::CpuNv12FrameResource(const Nv12FrameLayout layout,
                                           const domain::ColorMetadata colorMetadata,
                                           const std::size_t yPlaneBytes,
                                           FrameBudget::Reservation reservation,
                                           Nv12BufferPool::Buffer storage) noexcept
    : layout_(layout), colorMetadata_(colorMetadata), yPlaneBytes_(yPlaneBytes),
      reservation_(std::move(reservation)), storage_(std::move(storage)) {}

const Nv12FrameLayout& CpuNv12FrameResource::layout() const noexcept {
    return layout_;
}

std::span<const std::uint8_t> CpuNv12FrameResource::yPlane() const noexcept {
    return storage_.bytes().first(yPlaneBytes_);
}

std::span<const std::uint8_t> CpuNv12FrameResource::uvPlane() const noexcept {
    return storage_.bytes().subspan(yPlaneBytes_);
}

std::size_t CpuNv12FrameResource::byteCount() const noexcept {
    return storage_.bytes().size();
}

const domain::ColorMetadata& CpuNv12FrameResource::colorMetadata() const noexcept {
    return colorMetadata_;
}

} // namespace dvs::platform
