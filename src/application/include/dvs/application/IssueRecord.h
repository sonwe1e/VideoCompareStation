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
    // sessions that never committed a frame.
    bool hasPresentation = false;
    std::int64_t displayIndex = -1;
    std::int64_t presentationTimestampTicks = -1;
    std::int32_t timeBaseNumerator = 1;
    std::int32_t timeBaseDenominator = 1;
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
    // Image workspace only.
    std::int32_t compareMode = 0;
    double panX = 0.5;
    double panY = 0.5;
    bool resampleAllowed = false;
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
    std::uint64_t alignmentRevision = 0U;
    IssueViewContext view;
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
