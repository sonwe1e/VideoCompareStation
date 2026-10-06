#pragma once

#include "dvs/application/PlaybackCommands.h"
#include "dvs/domain/FrameTimeline.h"
#include "dvs/domain/Identifiers.h"
#include "dvs/domain/MediaError.h"
#include "dvs/domain/Result.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dvs::application {

// Identity of one user-triggered clip export. The GUI mints the value, the worker echoes it
// back, and the owner drops any completion whose request id or session epoch is stale.
using ClipExportRequestId = std::uint64_t;
inline constexpr ClipExportRequestId kInvalidClipExportRequestId = 0U;

// A stream copy can only begin at a keyframe, so the exported clip is described by the requested
// range plus the container-aligned span the writer will actually copy. Both halves are kept so
// the UI can be honest: "you asked for frames 42-90, the clip starts at frame 40".
struct ClipExportPlan final {
    // Closed canonical-frame interval the user asked for. Inclusive on both ends, exactly like
    // kernel range playback, so "export what I just played" cannot drift by one frame.
    PlaybackRange requestedRange{};
    // Container-aligned copy span on the frame-zero-normalized timeline. Start is a keyframe
    // presentation time; the end is exclusive and empty when the clip runs to the end of the stream
    // (an unknown/unnameable instant).
    std::int64_t startMicroseconds = 0;
    std::optional<std::int64_t> endMicroseconds;
    // Negative when the copy start moved earlier than the requested in-point (pre-roll over the
    // preceding frames of the GOP), positive when the requested in-point precedes the first
    // keyframe. Zero means the request already landed on a keyframe.
    std::int64_t startShiftMicroseconds = 0;
    // Canonical frame the copy actually starts on, when the demuxer's keyframe time maps back
    // onto the canonical timeline. Empty for a variable-rate timeline that cannot name it.
    std::optional<domain::FrameId> firstExportedFrame;
    std::int64_t requestedFrameCount = 0;

    [[nodiscard]] bool operator==(const ClipExportPlan&) const noexcept = default;
};

// Validates a requested range against the canonical timeline and turns it into a media-time span.
// The exclusive end is the start time of the frame after the out point; when the out point is the
// last frame the end is the end of the stream. The result still needs alignClipExportStart().
[[nodiscard]] domain::Result<ClipExportPlan> planClipExport(
    const domain::CanonicalTimeline& timeline, std::int64_t frameCount, PlaybackRange requested);

// Moves the plan's start onto the latest keyframe at or before the requested start. `keyframeTimes`
// must be ascending presentation times in microseconds relative to the first source frame. A
// request with no usable keyframe is rejected rather than silently exporting nothing.
[[nodiscard]] domain::Result<ClipExportPlan>
alignClipExportStart(const domain::CanonicalTimeline& timeline,
                     ClipExportPlan plan,
                     std::span<const std::int64_t> keyframeTimes);

enum class ClipExportOutcome {
    kCompleted,
    kCanceled,
    kFailed,
};

// Fraction of the copied span already written, 0.0-1.0. Called from the worker thread and must
// never block; the owner is responsible for hopping back to its own thread.
using ClipExportProgressCallback = std::function<void(double)>;

struct ClipExportJob final {
    ClipExportRequestId requestId = kInvalidClipExportRequestId;
    domain::SessionId sessionId{0};
    domain::SessionEpoch sessionEpoch{0};
    std::filesystem::path sourcePath;
    std::filesystem::path outputPath;
    ClipExportPlan plan;
    ClipExportProgressCallback progress;
};

struct ClipExportReport final {
    ClipExportRequestId requestId = kInvalidClipExportRequestId;
    ClipExportOutcome outcome = ClipExportOutcome::kFailed;
    std::int64_t packetsWritten = 0;
    // Presentation time of the first copied packet relative to the first source frame. When the
    // start was pre-rolled onto an earlier keyframe this is smaller than the requested in-point.
    // Output timestamps are rebased separately on the first copied decode timestamp.
    std::int64_t firstPresentationMicroseconds = 0;
    // Diagnostic only; never shown as a user-visible label.
    std::string technicalDetail;
};

// Bounded, user-triggered demux/remux of one frame range into a new file. Export is not part of
// the playback pipeline, so it stays out of the coordinator's state machine and publication order:
// both calls below block by design and belong on a worker thread that the caller owns.
class IClipExporter {
public:
    IClipExporter() = default;
    IClipExporter(const IClipExporter&) = delete;
    IClipExporter& operator=(const IClipExporter&) = delete;
    IClipExporter(IClipExporter&&) = delete;
    IClipExporter& operator=(IClipExporter&&) = delete;
    virtual ~IClipExporter() = default;

    // Demux-only query of the source's video keyframes: ascending presentation times in
    // microseconds relative to the first source frame, empty when no keyframe information is
    // available. It decodes nothing, but reads the file on the same worker thread as perform() and
    // honours the same cooperative cancellation. An empty result also means "stopped early";
    // the caller knows which of the two it asked for.
    [[nodiscard]] virtual std::vector<std::int64_t>
    keyframeTimes(const std::filesystem::path& sourcePath,
                  const std::atomic_bool& cancelRequested) = 0;

    // Runs on a worker thread. Honours cooperative cancellation through `cancelRequested` and
    // reports failure in the returned report instead of throwing. Implementations must write the
    // output transactionally: a canceled or failed export never leaves a partial file at
    // job.outputPath.
    [[nodiscard]] virtual ClipExportReport perform(const ClipExportJob& job,
                                                   const std::atomic_bool& cancelRequested) = 0;
};

} // namespace dvs::application
