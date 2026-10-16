#include "dvs/platform/GpuTransferActor.h"

#include "dvs/platform/CpuNv12FrameResource.h"
#include "dvs/platform/CpuP010FrameResource.h"
#include "dvs/platform/D3d11DecodedFrameResource.h"
#include "dvs/platform/FrameBudget.h"
#include "dvs/platform/FrameMailbox.h"
#include "dvs/platform/GraphicsDeviceBroker.h"
#include "dvs/platform/RenderActivitySink.h"

#include "D3d11GpuFrameBacking.h"
#include "GpuFrameResource.h"
#include "GpuFrameSet.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <wrl/client.h>

namespace dvs::platform {
namespace {

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

struct PlaybackScopeKey final {
    domain::SessionEpoch sessionEpoch{0U};
    domain::PlaybackGeneration playbackGeneration{0U};
};

[[nodiscard]] PlaybackScopeKey
scopeKey(const application::PlaybackRequestContext& context) noexcept {
    return PlaybackScopeKey{
        .sessionEpoch = context.request.sessionEpoch,
        .playbackGeneration = context.playbackGeneration,
    };
}

[[nodiscard]] int compareScopes(const PlaybackScopeKey& left,
                                const PlaybackScopeKey& right) noexcept {
    if (left.sessionEpoch < right.sessionEpoch) {
        return -1;
    }
    if (left.sessionEpoch > right.sessionEpoch) {
        return 1;
    }
    if (left.playbackGeneration < right.playbackGeneration) {
        return -1;
    }
    if (left.playbackGeneration > right.playbackGeneration) {
        return 1;
    }
    return 0;
}

[[nodiscard]] std::optional<std::size_t> checkedProduct(const std::size_t left,
                                                        const std::size_t right) noexcept {
    if (left != 0U && right > (std::numeric_limits<std::size_t>::max)() / left) {
        return std::nullopt;
    }
    return left * right;
}

[[nodiscard]] std::optional<std::size_t> checkedSum(const std::size_t left,
                                                    const std::size_t right) noexcept {
    if (right > (std::numeric_limits<std::size_t>::max)() - left) {
        return std::nullopt;
    }
    return left + right;
}

struct VisiblePlaneLayout final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t rowBytes = 0U;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct CpuSourceView final {
    domain::SourceId sourceId = 0;
    std::shared_ptr<const application::IFrameResource> resource;
    application::FrameGeometry geometry;
    application::NormalizedFrameFormat format = application::NormalizedFrameFormat::Nv12_8;
    domain::ColorMetadata colorMetadata;
    std::span<const std::uint8_t> yPlane;
    std::span<const std::uint8_t> uvPlane;
    std::uint32_t yStride = 0U;
    std::uint32_t uvStride = 0U;
    VisiblePlaneLayout y;
    VisiblePlaneLayout uv;
    std::size_t gpuBytes = 0U;
};

struct CpuSetView final {
    std::vector<CpuSourceView> sources;
    struct DirectSourceView final {
        domain::SourceId sourceId = 0U;
        std::shared_ptr<const D3d11DecodedFrameResource> resource;
        application::FrameGeometry geometry;
        application::NormalizedFrameFormat format = application::NormalizedFrameFormat::Nv12_8;
        domain::ColorMetadata colorMetadata;
        std::size_t gpuBytes = 0U;
    };
    std::vector<DirectSourceView> directSources;
    std::size_t stagingBytes = 0U;
};

[[nodiscard]] std::optional<CpuSourceView>
inspectSource(const domain::SourceId sourceId, const application::FrameHandle& handle) noexcept {
    if (!handle.isValid()) {
        return std::nullopt;
    }

    std::shared_ptr<const application::IFrameResource> resource;
    application::NormalizedFrameFormat format = application::NormalizedFrameFormat::Nv12_8;
    domain::ColorMetadata colorMetadata;
    std::span<const std::uint8_t> yPlane;
    std::span<const std::uint8_t> uvPlane;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t yStride = 0U;
    std::uint32_t uvStride = 0U;
    std::uint32_t bytesPerSample = 1U;

    if (const auto nv12 =
            std::dynamic_pointer_cast<const CpuNv12FrameResource>(handle.resource())) {
        const Nv12FrameLayout& layout = nv12->layout();
        if (!layout.isValid()) {
            return std::nullopt;
        }
        resource = nv12;
        colorMetadata = nv12->colorMetadata();
        yPlane = nv12->yPlane();
        uvPlane = nv12->uvPlane();
        width = layout.width;
        height = layout.height;
        yStride = layout.yStride;
        uvStride = layout.uvStride;
    } else if (const auto p010 =
                   std::dynamic_pointer_cast<const CpuP010FrameResource>(handle.resource())) {
        const P010FrameLayout& layout = p010->layout();
        if (!layout.isValid()) {
            return std::nullopt;
        }
        resource = p010;
        format = application::NormalizedFrameFormat::P010_10;
        colorMetadata = p010->colorMetadata();
        yPlane = p010->yPlane();
        uvPlane = p010->uvPlane();
        width = layout.width;
        height = layout.height;
        yStride = layout.yStride;
        uvStride = layout.uvStride;
        bytesPerSample = 2U;
    } else {
        return std::nullopt;
    }

    if (!colorMetadata.isValid() || handle.geometry().width != width ||
        handle.geometry().height != height) {
        return std::nullopt;
    }

    const std::uint64_t uvWidth64 = (static_cast<std::uint64_t>(width) + 1U) / 2U;
    const std::uint64_t uvHeight64 = (static_cast<std::uint64_t>(height) + 1U) / 2U;
    const std::uint64_t yRowBytes64 = static_cast<std::uint64_t>(width) * bytesPerSample;
    const std::uint64_t uvRowBytes64 = uvWidth64 * 2U * bytesPerSample;
    if (uvWidth64 > (std::numeric_limits<std::uint32_t>::max)() ||
        uvHeight64 > (std::numeric_limits<std::uint32_t>::max)() ||
        yRowBytes64 > (std::numeric_limits<std::uint32_t>::max)() ||
        uvRowBytes64 > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::nullopt;
    }

    const VisiblePlaneLayout y{
        .width = width,
        .height = height,
        .rowBytes = static_cast<std::uint32_t>(yRowBytes64),
        .format = format == application::NormalizedFrameFormat::P010_10 ? DXGI_FORMAT_R16_UNORM
                                                                        : DXGI_FORMAT_R8_UNORM,
    };
    const VisiblePlaneLayout uv{
        .width = static_cast<std::uint32_t>(uvWidth64),
        .height = static_cast<std::uint32_t>(uvHeight64),
        .rowBytes = static_cast<std::uint32_t>(uvRowBytes64),
        .format = format == application::NormalizedFrameFormat::P010_10 ? DXGI_FORMAT_R16G16_UNORM
                                                                        : DXGI_FORMAT_R8G8_UNORM,
    };
    const std::optional<std::size_t> yBytes = checkedProduct(y.rowBytes, y.height);
    const std::optional<std::size_t> uvBytes = checkedProduct(uv.rowBytes, uv.height);
    if (!yBytes || !uvBytes) {
        return std::nullopt;
    }
    const std::optional<std::size_t> gpuBytes = checkedSum(*yBytes, *uvBytes);
    if (!gpuBytes || *gpuBytes == 0U) {
        return std::nullopt;
    }

    return CpuSourceView{
        .sourceId = sourceId,
        .resource = std::move(resource),
        .geometry = handle.geometry(),
        .format = format,
        .colorMetadata = colorMetadata,
        .yPlane = yPlane,
        .uvPlane = uvPlane,
        .yStride = yStride,
        .uvStride = uvStride,
        .y = y,
        .uv = uv,
        .gpuBytes = *gpuBytes,
    };
}

[[nodiscard]] std::optional<CpuSetView> inspectSet(const application::FrameSet& set) noexcept {
    if (!set.canonicalFrameId().isValid()) {
        return std::nullopt;
    }

    std::vector<CpuSourceView> sources;
    std::vector<CpuSetView::DirectSourceView> directSources;
    std::size_t stagingBytes = 0U;

    for (const application::MappedSourceFrame& entry : set.sources()) {
        // Skip Missing entries — they carry no frame to upload.
        if (!entry.hasFrame()) {
            continue;
        }

        if (const auto direct = std::dynamic_pointer_cast<const D3d11DecodedFrameResource>(
                entry.frame->resource())) {
            if (!direct->colorMetadata().isValid() ||
                direct->byteCount() != entry.frame->accountedBytes()) {
                return std::nullopt;
            }
            directSources.push_back(CpuSetView::DirectSourceView{
                .sourceId = entry.sourceId,
                .resource = direct,
                .geometry = entry.frame->geometry(),
                .format = direct->format(),
                .colorMetadata = direct->colorMetadata(),
                .gpuBytes = direct->byteCount(),
            });
            continue;
        }

        std::optional<CpuSourceView> source = inspectSource(entry.sourceId, *entry.frame);
        if (!source) {
            return std::nullopt;
        }
        const std::optional<std::size_t> newStagingBytes =
            checkedSum(stagingBytes, source->gpuBytes);
        if (!newStagingBytes) {
            return std::nullopt;
        }
        stagingBytes = *newStagingBytes;
        sources.push_back(std::move(*source));
    }

    CpuSetView result{
        .sources = std::move(sources),
        .directSources = std::move(directSources),
        .stagingBytes = stagingBytes,
    };
    return result;
}

struct TransferTask final {
    application::FrameRequestContext context;
    application::FrameSet set;
    CpuSetView cpu;
};

struct UploadPlane final {
    VisiblePlaneLayout layout;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
};

struct UploadSource final {
    const CpuSourceView* cpu = nullptr;
    const CpuSetView::DirectSourceView* direct = nullptr;
    std::optional<FrameBudget::Reservation> gpuReservation;
    UploadPlane y;
    UploadPlane uv;
    ComPtr<ID3D11Query> fence;
};

[[nodiscard]] HRESULT createDirectSource(ID3D11Device* const device,
                                         const CpuSetView::DirectSourceView& direct,
                                         UploadSource& output) noexcept {
    if (direct.resource->deviceGeneration().value() == 0U ||
        direct.format == application::NormalizedFrameFormat::Bgra8) {
        return E_INVALIDARG;
    }
    ComPtr<ID3D11Device> textureDevice;
    direct.resource->texture()->GetDevice(textureDevice.GetAddressOf());
    if (textureDevice.Get() != device) {
        return E_INVALIDARG;
    }

    D3D11_TEXTURE2D_DESC textureDescription{};
    direct.resource->texture()->GetDesc(&textureDescription);
    const DXGI_FORMAT yFormat = direct.format == application::NormalizedFrameFormat::P010_10
                                    ? DXGI_FORMAT_R16_UNORM
                                    : DXGI_FORMAT_R8_UNORM;
    const DXGI_FORMAT uvFormat = direct.format == application::NormalizedFrameFormat::P010_10
                                     ? DXGI_FORMAT_R16G16_UNORM
                                     : DXGI_FORMAT_R8G8_UNORM;
    const auto createView = [&](const DXGI_FORMAT format,
                                ComPtr<ID3D11ShaderResourceView>& view) noexcept {
        D3D11_SHADER_RESOURCE_VIEW_DESC description{};
        description.Format = format;
        description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        description.Texture2DArray.MostDetailedMip = 0U;
        description.Texture2DArray.MipLevels = 1U;
        description.Texture2DArray.FirstArraySlice = direct.resource->arraySlice();
        description.Texture2DArray.ArraySize = 1U;
        return device->CreateShaderResourceView(
            direct.resource->texture(), &description, view.ReleaseAndGetAddressOf());
    };

    // Probe dimensions come from the visible frame region: D3D11VA surfaces are allocated with
    // aligned (padded) texture dimensions, and the readout must address visible samples only.
    // The shader views keep covering the whole texture; the display path crops by geometry.
    const std::uint32_t visibleWidth = direct.geometry.width;
    const std::uint32_t visibleHeight = direct.geometry.height;
    if (visibleWidth == 0U || visibleHeight == 0U || visibleWidth > textureDescription.Width ||
        visibleHeight > textureDescription.Height) {
        return E_INVALIDARG;
    }
    output.direct = &direct;
    output.y.layout = VisiblePlaneLayout{
        .width = visibleWidth,
        .height = visibleHeight,
        .rowBytes = 0U,
        .format = yFormat,
    };
    output.uv.layout = VisiblePlaneLayout{
        .width = (visibleWidth + 1U) / 2U,
        .height = (visibleHeight + 1U) / 2U,
        .rowBytes = 0U,
        .format = uvFormat,
    };
    output.y.texture = direct.resource->texture();
    output.uv.texture = direct.resource->texture();
    HRESULT result = createView(yFormat, output.y.view);
    if (FAILED(result)) {
        return result;
    }
    return createView(uvFormat, output.uv.view);
}

enum class UploadResultKind {
    Published,
    Cancelled,
    Failed,
    DeviceLost,
};

struct UploadResult final {
    UploadResultKind kind = UploadResultKind::Failed;
    HRESULT deviceLossReason = S_OK;
};

[[nodiscard]] std::optional<HRESULT> deviceLossReason(ID3D11Device* const device,
                                                      const HRESULT operationResult) noexcept {
    const HRESULT reason = device->GetDeviceRemovedReason();
    if (FAILED(reason)) {
        return reason;
    }
    if (operationResult == DXGI_ERROR_DEVICE_REMOVED ||
        operationResult == DXGI_ERROR_DEVICE_RESET || operationResult == DXGI_ERROR_DEVICE_HUNG ||
        operationResult == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        return operationResult;
    }
    return std::nullopt;
}

[[nodiscard]] HRESULT createPlane(ID3D11Device* const device,
                                  const VisiblePlaneLayout& layout,
                                  UploadPlane& output) noexcept {
    output.layout = layout;
    D3D11_TEXTURE2D_DESC textureDescription{};
    textureDescription.Width = layout.width;
    textureDescription.Height = layout.height;
    textureDescription.MipLevels = 1U;
    textureDescription.ArraySize = 1U;
    textureDescription.Format = layout.format;
    textureDescription.SampleDesc.Count = 1U;
    textureDescription.Usage = D3D11_USAGE_STAGING;
    textureDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT result = device->CreateTexture2D(
        &textureDescription, nullptr, output.staging.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        return result;
    }

    textureDescription.Usage = D3D11_USAGE_DEFAULT;
    textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    textureDescription.CPUAccessFlags = 0U;
    result = device->CreateTexture2D(
        &textureDescription, nullptr, output.texture.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        return result;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    viewDescription.Format = layout.format;
    viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDescription.Texture2D.MostDetailedMip = 0U;
    viewDescription.Texture2D.MipLevels = 1U;
    return device->CreateShaderResourceView(
        output.texture.Get(), &viewDescription, output.view.ReleaseAndGetAddressOf());
}

[[nodiscard]] HRESULT createUploadSource(ID3D11Device* const device,
                                         const CpuSourceView& cpu,
                                         UploadSource& output) noexcept {
    output.cpu = &cpu;
    HRESULT result = createPlane(device, cpu.y, output.y);
    if (FAILED(result)) {
        return result;
    }
    result = createPlane(device, cpu.uv, output.uv);
    if (FAILED(result)) {
        return result;
    }

    D3D11_QUERY_DESC queryDescription{};
    queryDescription.Query = D3D11_QUERY_EVENT;
    return device->CreateQuery(&queryDescription, output.fence.ReleaseAndGetAddressOf());
}

struct GpuTransferState final {
    GpuTransferState(std::shared_ptr<FrameBudget> budget,
                     std::shared_ptr<GraphicsDeviceBroker> broker,
                     std::shared_ptr<FrameMailbox> mailbox,
                     std::weak_ptr<IRenderActivitySink> activity)
        : frameBudget(std::move(budget)), deviceBroker(std::move(broker)),
          frameMailbox(std::move(mailbox)), renderActivity(std::move(activity)),
          retirementDomain(std::make_shared<GpuFrameRetirementDomain>()) {
        if (!frameBudget || !deviceBroker || !frameMailbox) {
            throw std::invalid_argument{"GPU transfer dependencies must not be null."};
        }
    }

    std::shared_ptr<FrameBudget> frameBudget;
    std::shared_ptr<GraphicsDeviceBroker> deviceBroker;
    std::shared_ptr<FrameMailbox> frameMailbox;
    std::weak_ptr<IRenderActivitySink> renderActivity;

    mutable std::mutex mutex;
    mutable std::condition_variable idle;
    std::condition_variable exitedCondition;
    std::shared_ptr<GpuFrameRetirementDomain> retirementDomain;
    std::optional<TransferTask> pending;
    std::optional<application::FrameRequestContext> latestContext;
    std::optional<PlaybackScopeKey> scopeHighWater;
    std::optional<domain::SessionId> sessionId;
    std::optional<GraphicsDeviceLease> deviceLease;
    bool scopeTombstoned = false;
    bool active = false;
    bool accepting = true;
    bool closing = false;
    bool exited = false;
    bool lossReported = false;
    GpuTransferStatistics statistics;
};

[[nodiscard]] bool acceptsSession(GpuTransferState& state,
                                  const domain::SessionId sessionId) noexcept {
    if (!state.sessionId) {
        state.sessionId = sessionId;
        return true;
    }
    return *state.sessionId == sessionId;
}

[[nodiscard]] bool isTaskCurrentLocked(const GpuTransferState& state,
                                       const application::FrameRequestContext& context) noexcept {
    return !state.closing && state.accepting && !state.scopeTombstoned && state.latestContext &&
           *state.latestContext == context;
}

[[nodiscard]] bool isTaskCurrent(const std::shared_ptr<GpuTransferState>& state,
                                 const application::FrameRequestContext& context) noexcept {
    const std::lock_guard lock{state->mutex};
    return isTaskCurrentLocked(*state, context);
}

[[nodiscard]] bool waitBrieflyOrCancelled(const std::shared_ptr<GpuTransferState>& state,
                                          const application::FrameRequestContext& context) {
    std::unique_lock lock{state->mutex};
    state->retirementDomain->activityCondition().wait_for(
        lock, 1ms, [&state, &context] { return !isTaskCurrentLocked(*state, context); });
    return !isTaskCurrentLocked(*state, context);
}

[[nodiscard]] UploadResult mapPlane(const std::shared_ptr<GpuTransferState>& state,
                                    ID3D11Device* const device,
                                    ID3D11DeviceContext* const context,
                                    const application::FrameRequestContext& taskContext,
                                    UploadPlane& destination,
                                    const std::span<const std::uint8_t> source,
                                    const std::uint32_t sourceStride) noexcept {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT result = E_FAIL;
    for (;;) {
        if (!isTaskCurrent(state, taskContext)) {
            return UploadResult{.kind = UploadResultKind::Cancelled};
        }
        result = context->Map(
            destination.staging.Get(), 0U, D3D11_MAP_WRITE, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (result != DXGI_ERROR_WAS_STILL_DRAWING) {
            break;
        }
        if (waitBrieflyOrCancelled(state, taskContext)) {
            return UploadResult{.kind = UploadResultKind::Cancelled};
        }
    }

    if (FAILED(result)) {
        if (const std::optional<HRESULT> removal = deviceLossReason(device, result)) {
            return UploadResult{
                .kind = UploadResultKind::DeviceLost,
                .deviceLossReason = *removal,
            };
        }
        return UploadResult{.kind = UploadResultKind::Failed};
    }

    if (mapped.pData == nullptr || mapped.RowPitch < destination.layout.rowBytes) {
        context->Unmap(destination.staging.Get(), 0U);
        return UploadResult{.kind = UploadResultKind::Failed};
    }

    auto* const target = static_cast<std::uint8_t*>(mapped.pData);
    for (std::uint32_t row = 0U; row < destination.layout.height; ++row) {
        std::memcpy(target + (static_cast<std::size_t>(row) * mapped.RowPitch),
                    source.data() + (static_cast<std::size_t>(row) * sourceStride),
                    destination.layout.rowBytes);
    }
    context->Unmap(destination.staging.Get(), 0U);
    return UploadResult{.kind = UploadResultKind::Published};
}

[[nodiscard]] UploadResult uploadCpuPlanes(const std::shared_ptr<GpuTransferState>& state,
                                           const GraphicsDeviceLease& lease,
                                           const TransferTask& task,
                                           std::vector<UploadSource>& sources) noexcept {
    ID3D11Device* const device = lease.device.Get();
    ID3D11DeviceContext* const context = lease.immediateContext.Get();

    for (UploadSource& source : sources) {
        UploadResult result = mapPlane(state,
                                       device,
                                       context,
                                       task.context,
                                       source.y,
                                       source.cpu->yPlane,
                                       source.cpu->yStride);
        if (result.kind != UploadResultKind::Published) {
            return result;
        }
        result = mapPlane(state,
                          device,
                          context,
                          task.context,
                          source.uv,
                          source.cpu->uvPlane,
                          source.cpu->uvStride);
        if (result.kind != UploadResultKind::Published) {
            return result;
        }
    }
    return UploadResult{.kind = UploadResultKind::Published};
}

[[nodiscard]] UploadResult waitForFences(const std::shared_ptr<GpuTransferState>& state,
                                         const GraphicsDeviceLease& lease,
                                         const application::FrameRequestContext& taskContext,
                                         std::vector<UploadSource>& sources) noexcept {
    std::vector<bool> complete(sources.size(), false);
    std::size_t completeCount = 0U;

    while (completeCount < sources.size()) {
        if (!isTaskCurrent(state, taskContext)) {
            return UploadResult{.kind = UploadResultKind::Cancelled};
        }

        HRESULT failedResult = S_OK;
        bool anyFailed = false;

        for (std::size_t index = 0U; index < sources.size(); ++index) {
            if (complete[index]) {
                continue;
            }

            const HRESULT result = lease.immediateContext->GetData(
                sources[index].fence.Get(), nullptr, 0U, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (FAILED(result)) {
                failedResult = result;
                anyFailed = true;
                break;
            }
            if (result == S_OK) {
                complete[index] = true;
                ++completeCount;
            }
        }

        if (anyFailed) {
            if (const std::optional<HRESULT> removal =
                    deviceLossReason(lease.device.Get(), failedResult)) {
                return UploadResult{
                    .kind = UploadResultKind::DeviceLost,
                    .deviceLossReason = *removal,
                };
            }
            return UploadResult{.kind = UploadResultKind::Failed};
        }

        if (completeCount < sources.size() && waitBrieflyOrCancelled(state, taskContext)) {
            return UploadResult{.kind = UploadResultKind::Cancelled};
        }
    }

    if (const std::optional<HRESULT> removal = deviceLossReason(lease.device.Get(), S_OK)) {
        return UploadResult{
            .kind = UploadResultKind::DeviceLost,
            .deviceLossReason = *removal,
        };
    }
    return UploadResult{.kind = UploadResultKind::Published};
}

[[nodiscard]] std::shared_ptr<const GpuFrameResource>
makeGpuResource(const std::shared_ptr<GpuTransferState>& state,
                const TransferTask& task,
                UploadSource& upload,
                const domain::SourceId sourceId) noexcept {
    if ((upload.cpu == nullptr && upload.direct == nullptr) ||
        (upload.cpu != nullptr && !upload.gpuReservation)) {
        return {};
    }

    const application::FrameGeometry geometry =
        upload.cpu != nullptr ? upload.cpu->geometry : upload.direct->geometry;
    const domain::ColorMetadata colorMetadata =
        upload.cpu != nullptr ? upload.cpu->colorMetadata : upload.direct->colorMetadata;
    const application::NormalizedFrameFormat format =
        upload.cpu != nullptr ? upload.cpu->format : upload.direct->format;
    std::shared_ptr<const void> lifetimeAnchor;
    if (upload.direct != nullptr) {
        // The decoder-owned resource already reserves these physical texture bytes. Retaining
        // that resource keeps both its FFmpeg surface lifetime and its one budget reservation
        // alive; creating shader views must not charge the same D3D11 texture a second time.
        lifetimeAnchor = upload.direct->resource;
    }
    std::unique_ptr<const IGpuFrameBacking> backing{new (std::nothrow) D3d11GpuFrameBacking{
        std::move(upload.y.texture),
        std::move(upload.y.view),
        D3d11PlaneDimensions{
            .width = upload.y.layout.width,
            .height = upload.y.layout.height,
        },
        std::move(upload.uv.texture),
        std::move(upload.uv.view),
        D3d11PlaneDimensions{
            .width = upload.uv.layout.width,
            .height = upload.uv.layout.height,
        },
        std::move(lifetimeAnchor),
    }};
    if (!backing) {
        return {};
    }

    const auto createResource = [&](GpuFrameAllocation allocation) {
        return GpuFrameResource::createDeferred(
            GpuFrameIdentity{
                .context = task.context,
                .frameId = task.set.canonicalFrameId(),
                .sourceId = sourceId,
            },
            geometry,
            colorMetadata,
            std::move(allocation),
            state->retirementDomain,
            format);
    };
    if (upload.cpu != nullptr) {
        GpuFrameAllocation allocation{std::move(*upload.gpuReservation), std::move(backing)};
        upload.gpuReservation.reset();
        return createResource(std::move(allocation));
    }
    return createResource(GpuFrameAllocation{
        upload.direct->gpuBytes,
        upload.direct->resource,
        std::move(backing),
    });
}

[[nodiscard]] UploadResult performUpload(const std::shared_ptr<GpuTransferState>& state,
                                         const GraphicsDeviceLease& lease,
                                         const TransferTask& task) noexcept {
    if (!isTaskCurrent(state, task.context) ||
        task.context.deviceGeneration != lease.deviceGeneration ||
        state->deviceBroker->currentGeneration() != lease.deviceGeneration) {
        return UploadResult{.kind = UploadResultKind::Cancelled};
    }

    // The comparison contract permits at most three present sources. Missing entries have
    // already been removed by inspectSet and therefore consume no upload slot.
    if ((task.cpu.sources.size() + task.cpu.directSources.size()) > 3U) {
        return UploadResult{.kind = UploadResultKind::Failed};
    }

    // Reserve all staging and newly allocated destination bytes before allocating resources or
    // reading a single decoder-owned byte. Direct D3D11VA textures already carry their own budget
    // reservation and only acquire shader views here.
    std::optional<FrameBudget::Reservation> stagingReservation;
    if (task.cpu.stagingBytes != 0U) {
        stagingReservation = state->frameBudget->tryReserve(task.cpu.stagingBytes);
        if (!stagingReservation) {
            return UploadResult{.kind = UploadResultKind::Failed};
        }
    }

    std::vector<UploadSource> uploadSources;
    uploadSources.reserve(task.cpu.sources.size());
    for (const CpuSourceView& cpuSource : task.cpu.sources) {
        std::optional<FrameBudget::Reservation> reservation =
            state->frameBudget->tryReserve(cpuSource.gpuBytes);
        if (!reservation) {
            return UploadResult{.kind = UploadResultKind::Failed};
        }
        uploadSources.push_back(UploadSource{
            .cpu = &cpuSource,
            .gpuReservation = std::move(reservation),
        });
    }
    std::vector<UploadSource> directSources;
    directSources.reserve(task.cpu.directSources.size());
    for (const CpuSetView::DirectSourceView& directSource : task.cpu.directSources) {
        if (directSource.resource->deviceGeneration() != lease.deviceGeneration) {
            return UploadResult{.kind = UploadResultKind::Cancelled};
        }
        directSources.push_back(UploadSource{
            .direct = &directSource,
        });
    }

    HRESULT result = S_OK;
    for (UploadSource& upload : uploadSources) {
        result = createUploadSource(lease.device.Get(), *upload.cpu, upload);
        if (FAILED(result)) {
            break;
        }
    }
    if (SUCCEEDED(result)) {
        for (UploadSource& upload : directSources) {
            result = createDirectSource(lease.device.Get(), *upload.direct, upload);
            if (FAILED(result)) {
                break;
            }
        }
    }
    if (FAILED(result)) {
        if (const std::optional<HRESULT> removal = deviceLossReason(lease.device.Get(), result)) {
            return UploadResult{
                .kind = UploadResultKind::DeviceLost,
                .deviceLossReason = *removal,
            };
        }
        return UploadResult{.kind = UploadResultKind::Failed};
    }

    UploadResult upload = uploadCpuPlanes(state, lease, task, uploadSources);
    if (upload.kind != UploadResultKind::Published) {
        return upload;
    }
    if (!isTaskCurrent(state, task.context)) {
        return UploadResult{.kind = UploadResultKind::Cancelled};
    }

    for (UploadSource& source : uploadSources) {
        lease.immediateContext->CopyResource(source.y.texture.Get(), source.y.staging.Get());
        lease.immediateContext->CopyResource(source.uv.texture.Get(), source.uv.staging.Get());
        lease.immediateContext->End(source.fence.Get());
    }
    lease.immediateContext->Flush();

    upload = waitForFences(state, lease, task.context, uploadSources);
    if (upload.kind != UploadResultKind::Published) {
        return upload;
    }

    // Staging is no longer needed once all event queries complete. Releasing it before creating
    // the immutable set also returns temporary budget before mailbox retention begins.
    for (UploadSource& source : uploadSources) {
        source.y.staging.Reset();
        source.uv.staging.Reset();
    }
    if (stagingReservation) {
        stagingReservation->reset();
    }

    std::vector<GpuFrameSlot> slots;
    slots.reserve(uploadSources.size());
    for (std::size_t index = 0U; index < uploadSources.size(); ++index) {
        std::shared_ptr<const GpuFrameResource> frame =
            makeGpuResource(state, task, uploadSources[index], uploadSources[index].cpu->sourceId);
        if (!frame) {
            return UploadResult{.kind = UploadResultKind::Failed};
        }
        slots.push_back(GpuFrameSlot{
            .sourceId = uploadSources[index].cpu->sourceId,
            .frame = std::move(frame),
        });
    }
    for (UploadSource& direct : directSources) {
        std::shared_ptr<const GpuFrameResource> frame =
            makeGpuResource(state, task, direct, direct.direct->sourceId);
        if (!frame) {
            return UploadResult{.kind = UploadResultKind::Failed};
        }
        slots.push_back(GpuFrameSlot{
            .sourceId = direct.direct->sourceId,
            .frame = std::move(frame),
        });
    }

    std::shared_ptr<const GpuFrameSet> set =
        GpuFrameSet::create(task.context, task.set.canonicalFrameId(), std::move(slots));
    if (!set) {
        return UploadResult{.kind = UploadResultKind::Failed};
    }

    {
        // clear() and this final context check/publication share the same mutex, making the
        // tombstone linearization explicit instead of relying only on eventual mailbox checks.
        const std::lock_guard lock{state->mutex};
        if (!isTaskCurrentLocked(*state, task.context) ||
            state->deviceBroker->currentGeneration() != task.context.deviceGeneration ||
            !state->deviceLease ||
            state->deviceLease->deviceGeneration != task.context.deviceGeneration) {
            return UploadResult{.kind = UploadResultKind::Cancelled};
        }
        if (state->frameMailbox->publish(set) != FrameMailboxPublishResult::Published) {
            return UploadResult{.kind = UploadResultKind::Cancelled};
        }
        ++state->statistics.publishedSets;
    }

    if (const std::shared_ptr<IRenderActivitySink> activity = state->renderActivity.lock()) {
        activity->notifyFramePublished();
    }

    return UploadResult{.kind = UploadResultKind::Published};
}

void drainRetirement(const std::shared_ptr<GpuTransferState>& state) noexcept {
    const std::size_t count = state->retirementDomain->drainRetired();
    if (count == 0U) {
        return;
    }
    const std::lock_guard lock{state->mutex};
    state->statistics.retiredResources += count;
    state->statistics.lastRetirementThread = std::this_thread::get_id();
    state->idle.notify_all();
}

[[nodiscard]] bool acquireLease(const std::shared_ptr<GpuTransferState>& state,
                                const application::FrameRequestContext& context) {
    for (;;) {
        {
            const std::lock_guard lock{state->mutex};
            if (!isTaskCurrentLocked(*state, context)) {
                return false;
            }
            if (state->deviceLease) {
                return state->deviceLease->deviceGeneration == context.deviceGeneration;
            }
        }

        GraphicsDeviceLeaseResult leaseResult = state->deviceBroker->tryLease();
        if (leaseResult.status == GraphicsDeviceLeaseStatus::Available && leaseResult.lease) {
            const std::lock_guard lock{state->mutex};
            if (!isTaskCurrentLocked(*state, context)) {
                return false;
            }
            if (leaseResult.lease->deviceGeneration != context.deviceGeneration) {
                state->accepting = false;
                state->scopeTombstoned = true;
                state->latestContext.reset();
                state->pending.reset();
                state->frameMailbox->shutdown();
                return false;
            }
            state->deviceLease = std::move(*leaseResult.lease);
            return true;
        }
        if (leaseResult.status != GraphicsDeviceLeaseStatus::Busy) {
            const std::lock_guard lock{state->mutex};
            if (isTaskCurrentLocked(*state, context)) {
                state->accepting = false;
                state->scopeTombstoned = true;
                state->latestContext.reset();
                state->pending.reset();
                state->frameMailbox->shutdown();
            }
            return false;
        }
        if (waitBrieflyOrCancelled(state, context)) {
            return false;
        }
    }
}

void reportLoss(const std::shared_ptr<GpuTransferState>& state, const HRESULT reason) noexcept {
    domain::DeviceGeneration expectedGeneration{0U};
    {
        const std::lock_guard lock{state->mutex};
        if (state->lossReported) {
            return;
        }
        state->lossReported = true;
        state->accepting = false;
        state->scopeTombstoned = true;
        state->latestContext.reset();
        state->pending.reset();
        if (state->deviceLease) {
            expectedGeneration = state->deviceLease->deviceGeneration;
        }
        state->frameMailbox->shutdown();
    }

    GraphicsDeviceBrokerResult report = GraphicsDeviceBrokerResult::Busy;
    while (report == GraphicsDeviceBrokerResult::Busy) {
        report = state->deviceBroker->reportDeviceLost(expectedGeneration, reason);
        if (report == GraphicsDeviceBrokerResult::Busy) {
            {
                const std::lock_guard lock{state->mutex};
                if (state->closing) {
                    break;
                }
            }
            std::this_thread::yield();
        }
    }
    if (report == GraphicsDeviceBrokerResult::Lost ||
        report == GraphicsDeviceBrokerResult::AlreadyUnavailable) {
        const std::lock_guard lock{state->mutex};
        ++state->statistics.deviceLossReports;
    }
    state->retirementDomain->notifyActivity();
}

void invalidateForBrokerTransition(const std::shared_ptr<GpuTransferState>& state) noexcept {
    const domain::DeviceGeneration brokerGeneration = state->deviceBroker->currentGeneration();
    const std::lock_guard lock{state->mutex};
    if (state->closing) {
        return;
    }

    std::optional<domain::DeviceGeneration> expectedGeneration;
    if (state->deviceLease) {
        expectedGeneration = state->deviceLease->deviceGeneration;
    } else if (state->latestContext) {
        expectedGeneration = state->latestContext->deviceGeneration;
    }
    if (!expectedGeneration || *expectedGeneration == brokerGeneration) {
        return;
    }

    state->accepting = false;
    state->scopeTombstoned = true;
    state->latestContext.reset();
    state->pending.reset();
    if (brokerGeneration > *expectedGeneration) {
        static_cast<void>(state->frameMailbox->advanceDeviceGeneration(brokerGeneration));
    } else {
        state->frameMailbox->shutdown();
    }
    state->idle.notify_all();
}

void runActor(const std::shared_ptr<GpuTransferState>& state) noexcept {
    {
        const std::lock_guard lock{state->mutex};
        state->statistics.workerThread = std::this_thread::get_id();
    }

    for (;;) {
        drainRetirement(state);
        invalidateForBrokerTransition(state);

        std::optional<TransferTask> task;
        {
            std::unique_lock lock{state->mutex};
            if (state->closing) {
                state->pending.reset();
                if (!state->active && state->retirementDomain->liveResourceCount() == 0U &&
                    !state->retirementDomain->hasRetiredResources()) {
                    state->deviceLease.reset();
                    state->exited = true;
                    state->exitedCondition.notify_all();
                    state->idle.notify_all();
                    return;
                }
            } else if (state->pending) {
                task = std::move(state->pending);
                state->pending.reset();
                state->active = true;
            }

            if (!task) {
                state->retirementDomain->activityCondition().wait_for(lock, 5ms, [&state] {
                    return state->closing || state->pending.has_value() ||
                           state->retirementDomain->hasRetiredResources();
                });
                continue;
            }
        }

        UploadResult result{.kind = UploadResultKind::Cancelled};
        const bool directOnly = task->cpu.sources.empty() && !task->cpu.directSources.empty();
        const auto transferStarted = std::chrono::steady_clock::now();
        if (acquireLease(state, task->context)) {
            result = performUpload(state, *state->deviceLease, *task);
        }
        const auto transferMicroseconds =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - transferStarted)
                                           .count());
        task.reset();
        if (result.kind == UploadResultKind::DeviceLost) {
            reportLoss(state, result.deviceLossReason);
        }

        {
            const std::lock_guard lock{state->mutex};
            if (result.kind == UploadResultKind::Failed) {
                ++state->statistics.failedSets;
            } else if (result.kind == UploadResultKind::Cancelled) {
                ++state->statistics.cancelledSets;
            }
            ++state->statistics.completedTransfers;
            state->statistics.totalTransferMicroseconds += transferMicroseconds;
            state->statistics.maximumTransferMicroseconds =
                (std::max)(state->statistics.maximumTransferMicroseconds, transferMicroseconds);
            if (result.kind == UploadResultKind::Published && directOnly) {
                ++state->statistics.zeroCopySets;
            }
            state->active = false;
            state->idle.notify_all();
        }
    }
}

} // namespace

class GpuTransferActor::Impl final {
public:
    Impl(std::shared_ptr<FrameBudget> frameBudget,
         std::shared_ptr<GraphicsDeviceBroker> deviceBroker,
         std::shared_ptr<FrameMailbox> frameMailbox,
         std::weak_ptr<IRenderActivitySink> renderActivity)
        : state_(std::make_shared<GpuTransferState>(std::move(frameBudget),
                                                    std::move(deviceBroker),
                                                    std::move(frameMailbox),
                                                    std::move(renderActivity))),
          worker_([state = state_] { runActor(state); }) {}

    ~Impl() {
        if (!worker_.joinable()) {
            return;
        }

        bool closeAlreadyRequested = false;
        bool alreadyExited = false;
        {
            const std::lock_guard lock{state_->mutex};
            closeAlreadyRequested = state_->closing;
            alreadyExited = state_->exited;
        }
        if (!closeAlreadyRequested) {
            if (shutdown(2s)) {
                return;
            }
        } else if (alreadyExited) {
            worker_.join();
            return;
        }

        // A render publication may legitimately outlive the bounded composition wait. The worker
        // and device lease remain owned by shared state until that publication retires; detaching
        // avoids either a render-thread COM release or an unbounded destructor join.
        worker_.detach();
    }

    [[nodiscard]] GpuTransferSubmitResult submit(const application::FrameRequestContext& context,
                                                 application::FrameSet set) noexcept {
        std::optional<CpuSetView> inspected = inspectSet(set);
        if (!inspected) {
            return GpuTransferSubmitResult::InvalidSet;
        }
        GraphicsDeviceLeaseResult leaseResult = state_->deviceBroker->tryLease();
        if (leaseResult.status == GraphicsDeviceLeaseStatus::Closed) {
            return GpuTransferSubmitResult::Closed;
        }
        if (state_->deviceBroker->currentGeneration() != context.deviceGeneration) {
            return GpuTransferSubmitResult::StaleContext;
        }
        if (leaseResult.status == GraphicsDeviceLeaseStatus::Unavailable) {
            return GpuTransferSubmitResult::DeviceUnavailable;
        }
        if (leaseResult.status == GraphicsDeviceLeaseStatus::Available &&
            (!leaseResult.lease ||
             leaseResult.lease->deviceGeneration != context.deviceGeneration)) {
            return GpuTransferSubmitResult::StaleContext;
        }

        const std::lock_guard lock{state_->mutex};
        if (!state_->accepting || state_->closing) {
            return GpuTransferSubmitResult::Closed;
        }
        if (state_->deviceLease &&
            state_->deviceLease->deviceGeneration != context.deviceGeneration) {
            return GpuTransferSubmitResult::StaleContext;
        }
        if (!state_->deviceLease && leaseResult.lease) {
            state_->deviceLease = std::move(*leaseResult.lease);
        }
        if (!acceptsSession(*state_, context.playback.request.sessionId)) {
            return GpuTransferSubmitResult::StaleContext;
        }

        const PlaybackScopeKey incomingScope = scopeKey(context.playback);
        if (state_->scopeHighWater) {
            const int order = compareScopes(incomingScope, *state_->scopeHighWater);
            if (order < 0 || (order == 0 && state_->scopeTombstoned)) {
                return GpuTransferSubmitResult::StaleContext;
            }
        }

        const bool replaced = state_->active || state_->pending.has_value();
        state_->scopeHighWater = incomingScope;
        state_->scopeTombstoned = false;
        state_->latestContext = context;
        state_->pending.emplace(TransferTask{
            .context = context,
            .set = std::move(set),
            .cpu = std::move(*inspected),
        });
        ++state_->statistics.submittedSets;
        if (replaced) {
            ++state_->statistics.replacedSets;
        }
        state_->retirementDomain->notifyActivity();
        return replaced ? GpuTransferSubmitResult::Replaced : GpuTransferSubmitResult::Accepted;
    }

    [[nodiscard]] bool clear(const application::PlaybackRequestContext& context) noexcept {
        const std::lock_guard lock{state_->mutex};
        if (!state_->accepting || state_->closing ||
            !acceptsSession(*state_, context.request.sessionId)) {
            return false;
        }

        const PlaybackScopeKey clearingScope = scopeKey(context);
        if (state_->scopeHighWater) {
            const int order = compareScopes(clearingScope, *state_->scopeHighWater);
            if (order < 0 || (order == 0 && state_->scopeTombstoned)) {
                return false;
            }
        }

        state_->scopeHighWater = clearingScope;
        state_->scopeTombstoned = true;
        state_->latestContext.reset();
        state_->pending.reset();
        const bool cleared = state_->frameMailbox->clear(context);
        state_->retirementDomain->notifyActivity();
        state_->idle.notify_all();
        return cleared;
    }

    [[nodiscard]] bool waitUntilIdle(const std::chrono::milliseconds timeout) const noexcept {
        std::unique_lock lock{state_->mutex};
        return state_->idle.wait_for(lock, timeout, [this] {
            return (!state_->active && !state_->pending.has_value()) || state_->exited;
        });
    }

    [[nodiscard]] bool shutdown(const std::chrono::milliseconds timeout) noexcept {
        {
            const std::lock_guard lock{state_->mutex};
            if (!state_->closing) {
                state_->closing = true;
                state_->accepting = false;
                state_->scopeTombstoned = true;
                state_->latestContext.reset();
                state_->pending.reset();
                state_->frameMailbox->shutdown();
            }
        }
        state_->retirementDomain->notifyActivity();

        bool exited = false;
        {
            std::unique_lock lock{state_->mutex};
            exited =
                state_->exitedCondition.wait_for(lock, timeout, [this] { return state_->exited; });
        }
        if (exited && worker_.joinable()) {
            worker_.join();
        }
        return exited;
    }

    [[nodiscard]] bool isClosed() const noexcept {
        const std::lock_guard lock{state_->mutex};
        return !state_->accepting;
    }

    [[nodiscard]] GpuTransferStatistics statistics() const noexcept {
        const std::lock_guard lock{state_->mutex};
        return state_->statistics;
    }

private:
    std::shared_ptr<GpuTransferState> state_;
    std::thread worker_;
};

GpuTransferActor::GpuTransferActor(std::shared_ptr<FrameBudget> frameBudget,
                                   std::shared_ptr<GraphicsDeviceBroker> deviceBroker,
                                   std::shared_ptr<FrameMailbox> frameMailbox,
                                   std::weak_ptr<IRenderActivitySink> renderActivity)
    : impl_(std::make_unique<Impl>(std::move(frameBudget),
                                   std::move(deviceBroker),
                                   std::move(frameMailbox),
                                   std::move(renderActivity))) {}

GpuTransferActor::~GpuTransferActor() = default;

GpuTransferSubmitResult GpuTransferActor::submit(const application::FrameRequestContext& context,
                                                 application::FrameSet set) noexcept {
    return impl_->submit(context, std::move(set));
}

bool GpuTransferActor::clear(const application::PlaybackRequestContext& context) noexcept {
    return impl_->clear(context);
}

bool GpuTransferActor::waitUntilIdle(const std::chrono::milliseconds timeout) const noexcept {
    return impl_->waitUntilIdle(timeout);
}

bool GpuTransferActor::shutdown(const std::chrono::milliseconds timeout) noexcept {
    return impl_->shutdown(timeout);
}

bool GpuTransferActor::isClosed() const noexcept {
    return impl_->isClosed();
}

GpuTransferStatistics GpuTransferActor::statistics() const noexcept {
    return impl_->statistics();
}

} // namespace dvs::platform
