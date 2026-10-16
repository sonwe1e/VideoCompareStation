#include "dvs/platform/D3d11DecodedFrameResource.h"
#include "dvs/platform/D3d11RenderChannel.h"
#include "dvs/platform/FrameBudget.h"
#include "dvs/platform/FrameMailbox.h"
#include "dvs/platform/FrameResourceFactory.h"
#include "dvs/platform/GpuTransferActor.h"
#include "dvs/platform/GraphicsDeviceBroker.h"

#include "D3d11GpuFrameBacking.h"
#include "D3d11SourcePixelProbe.h"
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
#include <iostream>
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

// The pixel probe is the video readout's numeric foundation: it must return the exact plane
// code values that were uploaded, not shader-interpolated presentation colours. The chroma
// sample is the one co-sited with the luma pixel's 2x2 block corner, and out-of-range probes
// report nothing instead of invented values.
TEST(GpuTransferActorTests, ProbePixelReadsExactNv12CodeValues) {
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
    std::optional<application::FrameSet> pair = makeCpuSet(*budget, layout, sourceA, sourceA);
    ASSERT_TRUE(pair.has_value());
    const application::FrameRequestContext context = makeContext();
    ASSERT_EQ(actor.submit(context, std::move(*pair)), GpuTransferSubmitResult::Accepted);
    std::optional<FrameMailboxPublication> publication =
        waitForPublication(*mailbox, context.deviceGeneration);
    ASSERT_TRUE(publication.has_value());
    const GpuFrameSlot* const slotA = publication->set->find(0U);
    ASSERT_NE(slotA, nullptr);
    const auto* const backing = dynamic_cast<const D3d11GpuFrameBacking*>(&slotA->frame->backing());
    ASSERT_NE(backing, nullptr);

    const GraphicsDeviceLeaseResult lease = runtime.broker->tryLease();
    ASSERT_EQ(lease.status, GraphicsDeviceLeaseStatus::Available);
    ASSERT_TRUE(lease.lease.has_value());
    ID3D11Device* const device = lease.lease->device.Get();
    ID3D11DeviceContext* const immediateContext = lease.lease->immediateContext.Get();

    // makeSourceBytes: y[row][col] = seed + row*11 + col;
    // uv[row][byte] = seed + 80 + row*7 + byte with Cb/Cr interleaved.
    const std::optional<D3d11SourcePixel> origin =
        backing->probePixel(device, immediateContext, 0U, 0U);
    ASSERT_TRUE(origin.has_value());
    EXPECT_EQ(origin->y, 10U);
    EXPECT_EQ(origin->cb, 90U);
    EXPECT_EQ(origin->cr, 91U);
    EXPECT_EQ(origin->bitDepth, 8U);

    const std::optional<D3d11SourcePixel> middle =
        backing->probePixel(device, immediateContext, 2U, 1U);
    ASSERT_TRUE(middle.has_value());
    EXPECT_EQ(middle->y, 10U + 11U + 2U);
    EXPECT_EQ(middle->cb, 10U + 80U + 2U);
    EXPECT_EQ(middle->cr, 10U + 80U + 3U);
    EXPECT_EQ(middle->bitDepth, 8U);

    // The far corner pairs with the last chroma sample (x>>1, y>>1).
    const std::optional<D3d11SourcePixel> corner =
        backing->probePixel(device, immediateContext, 4U, 2U);
    ASSERT_TRUE(corner.has_value());
    EXPECT_EQ(corner->y, 10U + 22U + 4U);
    EXPECT_EQ(corner->cb, 10U + 80U + 7U + 4U);
    EXPECT_EQ(corner->cr, 10U + 80U + 7U + 5U);

    // Out-of-range coordinates never invent values.
    EXPECT_FALSE(backing->probePixel(device, immediateContext, 5U, 0U).has_value());
    EXPECT_FALSE(backing->probePixel(device, immediateContext, 0U, 3U).has_value());
    EXPECT_FALSE(backing->probePixel(nullptr, immediateContext, 0U, 0U).has_value());
    EXPECT_FALSE(backing->probePixel(device, nullptr, 0U, 0U).has_value());

    EXPECT_TRUE(mailbox->clear(context.playback));
    publication.reset();
    EXPECT_TRUE(actor.shutdown(2s));
}

// P010 keeps each 10-bit sample in the most significant bits of a 16-bit word; the probe must
// right-shift them out instead of reporting the raw container. Built directly on the device so
// the fixture controls the exact 16-bit words.
TEST(GpuTransferActorTests, ProbePixelReadsTenBitP010CodeValues) {
    WarpRuntime runtime;
    ASSERT_TRUE(runtime.start());
    const GraphicsDeviceLeaseResult lease = runtime.broker->tryLease();
    ASSERT_EQ(lease.status, GraphicsDeviceLeaseStatus::Available);
    ASSERT_TRUE(lease.lease.has_value());
    ID3D11Device* const device = lease.lease->device.Get();
    ID3D11DeviceContext* const immediateContext = lease.lease->immediateContext.Get();

    D3D11_TEXTURE2D_DESC lumaDescription{};
    lumaDescription.Width = 2U;
    lumaDescription.Height = 2U;
    lumaDescription.MipLevels = 1U;
    lumaDescription.ArraySize = 1U;
    lumaDescription.Format = DXGI_FORMAT_R16_UNORM;
    lumaDescription.SampleDesc.Count = 1U;
    lumaDescription.Usage = D3D11_USAGE_DEFAULT;
    lumaDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // 10-bit code values, left-justified by 6.
    const std::uint16_t lumaWords[4] = {64U << 6U, 512U << 6U, 1000U << 6U, 1023U << 6U};
    const D3D11_SUBRESOURCE_DATA lumaData{
        .pSysMem = lumaWords,
        .SysMemPitch = 2U * sizeof(std::uint16_t),
    };
    ComPtr<ID3D11Texture2D> yTexture;
    ASSERT_TRUE(
        SUCCEEDED(device->CreateTexture2D(&lumaDescription, &lumaData, yTexture.GetAddressOf())));
    ComPtr<ID3D11ShaderResourceView> yView;
    ASSERT_TRUE(
        SUCCEEDED(device->CreateShaderResourceView(yTexture.Get(), nullptr, yView.GetAddressOf())));

    D3D11_TEXTURE2D_DESC chromaDescription = lumaDescription;
    chromaDescription.Width = 1U;
    chromaDescription.Height = 1U;
    chromaDescription.Format = DXGI_FORMAT_R16G16_UNORM;
    const std::uint16_t chromaWords[2] = {940U << 6U, 64U << 6U};
    const D3D11_SUBRESOURCE_DATA chromaData{
        .pSysMem = chromaWords,
        .SysMemPitch = 2U * sizeof(std::uint16_t),
    };
    ComPtr<ID3D11Texture2D> uvTexture;
    ASSERT_TRUE(SUCCEEDED(
        device->CreateTexture2D(&chromaDescription, &chromaData, uvTexture.GetAddressOf())));
    ComPtr<ID3D11ShaderResourceView> uvView;
    ASSERT_TRUE(SUCCEEDED(
        device->CreateShaderResourceView(uvTexture.Get(), nullptr, uvView.GetAddressOf())));

    const D3d11GpuFrameBacking backing{yTexture,
                                       yView,
                                       D3d11PlaneDimensions{.width = 2U, .height = 2U},
                                       uvTexture,
                                       uvView,
                                       D3d11PlaneDimensions{.width = 1U, .height = 1U}};

    const std::optional<D3d11SourcePixel> probed =
        backing.probePixel(device, immediateContext, 1U, 1U);
    ASSERT_TRUE(probed.has_value());
    EXPECT_EQ(probed->y, 1023U);
    EXPECT_EQ(probed->cb, 940U);
    EXPECT_EQ(probed->cr, 64U);
    EXPECT_EQ(probed->bitDepth, 10U);

    const std::optional<D3d11SourcePixel> firstPixel =
        backing.probePixel(device, immediateContext, 0U, 0U);
    ASSERT_TRUE(firstPixel.has_value());
    EXPECT_EQ(firstPixel->y, 64U);
    EXPECT_EQ(firstPixel->cb, 940U);
    EXPECT_EQ(firstPixel->cr, 64U);
}

// Zero-copy D3D11VA frames arrive as one NV12 Texture2DArray with aligned (padded) surface
// dimensions and per-frame plane views over a non-zero array slice. The probe must read the
// slice the view designates, address visible samples only (never the padding), and return
// exact code values. Before the shader-view probe this shape failed on real hardware with
// "readback failed": the plane-copy whitelist rejected the NV12 resource format and copied
// array slice 0 regardless of the view's slice. Plane-cast views additionally require the
// device to carry D3D11_CREATE_DEVICE_VIDEO_SUPPORT - the production decoder path and this
// test both create it that way (the view matrix in test-probe-diag-02.log shows a device
// without the flag rejects the same view with E_INVALIDARG).
TEST(GpuTransferActorTests, ProbePixelReadsNv12ArraySliceVisibleRegionCodeValues) {
    Microsoft::WRL::ComPtr<ID3D11Device> deviceHolder;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> contextHolder;
    HRESULT deviceResult =
        D3D11CreateDevice(nullptr,
                          D3D_DRIVER_TYPE_HARDWARE,
                          nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                          nullptr,
                          0U,
                          D3D11_SDK_VERSION,
                          deviceHolder.GetAddressOf(),
                          nullptr,
                          contextHolder.GetAddressOf());
    if (FAILED(deviceResult)) {
        GTEST_SKIP() << "no hardware D3D11 video device (hr=0x" << std::hex << deviceResult << ")";
    }
    ID3D11Device* const device = deviceHolder.Get();
    ID3D11DeviceContext* const immediateContext = contextHolder.Get();

    constexpr std::uint32_t textureWidth = 8U;
    constexpr std::uint32_t textureHeight = 4U;
    constexpr std::uint32_t visibleWidth = 5U;
    constexpr std::uint32_t visibleHeight = 3U;

    D3D11_TEXTURE2D_DESC textureDescription{};
    textureDescription.Width = textureWidth;
    textureDescription.Height = textureHeight;
    textureDescription.MipLevels = 1U;
    textureDescription.ArraySize = 2U;
    textureDescription.Format = DXGI_FORMAT_NV12;
    textureDescription.SampleDesc.Count = 1U;
    textureDescription.Usage = D3D11_USAGE_DEFAULT;
    // The production decoder-surface shape; the plane-cast views depend on it.
    textureDescription.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    ASSERT_TRUE(
        SUCCEEDED(device->CreateTexture2D(&textureDescription, nullptr, texture.GetAddressOf())));

    // Slice s: visible luma(x, y) = 30 + 100s + 11y + x; padding stays 0xF0. Visible chroma
    // (cx, cy) = 60 + 100s + 7cy + 5cx (+1 for Cr); padding chroma stays 0xF1. Slice 0 uses a
    // different seed so reading slice 0 can never satisfy the slice-1 expectations below.
    const auto fillSlice = [&](const std::uint32_t slice) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(textureWidth) * textureHeight * 2U,
                                        0x80U);
        const std::uint8_t lumaSeed = static_cast<std::uint8_t>(30U + 100U * slice);
        const std::uint8_t chromaSeed = static_cast<std::uint8_t>(60U + 100U * slice);
        for (std::uint32_t row = 0U; row < textureHeight; ++row) {
            for (std::uint32_t column = 0U; column < textureWidth; ++column) {
                bytes[static_cast<std::size_t>(row) * textureWidth + column] =
                    (column < visibleWidth && row < visibleHeight)
                        ? static_cast<std::uint8_t>(lumaSeed + 11U * row + column)
                        : 0xF0U;
            }
        }
        const std::size_t chromaBase = static_cast<std::size_t>(textureWidth) * textureHeight;
        for (std::uint32_t row = 0U; row < textureHeight / 2U; ++row) {
            for (std::uint32_t column = 0U; column < textureWidth / 2U; ++column) {
                const std::size_t offset = chromaBase +
                                           (static_cast<std::size_t>(row) * textureWidth) +
                                           (static_cast<std::size_t>(column) * 2U);
                if (column < (visibleWidth + 1U) / 2U && row < (visibleHeight + 1U) / 2U) {
                    const std::uint8_t value =
                        static_cast<std::uint8_t>(chromaSeed + 7U * row + 5U * column);
                    bytes[offset] = value;
                    bytes[offset + 1U] = static_cast<std::uint8_t>(value + 1U);
                } else {
                    bytes[offset] = 0xF1U;
                    bytes[offset + 1U] = 0xF1U;
                }
            }
        }
        // Planar UpdateSubresource layout: plane 1 begins at the depth-pitch argument.
        const D3D11_SUBRESOURCE_DATA data{
            .pSysMem = bytes.data(), .SysMemPitch = textureWidth, .SysMemSlicePitch = 0U};
        immediateContext->UpdateSubresource(texture.Get(),
                                            slice,
                                            nullptr,
                                            data.pSysMem,
                                            data.SysMemPitch,
                                            textureWidth * textureHeight);
    };
    fillSlice(0U);
    fillSlice(1U);

    const auto makePlaneView = [&](const DXGI_FORMAT format) {
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        viewDescription.Format = format;
        viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        // Mirror production createDirectSource exactly: MipLevels must be 1, not the
        // zero-initialized "all remaining" reading the earlier matrix run missed.
        viewDescription.Texture2DArray.MostDetailedMip = 0U;
        viewDescription.Texture2DArray.MipLevels = 1U;
        viewDescription.Texture2DArray.FirstArraySlice = 1U;
        viewDescription.Texture2DArray.ArraySize = 1U;
        ComPtr<ID3D11ShaderResourceView> view;
        const HRESULT viewResult =
            device->CreateShaderResourceView(texture.Get(), &viewDescription, view.GetAddressOf());
        EXPECT_TRUE(SUCCEEDED(viewResult)) << "plane-cast view hr=0x" << std::hex << viewResult;
        return view;
    };
    ComPtr<ID3D11ShaderResourceView> lumaView = makePlaneView(DXGI_FORMAT_R8_UNORM);
    ComPtr<ID3D11ShaderResourceView> chromaView = makePlaneView(DXGI_FORMAT_R8G8_UNORM);
    ASSERT_NE(lumaView.Get(), nullptr);
    ASSERT_NE(chromaView.Get(), nullptr);

    const D3d11GpuFrameBacking backing{
        texture,
        lumaView,
        D3d11PlaneDimensions{.width = visibleWidth, .height = visibleHeight},
        texture,
        chromaView,
        D3d11PlaneDimensions{.width = (visibleWidth + 1U) / 2U,
                             .height = (visibleHeight + 1U) / 2U}};

    const std::optional<D3d11SourcePixel> origin =
        backing.probePixel(device, immediateContext, 0U, 0U);
    ASSERT_TRUE(origin.has_value());
    EXPECT_EQ(origin->y, 30U + 100U);
    EXPECT_EQ(origin->cb, 60U + 100U);
    EXPECT_EQ(origin->cr, 60U + 100U + 1U);
    EXPECT_EQ(origin->bitDepth, 8U);

    const std::optional<D3d11SourcePixel> middle =
        backing.probePixel(device, immediateContext, 2U, 1U);
    ASSERT_TRUE(middle.has_value());
    // Revised during review: chroma is co-sited at (x>>1, y>>1) = (1, 0), so the middle pixel
    // reads cy=0, cx=1 -> 60+100+7*0+5*1. The earlier 7+10 terms belonged to the corner's
    // (2, 1) chroma sample; the product sampling contract was never in question.
    EXPECT_EQ(middle->y, 30U + 100U + 11U + 2U);
    EXPECT_EQ(middle->cb, 60U + 100U + 5U);
    EXPECT_EQ(middle->cr, 60U + 100U + 5U + 1U);

    // The far corner pairs with the co-sited chroma sample inside slice 1.
    const std::optional<D3d11SourcePixel> corner =
        backing.probePixel(device, immediateContext, 4U, 2U);
    ASSERT_TRUE(corner.has_value());
    EXPECT_EQ(corner->y, 30U + 100U + 22U + 4U);
    EXPECT_EQ(corner->cb, 60U + 100U + 7U + 10U);
    EXPECT_EQ(corner->cr, 60U + 100U + 7U + 10U + 1U);

    // Visible-region addressing: padding samples are never reachable.
    EXPECT_FALSE(backing.probePixel(device, immediateContext, 5U, 0U).has_value());
    EXPECT_FALSE(backing.probePixel(device, immediateContext, 0U, 3U).has_value());
    EXPECT_FALSE(backing.probePixel(device, immediateContext, 7U, 3U).has_value());
    EXPECT_FALSE(backing.probePixel(nullptr, immediateContext, 0U, 0U).has_value());
    EXPECT_FALSE(backing.probePixel(device, nullptr, 0U, 0U).has_value());
}

// P010 zero-copy frames keep each 10-bit sample in the most significant bits of a 16-bit word
// inside a decoder-owned Texture2DArray. The probe must read the non-zero array slice the
// view designates, address visible samples only, right-shift out the exact 10-bit codes, and
// stay bit-exact against the words the fixture uploaded.
TEST(GpuTransferActorTests, ProbePixelReadsP010ArraySliceTenBitCodeValues) {
    Microsoft::WRL::ComPtr<ID3D11Device> deviceHolder;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> contextHolder;
    HRESULT deviceResult =
        D3D11CreateDevice(nullptr,
                          D3D_DRIVER_TYPE_HARDWARE,
                          nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                          nullptr,
                          0U,
                          D3D11_SDK_VERSION,
                          deviceHolder.GetAddressOf(),
                          nullptr,
                          contextHolder.GetAddressOf());
    if (FAILED(deviceResult)) {
        GTEST_SKIP() << "no hardware D3D11 video device (hr=0x" << std::hex << deviceResult << ")";
    }
    ID3D11Device* const device = deviceHolder.Get();
    ID3D11DeviceContext* const immediateContext = contextHolder.Get();

    constexpr std::uint32_t textureWidth = 8U;
    constexpr std::uint32_t textureHeight = 4U;
    constexpr std::uint32_t visibleWidth = 5U;
    constexpr std::uint32_t visibleHeight = 3U;
    constexpr std::uint16_t lumaPaddingCode = 1000U;
    constexpr std::uint16_t chromaPaddingCode = 999U;

    D3D11_TEXTURE2D_DESC textureDescription{};
    textureDescription.Width = textureWidth;
    textureDescription.Height = textureHeight;
    textureDescription.MipLevels = 1U;
    textureDescription.ArraySize = 2U;
    textureDescription.Format = DXGI_FORMAT_P010;
    textureDescription.SampleDesc.Count = 1U;
    textureDescription.Usage = D3D11_USAGE_DEFAULT;
    textureDescription.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    ASSERT_TRUE(
        SUCCEEDED(device->CreateTexture2D(&textureDescription, nullptr, texture.GetAddressOf())));

    // Slice s: visible luma(x, y) = 300 + 100s + 11y + x (10-bit, left-justified by 6);
    // padding words stay 1000 << 6. Visible chroma (cx, cy) = 600 + 100s + 7cy + 5cx (+1 for
    // Cr); padding chroma words stay 999 << 6. Slice 0 uses a different seed so reading slice
    // 0 can never satisfy the slice-1 expectations below.
    const auto fillSlice = [&](const std::uint32_t slice) {
        std::vector<std::uint16_t> words(
            static_cast<std::size_t>(textureWidth) * textureHeight * 2U, 0x8000U);
        const std::uint16_t lumaSeed = static_cast<std::uint16_t>(300U + 100U * slice);
        const std::uint16_t chromaSeed = static_cast<std::uint16_t>(600U + 100U * slice);
        for (std::uint32_t row = 0U; row < textureHeight; ++row) {
            for (std::uint32_t column = 0U; column < textureWidth; ++column) {
                words[static_cast<std::size_t>(row) * textureWidth + column] =
                    (column < visibleWidth && row < visibleHeight)
                        ? static_cast<std::uint16_t>((lumaSeed + 11U * row + column) << 6U)
                        : static_cast<std::uint16_t>(lumaPaddingCode << 6U);
            }
        }
        const std::size_t chromaBase = static_cast<std::size_t>(textureWidth) * textureHeight;
        for (std::uint32_t row = 0U; row < textureHeight / 2U; ++row) {
            for (std::uint32_t column = 0U; column < textureWidth / 2U; ++column) {
                const std::size_t offset = chromaBase +
                                           (static_cast<std::size_t>(row) * textureWidth) +
                                           (static_cast<std::size_t>(column) * 2U);
                if (column < (visibleWidth + 1U) / 2U && row < (visibleHeight + 1U) / 2U) {
                    const std::uint16_t value =
                        static_cast<std::uint16_t>(chromaSeed + 7U * row + 5U * column);
                    words[offset] = static_cast<std::uint16_t>(value << 6U);
                    words[offset + 1U] = static_cast<std::uint16_t>((value + 1U) << 6U);
                } else {
                    words[offset] = static_cast<std::uint16_t>(chromaPaddingCode << 6U);
                    words[offset + 1U] = static_cast<std::uint16_t>(chromaPaddingCode << 6U);
                }
            }
        }
        // Planar UpdateSubresource layout: plane 1 begins at the depth-pitch argument.
        const D3D11_SUBRESOURCE_DATA data{
            .pSysMem = words.data(), .SysMemPitch = textureWidth * 2U, .SysMemSlicePitch = 0U};
        immediateContext->UpdateSubresource(texture.Get(),
                                            slice,
                                            nullptr,
                                            data.pSysMem,
                                            data.SysMemPitch,
                                            textureWidth * 2U * textureHeight);
    };
    fillSlice(0U);
    fillSlice(1U);

    const auto makePlaneView = [&](const DXGI_FORMAT format) {
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        viewDescription.Format = format;
        viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        // Production createDirectSource shape: MostDetailedMip 0, MipLevels 1, one slice.
        viewDescription.Texture2DArray.MostDetailedMip = 0U;
        viewDescription.Texture2DArray.MipLevels = 1U;
        viewDescription.Texture2DArray.FirstArraySlice = 1U;
        viewDescription.Texture2DArray.ArraySize = 1U;
        ComPtr<ID3D11ShaderResourceView> view;
        const HRESULT viewResult =
            device->CreateShaderResourceView(texture.Get(), &viewDescription, view.GetAddressOf());
        EXPECT_TRUE(SUCCEEDED(viewResult)) << "plane-cast view hr=0x" << std::hex << viewResult;
        return view;
    };
    ComPtr<ID3D11ShaderResourceView> lumaView = makePlaneView(DXGI_FORMAT_R16_UNORM);
    ComPtr<ID3D11ShaderResourceView> chromaView = makePlaneView(DXGI_FORMAT_R16G16_UNORM);
    ASSERT_NE(lumaView.Get(), nullptr);
    ASSERT_NE(chromaView.Get(), nullptr);

    const D3d11GpuFrameBacking backing{
        texture,
        lumaView,
        D3d11PlaneDimensions{.width = visibleWidth, .height = visibleHeight},
        texture,
        chromaView,
        D3d11PlaneDimensions{.width = (visibleWidth + 1U) / 2U,
                             .height = (visibleHeight + 1U) / 2U}};

    const std::optional<D3d11SourcePixel> origin =
        backing.probePixel(device, immediateContext, 0U, 0U);
    ASSERT_TRUE(origin.has_value());
    EXPECT_EQ(origin->y, 300U + 100U);
    EXPECT_EQ(origin->cb, 600U + 100U);
    EXPECT_EQ(origin->cr, 600U + 100U + 1U);
    EXPECT_EQ(origin->bitDepth, 10U);

    const std::optional<D3d11SourcePixel> middle =
        backing.probePixel(device, immediateContext, 2U, 1U);
    ASSERT_TRUE(middle.has_value());
    EXPECT_EQ(middle->y, 300U + 100U + 11U + 2U);
    EXPECT_EQ(middle->cb, 600U + 100U + 5U);
    EXPECT_EQ(middle->cr, 600U + 100U + 5U + 1U);

    const std::optional<D3d11SourcePixel> corner =
        backing.probePixel(device, immediateContext, 4U, 2U);
    ASSERT_TRUE(corner.has_value());
    EXPECT_EQ(corner->y, 300U + 100U + 22U + 4U);
    EXPECT_EQ(corner->cb, 600U + 100U + 7U + 10U);
    EXPECT_EQ(corner->cr, 600U + 100U + 7U + 10U + 1U);

    // Visible-region addressing: padding samples are never reachable.
    EXPECT_FALSE(backing.probePixel(device, immediateContext, 5U, 0U).has_value());
    EXPECT_FALSE(backing.probePixel(device, immediateContext, 0U, 3U).has_value());
    EXPECT_FALSE(backing.probePixel(device, immediateContext, 7U, 3U).has_value());
    EXPECT_FALSE(backing.probePixel(nullptr, immediateContext, 0U, 0U).has_value());
    EXPECT_FALSE(backing.probePixel(device, nullptr, 0U, 0U).has_value());
}

// E1: the pending-probe key's start/match/clear transitions, exercised without any GPU wait.
// The full-width request id is the contract: a key staged with a wide id must match the same
// wide id, must NOT match a request that shares only the low 32 bits, and a compiled
// truncation of the stored id must be detected by the same-lower-half request flipping to
// non-matching. Set identity and coordinates are part of every comparison.
TEST(GpuTransferActorTests, PendingProbeKeyTransitionsDetectTruncatedRequestIds) {
    int setA = 0;
    int setB = 0;
    PendingProbeKey key;
    key.start(&setA, 0x1'0000'0001ULL, 8U, 4U);

    // Exact re-match: same set, same full-width id, same texel.
    EXPECT_TRUE(key.matches(&setA, 0x1'0000'0001ULL, 8U, 4U));
    // Same low 32 bits, different high half: must NOT match.
    EXPECT_FALSE(key.matches(&setA, 0x2'0000'0001ULL, 8U, 4U));
    // Set identity and coordinates are part of the key.
    EXPECT_FALSE(key.matches(&setB, 0x1'0000'0001ULL, 8U, 4U));
    EXPECT_FALSE(key.matches(&setA, 0x1'0000'0001ULL, 9U, 4U));
    EXPECT_FALSE(key.matches(&setA, 0x1'0000'0001ULL, 8U, 5U));

    key.clear();
    EXPECT_FALSE(key.matches(&setA, 0x1'0000'0001ULL, 8U, 4U))
        << "a cleared key must not match anything";

    // The compiled truncation mutation (store through uint32) is detected: a key staged with
    // a truncated id stops matching the same full-width request.
    PendingProbeKey truncatedKey;
    truncatedKey.start(&setA, static_cast<std::uint32_t>(0x1'0000'0001ULL), 8U, 4U);
    EXPECT_FALSE(truncatedKey.matches(&setA, 0x1'0000'0001ULL, 8U, 4U))
        << "a truncated stored id must not match the wide request";
}

namespace {

// Small separate-plane backing (Texture2D views, the CPU-upload shape) for probe-slot tests.
struct ProbePlaneFixture final {
    ComPtr<ID3D11Texture2D> luma;
    ComPtr<ID3D11Texture2D> chroma;
    ComPtr<ID3D11ShaderResourceView> lumaView;
    ComPtr<ID3D11ShaderResourceView> chromaView;
    std::unique_ptr<const D3d11GpuFrameBacking> backing;
};

[[nodiscard]] ProbePlaneFixture makeSeparatePlaneBacking(ID3D11Device* const device,
                                                         const std::uint8_t lumaCode,
                                                         const std::uint8_t cbCode,
                                                         const std::uint8_t crCode) {
    ProbePlaneFixture fixture;
    D3D11_TEXTURE2D_DESC lumaDescription{};
    lumaDescription.Width = 2U;
    lumaDescription.Height = 2U;
    lumaDescription.MipLevels = 1U;
    lumaDescription.ArraySize = 1U;
    lumaDescription.Format = DXGI_FORMAT_R8_UNORM;
    lumaDescription.SampleDesc.Count = 1U;
    lumaDescription.Usage = D3D11_USAGE_DEFAULT;
    lumaDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const std::array<std::uint8_t, 4U> lumaBytes{lumaCode, lumaCode, lumaCode, lumaCode};
    const D3D11_SUBRESOURCE_DATA lumaData{.pSysMem = lumaBytes.data(), .SysMemPitch = 2U};
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(
        &lumaDescription, &lumaData, fixture.luma.ReleaseAndGetAddressOf())));
    D3D11_TEXTURE2D_DESC chromaDescription = lumaDescription;
    chromaDescription.Width = 1U;
    chromaDescription.Height = 1U;
    chromaDescription.Format = DXGI_FORMAT_R8G8_UNORM;
    const std::array<std::uint8_t, 2U> chromaBytes{cbCode, crCode};
    const D3D11_SUBRESOURCE_DATA chromaData{.pSysMem = chromaBytes.data(), .SysMemPitch = 2U};
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(
        &chromaDescription, &chromaData, fixture.chroma.ReleaseAndGetAddressOf())));
    EXPECT_TRUE(SUCCEEDED(device->CreateShaderResourceView(
        fixture.luma.Get(), nullptr, fixture.lumaView.ReleaseAndGetAddressOf())));
    EXPECT_TRUE(SUCCEEDED(device->CreateShaderResourceView(
        fixture.chroma.Get(), nullptr, fixture.chromaView.ReleaseAndGetAddressOf())));
    fixture.backing = std::make_unique<const D3d11GpuFrameBacking>(
        fixture.luma,
        fixture.lumaView,
        D3d11PlaneDimensions{.width = 2U, .height = 2U},
        fixture.chroma,
        fixture.chromaView,
        D3d11PlaneDimensions{.width = 1U, .height = 1U});
    return fixture;
}

[[nodiscard]] ULONG liveRefs(IUnknown* const object) {
    const ULONG afterAdd = object->AddRef();
    static_cast<void>(object->Release());
    return afterAdd - 1U;
}

} // namespace

// Probe slots own their pipeline objects per device: two slots on two independent broker
// devices interleave issue/poll without seeing each other's shaders, a slot re-targeted at a
// different device rebuilds, and a reset slot is reusable.
TEST(GpuTransferActorTests, ProbeSlotsAreDeviceOwnedAndInterleaveAcrossRuntimes) {
    WarpRuntime runtimeA;
    ASSERT_TRUE(runtimeA.start());
    WarpRuntime runtimeB;
    ASSERT_TRUE(runtimeB.start());
    const GraphicsDeviceLeaseResult leaseA = runtimeA.broker->tryLease();
    const GraphicsDeviceLeaseResult leaseB = runtimeB.broker->tryLease();
    ASSERT_EQ(leaseA.status, GraphicsDeviceLeaseStatus::Available);
    ASSERT_EQ(leaseB.status, GraphicsDeviceLeaseStatus::Available);
    ASSERT_TRUE(leaseA.lease.has_value());
    ASSERT_TRUE(leaseB.lease.has_value());
    ID3D11Device* const deviceA = leaseA.lease->device.Get();
    ID3D11Device* const deviceB = leaseB.lease->device.Get();
    ASSERT_NE(deviceA, deviceB);
    ID3D11DeviceContext* const contextA = leaseA.lease->immediateContext.Get();
    ID3D11DeviceContext* const contextB = leaseB.lease->immediateContext.Get();

    const ProbePlaneFixture fixtureA = makeSeparatePlaneBacking(deviceA, 40U, 90U, 91U);
    const ProbePlaneFixture fixtureB = makeSeparatePlaneBacking(deviceB, 50U, 100U, 101U);
    const std::optional<D3d11ProbePlane> lumaA = lumaProbePlane(*fixtureA.backing);
    const std::optional<D3d11ProbePlane> chromaA = chromaProbePlane(*fixtureA.backing);
    const std::optional<D3d11ProbePlane> lumaB = lumaProbePlane(*fixtureB.backing);
    const std::optional<D3d11ProbePlane> chromaB = chromaProbePlane(*fixtureB.backing);
    ASSERT_TRUE(lumaA.has_value() && chromaA.has_value());
    ASSERT_TRUE(lumaB.has_value() && chromaB.has_value());

    D3d11SourcePixelProbeSlot slotA;
    D3d11SourcePixelProbeSlot slotB;
    ASSERT_TRUE(slotA.issue(deviceA, contextA, *lumaA, *chromaA, 1U, 1U, 0U, 0U));
    ASSERT_TRUE(slotB.issue(deviceB, contextB, *lumaB, *chromaB, 0U, 0U, 0U, 0U));
    contextA->Flush();
    contextB->Flush();

    bool aReady = false;
    bool bReady = false;
    ASSERT_TRUE(waitUntil(
        [&] {
            if (!aReady) {
                aReady = slotA.poll(contextA).value_or(false);
            }
            if (!bReady) {
                bReady = slotB.poll(contextB).value_or(false);
            }
            return aReady && bReady;
        },
        5s));
    EXPECT_EQ(slotA.values().y, 40U);
    EXPECT_EQ(slotA.values().cb, 90U);
    EXPECT_EQ(slotA.values().cr, 91U);
    EXPECT_EQ(slotA.values().bitDepth, 8U);
    EXPECT_EQ(slotB.values().y, 50U);
    EXPECT_EQ(slotB.values().cb, 100U);
    EXPECT_EQ(slotB.values().cr, 101U);

    // A slot re-targeted at another device rebuilds its device-scoped objects and answers
    // from that device; a reset slot is reusable from scratch.
    ASSERT_TRUE(slotA.issue(deviceB, contextB, *lumaB, *chromaB, 0U, 0U, 0U, 0U));
    contextB->Flush();
    aReady = false;
    ASSERT_TRUE(waitUntil(
        [&] {
            aReady = slotA.poll(contextB).value_or(false);
            return aReady;
        },
        5s));
    EXPECT_EQ(slotA.values().y, 50U);
    slotA.reset();
    ASSERT_TRUE(slotA.issue(deviceA, contextA, *lumaA, *chromaA, 0U, 0U, 0U, 0U));
    contextA->Flush();
    aReady = false;
    ASSERT_TRUE(waitUntil(
        [&] {
            aReady = slotA.poll(contextA).value_or(false);
            return aReady;
        },
        5s));
    EXPECT_EQ(slotA.values().y, 40U);
}

// The probe draw must leave the caller's immediate-context state exactly as it found it
// (shader resources, render target, every viewport, blend state) and must not retain
// references to the probed frame planes after the call.
TEST(GpuTransferActorTests, ProbeIssuePreservesCallerStateAndFrameReferences) {
    WarpRuntime runtime;
    ASSERT_TRUE(runtime.start());
    const GraphicsDeviceLeaseResult lease = runtime.broker->tryLease();
    ASSERT_EQ(lease.status, GraphicsDeviceLeaseStatus::Available);
    ASSERT_TRUE(lease.lease.has_value());
    ID3D11Device* const device = lease.lease->device.Get();
    ID3D11DeviceContext* const context = lease.lease->immediateContext.Get();

    D3D11_TEXTURE2D_DESC targetDescription{};
    targetDescription.Width = 8U;
    targetDescription.Height = 8U;
    targetDescription.MipLevels = 1U;
    targetDescription.ArraySize = 1U;
    targetDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    targetDescription.SampleDesc.Count = 1U;
    targetDescription.Usage = D3D11_USAGE_DEFAULT;
    targetDescription.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> callerTargetTexture;
    ASSERT_TRUE(SUCCEEDED(
        device->CreateTexture2D(&targetDescription, nullptr, callerTargetTexture.GetAddressOf())));
    ComPtr<ID3D11RenderTargetView> callerTarget;
    ASSERT_TRUE(SUCCEEDED(device->CreateRenderTargetView(
        callerTargetTexture.Get(), nullptr, callerTarget.GetAddressOf())));

    D3D11_TEXTURE2D_DESC srvDescription = targetDescription;
    srvDescription.Width = 4U;
    srvDescription.Height = 4U;
    srvDescription.Format = DXGI_FORMAT_R8_UNORM;
    srvDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> srvTextures[2U];
    ComPtr<ID3D11ShaderResourceView> srvViews[2U];
    for (ComPtr<ID3D11Texture2D>& texture : srvTextures) {
        ASSERT_TRUE(SUCCEEDED(
            device->CreateTexture2D(&srvDescription, nullptr, texture.ReleaseAndGetAddressOf())));
    }
    for (std::size_t index = 0U; index < 2U; ++index) {
        ASSERT_TRUE(SUCCEEDED(device->CreateShaderResourceView(
            srvTextures[index].Get(), nullptr, srvViews[index].ReleaseAndGetAddressOf())));
    }
    ID3D11ShaderResourceView* const srvPointers[2U] = {srvViews[0U].Get(), srvViews[1U].Get()};
    context->PSSetShaderResources(0U, 2U, srvPointers);
    context->OMSetRenderTargets(1U, callerTarget.GetAddressOf(), nullptr);

    const std::array<D3D11_VIEWPORT, 2U> callerViewports{
        D3D11_VIEWPORT{0.0F, 0.0F, 4.0F, 4.0F, 0.0F, 1.0F},
        D3D11_VIEWPORT{4.0F, 0.0F, 4.0F, 4.0F, 0.0F, 1.0F}};
    context->RSSetViewports(static_cast<UINT>(callerViewports.size()), callerViewports.data());

    D3D11_BLEND_DESC blendDescription{};
    blendDescription.RenderTarget[0U].BlendEnable = TRUE;
    blendDescription.RenderTarget[0U].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDescription.RenderTarget[0U].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDescription.RenderTarget[0U].BlendOp = D3D11_BLEND_OP_ADD;
    blendDescription.RenderTarget[0U].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDescription.RenderTarget[0U].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDescription.RenderTarget[0U].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDescription.RenderTarget[0U].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> callerBlend;
    ASSERT_TRUE(SUCCEEDED(device->CreateBlendState(&blendDescription, callerBlend.GetAddressOf())));
    FLOAT blendFactors[4U] = {0.0F, 0.0F, 0.0F, 0.0F};
    context->OMSetBlendState(callerBlend.Get(), blendFactors, 0xFFFFFFFFU);

    // Baselines: capture what the caller has bound so the post-probe state must match.
    ComPtr<ID3D11ShaderResourceView> keptSrvs[2U];
    ComPtr<ID3D11RenderTargetView> keptTarget;
    ComPtr<ID3D11DepthStencilView> keptDepth;
    ComPtr<ID3D11BlendState> keptBlend;
    FLOAT keptBlendFactors[4U] = {};
    UINT keptSampleMask = 0U;
    UINT keptViewportCount = static_cast<UINT>(callerViewports.size());
    std::array<D3D11_VIEWPORT, 2U> keptViewports{};
    context->PSGetShaderResources(0U, 2U, keptSrvs[0U].ReleaseAndGetAddressOf());
    context->OMGetRenderTargets(
        1U, keptTarget.ReleaseAndGetAddressOf(), keptDepth.ReleaseAndGetAddressOf());
    context->OMGetBlendState(keptBlend.ReleaseAndGetAddressOf(), keptBlendFactors, &keptSampleMask);
    context->RSGetViewports(&keptViewportCount, keptViewports.data());
    ASSERT_EQ(keptViewportCount, 2U);
    ASSERT_EQ(keptSrvs[0U].Get(), srvViews[0U].Get());
    ASSERT_EQ(keptTarget.Get(), callerTarget.Get());

    const ProbePlaneFixture fixture = makeSeparatePlaneBacking(device, 70U, 120U, 121U);
    const std::optional<D3d11ProbePlane> luma = lumaProbePlane(*fixture.backing);
    const std::optional<D3d11ProbePlane> chroma = chromaProbePlane(*fixture.backing);
    ASSERT_TRUE(luma.has_value() && chroma.has_value());
    const ULONG lumaRefsBefore = liveRefs(fixture.luma.Get());

    D3d11SourcePixelProbeSlot slot;
    ASSERT_TRUE(slot.issue(device, context, *luma, *chroma, 1U, 0U, 0U, 0U));
    context->Flush();
    ASSERT_TRUE(slot.completeBounded(context));
    EXPECT_EQ(slot.values().y, 70U);
    EXPECT_EQ(slot.values().cb, 120U);
    EXPECT_EQ(slot.values().cr, 121U);
    EXPECT_EQ(slot.values().bitDepth, 8U);

    // Caller state restored exactly.
    ComPtr<ID3D11ShaderResourceView> restoredSrvs[2U];
    ComPtr<ID3D11RenderTargetView> restoredTarget;
    ComPtr<ID3D11DepthStencilView> restoredDepth;
    ComPtr<ID3D11BlendState> restoredBlend;
    FLOAT restoredBlendFactors[4U] = {};
    UINT restoredSampleMask = 0U;
    UINT restoredViewportCount = static_cast<UINT>(callerViewports.size());
    std::array<D3D11_VIEWPORT, 2U> restoredViewports{};
    context->PSGetShaderResources(0U, 2U, restoredSrvs[0U].ReleaseAndGetAddressOf());
    context->OMGetRenderTargets(
        1U, restoredTarget.ReleaseAndGetAddressOf(), restoredDepth.ReleaseAndGetAddressOf());
    context->OMGetBlendState(
        restoredBlend.ReleaseAndGetAddressOf(), restoredBlendFactors, &restoredSampleMask);
    context->RSGetViewports(&restoredViewportCount, restoredViewports.data());
    EXPECT_EQ(restoredSrvs[0U].Get(), srvViews[0U].Get());
    EXPECT_EQ(restoredSrvs[1U].Get(), srvViews[1U].Get());
    EXPECT_EQ(restoredTarget.Get(), callerTarget.Get());
    EXPECT_EQ(restoredDepth.Get(), nullptr);
    EXPECT_EQ(restoredBlend.Get(), callerBlend.Get());
    EXPECT_EQ(restoredSampleMask, keptSampleMask);
    EXPECT_EQ(restoredViewportCount, 2U);
    for (std::size_t index = 0U; index < 2U; ++index) {
        EXPECT_FLOAT_EQ(restoredViewports[index].TopLeftX, keptViewports[index].TopLeftX);
        EXPECT_FLOAT_EQ(restoredViewports[index].Width, keptViewports[index].Width);
    }

    // The probe call released its captured frame references: no budget pinning.
    EXPECT_EQ(liveRefs(fixture.luma.Get()), lumaRefsBefore);
}

} // namespace
} // namespace dvs::platform
