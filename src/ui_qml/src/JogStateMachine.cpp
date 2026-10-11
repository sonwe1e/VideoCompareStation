#include "dvs/ui/JogStateMachine.h"

#include <algorithm>
#include <cmath>

namespace dvs::ui {

namespace {
constexpr double kMinimumJogRate = 0.5;
constexpr double kMaximumJogRate = 2.0;
} // namespace

JogStateMachine::JogStateMachine(Host& host) noexcept : host_(host) {}

void JogStateMachine::setRate(const double rate) noexcept {
    rate_ = std::clamp(rate, kMinimumJogRate, kMaximumJogRate);
}

double JogStateMachine::rate() const noexcept {
    return rate_;
}

JogStateMachine::Phase JogStateMachine::phase() const noexcept {
    return phase_;
}

std::int64_t JogStateMachine::direction() const noexcept {
    return direction_;
}

bool JogStateMachine::press(const std::int64_t direction, const SteadyClockTime now) {
    if (direction != 1 && direction != -1) {
        return false;
    }
    if (phase_ != Phase::Idle) {
        // Same direction while already held: an OS auto-repeat that slipped through, or a
        // duplicate press. It must not re-trigger the tap step (E04: repeats never restart a
        // jog) and must not queue an extra command.
        if (direction == direction_) {
            return false;
        }
        cancel();
    }
    if (!host_.canJogToward(direction)) {
        return false;
    }
    const std::int64_t displayed = host_.jogDisplayedFrame();
    if (displayed < 0) {
        return false;
    }
    // Tap: the single step runs immediately, exactly like a one-off next/previous.
    if (!host_.submitJogStep(direction)) {
        return false;
    }
    phase_ = Phase::Pressing;
    direction_ = direction;
    pressTime_ = now;
    jogStart_ = now;
    lastRequestedFrame_ = displayed + direction;
    jogStartFrame_ = lastRequestedFrame_;
    return true;
}

void JogStateMachine::release() {
    stopFeeding();
}

void JogStateMachine::cancel() {
    stopFeeding();
}

void JogStateMachine::poll(const SteadyClockTime now) {
    if (phase_ == Phase::Idle) {
        return;
    }
    if (phase_ == Phase::Pressing) {
        if (now - pressTime_ < kHoldThreshold) {
            return;
        }
        // The continuous walk's media clock starts at the threshold, not at the press: the tap
        // step is a one-off, and the hold wait must not count as walked media time.
        phase_ = Phase::Jogging;
        jogStart_ = now;
        jogStartFrame_ = lastRequestedFrame_;
    }
    // Session gone or boundary reached: the held key can no longer step this direction.
    if (!host_.canJogToward(direction_)) {
        stopFeeding();
        return;
    }
    const std::int64_t displayed = host_.jogDisplayedFrame();
    if (displayed < 0) {
        return;
    }
    // External navigation can move the presented frame past what this jog tracked; resync so
    // the lead bound measures against reality, not a stale target.
    const std::int64_t presentedPastTarget = direction_ > 0 ? displayed - lastRequestedFrame_
                                                            : lastRequestedFrame_ - displayed;
    if (presentedPastTarget > 0) {
        lastRequestedFrame_ = displayed;
    }
    const int framesPerSecond = host_.jogFramesPerSecond();
    if (framesPerSecond <= 0) {
        return;
    }
    // Media-clock target: where this jog should be by now given its start frame, rate and the
    // source frame rate — never a function of how many key events arrived.
    const double elapsedSeconds =
        std::chrono::duration<double>(now - jogStart_).count() * rate_ * framesPerSecond;
    const std::int64_t frameCount = host_.jogFrameCount();
    std::int64_t expected =
        jogStartFrame_ + direction_ * static_cast<std::int64_t>(std::floor(elapsedSeconds));
    expected = std::clamp(expected, std::int64_t{0}, std::max<std::int64_t>(frameCount - 1, 0));
    // Presentation backpressure: never run further ahead of the presented frame than the lead
    // bound. A pipeline that keeps up advances one step per frame interval; a slow one (cold
    // reverse GOP) advances at its actual speed and reports it.
    const std::int64_t leadLimit = direction_ > 0 ? displayed + kMaximumPresentationLead
                                                  : displayed - kMaximumPresentationLead;
    std::int64_t limit =
        direction_ > 0 ? std::min(expected, leadLimit) : std::max(expected, leadLimit);
    // The jog target only ever advances in its own direction.
    limit = direction_ > 0 ? std::max(limit, lastRequestedFrame_)
                           : std::min(limit, lastRequestedFrame_);
    while (lastRequestedFrame_ != limit) {
        if (!host_.submitJogStep(direction_)) {
            break;
        }
        lastRequestedFrame_ += direction_;
    }
}

void JogStateMachine::stopFeeding() noexcept {
    phase_ = Phase::Idle;
    direction_ = 0;
}

} // namespace dvs::ui
