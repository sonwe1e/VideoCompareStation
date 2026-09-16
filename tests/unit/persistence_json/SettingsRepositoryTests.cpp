#include "dvs/persistence/SettingsRepository.h"
#include "dvs/test/ScopedTemporaryDirectory.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace dvs::persistence {
namespace {

[[nodiscard]] application::RequestContext requestContext(const std::uint64_t requestId) {
    return application::RequestContext{
        .sessionId = domain::SessionId{1U},
        .sessionEpoch = domain::SessionEpoch{2U},
        .requestId = domain::RequestId{requestId},
    };
}

[[nodiscard]] bool isTerminalFor(const application::ApplicationEvent& event,
                                 const application::RequestContext& expected) {
    const auto* terminal = std::get_if<application::RequestTerminal>(&event);
    if (terminal == nullptr) {
        return false;
    }
    return std::visit(
        [&expected](const auto& outcome) {
            const auto* context = std::get_if<application::RequestContext>(&outcome.context);
            return context != nullptr && *context == expected;
        },
        *terminal);
}

class RecordingEventSink final : public application::IApplicationEventSink {
public:
    [[nodiscard]] application::EventPostResult
    postCritical(application::ApplicationEvent event) noexcept override {
        {
            std::scoped_lock lock(mutex_);
            if (criticalIngressClosed_) {
                return application::EventPostResult::Closed;
            }
            try {
                criticalEvents_.push_back(std::move(event));
            } catch (...) {
                return application::EventPostResult::Closed;
            }
        }
        condition_.notify_all();
        return application::EventPostResult::Accepted;
    }

    [[nodiscard]] application::EventPostResult
    postRealtime(application::ApplicationEvent event) noexcept override {
        static_cast<void>(event);
        std::scoped_lock lock(mutex_);
        if (realtimeIngressClosed_) {
            return application::EventPostResult::Closed;
        }
        ++realtimePostCount_;
        return application::EventPostResult::Accepted;
    }

    void closeRealtimeIngress() noexcept override {
        std::scoped_lock lock(mutex_);
        realtimeIngressClosed_ = true;
    }

    void closeCriticalIngress() noexcept override {
        std::scoped_lock lock(mutex_);
        criticalIngressClosed_ = true;
    }

    [[nodiscard]] bool waitForTerminal(const application::RequestContext& context,
                                       const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this, &context] {
            for (const application::ApplicationEvent& event : criticalEvents_) {
                if (isTerminalFor(event, context)) {
                    return true;
                }
            }
            return false;
        });
    }

    [[nodiscard]] std::vector<application::ApplicationEvent> criticalEvents() const {
        std::scoped_lock lock(mutex_);
        return criticalEvents_;
    }

    [[nodiscard]] std::size_t realtimePostCount() const {
        std::scoped_lock lock(mutex_);
        return realtimePostCount_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<application::ApplicationEvent> criticalEvents_;
    std::size_t realtimePostCount_ = 0U;
    bool realtimeIngressClosed_ = false;
    bool criticalIngressClosed_ = false;
};

class BlockingEventSink final : public application::IApplicationEventSink {
public:
    [[nodiscard]] application::EventPostResult
    postCritical(application::ApplicationEvent event) noexcept override {
        static_cast<void>(event);
        std::unique_lock lock(mutex_);
        postEntered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
        return application::EventPostResult::Accepted;
    }

    [[nodiscard]] application::EventPostResult
    postRealtime(application::ApplicationEvent event) noexcept override {
        static_cast<void>(event);
        return application::EventPostResult::Accepted;
    }

    void closeRealtimeIngress() noexcept override {}
    void closeCriticalIngress() noexcept override {}

    [[nodiscard]] bool waitForPost(const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return postEntered_; });
    }

    void release() noexcept {
        {
            std::scoped_lock lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool postEntered_ = false;
    bool released_ = false;
};

class ExpiringEventSink final : public application::IApplicationEventSink {
public:
    ExpiringEventSink(std::atomic<std::size_t>& postCalls,
                      std::atomic<std::size_t>& destructions) noexcept
        : postCalls_(postCalls), destructions_(destructions) {}

    ~ExpiringEventSink() override {
        destructions_.fetch_add(1U, std::memory_order_relaxed);
    }

    [[nodiscard]] application::EventPostResult
    postCritical(application::ApplicationEvent event) noexcept override {
        static_cast<void>(event);
        postCalls_.fetch_add(1U, std::memory_order_relaxed);
        return application::EventPostResult::Accepted;
    }

    [[nodiscard]] application::EventPostResult
    postRealtime(application::ApplicationEvent event) noexcept override {
        static_cast<void>(event);
        return application::EventPostResult::Accepted;
    }

    void closeRealtimeIngress() noexcept override {}
    void closeCriticalIngress() noexcept override {}

private:
    std::atomic<std::size_t>& postCalls_;
    std::atomic<std::size_t>& destructions_;
};

class SettingsRepositoryTests : public ::testing::Test {
protected:
    [[nodiscard]] const std::filesystem::path& settingsFile() const noexcept {
        return settingsFile_;
    }

private:
    dvs::test::ScopedTemporaryDirectory directory_{"dvs-settings"};
    std::filesystem::path settingsFile_{directory_.path() / "nested" / "settings.json"};
};

TEST_F(SettingsRepositoryTests, SavesAndLoadsSettingsWithPayloadBeforeTerminal) {
    const auto saveEvents = std::make_shared<RecordingEventSink>();
    const auto loadEvents = std::make_shared<RecordingEventSink>();
    SettingsRepository repository(settingsFile());
    application::SettingsSnapshot settings;
    settings.values.emplace("theme", "dark");
    settings.values.emplace("zoom", "125");
    const application::SettingsSaveRequest saveRequest{
        .context = requestContext(3U),
        .settings = settings,
    };

    EXPECT_EQ(repository.submit(saveRequest, saveEvents), application::PortSubmitResult::Accepted);
    ASSERT_TRUE(saveEvents->waitForTerminal(saveRequest.context, std::chrono::seconds{2}));
    const auto saveCriticalEvents = saveEvents->criticalEvents();
    ASSERT_EQ(saveCriticalEvents.size(), 1U);
    const auto* saveTerminal =
        std::get_if<application::RequestTerminal>(&saveCriticalEvents.front());
    ASSERT_NE(saveTerminal, nullptr);
    const auto* saveSucceeded = std::get_if<application::RequestSucceeded>(saveTerminal);
    ASSERT_NE(saveSucceeded, nullptr);
    const auto* saveContext = std::get_if<application::RequestContext>(&saveSucceeded->context);
    ASSERT_NE(saveContext, nullptr);
    EXPECT_EQ(*saveContext, saveRequest.context);
    EXPECT_TRUE(std::filesystem::exists(settingsFile()));
    EXPECT_EQ(saveEvents->realtimePostCount(), 0U);

    const application::SettingsLoadRequest loadRequest{
        .context = requestContext(4U),
    };
    EXPECT_EQ(repository.submit(loadRequest, loadEvents), application::PortSubmitResult::Accepted);
    ASSERT_TRUE(loadEvents->waitForTerminal(loadRequest.context, std::chrono::seconds{2}));
    const auto loadCriticalEvents = loadEvents->criticalEvents();
    ASSERT_EQ(loadCriticalEvents.size(), 2U);
    const auto* loaded = std::get_if<application::SettingsLoaded>(&loadCriticalEvents[0]);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->context, loadRequest.context);
    EXPECT_EQ(loaded->settings.values, settings.values);
    const auto* loadTerminal = std::get_if<application::RequestTerminal>(&loadCriticalEvents[1]);
    ASSERT_NE(loadTerminal, nullptr);
    const auto* loadSucceeded = std::get_if<application::RequestSucceeded>(loadTerminal);
    ASSERT_NE(loadSucceeded, nullptr);
    const auto* loadContext = std::get_if<application::RequestContext>(&loadSucceeded->context);
    ASSERT_NE(loadContext, nullptr);
    EXPECT_EQ(*loadContext, loadRequest.context);
    EXPECT_EQ(loadEvents->realtimePostCount(), 0U);
}

TEST_F(SettingsRepositoryTests, DropsQueuedCompletionWhenEventSinkIsDestroyed) {
    std::atomic<std::size_t> postCalls{0U};
    std::atomic<std::size_t> destructions{0U};

    {
        SettingsRepository repository(settingsFile());
        const auto blockingEvents = std::make_shared<BlockingEventSink>();
        const application::SettingsLoadRequest blockingRequest{
            .context = requestContext(60U),
        };
        ASSERT_EQ(repository.submit(blockingRequest, blockingEvents),
                  application::PortSubmitResult::Accepted);
        if (!blockingEvents->waitForPost(std::chrono::seconds{2})) {
            blockingEvents->release();
            FAIL() << "The blocking repository operation did not reach event publication.";
        }

        auto expiringEvents = std::make_shared<ExpiringEventSink>(postCalls, destructions);
        const std::weak_ptr<application::IApplicationEventSink> weakEvents{expiringEvents};
        const application::SettingsLoadRequest queuedRequest{
            .context = requestContext(61U),
        };
        const auto queuedResult = repository.submit(queuedRequest, expiringEvents);
        if (queuedResult != application::PortSubmitResult::Accepted) {
            blockingEvents->release();
            FAIL() << "The queued repository operation was not accepted.";
        }

        expiringEvents.reset();
        EXPECT_TRUE(weakEvents.expired());
        EXPECT_EQ(destructions.load(std::memory_order_relaxed), 1U);
        blockingEvents->release();
    }

    EXPECT_EQ(postCalls.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(destructions.load(std::memory_order_relaxed), 1U);
}

} // namespace
} // namespace dvs::persistence
