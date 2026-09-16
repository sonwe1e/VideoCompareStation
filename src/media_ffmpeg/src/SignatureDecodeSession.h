#pragma once

#include "dvs/application/Alignment.h"
#include "dvs/domain/MediaDescriptor.h"
#include "dvs/domain/Result.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dvs::media::internal {

// Dedicated analysis decoder. It downsamples decoder-owned luma directly and never creates a
// playback FrameHandle, NV12 resource, FrameSet, render cache entry, or FrameBudget reservation.
class SignatureDecodeSession final {
public:
    using Progress = std::function<void(std::uint64_t)>;

    SignatureDecodeSession(domain::SourceId sourceId, domain::MediaDescriptor descriptor);
    ~SignatureDecodeSession();

    SignatureDecodeSession(const SignatureDecodeSession&) = delete;
    SignatureDecodeSession& operator=(const SignatureDecodeSession&) = delete;
    SignatureDecodeSession(SignatureDecodeSession&&) = delete;
    SignatureDecodeSession& operator=(SignatureDecodeSession&&) = delete;

    [[nodiscard]] domain::Status open(const std::atomic<bool>& cancellationRequested);
    [[nodiscard]] domain::Result<application::FrameLumaSignature>
    decodeSignature(domain::FrameId frameId, const std::atomic<bool>& cancellationRequested);
    [[nodiscard]] domain::Result<std::vector<application::FrameLumaSignature>>
    decodeRange(domain::FrameId firstFrame,
                std::int64_t frameCount,
                const std::atomic<bool>& cancellationRequested,
                Progress progress = {});

    void requestInterrupt() noexcept;
    void close() noexcept;

private:
    [[nodiscard]] domain::Result<application::FrameLumaSignature>
    decodeInternal(domain::FrameId frameId,
                   const std::atomic<bool>& cancellationRequested,
                   bool continueSequentially,
                   bool allowTimelineRecovery);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::media::internal
