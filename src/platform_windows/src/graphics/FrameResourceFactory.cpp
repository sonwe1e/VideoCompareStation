#include "dvs/platform/FrameResourceFactory.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

namespace dvs::platform {

FrameResourceFactory::FrameResourceFactory(
    FrameBudget& frameBudget, const application::FramePresentation presentation) noexcept
    : frameBudget_(frameBudget), presentation_(presentation) {}

WritableCpuNv12Frame::WritableCpuNv12Frame(const Nv12FrameLayout layout,
                                           const domain::ColorMetadata colorMetadata,
                                           const application::FramePresentation presentation,
                                           const Nv12PlaneByteCounts byteCounts,
                                           FrameBudget::Reservation reservation,
                                           Nv12BufferPool::Buffer storage) noexcept
    : layout_(layout), colorMetadata_(colorMetadata), presentation_(presentation),
      byteCounts_(byteCounts), reservation_(std::move(reservation)), storage_(std::move(storage)) {}

std::span<std::uint8_t> WritableCpuNv12Frame::yPlane() noexcept {
    return storage_.bytes().first(byteCounts_.y);
}

std::span<std::uint8_t> WritableCpuNv12Frame::uvPlane() noexcept {
    return storage_.bytes().subspan(byteCounts_.y, byteCounts_.uv);
}

std::optional<application::FrameHandle> WritableCpuNv12Frame::seal() && noexcept {
    try {
        auto resource = std::shared_ptr<CpuNv12FrameResource>(new CpuNv12FrameResource(
            layout_, colorMetadata_, byteCounts_.y, std::move(reservation_), std::move(storage_)));
        return application::FrameHandle::create(std::move(resource),
                                                application::FrameGeometry{
                                                    .width = layout_.width,
                                                    .height = layout_.height,
                                                    .presentation = presentation_,
                                                },
                                                byteCounts_.total());
    } catch (...) {
        return std::nullopt;
    }
}

WritableCpuP010Frame::WritableCpuP010Frame(const P010FrameLayout layout,
                                           const domain::ColorMetadata colorMetadata,
                                           const application::FramePresentation presentation,
                                           const P010PlaneByteCounts byteCounts,
                                           FrameBudget::Reservation reservation,
                                           Nv12BufferPool::Buffer storage) noexcept
    : layout_(layout), colorMetadata_(colorMetadata), presentation_(presentation),
      byteCounts_(byteCounts), reservation_(std::move(reservation)), storage_(std::move(storage)) {}

std::span<std::uint8_t> WritableCpuP010Frame::yPlane() noexcept {
    return storage_.bytes().first(byteCounts_.y);
}

std::span<std::uint8_t> WritableCpuP010Frame::uvPlane() noexcept {
    return storage_.bytes().subspan(byteCounts_.y, byteCounts_.uv);
}

std::optional<application::FrameHandle> WritableCpuP010Frame::seal() && noexcept {
    try {
        auto resource = std::shared_ptr<CpuP010FrameResource>(new CpuP010FrameResource(
            layout_, colorMetadata_, byteCounts_.y, std::move(reservation_), std::move(storage_)));
        return application::FrameHandle::create(std::move(resource),
                                                application::FrameGeometry{
                                                    .width = layout_.width,
                                                    .height = layout_.height,
                                                    .presentation = presentation_,
                                                },
                                                byteCounts_.total());
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<WritableCpuNv12Frame>
FrameResourceFactory::acquireWritableCpuNv12(const Nv12FrameLayout layout,
                                             const domain::ColorMetadata colorMetadata,
                                             Nv12BufferPool& bufferPool) const noexcept {
    const std::optional<Nv12PlaneByteCounts> byteCounts = layout.byteCounts();
    if (!byteCounts || !colorMetadata.isValid() || !presentation_.isValid()) {
        return std::nullopt;
    }
    std::optional<FrameBudget::Reservation> reservation =
        frameBudget_.tryReserve(byteCounts->total());
    if (!reservation) {
        return std::nullopt;
    }
    std::optional<Nv12BufferPool::Buffer> storage = bufferPool.acquire(byteCounts->total());
    if (!storage) {
        return std::nullopt;
    }
    return WritableCpuNv12Frame{layout,
                                colorMetadata,
                                presentation_,
                                *byteCounts,
                                std::move(*reservation),
                                std::move(*storage)};
}

std::optional<WritableCpuP010Frame>
FrameResourceFactory::acquireWritableCpuP010(const P010FrameLayout layout,
                                             const domain::ColorMetadata colorMetadata,
                                             Nv12BufferPool& bufferPool) const noexcept {
    const std::optional<P010PlaneByteCounts> byteCounts = layout.byteCounts();
    if (!byteCounts || !colorMetadata.isValid() || !presentation_.isValid()) {
        return std::nullopt;
    }
    std::optional<FrameBudget::Reservation> reservation =
        frameBudget_.tryReserve(byteCounts->total());
    if (!reservation) {
        return std::nullopt;
    }
    std::optional<Nv12BufferPool::Buffer> storage = bufferPool.acquire(byteCounts->total());
    if (!storage) {
        return std::nullopt;
    }
    return WritableCpuP010Frame{layout,
                                colorMetadata,
                                presentation_,
                                *byteCounts,
                                std::move(*reservation),
                                std::move(*storage)};
}

std::optional<application::FrameHandle>
FrameResourceFactory::createCpuNv12(const Nv12FrameLayout layout,
                                    const domain::ColorMetadata colorMetadata,
                                    const std::span<const std::uint8_t> sourceY,
                                    const std::span<const std::uint8_t> sourceUv) const noexcept {
    const std::optional<Nv12PlaneByteCounts> byteCounts = layout.byteCounts();
    if (!byteCounts || !colorMetadata.isValid() || sourceY.size() < byteCounts->y ||
        sourceUv.size() < byteCounts->uv) {
        return std::nullopt;
    }

    Nv12BufferPool bufferPool{0U};
    std::optional<WritableCpuNv12Frame> writable =
        acquireWritableCpuNv12(layout, colorMetadata, bufferPool);
    if (!writable) {
        return std::nullopt;
    }

    try {
        std::copy_n(sourceY.begin(), byteCounts->y, writable->yPlane().begin());
        std::copy_n(sourceUv.begin(), byteCounts->uv, writable->uvPlane().begin());
        return std::move(*writable).seal();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<application::FrameHandle>
FrameResourceFactory::createCpuP010(const P010FrameLayout layout,
                                    const domain::ColorMetadata colorMetadata,
                                    const std::span<const std::uint8_t> sourceY,
                                    const std::span<const std::uint8_t> sourceUv) const noexcept {
    const std::optional<P010PlaneByteCounts> byteCounts = layout.byteCounts();
    if (!byteCounts || !colorMetadata.isValid() || sourceY.size() < byteCounts->y ||
        sourceUv.size() < byteCounts->uv) {
        return std::nullopt;
    }

    Nv12BufferPool bufferPool{0U};
    std::optional<WritableCpuP010Frame> writable =
        acquireWritableCpuP010(layout, colorMetadata, bufferPool);
    if (!writable) {
        return std::nullopt;
    }

    try {
        std::copy_n(sourceY.begin(), byteCounts->y, writable->yPlane().begin());
        std::copy_n(sourceUv.begin(), byteCounts->uv, writable->uvPlane().begin());
        return std::move(*writable).seal();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace dvs::platform
