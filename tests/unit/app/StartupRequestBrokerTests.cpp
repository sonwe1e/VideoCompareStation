#include "StartupRequestBroker.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <QUuid>

#include <chrono>
#include <filesystem>
#include <future>
#include <gtest/gtest.h>
#include <optional>
#include <thread>
#include <utility>

namespace dvs::app {
namespace {

using namespace std::chrono_literals;

void ensureCoreApplication() {
    if (QCoreApplication::instance() != nullptr) {
        return;
    }
    static int argumentCount = 1;
    static char applicationName[] = "StartupRequestBrokerTests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application{argumentCount, arguments};
    static_cast<void>(application);
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout = 1s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1U);
    }
    return predicate();
}

TEST(StartupRequestBrokerTests, ForwardsUnicodeComparisonToExistingPrimary) {
    ensureCoreApplication();
    const QString endpoint = QStringLiteral("CompareStation.StartupRequest.Test.%1")
                                 .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    StartupRequestBroker primary{endpoint};
    std::optional<StartupRequest> received;
    primary.setRequestHandler([&received](StartupRequest request) {
        received = std::move(request);
        return true;
    });

    ASSERT_EQ(primary.startOrForward(StartupRequest{}), StartupRequestBroker::StartResult::Primary);

    const StartupRequest request{
        .kind = StartupRequest::Kind::Compare,
        .sources =
            {
                std::filesystem::path{LR"(C:\素材\甲 视频.mp4)"},
                std::filesystem::path{LR"(D:\素材\乙 视频.mkv)"},
            },
    };
    std::promise<StartupRequestBroker::StartResult> forwardedPromise;
    std::future<StartupRequestBroker::StartResult> forwarded = forwardedPromise.get_future();
    std::jthread secondary{[endpoint, request, promise = std::move(forwardedPromise)]() mutable {
        StartupRequestBroker broker{endpoint};
        promise.set_value(broker.startOrForward(request));
    }};
    ASSERT_TRUE(
        waitUntil([&forwarded] { return forwarded.wait_for(0ms) == std::future_status::ready; }));
    ASSERT_EQ(forwarded.get(), StartupRequestBroker::StartResult::Forwarded);
    ASSERT_TRUE(waitUntil([&received] { return received.has_value(); }));
    EXPECT_EQ(*received, request);
}

TEST(StartupRequestBrokerTests, QueuesForwardedRequestUntilPrimaryRegistersHandler) {
    ensureCoreApplication();
    const QString endpoint = QStringLiteral("CompareStation.StartupRequest.Test.%1")
                                 .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    StartupRequestBroker primary{endpoint};
    ASSERT_EQ(primary.startOrForward(StartupRequest{}), StartupRequestBroker::StartResult::Primary);

    const StartupRequest request{
        .kind = StartupRequest::Kind::Compare,
        .sources =
            {
                std::filesystem::path{LR"(C:\素材\启动 甲.mp4)"},
                std::filesystem::path{LR"(D:\素材\启动 乙.mkv)"},
            },
    };
    std::promise<StartupRequestBroker::StartResult> forwardedPromise;
    std::future<StartupRequestBroker::StartResult> forwarded = forwardedPromise.get_future();
    std::jthread secondary{[endpoint, request, promise = std::move(forwardedPromise)]() mutable {
        StartupRequestBroker broker{endpoint};
        promise.set_value(broker.startOrForward(request));
    }};
    ASSERT_TRUE(
        waitUntil([&forwarded] { return forwarded.wait_for(0ms) == std::future_status::ready; }));
    EXPECT_EQ(forwarded.get(), StartupRequestBroker::StartResult::Forwarded);

    std::vector<StartupRequest> received;
    primary.setRequestHandler([&received](StartupRequest queued) {
        received.push_back(std::move(queued));
        return true;
    });
    ASSERT_EQ(received.size(), 1U);
    EXPECT_EQ(received.front(), request);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_EQ(received.size(), 1U);
}

TEST(StartupRequestBrokerTests, RejectsForwardedRequestWhenInteractionQueueIsFull) {
    ensureCoreApplication();
    const QString endpoint = QStringLiteral("CompareStation.StartupRequest.Test.%1")
                                 .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    StartupRequestBroker primary{endpoint};
    primary.setRequestHandler([](StartupRequest) { return false; });
    ASSERT_EQ(primary.startOrForward(StartupRequest{}), StartupRequestBroker::StartResult::Primary);

    const StartupRequest request{
        .kind = StartupRequest::Kind::PlaySingle,
        .sources = {std::filesystem::path{LR"(C:\media\queued.mp4)"}},
    };
    std::promise<StartupRequestBroker::StartResult> forwardedPromise;
    std::future<StartupRequestBroker::StartResult> forwarded = forwardedPromise.get_future();
    std::jthread secondary{[endpoint, request, promise = std::move(forwardedPromise)]() mutable {
        StartupRequestBroker broker{endpoint};
        promise.set_value(broker.startOrForward(request));
    }};
    ASSERT_TRUE(
        waitUntil([&forwarded] { return forwarded.wait_for(0ms) == std::future_status::ready; }));
    EXPECT_EQ(forwarded.get(), StartupRequestBroker::StartResult::Failed);
}

} // namespace
} // namespace dvs::app
