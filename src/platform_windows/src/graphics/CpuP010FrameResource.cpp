#include "dvs/platform/CpuP010FrameResource.h"

#include <limits>
#include <utility>

namespace dvs::platform {
namespace {

[[nodiscard]] std::optional<std::size_t> checkedProduct(const std::size_t left,
                                                        const std::size_t right) noexcept {
    if (left != 0U && right > (std::numeric_limits<std::size_t>::max)() / left) {
        return std::nullopt;
    }
    return left * right;
}

[[nodiscard]] std::optional<std::uint32_t> minimumYStride(const std::uint32_t width) noexcept {
    if (width > (std::numeric_limits<std::uint32_t>::max)() / 2U) {
        return std::nullopt;
    }
    return width * 2U;
}

[[nodiscard]] std::optional<std::uint32_t> minimumUvStride(const std::uint32_t width) noexcept {
    const std::uint64_t chromaPairs = (static_cast<std::uint64_t>(width) + 1U) / 2U;
    const std::uint64_t bytes = chromaPairs * 4U;
    if (bytes > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(bytes);
}

} // namespace

bool P010FrameLayout::isValid() const noexcept {
    return byteCounts().has_value();
}

std::optional<P010PlaneByteCounts> P010FrameLayout::byteCounts() const noexcept {
    const std::optional<std::uint32_t> minimumY = minimumYStride(width);
    const std::optional<std::uint32_t> minimumUv = minimumUvStride(width);
    if (width == 0U || height == 0U || !minimumY || !minimumUv || yStride < *minimumY ||
        uvStride < *minimumUv || (yStride % 2U) != 0U || (uvStride % 2U) != 0U) {
        return std::nullopt;
    }

    const std::size_t chromaRows = (static_cast<std::size_t>(height) + 1U) / 2U;
    const std::optional<std::size_t> yBytes = checkedProduct(yStride, height);
    const std::optional<std::size_t> uvBytes = checkedProduct(uvStride, chromaRows);
    if (!yBytes || !uvBytes || *yBytes > (std::numeric_limits<std::size_t>::max)() - *uvBytes) {
        return std::nullopt;
    }
    return P010PlaneByteCounts{.y = *yBytes, .uv = *uvBytes};
}

CpuP010FrameResource::CpuP010FrameResource(const P010FrameLayout layout,
                                           const domain::ColorMetadata colorMetadata,
                                           const std::size_t yPlaneBytes,
                                           FrameBudget::Reservation reservation,
                                           Nv12BufferPool::Buffer storage) noexcept
    : layout_(layout), colorMetadata_(colorMetadata), yPlaneBytes_(yPlaneBytes),
      reservation_(std::move(reservation)), storage_(std::move(storage)) {}

const P010FrameLayout& CpuP010FrameResource::layout() const noexcept {
    return layout_;
}

std::span<const std::uint8_t> CpuP010FrameResource::yPlane() const noexcept {
    return storage_.bytes().first(yPlaneBytes_);
}

std::span<const std::uint8_t> CpuP010FrameResource::uvPlane() const noexcept {
    return storage_.bytes().subspan(yPlaneBytes_);
}

std::size_t CpuP010FrameResource::byteCount() const noexcept {
    return storage_.bytes().size();
}

const domain::ColorMetadata& CpuP010FrameResource::colorMetadata() const noexcept {
    return colorMetadata_;
}

} // namespace dvs::platform
