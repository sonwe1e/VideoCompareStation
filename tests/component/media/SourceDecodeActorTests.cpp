#include "dvs/media/MediaProbe.h"
#include "dvs/platform/FrameBudget.h"

#include "SourceDecodeActor.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>

namespace dvs::media::internal {
namespace {

using namespace std::chrono_literals;

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate&& predicate, const std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

[[nodiscard]] std::filesystem::path fixture(const char* const name) {
    return std::filesystem::path{DVS_MEDIA_FIXTURE_DIR} / name;
}

[[nodiscard]] domain::MediaDescriptor descriptor(const char* const name) {
    const auto probed = MediaProbe::inspect(fixture(name), 0U);
    EXPECT_TRUE(probed);
    return probed.value();
}

TEST(SourceDecodeActorTests, ReusesOneWorkerAcrossRapidExactSeeks) {
    platform::FrameBudget budget{16U * 1024U * 1024U};
    std::atomic<bool> interrupted = false;
    std::atomic<bool> canceled = false;
    SourceDecodeActor actor{
        0U,
        descriptor("h265_a_320x180_30fps_12.mp4"),
        budget,
        &interrupted,
    };
    ASSERT_TRUE(actor.open(canceled));
    const std::thread::id worker = actor.workerThreadId();
    ASSERT_NE(worker, std::thread::id{});

    constexpr std::array<std::int64_t, 12U> kSeekOrder{
        0,
        11,
        1,
        10,
        2,
        9,
        3,
        8,
        4,
        7,
        5,
        6,
    };
    for (const std::int64_t frame : kSeekOrder) {
        SourceDecodeSubmission submitted = actor.submit(SourceDecodeRequest{
            .frameId = domain::FrameId{frame},
            .priority = SourceDecodePriority::Exact,
            .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
                &canceled, [](const std::atomic<bool>*) noexcept {}),
        });
        ASSERT_EQ(submitted.status, application::PortSubmitResult::Accepted);
        ASSERT_TRUE(submitted.completion.valid());
        const domain::Result<DecodedFrame> decoded = submitted.completion.get();
        ASSERT_TRUE(decoded) << decoded.error().technicalDetail;
        EXPECT_EQ(actor.workerThreadId(), worker);
        EXPECT_EQ(actor.lastDecodeThreadId(), worker);
    }
    EXPECT_EQ(actor.completedDecodeCount(), kSeekOrder.size());
}

TEST(SourceDecodeActorTests, ExactWorkDisplacesQueuedPrefetchWithoutCreatingWorkers) {
    platform::FrameBudget budget{16U * 1024U * 1024U};
    std::atomic<bool> interrupted = false;
    std::atomic<bool> canceled = false;
    SourceDecodeActor actor{
        0U,
        descriptor("h264_a_320x180_30fps_12.mp4"),
        budget,
        &interrupted,
    };
    ASSERT_TRUE(actor.open(canceled));
    const std::thread::id worker = actor.workerThreadId();

    std::vector<std::future<domain::Result<DecodedFrame>>> prefetch;
    for (std::int64_t frame = 1; frame < 12; ++frame) {
        SourceDecodeSubmission submitted = actor.submit(SourceDecodeRequest{
            .frameId = domain::FrameId{frame},
            .priority = SourceDecodePriority::Prefetch,
            .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
                &canceled, [](const std::atomic<bool>*) noexcept {}),
        });
        ASSERT_EQ(submitted.status, application::PortSubmitResult::Accepted);
        prefetch.push_back(std::move(submitted.completion));
    }

    SourceDecodeSubmission exact = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{0},
        .priority = SourceDecodePriority::Exact,
        .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
            &canceled, [](const std::atomic<bool>*) noexcept {}),
    });
    ASSERT_EQ(exact.status, application::PortSubmitResult::Accepted);
    const domain::Result<DecodedFrame> exactResult = exact.completion.get();
    ASSERT_TRUE(exactResult) << exactResult.error().technicalDetail;

    std::size_t displaced = 0U;
    for (auto& completion : prefetch) {
        if (!completion.get()) {
            ++displaced;
        }
    }
    EXPECT_GT(displaced, 0U);
    EXPECT_EQ(actor.workerThreadId(), worker);
    EXPECT_EQ(actor.lastDecodeThreadId(), worker);
}

TEST(SourceDecodeActorTests, ExactRequestReusesAPrefetchedSourceFrameAcrossRequestIdentity) {
    platform::FrameBudget budget{16U * 1024U * 1024U};
    std::atomic<bool> interrupted = false;
    std::atomic<bool> canceled = false;
    SourceDecodeActor actor{
        0U,
        descriptor("h264_a_320x180_30fps_12.mp4"),
        budget,
        &interrupted,
        false,
        2U * 1024U * 1024U,
    };
    ASSERT_TRUE(actor.open(canceled));

    SourceDecodeSubmission prefetch = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{6},
        .priority = SourceDecodePriority::Prefetch,
        .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
            &canceled, [](const std::atomic<bool>*) noexcept {}),
    });
    ASSERT_EQ(prefetch.status, application::PortSubmitResult::Accepted);
    domain::Result<DecodedFrame> prefetched = prefetch.completion.get();
    ASSERT_TRUE(prefetched) << prefetched.error().technicalDetail;
    ASSERT_EQ(actor.completedDecodeCount(), 1U);

    SourceDecodeSubmission exact = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{6},
        .priority = SourceDecodePriority::Exact,
        .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
            &canceled, [](const std::atomic<bool>*) noexcept {}),
    });
    ASSERT_EQ(exact.status, application::PortSubmitResult::Accepted);
    domain::Result<DecodedFrame> reused = exact.completion.get();
    ASSERT_TRUE(reused) << reused.error().technicalDetail;

    EXPECT_EQ(actor.completedDecodeCount(), 1U);
    EXPECT_EQ(prefetched.value().handle.resource(), reused.value().handle.resource());
    EXPECT_EQ(prefetched.value().presentationTime, reused.value().presentationTime);
}

TEST(SourceDecodeActorTests, ExactSuccessorPrefetchContinuesOnTheDedicatedDecoder) {
    platform::FrameBudget budget{16U * 1024U * 1024U};
    std::atomic<bool> interrupted = false;
    std::atomic<bool> canceled = false;
    SourceDecodeActor actor{
        0U,
        descriptor("h264_a_320x180_30fps_12.mp4"),
        budget,
        &interrupted,
        false,
        2U * 1024U * 1024U,
    };
    ASSERT_TRUE(actor.open(canceled));

    SourceDecodeSubmission exact = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{6},
        .priority = SourceDecodePriority::Exact,
        .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
            &canceled, [](const std::atomic<bool>*) noexcept {}),
    });
    ASSERT_EQ(exact.status, application::PortSubmitResult::Accepted);
    ASSERT_TRUE(exact.completion.get());
    ASSERT_EQ(actor.backendStatus().exactSeekCount, 1U);

    SourceDecodeSubmission prefetch = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{7},
        .priority = SourceDecodePriority::Prefetch,
        .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
            &canceled, [](const std::atomic<bool>*) noexcept {}),
    });
    ASSERT_EQ(prefetch.status, application::PortSubmitResult::Accepted);
    ASSERT_TRUE(prefetch.completion.get());

    EXPECT_EQ(actor.completedDecodeCount(), 2U);
    EXPECT_EQ(actor.backendStatus().exactSeekCount, 1U);
}

TEST(SourceDecodeActorTests, ReopensTheDedicatedDecoderAfterAnInterruptedRequest) {
    platform::FrameBudget budget{16U * 1024U * 1024U};
    std::atomic<bool> interrupted = false;
    std::atomic<bool> canceled = false;
    SourceDecodeActor actor{
        0U,
        descriptor("h264_a_320x180_30fps_12.mp4"),
        budget,
        &interrupted,
        false,
        2U * 1024U * 1024U,
    };
    ASSERT_TRUE(actor.open(canceled));

    interrupted.store(true, std::memory_order_release);
    SourceDecodeSubmission interruptedDecode = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{6},
        .priority = SourceDecodePriority::Exact,
        .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
            &canceled, [](const std::atomic<bool>*) noexcept {}),
    });
    ASSERT_EQ(interruptedDecode.status, application::PortSubmitResult::Accepted);
    ASSERT_FALSE(interruptedDecode.completion.get());

    interrupted.store(false, std::memory_order_release);
    SourceDecodeSubmission recovered = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{6},
        .priority = SourceDecodePriority::Exact,
        .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
            &canceled, [](const std::atomic<bool>*) noexcept {}),
    });
    ASSERT_EQ(recovered.status, application::PortSubmitResult::Accepted);
    const auto decoded = recovered.completion.get();
    ASSERT_TRUE(decoded) << decoded.error().technicalDetail;
    EXPECT_EQ(decoded.value().presentationTime, domain::MediaTime{200000});
}

TEST(SourceDecodeActorTests, SequentialRequestReusesAPrefetchedSourceFrameWithoutDecodingAgain) {
    platform::FrameBudget budget{16U * 1024U * 1024U};
    std::atomic<bool> interrupted = false;
    std::atomic<bool> canceled = false;
    SourceDecodeActor actor{
        0U,
        descriptor("h264_a_320x180_30fps_12.mp4"),
        budget,
        &interrupted,
        false,
        2U * 1024U * 1024U,
    };
    ASSERT_TRUE(actor.open(canceled));

    SourceDecodeSubmission prefetch = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{1},
        .priority = SourceDecodePriority::Prefetch,
        .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
            &canceled, [](const std::atomic<bool>*) noexcept {}),
    });
    ASSERT_EQ(prefetch.status, application::PortSubmitResult::Accepted);
    domain::Result<DecodedFrame> prefetched = prefetch.completion.get();
    ASSERT_TRUE(prefetched) << prefetched.error().technicalDetail;
    ASSERT_EQ(actor.completedDecodeCount(), 1U);

    SourceDecodeSubmission sequential = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{1},
        .priority = SourceDecodePriority::Sequential,
        .continueSequentially = true,
        .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
            &canceled, [](const std::atomic<bool>*) noexcept {}),
    });
    ASSERT_EQ(sequential.status, application::PortSubmitResult::Accepted);
    domain::Result<DecodedFrame> reused = sequential.completion.get();
    ASSERT_TRUE(reused) << reused.error().technicalDetail;

    EXPECT_EQ(actor.completedDecodeCount(), 1U);
    EXPECT_EQ(prefetched.value().handle.resource(), reused.value().handle.resource());
    EXPECT_EQ(prefetched.value().presentationTime, reused.value().presentationTime);
}

TEST(SourceDecodeActorTests, SequentialReadAheadFillsOnlyTheSourceFrameCache) {
    platform::FrameBudget budget{16U * 1024U * 1024U};
    std::atomic<bool> interrupted = false;
    std::atomic<bool> canceled = false;
    SourceDecodeActor actor{
        0U,
        descriptor("h264_a_320x180_30fps_12.mp4"),
        budget,
        &interrupted,
        false,
        2U * 1024U * 1024U,
    };
    ASSERT_TRUE(actor.open(canceled));

    SourceDecodeSubmission first = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{0},
        .priority = SourceDecodePriority::Sequential,
        .readAheadCount = 3U,
        .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
            &canceled, [](const std::atomic<bool>*) noexcept {}),
    });
    ASSERT_EQ(first.status, application::PortSubmitResult::Accepted);
    ASSERT_TRUE(first.completion.get());
    ASSERT_TRUE(waitUntil([&actor] { return actor.completedDecodeCount() == 4U; }));

    SourceDecodeSubmission cached = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{1},
        .priority = SourceDecodePriority::Sequential,
        .continueSequentially = true,
        .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
            &canceled, [](const std::atomic<bool>*) noexcept {}),
    });
    ASSERT_EQ(cached.status, application::PortSubmitResult::Accepted);
    ASSERT_TRUE(cached.completion.get());
    EXPECT_EQ(actor.completedDecodeCount(), 4U);
    EXPECT_EQ(actor.backendStatus().cacheHitCount, 1U);
}

TEST(SourceDecodeActorTests, SequentialReadAheadSkipsAndClearsAOneFrameCache) {
    platform::FrameBudget budget{16U * 1024U * 1024U};
    std::atomic<bool> interrupted = false;
    std::atomic<bool> canceled = false;
    SourceDecodeActor actor{
        0U,
        descriptor("h264_a_320x180_30fps_12.mp4"),
        budget,
        &interrupted,
        false,
        100U * 1024U,
    };
    ASSERT_TRUE(actor.open(canceled));

    SourceDecodeSubmission first = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{0},
        .priority = SourceDecodePriority::Exact,
        .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
            &canceled, [](const std::atomic<bool>*) noexcept {}),
    });
    ASSERT_EQ(first.status, application::PortSubmitResult::Accepted);
    ASSERT_TRUE(first.completion.get());

    SourceDecodeSubmission sequential = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{1},
        .priority = SourceDecodePriority::Sequential,
        .continueSequentially = true,
        .readAheadCount = 3U,
        .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
            &canceled, [](const std::atomic<bool>*) noexcept {}),
    });
    ASSERT_EQ(sequential.status, application::PortSubmitResult::Accepted);
    ASSERT_TRUE(sequential.completion.get());
    ASSERT_EQ(actor.completedDecodeCount(), 2U);

    SourceDecodeSubmission exactAgain = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{0},
        .priority = SourceDecodePriority::Exact,
        .cancellationRequested = std::shared_ptr<const std::atomic<bool>>(
            &canceled, [](const std::atomic<bool>*) noexcept {}),
    });
    ASSERT_EQ(exactAgain.status, application::PortSubmitResult::Accepted);
    ASSERT_TRUE(exactAgain.completion.get());
    EXPECT_EQ(actor.completedDecodeCount(), 3U);
    EXPECT_EQ(actor.backendStatus().cacheHitCount, 0U);
}

// Regression for the read-ahead cancellation lifetime defect: the request's cancellation flag
// is destroyed together with the submitter's only owner before read-ahead starts. The actor
// must hold its own shared copy so speculative decoding stays well-defined; it also must
// remain cancelable through that copy.
TEST(SourceDecodeActorTests, ReadAheadSurvivesRequestCancellationFlagRelease) {
    platform::FrameBudget budget{16U * 1024U * 1024U};
    std::atomic<bool> interrupted = false;
    auto canceled = std::make_shared<std::atomic<bool>>(false);
    SourceDecodeActor actor{
        0U,
        descriptor("h264_a_320x180_30fps_12.mp4"),
        budget,
        &interrupted,
        false,
        2U * 1024U * 1024U,
    };
    ASSERT_TRUE(actor.open(*canceled));

    SourceDecodeSubmission first;
    {
        const std::shared_ptr<const std::atomic<bool>> requestFlag = canceled;
        first = actor.submit(SourceDecodeRequest{
            .frameId = domain::FrameId{0},
            .priority = SourceDecodePriority::Sequential,
            .readAheadCount = 3U,
            .cancellationRequested = requestFlag,
        });
        ASSERT_EQ(first.status, application::PortSubmitResult::Accepted);
    }
    // The requester's scope ended; the submitted request now shares the only owners besides the
    // actor. Wait for the completion plus the three read-ahead decodes before dropping the last
    // named owner, so the cache path (already exercised) and the fresh-decode path both released
    // the submitter's callback lifetime first.
    ASSERT_TRUE(first.completion.get());
    ASSERT_TRUE(waitUntil([&actor] { return actor.completedDecodeCount() == 4U; }));

    // Submit one more sequential request and release the operation flag while its read-ahead is
    // potentially still pending; the actor's copy of the flag must stay valid and remain
    // writable by the provider side.
    SourceDecodeSubmission second;
    {
        const std::shared_ptr<const std::atomic<bool>> requestFlag = canceled;
        second = actor.submit(SourceDecodeRequest{
            .frameId = domain::FrameId{4},
            .priority = SourceDecodePriority::Sequential,
            .continueSequentially = true,
            .readAheadCount = 3U,
            .cancellationRequested = requestFlag,
        });
        ASSERT_EQ(second.status, application::PortSubmitResult::Accepted);
    }
    ASSERT_TRUE(second.completion.get());
    // The supplier cancels the superseded operation after admission, through its own owner of
    // the same shared flag; read-ahead must observe it and stop without touching freed state.
    canceled->store(true, std::memory_order_release);
    ASSERT_TRUE(waitUntil([&] { return actor.completedDecodeCount() >= 5U; }));

    // The interrupted decoder requires an explicit reopen before the next exact request.
    canceled->store(false, std::memory_order_release);
    SourceDecodeSubmission afterCancel = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{8},
        .priority = SourceDecodePriority::Exact,
        .cancellationRequested = canceled,
    });
    ASSERT_EQ(afterCancel.status, application::PortSubmitResult::Accepted);
    const domain::Result<DecodedFrame> recovered = afterCancel.completion.get();
    ASSERT_TRUE(recovered) << recovered.error().technicalDetail;
    EXPECT_EQ(recovered.value().presentationTime, domain::MediaTime{266667});
}

// Regression for the requestInterrupt data race: an external thread signals the decoder while
// the worker is decoding, and the worker then continues sequentially from its own state. The
// signal must not write worker-owned members; the sequential path still works from the
// worker's own sequentialReady bookkeeping after the interrupted decoders reopen.
TEST(SourceDecodeActorTests, ExternalInterruptSignalRacesSequentialDecodeSafely) {
    platform::FrameBudget budget{16U * 1024U * 1024U};
    std::atomic<bool> interrupted = false;
    auto canceled = std::make_shared<std::atomic<bool>>(false);
    SourceDecodeActor actor{
        0U,
        descriptor("h264_a_320x180_30fps_12.mp4"),
        budget,
        &interrupted,
        false,
        2U * 1024U * 1024U,
    };
    ASSERT_TRUE(actor.open(*canceled));

    // Hold the worker under continuous interrupt signaling from an unrelated thread while it
    // decodes; requestInterrupt must stay a signal and never write worker-owned state.
    std::atomic<bool> stopSignaling{false};
    std::atomic<bool> signalerStarted{false};
    std::thread signaler{[&actor, &stopSignaling, &signalerStarted] {
        signalerStarted.store(true, std::memory_order_release);
        while (!stopSignaling.load(std::memory_order_acquire)) {
            actor.requestInterrupt();
        }
    }};
    ASSERT_TRUE(
        waitUntil([&signalerStarted] { return signalerStarted.load(std::memory_order_acquire); }));
    for (int round = 0; round < 8; ++round) {
        SourceDecodeSubmission racing = actor.submit(SourceDecodeRequest{
            .frameId = domain::FrameId{1},
            .priority = SourceDecodePriority::Exact,
            .cancellationRequested = canceled,
        });
        ASSERT_EQ(racing.status, application::PortSubmitResult::Accepted);
        static_cast<void>(racing.completion.get());
    }
    stopSignaling.store(true, std::memory_order_release);
    signaler.join();

    // The actor's decoders reopen after interruption; the sequential path must still decode
    // correctly from worker-owned state written only by the worker.
    SourceDecodeSubmission recovered = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{0},
        .priority = SourceDecodePriority::Exact,
        .cancellationRequested = canceled,
    });
    ASSERT_EQ(recovered.status, application::PortSubmitResult::Accepted);
    const auto first = recovered.completion.get();
    ASSERT_TRUE(first) << first.error().technicalDetail;

    SourceDecodeSubmission next = actor.submit(SourceDecodeRequest{
        .frameId = domain::FrameId{1},
        .priority = SourceDecodePriority::Sequential,
        .continueSequentially = true,
        .readAheadCount = 2U,
        .cancellationRequested = canceled,
    });
    ASSERT_EQ(next.status, application::PortSubmitResult::Accepted);
    const auto sequential = next.completion.get();
    ASSERT_TRUE(sequential) << sequential.error().technicalDetail;
    EXPECT_EQ(sequential.value().presentationTime, domain::MediaTime{33333});
    ASSERT_TRUE(waitUntil([&actor] { return actor.completedDecodeCount() >= 3U; }));
}

} // namespace
} // namespace dvs::media::internal
