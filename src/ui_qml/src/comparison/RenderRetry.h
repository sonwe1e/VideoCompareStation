#pragma once

#include "dvs/platform/D3d11ComparisonRenderer.h"
#include "dvs/ui/GraphicsBackend.h"

#include <QMetaObject>
#include <QQuickWindow>

#include <atomic>
#include <memory>

namespace dvs::ui::detail {

class RenderRetry final {
public:
    void retryContendedRender(QQuickWindow& window, const platform::ComparisonRenderResult result) {
        if (result == platform::ComparisonRenderResult::Contended) {
            schedule(window);
        }
    }

    void retryContendedRender(QQuickWindow& window, const GraphicsBackendResult result) {
        if (result == GraphicsBackendResult::BrokerBusy) {
            schedule(window);
        }
    }

private:
    void schedule(QQuickWindow& window) {
        if (pending_->exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        // Both contention paths consumed the original update. Coalesce them until the GUI
        // receives it. Capture only shared state: the render node can be destroyed first.
        QMetaObject::invokeMethod(
            &window,
            [pending = pending_, target = &window] {
                pending->store(false, std::memory_order_release);
                target->update();
            },
            Qt::QueuedConnection);
    }

    std::shared_ptr<std::atomic<bool>> pending_ = std::make_shared<std::atomic<bool>>(false);
};

} // namespace dvs::ui::detail
