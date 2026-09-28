#include "StartupRequestDispatch.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <chrono>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace dvs::app {
namespace {

using namespace std::chrono_literals;

void ensureCoreApplication() {
    if (QCoreApplication::instance() != nullptr) {
        return;
    }
    static int argumentCount = 1;
    static char applicationName[] = "StartupRequestDispatchTests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application{argumentCount, arguments};
    static_cast<void>(application);
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return predicate();
}

[[nodiscard]] StartupRequest requestFor(const std::string& name) {
    return StartupRequest{
        .kind = StartupRequest::Kind::PlaySingle,
        .sources = {std::filesystem::path{L"C:\\media"} / name},
    };
}

[[nodiscard]] std::string nameOf(const StartupRequest& request) {
    return request.sources.empty() ? std::string{} : request.sources.front().filename().string();
}

// A session whose two answers the test decides, so every branch of the decision can be reached
// without a graphics device, a decoder, or a real launch.
struct ScriptedSession final {
    std::function<bool(StartupRequest)> apply;
    std::function<bool()> ready;
    std::vector<std::string> offered;
    std::vector<std::string> failed;
};

[[nodiscard]] std::unique_ptr<StartupRequestDispatcher> makeDispatcher(ScriptedSession& session) {
    return std::make_unique<StartupRequestDispatcher>(
        [&session](StartupRequest request) {
            session.offered.push_back(nameOf(request));
            return session.apply(std::move(request));
        },
        [&session] { return session.ready(); },
        [&session](const StartupRequest& request) { session.failed.push_back(nameOf(request)); });
}

TEST(StartupRequestDispatchTests, TakesARequestTheSessionAcceptsStraightAway) {
    ensureCoreApplication();
    ScriptedSession session;
    session.apply = [](StartupRequest) { return true; };
    session.ready = [] { return true; };
    const std::unique_ptr<StartupRequestDispatcher> dispatcher = makeDispatcher(session);

    EXPECT_TRUE(dispatcher->submit(requestFor("a.mp4")));
    EXPECT_EQ(dispatcher->waiting(), 0U);
    EXPECT_EQ(dispatcher->appliedCount(), 1U);
    EXPECT_EQ(dispatcher->rejectedCount(), 0U);
    EXPECT_TRUE(session.failed.empty());
}

TEST(StartupRequestDispatchTests, OffersARefusedRequestAgainOnceTheSessionIsReady) {
    // This is the launch from Explorer's right-click menu: the window is up, the session has not
    // adopted its graphics device yet, and the open is refused. Losing that race used to end the
    // process, so the request has to stay queued instead.
    ensureCoreApplication();
    ScriptedSession session;
    bool ready = false;
    session.apply = [&ready](StartupRequest) { return ready; };
    session.ready = [&ready] { return ready; };
    const std::unique_ptr<StartupRequestDispatcher> dispatcher = makeDispatcher(session);

    ASSERT_TRUE(dispatcher->submit(requestFor("a.mp4")));
    ASSERT_EQ(dispatcher->waiting(), 1U);
    EXPECT_EQ(dispatcher->appliedCount(), 0U);
    // Waiting is not failing: nothing is reported while the session may still take the request.
    EXPECT_TRUE(session.failed.empty());

    ready = true;
    dispatcher->drain();
    EXPECT_EQ(dispatcher->waiting(), 0U);
    EXPECT_EQ(dispatcher->appliedCount(), 1U);
    EXPECT_TRUE(session.failed.empty());
}

TEST(StartupRequestDispatchTests, ReportsARequestTheReadySessionRefuses) {
    // The session can take requests and says no, so the request itself is the problem - a file
    // that is not there. Reporting it is the only way the user learns why nothing opened.
    ensureCoreApplication();
    ScriptedSession session;
    session.apply = [](StartupRequest) { return false; };
    session.ready = [] { return true; };
    const std::unique_ptr<StartupRequestDispatcher> dispatcher = makeDispatcher(session);

    EXPECT_TRUE(dispatcher->submit(requestFor("missing.mp4")));
    EXPECT_EQ(dispatcher->waiting(), 0U);
    EXPECT_EQ(dispatcher->appliedCount(), 0U);
    EXPECT_EQ(dispatcher->rejectedCount(), 1U);
    ASSERT_EQ(session.failed.size(), 1U);
    EXPECT_EQ(session.failed.front(), "missing.mp4");
}

TEST(StartupRequestDispatchTests, LeavesTheQueueBehindAWaitingRequestAlone) {
    // A refused request is a fact about the session, not about the file: while the front request
    // waits, the ones behind it would be refused for the same reason, so they are not offered -
    // neither when they arrive nor by the next retry.
    ensureCoreApplication();
    ScriptedSession session;
    session.apply = [](StartupRequest) { return false; };
    session.ready = [] { return false; };
    const std::unique_ptr<StartupRequestDispatcher> dispatcher = makeDispatcher(session);

    ASSERT_TRUE(dispatcher->submit(requestFor("first.mp4")));
    ASSERT_EQ(session.offered, (std::vector<std::string>{"first.mp4"}));
    ASSERT_TRUE(dispatcher->submit(requestFor("second.mp4")));
    EXPECT_EQ(session.offered, (std::vector<std::string>{"first.mp4"}));
    EXPECT_EQ(dispatcher->waiting(), 2U);

    dispatcher->drain();
    // One more attempt for the front request, and the request behind it is left alone.
    EXPECT_EQ(session.offered, (std::vector<std::string>{"first.mp4", "first.mp4"}));
    EXPECT_EQ(dispatcher->waiting(), 2U);
    EXPECT_TRUE(session.failed.empty());
}

TEST(StartupRequestDispatchTests, AppliesWaitingRequestsInArrivalOrder) {
    ensureCoreApplication();
    ScriptedSession session;
    bool ready = false;
    session.apply = [&ready](StartupRequest) { return ready; };
    session.ready = [&ready] { return ready; };
    const std::unique_ptr<StartupRequestDispatcher> dispatcher = makeDispatcher(session);

    ASSERT_TRUE(dispatcher->submit(requestFor("first.mp4")));
    ASSERT_TRUE(dispatcher->submit(requestFor("second.mp4")));
    ASSERT_EQ(dispatcher->waiting(), 2U);

    ready = true;
    dispatcher->drain();
    EXPECT_EQ(session.offered, (std::vector<std::string>{"first.mp4", "first.mp4", "second.mp4"}));
    EXPECT_EQ(dispatcher->waiting(), 0U);
    EXPECT_EQ(dispatcher->appliedCount(), 2U);
    EXPECT_TRUE(session.failed.empty());
}

TEST(StartupRequestDispatchTests, OffersAWaitingRequestFromTheEventLoopAlone) {
    // After a launch the only thing still running is the application itself, so the retry has to
    // come from the event loop rather than from a caller that asked.
    ensureCoreApplication();
    ScriptedSession session;
    bool ready = false;
    session.apply = [&ready](StartupRequest) { return ready; };
    session.ready = [&ready] { return ready; };
    const std::unique_ptr<StartupRequestDispatcher> dispatcher = makeDispatcher(session);

    ASSERT_TRUE(dispatcher->submit(requestFor("a.mp4")));
    ASSERT_EQ(dispatcher->waiting(), 1U);

    ready = true;
    EXPECT_TRUE(waitUntil([&dispatcher] { return dispatcher->appliedCount() == 1U; }));
    EXPECT_EQ(dispatcher->waiting(), 0U);
    EXPECT_TRUE(session.failed.empty());
}

TEST(StartupRequestDispatchTests, StopsOfferingARequestWhenTheSessionIsShutDown) {
    ensureCoreApplication();
    ScriptedSession session;
    session.apply = [](StartupRequest) { return false; };
    session.ready = [] { return false; };
    const std::unique_ptr<StartupRequestDispatcher> dispatcher = makeDispatcher(session);

    ASSERT_TRUE(dispatcher->submit(requestFor("a.mp4")));
    ASSERT_EQ(dispatcher->waiting(), 1U);
    dispatcher->stop();
    // A session that is shutting down has no user left to tell, so the request goes without a
    // report - and nothing is offered to a scene graph that is about to be released.
    EXPECT_EQ(dispatcher->waiting(), 0U);
    EXPECT_TRUE(session.failed.empty());
    EXPECT_FALSE(dispatcher->submit(requestFor("b.mp4")));
}

TEST(StartupRequestDispatchTests, KeepsAnEmptyLaunchOutOfTheQueue) {
    // An empty request is the launch itself, not something to hand to a session that already
    // starts empty, and it must not be reported when the session is busy.
    ensureCoreApplication();
    ScriptedSession session;
    session.apply = [](StartupRequest) {
        ADD_FAILURE() << "an empty request must not reach the session";
        return true;
    };
    session.ready = [] { return false; };
    const std::unique_ptr<StartupRequestDispatcher> dispatcher = makeDispatcher(session);

    EXPECT_TRUE(dispatcher->submit(StartupRequest{}));
    EXPECT_EQ(dispatcher->waiting(), 0U);
    EXPECT_EQ(dispatcher->appliedCount(), 0U);
    EXPECT_EQ(dispatcher->rejectedCount(), 0U);
    EXPECT_TRUE(session.failed.empty());
    EXPECT_TRUE(session.offered.empty());
}

TEST(StartupRequestDispatchTests, KeepsWaitingRatherThanReportingWhileTheSessionIsNotReady) {
    // The patience is bounded on purpose, so the wait has to be observable: a session that has not
    // become ready yet must not be reported, and the request stays queued across several retries.
    ensureCoreApplication();
    EXPECT_GT(startupRequestPatience(), startupRequestRetryInterval());
    EXPECT_GT(startupRequestPatience(), 5s);

    ScriptedSession session;
    session.apply = [](StartupRequest) { return false; };
    session.ready = [] { return false; };
    const std::unique_ptr<StartupRequestDispatcher> dispatcher = makeDispatcher(session);

    ASSERT_TRUE(dispatcher->submit(requestFor("a.mp4")));
    static_cast<void>(waitUntil([] { return false; }, 150ms));
    EXPECT_GE(session.offered.size(), 2U);
    EXPECT_EQ(dispatcher->waiting(), 1U);
    EXPECT_EQ(dispatcher->rejectedCount(), 0U);
    EXPECT_TRUE(session.failed.empty());
}

TEST(StartupRequestDispatchTests, ClassifiesTheThreeOutcomesApart) {
    EXPECT_EQ(classifyStartupRequest(true, true), StartupRequestDisposition::Applied);
    EXPECT_EQ(classifyStartupRequest(true, false), StartupRequestDisposition::Applied);
    EXPECT_EQ(classifyStartupRequest(false, false), StartupRequestDisposition::RetryWhenReady);
    EXPECT_EQ(classifyStartupRequest(false, true), StartupRequestDisposition::Rejected);
}

} // namespace
} // namespace dvs::app
