#pragma once

#include "dvs/application/PlaybackTrace.h"
#include "dvs/media/DecoderBackend.h"
#include "dvs/media/MediaProbe.h"
#include "dvs/media/MultiSourceFrameProvider.h"
#include "dvs/platform/GpuTransferActor.h"
#include "dvs/ui/RenderAckRelay.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace dvs {
namespace application {
class IClipExporter;
class IIssueRecordRepository;
class ISampleExporter;
struct SessionSnapshot;
} // namespace application
namespace ui {
class ComparisonSurface;
class PairMetricsController;
class PreviewThumbnailController;
class ReviewController;
class ReviewPreferencesController;
} // namespace ui
namespace app {

// Owns the complete direct-review adapter graph. The desktop host releases the QML scene graph
// between prepareForSceneGraphRelease() and shutdownAfterSceneGraphRelease() so the render node
// drops its pinned front pair before the transfer actor is drained.
class ReviewRuntime final {
public:
    [[nodiscard]] static std::unique_ptr<ReviewRuntime> create();
    // Same composition, but the settings repository reads/writes the given file instead of
    // the user's roaming settings. UI evidence automation uses this so repeated runs never
    // touch (or resume from) the user's real playback positions. Empty path = production
    // behaviour, identical to create().
    [[nodiscard]] static std::unique_ptr<ReviewRuntime>
    createWithSettingsFile(const std::filesystem::path& settingsFile);
    ~ReviewRuntime();

    ReviewRuntime(const ReviewRuntime&) = delete;
    ReviewRuntime& operator=(const ReviewRuntime&) = delete;
    ReviewRuntime(ReviewRuntime&&) = delete;
    ReviewRuntime& operator=(ReviewRuntime&&) = delete;

    [[nodiscard]] ui::ReviewController* controller() noexcept;
    [[nodiscard]] ui::ReviewPreferencesController* preferences() noexcept;
    // GUI-thread pair-metrics projection over the independent media metrics service.
    [[nodiscard]] ui::PairMetricsController* pairMetrics() noexcept;
    // GUI-thread timeline hover thumbnails decoded outside the playback pipeline.
    [[nodiscard]] ui::PreviewThumbnailController* previewThumbnails() noexcept;
    // T7 issue-record persistence port owned by the composition root (architecture: ui_qml
    // must not link persistence_json).
    [[nodiscard]] application::IIssueRecordRepository* issueRecordRepository() noexcept;
    // Lossless range clip-export port owned by the composition root (architecture: ui_qml must not
    // link media_ffmpeg). Shared so a running export survives the teardown hand-off.
    [[nodiscard]] std::shared_ptr<application::IClipExporter> clipExporter() const noexcept;
    // E01 training sample triplet export port owned by the composition root.
    [[nodiscard]] std::shared_ptr<application::ISampleExporter> sampleExporter() const noexcept;
    [[nodiscard]] bool attachSurface(ui::ComparisonSurface& surface) noexcept;
    [[nodiscard]] std::vector<media::DecoderBackendStatus> decoderBackendStatuses() const;
    // E03: the coordinator's published session snapshot (playback accounting fields included) for
    // the performance report path. Same thread-safe pull the UI controllers use.
    [[nodiscard]] std::shared_ptr<const application::SessionSnapshot> sessionSnapshot() const;
    [[nodiscard]] media::MediaProbeStatistics mediaProbeStatistics() const noexcept;
    // Bytes the pair-metrics worker pins for RGBA conversion staging. Reported separately from
    // the decoded-frame budget: it is transcode staging, not a decoded-frame cache entry.
    [[nodiscard]] std::size_t pairMetricsScratchBytes() const noexcept;
    [[nodiscard]] media::FrameProviderStatistics frameProviderStatistics() const noexcept;
    [[nodiscard]] std::uint64_t decodedSignatureCount() const noexcept;
    [[nodiscard]] platform::GpuTransferStatistics transferStatistics() const noexcept;
    [[nodiscard]] ui::RenderAckRelayStatistics renderRelayStatistics() const noexcept;
    [[nodiscard]] std::size_t reservedFrameBytes() const noexcept;

    // Holds a trace sink (e.g. FileTraceSink) for the lifetime of the runtime so the
    // PlaybackTrace sink pointer stays valid until the event loop exits. No-op if unset.
    void setTraceSink(std::shared_ptr<application::ITraceSink> sink) noexcept;

    // GUI-thread phase. Stops UI ingress and detaches non-owning QML/render references without
    // waiting on media, render, or acknowledgement workers.
    void prepareForSceneGraphRelease() noexcept;

    // Called after the desktop host has destroyed its QML engine/window. Adapter teardown is
    // transferred to a detached control task and this call returns within the seven-second total
    // shutdown budget. Controller and relay QObjects remain owned by the creating GUI thread. A
    // false result means teardown is still running; the executable host must terminate without
    // normal static destruction rather than return from main while that work remains active.
    [[nodiscard]] bool shutdownAfterSceneGraphRelease() noexcept;

private:
    ReviewRuntime();
    explicit ReviewRuntime(const std::filesystem::path& settingsFile);

    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace app
} // namespace dvs
