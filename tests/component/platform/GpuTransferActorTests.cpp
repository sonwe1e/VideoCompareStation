#include "dvs/platform/D3d11DecodedFrameResource.h"
#include "dvs/platform/D3d11RenderChannel.h"
#include "dvs/platform/FrameBudget.h"
#include "dvs/platform/FrameMailbox.h"
#include "dvs/platform/FrameResourceFactory.h"
#include "dvs/platform/GpuTransferActor.h"
#include "dvs/platform/GraphicsDeviceBroker.h"

#include "D3d11GpuFrameBacking.h"
#include "GpuFrameResource.h"
#include "GpuFrameSet.h"
#include "MinimalQuickRenderHarness.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <d3d11.h>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>
#include <wrl/client.h>

namespace dvs::platform {
namespace {

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

struct SourceBytes final {
    std::vector<std::uint8_t> y;
    std::vector<std::uint8_t> uv;
    std::vector<std::uint8_t> visibleY;
    std::vector<std::uint8_t> visibleUv;
};

[[nodiscard]] application::FrameRequestContext
makeContext(const std::uint64_t requestId = 1U,
            const std::uint64_t deviceGeneration = 1U,
            const std::uint64_t playbackGeneration = 1U) {
    return application::FrameRequestContext{
        .playback =
            application::PlaybackRequestContext{
                .request =
                    application::RequestContext{
                        .sessionId = domain::SessionId{21U},
                        .sessionEpoch = domain::SessionEpoch{4U},
                        .requestId = domain::RequestId{requestId},
                    },
                .playbackGeneration = domain::PlaybackGeneration{playbackGeneration},
            },
        .deviceGeneration = domain::DeviceGeneration{deviceGeneration},
    };
}

[[nodiscard]] SourceBytes makeSourceBytes(const Nv12FrameLayout& layout, const std::uint8_t seed) {
    const std::size_t uvWidth = (static_cast<std::size_t>(layout.width) + 1U) / 2U;
    const std::size_t uvHeight = (static_cast<std::size_t>(layout.height) + 1U) / 2U;
    const std::size_t uvRowBytes = uvWidth * 2U;
    SourceBytes bytes{
        .y = std::vector<std::uint8_t>(static_cast<std::size_t>(layout.yStride) * layout.height,
                                       0xEEU),
        .uv =
            std::vector<std::uint8_t>(static_cast<std::size_t>(layout.uvStride) * uvHeight, 0xDDU),
        .visibleY =
            std::vector<std::uint8_t>(static_cast<std::size_t>(layout.width) * layout.height),
        .visibleUv = std::vector<std::uint8_t>(uvRowBytes * uvHeight),
    };

    for (std::uint32_t row = 0U; row < layout.height; ++row) {
        for (std::uint32_t column = 0U; column < layout.width; ++column) {
            const auto value = static_cast<std::uint8_t>(seed + row * 11U + column);
            bytes.y[static_cast<std::size_t>(row) * layout.yStride + column] = value;
            bytes.visibleY[static_cast<std::size_t>(row) * layout.width + column] = value;
        }
    }
    for (std::size_t row = 0U; row < uvHeight; ++row) {
        for (std::size_t column = 0U; column < uvRowBytes; ++column) {
            const auto value = static_cast<std::uint8_t>(seed + 80U + row * 7U + column);
            bytes.uv[row * layout.uvStride + column] = value;
            bytes.visibleUv[row * uvRowBytes + column] = value;
        }
    }
    return bytes;
}

[[nodiscard]] std::optional<application::FrameSet> makeCpuSet(FrameBudget& budget,
                                                              const Nv12FrameLayout& layout,
                                                              const SourceBytes& sourceA,
                                                              const SourceBytes& sourceB,
                                                              const std::int64_t frameId = 3) {
    FrameResourceFactory factory{budget};
    const domain::ColorMetadata colorA{
        .matrix = domain::ColorMatrix::kBt601,
        .range = domain::ColorRange::kLimited,
        .matrixInferred = false,
    };
    const domain::ColorMetadata colorB{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kFull,
        .matrixInferred = true,
    };
    const std::optional<application::FrameHandle> frameA =
        factory.createCpuNv12(layout, colorA, std::span{sourceA.y}, std::span{sourceA.uv});
    const std::optional<application::FrameHandle> frameB =
        factory.createCpuNv12(layout, colorB, std::span{sourceB.y}, std::span{sourceB.uv});
    if (!frameA || !frameB) {
        return std::nullopt;
    }

    std::vector<application::MappedSourceFrame> sources{
        application::MappedSourceFrame{
            .sourceId = 0U,
            .sourceFrameId = domain::FrameId{frameId},
            .frame = *frameA,
            .presentationTime = domain::MediaTime{100'000},
            .matchKind = application::FrameMatchKind::ExactIndex,
            .alignmentConfidence = 1.0F,
        },
        application::MappedSourceFrame{
            .sourceId = 1U,
            .sourceFrameId = domain::FrameId{frameId},
            .frame = *frameB,
            .presentationTime = domain::MediaTime{100'000},
            .matchKind = application::FrameMatchKind::ExactIndex,
            .alignmentConfidence = 1.0F,
        },
    };
    return application::FrameSet::create(
        domain::FrameId{frameId}, domain::MediaTime{100'000}, std::move(sources));
}

[[nodiscard]] std::optional<application::FrameSet>
makeThreeSourceCpuSet(FrameBudget& budget,
                      const Nv12FrameLayout& layout,
                      const SourceBytes& sourceA,
                      const SourceBytes& sourceB,
                      const SourceBytes& sourceC,
                      const std::int64_t frameId = 3) {
    FrameResourceFactory factory{budget};
    const domain::ColorMetadata color{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kFull,
        .matrixInferred = false,
    };
    std::array<std::optional<application::FrameHandle>, 3U> frames{
        factory.createCpuNv12(layout, color, std::span{sourceA.y}, std::span{sourceA.uv}),
        factory.createCpuNv12(layout, color, std::span{sourceB.y}, std::span{sourceB.uv}),
        factory.createCpuNv12(layout, color, std::span{sourceC.y}, std::span{sourceC.uv}),
    };
    if (!frames[0U] || !frames[1U] || !frames[2U]) {
        return std::nullopt;
    }
    std::vector<application::MappedSourceFrame> sources;
    sources.reserve(frames.size());
    for (std::size_t index = 0U; index < frames.size(); ++index) {
        sources.push_back(application::MappedSourceFrame{
            .sourceId = static_cast<domain::SourceId>(index),
            .sourceFrameId = domain::FrameId{frameId},
            .frame = *frames[index],
            .presentationTime = domain::MediaTime{100'000},
            .matchKind = application::FrameMatchKind::ExactIndex,
            .alignmentConfidence = 1.0F,
        });
    }
    return application::FrameSet::create(
        domain::FrameId{frameId}, domain::MediaTime{100'000}, std::move(sources));
}

[[nodiscard]] std::optional<application::FrameSet>
makeCpuSetWithMissing(FrameBudget& budget,
                      const Nv12FrameLayout& layout,
                      const SourceBytes& sourceA,
                      const std::int64_t frameId = 3) {
    FrameResourceFactory factory{budget};
    const domain::ColorMetadata colorA{
        .matrix = domain::ColorMatrix::kBt601,
        .range = domain::ColorRange::kLimited,
        .matrixInferred = false,
    };
    const std::optional<application::FrameHandle> frameA =
        factory.createCpuNv12(layout, colorA, std::span{sourceA.y}, std::span{sourceA.uv});
    if (!frameA) {
        return std::nullopt;
    }

    std::vector<application::MappedSourceFrame> sources{
        application::MappedSourceFrame{
            .sourceId = 0U,
            .sourceFrameId = domain::FrameId{frameId},
            .frame = *frameA,
            .presentationTime = domain::MediaTime{100'000},
            .matchKind = application::FrameMatchKind::ExactIndex,
            .alignmentConfidence = 1.0F,
        },
        application::MappedSourceFrame{
            .sourceId = 1U,
            .sourceFrameId = std::nullopt,
            .frame = std::nullopt,
            .presentationTime = domain::MediaTime{0},
            .matchKind = application::FrameMatchKind::Missing,
            .alignmentConfidence = 1.0F,
            .missingReason = application::MissingReason::AlignmentGap,
        },
    };
    return application::FrameSet::create(
        domain::FrameId{frameId}, domain::MediaTime{100'000}, std::move(sources));
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        QThread::yieldCurrentThread();
    }
    return predicate();
}

[[nodiscard]] std::optional<FrameMailboxPublication>
waitForPublication(FrameMailbox& mailbox,
                   const domain::DeviceGeneration generation,
                   const std::chrono::milliseconds timeout = 5s) {
    std::optional<FrameMailboxPublication> publication;
    const bool available = waitUntil(
        [&] {
            const FrameMailboxReadResult read = mailbox.tryLatest(generation);
            if (read.status == FrameMailboxReadStatus::Available && read.publication) {
                publication = *read.publication;
                return true;
            }
            return false;
        },
        timeout);
    return available ? publication : std::nullopt;
}

[[nodiscard]] std::vector<std::uint8_t> readTexture(ID3D11Device* const device,
                                                    ID3D11DeviceContext* const context,
                                                    ID3D11Texture2D* const source,
                                                    const std::uint32_t visibleRowBytes) {
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0U;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0U;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&description, nullptr, staging.GetAddressOf()))) {
        return {};
    }
    context->CopyResource(staging.Get(), source);
    context->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped)) ||
        mapped.pData == nullptr || mapped.RowPitch < visibleRowBytes) {
        return {};
    }
    std::vector<std::uint8_t> result(static_cast<std::size_t>(visibleRowBytes) *
                                     description.Height);
    const auto* const bytes = static_cast<const std::uint8_t*>(mapped.pData);
    for (std::uint32_t row = 0U; row < description.Height; ++row) {
        std::copy_n(bytes + static_cast<std::size_t>(row) * mapped.RowPitch,
                    visibleRowBytes,
                    result.data() + static_cast<std::size_t>(row) * visibleRowBytes);
    }
    context->Unmap(staging.Get(), 0U);
    return result;
}

struct WarpRuntime final {
    std::shared_ptr<GraphicsDeviceBroker> broker = std::make_shared<GraphicsDeviceBroker>();
    test::MinimalQuickRenderHarness harness{*broker};

    [[nodiscard]] bool start() {
        if (!harness.showAndWait()) {
            return false;
        }
        static_cast<void>(broker->tryConsumeNotification());
        return broker->currentGeneration() == domain::DeviceGeneration{1U};
    }
};

TEST(GpuTransferActorTests, UploadsPaddedOddNv12PlanesAndPublishesOneImmutablePair) {
    WarpRuntime runtime;
    ASSERT_TRUE(runtime.start());
    auto budget = std::make_shared<FrameBudget>(1U << 20U);
    auto mailbox = std::make_shared<FrameMailbox>(domain::DeviceGeneration{1U});
    GpuTransferActor actor{budget, runtime.broker, mailbox};

    const Nv12FrameLayout layout{
        .width = 5U,
        .height = 3U,
        .yStride = 8U,
        .uvStride = 8U,
    };
    const SourceBytes sourceA = makeSourceBytes(layout, 10U);
    const SourceBytes sourceB = makeSourceBytes(layout, 120U);
    std::optional<application::FrameSet> pair = makeCpuSet(*budget, layout, sourceA, sourceB);
    ASSERT_TRUE(pair.has_value());

    const application::FrameRequestContext context = makeContext();
    EXPECT_EQ(actor.submit(context, std::move(*pair)), GpuTransferSubmitResult::Accepted);
    std::optional<FrameMailboxPublication> publication =
        waitForPublication(*mailbox, context.deviceGeneration);
    ASSERT_TRUE(publication.has_value());
    ASSERT_NE(publication->set, nullptr);
    EXPECT_EQ(publication->set->frameId(), domain::FrameId{3});
    EXPECT_EQ(publication->set->context(), context);

    const GpuFrameSlot* const slotA = publication->set->find(0U);
    const GpuFrameSlot* const slotB = publication->set->find(1U);
    ASSERT_NE(slotA, nullptr);
    ASSERT_NE(slotB, nullptr);
    const auto* const backingA =
        dynamic_cast<const D3d11GpuFrameBacking*>(&slotA->frame->backing());
    const auto* const backingB =
        dynamic_cast<const D3d11GpuFrameBacking*>(&slotB->frame->backing());
    ASSERT_NE(backingA, nullptr);
    ASSERT_NE(backingB, nullptr);
    EXPECT_EQ(backingA->yDimensions(), (D3d11PlaneDimensions{.width = 5U, .height = 3U}));
    EXPECT_EQ(backingA->uvDimensions(), (D3d11PlaneDimensions{.width = 3U, .height = 2U}));
    EXPECT_EQ(slotA->frame->colorMetadata().matrix, domain::ColorMatrix::kBt601);
    EXPECT_EQ(slotB->frame->colorMetadata().range, domain::ColorRange::kFull);

    const GraphicsDeviceLeaseResult lease = runtime.broker->tryLease();
    ASSERT_EQ(lease.status, GraphicsDeviceLeaseStatus::Available);
    ASSERT_TRUE(lease.lease.has_value());
    EXPECT_EQ(readTexture(lease.lease->device.Get(),
                          lease.lease->immediateContext.Get(),
                          backingA->yTexture(),
                          layout.width),
              sourceA.visibleY);
    EXPECT_EQ(readTexture(lease.lease->device.Get(),
                          lease.lease->immediateContext.Get(),
                          backingA->uvTexture(),
                          6U),
              sourceA.visibleUv);
    EXPECT_EQ(readTexture(lease.lease->device.Get(),
                          lease.lease->immediateContext.Get(),
                          backingB->yTexture(),
                          layout.width),
              sourceB.visibleY);
    EXPECT_EQ(readTexture(lease.lease->device.Get(),
                          lease.lease->immediateContext.Get(),
                          backingB->uvTexture(),
                          6U),
              sourceB.visibleUv);

    EXPECT_TRUE(mailbox->clear(context.playback));
    publication.reset();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(GpuTransferActorTests, ClearTombstoneAndDeviceGenerationRejectLateOrStalePairs) {
    WarpRuntime runtime;
    ASSERT_TRUE(runtime.start());
    auto budget = std::make_shared<FrameBudget>(1U << 20U);
    auto mailbox = std::make_shared<FrameMailbox>(domain::DeviceGeneration{1U});
    GpuTransferActor actor{budget, runtime.broker, mailbox};
    D3d11RenderChannel channel{std::shared_ptr<GpuTransferActor>{&actor, [](GpuTransferActor*) {}}};

    const Nv12FrameLayout layout{
        .width = 8U,
        .height = 4U,
        .yStride = 8U,
        .uvStride = 8U,
    };
    const SourceBytes sourceA = makeSourceBytes(layout, 1U);
    const SourceBytes sourceB = makeSourceBytes(layout, 2U);
    const application::FrameRequestContext context = makeContext();
    std::optional<application::FrameSet> first = makeCpuSet(*budget, layout, sourceA, sourceB);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(channel.publish(context, std::move(*first)),
              application::RenderPublishResult::Accepted);
    channel.clear(context.playback);
    ASSERT_TRUE(actor.waitUntilIdle(2s));
    EXPECT_NE(mailbox->tryLatest(context.deviceGeneration).status,
              FrameMailboxReadStatus::Available);

    std::optional<application::FrameSet> late = makeCpuSet(*budget, layout, sourceA, sourceB);
    ASSERT_TRUE(late.has_value());
    EXPECT_EQ(actor.submit(context, std::move(*late)), GpuTransferSubmitResult::StaleContext);

    std::optional<application::FrameSet> wrongGeneration =
        makeCpuSet(*budget, layout, sourceA, sourceB);
    ASSERT_TRUE(wrongGeneration.has_value());
    EXPECT_EQ(actor.submit(makeContext(2U, 2U, 2U), std::move(*wrongGeneration)),
              GpuTransferSubmitResult::StaleContext);
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(GpuTransferActorTests, FinalRenderReleaseRetiresComAndBudgetOnTheActorWorker) {
    WarpRuntime runtime;
    ASSERT_TRUE(runtime.start());
    auto budget = std::make_shared<FrameBudget>(1U << 20U);
    auto mailbox = std::make_shared<FrameMailbox>(domain::DeviceGeneration{1U});
    GpuTransferActor actor{budget, runtime.broker, mailbox};

    const Nv12FrameLayout layout{
        .width = 5U,
        .height = 3U,
        .yStride = 8U,
        .uvStride = 8U,
    };
    const SourceBytes sourceA = makeSourceBytes(layout, 30U);
    const SourceBytes sourceB = makeSourceBytes(layout, 60U);
    std::optional<application::FrameSet> pair = makeCpuSet(*budget, layout, sourceA, sourceB);
    ASSERT_TRUE(pair.has_value());
    const application::FrameRequestContext context = makeContext();
    ASSERT_EQ(actor.submit(context, std::move(*pair)), GpuTransferSubmitResult::Accepted);
    std::optional<FrameMailboxPublication> publication =
        waitForPublication(*mailbox, context.deviceGeneration);
    ASSERT_TRUE(publication.has_value());
    ASSERT_TRUE(actor.waitUntilIdle(2s));

    const std::size_t gpuBytesPerSource = 5U * 3U + 6U * 2U;
    ASSERT_EQ(budget->reservedBytes(), gpuBytesPerSource * 2U);
    ASSERT_TRUE(mailbox->clear(context.playback));
    ASSERT_EQ(budget->reservedBytes(), gpuBytesPerSource * 2U);

    std::thread::id releaseThread;
    std::thread renderRelease{[&publication, &releaseThread] {
        releaseThread = std::this_thread::get_id();
        publication.reset();
    }};
    renderRelease.join();

    ASSERT_TRUE(waitUntil([&budget] { return budget->reservedBytes() == 0U; }, 2s));
    ASSERT_TRUE(waitUntil([&actor] { return actor.statistics().retiredResources == 2U; }, 2s));
    const GpuTransferStatistics statistics = actor.statistics();
    EXPECT_EQ(statistics.retiredResources, 2U);
    EXPECT_EQ(statistics.lastRetirementThread, statistics.workerThread);
    EXPECT_NE(statistics.lastRetirementThread, releaseThread);
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(GpuTransferActorTests, RollsBackAdmissionBudgetAndShutsDownWithinTwoSeconds) {
    WarpRuntime runtime;
    ASSERT_TRUE(runtime.start());
    const Nv12FrameLayout layout{
        .width = 8U,
        .height = 4U,
        .yStride = 8U,
        .uvStride = 8U,
    };
    const SourceBytes sourceA = makeSourceBytes(layout, 4U);
    const SourceBytes sourceB = makeSourceBytes(layout, 8U);

    // The two CPU resources fit (96 bytes), but no staging or destination reservation can.
    auto budget = std::make_shared<FrameBudget>(100U);
    auto mailbox = std::make_shared<FrameMailbox>(domain::DeviceGeneration{1U});
    GpuTransferActor actor{budget, runtime.broker, mailbox};
    std::optional<application::FrameSet> pair = makeCpuSet(*budget, layout, sourceA, sourceB);
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor.submit(makeContext(), std::move(*pair)), GpuTransferSubmitResult::Accepted);
    ASSERT_TRUE(actor.waitUntilIdle(2s));
    EXPECT_EQ(budget->reservedBytes(), 0U);
    EXPECT_EQ(mailbox->tryLatest(domain::DeviceGeneration{1U}).status,
              FrameMailboxReadStatus::Empty);

    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(actor.shutdown(2s));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
    EXPECT_TRUE(actor.isClosed());
}

TEST(GpuTransferActorTests, RejectsAdmissionWhileTheBrokerHasNoAvailableLease) {
    auto broker = std::make_shared<GraphicsDeviceBroker>();
    ASSERT_EQ(broker->reportUnavailable("No scene-graph device is available."),
              GraphicsDeviceBrokerResult::Unavailable);
    auto budget = std::make_shared<FrameBudget>(1U << 20U);
    auto mailbox = std::make_shared<FrameMailbox>(domain::DeviceGeneration{1U});
    GpuTransferActor actor{budget, broker, mailbox};
    const Nv12FrameLayout layout{
        .width = 4U,
        .height = 2U,
        .yStride = 4U,
        .uvStride = 4U,
    };
    const SourceBytes sourceA = makeSourceBytes(layout, 1U);
    const SourceBytes sourceB = makeSourceBytes(layout, 2U);
    std::optional<application::FrameSet> pair = makeCpuSet(*budget, layout, sourceA, sourceB);
    ASSERT_TRUE(pair.has_value());

    EXPECT_EQ(actor.submit(makeContext(1U, 1U), std::move(*pair)),
              GpuTransferSubmitResult::DeviceUnavailable);
    EXPECT_EQ(budget->reservedBytes(), 0U);
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(GpuTransferActorTests, TimedOutActorDestructionKeepsDependenciesUntilLateRenderRetirement) {
    WarpRuntime runtime;
    ASSERT_TRUE(runtime.start());
    auto budget = std::make_shared<FrameBudget>(1U << 20U);
    auto mailbox = std::make_shared<FrameMailbox>(domain::DeviceGeneration{1U});
    auto actor = std::make_unique<GpuTransferActor>(budget, runtime.broker, mailbox);
    const Nv12FrameLayout layout{
        .width = 5U,
        .height = 3U,
        .yStride = 8U,
        .uvStride = 8U,
    };
    const SourceBytes sourceA = makeSourceBytes(layout, 15U);
    const SourceBytes sourceB = makeSourceBytes(layout, 45U);
    std::optional<application::FrameSet> pair = makeCpuSet(*budget, layout, sourceA, sourceB);
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor->submit(makeContext(), std::move(*pair)), GpuTransferSubmitResult::Accepted);
    std::optional<FrameMailboxPublication> publication =
        waitForPublication(*mailbox, domain::DeviceGeneration{1U});
    ASSERT_TRUE(publication.has_value());

    const std::weak_ptr<FrameBudget> weakBudget = budget;
    const std::weak_ptr<FrameMailbox> weakMailbox = mailbox;
    const auto started = std::chrono::steady_clock::now();
    actor.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2500ms);

    budget.reset();
    mailbox.reset();
    EXPECT_FALSE(weakBudget.expired());
    EXPECT_FALSE(weakMailbox.expired());

    std::thread renderRelease{[&publication] { publication.reset(); }};
    renderRelease.join();
    EXPECT_TRUE(waitUntil(
        [&weakBudget, &weakMailbox] { return weakBudget.expired() && weakMailbox.expired(); }, 2s));
}

TEST(GpuTransferActorTests, RejectsWorkAfterBrokerDeviceLossTransition) {
    WarpRuntime runtime;
    ASSERT_TRUE(runtime.start());
    auto budget = std::make_shared<FrameBudget>(1U << 20U);
    auto mailbox = std::make_shared<FrameMailbox>(domain::DeviceGeneration{1U});
    GpuTransferActor actor{budget, runtime.broker, mailbox};

    const Nv12FrameLayout layout{
        .width = 4U,
        .height = 2U,
        .yStride = 4U,
        .uvStride = 4U,
    };
    const SourceBytes sourceA = makeSourceBytes(layout, 1U);
    const SourceBytes sourceB = makeSourceBytes(layout, 2U);
    std::optional<application::FrameSet> pair = makeCpuSet(*budget, layout, sourceA, sourceB);
    ASSERT_TRUE(pair.has_value());
    ASSERT_EQ(actor.submit(makeContext(), std::move(*pair)), GpuTransferSubmitResult::Accepted);
    std::optional<FrameMailboxPublication> publication =
        waitForPublication(*mailbox, domain::DeviceGeneration{1U});
    ASSERT_TRUE(publication.has_value());

    ASSERT_EQ(
        runtime.broker->reportDeviceLost(domain::DeviceGeneration{1U}, DXGI_ERROR_DEVICE_REMOVED),
        GraphicsDeviceBrokerResult::Lost);
    ASSERT_TRUE(waitUntil([&actor] { return actor.isClosed(); }, 2s));
    EXPECT_NE(mailbox->tryLatest(domain::DeviceGeneration{2U}).status,
              FrameMailboxReadStatus::Available);

    std::optional<application::FrameSet> stale = makeCpuSet(*budget, layout, sourceA, sourceB);
    ASSERT_TRUE(stale.has_value());
    EXPECT_EQ(actor.submit(makeContext(), std::move(*stale)),
              GpuTransferSubmitResult::StaleContext);
    publication.reset();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(GpuTransferActorTests, MissingEntriesAreSkippedWhilePresentEntriesUpload) {
    WarpRuntime runtime;
    ASSERT_TRUE(runtime.start());
    auto budget = std::make_shared<FrameBudget>(1U << 20U);
    auto mailbox = std::make_shared<FrameMailbox>(domain::DeviceGeneration{1U});
    GpuTransferActor actor{budget, runtime.broker, mailbox};

    const Nv12FrameLayout layout{
        .width = 4U,
        .height = 2U,
        .yStride = 4U,
        .uvStride = 4U,
    };
    const SourceBytes sourceA = makeSourceBytes(layout, 10U);
    std::optional<application::FrameSet> setWithMissing =
        makeCpuSetWithMissing(*budget, layout, sourceA);
    ASSERT_TRUE(setWithMissing.has_value());

    const application::FrameRequestContext context = makeContext();
    EXPECT_EQ(actor.submit(context, std::move(*setWithMissing)), GpuTransferSubmitResult::Accepted);
    std::optional<FrameMailboxPublication> publication =
        waitForPublication(*mailbox, context.deviceGeneration);
    ASSERT_TRUE(publication.has_value());
    ASSERT_NE(publication->set, nullptr);
    EXPECT_EQ(publication->set->frameId(), domain::FrameId{3});
    EXPECT_EQ(publication->set->context(), context);

    // Only the present entry (sourceId=0) should be uploaded; the Missing entry (sourceId=1) is
    // skipped.
    EXPECT_EQ(publication->set->frameCount(), 1U);
    const GpuFrameSlot* const slotA = publication->set->find(0U);
    const GpuFrameSlot* const slotB = publication->set->find(1U);
    ASSERT_NE(slotA, nullptr);
    EXPECT_EQ(slotA->sourceId, 0U);
    EXPECT_NE(slotA->frame, nullptr);
    EXPECT_EQ(slotB, nullptr);

    EXPECT_TRUE(mailbox->clear(context.playback));
    publication.reset();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(GpuTransferActorTests, UploadsThreeSourceFrameSetsAtomically) {
    WarpRuntime runtime;
    ASSERT_TRUE(runtime.start());
    auto budget = std::make_shared<FrameBudget>(1U << 20U);
    auto mailbox = std::make_shared<FrameMailbox>(domain::DeviceGeneration{1U});
    GpuTransferActor actor{budget, runtime.broker, mailbox};

    const Nv12FrameLayout layout{
        .width = 4U,
        .height = 2U,
        .yStride = 4U,
        .uvStride = 4U,
    };
    const SourceBytes sourceA = makeSourceBytes(layout, 1U);
    const SourceBytes sourceB = makeSourceBytes(layout, 2U);
    const SourceBytes sourceC = makeSourceBytes(layout, 3U);
    std::optional<application::FrameSet> set =
        makeThreeSourceCpuSet(*budget, layout, sourceA, sourceB, sourceC);
    ASSERT_TRUE(set.has_value());
    const application::FrameRequestContext context = makeContext();
    ASSERT_EQ(actor.submit(context, std::move(*set)), GpuTransferSubmitResult::Accepted);

    std::optional<FrameMailboxPublication> publication =
        waitForPublication(*mailbox, context.deviceGeneration);
    ASSERT_TRUE(publication.has_value());
    ASSERT_NE(publication->set, nullptr);
    EXPECT_EQ(publication->set->frameCount(), 3U);
    EXPECT_NE(publication->set->find(0U), nullptr);
    EXPECT_NE(publication->set->find(1U), nullptr);
    const GpuFrameSlot* const slotC = publication->set->find(2U);
    ASSERT_NE(slotC, nullptr);
    const auto* const backingC =
        dynamic_cast<const D3d11GpuFrameBacking*>(&slotC->frame->backing());
    ASSERT_NE(backingC, nullptr);
    const GraphicsDeviceLeaseResult lease = runtime.broker->tryLease();
    ASSERT_EQ(lease.status, GraphicsDeviceLeaseStatus::Available);
    ASSERT_TRUE(lease.lease.has_value());
    EXPECT_EQ(readTexture(lease.lease->device.Get(),
                          lease.lease->immediateContext.Get(),
                          backingC->yTexture(),
                          layout.width),
              sourceC.visibleY);

    EXPECT_TRUE(mailbox->clear(context.playback));
    publication.reset();
    EXPECT_TRUE(actor.shutdown(2s));
}

TEST(GpuTransferActorTests, PublishesDecoderOwnedNv12ArraySlicesWithoutACopy) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediateContext;
    D3D_FEATURE_LEVEL featureLevel{};
    const HRESULT created =
        D3D11CreateDevice(nullptr,
                          D3D_DRIVER_TYPE_HARDWARE,
                          nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                          nullptr,
                          0U,
                          D3D11_SDK_VERSION,
                          device.GetAddressOf(),
                          &featureLevel,
                          immediateContext.GetAddressOf());
    if (FAILED(created)) {
        GTEST_SKIP() << "No hardware D3D11 video device is available.";
    }

    auto broker = std::make_shared<GraphicsDeviceBroker>();
    ASSERT_EQ(broker->adoptQtDevice(device.Get(), immediateContext.Get()),
              GraphicsDeviceBrokerResult::Ready);
    auto budget = std::make_shared<FrameBudget>(8U * 1024U * 1024U);
    auto mailbox = std::make_shared<FrameMailbox>(domain::DeviceGeneration{1U});
    GpuTransferActor actor{budget, broker, mailbox};

    D3D11_TEXTURE2D_DESC description{};
    description.Width = 320U;
    description.Height = 180U;
    description.MipLevels = 1U;
    description.ArraySize = 2U;
    description.Format = DXGI_FORMAT_NV12;
    description.SampleDesc.Count = 1U;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> decodedTexture;
    ASSERT_TRUE(
        SUCCEEDED(device->CreateTexture2D(&description, nullptr, decodedTexture.GetAddressOf())));

    const domain::ColorMetadata color{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kLimited,
        .matrixInferred = false,
    };
    std::vector<application::MappedSourceFrame> sources;
    for (std::uint32_t slice = 0U; slice < 2U; ++slice) {
        auto anchor = std::make_shared<const std::uint32_t>(slice);
        const auto frame =
            D3d11DecodedFrameResource::create(decodedTexture,
                                              slice,
                                              description.Width,
                                              description.Height,
                                              domain::DeviceGeneration{1U},
                                              application::NormalizedFrameFormat::Nv12_8,
                                              color,
                                              application::FramePresentation{},
                                              anchor,
                                              *budget);
        ASSERT_TRUE(frame.has_value());
        sources.push_back(application::MappedSourceFrame{
            .sourceId = slice,
            .sourceFrameId = domain::FrameId{3},
            .frame = *frame,
            .presentationTime = domain::MediaTime{100'000},
            .matchKind = application::FrameMatchKind::ExactIndex,
            .alignmentConfidence = 1.0F,
        });
    }
    auto set = application::FrameSet::create(
        domain::FrameId{3}, domain::MediaTime{100'000}, std::move(sources));
    ASSERT_TRUE(set.has_value());
    const std::size_t decodedBytes =
        static_cast<std::size_t>(description.Width) * description.Height * 3U / 2U;
    ASSERT_EQ(budget->reservedBytes(), decodedBytes * 2U);

    const application::FrameRequestContext context = makeContext();
    ASSERT_EQ(actor.submit(context, std::move(*set)), GpuTransferSubmitResult::Accepted);
    auto publication = waitForPublication(*mailbox, context.deviceGeneration);
    ASSERT_TRUE(publication.has_value());
    for (domain::SourceId sourceId = 0U; sourceId < 2U; ++sourceId) {
        const GpuFrameSlot* const slot = publication->set->find(sourceId);
        ASSERT_NE(slot, nullptr);
        const auto* const backing =
            dynamic_cast<const D3d11GpuFrameBacking*>(&slot->frame->backing());
        ASSERT_NE(backing, nullptr);
        EXPECT_EQ(backing->yTexture(), decodedTexture.Get());
        EXPECT_EQ(backing->uvTexture(), decodedTexture.Get());
        EXPECT_NE(backing->yView(), nullptr);
        EXPECT_NE(backing->uvView(), nullptr);
    }
    EXPECT_EQ(budget->reservedBytes(), decodedBytes * 2U);

    EXPECT_TRUE(mailbox->clear(context.playback));
    publication.reset();
    EXPECT_TRUE(actor.shutdown(2s));
    EXPECT_EQ(budget->reservedBytes(), 0U);
}

// Regression for the hardware-only flush: a direct D3D11VA set submits no upload commands, so
// publication must complete without the immediate-context flush that CPU sets require. The
// published views must still bind and render identically, proven by grabbing the harness frame.
TEST(GpuTransferActorTests, HardwareOnlySetPublishesWithoutAnImmediateContextFlush) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediateContext;
    D3D_FEATURE_LEVEL featureLevel{};
    const HRESULT created =
        D3D11CreateDevice(nullptr,
                          D3D_DRIVER_TYPE_HARDWARE,
                          nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                          nullptr,
                          0U,
                          D3D11_SDK_VERSION,
                          device.GetAddressOf(),
                          &featureLevel,
                          immediateContext.GetAddressOf());
    if (FAILED(created)) {
        GTEST_SKIP() << "No hardware D3D11 video device is available.";
    }

    auto broker = std::make_shared<GraphicsDeviceBroker>();
    ASSERT_EQ(broker->adoptQtDevice(device.Get(), immediateContext.Get()),
              GraphicsDeviceBrokerResult::Ready);
    auto budget = std::make_shared<FrameBudget>(8U * 1024U * 1024U);
    auto mailbox = std::make_shared<FrameMailbox>(domain::DeviceGeneration{1U});
    GpuTransferActor actor{budget, broker, mailbox};

    D3D11_TEXTURE2D_DESC description{};
    description.Width = 320U;
    description.Height = 180U;
    description.MipLevels = 1U;
    description.ArraySize = 1U;
    description.Format = DXGI_FORMAT_NV12;
    description.SampleDesc.Count = 1U;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> decodedTexture;
    ASSERT_TRUE(
        SUCCEEDED(device->CreateTexture2D(&description, nullptr, decodedTexture.GetAddressOf())));

    const domain::ColorMetadata color{
        .matrix = domain::ColorMatrix::kBt709,
        .range = domain::ColorRange::kLimited,
        .matrixInferred = false,
    };
    auto anchor = std::make_shared<const std::uint32_t>(0U);
    std::optional<application::FrameSet> set;
    {
        const auto frame =
            D3d11DecodedFrameResource::create(decodedTexture,
                                              0U,
                                              description.Width,
                                              description.Height,
                                              domain::DeviceGeneration{1U},
                                              application::NormalizedFrameFormat::Nv12_8,
                                              color,
                                              application::FramePresentation{},
                                              anchor,
                                              *budget);
        ASSERT_TRUE(frame.has_value());
        set = application::FrameSet::create(
            domain::FrameId{7},
            domain::MediaTime{233'333},
            std::vector<application::MappedSourceFrame>{
                application::MappedSourceFrame{
                    .sourceId = 0U,
                    .sourceFrameId = domain::FrameId{7},
                    .frame = *frame,
                    .presentationTime = domain::MediaTime{233'333},
                    .matchKind = application::FrameMatchKind::ExactIndex,
                    .alignmentConfidence = 1.0F,
                },
            });
        ASSERT_TRUE(set.has_value());
    }

    const application::FrameRequestContext context = makeContext();
    ASSERT_EQ(actor.submit(context, std::move(*set)), GpuTransferSubmitResult::Accepted);
    // Publication must complete even though the direct-only path never flushes the shared
    // immediate context; view creation is ordered by the actor's own context calls.
    auto publication = waitForPublication(*mailbox, context.deviceGeneration);
    ASSERT_TRUE(publication.has_value());
    const GpuFrameSlot* const slot = publication->set->find(0U);
    ASSERT_NE(slot, nullptr);
    const auto* const backing = dynamic_cast<const D3d11GpuFrameBacking*>(&slot->frame->backing());
    ASSERT_NE(backing, nullptr);
    EXPECT_EQ(backing->yTexture(), decodedTexture.Get());
    EXPECT_NE(backing->yView(), nullptr);
    EXPECT_NE(backing->uvView(), nullptr);
    EXPECT_EQ(slot->frame->accountedBytes(), 86'400U);

    EXPECT_TRUE(mailbox->clear(context.playback));
    publication.reset();
    anchor.reset();
    EXPECT_TRUE(waitUntil([&budget] { return budget->reservedBytes() == 0U; }, 2s));
    EXPECT_TRUE(actor.shutdown(2s));
}

} // namespace
} // namespace dvs::platform
