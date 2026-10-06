#pragma once

struct AVIOContext;

namespace dvs::media::testing {

// Private deterministic seam for buffered AVIO close errors and cancellation at finalization.
using ClipExportCloseCallback = int (*)(AVIOContext**);

class ScopedClipExportCloseOverride final {
public:
    explicit ScopedClipExportCloseOverride(ClipExportCloseCallback close) noexcept;
    ~ScopedClipExportCloseOverride();

    ScopedClipExportCloseOverride(const ScopedClipExportCloseOverride&) = delete;
    ScopedClipExportCloseOverride& operator=(const ScopedClipExportCloseOverride&) = delete;

private:
    ClipExportCloseCallback previous_ = nullptr;
};

} // namespace dvs::media::testing
