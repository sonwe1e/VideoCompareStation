#pragma once

#include <filesystem>

namespace dvs::media::testing {

// Private deterministic seams for component tests. The staged-member hook is invoked with each
// staging file path after that file is closed and before read-back validation, so a test can
// cancel mid-flight or sabotage the staged bytes; the WIC override forces the Windows Imaging
// Component fallback even when the avcodec png encoder is available.

using SampleExportStagedMemberCallback = void (*)(const std::filesystem::path& memberPath);

class ScopedSampleExportStagedMemberHook final {
public:
    explicit ScopedSampleExportStagedMemberHook(SampleExportStagedMemberCallback callback) noexcept;
    ~ScopedSampleExportStagedMemberHook();

    ScopedSampleExportStagedMemberHook(const ScopedSampleExportStagedMemberHook&) = delete;
    ScopedSampleExportStagedMemberHook&
    operator=(const ScopedSampleExportStagedMemberHook&) = delete;

private:
    SampleExportStagedMemberCallback previous_ = nullptr;
};

class ScopedSampleExportWicFallback final {
public:
    ScopedSampleExportWicFallback() noexcept;
    ~ScopedSampleExportWicFallback();

    ScopedSampleExportWicFallback(const ScopedSampleExportWicFallback&) = delete;
    ScopedSampleExportWicFallback& operator=(const ScopedSampleExportWicFallback&) = delete;
};

} // namespace dvs::media::testing
