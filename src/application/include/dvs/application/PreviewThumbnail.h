#pragma once

#include "dvs/application/Ports.h"
#include "dvs/application/RequestContext.h"
#include "dvs/domain/ComparisonSource.h"
#include "dvs/domain/Identifiers.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace dvs::application {

// Independent, playback-pipeline-free single-frame thumbnail for timeline hover of unplayed
// positions. Decodes one source frame to tightly packed RGBA8 and downscales for the popup.
struct PreviewThumbnailRequest final {
    RequestContext context;
    domain::ComparisonSource source;
    domain::FrameId frameId{0};
    std::uint32_t maxWidth = 176U;
    std::uint32_t maxHeight = 99U;

    [[nodiscard]] bool isValid() const noexcept {
        return frameId.isValid() && maxWidth > 0U && maxHeight > 0U &&
               source.descriptor.frameCount.value > 0 &&
               frameId.value() < source.descriptor.frameCount.value;
    }
};

struct PreviewThumbnailResult final {
    RequestContext context;
    domain::FrameId frameId{0};
    std::vector<std::uint8_t> rgba;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    bool available = false;

    [[nodiscard]] bool operator==(const PreviewThumbnailResult&) const = default;
};

class IPreviewThumbnailSink {
public:
    virtual ~IPreviewThumbnailSink() = default;
    virtual void onPreviewThumbnail(PreviewThumbnailResult result) = 0;
};

class IPreviewThumbnailService {
public:
    virtual ~IPreviewThumbnailService() = default;

    [[nodiscard]] virtual PortSubmitResult submit(const PreviewThumbnailRequest& request,
                                                  std::shared_ptr<IPreviewThumbnailSink> sink) = 0;
    virtual void cancel(const RequestContext& context) noexcept = 0;
};

} // namespace dvs::application
