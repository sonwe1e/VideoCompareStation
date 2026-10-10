#pragma once

#ifndef _WIN32
#error "PresentationStatisticsProbe test hooks are only supported on Windows."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <dxgi.h>
#include <optional>
#include <windows.h>

namespace dvs::platform::testing {

// This private seam lets component tests reach documented failure paths of the presentation
// statistics probe that cannot be induced deterministically from outside: a rejected offscreen
// window, D3D11 creation flags the runtime refuses, the windowed bitblt-model statistics path,
// and HRESULT/statistics answers the local driver would never produce. It is intentionally not
// part of the platform adapter's public interface and exists only while a test installs it.
struct PresentationStatisticsProbeOverrides final {
    // Emulate CreateWindowExW failure for the offscreen host window.
    bool failWindowCreation = false;

    // Replacement creation flags passed to D3D11CreateDevice for both the hardware and the WARP
    // attempt; tests inject a flag bit the runtime rejects with E_INVALIDARG.
    std::optional<unsigned int> deviceCreationFlags;

    // Create the probe chain with the legacy bitblt model instead of flip model to reach the
    // documented "GetFrameStatistics unsupported for windowed bitblt chains" answer.
    bool forceBltModel = false;

    // When set, the probe answers GetFrameStatistics with this HRESULT and never calls DXGI.
    std::optional<long> frameStatisticsResult;

    // When set together with frameStatisticsResult == S_OK, the probe reports these statistics
    // verbatim; used for exact counter mapping and QPC-regression checks.
    std::optional<DXGI_FRAME_STATISTICS> frameStatisticsValue;
};

class ScopedPresentationStatisticsProbeOverride final {
public:
    // Installs the overrides process-wide for the lifetime of the guard. The probe API is
    // single-threaded; install guards from the same thread that calls the probe.
    explicit ScopedPresentationStatisticsProbeOverride(
        PresentationStatisticsProbeOverrides overrides) noexcept;
    ~ScopedPresentationStatisticsProbeOverride();

    ScopedPresentationStatisticsProbeOverride(const ScopedPresentationStatisticsProbeOverride&) =
        delete;
    ScopedPresentationStatisticsProbeOverride&
    operator=(const ScopedPresentationStatisticsProbeOverride&) = delete;

private:
    PresentationStatisticsProbeOverrides previous_;
};

} // namespace dvs::platform::testing
