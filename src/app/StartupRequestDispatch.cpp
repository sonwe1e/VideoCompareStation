#include "StartupRequestDispatch.h"

#include <algorithm>
#include <utility>

namespace dvs::app {
namespace {

// The first open after a launch waits for the device the scene graph creates. That normally lands
// within about a second; ten seconds is far past anything a working machine needs and still short
// enough that a session which will never be ready says so instead of hanging.
constexpr std::chrono::milliseconds kPatience{10000};

// Fast enough that the wait is invisible to the user who just double-clicked a file, slow enough
// that a session which is refusing every attempt is not asked thousands of times.
constexpr std::chrono::milliseconds kRetryInterval{50};

// A queue deep enough for every request a user can produce in one gesture, and no deeper: the
// review session serialises requests anyway, and an unbounded queue would only turn a stuck
// session into a growing backlog of files the user no longer cares about.
constexpr std::size_t kMaximumWaitingRequests = 8U;

} // namespace

StartupRequestDisposition classifyStartupRequest(const bool applied,
                                                 const bool sessionReady) noexcept {
    if (applied) {
        return StartupRequestDisposition::Applied;
    }
    return sessionReady ? StartupRequestDisposition::Rejected
                        : StartupRequestDisposition::RetryWhenReady;
}

std::chrono::milliseconds startupRequestPatience() noexcept {
    return kPatience;
}

std::chrono::milliseconds startupRequestRetryInterval() noexcept {
    return kRetryInterval;
}

StartupRequestDispatcher::StartupRequestDispatcher(ApplyRequest apply,
                                                   SessionReady ready,
                                                   RequestFailed failed)
    : apply_(std::move(apply)), ready_(std::move(ready)), failed_(std::move(failed)) {
    retry_.setInterval(std::chrono::milliseconds{kRetryInterval}.count());
    retry_.setSingleShot(false);
    // The timer is the context object, so a connection made here dies with the dispatcher rather
    // than with the next tick that happens to fire after it is gone.
    QObject::connect(&retry_, &QTimer::timeout, &retry_, [this] { drain(); });
}

StartupRequestDispatcher::~StartupRequestDispatcher() {
    stop();
}

bool StartupRequestDispatcher::submit(StartupRequest request) {
    if (stopped_) {
        return false;
    }
    if (request.kind == StartupRequest::Kind::Empty) {
        // An empty request is not something to hand over: it is the launch itself, and the session
        // already starts empty.
        return true;
    }
    if (waiting_.size() >= kMaximumWaitingRequests) {
        // The oldest request goes: it has waited longest, and the session takes requests in order,
        // so keeping it would only delay every request behind it. The user is told through the
        // failure callback instead of losing a file silently.
        const StartupRequest dropped = std::move(waiting_.front().first);
        waiting_.pop_front();
        ++rejected_;
        if (failed_) {
            failed_(dropped);
        }
    }
    const bool reachesTheFront = waiting_.empty();
    waiting_.emplace_back(std::move(request), std::chrono::steady_clock::now());
    // Offering it right here is what keeps a request that arrives on a ready session off the retry
    // timer. A request that queues up behind another one waits for its turn instead: the session
    // takes requests in order, and the ones already waiting were refused for the same reason this
    // one would be.
    if (reachesTheFront) {
        drain();
    }
    return true;
}

void StartupRequestDispatcher::drain() {
    if (stopped_) {
        return;
    }
    const bool sessionReady = ready_ ? ready_() : true;
    while (!waiting_.empty()) {
        const StartupRequest request = waiting_.front().first;
        const bool applied = apply_ ? apply_(request) : false;
        switch (classifyStartupRequest(applied, sessionReady)) {
        case StartupRequestDisposition::Applied:
            waiting_.pop_front();
            ++applied_;
            break;
        case StartupRequestDisposition::RetryWhenReady:
            if (isPatienceExpired(request)) {
                waiting_.pop_front();
                ++rejected_;
                if (failed_) {
                    failed_(request);
                }
                break;
            }
            // Everything behind this one would be refused for the same reason, so stop here.
            retry_.start();
            return;
        case StartupRequestDisposition::Rejected:
            waiting_.pop_front();
            ++rejected_;
            if (failed_) {
                failed_(request);
            }
            break;
        }
    }
    retry_.stop();
}

void StartupRequestDispatcher::stop() noexcept {
    stopped_ = true;
    retry_.stop();
    waiting_.clear();
}

std::size_t StartupRequestDispatcher::waiting() const noexcept {
    return waiting_.size();
}

std::size_t StartupRequestDispatcher::appliedCount() const noexcept {
    return applied_;
}

std::size_t StartupRequestDispatcher::rejectedCount() const noexcept {
    return rejected_;
}

bool StartupRequestDispatcher::isPatienceExpired(const StartupRequest& request) const {
    static_cast<void>(request);
    const std::chrono::steady_clock::time_point submitted = waiting_.front().second;
    return std::chrono::steady_clock::now() - submitted >= kPatience;
}

} // namespace dvs::app
