#include "dvs/platform/D3d11DecodedFrameResource.h"

#include <limits>
#include <new>
#include <utility>

namespace dvs::platform {
namespace {

[[nodiscard]] std::optional<std::size_t>
decodedByteCount(const D3D11_TEXTURE2D_DESC& description,
                 const std::uint32_t visibleWidth,
                 const std::uint32_t visibleHeight,
                 const application::NormalizedFrameFormat format) noexcept {
    if (visibleWidth == 0U || visibleHeight == 0U || visibleWidth > description.Width ||
        visibleHeight > description.Height ||
        (format != application::NormalizedFrameFormat::Nv12_8 &&
         format != application::NormalizedFrameFormat::P010_10)) {
        return std::nullopt;
    }
    const std::size_t bytesPerSample =
        format == application::NormalizedFrameFormat::P010_10 ? 2U : 1U;
    const std::size_t width = visibleWidth;
    const std::size_t height = visibleHeight;
    const std::size_t chromaWidth = (width + 1U) / 2U;
    const std::size_t chromaHeight = (height + 1U) / 2U;
    if (width > (std::numeric_limits<std::size_t>::max)() / height ||
        (width * height) > (std::numeric_limits<std::size_t>::max)() / bytesPerSample ||
        chromaWidth > (std::numeric_limits<std::size_t>::max)() / chromaHeight ||
        (chromaWidth * chromaHeight) >
            (std::numeric_limits<std::size_t>::max)() / (2U * bytesPerSample)) {
        return std::nullopt;
    }
    const std::size_t luma = width * height * bytesPerSample;
    const std::size_t chroma = chromaWidth * chromaHeight * 2U * bytesPerSample;
    if (luma > (std::numeric_limits<std::size_t>::max)() - chroma) {
        return std::nullopt;
    }
    return luma + chroma;
}

} // namespace

std::optional<application::FrameHandle>
D3d11DecodedFrameResource::create(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
                                  const std::uint32_t arraySlice,
                                  const std::uint32_t visibleWidth,
                                  const std::uint32_t visibleHeight,
                                  const domain::DeviceGeneration deviceGeneration,
                                  const application::NormalizedFrameFormat format,
                                  const domain::ColorMetadata colorMetadata,
                                  const application::FramePresentation presentation,
                                  std::shared_ptr<const void> lifetimeAnchor,
                                  FrameBudget& frameBudget) noexcept {
    if (!texture || deviceGeneration.value() == 0U || !colorMetadata.isValid() ||
        !presentation.isValid() || !lifetimeAnchor) {
        return std::nullopt;
    }

    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    const DXGI_FORMAT expectedFormat =
        format == application::NormalizedFrameFormat::P010_10 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    const std::optional<std::size_t> bytes =
        decodedByteCount(description, visibleWidth, visibleHeight, format);
    if (!bytes || description.Format != expectedFormat || arraySlice >= description.ArraySize ||
        (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0U) {
        return std::nullopt;
    }

    std::optional<FrameBudget::Reservation> reservation = frameBudget.tryReserve(*bytes);
    if (!reservation) {
        return std::nullopt;
    }
    auto resource = std::shared_ptr<D3d11DecodedFrameResource>(
        new (std::nothrow) D3d11DecodedFrameResource(std::move(texture),
                                                     arraySlice,
                                                     deviceGeneration,
                                                     format,
                                                     colorMetadata,
                                                     std::move(lifetimeAnchor),
                                                     std::move(*reservation)));
    if (!resource) {
        return std::nullopt;
    }
    return application::FrameHandle::create(
        std::move(resource),
        application::FrameGeometry{
            .width = visibleWidth,
            .height = visibleHeight,
            .textureRegion =
                application::TextureRegion{
                    .right =
                        static_cast<float>(visibleWidth) / static_cast<float>(description.Width),
                    .bottom =
                        static_cast<float>(visibleHeight) / static_cast<float>(description.Height),
                },
            .presentation = presentation,
        },
        *bytes);
}

D3d11DecodedFrameResource::D3d11DecodedFrameResource(
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
    const std::uint32_t arraySlice,
    const domain::DeviceGeneration deviceGeneration,
    const application::NormalizedFrameFormat format,
    const domain::ColorMetadata colorMetadata,
    std::shared_ptr<const void> lifetimeAnchor,
    FrameBudget::Reservation reservation) noexcept
    : texture_(std::move(texture)), arraySlice_(arraySlice), deviceGeneration_(deviceGeneration),
      format_(format), colorMetadata_(colorMetadata), lifetimeAnchor_(std::move(lifetimeAnchor)),
      reservation_(std::move(reservation)) {}

ID3D11Texture2D* D3d11DecodedFrameResource::texture() const noexcept {
    return texture_.Get();
}

std::uint32_t D3d11DecodedFrameResource::arraySlice() const noexcept {
    return arraySlice_;
}

domain::DeviceGeneration D3d11DecodedFrameResource::deviceGeneration() const noexcept {
    return deviceGeneration_;
}

application::NormalizedFrameFormat D3d11DecodedFrameResource::format() const noexcept {
    return format_;
}

const domain::ColorMetadata& D3d11DecodedFrameResource::colorMetadata() const noexcept {
    return colorMetadata_;
}

std::size_t D3d11DecodedFrameResource::byteCount() const noexcept {
    return reservation_.bytes();
}

} // namespace dvs::platform
