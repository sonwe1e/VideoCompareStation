#include "dvs/platform/GraphicsDeviceBroker.h"
#include "dvs/ui/GraphicsBackend.h"

#include "MinimalQuickRenderHarness.h"

#include <QGuiApplication>
#include <QQuickWindow>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <future>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <wrl/client.h>

namespace dvs::platform {
namespace {

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

TEST(GraphicsDeviceBrokerTests, BlockingWaitWakesForNotificationAndStop) {
    GraphicsDeviceBroker broker;
    std::stop_source notificationStop;
    std::future<std::optional<GraphicsDeviceNotification>> notification =
        std::async(std::launch::async, [&broker, token = notificationStop.get_token()] {
            return broker.waitForNotification(token);
        });
    EXPECT_EQ(notification.wait_for(20ms), std::future_status::timeout);
    ASSERT_EQ(broker.reportUnavailable("blocking-wait-test"),
              GraphicsDeviceBrokerResult::Unavailable);
    ASSERT_EQ(notification.wait_for(1s), std::future_status::ready);
    ASSERT_TRUE(notification.get().has_value());

    std::stop_source canceled;
    std::future<std::optional<GraphicsDeviceNotification>> stopped =
        std::async(std::launch::async, [&broker, token = canceled.get_token()] {
            return broker.waitForNotification(token);
        });
    EXPECT_EQ(stopped.wait_for(20ms), std::future_status::timeout);
    canceled.request_stop();
    ASSERT_EQ(stopped.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(stopped.get().has_value());
}

TEST(GraphicsDeviceBrokerTests, RetainsQtWarpDeviceWithProtectedImmediateContext) {
    GraphicsDeviceBroker broker;
    test::MinimalQuickRenderHarness harness{broker};

    ASSERT_TRUE(harness.showAndWait())
        << "exposed=" << harness.isExposed() << " unobtrusive=" << harness.isUnobtrusive()
        << " renders=" << harness.renderCount();
    ASSERT_TRUE(harness.isExposed());
    ASSERT_TRUE(harness.isUnobtrusive());
    ASSERT_EQ(harness.firstResult(), ui::GraphicsBackendResult::Ready);
    ASSERT_EQ(QQuickWindow::graphicsApi(), QSGRendererInterface::Direct3D11);

    std::optional<GraphicsDeviceNotification> notification = broker.tryConsumeNotification();
    ASSERT_TRUE(notification.has_value());
    const auto* const ready = std::get_if<application::GraphicsDeviceReady>(&*notification);
    ASSERT_NE(ready, nullptr);
    EXPECT_EQ(ready->context.deviceGeneration, domain::DeviceGeneration{1U});

    const GraphicsDeviceLeaseResult leaseResult = broker.tryLease();
    ASSERT_EQ(leaseResult.status, GraphicsDeviceLeaseStatus::Available);
    ASSERT_TRUE(leaseResult.lease.has_value());
    const GraphicsDeviceLease& lease = *leaseResult.lease;
    ASSERT_NE(lease.device.Get(), nullptr);
    ASSERT_NE(lease.immediateContext.Get(), nullptr);
    EXPECT_EQ(lease.deviceGeneration, domain::DeviceGeneration{1U});
    EXPECT_GE(lease.device->GetFeatureLevel(), D3D_FEATURE_LEVEL_10_0);

    ComPtr<ID3D11DeviceContext> immediateContext;
    lease.device->GetImmediateContext(immediateContext.GetAddressOf());
    ASSERT_NE(immediateContext.Get(), nullptr);
    ComPtr<IUnknown> expectedContextIdentity;
    ComPtr<IUnknown> retainedContextIdentity;
    ASSERT_TRUE(SUCCEEDED(immediateContext.As(&expectedContextIdentity)));
    ASSERT_TRUE(SUCCEEDED(lease.immediateContext.As(&retainedContextIdentity)));
    EXPECT_EQ(retainedContextIdentity.Get(), expectedContextIdentity.Get());

    ComPtr<ID3D10Multithread> multithread;
    ASSERT_TRUE(SUCCEEDED(lease.immediateContext.As(&multithread)));
    ASSERT_NE(multithread.Get(), nullptr);
    EXPECT_EQ(multithread->GetMultithreadProtected(), TRUE);

    ComPtr<IDXGIDevice> dxgiDevice;
    ASSERT_TRUE(SUCCEEDED(lease.device.As(&dxgiDevice)));
    ComPtr<IDXGIAdapter> adapter;
    ASSERT_TRUE(SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf())));
    ComPtr<IDXGIAdapter1> adapter1;
    ASSERT_TRUE(SUCCEEDED(adapter.As(&adapter1)));
    DXGI_ADAPTER_DESC1 description{};
    ASSERT_TRUE(SUCCEEDED(adapter1->GetDesc1(&description)));
    EXPECT_NE(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE, 0U);

    const std::uint64_t previousRenderCount = harness.renderCount();
    harness.requestRender();
    ASSERT_TRUE(harness.waitForRenderCount(previousRenderCount + 1U));
    EXPECT_EQ(harness.latestResult(), ui::GraphicsBackendResult::AlreadyReady);
    EXPECT_EQ(broker.currentGeneration(), domain::DeviceGeneration{1U});
    EXPECT_FALSE(broker.tryConsumeNotification().has_value());
}

TEST(GraphicsDeviceBrokerTests, PublishesMonotonicLossAndUnavailableTransitionsThenCloses) {
    GraphicsDeviceBroker broker;
    test::MinimalQuickRenderHarness harness{broker};

    ASSERT_TRUE(harness.showAndWait())
        << "exposed=" << harness.isExposed() << " unobtrusive=" << harness.isUnobtrusive()
        << " renders=" << harness.renderCount();
    ASSERT_EQ(harness.firstResult(), ui::GraphicsBackendResult::Ready);
    ASSERT_TRUE(broker.tryConsumeNotification().has_value());
    const GraphicsDeviceLeaseResult retainedLease = broker.tryLease();
    ASSERT_EQ(retainedLease.status, GraphicsDeviceLeaseStatus::Available);
    ASSERT_TRUE(retainedLease.lease.has_value());
    harness.hide();

    ASSERT_EQ(broker.reportDeviceLost(domain::DeviceGeneration{1U}, DXGI_ERROR_DEVICE_REMOVED),
              GraphicsDeviceBrokerResult::Lost);
    std::optional<GraphicsDeviceNotification> notification = broker.tryConsumeNotification();
    ASSERT_TRUE(notification.has_value());
    const auto* const lost = std::get_if<application::GraphicsDeviceLost>(&*notification);
    ASSERT_NE(lost, nullptr);
    EXPECT_EQ(lost->context.deviceGeneration, domain::DeviceGeneration{2U});
    EXPECT_EQ(lost->error.code, domain::MediaErrorCode::kGraphicsDeviceLost);
    EXPECT_NE(lost->error.technicalDetail.find("0x887A0005"), std::string::npos);
    EXPECT_EQ(broker.tryLease().status, GraphicsDeviceLeaseStatus::Unavailable);

    EXPECT_EQ(broker.reportUnavailable("The scene graph is no longer available."),
              GraphicsDeviceBrokerResult::AlreadyUnavailable);
    EXPECT_EQ(broker.adoptQtDevice(retainedLease.lease->device.Get(),
                                   retainedLease.lease->immediateContext.Get()),
              GraphicsDeviceBrokerResult::AlreadyUnavailable);
    EXPECT_EQ(broker.reportDeviceLost(domain::DeviceGeneration{2U}, DXGI_ERROR_DEVICE_RESET),
              GraphicsDeviceBrokerResult::AlreadyUnavailable);
    EXPECT_EQ(broker.currentGeneration(), domain::DeviceGeneration{2U});
    EXPECT_EQ(broker.tryLease().status, GraphicsDeviceLeaseStatus::Unavailable);
    EXPECT_FALSE(broker.tryConsumeNotification().has_value());

    broker.shutdown();
    EXPECT_TRUE(broker.isClosed());
    EXPECT_EQ(broker.tryLease().status, GraphicsDeviceLeaseStatus::Closed);
    EXPECT_EQ(broker.reportDeviceLost(domain::DeviceGeneration{2U}, DXGI_ERROR_DEVICE_REMOVED),
              GraphicsDeviceBrokerResult::Closed);
    EXPECT_EQ(broker.currentGeneration(), domain::DeviceGeneration{2U});
}

TEST(GraphicsDeviceBrokerTests, AdoptsARebuiltDeviceAfterALossButNotTheLostOne) {
    GraphicsDeviceBroker broker;
    test::MinimalQuickRenderHarness harness{broker};

    ASSERT_TRUE(harness.showAndWait());
    ASSERT_EQ(harness.firstResult(), ui::GraphicsBackendResult::Ready);
    ASSERT_TRUE(broker.tryConsumeNotification().has_value());
    const GraphicsDeviceLeaseResult lostLease = broker.tryLease();
    ASSERT_EQ(lostLease.status, GraphicsDeviceLeaseStatus::Available);
    ASSERT_TRUE(lostLease.lease.has_value());
    harness.hide();

    ASSERT_EQ(broker.reportDeviceLost(domain::DeviceGeneration{1U}, DXGI_ERROR_DEVICE_REMOVED),
              GraphicsDeviceBrokerResult::Lost);
    ASSERT_TRUE(broker.tryConsumeNotification().has_value());

    // The device that was just declared lost cannot be adopted again; otherwise a component could
    // re-adopt the very failure it reported.
    EXPECT_EQ(broker.adoptQtDevice(lostLease.lease->device.Get(),
                                   lostLease.lease->immediateContext.Get()),
              GraphicsDeviceBrokerResult::AlreadyUnavailable);

    // A device the renderer rebuilt is the recovery path: it advances the generation and publishes
    // Ready so every consumer resynchronizes instead of staying lost for the rest of the process.
    ComPtr<ID3D11Device> rebuilt;
    ComPtr<ID3D11DeviceContext> rebuiltContext;
    D3D_FEATURE_LEVEL featureLevel{};
    ASSERT_EQ(D3D11CreateDevice(nullptr,
                                D3D_DRIVER_TYPE_WARP,
                                nullptr,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                nullptr,
                                0U,
                                D3D11_SDK_VERSION,
                                rebuilt.GetAddressOf(),
                                &featureLevel,
                                rebuiltContext.GetAddressOf()),
              S_OK);
    ASSERT_EQ(broker.adoptQtDevice(rebuilt.Get(), rebuiltContext.Get()),
              GraphicsDeviceBrokerResult::Ready);
    EXPECT_EQ(broker.currentGeneration(), domain::DeviceGeneration{3U});

    std::optional<GraphicsDeviceNotification> notification = broker.tryConsumeNotification();
    ASSERT_TRUE(notification.has_value());
    const auto* const ready = std::get_if<application::GraphicsDeviceReady>(&*notification);
    ASSERT_NE(ready, nullptr);
    EXPECT_EQ(ready->context.deviceGeneration, domain::DeviceGeneration{3U});

    const GraphicsDeviceLeaseResult recoveredLease = broker.tryLease();
    ASSERT_EQ(recoveredLease.status, GraphicsDeviceLeaseStatus::Available);
    ASSERT_TRUE(recoveredLease.lease.has_value());
    EXPECT_EQ(recoveredLease.lease->deviceGeneration, domain::DeviceGeneration{3U});
}

TEST(GraphicsDeviceBrokerTests, CoalescesPressureWithoutVetoingDeviceLoss) {
    GraphicsDeviceBroker broker;
    test::MinimalQuickRenderHarness harness{broker};

    ASSERT_TRUE(harness.showAndWait())
        << "exposed=" << harness.isExposed() << " unobtrusive=" << harness.isUnobtrusive()
        << " renders=" << harness.renderCount();
    ASSERT_EQ(harness.firstResult(), ui::GraphicsBackendResult::Ready);
    const GraphicsDeviceLeaseResult retainedLease = broker.tryLease();
    ASSERT_EQ(retainedLease.status, GraphicsDeviceLeaseStatus::Available);
    ASSERT_TRUE(retainedLease.lease.has_value());
    harness.hide();

    std::uint64_t expectedGeneration = 1U;
    for (std::size_t index = 0U; index < 12U; ++index) {
        ASSERT_EQ(broker.reportUnavailable("synthetic queue pressure"),
                  GraphicsDeviceBrokerResult::Unavailable);
        ++expectedGeneration;
        ASSERT_EQ(broker.adoptQtDevice(retainedLease.lease->device.Get(),
                                       retainedLease.lease->immediateContext.Get()),
                  GraphicsDeviceBrokerResult::Ready);
        ++expectedGeneration;
    }

    ASSERT_EQ(broker.reportDeviceLost(domain::DeviceGeneration{expectedGeneration},
                                      DXGI_ERROR_DEVICE_REMOVED),
              GraphicsDeviceBrokerResult::Lost);
    ++expectedGeneration;
    EXPECT_EQ(broker.currentGeneration(), domain::DeviceGeneration{expectedGeneration});
    EXPECT_EQ(broker.tryLease().status, GraphicsDeviceLeaseStatus::Unavailable);

    std::optional<GraphicsDeviceNotification> lastNotification;
    std::size_t notificationCount = 0U;
    while (std::optional<GraphicsDeviceNotification> notification =
               broker.tryConsumeNotification()) {
        lastNotification = std::move(notification);
        ++notificationCount;
    }
    ASSERT_TRUE(lastNotification.has_value());
    const auto* const lost = std::get_if<application::GraphicsDeviceLost>(&*lastNotification);
    ASSERT_NE(lost, nullptr);
    EXPECT_EQ(lost->context.deviceGeneration, domain::DeviceGeneration{expectedGeneration});
    EXPECT_LE(notificationCount, 9U);
}

TEST(GraphicsDeviceBrokerTests, IgnoresLossReportedByAnObsoleteLeaseGeneration) {
    GraphicsDeviceBroker broker;
    test::MinimalQuickRenderHarness harness{broker};

    ASSERT_TRUE(harness.showAndWait())
        << "exposed=" << harness.isExposed() << " unobtrusive=" << harness.isUnobtrusive()
        << " renders=" << harness.renderCount();
    ASSERT_TRUE(broker.tryConsumeNotification().has_value());
    const GraphicsDeviceLeaseResult firstLease = broker.tryLease();
    ASSERT_EQ(firstLease.status, GraphicsDeviceLeaseStatus::Available);
    ASSERT_TRUE(firstLease.lease.has_value());
    harness.hide();

    ASSERT_EQ(broker.reportUnavailable("synthetic replacement"),
              GraphicsDeviceBrokerResult::Unavailable);
    ASSERT_TRUE(broker.tryConsumeNotification().has_value());
    ASSERT_EQ(broker.adoptQtDevice(firstLease.lease->device.Get(),
                                   firstLease.lease->immediateContext.Get()),
              GraphicsDeviceBrokerResult::Ready);
    ASSERT_TRUE(broker.tryConsumeNotification().has_value());
    ASSERT_EQ(broker.currentGeneration(), domain::DeviceGeneration{3U});

    EXPECT_EQ(broker.reportDeviceLost(domain::DeviceGeneration{1U}, DXGI_ERROR_DEVICE_REMOVED),
              GraphicsDeviceBrokerResult::StaleGeneration);
    EXPECT_EQ(broker.currentGeneration(), domain::DeviceGeneration{3U});
    EXPECT_EQ(broker.tryLease().status, GraphicsDeviceLeaseStatus::Available);
    EXPECT_FALSE(broker.tryConsumeNotification().has_value());

    ASSERT_EQ(broker.reportDeviceLost(domain::DeviceGeneration{3U}, DXGI_ERROR_DEVICE_REMOVED),
              GraphicsDeviceBrokerResult::Lost);
    const std::optional<GraphicsDeviceNotification> notification = broker.tryConsumeNotification();
    ASSERT_TRUE(notification.has_value());
    const auto* const lost = std::get_if<application::GraphicsDeviceLost>(&*notification);
    ASSERT_NE(lost, nullptr);
    EXPECT_EQ(lost->context.deviceGeneration, domain::DeviceGeneration{4U});
}

} // namespace
} // namespace dvs::platform

int main(int argc, char* argv[]) {
    dvs::ui::configureGraphicsBackend();
    QGuiApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
