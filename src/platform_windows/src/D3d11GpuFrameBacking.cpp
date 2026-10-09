#include "D3d11GpuFrameBacking.h"

#include <array>
#include <utility>

namespace dvs::platform {
namespace {

using Microsoft::WRL::ComPtr;

// One texel copied into a fresh 1x1 staging texture. Returns the (up to) two channel values
// already right-shifted to their code-value range; single-channel formats repeat the value.
[[nodiscard]] std::optional<std::array<std::uint32_t, 2U>>
readPlaneTexel(ID3D11Device* const device,
               ID3D11DeviceContext* const context,
               ID3D11Texture2D* const source,
               const std::uint32_t x,
               const std::uint32_t y) {
    if (device == nullptr || context == nullptr || source == nullptr) {
        return std::nullopt;
    }
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    if (x >= description.Width || y >= description.Height) {
        return std::nullopt;
    }
    D3D11_TEXTURE2D_DESC stagingDescription = description;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0U;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0U;
    stagingDescription.Width = 1U;
    stagingDescription.Height = 1U;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDescription, nullptr, staging.GetAddressOf()))) {
        return std::nullopt;
    }
    const D3D11_BOX region{x, y, 0U, x + 1U, y + 1U, 1U};
    context->CopySubresourceRegion(staging.Get(), 0U, 0U, 0U, 0U, source, 0U, &region);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped)) ||
        mapped.pData == nullptr) {
        return std::nullopt;
    }
    std::array<std::uint32_t, 2U> values{};
    switch (description.Format) {
    case DXGI_FORMAT_R8_UNORM: {
        const auto value = *static_cast<const std::uint8_t*>(mapped.pData);
        values = {value, value};
        break;
    }
    case DXGI_FORMAT_R8G8_UNORM: {
        const auto* const channels = static_cast<const std::uint8_t*>(mapped.pData);
        values = {channels[0], channels[1]};
        break;
    }
    case DXGI_FORMAT_R16_UNORM: {
        // P010 luma: the 10-bit sample occupies the most significant bits of the 16-bit word.
        const auto raw = *static_cast<const std::uint16_t*>(mapped.pData);
        const auto code = static_cast<std::uint32_t>(raw >> 6);
        values = {code, code};
        break;
    }
    case DXGI_FORMAT_R16G16_UNORM: {
        // P010 chroma pair, same left-justified layout per word.
        const auto* const channels = static_cast<const std::uint16_t*>(mapped.pData);
        values = {static_cast<std::uint32_t>(channels[0] >> 6),
                  static_cast<std::uint32_t>(channels[1] >> 6)};
        break;
    }
    default:
        context->Unmap(staging.Get(), 0U);
        return std::nullopt;
    }
    context->Unmap(staging.Get(), 0U);
    return values;
}

} // namespace

D3d11GpuFrameBacking::D3d11GpuFrameBacking(Microsoft::WRL::ComPtr<ID3D11Texture2D> yTexture,
                                           Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> yView,
                                           const D3d11PlaneDimensions yDimensions,
                                           Microsoft::WRL::ComPtr<ID3D11Texture2D> uvTexture,
                                           Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uvView,
                                           const D3d11PlaneDimensions uvDimensions,
                                           std::shared_ptr<const void> lifetimeAnchor) noexcept
    : yTexture_(std::move(yTexture)), yView_(std::move(yView)), yDimensions_(yDimensions),
      uvTexture_(std::move(uvTexture)), uvView_(std::move(uvView)), uvDimensions_(uvDimensions),
      lifetimeAnchor_(std::move(lifetimeAnchor)) {}

ID3D11Texture2D* D3d11GpuFrameBacking::yTexture() const noexcept {
    return yTexture_.Get();
}

ID3D11ShaderResourceView* D3d11GpuFrameBacking::yView() const noexcept {
    return yView_.Get();
}

const D3d11PlaneDimensions& D3d11GpuFrameBacking::yDimensions() const noexcept {
    return yDimensions_;
}

ID3D11Texture2D* D3d11GpuFrameBacking::uvTexture() const noexcept {
    return uvTexture_.Get();
}

ID3D11ShaderResourceView* D3d11GpuFrameBacking::uvView() const noexcept {
    return uvView_.Get();
}

const D3d11PlaneDimensions& D3d11GpuFrameBacking::uvDimensions() const noexcept {
    return uvDimensions_;
}

std::optional<D3d11SourcePixel> D3d11GpuFrameBacking::probePixel(ID3D11Device* const device,
                                                                 ID3D11DeviceContext* const context,
                                                                 const std::uint32_t x,
                                                                 const std::uint32_t y) const {
    if (x >= yDimensions_.width || y >= yDimensions_.height) {
        return std::nullopt;
    }
    D3D11_TEXTURE2D_DESC lumaDescription{};
    yTexture_->GetDesc(&lumaDescription);
    const std::uint32_t bitDepth =
        lumaDescription.Format == DXGI_FORMAT_R8_UNORM
            ? 8U
            : (lumaDescription.Format == DXGI_FORMAT_R16_UNORM ? 10U : 0U);
    if (bitDepth == 0U) {
        return std::nullopt;
    }
    const std::optional<std::array<std::uint32_t, 2U>> luma =
        readPlaneTexel(device, context, yTexture_.Get(), x, y);
    if (!luma.has_value()) {
        return std::nullopt;
    }
    const std::optional<std::array<std::uint32_t, 2U>> chroma =
        readPlaneTexel(device, context, uvTexture_.Get(), x >> 1U, y >> 1U);
    if (!chroma.has_value()) {
        return std::nullopt;
    }
    return D3d11SourcePixel{
        .y = (*luma)[0],
        .cb = (*chroma)[0],
        .cr = (*chroma)[1],
        .bitDepth = bitDepth,
    };
}

} // namespace dvs::platform
