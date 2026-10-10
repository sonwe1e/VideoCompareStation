#include "dvs/platform/PresentationStatisticsProbe.h"

#include "PresentationStatisticsProbeTestHooks.h"

#include <cstdint>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <windows.h>
#include <wrl/client.h>

namespace dvs::platform {
namespace {

// Frame statistics can lag the last present by a vblank, so read until two consecutive answers
// agree before comparing counters. A bounded miss returns nullopt; the caller decides whether
// that is a skip or a failure for the scenario under test.
[[nodiscard]] std::optional<PresentationStatisticsSnapshot>
drainedSample(PresentationStatisticsProbe& probe) {
    std::optional<PresentationStatisticsSnapshot> previous;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const PresentationStatisticsSample sample = probe.sampleFrameStatistics();
        if (sample.status != PresentationStatisticsStatus::kAvailable ||
            !sample.snapshot.has_value()) {
            return std::nullopt;
        }
        if (previous.has_value() && previous->presentCount == sample.snapshot->presentCount) {
            return sample.snapshot;
        }
        previous = sample.snapshot;
    }
    return previous;
}

[[nodiscard]] bool isExplicitUsableHeadline(const PresentationStatisticsStatus status) noexcept {
    return status == PresentationStatisticsStatus::kAvailable ||
           status == PresentationStatisticsStatus::kMediaStatisticsUnsupported;
}

[[nodiscard]] bool isExplicitFailureHeadline(const PresentationStatisticsStatus status) noexcept {
    switch (status) {
    case PresentationStatisticsStatus::kRequiresFlipModel:
    case PresentationStatisticsStatus::kStatisticsDisjoint:
    case PresentationStatisticsStatus::kDeviceUnavailable:
    case PresentationStatisticsStatus::kWindowUnavailable:
    case PresentationStatisticsStatus::kUnknown:
        return true;
    case PresentationStatisticsStatus::kAvailable:
    case PresentationStatisticsStatus::kMediaStatisticsUnsupported:
        return false;
    }
    return false;
}

TEST(PresentationStatisticsProbeTests, CapabilityReportsExplicitGradedStatusAndEvidenceScope) {
    PresentationStatisticsProbe probe;
    const PresentationStatisticsCapability capability = probe.probeCapability();

    // Whatever this machine answers, the headline must be an explicit grade and internally
    // consistent; no path silently reports usable or zeroed counters.
    if (capability.frameStatisticsUsable) {
        ASSERT_TRUE(isExplicitUsableHeadline(capability.status))
            << "status=" << static_cast<int>(capability.status);
        ASSERT_TRUE(capability.driverKind.has_value());
        EXPECT_EQ(capability.mediaStatisticsUsable,
                  capability.status == PresentationStatisticsStatus::kAvailable);
        EXPECT_EQ(capability.mediaStatisticsUsable,
                  capability.mediaStatus == PresentationStatisticsMediaStatus::kMediaAvailable);
    } else {
        ASSERT_TRUE(isExplicitFailureHeadline(capability.status))
            << "status=" << static_cast<int>(capability.status);
        EXPECT_NE(capability.status, PresentationStatisticsStatus::kAvailable);
        // The media API answers independently of the legacy counter path: on remote-display
        // sessions the legacy PresentCount can lag past the probe's verification window while
        // GetFrameStatisticsMedia still succeeds. The honest combination is "legacy counters
        // unconfirmed + media path answered", so only the media pair must stay self-consistent.
        EXPECT_EQ(capability.mediaStatisticsUsable,
                  capability.mediaStatus == PresentationStatisticsMediaStatus::kMediaAvailable);
    }

    // The evidence scope must tell consumers that these counters come from the probe's own swap
    // chain and cannot be matched frame-by-frame with the Qt scene graph.
    EXPECT_EQ(capability.evidenceScope.find("probe-owned offscreen swap chain"), 0U);
    EXPECT_NE(capability.evidenceScope.find("interval-level correlation"), std::string::npos);
    EXPECT_NE(capability.evidenceScope.find("no per-frame correspondence claimed"),
              std::string::npos);

    if (!capability.frameStatisticsUsable) {
        // Unavailable outcomes carry a technical detail, never a silent zero. A later sample may
        // still succeed when the legacy counters advanced after the capability window closed;
        // such a sample must carry an explicit available grade, never ungraded numbers.
        EXPECT_FALSE(capability.technicalDetail.empty());
        const PresentationStatisticsSample sample = probe.sampleFrameStatistics();
        if (sample.snapshot.has_value()) {
            EXPECT_EQ(sample.status, PresentationStatisticsStatus::kAvailable);
        }
    }
}

TEST(PresentationStatisticsProbeTests, OffscreenWindowFailureIsWindowUnavailableNotZero) {
    const testing::ScopedPresentationStatisticsProbeOverride guard{
        testing::PresentationStatisticsProbeOverrides{.failWindowCreation = true}};
    PresentationStatisticsProbe probe;
    const PresentationStatisticsCapability capability = probe.probeCapability();

    EXPECT_EQ(capability.status, PresentationStatisticsStatus::kWindowUnavailable);
    EXPECT_FALSE(capability.frameStatisticsUsable);
    EXPECT_FALSE(capability.driverKind.has_value());
    EXPECT_NE(capability.technicalDetail.find("CreateWindowExW"), std::string::npos);

    const PresentationStatisticsSample sample = probe.sampleFrameStatistics();
    EXPECT_EQ(sample.status, PresentationStatisticsStatus::kWindowUnavailable);
    EXPECT_FALSE(sample.snapshot.has_value());
    EXPECT_FALSE(probe.presentForEvidence(1U));
}

TEST(PresentationStatisticsProbeTests, RejectedDeviceCreationFlagsAreDeviceUnavailable) {
    // 0x00001000 is not a defined D3D11_CREATE_DEVICE flag for this runtime; the local probe
    // under out/verification/presentation-probe recorded E_INVALIDARG for both hardware and WARP.
    const testing::ScopedPresentationStatisticsProbeOverride guard{
        testing::PresentationStatisticsProbeOverrides{.deviceCreationFlags = 0x00001000U}};
    PresentationStatisticsProbe probe;
    const PresentationStatisticsCapability capability = probe.probeCapability();

    EXPECT_EQ(capability.status, PresentationStatisticsStatus::kDeviceUnavailable);
    EXPECT_FALSE(capability.frameStatisticsUsable);
    EXPECT_NE(capability.technicalDetail.find("D3D11CreateDevice failed"), std::string::npos);
    EXPECT_NE(capability.technicalDetail.find("0x80070057"), std::string::npos);

    const PresentationStatisticsSample sample = probe.sampleFrameStatistics();
    EXPECT_EQ(sample.status, PresentationStatisticsStatus::kDeviceUnavailable);
    EXPECT_FALSE(sample.snapshot.has_value());
    EXPECT_FALSE(probe.presentForEvidence(1U));
}

TEST(PresentationStatisticsProbeTests, WindowedBltModelChainMapsToRequiresFlipModel) {
    // IDXGISwapChain::GetFrameStatistics is documented as unusable for windowed bitblt-model
    // chains; the test seam forces that presentation model to observe the mapping.
    const testing::ScopedPresentationStatisticsProbeOverride guard{
        testing::PresentationStatisticsProbeOverrides{.forceBltModel = true}};
    PresentationStatisticsProbe probe;
    const PresentationStatisticsCapability capability = probe.probeCapability();

    ASSERT_EQ(capability.status, PresentationStatisticsStatus::kRequiresFlipModel)
        << capability.technicalDetail;
    EXPECT_FALSE(capability.frameStatisticsUsable);
    EXPECT_NE(capability.technicalDetail.find("GetFrameStatistics"), std::string::npos);

    const PresentationStatisticsSample sample = probe.sampleFrameStatistics();
    EXPECT_EQ(sample.status, PresentationStatisticsStatus::kRequiresFlipModel);
    EXPECT_FALSE(sample.snapshot.has_value());
}

TEST(PresentationStatisticsProbeTests, AvailablePathAdvancesPresentCountAndKeepsQpcMonotonic) {
    PresentationStatisticsProbe probe;
    const PresentationStatisticsCapability capability = probe.probeCapability();
    if (!capability.frameStatisticsUsable) {
        GTEST_SKIP() << "No usable frame statistics on this machine: "
                     << capability.technicalDetail;
    }

    LARGE_INTEGER frequency{};
    ASSERT_NE(QueryPerformanceFrequency(&frequency), 0);

    const std::optional<PresentationStatisticsSnapshot> baseline = drainedSample(probe);
    ASSERT_TRUE(baseline.has_value());
    EXPECT_EQ(baseline->qpcFrequency, static_cast<std::uint64_t>(frequency.QuadPart));

    ASSERT_TRUE(probe.presentForEvidence(3U));
    const std::optional<PresentationStatisticsSnapshot> after = drainedSample(probe);
    ASSERT_TRUE(after.has_value());
    EXPECT_GE(after->presentCount, baseline->presentCount + 1U);
    EXPECT_GE(after->syncQpcTime, baseline->syncQpcTime);
    EXPECT_EQ(after->driverKind, baseline->driverKind);

    // A zero-frame request must not generate evidence.
    ASSERT_TRUE(probe.presentForEvidence(0U));
    const std::optional<PresentationStatisticsSnapshot> unchanged = drainedSample(probe);
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(unchanged->presentCount, after->presentCount);
}

TEST(PresentationStatisticsProbeTests, RepeatedCapabilityCallsAndSamplesAreStable) {
    PresentationStatisticsProbe probe;
    const PresentationStatisticsCapability first = probe.probeCapability();
    const PresentationStatisticsCapability second = probe.probeCapability();

    EXPECT_EQ(first.status, second.status);
    EXPECT_EQ(first.mediaStatus, second.mediaStatus);
    EXPECT_EQ(first.frameStatisticsUsable, second.frameStatisticsUsable);
    EXPECT_EQ(first.mediaStatisticsUsable, second.mediaStatisticsUsable);
    EXPECT_EQ(first.driverKind.has_value(), second.driverKind.has_value());
    EXPECT_EQ(first.technicalDetail, second.technicalDetail);
    EXPECT_EQ(first.evidenceScope, second.evidenceScope);

    if (!first.frameStatisticsUsable) {
        GTEST_SKIP() << "No usable frame statistics on this machine: " << first.technicalDetail;
    }
    const std::optional<PresentationStatisticsSnapshot> left = drainedSample(probe);
    const std::optional<PresentationStatisticsSnapshot> right = drainedSample(probe);
    ASSERT_TRUE(left.has_value());
    ASSERT_TRUE(right.has_value());
    EXPECT_EQ(left->presentCount, right->presentCount);
    EXPECT_EQ(left->syncQpcTime, right->syncQpcTime);
}

// Ground truth for the media-statistics grade: a minimal flip-model chain built by the test
// itself answers GetFrameStatisticsMedia, and the probe must classify that answer the same way.
[[nodiscard]] std::optional<long> localFlipChainMediaAnswer() {
    using Microsoft::WRL::ComPtr;

    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL featureLevel{};
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr,
                                 D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr,
                                 0U,
                                 D3D11_SDK_VERSION,
                                 device.GetAddressOf(),
                                 &featureLevel,
                                 context.GetAddressOf())) &&
        FAILED(D3D11CreateDevice(nullptr,
                                 D3D_DRIVER_TYPE_WARP,
                                 nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr,
                                 0U,
                                 D3D11_SDK_VERSION,
                                 device.GetAddressOf(),
                                 &featureLevel,
                                 context.GetAddressOf()))) {
        return std::nullopt;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) {
        return std::nullopt;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(WNDCLASSEXW);
    windowClass.lpfnWndProc = &DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"DvsPresentationProbeMediaTruthWindow";
    static_cast<void>(RegisterClassExW(&windowClass));
    const HWND window = CreateWindowExW(0U,
                                        windowClass.lpszClassName,
                                        L"probe media truth",
                                        WS_OVERLAPPED,
                                        -32000,
                                        -32000,
                                        8,
                                        8,
                                        nullptr,
                                        nullptr,
                                        windowClass.hInstance,
                                        nullptr);
    if (window == nullptr) {
        return std::nullopt;
    }

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = 8U;
    description.Height = 8U;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1U;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2U;
    description.Scaling = DXGI_SCALING_NONE;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swapChain;
    const HRESULT created = factory->CreateSwapChainForHwnd(
        device.Get(), window, &description, nullptr, nullptr, swapChain.GetAddressOf());
    if (FAILED(created)) {
        static_cast<void>(DestroyWindow(window));
        return std::nullopt;
    }

    static_cast<void>(swapChain->Present(1U, 0U));
    ComPtr<IDXGISwapChainMedia> media;
    long answer = E_NOINTERFACE;
    if (SUCCEEDED(swapChain.As(&media))) {
        // Media statistics can be refused until presentation has actually started; mirror the
        // probe's verification cadence (presents plus DwmFlush) before asking.
        for (unsigned round = 0U; round < 3U; ++round) {
            static_cast<void>(swapChain->Present(1U, 0U));
            static_cast<void>(DwmFlush());
            DXGI_FRAME_STATISTICS_MEDIA statistics{};
            answer = static_cast<long>(media->GetFrameStatisticsMedia(&statistics));
            if (SUCCEEDED(answer)) {
                break;
            }
        }
    }
    swapChain.Reset();
    static_cast<void>(DestroyWindow(window));
    return answer;
}

TEST(PresentationStatisticsProbeTests, MediaStatisticsGradeMatchesTheActualPathAnswer) {
    PresentationStatisticsProbe probe;
    const PresentationStatisticsCapability capability = probe.probeCapability();
    if (!capability.frameStatisticsUsable) {
        GTEST_SKIP() << "No usable frame statistics on this machine: "
                     << capability.technicalDetail;
    }

    const std::optional<long> truth = localFlipChainMediaAnswer();
    ASSERT_TRUE(truth.has_value()) << "The test could not build its own flip chain.";
    const HRESULT mediaAnswer = static_cast<HRESULT>(*truth);

    if (SUCCEEDED(mediaAnswer)) {
        EXPECT_EQ(capability.mediaStatus, PresentationStatisticsMediaStatus::kMediaAvailable);
        EXPECT_EQ(capability.status, PresentationStatisticsStatus::kAvailable);
        EXPECT_TRUE(capability.mediaStatisticsUsable);
    } else if (mediaAnswer == DXGI_ERROR_UNSUPPORTED || mediaAnswer == E_NOTIMPL ||
               mediaAnswer == E_NOINTERFACE) {
        // Microsoft documents the media variant as valid only on some presentation paths; a
        // refusal on this plain HWND flip chain must surface as unavailable, never as zeros.
        EXPECT_NE(capability.mediaStatus, PresentationStatisticsMediaStatus::kMediaAvailable);
        EXPECT_FALSE(capability.mediaStatisticsUsable);
        EXPECT_EQ(capability.status, PresentationStatisticsStatus::kMediaStatisticsUnsupported);
        EXPECT_NE(capability.technicalDetail.find("GetFrameStatisticsMedia"), std::string::npos);
    } else {
        EXPECT_EQ(capability.status, PresentationStatisticsStatus::kMediaStatisticsUnsupported);
    }
}

TEST(PresentationStatisticsProbeTests, InjectedStatisticsMapExactlyAndQpcRegressionIsRejected) {
    PresentationStatisticsProbe probe;
    const PresentationStatisticsCapability capability = probe.probeCapability();
    if (!capability.frameStatisticsUsable) {
        GTEST_SKIP() << "No usable frame statistics on this machine: "
                     << capability.technicalDetail;
    }

    // The injected QPC must sit above anything the probe observed during capability probing, so
    // derive it from the live counter instead of guessing the machine's uptime.
    LARGE_INTEGER liveQpc{};
    ASSERT_NE(QueryPerformanceCounter(&liveQpc), 0);

    DXGI_FRAME_STATISTICS injected{};
    injected.PresentCount = 12U;
    injected.PresentRefreshCount = 34U;
    injected.SyncRefreshCount = 56U;
    injected.SyncQPCTime.QuadPart = liveQpc.QuadPart + 1'000'000;

    {
        const testing::ScopedPresentationStatisticsProbeOverride guard{
            testing::PresentationStatisticsProbeOverrides{
                .frameStatisticsResult = static_cast<long>(S_OK),
                .frameStatisticsValue = injected,
            }};
        const PresentationStatisticsSample sample = probe.sampleFrameStatistics();
        ASSERT_EQ(sample.status, PresentationStatisticsStatus::kAvailable)
            << sample.technicalDetail;
        ASSERT_TRUE(sample.snapshot.has_value());
        // Exact counter mapping; an off-by-one in the probe shows up here immediately.
        EXPECT_EQ(sample.snapshot->presentCount, 12U);
        EXPECT_EQ(sample.snapshot->presentRefreshCount, 34U);
        EXPECT_EQ(sample.snapshot->syncRefreshCount, 56U);
        EXPECT_EQ(sample.snapshot->syncQpcTime,
                  static_cast<std::uint64_t>(liveQpc.QuadPart + 1'000'000));
        EXPECT_EQ(sample.snapshot->driverKind, capability.driverKind);
    }

    {
        DXGI_FRAME_STATISTICS regressed = injected;
        regressed.PresentCount = 13U;
        regressed.SyncQPCTime.QuadPart = injected.SyncQPCTime.QuadPart - 1'000;
        const testing::ScopedPresentationStatisticsProbeOverride guard{
            testing::PresentationStatisticsProbeOverrides{
                .frameStatisticsResult = static_cast<long>(S_OK),
                .frameStatisticsValue = regressed,
            }};
        const PresentationStatisticsSample sample = probe.sampleFrameStatistics();
        // QPC is monotonic by contract; a regression is refused instead of handed out as
        // evidence.
        EXPECT_EQ(sample.status, PresentationStatisticsStatus::kUnknown);
        EXPECT_FALSE(sample.snapshot.has_value());
        EXPECT_NE(sample.technicalDetail.find("regressed"), std::string::npos);
    }
}

TEST(PresentationStatisticsProbeTests, DeviceRemovedAnswerIsTerminalDeviceUnavailable) {
    PresentationStatisticsProbe probe;
    const PresentationStatisticsCapability capability = probe.probeCapability();
    if (!capability.frameStatisticsUsable) {
        GTEST_SKIP() << "No usable frame statistics on this machine: "
                     << capability.technicalDetail;
    }

    {
        const testing::ScopedPresentationStatisticsProbeOverride guard{
            testing::PresentationStatisticsProbeOverrides{
                .frameStatisticsResult = static_cast<long>(DXGI_ERROR_DEVICE_REMOVED),
            }};
        const PresentationStatisticsSample sample = probe.sampleFrameStatistics();
        EXPECT_EQ(sample.status, PresentationStatisticsStatus::kDeviceUnavailable);
        EXPECT_FALSE(sample.snapshot.has_value());
        EXPECT_NE(sample.technicalDetail.find("0x887A0005"), std::string::npos);
    }

    // The loss is terminal for this probe instance.
    EXPECT_EQ(probe.probeCapability().status, PresentationStatisticsStatus::kDeviceUnavailable);
    EXPECT_EQ(probe.sampleFrameStatistics().status,
              PresentationStatisticsStatus::kDeviceUnavailable);
    EXPECT_FALSE(probe.presentForEvidence(2U));
}

TEST(PresentationStatisticsProbeTests, ExternallyReportedDeviceLossIsTerminalAndExplained) {
    PresentationStatisticsProbe probe;
    static_cast<void>(probe.probeCapability());

    probe.reportDeviceLost("external test signal 0x887A0005");

    const PresentationStatisticsCapability capability = probe.probeCapability();
    EXPECT_EQ(capability.status, PresentationStatisticsStatus::kDeviceUnavailable);
    EXPECT_FALSE(capability.frameStatisticsUsable);
    EXPECT_FALSE(capability.mediaStatisticsUsable);
    EXPECT_NE(capability.technicalDetail.find("external test signal"), std::string::npos);

    const PresentationStatisticsSample sample = probe.sampleFrameStatistics();
    EXPECT_EQ(sample.status, PresentationStatisticsStatus::kDeviceUnavailable);
    EXPECT_FALSE(sample.snapshot.has_value());
    EXPECT_FALSE(probe.presentForEvidence(1U));
}

} // namespace
} // namespace dvs::platform
