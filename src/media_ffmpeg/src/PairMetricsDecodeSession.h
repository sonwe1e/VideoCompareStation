#pragma once

#include "dvs/domain/MediaDescriptor.h"
#include "dvs/domain/Result.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace dvs::media::internal {

// Dedicated pair-metrics decoder. It converts decoder-owned frames to tightly packed RGBA8 and
// never creates a playback FrameHandle, NV12 resource, FrameSet, render cache entry, or
// FrameBudget reservation. Modeled on SignatureDecodeSession: same bounded exact-seek back-off,
// same strict timestamp acceptance, independent demuxer per source.
class PairMetricsDecodeSession final {
public:
    struct RgbaFrame final {
        std::vector<std::uint8_t> pixels;
        std::uint32_t width = 0;
        std::uint32_t height = 0;

        [[nodiscard]] bool isEmpty() const noexcept {
            return pixels.empty() || width == 0 || height == 0;
        }
    };

    PairMetricsDecodeSession(domain::SourceId sourceId, domain::MediaDescriptor descriptor);
    ~PairMetricsDecodeSession();

    PairMetricsDecodeSession(const PairMetricsDecodeSession&) = delete;
    PairMetricsDecodeSession& operator=(const PairMetricsDecodeSession&) = delete;
    PairMetricsDecodeSession(PairMetricsDecodeSession&&) = delete;
    PairMetricsDecodeSession& operator=(PairMetricsDecodeSession&&) = delete;

    [[nodiscard]] domain::Status open(const std::atomic<bool>& cancellationRequested);
    [[nodiscard]] domain::Result<RgbaFrame>
    decodeRgba(domain::FrameId frameId, const std::atomic<bool>& cancellationRequested);

    void requestInterrupt() noexcept;
    void close() noexcept;
    // True when the session can serve the descriptor without reopening (same source identity,
    // geometry, and frame count).
    [[nodiscard]] bool matches(const domain::MediaDescriptor& descriptor) const noexcept;
    [[nodiscard]] bool isOpen() const noexcept;

private:
    [[nodiscard]] domain::Result<RgbaFrame>
    decodeInternal(domain::FrameId frameId,
                   const std::atomic<bool>& cancellationRequested,
                   bool continueSequentially,
                   bool allowTimelineRecovery,
                   std::size_t seekOrdinalBackOff = 0U);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::media::internal
