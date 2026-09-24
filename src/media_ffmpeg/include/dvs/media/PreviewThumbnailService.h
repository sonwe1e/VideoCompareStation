#pragma once

#include "dvs/application/PreviewThumbnail.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace dvs::media {

// Single-worker thumbnail decoder for timeline hover. Uses the pair-metrics decode session
// class (no FrameBudget / render cache) and keeps one decoder open across jobs while the
// addressed source stays identical. A new submit supersedes the queued job and cooperatively
// cancels the active one so rapid scrubbing never piles up seeks.
class PreviewThumbnailService final : public application::IPreviewThumbnailService {
public:
    explicit PreviewThumbnailService(std::size_t queueCapacity = 1U);
    ~PreviewThumbnailService() override;

    PreviewThumbnailService(const PreviewThumbnailService&) = delete;
    PreviewThumbnailService& operator=(const PreviewThumbnailService&) = delete;
    PreviewThumbnailService(PreviewThumbnailService&&) = delete;
    PreviewThumbnailService& operator=(PreviewThumbnailService&&) = delete;

    [[nodiscard]] application::PortSubmitResult
    submit(const application::PreviewThumbnailRequest& request,
           std::shared_ptr<application::IPreviewThumbnailSink> sink) override;
    void cancel(const application::RequestContext& context) noexcept override;

    [[nodiscard]] std::uint64_t decodedFrameCountForTesting() const noexcept;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::media
