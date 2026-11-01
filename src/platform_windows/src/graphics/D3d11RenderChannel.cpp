#include "dvs/platform/D3d11RenderChannel.h"

#include "dvs/platform/GpuTransferActor.h"

#include <utility>

namespace dvs::platform {

D3d11RenderChannel::D3d11RenderChannel(std::shared_ptr<GpuTransferActor> transferActor) noexcept
    : transferActor_(std::move(transferActor)) {}

application::RenderPublishResult
D3d11RenderChannel::publish(const application::FrameRequestContext& context,
                            application::FrameSet set) noexcept {
    if (!transferActor_) {
        return application::RenderPublishResult::Closed;
    }

    switch (transferActor_->submit(context, std::move(set))) {
    case GpuTransferSubmitResult::Accepted:
        return application::RenderPublishResult::Accepted;
    case GpuTransferSubmitResult::Replaced:
        return application::RenderPublishResult::Replaced;
    case GpuTransferSubmitResult::InvalidSet:
    case GpuTransferSubmitResult::StaleContext:
    case GpuTransferSubmitResult::DeviceUnavailable:
    case GpuTransferSubmitResult::Closed:
        return application::RenderPublishResult::Closed;
    }

    return application::RenderPublishResult::Closed;
}

void D3d11RenderChannel::clear(const application::PlaybackRequestContext& context) noexcept {
    if (transferActor_) {
        static_cast<void>(transferActor_->clear(context));
    }
}

} // namespace dvs::platform
