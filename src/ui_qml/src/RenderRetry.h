#pragma once

#include "dvs/platform/D3d11ComparisonRenderer.h"

#include <QMetaObject>
#include <QQuickWindow>

namespace dvs::ui::detail {

inline void retryContendedRender(QQuickWindow& window,
                                 const platform::ComparisonRenderResult result) {
    if (result == platform::ComparisonRenderResult::Contended) {
        // The publication remains pending, but its original update has already been consumed.
        QMetaObject::invokeMethod(&window, &QQuickWindow::update, Qt::QueuedConnection);
    }
}

} // namespace dvs::ui::detail
