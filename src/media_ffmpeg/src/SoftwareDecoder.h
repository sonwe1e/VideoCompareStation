#pragma once

#include "dvs/application/FrameHandle.h"
#include "dvs/domain/ComparisonSource.h"
#include "dvs/domain/MediaDescriptor.h"
#include "dvs/domain/Result.h"
#include "dvs/media/DecoderBackend.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace dvs::platform {
class FrameBudget;
class GraphicsDeviceBroker;
} // namespace dvs::platform

namespace dvs::media::internal {

struct DecodedFrame final {
    application::FrameHandle handle;
    domain::MediaTime presentationTime;
};

// One decoder owns one demuxer and codec context. It is intentionally used only by the provider
// actor that created it; no source pair ever shares seek or packet state.
class SoftwareDecoder final {
public:
    SoftwareDecoder(domain::SourceId sourceId,
                    domain::MediaDescriptor descriptor,
                    platform::FrameBudget& frameBudget,
                    const std::atomic<bool>* externalInterrupt = nullptr,
                    std::shared_ptr<platform::GraphicsDeviceBroker> deviceBroker = {},
                    std::uint32_t softwareThreadCount = 0U);
    ~SoftwareDecoder();

    SoftwareDecoder(const SoftwareDecoder&) = delete;
    SoftwareDecoder& operator=(const SoftwareDecoder&) = delete;
    SoftwareDecoder(SoftwareDecoder&&) = delete;
    SoftwareDecoder& operator=(SoftwareDecoder&&) = delete;

    [[nodiscard]] domain::Status open(const std::atomic<bool>& cancellationRequested);
    [[nodiscard]] domain::Result<DecodedFrame>
    decodeExact(domain::FrameId frameId, const std::atomic<bool>& cancellationRequested);
    [[nodiscard]] domain::Result<DecodedFrame>
    decodeSequential(domain::FrameId frameId, const std::atomic<bool>& cancellationRequested);

    // Internal diagnostic used by component tests to keep the sequential path honest.
    [[nodiscard]] std::uint64_t exactSeekCount() const noexcept;
    // True when the most recent decode returned because the request was canceled or interrupted
    // rather than because the source failed. An interrupted decode leaves the demuxer and codec
    // in a clean, re-seekable state, so the owning actor must not reopen the source for it.
    [[nodiscard]] bool lastDecodeInterrupted() const noexcept;
    [[nodiscard]] media::DecoderBackend backend() const noexcept;
    [[nodiscard]] std::string fallbackReason() const;
    [[nodiscard]] domain::DeviceGeneration deviceGeneration() const noexcept;

    void requestInterrupt() noexcept;
    void close() noexcept;

private:
    [[nodiscard]] domain::Result<DecodedFrame>
    decodeInternal(domain::FrameId frameId,
                   const std::atomic<bool>& cancellationRequested,
                   bool continueSequentially,
                   bool allowTimelineRecovery,
                   std::size_t seekOrdinalBackOff = 0U);

    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::media::internal
