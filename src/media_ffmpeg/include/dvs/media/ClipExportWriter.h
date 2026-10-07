#pragma once

#include "dvs/application/ClipExport.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace dvs::media {

// Stream-copy clip writer. Demuxes the plan's span out of the source container and remuxes the
// video packets into a fresh file without re-encoding, so the exported pixels are bit-identical
// to the source and no encoder build option is involved. The compressed packets are the only
// thing written: audio and subtitle streams of the source are deliberately left out, matching the
// product's silent-video scope.
//
// Which packets belong to the span is decided in presentation time, so a reordered stream keeps
// exactly the frames the range asked for, and a packet that presents past the out point is still
// carried along when a written frame decodes from it.
//
// The output is built beside its target and moved into place only after the trailer is written,
// so a canceled or failed export never leaves a partial clip where the user pointed the dialog.
class ClipExportWriter final : public application::IClipExporter {
public:
    ClipExportWriter() = default;
    ~ClipExportWriter() override = default;

    ClipExportWriter(const ClipExportWriter&) = delete;
    ClipExportWriter& operator=(const ClipExportWriter&) = delete;
    ClipExportWriter(ClipExportWriter&&) = delete;
    ClipExportWriter& operator=(ClipExportWriter&&) = delete;

    // Presentation times of the sync samples, in frame-zero-normalized microseconds, ascending.
    // A container index only stores seek timestamps (decode-order values for a reordered stream),
    // so each sync entry is resolved to the presentation time of the packet a backward seek to it
    // lands on; a container
    // without an index is walked packet by packet instead. Both paths only demux and never decode,
    // and the times are the ones the planner aligns the in point against.
    [[nodiscard]] std::vector<std::int64_t>
    keyframeTimes(const std::filesystem::path& sourcePath,
                  const std::atomic_bool& cancelRequested) override;

    [[nodiscard]] application::ClipExportReport
    perform(const application::ClipExportJob& job,
            const std::atomic_bool& cancelRequested) override;
};

} // namespace dvs::media
