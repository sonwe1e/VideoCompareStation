#include "StartupRequestBroker.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLocalSocket>
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

TEST(StartupRequestBrokerTests, StartsAgainAfterThePreviousPrimaryIsGone) {
    ensureCoreApplication();
    const QString endpoint = QStringLiteral("CompareStation.StartupRequest.Test.%1")
                                 .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const StartupRequest request{
        .kind = StartupRequest::Kind::PlaySingle,
        .sources = {std::filesystem::path{LR"(C:\media\relaunch.mp4)"}},
    };
    {
        StartupRequestBroker first{endpoint};
        ASSERT_EQ(first.startOrForward(request), StartupRequestBroker::StartResult::Primary);
    }
    // The endpoint name and the election lock outlive their owner for a moment after a crash or a
    // kill, and the relaunch has to win the election again rather than report a fatal startup
    // error.
    StartupRequestBroker second{endpoint};
    EXPECT_EQ(second.startOrForward(request), StartupRequestBroker::StartResult::Primary);
}

// Windows only lets the foreground process hand the foreground right to another process, and the
// forwarder is the one that holds it during a double-click from Explorer. So the primary has to
// announce its process id before the forwarder sends anything, and the forwarder has to read that
// line off the socket. This asserts the announce-then-read handshake on its own: without the
// greeting the forwarder would send the request having granted nobody, which is exactly the state
// that leaves the running window behind the windows the user was already looking at.
TEST(StartupRequestBrokerTests, PrimaryAnnouncesItsProcessIdBeforeTheForwarderSends) {
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

    // Speak to the endpoint from another thread, the way the real forwarder does. Driving the
    // socket
    // from the thread that also pumps the server's event loop re-enters the broker mid-read and
    // faults.
    std::promise<QByteArray> greetingPromise;
    std::future<QByteArray> greetingFuture = greetingPromise.get_future();
    std::jthread client{[endpoint, promise = std::move(greetingPromise)]() mutable {
        QLocalSocket socket;
        socket.connectToServer(endpoint, QIODevice::ReadWrite);
        // connectToServer returns void; waitForConnected is the only thing that can fail here.
        if (!socket.waitForConnected(1000)) {
            promise.set_value(QByteArray{});
            return;
        }
        QByteArray greeting;
        QElapsedTimer greetingWindow;
        greetingWindow.start();
        while (!greeting.contains('\n') && greetingWindow.elapsed() < 1000) {
            if (socket.bytesAvailable() == 0) {
                static_cast<void>(socket.waitForReadyRead(50));
            }
            greeting.append(socket.readAll());
        }
        socket.disconnectFromServer();
        promise.set_value(greeting);
    }};
    ASSERT_TRUE(waitUntil(
        [&greetingFuture] { return greetingFuture.wait_for(0ms) == std::future_status::ready; }));
    const QByteArray greeting = greetingFuture.get();
    const qsizetype terminator = greeting.indexOf('\n');
    ASSERT_GE(terminator, 0) << "the primary never announced its process id";
    const QString line = QString::fromLatin1(greeting.left(terminator));
    ASSERT_TRUE(line.startsWith(QStringLiteral("PID ")))
        << "unexpected greeting: " << line.toStdString();
    EXPECT_EQ(line.mid(4).trimmed().toLongLong(), QCoreApplication::applicationPid());
}

} // namespace
} // namespace dvs::app
