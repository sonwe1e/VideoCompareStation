#pragma once

#include "dvs/application/FrameHandle.h"
#include "dvs/application/NormalizedFrameFormat.h"
#include "dvs/application/RequestContext.h"
#include "dvs/domain/Identifiers.h"
#include "dvs/domain/MediaDescriptor.h"
#include "dvs/domain/MediaError.h"
#include "dvs/platform/FrameBudget.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>

namespace dvs::platform {

class GpuFrameResource;

// Deferred resources use an intrusive MPSC retirement stack. A render-thread final release only
// performs a lock-free push and notification; the transfer actor is the sole drain/delete owner.
class GpuFrameRetirementDomain final {
public:
    GpuFrameRetirementDomain() = default;
    ~GpuFrameRetirementDomain();

    GpuFrameRetirementDomain(const GpuFrameRetirementDomain&) = delete;
    GpuFrameRetirementDomain& operator=(const GpuFrameRetirementDomain&) = delete;

    [[nodiscard]] std::size_t drainRetired() noexcept;
    [[nodiscard]] std::size_t liveResourceCount() const noexcept;
    [[nodiscard]] bool hasRetiredResources() const noexcept;
    void notifyActivity() noexcept;
    [[nodiscard]] std::condition_variable& activityCondition() noexcept;

private:
    friend class GpuFrameResource;

    void registerResource() noexcept;
    void retire(const GpuFrameResource* resource) noexcept;

    std::condition_variable activity_;
    std::atomic<GpuFrameResource*> retiredHead_{nullptr};
    std::atomic<std::size_t> liveResourceCount_{0U};
};

// The D3D11 upload checkpoint supplies a concrete backing that owns its texture and shader views.
// Keeping the polymorphic owner private to the platform adapter avoids native handles in public
// application contracts while making the frame lifetime independently testable now.
class IGpuFrameBacking {
public:
    virtual ~IGpuFrameBacking();

protected:
    IGpuFrameBacking() = default;
};

// Backing and reservation are deliberately inseparable once handed to a frame. Member destruction
// is reversed, so every exit path destroys the native backing before returning its budget bytes.
class GpuFrameAllocation final {
public:
    GpuFrameAllocation(FrameBudget::Reservation reservation,
                       std::unique_ptr<const IGpuFrameBacking> backing) noexcept;
    GpuFrameAllocation(std::size_t accountedBytes,
                       std::shared_ptr<const void> accountingAnchor,
                       std::unique_ptr<const IGpuFrameBacking> backing) noexcept;
    ~GpuFrameAllocation() = default;

    GpuFrameAllocation(const GpuFrameAllocation&) = delete;
    GpuFrameAllocation& operator=(const GpuFrameAllocation&) = delete;
    GpuFrameAllocation(GpuFrameAllocation&&) noexcept = default;
    GpuFrameAllocation& operator=(GpuFrameAllocation&&) = delete;

private:
    friend class GpuFrameResource;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] std::size_t accountedBytes() const noexcept;
    [[nodiscard]] const IGpuFrameBacking& backing() const noexcept;

    FrameBudget::Reservation reservation_;
    std::size_t accountedBytes_ = 0U;
    std::shared_ptr<const void> accountingAnchor_;
    std::unique_ptr<const IGpuFrameBacking> backing_;
};

struct GpuFrameIdentity final {
    application::FrameRequestContext context;
    domain::FrameId frameId;
    domain::SourceId sourceId;

    [[nodiscard]] constexpr bool operator==(const GpuFrameIdentity&) const = default;
};

class GpuFrameResource final : public application::IFrameResource {
public:
    [[nodiscard]] static std::shared_ptr<const GpuFrameResource>
    create(GpuFrameIdentity identity,
           application::FrameGeometry geometry,
           domain::ColorMetadata colorMetadata,
           GpuFrameAllocation allocation,
           application::NormalizedFrameFormat format =
               application::NormalizedFrameFormat::Nv12_8) noexcept;

    // Used only by the D3D11 transfer actor. The domain must outlive every returned control block;
    // the shared deleter captures it without the resource retaining a reference back to it.
    [[nodiscard]] static std::shared_ptr<const GpuFrameResource>
    createDeferred(GpuFrameIdentity identity,
                   application::FrameGeometry geometry,
                   domain::ColorMetadata colorMetadata,
                   GpuFrameAllocation allocation,
                   std::shared_ptr<GpuFrameRetirementDomain> retirementDomain,
                   application::NormalizedFrameFormat format =
                       application::NormalizedFrameFormat::Nv12_8) noexcept;

    ~GpuFrameResource() override = default;

    [[nodiscard]] const application::FrameRequestContext& context() const noexcept;
    [[nodiscard]] const domain::FrameId& frameId() const noexcept;
    [[nodiscard]] const application::FrameGeometry& geometry() const noexcept;
    [[nodiscard]] const domain::ColorMetadata& colorMetadata() const noexcept;
    [[nodiscard]] application::NormalizedFrameFormat format() const noexcept;
    [[nodiscard]] domain::DeviceGeneration deviceGeneration() const noexcept;
    [[nodiscard]] const IGpuFrameBacking& backing() const noexcept;
    [[nodiscard]] std::size_t accountedBytes() const noexcept;

private:
    friend class GpuFrameRetirementDomain;

    GpuFrameResource(GpuFrameIdentity identity,
                     application::FrameGeometry geometry,
                     domain::ColorMetadata colorMetadata,
                     GpuFrameAllocation&& allocation,
                     application::NormalizedFrameFormat format) noexcept;

    GpuFrameIdentity identity_;
    application::FrameGeometry geometry_;
    domain::ColorMetadata colorMetadata_;
    GpuFrameAllocation allocation_;
    application::NormalizedFrameFormat format_ = application::NormalizedFrameFormat::Nv12_8;
    mutable std::atomic<GpuFrameResource*> retirementNext_{nullptr};
};

} // namespace dvs::platform
