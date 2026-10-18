#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dvs::application {

// T7 read-only issue-record schema. Version 1 only; unknown versions are rejected by the
// persistence adapter with an explicit explanation and never rewritten in place.
inline constexpr std::int64_t kIssueRecordSchemaVersion = 1;

enum class IssueRecordKind : std::uint8_t {
    Video,
    ImagePair,
};

// How a recorded source path compares to the file currently on disk.
enum class IssueSourceMatch : std::uint8_t {
    Matched,
    Missing,
    Modified,
    // The record never captured a complete identity (size/mtime), so restore cannot claim
    // the on-disk file is the same revision.
    IncompleteIdentity,
};

enum class IssueRestoreDecision : std::uint8_t {
    // Identity checks passed; observation context can be restored.
    Ready,
    // At least one source moved or changed; the user must relocate before restore.
    RelocationRequired,
    // Sources match, but the record carries no valid presentation (no committed frame/pair).
    NoValidPresentation,
    // The record itself cannot be used (unsupported kind, empty sources, etc.).
    Blocked,
};

struct IssueSourceRef final {
    std::string path;
    std::int64_t byteSize = -1;
    std::int64_t modifiedUtcMilliseconds = -1;
    // Optional adapter fingerprint; empty means "not captured".
    std::string fingerprintSha256;
    // Video presentation payload. hasPresentation is false for image sides and for video
    // sessions that never committed a frame, and for a source that was Missing at capture.
    bool hasPresentation = false;
    // The side's actual displayed frame on the canonical timeline, taken from the committed
    // snapshot's PresentedSourceState::sourceFrameId. -1 when no frame was presented for this
    // side (never the canonical frame written to every source).
    std::int64_t displayIndex = -1;
    // Source presentation timestamp in microseconds, valid only when hasPresentation is true.
    // PTS seconds = presentationTimestampTicks * timeBaseNumerator / timeBaseDenominator, with
    // the stored convention timeBase 1/1'000'000 (i.e. ticks are microseconds).
    std::int64_t presentationTimestampTicks = -1;
    std::int32_t timeBaseNumerator = 1;
    std::int32_t timeBaseDenominator = 1;
    // application::FrameMatchKind code for how this side mapped to the canonical position
    // (0 ExactIndex, 1 GlobalOffset, 2 AutoAligned, 3 ManualAnchor, 4 Missing).
    std::int32_t presentationMatchKind = 0;
    // application::MissingReason code + 1 when the side was Missing at capture
    // (1 AlignmentGap, 2 BeforeSourceStart, 3 AfterSourceEnd); 0 when not applicable.
    std::int32_t presentationMissingReason = 0;
};

struct IssueViewContext final {
    std::int32_t viewMode = 0;
    std::int32_t differenceEdge = 0;
    bool roiEnabled = false;
    double roiLeft = 0.0;
    double roiTop = 0.0;
    double roiRight = 1.0;
    double roiBottom = 1.0;
    double zoom = 1.0;
    double centerX = 0.5;
    double centerY = 0.5;
    // E1: wipe split axis (0 vertical, 1 horizontal) and split ratio. -1 / negative means the
    // record carries no wipe state (legacy records, or a capture outside wipe mode): restore
    // keeps the current orientation and must not claim an exact split-ratio replay. Records
    // written with a wipe view carry the real values so orientation and ratio round-trip.
    std::int32_t wipeOrientation = -1;
    double wipePosition = -1.0;
    // Image workspace only.
    std::int32_t compareMode = 0;
    double panX = 0.5;
    double panY = 0.5;
    bool resampleAllowed = false;
    // FLOW1 image observation settings. -1 means "not captured" (legacy record or a capture
    // outside the image workspace); valid values replay exactly. Alpha filtering is a
    // tri-state for the same reason: -1 not captured, 0 off, 1 on.
    std::int32_t imageViewMode = -1;
    std::int32_t imageBackgroundMode = -1;
    std::int32_t imageDiffGain = -1;
    std::int32_t imageAlphaFilterEnabled = -1;
    std::int32_t imageAlphaThreshold = -1;
};

// P06 (D02) nearby-frames diagnostic interval. The anchor is the frame the session actually
// presented when the issue was marked (never a pending seek target); inFrame/outFrame bound the
// recorded neighborhood on the canonical timeline; sessionEpoch and playbackGeneration bind the
// capture to the session identity that presented the anchor, so a card opened against a
// different session cannot claim the same epoch. A negative anchorFrame means the record
// carries no interval (all records written before this field existed).
struct IssueIntervalContext final {
    std::int64_t anchorFrame = -1;
    std::int64_t inFrame = -1;
    std::int64_t outFrame = -1;
    std::uint64_t sessionEpoch = 0U;
    std::uint64_t playbackGeneration = 0U;
    // True when the interval's out frame coincides with an active loop range's out point, so
    // the film strip must mark the loop seam instead of letting automatic analysis run across
    // the wrap.
    bool loopSeamAtOut = false;
};

struct IssueRecord final {
    std::int64_t schemaVersion = kIssueRecordSchemaVersion;
    IssueRecordKind kind = IssueRecordKind::Video;
    std::int64_t createdAtUtcMilliseconds = 0;
    std::string note;
    // Screenshots are display captures, never original pixel exports.
    bool screenshotIsDisplayResult = true;
    std::string screenshotPath;
    // True only when a committed frame/pair identity is present.
    bool hasValidPresentation = false;
    std::vector<IssueSourceRef> sources;
    std::int32_t canonicalSourceIndex = 0;
    // Reference (GT) slot at capture time. Distinct from the canonical source, which only
    // anchors the timeline. -1 for records written before this field existed.
    std::int32_t referenceSourceIndex = -1;
    std::uint64_t alignmentRevision = 0U;
    IssueViewContext view;
    IssueIntervalContext interval;
    // Image-folder payload (kind == ImagePair).
    std::string leftFolder;
    std::string rightFolder;
    std::string leftPath;
    std::string rightPath;
    std::string fileName;
    std::int32_t rowIndex = -1;
};

struct ObservedSourceIdentity final {
    bool exists = false;
    std::int64_t byteSize = -1;
    std::int64_t modifiedUtcMilliseconds = -1;
};

struct IssueSourceMatchResult final {
    std::string path;
    IssueSourceMatch match = IssueSourceMatch::Missing;
};

struct IssueRestoreEvaluation final {
    IssueRestoreDecision decision = IssueRestoreDecision::Blocked;
    std::string message;
    std::vector<IssueSourceMatchResult> sources;
};

// Pure identity comparison used by restore. Does not touch the filesystem.
[[nodiscard]] IssueSourceMatch
matchIssueSourceIdentity(const IssueSourceRef& recorded,
                         const ObservedSourceIdentity& observed) noexcept;

// Decides whether a record may restore observation context. Callers supply the on-disk
// identity for each recorded source in the same order as record.sources.
[[nodiscard]] IssueRestoreEvaluation
evaluateIssueRestore(const IssueRecord& record,
                     const std::vector<ObservedSourceIdentity>& observedIdentities);

// Default radius for nearby-frames diagnostic interval (4 frames before, 4 frames after =
// 9 frames span).
inline constexpr std::int64_t kDefaultDiagnosticIntervalRadius = 4;

// Clamps the diagnostic neighborhood [anchorFrame - radius, anchorFrame + radius] onto the
// canonical timeline [0, frameCount - 1]. The anchor stays the center; the window truncates at
// timeline edges instead of shifting, so the recorded interval only ever names positions that
// exist around the presented frame. A radius below zero is treated as zero. Inputs that are not
// a presented position on a non-empty timeline return the "not captured" context (negative
// anchor). Session identity fields are left for the caller to fill from the snapshot.
[[nodiscard]] IssueIntervalContext
clampIssueInterval(std::int64_t anchorFrame, std::int64_t radius, std::int64_t frameCount) noexcept;

struct IssueRecordIoResult final {
    bool ok = false;
    std::string error;
    std::vector<IssueRecord> records;
};

// Persistence port for T7 issue records. The adapter owns JSON encoding, schema rejection,
// and atomic publication; the UI never parses issue JSON itself.
class IIssueRecordRepository {
public:
    virtual ~IIssueRecordRepository() = default;

    [[nodiscard]] virtual IssueRecordIoResult load(const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual bool save(const std::filesystem::path& path,
                                    const std::vector<IssueRecord>& records,
                                    std::string* error) const = 0;
};

} // namespace dvs::application
