#include "dvs/platform/PresentationAckMailbox.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <thread>

namespace dvs::platform {
namespace {

[[nodiscard]] application::FrameSetPresented makeAcknowledgement(const std::uint64_t value) {
    return application::FrameSetPresented{
        .context =
            application::FrameRequestContext{
                .playback =
                    application::PlaybackRequestContext{
                        .request =
                            application::RequestContext{
                                .sessionId = domain::SessionId{1},
                                .sessionEpoch = domain::SessionEpoch{2},
                                .requestId = domain::RequestId{value},
                            },
                        .playbackGeneration = domain::PlaybackGeneration{3},
                    },
                .deviceGeneration = domain::DeviceGeneration{4},
            },
        .frameId = domain::FrameId{static_cast<std::int64_t>(value)},
    };
}

void expectAcknowledgement(const application::FrameSetPresented& actual,
                           const application::FrameSetPresented& expected) {
    EXPECT_EQ(actual.context, expected.context);
    EXPECT_EQ(actual.frameId, expected.frameId);
}

TEST(PresentationAckMailboxTests, PreservesFullCapacityUnderPressureWithoutOverwriting) {
    PresentationAckMailbox mailbox;
    const auto first = makeAcknowledgement(1U);
    const auto second = makeAcknowledgement(2U);
    const auto third = makeAcknowledgement(3U);
    const auto fourth = makeAcknowledgement(4U);
    const auto fifth = makeAcknowledgement(5U);

    ASSERT_EQ(mailbox.tryPush(first), PresentationAckPushResult::Accepted);
    ASSERT_EQ(mailbox.tryPush(second), PresentationAckPushResult::Accepted);
    ASSERT_EQ(mailbox.tryPush(third), PresentationAckPushResult::Accepted);
    ASSERT_EQ(mailbox.tryPush(fourth), PresentationAckPushResult::Accepted);
    EXPECT_EQ(mailbox.tryPush(fifth), PresentationAckPushResult::Full);

    std::optional<application::FrameSetPresented> popped = mailbox.tryPop();
    ASSERT_TRUE(popped.has_value());
    expectAcknowledgement(*popped, first);
    ASSERT_EQ(mailbox.tryPush(fifth), PresentationAckPushResult::Accepted);

    popped = mailbox.tryPop();
    ASSERT_TRUE(popped.has_value());
    expectAcknowledgement(*popped, second);
    popped = mailbox.tryPop();
    ASSERT_TRUE(popped.has_value());
    expectAcknowledgement(*popped, third);
    popped = mailbox.tryPop();
    ASSERT_TRUE(popped.has_value());
    expectAcknowledgement(*popped, fourth);
    popped = mailbox.tryPop();
    ASSERT_TRUE(popped.has_value());
    expectAcknowledgement(*popped, fifth);
    EXPECT_FALSE(mailbox.tryPop().has_value());
}

TEST(PresentationAckMailboxTests, CloseRejectsNewEntriesButAllowsQueuedEntriesToDrain) {
    PresentationAckMailbox mailbox;
    const application::FrameSetPresented first = makeAcknowledgement(1U);
    const application::FrameSetPresented second = makeAcknowledgement(2U);
    ASSERT_EQ(mailbox.tryPush(first), PresentationAckPushResult::Accepted);

    mailbox.close();
    EXPECT_TRUE(mailbox.isClosed());
    EXPECT_FALSE(mailbox.isDrained());
    EXPECT_EQ(mailbox.tryPush(second), PresentationAckPushResult::Closed);
    const std::optional<application::FrameSetPresented> popped = mailbox.tryPop();
    ASSERT_TRUE(popped.has_value());
    expectAcknowledgement(*popped, first);
    EXPECT_FALSE(mailbox.tryPop().has_value());
    EXPECT_TRUE(mailbox.isDrained());
    mailbox.close();
}

TEST(PresentationAckMailboxTests, SingleProducerAndConsumerTransferEveryEntryInOrder) {
    PresentationAckMailbox mailbox;
    constexpr std::uint64_t kAcknowledgementCount = 2'000U;

    std::thread producer([&mailbox] {
        for (std::uint64_t index = 0; index < kAcknowledgementCount; ++index) {
            const application::FrameSetPresented acknowledgement = makeAcknowledgement(index);
            while (mailbox.tryPush(acknowledgement) == PresentationAckPushResult::Full) {
                std::this_thread::yield();
            }
        }
    });

    for (std::uint64_t index = 0; index < kAcknowledgementCount; ++index) {
        std::optional<application::FrameSetPresented> acknowledgement;
        while (!(acknowledgement = mailbox.tryPop())) {
            std::this_thread::yield();
        }
        expectAcknowledgement(*acknowledgement, makeAcknowledgement(index));
    }
    producer.join();
    EXPECT_FALSE(mailbox.tryPop().has_value());
}

TEST(PresentationAckMailboxTests, ConcurrentCloseEitherPublishesBeforeCloseOrRejectsAtomically) {
    for (std::uint64_t iteration = 0U; iteration < 100U; ++iteration) {
        PresentationAckMailbox mailbox;
        const application::FrameSetPresented acknowledgement = makeAcknowledgement(iteration);
        std::atomic<bool> start{false};
        PresentationAckPushResult pushResult = PresentationAckPushResult::Full;

        std::thread producer([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            pushResult = mailbox.tryPush(acknowledgement);
        });
        start.store(true, std::memory_order_release);
        mailbox.close();
        producer.join();

        const std::optional<application::FrameSetPresented> popped = mailbox.tryPop();
        if (pushResult == PresentationAckPushResult::Accepted) {
            ASSERT_TRUE(popped.has_value());
            expectAcknowledgement(*popped, acknowledgement);
        } else {
            EXPECT_EQ(pushResult, PresentationAckPushResult::Closed);
            EXPECT_FALSE(popped.has_value());
        }
        EXPECT_TRUE(mailbox.isDrained());
    }
}

} // namespace
} // namespace dvs::platform
