#include "dvs/platform/GraphicsDeviceBroker.h"

#include "dvs/domain/MediaError.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <d3d10.h>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <utility>
#include <wrl/client.h>

namespace dvs::platform {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kNotificationCapacity = 8U;

enum class DeviceState {
    Initial,
    Ready,
    Unavailable,
    Lost,
    Closed,
};

struct ValidatedDevice final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediateContext;
    ComPtr<IUnknown> deviceIdentity;
};

[[nodiscard]] std::string hresultText(const HRESULT result) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8)
           << static_cast<std::uint32_t>(result);
    return stream.str();
}

template <typename Interface>
[[nodiscard]] bool sameComIdentity(const ComPtr<Interface>& left,
                                   const ComPtr<Interface>& right) noexcept {
    ComPtr<IUnknown> leftIdentity;
    ComPtr<IUnknown> rightIdentity;
    return SUCCEEDED(left.As(&leftIdentity)) && SUCCEEDED(right.As(&rightIdentity)) &&
           leftIdentity.Get() == rightIdentity.Get();
}

[[nodiscard]] std::optional<ValidatedDevice>
validateDevice(ID3D11Device* const rawDevice,
               ID3D11DeviceContext* const rawImmediateContext,
               std::string& technicalDetail,
               std::optional<HRESULT>& removalReason) noexcept {
    if (rawDevice == nullptr || rawImmediateContext == nullptr) {
        technicalDetail = "Qt Quick did not provide both D3D11 device resources.";
        return std::nullopt;
    }

    // Constructing ComPtr from a raw interface calls AddRef. Attach is deliberately not used:
    // Qt retains its own ownership of both interfaces.
    ComPtr<ID3D11Device> device{rawDevice};
    ComPtr<ID3D11DeviceContext> suppliedContext{rawImmediateContext};

    const HRESULT deviceRemovalReason = device->GetDeviceRemovedReason();
    if (FAILED(deviceRemovalReason)) {
        removalReason = deviceRemovalReason;
        technicalDetail =
            "Qt Quick's D3D11 device was already removed: " + hresultText(deviceRemovalReason) +
            '.';
        return std::nullopt;
    }
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_10_0) {
        technicalDetail =
            "Qt Quick's D3D11 device does not support the required feature level 10.0.";
        return std::nullopt;
    }

    ComPtr<ID3D11DeviceContext> actualImmediateContext;
    device->GetImmediateContext(actualImmediateContext.GetAddressOf());
    if (!actualImmediateContext || !sameComIdentity(suppliedContext, actualImmediateContext)) {
        technicalDetail = "Qt Quick's DeviceContextResource is not the device immediate context.";
        return std::nullopt;
    }

    ComPtr<ID3D11Device> contextDevice;
    suppliedContext->GetDevice(contextDevice.GetAddressOf());
    if (!contextDevice || !sameComIdentity(device, contextDevice)) {
        technicalDetail = "Qt Quick's device and context resources have different COM owners.";
        return std::nullopt;
    }

    ComPtr<ID3D10Multithread> multithread;
    const HRESULT multithreadResult = suppliedContext.As(&multithread);
    if (FAILED(multithreadResult) || !multithread) {
        technicalDetail = "ID3D11DeviceContext::QueryInterface(ID3D10Multithread) failed: " +
                          hresultText(multithreadResult) + '.';
        return std::nullopt;
    }
    static_cast<void>(multithread->SetMultithreadProtected(TRUE));
    if (multithread->GetMultithreadProtected() != TRUE) {
        technicalDetail = "D3D11 multithread protection did not remain enabled.";
        return std::nullopt;
    }

    ComPtr<IUnknown> deviceIdentity;
    const HRESULT identityResult = device.As(&deviceIdentity);
    if (FAILED(identityResult) || !deviceIdentity) {
        technicalDetail =
            "The D3D11 device has no canonical IUnknown identity: " + hresultText(identityResult) +
            '.';
        return std::nullopt;
    }

    return ValidatedDevice{
        .device = std::move(device),
        .immediateContext = std::move(suppliedContext),
        .deviceIdentity = std::move(deviceIdentity),
    };
}

[[nodiscard]] domain::MediaError unavailableError(std::string technicalDetail) {
    return domain::makeMediaError(domain::MediaErrorCode::kGraphicsUnavailable,
                                  domain::MediaOperation::kGraphicsInitialization,
                                  std::nullopt,
                                  true,
                                  std::move(technicalDetail));
}

[[nodiscard]] domain::MediaError lostError(const HRESULT reason) {
    return domain::makeMediaError(domain::MediaErrorCode::kGraphicsDeviceLost,
                                  domain::MediaOperation::kGraphicsInitialization,
                                  std::nullopt,
                                  true,
                                  "ID3D11Device::GetDeviceRemovedReason reported " +
                                      hresultText(reason) + '.');
}

} // namespace

class GraphicsDeviceBroker::Impl final {
public:
    [[nodiscard]] GraphicsDeviceBrokerResult
    adoptQtDevice(ID3D11Device* const rawDevice,
                  ID3D11DeviceContext* const rawImmediateContext) noexcept {
        domain::DeviceGeneration observedGeneration{0U};
        {
            std::unique_lock lock{stateMutex_, std::try_to_lock};
            if (!lock.owns_lock()) {
                return GraphicsDeviceBrokerResult::Busy;
            }
            if (state_ == DeviceState::Closed) {
                return GraphicsDeviceBrokerResult::Closed;
            }
            if (state_ == DeviceState::Lost) {
                return GraphicsDeviceBrokerResult::AlreadyUnavailable;
            }
            observedGeneration = domain::DeviceGeneration{generation_};
            if (state_ == DeviceState::Ready && device_.Get() == rawDevice &&
                immediateContext_.Get() == rawImmediateContext) {
                const HRESULT removalReason = rawDevice->GetDeviceRemovedReason();
                if (SUCCEEDED(removalReason)) {
                    return GraphicsDeviceBrokerResult::AlreadyReady;
                }
                lock.unlock();
                return reportDeviceLost(observedGeneration, removalReason);
            }
        }

        std::string technicalDetail;
        std::optional<HRESULT> removalReason;
        std::optional<ValidatedDevice> validated =
            validateDevice(rawDevice, rawImmediateContext, technicalDetail, removalReason);
        if (!validated.has_value()) {
            if (removalReason.has_value()) {
                return reportObservedDeviceLost(observedGeneration, *removalReason);
            }
            return reportUnavailable(std::move(technicalDetail));
        }

        std::unique_lock lock{stateMutex_, std::try_to_lock};
        if (!lock.owns_lock()) {
            return GraphicsDeviceBrokerResult::Busy;
        }
        if (state_ == DeviceState::Closed) {
            return GraphicsDeviceBrokerResult::Closed;
        }
        if (state_ == DeviceState::Lost) {
            return GraphicsDeviceBrokerResult::AlreadyUnavailable;
        }
        if (state_ == DeviceState::Ready &&
            deviceIdentity_.Get() == validated->deviceIdentity.Get()) {
            return GraphicsDeviceBrokerResult::AlreadyReady;
        }

        const std::uint64_t nextGeneration = generation_ + 1U;
        const application::GraphicsDeviceReady ready{
            .context =
                application::GraphicsEventContext{
                    .deviceGeneration = domain::DeviceGeneration{nextGeneration},
                },
        };
        generation_ = nextGeneration;
        state_ = DeviceState::Ready;
        device_ = std::move(validated->device);
        immediateContext_ = std::move(validated->immediateContext);
        deviceIdentity_ = std::move(validated->deviceIdentity);
        publishedGeneration_.store(nextGeneration, std::memory_order_release);
        publishNotification(GraphicsDeviceNotification{ready});
        return GraphicsDeviceBrokerResult::Ready;
    }

    [[nodiscard]] GraphicsDeviceBrokerResult
    reportUnavailable(std::string technicalDetail) noexcept {
        std::unique_lock lock{stateMutex_, std::try_to_lock};
        if (!lock.owns_lock()) {
            return GraphicsDeviceBrokerResult::Busy;
        }
        if (state_ == DeviceState::Closed) {
            return GraphicsDeviceBrokerResult::Closed;
        }
        if (state_ == DeviceState::Lost) {
            return GraphicsDeviceBrokerResult::AlreadyUnavailable;
        }
        if (state_ == DeviceState::Unavailable) {
            return GraphicsDeviceBrokerResult::AlreadyUnavailable;
        }

        const std::uint64_t nextGeneration = generation_ + 1U;
        const application::GraphicsDeviceUnavailable unavailable{
            .context =
                application::GraphicsEventContext{
                    .deviceGeneration = domain::DeviceGeneration{nextGeneration},
                },
            .error = unavailableError(std::move(technicalDetail)),
        };
        clearDevice();
        generation_ = nextGeneration;
        state_ = DeviceState::Unavailable;
        publishedGeneration_.store(nextGeneration, std::memory_order_release);
        publishNotification(GraphicsDeviceNotification{unavailable});
        return GraphicsDeviceBrokerResult::Unavailable;
    }

    [[nodiscard]] GraphicsDeviceBrokerResult
    reportDeviceLost(const domain::DeviceGeneration expectedGeneration,
                     const HRESULT reason) noexcept {
        std::unique_lock lock{stateMutex_, std::try_to_lock};
        if (!lock.owns_lock()) {
            return GraphicsDeviceBrokerResult::Busy;
        }
        if (state_ == DeviceState::Closed) {
            return GraphicsDeviceBrokerResult::Closed;
        }
        if (state_ == DeviceState::Lost) {
            return GraphicsDeviceBrokerResult::AlreadyUnavailable;
        }
        if (state_ != DeviceState::Ready ||
            domain::DeviceGeneration{generation_} != expectedGeneration) {
            return GraphicsDeviceBrokerResult::StaleGeneration;
        }

        return commitLossLocked(reason);
    }

    [[nodiscard]] GraphicsDeviceLeaseResult tryLease() const noexcept {
        std::unique_lock lock{stateMutex_, std::try_to_lock};
        if (!lock.owns_lock()) {
            return GraphicsDeviceLeaseResult{.status = GraphicsDeviceLeaseStatus::Busy};
        }
        if (state_ == DeviceState::Closed) {
            return GraphicsDeviceLeaseResult{.status = GraphicsDeviceLeaseStatus::Closed};
        }
        if (state_ != DeviceState::Ready || !device_ || !immediateContext_) {
            return GraphicsDeviceLeaseResult{.status = GraphicsDeviceLeaseStatus::Unavailable};
        }
        return GraphicsDeviceLeaseResult{
            .status = GraphicsDeviceLeaseStatus::Available,
            .lease =
                GraphicsDeviceLease{
                    .deviceGeneration = domain::DeviceGeneration{generation_},
                    .device = device_,
                    .immediateContext = immediateContext_,
                },
        };
    }

    [[nodiscard]] std::optional<GraphicsDeviceNotification> tryConsumeNotification() noexcept {
        return consumeNotification(false);
    }

    [[nodiscard]] std::optional<GraphicsDeviceNotification>
    waitForNotification(const std::stop_token stopToken) noexcept {
        while (!stopToken.stop_requested()) {
            const std::uint64_t observed = notificationSequence_.load(std::memory_order_acquire);
            if (std::optional<GraphicsDeviceNotification> notification =
                    consumeNotification(true)) {
                return notification;
            }
            if (notificationClosed_.load(std::memory_order_acquire)) {
                return std::nullopt;
            }
            std::unique_lock lock{notificationWaitMutex_};
            static_cast<void>(notificationAvailable_.wait(lock, stopToken, [this, observed] {
                return notificationClosed_.load(std::memory_order_acquire) ||
                       notificationSequence_.load(std::memory_order_acquire) != observed;
            }));
        }
        return std::nullopt;
    }

    [[nodiscard]] domain::DeviceGeneration currentGeneration() const noexcept {
        return domain::DeviceGeneration{publishedGeneration_.load(std::memory_order_acquire)};
    }

    void shutdown() noexcept {
        std::lock_guard lock{stateMutex_};
        if (state_ == DeviceState::Closed) {
            return;
        }
        clearDevice();
        state_ = DeviceState::Closed;
        notificationClosed_.store(true, std::memory_order_release);
        notificationSequence_.fetch_add(1U, std::memory_order_release);
        notificationAvailable_.notify_all();
    }

    [[nodiscard]] bool isClosed() const noexcept {
        std::unique_lock lock{stateMutex_, std::try_to_lock};
        return lock.owns_lock() && state_ == DeviceState::Closed;
    }

private:
    [[nodiscard]] std::optional<GraphicsDeviceNotification>
    consumeNotification(const bool blockForOverflow) noexcept {
        bool expected = false;
        if (!notificationConsumerActive_.compare_exchange_strong(
                expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
            return std::nullopt;
        }
        struct ConsumerGuard final {
            std::atomic<bool>& active;

            ~ConsumerGuard() {
                active.store(false, std::memory_order_release);
            }
        } guard{notificationConsumerActive_};

        const std::uint64_t read = notificationRead_.load(std::memory_order_relaxed);
        if (read != notificationWrite_.load(std::memory_order_acquire)) {
            const std::size_t index = static_cast<std::size_t>(read % kNotificationCapacity);
            std::optional<GraphicsDeviceNotification> notification =
                std::move(notifications_[index]);
            notifications_[index].reset();
            notificationRead_.store(read + 1U, std::memory_order_release);
            return notification;
        }

        std::unique_lock lock{stateMutex_, std::defer_lock};
        if (blockForOverflow) {
            lock.lock();
        } else if (!lock.try_lock()) {
            return std::nullopt;
        }
        if (!overflowNotification_.has_value()) {
            return std::nullopt;
        }
        std::optional<GraphicsDeviceNotification> notification = std::move(overflowNotification_);
        overflowNotification_.reset();
        return notification;
    }

    [[nodiscard]] GraphicsDeviceBrokerResult
    reportObservedDeviceLost(const domain::DeviceGeneration observedGeneration,
                             const HRESULT reason) noexcept {
        std::unique_lock lock{stateMutex_, std::try_to_lock};
        if (!lock.owns_lock()) {
            return GraphicsDeviceBrokerResult::Busy;
        }
        if (state_ == DeviceState::Closed) {
            return GraphicsDeviceBrokerResult::Closed;
        }
        if (state_ == DeviceState::Lost) {
            return GraphicsDeviceBrokerResult::AlreadyUnavailable;
        }
        if (domain::DeviceGeneration{generation_} != observedGeneration) {
            return GraphicsDeviceBrokerResult::StaleGeneration;
        }
        return commitLossLocked(reason);
    }

    [[nodiscard]] GraphicsDeviceBrokerResult commitLossLocked(const HRESULT reason) noexcept {
        const std::uint64_t nextGeneration = generation_ + 1U;
        const application::GraphicsDeviceLost lost{
            .context =
                application::GraphicsEventContext{
                    .deviceGeneration = domain::DeviceGeneration{nextGeneration},
                },
            .error = lostError(reason),
        };
        clearDevice();
        generation_ = nextGeneration;
        state_ = DeviceState::Lost;
        publishedGeneration_.store(nextGeneration, std::memory_order_release);
        publishNotification(GraphicsDeviceNotification{lost});
        return GraphicsDeviceBrokerResult::Lost;
    }

    void publishNotification(GraphicsDeviceNotification notification) noexcept {
        // Once pressure starts, keep coalescing the authoritative latest state until every older
        // ring entry is drained. Notification pressure must never veto device invalidation.
        if (overflowNotification_.has_value()) {
            overflowNotification_.emplace(std::move(notification));
            notificationSequence_.fetch_add(1U, std::memory_order_release);
            notificationAvailable_.notify_one();
            return;
        }

        const std::uint64_t write = notificationWrite_.load(std::memory_order_relaxed);
        if (write - notificationRead_.load(std::memory_order_acquire) >= kNotificationCapacity) {
            overflowNotification_.emplace(std::move(notification));
            notificationSequence_.fetch_add(1U, std::memory_order_release);
            notificationAvailable_.notify_one();
            return;
        }

        const std::size_t index = static_cast<std::size_t>(write % kNotificationCapacity);
        notifications_[index].emplace(std::move(notification));
        notificationWrite_.store(write + 1U, std::memory_order_release);
        notificationSequence_.fetch_add(1U, std::memory_order_release);
        notificationAvailable_.notify_one();
    }

    void clearDevice() noexcept {
        immediateContext_.Reset();
        deviceIdentity_.Reset();
        device_.Reset();
    }

    mutable std::mutex stateMutex_;
    DeviceState state_ = DeviceState::Initial;
    std::uint64_t generation_ = 0U;
    std::atomic<std::uint64_t> publishedGeneration_{0U};
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> immediateContext_;
    ComPtr<IUnknown> deviceIdentity_;

    std::array<std::optional<GraphicsDeviceNotification>, kNotificationCapacity> notifications_{};
    std::optional<GraphicsDeviceNotification> overflowNotification_;
    std::atomic<std::uint64_t> notificationRead_{0U};
    std::atomic<std::uint64_t> notificationWrite_{0U};
    std::atomic<bool> notificationConsumerActive_{false};
    std::atomic<std::uint64_t> notificationSequence_{0U};
    std::atomic<bool> notificationClosed_{false};
    std::mutex notificationWaitMutex_;
    std::condition_variable_any notificationAvailable_;
};

GraphicsDeviceBroker::GraphicsDeviceBroker() : impl_(std::make_unique<Impl>()) {}

GraphicsDeviceBroker::~GraphicsDeviceBroker() {
    shutdown();
}

GraphicsDeviceBrokerResult
GraphicsDeviceBroker::adoptQtDevice(ID3D11Device* const device,
                                    ID3D11DeviceContext* const immediateContext) noexcept {
    return impl_->adoptQtDevice(device, immediateContext);
}

GraphicsDeviceBrokerResult
GraphicsDeviceBroker::reportUnavailable(std::string technicalDetail) noexcept {
    return impl_->reportUnavailable(std::move(technicalDetail));
}

GraphicsDeviceBrokerResult
GraphicsDeviceBroker::reportDeviceLost(const domain::DeviceGeneration expectedGeneration,
                                       const HRESULT reason) noexcept {
    return impl_->reportDeviceLost(expectedGeneration, reason);
}

GraphicsDeviceLeaseResult GraphicsDeviceBroker::tryLease() const noexcept {
    return impl_->tryLease();
}

std::optional<GraphicsDeviceNotification> GraphicsDeviceBroker::tryConsumeNotification() noexcept {
    return impl_->tryConsumeNotification();
}

std::optional<GraphicsDeviceNotification>
GraphicsDeviceBroker::waitForNotification(const std::stop_token stopToken) noexcept {
    return impl_->waitForNotification(stopToken);
}

domain::DeviceGeneration GraphicsDeviceBroker::currentGeneration() const noexcept {
    return impl_->currentGeneration();
}

void GraphicsDeviceBroker::shutdown() noexcept {
    impl_->shutdown();
}

bool GraphicsDeviceBroker::isClosed() const noexcept {
    return impl_->isClosed();
}

} // namespace dvs::platform
