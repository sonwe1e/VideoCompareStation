#pragma once

#include "dvs/application/FrameHandle.h"
#include "dvs/application/NormalizedFrameFormat.h"
#include "dvs/domain/Identifiers.h"
#include "dvs/domain/MediaDescriptor.h"
#include "dvs/platform/FrameBudget.h"

#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <optional>
#include <wrl/client.h>

namespace dvs::platform {

// Retains one decoder-owned D3D11VA surface slice without exposing graphics types to the
// application layer. The opaque lifetime anchor keeps FFmpeg's AVFrame reference alive until the
// final GPU publication retires, preventing the decoder from reusing the slice while rendering.
class D3d11DecodedFrameResource final : public application::IFrameResource {
public:
    [[nodiscard]] static std::optional<application::FrameHandle>
    create(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
           std::uint32_t arraySlice,
           std::uint32_t visibleWidth,
           std::uint32_t visibleHeight,
           domain::DeviceGeneration deviceGeneration,
           application::NormalizedFrameFormat format,
           domain::ColorMetadata colorMetadata,
           application::FramePresentation presentation,
           std::shared_ptr<const void> lifetimeAnchor,
           FrameBudget& frameBudget) noexcept;

    [[nodiscard]] ID3D11Texture2D* texture() const noexcept;
    [[nodiscard]] std::uint32_t arraySlice() const noexcept;
    [[nodiscard]] domain::DeviceGeneration deviceGeneration() const noexcept;
    [[nodiscard]] application::NormalizedFrameFormat format() const noexcept;
    [[nodiscard]] const domain::ColorMetadata& colorMetadata() const noexcept;
    [[nodiscard]] std::size_t byteCount() const noexcept;

private:
    D3d11DecodedFrameResource(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
                              std::uint32_t arraySlice,
                              domain::DeviceGeneration deviceGeneration,
                              application::NormalizedFrameFormat format,
                              domain::ColorMetadata colorMetadata,
                              std::shared_ptr<const void> lifetimeAnchor,
                              FrameBudget::Reservation reservation) noexcept;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    std::uint32_t arraySlice_ = 0U;
    domain::DeviceGeneration deviceGeneration_{0U};
    application::NormalizedFrameFormat format_ = application::NormalizedFrameFormat::Nv12_8;
    domain::ColorMetadata colorMetadata_;
    std::shared_ptr<const void> lifetimeAnchor_;
    FrameBudget::Reservation reservation_;
};

} // namespace dvs::platform
