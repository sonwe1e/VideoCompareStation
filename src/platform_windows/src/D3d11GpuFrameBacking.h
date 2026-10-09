#pragma once

#include "GpuFrameResource.h"

#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <optional>
#include <wrl/client.h>

namespace dvs::platform {

struct D3d11PlaneDimensions final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;

    [[nodiscard]] constexpr bool operator==(const D3d11PlaneDimensions&) const = default;
};

// Raw plane code values of one source pixel, ready for display: 8-bit samples read as 0..255,
// 10-bit P010 samples right-shifted out of their 16-bit containers as 0..1023. The chroma pair
// is the sample co-sited with the luma pixel's 2x2 block corner (x >> 1, y >> 1); which of the
// neighbouring chroma samples the shader interpolates between on screen is a presentation
// choice - the readout states the raw stored one. bitDepth 0 means the format is unknown.
struct D3d11SourcePixel final {
    std::uint32_t y = 0U;
    std::uint32_t cb = 0U;
    std::uint32_t cr = 0U;
    std::uint32_t bitDepth = 0U;
};

// Immutable shader-visible plane resources. Upload staging resources stay actor-local and are
// released after the event fences complete; only the default textures and SRVs reach rendering.
class D3d11GpuFrameBacking final : public IGpuFrameBacking {
public:
    D3d11GpuFrameBacking(Microsoft::WRL::ComPtr<ID3D11Texture2D> yTexture,
                         Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> yView,
                         D3d11PlaneDimensions yDimensions,
                         Microsoft::WRL::ComPtr<ID3D11Texture2D> uvTexture,
                         Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uvView,
                         D3d11PlaneDimensions uvDimensions,
                         std::shared_ptr<const void> lifetimeAnchor = {}) noexcept;
    ~D3d11GpuFrameBacking() override = default;

    [[nodiscard]] ID3D11Texture2D* yTexture() const noexcept;
    [[nodiscard]] ID3D11ShaderResourceView* yView() const noexcept;
    [[nodiscard]] const D3d11PlaneDimensions& yDimensions() const noexcept;
    [[nodiscard]] ID3D11Texture2D* uvTexture() const noexcept;
    [[nodiscard]] ID3D11ShaderResourceView* uvView() const noexcept;
    [[nodiscard]] const D3d11PlaneDimensions& uvDimensions() const noexcept;

    // Reads one source pixel's plane code values through a 1x1 staging copy per plane - never
    // a full-frame readback. Runs on the graphics thread while a device lease is held; the
    // Map after the copy is a bounded sync of that one-texel copy. Out-of-range coordinates
    // and non-NV12/P010 plane formats return nullopt instead of invented values.
    [[nodiscard]] std::optional<D3d11SourcePixel> probePixel(ID3D11Device* device,
                                                             ID3D11DeviceContext* context,
                                                             std::uint32_t x,
                                                             std::uint32_t y) const;

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> yTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> yView_;
    D3d11PlaneDimensions yDimensions_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> uvTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uvView_;
    D3d11PlaneDimensions uvDimensions_;
    std::shared_ptr<const void> lifetimeAnchor_;
};

} // namespace dvs::platform
