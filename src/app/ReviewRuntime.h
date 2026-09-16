#pragma once

#include "dvs/application/PlaybackTrace.h"
#include "dvs/media/DecoderBackend.h"
#include "dvs/media/MediaProbe.h"
#include "dvs/media/MultiSourceFrameProvider.h"
#include "dvs/platform/GpuTransferActor.h"
#include "dvs/ui/RenderAckRelay.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dvs::ui {
class ComparisonSurface;
class ReviewController;
class ReviewPreferencesController;
} // namespace dvs::ui

namespace dvs::app {

// Owns the complete direct-review adapter graph. The desktop host releases the QML scene graph
// between prepareForSceneGraphRelease() and shutdownAfterSceneGraphRelease() so the render node
// drops its pinned front pair before the transfer actor is drained.
class ReviewRuntime final {
public:
    [[nodiscard]] static std::unique_ptr<ReviewRuntime> create();
    ~ReviewRuntime();

    ReviewRuntime(const ReviewRuntime&) = delete;
    ReviewRuntime& operator=(const ReviewRuntime&) = delete;
    ReviewRuntime(ReviewRuntime&&) = delete;
    ReviewRuntime& operator=(ReviewRuntime&&) = delete;

    [[nodiscard]] ui::ReviewController* controller() noexcept;
    [[nodiscard]] ui::ReviewPreferencesController* preferences() noexcept;
    [[nodiscard]] bool attachSurface(ui::ComparisonSurface& surface) noexcept;
    [[nodiscard]] std::vector<media::DecoderBackendStatus> decoderBackendStatuses() const;
    [[nodiscard]] media::MediaProbeStatistics mediaProbeStatistics() const noexcept;
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

    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::app
