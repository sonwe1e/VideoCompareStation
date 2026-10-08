#include "dvs/ui/DirectCompare.h"

#include "dvs/application/FrameSet.h"
#include "dvs/application/PlaybackCoordinator.h"
#include "dvs/domain/ComparisonSource.h"
#include "dvs/platform/FrameBudget.h"
#include "dvs/platform/SteadyDeadlineScheduler.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dvs::ui {
namespace {

class DescriptorOnlyMediaProbe final : public application::IMediaProbe {
public:
    [[nodiscard]] application::PortSubmitResult
    submit(const application::MediaProbeRequest&,
           std::shared_ptr<application::IApplicationEventSink>) override {
        return application::PortSubmitResult::Closed;
    }

    void cancel(const application::RequestContext&) noexcept override {}
};

class CoordinatorIngressCloser final {
public:
    explicit CoordinatorIngressCloser(
        std::shared_ptr<application::PlaybackCoordinator> coordinator) noexcept
        : coordinator_(std::move(coordinator)) {}

    ~CoordinatorIngressCloser() {
        if (coordinator_) {
            coordinator_->closeRealtimeIngress();
            coordinator_->closeCriticalIngress();
        }
    }

    CoordinatorIngressCloser(const CoordinatorIngressCloser&) = delete;
    CoordinatorIngressCloser& operator=(const CoordinatorIngressCloser&) = delete;

private:
    std::shared_ptr<application::PlaybackCoordinator> coordinator_;
};

class CapturingRenderChannel final : public application::IRenderChannel {
public:
    [[nodiscard]] application::RenderPublishResult
    publish(const application::FrameRequestContext& context,
            application::FrameSet set) noexcept override {
        try {
            std::scoped_lock lock(mutex_);
            captured_.push_back(CapturedSet{
                .context = context,
                .set = std::move(set),
            });
        } catch (...) {
            return application::RenderPublishResult::Closed;
        }
        return application::RenderPublishResult::Accepted;
    }

    void clear(const application::PlaybackRequestContext& context) noexcept override {
        std::scoped_lock lock(mutex_);
        std::erase_if(captured_, [&context](const CapturedSet& captured) {
            return samePlaybackScope(captured.context.playback, context);
        });
    }

    [[nodiscard]] std::optional<application::FrameSet> latestSet() const {
        std::scoped_lock lock(mutex_);
        if (captured_.empty()) {
            return std::nullopt;
        }
        return captured_.back().set;
    }

    [[nodiscard]] std::optional<application::FrameSetPresented> takeUnconfirmedPresentation() {
        std::scoped_lock lock(mutex_);
        for (CapturedSet& captured : captured_) {
            if (captured.confirmed) {
                continue;
            }
            captured.confirmed = true;
            return application::FrameSetPresented{
                .context = captured.context,
                .frameId = captured.set.canonicalFrameId(),
            };
        }
        return std::nullopt;
    }

private:
    [[nodiscard]] static bool
    samePlaybackScope(const application::PlaybackRequestContext& lhs,
                      const application::PlaybackRequestContext& rhs) noexcept {
        return lhs.request.sessionId == rhs.request.sessionId &&
               lhs.request.sessionEpoch == rhs.request.sessionEpoch &&
               lhs.playbackGeneration == rhs.playbackGeneration;
    }

    struct CapturedSet final {
        application::FrameRequestContext context;
        application::FrameSet set;
        bool confirmed = false;
    };

    mutable std::mutex mutex_;
    std::vector<CapturedSet> captured_;
};

[[nodiscard]] domain::MediaError comparisonError(std::string detail,
                                                 const bool recoverable = true) {
    return domain::makeMediaError(domain::MediaErrorCode::kMediaDecodeFailed,
                                  domain::MediaOperation::kMediaDecode,
                                  std::nullopt,
                                  recoverable,
                                  std::move(detail));
}

[[nodiscard]] std::optional<application::CommandTerminal>
waitForCommandTerminal(const std::shared_ptr<application::PlaybackCoordinator>& coordinator,
                       const std::shared_ptr<CapturingRenderChannel>& renderChannel,
                       const domain::CommandId commandId) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < deadline) {
        while (const std::optional<application::FrameSetPresented> presented =
                   renderChannel->takeUnconfirmedPresentation()) {
            static_cast<void>(coordinator->postCritical(application::ApplicationEvent{*presented}));
        }
        for (application::CommandTerminal terminal : coordinator->takeCompletedCommands()) {
            if (terminal.context.commandId == commandId) {
                return terminal;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return std::nullopt;
}

[[nodiscard]] domain::Result<DirectComparisonResult>
failureForTerminal(const std::optional<application::CommandTerminal>& terminal,
                   const std::string& stage) {
    if (terminal.has_value() && terminal->error.has_value()) {
        return domain::Result<DirectComparisonResult>::failure(*terminal->error);
    }
    if (!terminal.has_value()) {
        return domain::Result<DirectComparisonResult>::failure(comparisonError(
            "The coordinator timed out while waiting for the " + stage + " command."));
    }
    return domain::Result<DirectComparisonResult>::failure(
        comparisonError("The coordinator did not complete the " + stage + " command."));
}

[[nodiscard]] bool commandSucceeded(const std::optional<application::CommandTerminal>& terminal) {
    return terminal.has_value() && terminal->outcome == application::CommandOutcome::Succeeded;
}

} // namespace

domain::Result<DirectComparisonResult>
compareDirectSources(std::shared_ptr<application::IFrameProvider> provider,
                     platform::FrameBudget& frameBudget,
                     const domain::MediaDescriptor& sourceA,
                     const domain::MediaDescriptor& sourceB,
                     const domain::FrameId frameId) {
    if (!provider) {
        return domain::Result<DirectComparisonResult>::failure(
            comparisonError("A direct frame provider is required.", false));
    }
    if (!frameId.isValid()) {
        return domain::Result<DirectComparisonResult>::failure(
            domain::makeMediaError(domain::MediaErrorCode::kInvalidFrameId,
                                   domain::MediaOperation::kMediaDecode,
                                   std::nullopt,
                                   false,
                                   "The requested direct comparison frame is invalid."));
    }

    const auto renderChannel = std::make_shared<CapturingRenderChannel>();
    const auto coordinator = application::PlaybackCoordinator::create(
        domain::SessionId{1},
        application::PlaybackCoordinator::Dependencies{
            .mediaProbe = std::make_shared<DescriptorOnlyMediaProbe>(),
            .directFrameProvider = std::move(provider),
            .deadlineScheduler = std::make_shared<platform::SteadyDeadlineScheduler>(),
            .clock = std::make_shared<platform::SystemSteadyClock>(),
            .renderChannel = renderChannel,
        });
    if (!coordinator) {
        return domain::Result<DirectComparisonResult>::failure(
            comparisonError("The playback coordinator could not be created.", false));
    }
    const CoordinatorIngressCloser closeIngress{coordinator};

    const std::shared_ptr<const application::SessionSnapshot> initial = coordinator->snapshot();
    if (!initial || coordinator->submit(application::OpenDirectComparisonCommand{
                        .context =
                            application::CommandContext{
                                .sessionId = initial->sessionId,
                                .sessionEpoch = initial->sessionEpoch,
                                .commandId = domain::CommandId{1},
                            },
                        .sources =
                            {
                                domain::ComparisonSource{
                                    .id = 0U,
                                    .role = domain::ComparisonRole::kPrediction,
                                    .descriptor = sourceA,
                                    .displayName = sourceA.normalizedPath.filename().string(),
                                },
                                domain::ComparisonSource{
                                    .id = 1U,
                                    .role = domain::ComparisonRole::kPrediction,
                                    .descriptor = sourceB,
                                    .displayName = sourceB.normalizedPath.filename().string(),
                                },
                            },
                    }) != application::PortSubmitResult::Accepted) {
        return domain::Result<DirectComparisonResult>::failure(
            comparisonError("The coordinator did not accept the direct source pair.", true));
    }
    const std::optional<application::CommandTerminal> openTerminal =
        waitForCommandTerminal(coordinator, renderChannel, domain::CommandId{1});
    if (!commandSucceeded(openTerminal)) {
        return failureForTerminal(openTerminal, "open");
    }

    domain::CommandId closeCommandId{2};
    if (frameId != domain::FrameId{0}) {
        const std::shared_ptr<const application::SessionSnapshot> ready = coordinator->snapshot();
        if (!ready || coordinator->submit(application::SeekFrameCommand{
                          .context =
                              application::CommandContext{
                                  .sessionId = ready->sessionId,
                                  .sessionEpoch = ready->sessionEpoch,
                                  .commandId = domain::CommandId{2},
                              },
                          .frameId = frameId,
                      }) != application::PortSubmitResult::Accepted) {
            return domain::Result<DirectComparisonResult>::failure(
                comparisonError("The coordinator did not accept the exact frame seek.", true));
        }
        const std::optional<application::CommandTerminal> seekTerminal =
            waitForCommandTerminal(coordinator, renderChannel, domain::CommandId{2});
        if (!commandSucceeded(seekTerminal)) {
            return failureForTerminal(seekTerminal, "seek");
        }
        closeCommandId = domain::CommandId{3};
    }

    const std::optional<application::FrameSet> set = renderChannel->latestSet();
    if (!set.has_value() || set->canonicalFrameId() != frameId) {
        return domain::Result<DirectComparisonResult>::failure(comparisonError(
            "The coordinator completed without publishing the requested frame set."));
    }
    const application::MappedSourceFrame* const entryA = set->find(0U);
    const application::MappedSourceFrame* const entryB = set->find(1U);
    if (entryA == nullptr || entryB == nullptr || !entryA->hasFrame() || !entryB->hasFrame()) {
        return domain::Result<DirectComparisonResult>::failure(comparisonError(
            "The coordinator completed without a complete frame for both sources."));
    }
    // Both frames are engaged: the guard above returned on !hasFrame().
    const application::FrameHandle& frameA = *entryA->frame;
    const application::FrameHandle& frameB = *entryB->frame;
    const DirectComparisonResult result{
        .frameId = set->canonicalFrameId(),
        .sourceA =
            DirectComparisonFrame{
                .width = frameA.geometry().width,
                .height = frameA.geometry().height,
                .accountedBytes = frameA.accountedBytes(),
            },
        .sourceB =
            DirectComparisonFrame{
                .width = frameB.geometry().width,
                .height = frameB.geometry().height,
                .accountedBytes = frameB.accountedBytes(),
            },
        .reservedBytes = frameBudget.reservedBytes(),
    };

    const std::shared_ptr<const application::SessionSnapshot> readyToClose =
        coordinator->snapshot();
    if (!readyToClose || coordinator->submit(application::CloseSessionCommand{
                             .context =
                                 application::CommandContext{
                                     .sessionId = readyToClose->sessionId,
                                     .sessionEpoch = readyToClose->sessionEpoch,
                                     .commandId = closeCommandId,
                                 },
                         }) != application::PortSubmitResult::Accepted) {
        return domain::Result<DirectComparisonResult>::failure(
            comparisonError("The coordinator did not accept the close command.", true));
    }
    const std::optional<application::CommandTerminal> closeTerminal =
        waitForCommandTerminal(coordinator, renderChannel, closeCommandId);
    if (!commandSucceeded(closeTerminal)) {
        return failureForTerminal(closeTerminal, "close");
    }

    return domain::Result<DirectComparisonResult>::success(result);
}

} // namespace dvs::ui
