#pragma once

#include "dvs/application/Ports.h"

#include <memory>

namespace dvs::platform {

class GpuTransferActor;

// Application-facing adapter for the complete-set upload actor. Admission is bounded to the
// actor's active task plus one latest pending set; no D3D call occurs on the publisher thread.
class D3d11RenderChannel final : public application::IRenderChannel {
public:
    explicit D3d11RenderChannel(std::shared_ptr<GpuTransferActor> transferActor) noexcept;
    ~D3d11RenderChannel() override = default;

    [[nodiscard]] application::RenderPublishResult
    publish(const application::FrameRequestContext& context,
            application::FrameSet set) noexcept override;
    [[nodiscard]] bool clear(const application::PlaybackRequestContext& context) noexcept override;

private:
    std::shared_ptr<GpuTransferActor> transferActor_;
};

} // namespace dvs::platform
