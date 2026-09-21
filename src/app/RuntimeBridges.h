#pragma once

#include "dvs/platform/RenderActivitySink.h"
#include "dvs/ui/ReviewController.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace dvs::media {
class MultiSourceFrameProvider;
}

namespace dvs::app::detail {

class RenderActivityBridge final : public platform::IRenderActivitySink {
public:
    void bind(std::shared_ptr<platform::IRenderActivitySink> sink) noexcept;
    void unbind() noexcept;
    void notifyFramePublished() noexcept override;
    void notifyFrameRenderStarted() noexcept override;
    void notifyAckPublished() noexcept override;
    void notifyAckBackpressured() noexcept override;

private:
    std::atomic<std::shared_ptr<platform::IRenderActivitySink>> sink_;
};

// Bind/unbind run on the controller's GUI thread; notify may run on any producer thread.
class ReviewProjectionBridge final {
public:
    void bind(ui::ReviewController& controller) noexcept;
    void unbind() noexcept;
    void notify() noexcept;

private:
    struct Binding {
        bool active = true;
        bool refreshPending = false;
    };

    std::shared_ptr<std::mutex> mutex_ = std::make_shared<std::mutex>();
    ui::ReviewController* controller_ = nullptr;
    std::shared_ptr<Binding> binding_;
};

class DecoderBackendStateCache final {
public:
    void refresh(const std::weak_ptr<media::MultiSourceFrameProvider>& provider) noexcept;
    [[nodiscard]] std::vector<ui::ReviewController::DecoderBackendState> snapshot() const noexcept;

private:
    std::atomic<std::shared_ptr<const std::vector<ui::ReviewController::DecoderBackendState>>>
        states_;
};

} // namespace dvs::app::detail
