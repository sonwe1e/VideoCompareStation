#pragma once

#include "dvs/application/SampleExportWriter.h"

#include <atomic>
#include <filesystem>

namespace dvs::media {

// Executes sample-triplet plans through the exact decode path: every member is addressed by its
// zero-based display-order ordinal via the same SoftwareDecoder the playback pipeline uses
// (keyframe pre-roll is an implementation detail; timestamp approximation is not), converted
// from the normalized NV12/P010 display format to RGB24 with swscale, optionally cropped in
// source pixel coordinates, and encoded as lossless PNG.
//
// PNG encoder selection: the FFmpeg avcodec png encoder is the primary path (probed at runtime
// by opening a small context); when the build carries no usable avcodec png encoder the writer
// falls back to the Windows Imaging Component, which adds no new dependency to the product.
// Both routes are lossless; the selected encoder is recorded in the manifest. Verified against
// this repository's locked vcpkg FFmpeg: it is configured --disable-zlib, so avcodec carries no
// png encoder at all there and the WIC route is the one these builds exercise.
//
// The sample directory is published transactionally: members and manifest are written into a
// `.staging-<requestId>/` directory beside the final location, read back and validated (a
// member must exist, be non-empty, and carry the PNG signature; the manifest must parse back
// with the expected identity), and only then renamed into place. Cancellation, failure, or a
// validation mismatch removes only the staging tree this invocation created; a previous sample
// directory is swapped out only after the replacement validated.
class SampleExportWriter final : public application::ISampleExporter {
public:
    SampleExportWriter() = default;
    ~SampleExportWriter() override = default;

    SampleExportWriter(const SampleExportWriter&) = delete;
    SampleExportWriter& operator=(const SampleExportWriter&) = delete;
    SampleExportWriter(SampleExportWriter&&) = delete;
    SampleExportWriter& operator=(SampleExportWriter&&) = delete;

    [[nodiscard]] application::SampleExportReport
    perform(const application::SampleExportJob& job,
            const std::atomic_bool& cancelRequested) override;
};

} // namespace dvs::media
