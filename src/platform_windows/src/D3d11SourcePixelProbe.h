#pragma once

#include "D3d11GpuFrameBacking.h"

#include <cstdint>
#include <d3d11.h>
#include <d3d11_1.h>
#include <optional>
#include <wrl/client.h>

namespace dvs::platform {

// One plane's probe source: the shader-resource view the display path uses, the visible
// (non-padding) sample dimensions, and how the view's UNORM floats map back to integer codes.
// containerMax is the view's sample range (255 for 8-bit planes, 65535 for 16-bit containers);
// codeShift is 0, or 6 when the 16-bit container holds a P010 sample left-justified by 6.
struct D3d11ProbePlane final {
    ID3D11ShaderResourceView* view = nullptr;
    std::uint32_t visibleWidth = 0U;
    std::uint32_t visibleHeight = 0U;
    std::uint32_t containerMax = 0U;
    std::uint32_t codeShift = 0U;
};

// E1: the pending-probe identity key shared by the renderer's staging slots. It bundles the
// set identity (pointer identity of the presented GpuFrameSet - never dereferenced), the
// full-width request id, and the exact texel. The renderer asks matches() before re-using a
// slot and start() when it re-issues; unit tests exercise these transitions without any GPU
// wait, so an accidental narrowing of the request id (or any field) is detected
// deterministically instead of racing a WARP completion.
struct PendingProbeKey final {
    const void* set = nullptr;
    std::uint64_t requestId = 0U;
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;

    [[nodiscard]] constexpr bool matches(const void* setIdentity,
                                         std::uint64_t id,
                                         std::uint32_t texelX,
                                         std::uint32_t texelY) const noexcept {
        return set == setIdentity && requestId == id && x == texelX && y == texelY;
    }

    void start(const void* setIdentity,
               std::uint64_t id,
               std::uint32_t texelX,
               std::uint32_t texelY) noexcept {
        set = setIdentity;
        requestId = id;
        x = texelX;
        y = texelY;
    }

    void clear() noexcept {
        set = nullptr;
        requestId = 0U;
        x = 0U;
        y = 0U;
    }
};

// One probe slot: a 1x1 UINT render target plus matching staging, reused across cursor
// positions. The draw and its copy are issued once per request; every later pass only polls
// the staging with DO_NOT_WAIT, so the GPU is never stalled. The slot owns its shaders,
// rasterizer and isolated device-context state - there is deliberately no process-global
// probe pipeline, so slots on different devices (or rebuilt devices) can never see each
// other's shader objects. Slots hold no batch semantics - identity pinning and retries stay
// with the caller.
class D3d11SourcePixelProbeSlot final {
public:
    D3d11SourcePixelProbeSlot() = default;
    ~D3d11SourcePixelProbeSlot() = default;
    D3d11SourcePixelProbeSlot(const D3d11SourcePixelProbeSlot&) = delete;
    D3d11SourcePixelProbeSlot& operator=(const D3d11SourcePixelProbeSlot&) = delete;

    // Issues the 1x1 probe draw and its staging copy on the caller's lease. The draw runs
    // against an isolated ID3DDeviceContextState that is swapped out and back atomically
    // (D3D11.1): the context-state swap is the API-level guarantee that the caller's entire
    // bound state survives the probe untouched, and the previous state's captured resource
    // references are released before the call returns (no frame pinning beyond the call).
    // Behaviourally verified so far: shader resources t0/t1, the render target, multiple
    // viewports and the blend state (see GpuTransferActorTests); wider stages rely on the
    // swap guarantee. Returns false only on device-side failure; the coordinates must
    // already be validated against the visible plane dimensions.
    [[nodiscard]] bool issue(ID3D11Device* device,
                             ID3D11DeviceContext* context,
                             const D3d11ProbePlane& luma,
                             const D3d11ProbePlane& chroma,
                             std::uint32_t lumaX,
                             std::uint32_t lumaY,
                             std::uint32_t chromaX,
                             std::uint32_t chromaY);

    // nullopt: the GPU copy is still in flight (retry on a later pass); false: the readback
    // failed and the slot must be re-issued; true: `values()` now holds the recovered codes.
    // Production path: DO_NOT_WAIT only.
    [[nodiscard]] std::optional<bool> poll(ID3D11DeviceContext* context);

    // Test/inspection completion for the synchronous wrapper: polls with DO_NOT_WAIT on a
    // bounded deadline (never an unbounded blocking Map) and returns whether the values are
    // ready. Never called on the GUI/render production chain.
    [[nodiscard]] bool completeBounded(ID3D11DeviceContext* context);

    // Drops the staged values and plane descriptors while keeping the GPU resources for reuse.
    void clearResult() noexcept;

    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return values_.has_value();
    }
    [[nodiscard]] const D3d11SourcePixel& values() const noexcept {
        return *values_;
    }

private:
    [[nodiscard]] bool ensureDeviceResources(ID3D11Device* device);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> target_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> targetView_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader2D_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader2DArray_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> isolatedState_;
    ID3D11Device* device_ = nullptr;
    D3d11ProbePlane lumaPlane_{};
    D3d11ProbePlane chromaPlane_{};
    std::optional<D3d11SourcePixel> values_;
};

// Synchronous one-shot probe for the D3d11GpuFrameBacking test/inspection API only - it is
// not on the GUI/render production chain. Issues the same 1x1 draw, then completes the
// one-texel readback by bounded DO_NOT_WAIT polling, never an unbounded blocking Map.
[[nodiscard]] std::optional<D3d11SourcePixel> probeSourcePixelSync(ID3D11Device* device,
                                                                   ID3D11DeviceContext* context,
                                                                   const D3d11ProbePlane& luma,
                                                                   const D3d11ProbePlane& chroma,
                                                                   std::uint32_t lumaX,
                                                                   std::uint32_t lumaY,
                                                                   std::uint32_t chromaX,
                                                                   std::uint32_t chromaY);

// Describes a backing's luma/chroma plane through its shader view. Returns nullopt when the
// view format is not one of the normalized plane formats (8-bit R8/R8G8, P010-in-16-bit
// R16/R16G16); such planes cannot be probed without inventing semantics.
[[nodiscard]] std::optional<D3d11ProbePlane> lumaProbePlane(const D3d11GpuFrameBacking& backing);
[[nodiscard]] std::optional<D3d11ProbePlane> chromaProbePlane(const D3d11GpuFrameBacking& backing);

} // namespace dvs::platform
