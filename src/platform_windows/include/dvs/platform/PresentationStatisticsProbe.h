#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dvs::platform {

// Graded outcome of probing system presentation statistics (route item P10, evidence level
// "system presentation record"). Every non-available outcome is explicit: unavailable paths,
// missing APIs and permission-related failures surface as their own status, never as zeroed
// counters.
enum class PresentationStatisticsStatus {
    // Frame statistics were retrieved from the probe's own swap chain.
    kAvailable,
    // IDXGISwapChain::GetFrameStatistics is documented as unusable for swap chains that both use
    // the bit-block transfer model and draw in windowed mode; DXGI answers DXGI_ERROR_UNSUPPORTED
    // there. The probe's flip-model path is required.
    kRequiresFlipModel,
    // Partial headline state: frame statistics work, but the DXGI 1.3 media-statistics path
    // (IDXGISwapChainMedia::GetFrameStatisticsMedia) is refused on this presentation path.
    kMediaStatisticsUnsupported,
    // DXGI_ERROR_FRAME_STATISTICS_DISJOINT: the statistics are not cohesive right now (for
    // example immediately after swap-chain creation or a mode change). Retry later; never
    // treat as zeros.
    kStatisticsDisjoint,
    // D3D11 device creation failed (no usable driver, rejected creation flags) or the device was
    // lost/removed after creation. Terminal for the probe instance.
    kDeviceUnavailable,
    // The offscreen host window could not be created.
    kWindowUnavailable,
    // API missing, an unexpected HRESULT, or statistics that succeed but never advance; the
    // technical detail explains which. The probe refuses to present such a state as evidence.
    kUnknown,
};

// Which adapter backs the probe's device. WARP statistics still exercise the OS presentation
// path but say nothing about the cost of real hardware presents.
enum class PresentationStatisticsDriverKind {
    kHardware,
    kWarp,
};

// Granular state of the DXGI 1.3 media-statistics variant. Microsoft documents this family as
// valid only on some presentation paths; a plain HWND flip chain is frequently refused.
enum class PresentationStatisticsMediaStatus {
    kMediaAvailable,
    kMediaUnsupported,
    kMediaApiMissing,
    kMediaUnknown,
};

// Scope note attached to every capability answer. The probe never adopts, wraps or paces the Qt
// Quick render loop or any existing swap chain, so its evidence can only be correlated with the
// Qt scenario at interval level. It must never be shown as a per-frame correspondence.
inline constexpr std::string_view kProbeEvidenceScope =
    "probe-owned offscreen swap chain; interval-level correlation with the Qt scene graph only; "
    "no per-frame correspondence claimed";

struct PresentationStatisticsCapability final {
    PresentationStatisticsStatus status = PresentationStatisticsStatus::kUnknown;
    PresentationStatisticsMediaStatus mediaStatus =
        PresentationStatisticsMediaStatus::kMediaUnknown;
    bool frameStatisticsUsable = false;
    bool mediaStatisticsUsable = false;
    bool isIndirectDisplayPath = false;
    std::optional<PresentationStatisticsDriverKind> driverKind;
    std::string technicalDetail;
    std::string evidenceScope{std::string_view{kProbeEvidenceScope}};
};

// Read-only copy of the last IDXGISwapChain::GetFrameStatistics answer of the probe's own swap
// chain. PresentCount counts presented images, not Present calls (DXGI semantics), so it is
// evidence about the probe chain's own presentation only. syncQpcTime is in QueryPerformanceCounter
// units; qpcFrequency carries the counter frequency so callers can convert without assuming a
// particular timer resolution.
struct PresentationStatisticsSnapshot final {
    std::uint32_t presentCount = 0U;
    std::uint32_t presentRefreshCount = 0U;
    std::uint32_t syncRefreshCount = 0U;
    std::uint64_t syncQpcTime = 0U;
    std::uint64_t qpcFrequency = 0U;
    PresentationStatisticsDriverKind driverKind = PresentationStatisticsDriverKind::kWarp;
};

struct PresentationStatisticsSample final {
    PresentationStatisticsStatus status = PresentationStatisticsStatus::kUnknown;
    std::optional<PresentationStatisticsSnapshot> snapshot;
    std::string technicalDetail;
};

// Independent presentation-statistics probe (P10 proof of concept). It creates its own minimal
// D3D11 device (hardware first, WARP fallback) and its own flip-model swap chain hosted by a
// never-shown offscreen window:
//  - a message-only window (HWND_MESSAGE parent) has no DWM surface, so CreateSwapChainForHwnd
//    cannot target it; a hidden top-level window is the least intrusive window that still
//    participates in presentation without a pixel reaching the user's screen;
//  - the probe never touches the Qt render loop or any existing swap chain, so constructing it -
//    or never calling it - cannot affect playback. That is the fallback: when the probe reports
//    anything but success, application-level evidence (render/commit traces) remains usable
//    unchanged.
// All entry points are synchronous, run on the caller's thread, and create no worker threads, no
// UI and no wiring. D3D/DXGI types never appear in this interface.
class PresentationStatisticsProbe final {
public:
    PresentationStatisticsProbe();
    ~PresentationStatisticsProbe();

    PresentationStatisticsProbe(const PresentationStatisticsProbe&) = delete;
    PresentationStatisticsProbe& operator=(const PresentationStatisticsProbe&) = delete;
    PresentationStatisticsProbe(PresentationStatisticsProbe&&) = delete;
    PresentationStatisticsProbe& operator=(PresentationStatisticsProbe&&) = delete;

    // Creates the probe resources on first call and reports the graded capability. The probe only
    // claims usable frame statistics after its own present burst actually made PresentCount
    // advance. Repeated calls return the cached outcome unchanged; a reported device loss makes
    // every later call answer kDeviceUnavailable. Status and technical details are heap-allocated
    // strings, so only std::bad_alloc can escape; probe failures are reported, never thrown.
    [[nodiscard]] PresentationStatisticsCapability probeCapability();

    // Presents up to 16 frames (clamped) on the probe's own swap chain with sync interval 1 to
    // generate statistics evidence. Returns false when the probe is unavailable or lost; a
    // frameCount of zero is a successful no-op on a ready probe.
    [[nodiscard]] bool presentForEvidence(unsigned frameCount);

    // Read-only snapshot from the probe's own swap chain. When the returned status is not
    // kAvailable there is no snapshot and technicalDetail says why; counters are never
    // fabricated. Statistics whose QPC ordering regresses are rejected as kUnknown for that call
    // rather than handed out.
    [[nodiscard]] PresentationStatisticsSample sampleFrameStatistics();

    // External loss signal (for example a device-removed HRESULT observed elsewhere). Terminal
    // for this probe instance; build a new probe to retry.
    void reportDeviceLost(std::string technicalDetail);

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::platform
