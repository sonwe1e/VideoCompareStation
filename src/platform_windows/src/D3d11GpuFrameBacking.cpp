#include "D3d11GpuFrameBacking.h"

#include "D3d11SourcePixelProbe.h"

#include <algorithm>
#include <utility>

namespace dvs::platform {

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

// The texture members are retained for lifetime ownership (the CPU-upload path owns its
// per-plane textures here); the probe itself reads through the shader views, which encode the
// plane format and - for zero-copy D3D11VA frames - the non-zero array slice.
std::optional<D3d11SourcePixel> D3d11GpuFrameBacking::probePixel(ID3D11Device* const device,
                                                                 ID3D11DeviceContext* const context,
                                                                 const std::uint32_t x,
                                                                 const std::uint32_t y) const {
    if (x >= yDimensions_.width || y >= yDimensions_.height) {
        return std::nullopt;
    }
    const std::optional<D3d11ProbePlane> luma = lumaProbePlane(*this);
    const std::optional<D3d11ProbePlane> chroma = chromaProbePlane(*this);
    if (!luma.has_value() || !chroma.has_value()) {
        return std::nullopt;
    }
    const std::uint32_t chromaX = (std::min)(x >> 1U, chroma->visibleWidth - 1U);
    const std::uint32_t chromaY = (std::min)(y >> 1U, chroma->visibleHeight - 1U);
    return probeSourcePixelSync(device, context, *luma, *chroma, x, y, chromaX, chromaY);
}

} // namespace dvs::platform
