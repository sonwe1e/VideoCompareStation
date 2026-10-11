#pragma once

#include <chrono>
#include <cstdint>

namespace dvs::ui {

// E04 input-layer Jog state machine: tap = one immediate ±1 step, hold >= 250 ms = continuous
// walking, release = stop feeding. Pure and clock-injected so tests drive every transition
// without a live event loop. The host owns execution: step submission goes through the same
// PlaybackCoordinator StepFramesCommand path as a single tap, so one held key extends one
// interactive stream (one generation, current+prepared pipeline) instead of queueing one
// command per key repeat. Pacing is media-clock based: each poll derives the frame the jog
// should have reached from elapsed time and rate, then clamps to a small lead over the last
// presented frame, so decode/presentation backpressure — not key count — decides advancement.
class JogStateMachine final {
public:
    using SteadyClockTime = std::chrono::steady_clock::time_point;

    // Execution surface implemented by the owning controller. All calls stay on the GUI thread.
    class Host {
    public:
        virtual ~Host() = default;
        // Submit one ±1 canonical step. False = rejected (backpressure, boundary, closed port);
        // the machine stops extending toward the current target and retries on the next poll.
        [[nodiscard]] virtual bool submitJogStep(std::int64_t delta) = 0;
        // Latest presented canonical frame, or a negative value when nothing is presented yet.
        [[nodiscard]] virtual std::int64_t jogDisplayedFrame() const = 0;
        [[nodiscard]] virtual std::int64_t jogFrameCount() const = 0;
        [[nodiscard]] virtual int jogFramesPerSecond() const = 0;
        // Whether the session can still step one frame in `delta` direction.
        [[nodiscard]] virtual bool canJogToward(std::int64_t delta) const = 0;
    };

    enum class Phase : std::uint8_t {
        // No key or button is held.
        Idle,
        // Held shorter than the hold threshold: the tap step already ran, waiting to see
        // whether this is a hold.
        Pressing,
        // Hold threshold elapsed: continuously feeding pace steps.
        Jogging,
    };

    // A tap shorter than this never enters the continuous mode. Plan E04: ~250 ms.
    static constexpr std::chrono::milliseconds kHoldThreshold{250};
    // Poll granularity for the pacing timer; pacing itself is computed from media time, so this
    // only bounds how quickly the lead bound reacts to presentation.
    static constexpr std::chrono::milliseconds kPacingTick{16};
    // Input lead over the presented frame. The coordinator's interactive stream keeps one
    // current + one prepared frame in flight; staying at two keeps the pipeline fed without
    // reaching the coordinator's own input-lookahead Busy bound.
    static constexpr std::int64_t kMaximumPresentationLead = 2;

    explicit JogStateMachine(Host& host) noexcept;

    // Continuous walking rate in real-time multiples. Clamped to [0.5, 2.0]; default 1.0.
    void setRate(double rate) noexcept;
    [[nodiscard]] double rate() const noexcept;
    [[nodiscard]] Phase phase() const noexcept;
    // Active direction while not Idle (+1 forward, -1 reverse), otherwise 0.
    [[nodiscard]] std::int64_t direction() const noexcept;

    // Begins a press. The immediate tap step is submitted here. A press while already held in
    // the same direction is an OS auto-repeat or stray second press and is ignored (returns
    // false, no command); the opposite direction ends the current hold/jog first, which lets
    // the coordinator cancel the old stream and start a fresh generation for the new one.
    bool press(std::int64_t direction, SteadyClockTime now);
    // Ends the hold/jog. Frames already requested keep their bounded coordinator drain; no
    // further steps are fed after release.
    void release();
    // Non-release end (Esc, focus loss, dialog, session change). Same state effect as release.
    void cancel();
    // Advances the machine: Pressing -> Jogging at the hold threshold, then media-clock-paced
    // stepping with the presentation lead bound. Reaching a session boundary cancels.
    void poll(SteadyClockTime now);

private:
    void stopFeeding() noexcept;

    Host& host_;
    Phase phase_ = Phase::Idle;
    std::int64_t direction_ = 0;
    double rate_ = 1.0;
    SteadyClockTime pressTime_{};
    SteadyClockTime jogStart_{};
    // Newest canonical frame this jog has asked for. Re-synced from the presented frame on
    // every poll so external navigation cannot desync the lead bound.
    std::int64_t lastRequestedFrame_ = 0;
    std::int64_t jogStartFrame_ = 0;
};

} // namespace dvs::ui
