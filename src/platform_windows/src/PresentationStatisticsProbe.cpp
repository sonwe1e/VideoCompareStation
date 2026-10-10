#include "dvs/platform/PresentationStatisticsProbe.h"

#include "PresentationStatisticsProbeTestHooks.h"

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_3.h>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <wrl/client.h>

namespace dvs::platform {
namespace {

using Microsoft::WRL::ComPtr;

// Test-only seam (see PresentationStatisticsProbeTestHooks.h). The storage always exists; the
// default-constructed state is behaviourally identical to "no overrides installed".
testing::PresentationStatisticsProbeOverrides g_probeOverridesStorage{};
testing::PresentationStatisticsProbeOverrides const* const g_probeOverrides =
    &g_probeOverridesStorage;

constexpr wchar_t kProbeWindowClassName[] = L"DvsPresentationStatisticsProbeWindow";
constexpr unsigned kMaxPresentedFramesPerCall = 16U;
constexpr unsigned kCapabilityVerificationRounds = 3U;
constexpr unsigned kCapabilityPresentsPerRound = 2U;

[[nodiscard]] std::string hresultText(const HRESULT result) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8)
           << static_cast<std::uint32_t>(result);
    return stream.str();
}

[[nodiscard]] bool isDeviceRemoval(const HRESULT result) noexcept {
    return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
           result == DXGI_ERROR_DRIVER_INTERNAL_ERROR || result == DXGI_ERROR_DEVICE_HUNG;
}

[[nodiscard]] bool isMissingApi(const HRESULT result) noexcept {
    return result == E_NOTIMPL || result == E_NOINTERFACE;
}

[[nodiscard]] LRESULT CALLBACK probeWindowProcedure(const HWND window,
                                                    const UINT message,
                                                    const WPARAM wParam,
                                                    const LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}

[[nodiscard]] const wchar_t* registerProbeWindowClass() {
    // Function-local static: registered once per process and reused by every probe instance.
    static const ATOM registered = [] {
        const WNDCLASSEXW description{
            .cbSize = sizeof(WNDCLASSEXW),
            .lpfnWndProc = &probeWindowProcedure,
            .hInstance = GetModuleHandleW(nullptr),
            .lpszClassName = kProbeWindowClassName,
        };
        return RegisterClassExW(&description);
    }();
    return registered != 0U ? kProbeWindowClassName : nullptr;
}

// A message-only window (HWND_MESSAGE parent) has no DWM surface, so CreateSwapChainForHwnd
// cannot attach presentation to it. A hidden top-level window is the least intrusive window that
// still participates in presentation; it is never shown, so no pixel reaches the user's screen
// and the Qt render loop stays untouched.
[[nodiscard]] HWND createOffscreenProbeWindow(std::string& technicalDetail) {
    if (g_probeOverrides != nullptr && g_probeOverrides->failWindowCreation) {
        technicalDetail = "CreateWindowExW failed (injected): 0x00000005";
        return nullptr;
    }

    const wchar_t* const className = registerProbeWindowClass();
    if (className == nullptr) {
        technicalDetail =
            "RegisterClassExW failed: " + hresultText(HRESULT_FROM_WIN32(GetLastError()));
        return nullptr;
    }

    const HWND window = CreateWindowExW(WS_EX_NOACTIVATE,
                                        className,
                                        L"CompareStation presentation statistics probe",
                                        WS_OVERLAPPED,
                                        -32000,
                                        -32000,
                                        8,
                                        8,
                                        nullptr,
                                        nullptr,
                                        GetModuleHandleW(nullptr),
                                        nullptr);
    if (window == nullptr) {
        technicalDetail =
            "CreateWindowExW failed: " + hresultText(HRESULT_FROM_WIN32(GetLastError()));
    }
    return window;
}

struct ProbeDevice final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediateContext;
    PresentationStatisticsDriverKind driverKind = PresentationStatisticsDriverKind::kWarp;
};

[[nodiscard]] bool
createProbeDevice(const UINT creationFlags, ProbeDevice& created, std::string& technicalDetail) {
    // Hardware first: its statistics include real present cost. WARP stays as the fallback so a
    // software-only or headless session still reports an explicit capability instead of failing.
    struct Attempt final {
        D3D_DRIVER_TYPE type;
        PresentationStatisticsDriverKind kind;
    };
    const Attempt attempts[] = {
        {D3D_DRIVER_TYPE_HARDWARE, PresentationStatisticsDriverKind::kHardware},
        {D3D_DRIVER_TYPE_WARP, PresentationStatisticsDriverKind::kWarp},
    };

    std::string failures;
    for (const Attempt& attempt : attempts) {
        ProbeDevice candidate;
        D3D_FEATURE_LEVEL featureLevel{};
        const HRESULT result = D3D11CreateDevice(nullptr,
                                                 attempt.type,
                                                 nullptr,
                                                 creationFlags,
                                                 nullptr,
                                                 0U,
                                                 D3D11_SDK_VERSION,
                                                 candidate.device.GetAddressOf(),
                                                 &featureLevel,
                                                 candidate.immediateContext.GetAddressOf());
        if (SUCCEEDED(result)) {
            candidate.driverKind = attempt.kind;
            created = std::move(candidate);
            return true;
        }
        failures += " " +
                    std::string{attempt.type == D3D_DRIVER_TYPE_HARDWARE ? "hardware" : "warp"} +
                    "=" + hresultText(result);
    }

    technicalDetail = "D3D11CreateDevice failed:" + failures;
    return false;
}

[[nodiscard]] ComPtr<IDXGISwapChain1>
createProbeSwapChain(ID3D11Device& device, const HWND window, std::string& technicalDetail) {
    // The factory is reached through the device so the chain is created against the device's own
    // adapter, matching how the probe's statistics relate to that device.
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device.QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()))) ||
        FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) {
        technicalDetail = "Resolving IDXGIFactory2 from the probe device failed.";
        return nullptr;
    }

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = 8U;
    description.Height = 8U;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.Stereo = FALSE;
    description.SampleDesc.Count = 1U;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2U;
    description.Scaling = DXGI_SCALING_NONE;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    if (g_probeOverrides != nullptr && g_probeOverrides->forceBltModel) {
        // Legacy bitblt windowed chains need stretch scaling and one buffer; the probe uses this
        // path only under the test seam to observe the documented unsupported-statistics answer.
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.BufferCount = 1U;
    }

    ComPtr<IDXGISwapChain1> swapChain;
    const HRESULT result = factory->CreateSwapChainForHwnd(
        &device, window, &description, nullptr, nullptr, swapChain.GetAddressOf());
    if (FAILED(result)) {
        technicalDetail = "CreateSwapChainForHwnd failed: " + hresultText(result);
        return nullptr;
    }
    return swapChain;
}

} // namespace

class PresentationStatisticsProbe::Impl final {
public:
    Impl() {
        LARGE_INTEGER frequency{};
        if (QueryPerformanceFrequency(&frequency) != FALSE) {
            qpcFrequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
        }
    }

    ~Impl() {
        // Release the swap chain before its host window; both are probe-owned and isolated from
        // Qt Quick's device and swap chains.
        swapChain_.Reset();
        if (window_ != nullptr) {
            static_cast<void>(DestroyWindow(window_));
            window_ = nullptr;
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] PresentationStatisticsCapability ensureCapability() {
        if (lost_) {
            return lostCapability();
        }
        if (capability_.has_value()) {
            return *capability_;
        }

        PresentationStatisticsCapability capability;
        if (!initializeResources(capability)) {
            capability_ = capability;
            return capability;
        }

        gradeFrameStatistics(capability);
        gradeMediaStatistics(capability);
        capability.status = headlineStatus(capability);
        capability_ = capability;
        return capability;
    }

    [[nodiscard]] bool presentForEvidence(const unsigned frameCount) {
        if (lost_ || !ensureReadyForUse()) {
            return false;
        }

        const unsigned boundedCount = (std::min)(frameCount, kMaxPresentedFramesPerCall);
        for (unsigned index = 0U; index < boundedCount; ++index) {
            const HRESULT result = swapChain_->Present(1U, 0U);
            if (FAILED(result)) {
                if (isDeviceRemoval(result)) {
                    markDeviceLost("Present reported device loss: " + hresultText(result));
                }
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] PresentationStatisticsSample sampleFrameStatistics() {
        if (lost_) {
            return lostSample();
        }
        if (!ensureReadyForUse()) {
            return unavailableSample();
        }
        return readFrameStatistics();
    }

    void markDeviceLost(std::string technicalDetail) {
        if (technicalDetail.empty()) {
            technicalDetail = "device lost";
        }
        lost_ = true;
        lostDetail_ = std::move(technicalDetail);
    }

    [[nodiscard]] bool isLost() const noexcept {
        return lost_;
    }

private:
    [[nodiscard]] bool initializeResources(PresentationStatisticsCapability& capability) {
        window_ = createOffscreenProbeWindow(capability.technicalDetail);
        if (window_ == nullptr) {
            capability.status = PresentationStatisticsStatus::kWindowUnavailable;
            return false;
        }

        const UINT creationFlags =
            (g_probeOverrides != nullptr && g_probeOverrides->deviceCreationFlags.has_value())
                ? static_cast<UINT>(*g_probeOverrides->deviceCreationFlags)
                : D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        ProbeDevice device;
        if (!createProbeDevice(creationFlags, device, capability.technicalDetail)) {
            capability.status = PresentationStatisticsStatus::kDeviceUnavailable;
            return false;
        }
        device_ = std::move(device.device);
        immediateContext_ = std::move(device.immediateContext);
        driverKind_ = device.driverKind;
        capability.driverKind = device.driverKind;

        swapChain_ = createProbeSwapChain(*device_.Get(), window_, capability.technicalDetail);
        if (swapChain_ == nullptr) {
            capability.status = PresentationStatisticsStatus::kUnknown;
            return false;
        }

        if (FAILED(swapChain_.As(&mediaChain_))) {
            mediaChain_.Reset();
        }
        resourcesReady_ = true;
        capability.technicalDetail.clear();
        return true;
    }

    void gradeFrameStatistics(PresentationStatisticsCapability& capability) {
        // The probe only claims usable statistics after its own presents made PresentCount move;
        // a steady zero counter is not evidence and stays explicit.
        std::uint32_t highestPresentCount = 0U;
        for (unsigned round = 0U; round < kCapabilityVerificationRounds; ++round) {
            static_cast<void>(presentForEvidence(kCapabilityPresentsPerRound));
            if (lost_) {
                capability.status = PresentationStatisticsStatus::kDeviceUnavailable;
                capability.technicalDetail = lostDetail_;
                return;
            }

            PresentationStatisticsSample sample = readFrameStatistics();
            if (sample.status == PresentationStatisticsStatus::kAvailable &&
                sample.snapshot.has_value()) {
                highestPresentCount =
                    (std::max)(highestPresentCount, sample.snapshot->presentCount);
                if (highestPresentCount > 0U) {
                    capability.frameStatisticsUsable = true;
                    return;
                }
                continue;
            }
            if (sample.status == PresentationStatisticsStatus::kStatisticsDisjoint) {
                // DXGI_ERROR_FRAME_STATISTICS_DISJOINT right after creation is expected to clear.
                continue;
            }
            if (sample.status == PresentationStatisticsStatus::kDeviceUnavailable) {
                capability.status = sample.status;
                capability.technicalDetail = sample.technicalDetail;
                return;
            }
            if (sample.status == PresentationStatisticsStatus::kRequiresFlipModel ||
                sample.status == PresentationStatisticsStatus::kUnknown) {
                capability.status = sample.status;
                capability.technicalDetail = sample.technicalDetail;
                return;
            }
        }

        if (highestPresentCount == 0U) {
            capability.status = PresentationStatisticsStatus::kUnknown;
            capability.technicalDetail =
                "GetFrameStatistics succeeded but PresentCount never advanced after the probe's "
                "own presents; the counters are not usable evidence.";
        } else {
            capability.status = PresentationStatisticsStatus::kStatisticsDisjoint;
            capability.technicalDetail =
                "GetFrameStatistics stayed disjoint after the probe's own presents.";
        }
    }

    void gradeMediaStatistics(PresentationStatisticsCapability& capability) {
        if (mediaChain_ == nullptr) {
            capability.mediaStatus = PresentationStatisticsMediaStatus::kMediaApiMissing;
            capability.technicalDetail += " IDXGISwapChainMedia is not available.";
            return;
        }

        DXGI_FRAME_STATISTICS_MEDIA statistics{};
        const HRESULT result = mediaChain_->GetFrameStatisticsMedia(&statistics);
        if (SUCCEEDED(result)) {
            capability.mediaStatus = PresentationStatisticsMediaStatus::kMediaAvailable;
            capability.mediaStatisticsUsable = true;
            capability.technicalDetail += " GetFrameStatisticsMedia composition mode " +
                                          std::to_string(statistics.CompositionMode) +
                                          ", approved present duration " +
                                          std::to_string(statistics.ApprovedPresentDuration) + ".";
            return;
        }
        if (result == DXGI_ERROR_UNSUPPORTED || isMissingApi(result)) {
            capability.mediaStatus = PresentationStatisticsMediaStatus::kMediaUnsupported;
            capability.technicalDetail +=
                " GetFrameStatisticsMedia refused on this presentation path: " +
                hresultText(result);
            return;
        }
        capability.mediaStatus = PresentationStatisticsMediaStatus::kMediaUnknown;
        capability.technicalDetail += " GetFrameStatisticsMedia failed: " + hresultText(result);
    }

    [[nodiscard]] static PresentationStatisticsStatus
    headlineStatus(const PresentationStatisticsCapability& capability) noexcept {
        if (!capability.frameStatisticsUsable) {
            // gradeFrameStatistics already stored the explicit failure status.
            return capability.status;
        }
        return capability.mediaStatisticsUsable
                   ? PresentationStatisticsStatus::kAvailable
                   : PresentationStatisticsStatus::kMediaStatisticsUnsupported;
    }

    // Lazily builds the probe resources for callers that sample or present before asking for
    // capability. Returns false when the capability is cached as not ready.
    [[nodiscard]] bool ensureReadyForUse() {
        if (resourcesReady_) {
            return true;
        }
        const PresentationStatisticsCapability capability = ensureCapability();
        return capability.frameStatisticsUsable && resourcesReady_;
    }

    [[nodiscard]] PresentationStatisticsSample readFrameStatistics() {
        DXGI_FRAME_STATISTICS statistics{};
        HRESULT result = S_OK;
        if (g_probeOverrides != nullptr && g_probeOverrides->frameStatisticsResult.has_value()) {
            result = static_cast<HRESULT>(*g_probeOverrides->frameStatisticsResult);
            if (g_probeOverrides->frameStatisticsValue.has_value()) {
                statistics = *g_probeOverrides->frameStatisticsValue;
            }
        } else {
            // Hardware Flip Queue systems only refresh statistics after a DwmFlush
            // (IDXGISwapChain::GetFrameStatistics remarks).
            static_cast<void>(DwmFlush());
            result = swapChain_->GetFrameStatistics(&statistics);
        }
        return buildSample(result, statistics);
    }

    [[nodiscard]] PresentationStatisticsSample
    buildSample(const HRESULT result, const DXGI_FRAME_STATISTICS& statistics) {
        PresentationStatisticsSample sample;
        if (isDeviceRemoval(result)) {
            markDeviceLost("GetFrameStatistics reported device loss: " + hresultText(result));
            sample.status = PresentationStatisticsStatus::kDeviceUnavailable;
            sample.technicalDetail = lostDetail_;
            return sample;
        }
        if (result == DXGI_ERROR_UNSUPPORTED) {
            sample.status = PresentationStatisticsStatus::kRequiresFlipModel;
            sample.technicalDetail =
                "GetFrameStatistics refused this windowed bitblt-model chain: " +
                hresultText(result);
            return sample;
        }
        if (result == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
            sample.status = PresentationStatisticsStatus::kStatisticsDisjoint;
            sample.technicalDetail = "DXGI_ERROR_FRAME_STATISTICS_DISJOINT; retry later, never "
                                     "treat the fields as zeros.";
            return sample;
        }
        if (FAILED(result)) {
            sample.status = PresentationStatisticsStatus::kUnknown;
            sample.technicalDetail =
                (isMissingApi(result) ? "GetFrameStatistics is not implemented: "
                                      : "GetFrameStatistics failed: ") +
                hresultText(result);
            return sample;
        }

        const std::uint64_t syncQpcTime =
            static_cast<std::uint64_t>(statistics.SyncQPCTime.QuadPart);
        if (observedQpc_ && syncQpcTime < lastSyncQpcTime_) {
            // QPC is monotonic by contract; a regression means the answer cannot be ordered
            // against earlier evidence, so it is refused instead of handed out.
            sample.status = PresentationStatisticsStatus::kUnknown;
            sample.technicalDetail =
                "SyncQPCTime regressed from " + std::to_string(lastSyncQpcTime_) + " to " +
                std::to_string(syncQpcTime) + "; the sample is not usable evidence.";
            return sample;
        }
        observedQpc_ = true;
        lastSyncQpcTime_ = (std::max)(lastSyncQpcTime_, syncQpcTime);

        sample.status = PresentationStatisticsStatus::kAvailable;
        sample.snapshot = PresentationStatisticsSnapshot{
            .presentCount = statistics.PresentCount,
            .presentRefreshCount = statistics.PresentRefreshCount,
            .syncRefreshCount = statistics.SyncRefreshCount,
            .syncQpcTime = syncQpcTime,
            .qpcFrequency = qpcFrequency_,
            .driverKind = driverKind_,
        };
        return sample;
    }

    [[nodiscard]] PresentationStatisticsCapability lostCapability() const {
        PresentationStatisticsCapability capability;
        capability.status = PresentationStatisticsStatus::kDeviceUnavailable;
        capability.technicalDetail =
            lostDetail_ + " The probe is terminal; build a new probe to retry.";
        return capability;
    }

    [[nodiscard]] PresentationStatisticsSample lostSample() const {
        PresentationStatisticsSample sample;
        sample.status = PresentationStatisticsStatus::kDeviceUnavailable;
        sample.technicalDetail = lostDetail_;
        return sample;
    }

    [[nodiscard]] PresentationStatisticsSample unavailableSample() const {
        PresentationStatisticsSample sample;
        sample.status =
            capability_.has_value() ? capability_->status : PresentationStatisticsStatus::kUnknown;
        sample.technicalDetail = capability_.has_value() ? capability_->technicalDetail
                                                         : "The probe could not be initialized.";
        return sample;
    }

    HWND window_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> immediateContext_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<IDXGISwapChainMedia> mediaChain_;
    PresentationStatisticsDriverKind driverKind_ = PresentationStatisticsDriverKind::kWarp;
    std::optional<PresentationStatisticsCapability> capability_;
    std::string lostDetail_;
    bool lost_ = false;
    bool resourcesReady_ = false;
    std::uint64_t qpcFrequency_ = 0U;
    std::uint64_t lastSyncQpcTime_ = 0U;
    bool observedQpc_ = false;
};

PresentationStatisticsProbe::PresentationStatisticsProbe() : impl_(std::make_unique<Impl>()) {}

PresentationStatisticsProbe::~PresentationStatisticsProbe() = default;

PresentationStatisticsCapability PresentationStatisticsProbe::probeCapability() {
    return impl_->ensureCapability();
}

bool PresentationStatisticsProbe::presentForEvidence(const unsigned frameCount) {
    return impl_->presentForEvidence(frameCount);
}

PresentationStatisticsSample PresentationStatisticsProbe::sampleFrameStatistics() {
    return impl_->sampleFrameStatistics();
}

void PresentationStatisticsProbe::reportDeviceLost(std::string technicalDetail) {
    impl_->markDeviceLost(std::move(technicalDetail));
}

namespace testing {

ScopedPresentationStatisticsProbeOverride::ScopedPresentationStatisticsProbeOverride(
    PresentationStatisticsProbeOverrides overrides) noexcept
    : previous_{g_probeOverridesStorage} {
    g_probeOverridesStorage = overrides;
}

ScopedPresentationStatisticsProbeOverride::~ScopedPresentationStatisticsProbeOverride() {
    g_probeOverridesStorage = previous_;
}

} // namespace testing

} // namespace dvs::platform
