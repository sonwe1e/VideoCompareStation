#include "D3d11SourcePixelProbe.h"

#include <array>
#include <chrono>
#include <cstring>
#include <dvs/platform/shaders/DvsNv12Shaders.generated.h>
#include <thread>

namespace dvs::platform {
namespace {

using Microsoft::WRL::ComPtr;

struct alignas(16) ProbeTexelConstants final {
    std::uint32_t lumaX = 0U;
    std::uint32_t lumaY = 0U;
    std::uint32_t chromaX = 0U;
    std::uint32_t chromaY = 0U;
};

static_assert(sizeof(ProbeTexelConstants) == 16U);

[[nodiscard]] float bitsAsFloat(const std::uint32_t bits) {
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// An UNORM view returns sample/containerMax as a float. Both the division on the GPU and the
// multiply back land within half an ulp of the exact rational for 8-bit samples and 16-bit
// containers, so round-to-nearest recovers the exact stored code; P010 codes then drop the 6
// zero low bits. No colour conversion, filtering, or 8-bit quantization happens in between.
[[nodiscard]] std::uint32_t recoverCode(const float sample, const std::uint32_t containerMax) {
    const double recovered =
        (static_cast<double>(sample) * static_cast<double>(containerMax)) + 0.5;
    if (recovered <= 0.0) {
        return 0U;
    }
    const auto code = static_cast<std::uint32_t>(recovered);
    return code > containerMax ? containerMax : code;
}

[[nodiscard]] bool planeIsUsable(const D3d11ProbePlane& plane) {
    return plane.view != nullptr && plane.visibleWidth != 0U && plane.visibleHeight != 0U &&
           plane.containerMax != 0U;
}

// The draw wrote (raw luma bits, raw Cb bits, raw Cr bits, 0) as uint4.
[[nodiscard]] D3d11SourcePixel decodedValues(const std::array<std::uint32_t, 3U>& raw,
                                             const D3d11ProbePlane& luma,
                                             const D3d11ProbePlane& chroma) {
    D3d11SourcePixel decoded{};
    decoded.y = recoverCode(bitsAsFloat(raw[0U]), luma.containerMax) >> luma.codeShift;
    decoded.cb = recoverCode(bitsAsFloat(raw[1U]), chroma.containerMax) >> chroma.codeShift;
    decoded.cr = recoverCode(bitsAsFloat(raw[2U]), chroma.containerMax) >> chroma.codeShift;
    decoded.bitDepth = luma.codeShift == 0U ? 8U : 10U;
    return decoded;
}

[[nodiscard]] std::uint32_t planeContainerMax(const DXGI_FORMAT format, bool* const known) {
    switch (format) {
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
        *known = true;
        return 255U;
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16G16_UNORM:
        // The project only normalizes into NV12/P010 planes, so a 16-bit plane view is always
        // a P010 container: the 10-bit sample sits in the most significant bits.
        *known = true;
        return 65535U;
    default:
        *known = false;
        return 0U;
    }
}

// Bounded completion budget for the synchronous test/inspection wrapper. A one-texel copy
// finishes far inside this budget on WARP and hardware; the bound exists so termination is
// provable, not to tune latency.
constexpr std::chrono::milliseconds kSyncPollDeadline{250};
constexpr std::chrono::microseconds kSyncPollSleep{100};

} // namespace

bool D3d11SourcePixelProbeSlot::ensureDeviceResources(ID3D11Device* const device) {
    if (device == device_ && vertexShader_ != nullptr && pixelShader2D_ != nullptr &&
        pixelShader2DArray_ != nullptr && rasterizer_ != nullptr && isolatedState_ != nullptr) {
        return true;
    }
    if (device != device_) {
        target_.Reset();
        targetView_.Reset();
        staging_.Reset();
        constants_.Reset();
        device_ = device;
    }
    vertexShader_.Reset();
    pixelShader2D_.Reset();
    pixelShader2DArray_.Reset();
    rasterizer_.Reset();
    isolatedState_.Reset();

    // Slot-owned pipeline objects: no process-global probe pipeline exists, so slots on
    // different devices (or after a device rebuild) can never carry each other's shaders.
    HRESULT result = device->CreateVertexShader(shaders::kProbeVertexShader.data(),
                                                shaders::kProbeVertexShader.size(),
                                                nullptr,
                                                vertexShader_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(result)) {
        result = device->CreatePixelShader(shaders::kProbePixelShader2D.data(),
                                           shaders::kProbePixelShader2D.size(),
                                           nullptr,
                                           pixelShader2D_.ReleaseAndGetAddressOf());
    }
    if (SUCCEEDED(result)) {
        result = device->CreatePixelShader(shaders::kProbePixelShader2DArray.data(),
                                           shaders::kProbePixelShader2DArray.size(),
                                           nullptr,
                                           pixelShader2DArray_.ReleaseAndGetAddressOf());
    }
    if (SUCCEEDED(result)) {
        const D3D11_RASTERIZER_DESC rasterizerDescription{
            .FillMode = D3D11_FILL_SOLID,
            .CullMode = D3D11_CULL_NONE,
            .FrontCounterClockwise = FALSE,
            .DepthBias = 0,
            .DepthBiasClamp = 0.0F,
            .SlopeScaledDepthBias = 0.0F,
            .DepthClipEnable = FALSE,
            .ScissorEnable = FALSE,
            .MultisampleEnable = FALSE,
            .AntialiasedLineEnable = FALSE,
        };
        result = device->CreateRasterizerState(&rasterizerDescription,
                                               rasterizer_.ReleaseAndGetAddressOf());
    }
    if (SUCCEEDED(result)) {
        // Isolated device-context state (D3D11.1): everything the probe draw touches - every
        // render target slot, all viewports, all shader stages, blend/rasterizer/depth - is
        // swapped out and back atomically around the draw. Per the CreateDeviceContextState
        // contract, the feature-level list must be non-empty with a positive count, the
        // emulated interface is ID3D11Device's IID, and the chosen level is returned separately.
        ComPtr<ID3D11Device1> device1;
        result = device->QueryInterface(IID_PPV_ARGS(device1.GetAddressOf()));
        if (SUCCEEDED(result)) {
            const std::array<D3D_FEATURE_LEVEL, 1U> featureLevels{device->GetFeatureLevel()};
            D3D_FEATURE_LEVEL chosenLevel = D3D_FEATURE_LEVEL_9_1;
            // The d3d11_1.h signature puts the chosen-level out-param before the state.
            result = device1->CreateDeviceContextState(0U,
                                                       featureLevels.data(),
                                                       static_cast<UINT>(featureLevels.size()),
                                                       D3D11_SDK_VERSION,
                                                       __uuidof(ID3D11Device),
                                                       &chosenLevel,
                                                       isolatedState_.GetAddressOf());
        }
    }
    if (FAILED(result)) {
        vertexShader_.Reset();
        pixelShader2D_.Reset();
        pixelShader2DArray_.Reset();
        rasterizer_.Reset();
        isolatedState_.Reset();
        return false;
    }
    return true;
}

bool D3d11SourcePixelProbeSlot::issue(ID3D11Device* const device,
                                      ID3D11DeviceContext* const context,
                                      const D3d11ProbePlane& luma,
                                      const D3d11ProbePlane& chroma,
                                      const std::uint32_t lumaX,
                                      const std::uint32_t lumaY,
                                      const std::uint32_t chromaX,
                                      const std::uint32_t chromaY) {
    values_.reset();
    if (device == nullptr || context == nullptr || !planeIsUsable(luma) || !planeIsUsable(chroma) ||
        lumaX >= luma.visibleWidth || lumaY >= luma.visibleHeight ||
        chromaX >= chroma.visibleWidth || chromaY >= chroma.visibleHeight) {
        return false;
    }
    if (!ensureDeviceResources(device)) {
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC lumaDescription{};
    luma.view->GetDesc(&lumaDescription);
    D3D11_SHADER_RESOURCE_VIEW_DESC chromaDescription{};
    chroma.view->GetDesc(&chromaDescription);
    const bool lumaIsArray = lumaDescription.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    const bool chromaIsArray =
        chromaDescription.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    if (lumaIsArray != chromaIsArray) {
        return false;
    }
    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(context1.GetAddressOf())))) {
        return false;
    }
    if (!target_ || !targetView_ || !staging_) {
        D3D11_TEXTURE2D_DESC textureDescription{};
        textureDescription.Width = 1U;
        textureDescription.Height = 1U;
        textureDescription.MipLevels = 1U;
        textureDescription.ArraySize = 1U;
        textureDescription.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        textureDescription.SampleDesc.Count = 1U;
        textureDescription.Usage = D3D11_USAGE_DEFAULT;
        textureDescription.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(
                &textureDescription, nullptr, target_.ReleaseAndGetAddressOf()))) {
            target_.Reset();
            return false;
        }
        if (FAILED(device->CreateRenderTargetView(
                target_.Get(), nullptr, targetView_.ReleaseAndGetAddressOf()))) {
            target_.Reset();
            targetView_.Reset();
            return false;
        }
        textureDescription.Usage = D3D11_USAGE_STAGING;
        textureDescription.BindFlags = 0U;
        textureDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(
                &textureDescription, nullptr, staging_.ReleaseAndGetAddressOf()))) {
            target_.Reset();
            targetView_.Reset();
            staging_.Reset();
            return false;
        }
    }
    if (!constants_) {
        D3D11_BUFFER_DESC constantDescription{};
        constantDescription.ByteWidth = sizeof(ProbeTexelConstants);
        constantDescription.Usage = D3D11_USAGE_DYNAMIC;
        constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(
                &constantDescription, nullptr, constants_.ReleaseAndGetAddressOf()))) {
            constants_.Reset();
            return false;
        }
    }
    const ProbeTexelConstants texels{
        .lumaX = lumaX, .lumaY = lumaY, .chromaX = chromaX, .chromaY = chromaY};
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(constants_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped))) {
            return false;
        }
        std::memcpy(mapped.pData, &texels, sizeof(texels));
        context->Unmap(constants_.Get(), 0U);
    }

    // Atomically swap to the isolated state. The caller's entire pipeline state - every
    // render target slot, all viewports, every shader stage's bindings, blend, rasterizer,
    // depth - is captured in the returned previous-state object, and the draw below can never
    // leak into it or read from it.
    ComPtr<ID3DDeviceContextState> previousState;
    context1->SwapDeviceContextState(isolatedState_.Get(), previousState.ReleaseAndGetAddressOf());

    ID3D11ShaderResourceView* const views[2U] = {luma.view, chroma.view};
    ID3D11ShaderResourceView* const nullViews[2U] = {nullptr, nullptr};
    const D3D11_VIEWPORT viewport{0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F};
    context->RSSetState(rasterizer_.Get());
    context->RSSetViewports(1U, &viewport);
    context->OMSetRenderTargets(1U, targetView_.GetAddressOf(), nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader_.Get(), nullptr, 0U);
    context->PSSetShader(
        lumaIsArray ? pixelShader2DArray_.Get() : pixelShader2D_.Get(), nullptr, 0U);
    context->PSSetConstantBuffers(0U, 1U, constants_.GetAddressOf());
    context->PSSetShaderResources(0U, 2U, views);
    context->Draw(3U, 0U);
    context->CopyResource(staging_.Get(), target_.Get());
    // Unbind the frame-derived views and the target inside the isolated state BEFORE swapping
    // back: the state object must not retain references to the frame planes, or it would pin
    // them past their budget-managed retirement.
    context->PSSetShaderResources(0U, 2U, nullViews);
    context->OMSetRenderTargets(0U, nullptr, nullptr);
    // Atomic restore of the caller's state; the captured previous-state object is released
    // immediately so its references to the caller's resources do not outlive this call.
    context1->SwapDeviceContextState(previousState.Get(), nullptr);
    previousState.Reset();

    lumaPlane_ = luma;
    chromaPlane_ = chroma;
    return true;
}

std::optional<bool> D3d11SourcePixelProbeSlot::poll(ID3D11DeviceContext* const context) {
    if (values_.has_value()) {
        return true;
    }
    if (context == nullptr || !staging_ || !lumaPlane_.view) {
        return false;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT status =
        context->Map(staging_.Get(), 0U, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (status == DXGI_ERROR_WAS_STILL_DRAWING) {
        return std::nullopt;
    }
    if (FAILED(status) || mapped.pData == nullptr) {
        lumaPlane_ = {};
        chromaPlane_ = {};
        return false;
    }
    const auto* const words = static_cast<const std::uint32_t*>(mapped.pData);
    const std::array<std::uint32_t, 3U> raw{words[0U], words[1U], words[2U]};
    context->Unmap(staging_.Get(), 0U);
    values_ = decodedValues(raw, lumaPlane_, chromaPlane_);
    lumaPlane_ = {};
    chromaPlane_ = {};
    return true;
}

bool D3d11SourcePixelProbeSlot::completeBounded(ID3D11DeviceContext* const context) {
    if (values_.has_value()) {
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + kSyncPollDeadline;
    for (;;) {
        const std::optional<bool> polled = poll(context);
        if (polled.has_value()) {
            return *polled;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(kSyncPollSleep);
    }
}

void D3d11SourcePixelProbeSlot::clearResult() noexcept {
    values_.reset();
    lumaPlane_ = {};
    chromaPlane_ = {};
}

void D3d11SourcePixelProbeSlot::reset() noexcept {
    target_.Reset();
    targetView_.Reset();
    staging_.Reset();
    constants_.Reset();
    vertexShader_.Reset();
    pixelShader2D_.Reset();
    pixelShader2DArray_.Reset();
    rasterizer_.Reset();
    isolatedState_.Reset();
    device_ = nullptr;
    values_.reset();
    lumaPlane_ = {};
    chromaPlane_ = {};
}

std::optional<D3d11SourcePixel> probeSourcePixelSync(ID3D11Device* const device,
                                                     ID3D11DeviceContext* const context,
                                                     const D3d11ProbePlane& luma,
                                                     const D3d11ProbePlane& chroma,
                                                     const std::uint32_t lumaX,
                                                     const std::uint32_t lumaY,
                                                     const std::uint32_t chromaX,
                                                     const std::uint32_t chromaY) {
    if (device == nullptr || context == nullptr) {
        return std::nullopt;
    }
    D3d11SourcePixelProbeSlot slot;
    if (!slot.issue(device, context, luma, chroma, lumaX, lumaY, chromaX, chromaY)) {
        return std::nullopt;
    }
    context->Flush();
    if (!slot.completeBounded(context)) {
        return std::nullopt;
    }
    return slot.values();
}

std::optional<D3d11ProbePlane> lumaProbePlane(const D3d11GpuFrameBacking& backing) {
    ID3D11ShaderResourceView* const view = backing.yView();
    if (view == nullptr) {
        return std::nullopt;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    view->GetDesc(&description);
    bool known = false;
    D3d11ProbePlane plane{};
    plane.view = view;
    plane.visibleWidth = backing.yDimensions().width;
    plane.visibleHeight = backing.yDimensions().height;
    plane.containerMax = planeContainerMax(description.Format, &known);
    plane.codeShift = plane.containerMax == 65535U ? 6U : 0U;
    if (!known) {
        return std::nullopt;
    }
    return plane;
}

std::optional<D3d11ProbePlane> chromaProbePlane(const D3d11GpuFrameBacking& backing) {
    ID3D11ShaderResourceView* const view = backing.uvView();
    if (view == nullptr) {
        return std::nullopt;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    view->GetDesc(&description);
    bool known = false;
    D3d11ProbePlane plane{};
    plane.view = view;
    plane.visibleWidth = backing.uvDimensions().width;
    plane.visibleHeight = backing.uvDimensions().height;
    plane.containerMax = planeContainerMax(description.Format, &known);
    plane.codeShift = plane.containerMax == 65535U ? 6U : 0U;
    if (!known) {
        return std::nullopt;
    }
    return plane;
}

} // namespace dvs::platform
