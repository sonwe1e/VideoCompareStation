#include "dvs/ui/JogStateMachine.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace dvs::ui {
namespace {

using SteadyClockTime = JogStateMachine::SteadyClockTime;

// Deterministic host. `presentedFrameFollowsRequests` models an infinitely fast pipeline (every
// submitted step is presented immediately) so media-clock pacing is observable without waits;
// leaving it false models real backpressure where the presented frame lags the requests.
class FakeJogHost final : public JogStateMachine::Host {
public:
    std::int64_t displayed = 10;
    std::int64_t frameCount = 100;
    int framesPerSecond = 30;
    bool canBackward = true;
    bool canForward = true;
    bool acceptsSteps = true;
    bool presentedFrameFollowsRequests = false;
    std::vector<std::int64_t> submittedSteps;

    [[nodiscard]] bool submitJogStep(const std::int64_t delta) override {
        if (!acceptsSteps) {
            return false;
        }
        submittedSteps.push_back(delta);
        if (presentedFrameFollowsRequests) {
            displayed += delta;
        }
        return true;
    }

    [[nodiscard]] std::int64_t jogDisplayedFrame() const override {
        return displayed;
    }

    [[nodiscard]] std::int64_t jogFrameCount() const override {
        return frameCount;
    }

    [[nodiscard]] int jogFramesPerSecond() const override {
        return framesPerSecond;
    }

    [[nodiscard]] bool canJogToward(const std::int64_t delta) const override {
        return delta < 0 ? canBackward : canForward;
    }

    void clear() {
        submittedSteps.clear();
    }

    [[nodiscard]] std::int64_t submittedTotal() const {
        std::int64_t total = 0;
        for (const std::int64_t step : submittedSteps) {
            total += step;
        }
        return total;
    }
};

[[nodiscard]] SteadyClockTime atMilliseconds(const std::int64_t milliseconds) {
    return SteadyClockTime{} + std::chrono::milliseconds{milliseconds};
}

class JogStateMachineTests : public testing::Test {
protected:
    FakeJogHost host;
    JogStateMachine jog{host};
};

TEST_F(JogStateMachineTests, TapSubmitsExactlyOneImmediateStepAndStaysPausedOnRelease) {
    EXPECT_TRUE(jog.press(1, atMilliseconds(0)));
    EXPECT_EQ(host.submittedSteps, std::vector<std::int64_t>{1});
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Pressing);

    // Still inside the 250 ms hold window: no continuous steps.
    jog.poll(atMilliseconds(249));
    EXPECT_EQ(host.submittedSteps.size(), 1U);

    jog.release();
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Idle);
    jog.poll(atMilliseconds(400));
    EXPECT_EQ(host.submittedSteps.size(), 1U);
}

TEST_F(JogStateMachineTests, HoldThresholdEntersJoggingAndPollBelowItDoesNot) {
    EXPECT_TRUE(jog.press(-1, atMilliseconds(0)));
    jog.poll(atMilliseconds(249));
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Pressing);

    jog.poll(atMilliseconds(250));
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Jogging);
    EXPECT_EQ(jog.direction(), -1);
}

TEST_F(JogStateMachineTests, JoggingPacesByMediaClockNotByPollCount) {
    host.presentedFrameFollowsRequests = true;
    EXPECT_TRUE(jog.press(1, atMilliseconds(0)));
    ASSERT_EQ(host.submittedSteps, std::vector<std::int64_t>{1});

    // Hold threshold: the walk clock starts here, elapsed 0 -> no paced step yet.
    jog.poll(atMilliseconds(250));
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Jogging);
    EXPECT_EQ(host.submittedSteps.size(), 1U);

    // 100 ms at 30 fps 1x = 3 frames after the start frame; the lead bound feeds two now.
    jog.poll(atMilliseconds(350));
    EXPECT_EQ(host.submittedSteps, std::vector<std::int64_t>({1, 1, 1}));
    EXPECT_EQ(host.displayed, 13);

    // The remaining media-clock debt is repaid on the next tick, not by extra polling speed.
    jog.poll(atMilliseconds(366));
    EXPECT_EQ(host.submittedSteps, std::vector<std::int64_t>({1, 1, 1, 1}));
    EXPECT_EQ(host.displayed, 14);

    // Media time has not moved: further polls step nothing.
    jog.poll(atMilliseconds(370));
    EXPECT_EQ(host.submittedSteps.size(), 4U);
}

TEST_F(JogStateMachineTests, PresentationBackpressureCapsLeadOverDisplayedFrame) {
    // A stalled pipeline: the displayed frame never advances, so the lead bound — not elapsed
    // media time — limits what the input may queue.
    EXPECT_TRUE(jog.press(1, atMilliseconds(0)));
    jog.poll(atMilliseconds(250));
    jog.poll(atMilliseconds(1000));
    jog.poll(atMilliseconds(2000));
    // Displayed 10, tap step reached 11; the lead may grow to 12 but no further.
    EXPECT_EQ(host.submittedSteps, std::vector<std::int64_t>({1, 1}));
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Jogging);
}

TEST_F(JogStateMachineTests, ReleaseStopsFeedingAndOldRequestsKeepTheirBoundedDrain) {
    EXPECT_TRUE(jog.press(1, atMilliseconds(0)));
    jog.poll(atMilliseconds(250));
    jog.poll(atMilliseconds(1000));
    const std::size_t stepsAtRelease = host.submittedSteps.size();
    jog.release();
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Idle);
    jog.poll(atMilliseconds(2000));
    jog.poll(atMilliseconds(3000));
    EXPECT_EQ(host.submittedSteps.size(), stepsAtRelease);
}

TEST_F(JogStateMachineTests, BoundaryCancelsJogWhenDirectionCanNoLongerStep) {
    EXPECT_TRUE(jog.press(1, atMilliseconds(0)));
    jog.poll(atMilliseconds(250));
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Jogging);

    host.canForward = false;
    jog.poll(atMilliseconds(260));
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Idle);
    EXPECT_EQ(jog.direction(), 0);

    host.canForward = true;
    jog.poll(atMilliseconds(270));
    EXPECT_EQ(host.submittedSteps.size(), 1U);
}

TEST_F(JogStateMachineTests, SameDirectionRepeatPressIsIgnoredAsAutoRepeat) {
    EXPECT_TRUE(jog.press(1, atMilliseconds(0)));
    EXPECT_FALSE(jog.press(1, atMilliseconds(50)));
    EXPECT_EQ(host.submittedSteps.size(), 1U);

    jog.poll(atMilliseconds(250));
    EXPECT_FALSE(jog.press(1, atMilliseconds(300)));
    EXPECT_EQ(host.submittedSteps.size(), 1U);
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Jogging);
}

TEST_F(JogStateMachineTests, OppositeDirectionPressEndsOldJogAndStartsNewOne) {
    EXPECT_TRUE(jog.press(1, atMilliseconds(0)));
    jog.poll(atMilliseconds(250));
    ASSERT_EQ(jog.phase(), JogStateMachine::Phase::Jogging);

    host.clear();
    host.presentedFrameFollowsRequests = true;
    EXPECT_TRUE(jog.press(-1, atMilliseconds(300)));
    EXPECT_EQ(host.submittedSteps, std::vector<std::int64_t>{-1});
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Pressing);
    EXPECT_EQ(jog.direction(), -1);
    EXPECT_EQ(host.displayed, 9);

    // The new hold window runs from its own press; crossing it enters Jogging, and 100 ms of
    // walk at 30 fps adds three reverse steps from the current presented frame.
    jog.poll(atMilliseconds(550));
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Jogging);
    EXPECT_EQ(host.submittedSteps.size(), 1U);
    jog.poll(atMilliseconds(650));
    EXPECT_EQ(host.submittedSteps, std::vector<std::int64_t>({-1, -1, -1}));
    EXPECT_EQ(host.displayed, 7);
}

TEST_F(JogStateMachineTests, RateLadderIsClampedToHalfAndDouble) {
    jog.setRate(4.0);
    EXPECT_DOUBLE_EQ(jog.rate(), 2.0);
    jog.setRate(0.1);
    EXPECT_DOUBLE_EQ(jog.rate(), 0.5);

    // 0.5x at 30 fps: 200 ms of walk equals 3 frames of media time.
    host.presentedFrameFollowsRequests = true;
    jog.setRate(0.5);
    EXPECT_TRUE(jog.press(1, atMilliseconds(0)));
    jog.poll(atMilliseconds(250));
    jog.poll(atMilliseconds(450));
    jog.poll(atMilliseconds(466));
    EXPECT_EQ(host.submittedTotal(), 4); // tap + 3 paced frames
}

TEST_F(JogStateMachineTests, InvalidDirectionAndMissingPresentationAreRejected) {
    EXPECT_FALSE(jog.press(0, atMilliseconds(0)));
    EXPECT_FALSE(jog.press(5, atMilliseconds(0)));
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Idle);

    host.displayed = -1;
    EXPECT_FALSE(jog.press(1, atMilliseconds(0)));
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Idle);
    EXPECT_TRUE(host.submittedSteps.empty());
}

TEST_F(JogStateMachineTests, RejectedStepSubmissionPausesLeadGrowthWithoutCancelling) {
    host.acceptsSteps = false;
    EXPECT_FALSE(jog.press(1, atMilliseconds(0)));
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Idle);

    host.acceptsSteps = true;
    EXPECT_TRUE(jog.press(1, atMilliseconds(0)));
    host.acceptsSteps = false;
    jog.poll(atMilliseconds(1000));
    EXPECT_EQ(jog.phase(), JogStateMachine::Phase::Jogging);
    EXPECT_EQ(host.submittedSteps.size(), 1U);
}

} // namespace
} // namespace dvs::ui
