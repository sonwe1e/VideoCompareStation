#pragma once

#include "StartupRequest.h"

#include <QTimer>

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>

namespace dvs::app {

// What one attempt to hand a request to the review session told us. The distinction is the whole
// point of this file: "the session cannot take it yet" is a moment in time, while "the session
// refused it" is a fact about the request, and retrying the second one only repeats it.
enum class StartupRequestDisposition {
    // The session took the request. Nothing is left to do.
    Applied,

    // The session is not able to take requests yet - it has no graphics device, or it is still
    // busy with an earlier one. The same request has to be offered again.
    RetryWhenReady,

    // The session is able to take requests and declined this one, so the request itself is the
    // problem: a file that is not there, a selection the session cannot represent. Retrying it
    // forever would hide that from the user.
    Rejected,
};

[[nodiscard]] StartupRequestDisposition classifyStartupRequest(const bool applied,
                                                               const bool sessionReady) noexcept;

// How long a request that arrived before the session could take it keeps waiting. It is bounded on
// purpose: a request that is still refused after this is reported rather than retried until the
// process ends, because "the device never came up" has to reach the user as a fact.
[[nodiscard]] std::chrono::milliseconds startupRequestPatience() noexcept;

// How often a waiting request is offered again while the session is not ready.
[[nodiscard]] std::chrono::milliseconds startupRequestRetryInterval() noexcept;

// Owns the requests that arrived before the review session could take them, and offers them again
// until they are taken or the patience runs out.
//
// This exists because a launch from Explorer's right-click menu is answered exactly once, at a
// fixed point: right after the window is shown. The session only accepts an open once its graphics
// device has been adopted, and that hand-off crosses three threads (render thread -> graphics pump
// -> session), so whether the device is ready at that fixed point is a race, not a guarantee.
// Losing it used to be fatal: the process showed a "CompareStation could not start" dialog and
// exited, which is both untrue (the window is open and usable) and unrecoverable (the file the
// user picked is simply never opened).
class StartupRequestDispatcher final {
public:
    // Offers one request to the session. Returning false is the session's answer, not an error.
    using ApplyRequest = std::function<bool(StartupRequest)>;

    // Whether the session can take a request right now.
    using SessionReady = std::function<bool()>;

    // Called once per request that will not be applied, with the request that was refused.
    using RequestFailed = std::function<void(const StartupRequest&)>;

    StartupRequestDispatcher(ApplyRequest apply, SessionReady ready, RequestFailed failed = {});
    ~StartupRequestDispatcher();

    StartupRequestDispatcher(const StartupRequestDispatcher&) = delete;
    StartupRequestDispatcher& operator=(const StartupRequestDispatcher&) = delete;

    // Accepts a request. Returns whether the request was taken into the queue, which is false only
    // when there is nowhere left to keep it - never because the session said no, which is reported
    // through the failure callback instead. A caller that has to answer a remote process (the
    // startup broker) therefore reports success for a request that is still waiting, which is
    // exactly what it promised that process.
    [[nodiscard]] bool submit(StartupRequest request);

    // Offers every waiting request to the session once, in the order the requests arrived, and
    // stops at the first one the session is not ready for: the ones behind it would be refused for
    // the same reason.
    void drain();

    // Stops offering anything. The waiting requests are dropped without being reported, because a
    // session that is shutting down has no user left to tell.
    void stop() noexcept;

    [[nodiscard]] std::size_t waiting() const noexcept;
    [[nodiscard]] std::size_t appliedCount() const noexcept;
    [[nodiscard]] std::size_t rejectedCount() const noexcept;

private:
    [[nodiscard]] bool isPatienceExpired(const StartupRequest& request) const;

    ApplyRequest apply_;
    SessionReady ready_;
    RequestFailed failed_;
    QTimer retry_;
    std::deque<std::pair<StartupRequest, std::chrono::steady_clock::time_point>> waiting_;
    std::size_t applied_ = 0U;
    std::size_t rejected_ = 0U;
    bool stopped_ = false;
};

} // namespace dvs::app
