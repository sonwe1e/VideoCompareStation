#include "dvs/ui/GraphicsBackend.h"

#include "dvs/platform/GraphicsDeviceBroker.h"

#include <QByteArray>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include <d3d11.h>
#include <string>

namespace dvs::ui {
namespace {

[[nodiscard]] GraphicsBackendResult
mapBrokerResult(const platform::GraphicsDeviceBrokerResult result) noexcept {
    switch (result) {
    case platform::GraphicsDeviceBrokerResult::Ready:
        return GraphicsBackendResult::Ready;
    case platform::GraphicsDeviceBrokerResult::AlreadyReady:
        return GraphicsBackendResult::AlreadyReady;
    case platform::GraphicsDeviceBrokerResult::Busy:
        return GraphicsBackendResult::BrokerBusy;
    case platform::GraphicsDeviceBrokerResult::StaleGeneration:
        return GraphicsBackendResult::DeviceRejected;
    case platform::GraphicsDeviceBrokerResult::NotificationQueueFull:
        return GraphicsBackendResult::NotificationQueueFull;
    case platform::GraphicsDeviceBrokerResult::Closed:
        return GraphicsBackendResult::Closed;
    case platform::GraphicsDeviceBrokerResult::Unavailable:
    case platform::GraphicsDeviceBrokerResult::AlreadyUnavailable:
    case platform::GraphicsDeviceBrokerResult::Lost:
        return GraphicsBackendResult::DeviceRejected;
    }
    return GraphicsBackendResult::DeviceRejected;
}

[[nodiscard]] GraphicsBackendResult publishUnavailable(platform::GraphicsDeviceBroker& broker,
                                                       std::string technicalDetail,
                                                       const GraphicsBackendResult backendFailure) {
    const platform::GraphicsDeviceBrokerResult result =
        broker.reportUnavailable(std::move(technicalDetail));
    if (result == platform::GraphicsDeviceBrokerResult::Unavailable ||
        result == platform::GraphicsDeviceBrokerResult::AlreadyUnavailable) {
        return backendFailure;
    }
    return mapBrokerResult(result);
}

} // namespace

void configureGraphicsBackend() noexcept {
    // Qt's threaded render loop can inherit the refresh cadence of an unrelated Windows virtual
    // display. On mixed physical/virtual-adapter systems that delayed an explicitly requested
    // scene-graph update by roughly four 60 Hz intervals, even though the D3D11 render itself took
    // only microseconds. This application owns canonical media cadence and requests updates only
    // when a complete FrameSet is ready, so display-loop throttling must not pace frame admission.
    if (!qEnvironmentVariableIsSet("QSG_NO_VSYNC")) {
        static_cast<void>(qputenv("QSG_NO_VSYNC", QByteArrayLiteral("1")));
    }
    // QWindow otherwise keeps a small idle delay before UpdateRequest delivery. Media cadence
    // already provides the idle boundary, and the extra GUI-thread delay is large enough to miss
    // 120 fps frame deadlines even on a high-refresh physical display.
    if (!qEnvironmentVariableIsSet("QT_QPA_UPDATE_IDLE_TIME")) {
        static_cast<void>(qputenv("QT_QPA_UPDATE_IDLE_TIME", QByteArrayLiteral("0")));
    }
    // Main.qml customizes control backgrounds. The Windows native style rejects those
    // customizations and retries style images on every rendered frame, so the application ships
    // the lightweight Basic style and pins it: leaving QT_QUICK_CONTROLS_STYLE overridable would
    // let a caller ask for a style the package deliberately no longer contains.
    static_cast<void>(qputenv("QT_QUICK_CONTROLS_STYLE", QByteArrayLiteral("Basic")));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
}

GraphicsBackendResult
bindGraphicsBackendOnRenderThread(QQuickWindow& window,
                                  platform::GraphicsDeviceBroker& broker) noexcept {
    QSGRendererInterface* const renderer = window.rendererInterface();
    if (renderer == nullptr || renderer->graphicsApi() != QSGRendererInterface::Direct3D11) {
        return publishUnavailable(broker,
                                  "Qt Quick did not initialize the Direct3D 11 renderer.",
                                  GraphicsBackendResult::UnsupportedGraphicsApi);
    }

    auto* const device = static_cast<ID3D11Device*>(
        renderer->getResource(&window, QSGRendererInterface::DeviceResource));
    auto* const immediateContext = static_cast<ID3D11DeviceContext*>(
        renderer->getResource(&window, QSGRendererInterface::DeviceContextResource));
    if (device == nullptr || immediateContext == nullptr) {
        return publishUnavailable(broker,
                                  "Qt Quick exposed incomplete Direct3D 11 native resources.",
                                  GraphicsBackendResult::MissingNativeResource);
    }

    return mapBrokerResult(broker.adoptQtDevice(device, immediateContext));
}

} // namespace dvs::ui
