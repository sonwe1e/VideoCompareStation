#include "dvs/domain/ComparisonValidator.h"

#include <algorithm>
#include <utility>

namespace dvs::domain {

ValidatedComparisonSet::ValidatedComparisonSet(std::vector<ComparisonSource> sources,
                                               const SourceId canonicalSourceId,
                                               std::optional<SourceId> referenceSourceId)
    : sources_(std::move(sources)), canonicalSourceId_(canonicalSourceId),
      referenceSourceId_(referenceSourceId) {}

std::span<const ComparisonSource> ValidatedComparisonSet::sources() const noexcept {
    return sources_;
}

std::size_t ValidatedComparisonSet::sourceCount() const noexcept {
    return sources_.size();
}

const ComparisonSource* ValidatedComparisonSet::find(const SourceId id) const noexcept {
    const auto iterator =
        std::find_if(sources_.begin(), sources_.end(), [id](const ComparisonSource& source) {
            return source.id == id;
        });
    if (iterator == sources_.end()) {
        return nullptr;
    }
    return &*iterator;
}

SourceId ValidatedComparisonSet::canonicalSourceId() const noexcept {
    return canonicalSourceId_;
}

std::optional<SourceId> ValidatedComparisonSet::referenceSourceId() const noexcept {
    return referenceSourceId_;
}

const MediaDescriptor& ValidatedComparisonSet::canonicalDescriptor() const noexcept {
    return find(canonicalSourceId_)->descriptor;
}

const std::optional<RationalRate>& ValidatedComparisonSet::canonicalRate() const noexcept {
    return canonicalDescriptor().frameRate;
}

std::int64_t ValidatedComparisonSet::canonicalFrameCount() const noexcept {
    return canonicalDescriptor().frameCount.value;
}

bool ValidatedComparisonSet::hasEstimatedFrameCount() const noexcept {
    return std::any_of(sources_.begin(), sources_.end(), [](const ComparisonSource& source) {
        return source.descriptor.frameCount.origin == FrameCountOrigin::kEstimated;
    });
}

void CompatibilityReport::add(CompatibilityFinding finding) {
    findings_.push_back(std::move(finding));
}

std::span<const CompatibilityFinding> CompatibilityReport::findings() const noexcept {
    return findings_;
}

bool CompatibilityReport::isEmpty() const noexcept {
    return findings_.empty();
}

bool CompatibilityReport::hasFatal() const noexcept {
    return std::any_of(findings_.begin(), findings_.end(), [](const CompatibilityFinding& finding) {
        return finding.severity == CompatibilitySeverity::kFatal;
    });
}

bool CompatibilityReport::hasAlignmentRequired() const noexcept {
    return std::any_of(findings_.begin(), findings_.end(), [](const CompatibilityFinding& finding) {
        return finding.severity == CompatibilitySeverity::kAlignmentRequired;
    });
}

namespace {

constexpr std::size_t kMinimumSources = 1U;
constexpr std::size_t kMaximumSources = 3U;

[[nodiscard]] MediaError setFailure(const MediaErrorCode code, std::string technicalDetail) {
    return makeMediaError(code,
                          MediaOperation::kSourcePairValidation,
                          std::nullopt,
                          false,
                          std::move(technicalDetail));
}

void comparePair(const ComparisonSource& first,
                 const ComparisonSource& second,
                 CompatibilityReport& report) {
    const std::vector<SourceId> pair{first.id, second.id};
    const MediaDescriptor& a = first.descriptor;
    const MediaDescriptor& b = second.descriptor;

    if (a.frameCount.value != b.frameCount.value) {
        report.add(CompatibilityFinding{
            .severity = CompatibilitySeverity::kAlignmentRequired,
            .code = MediaErrorCode::kSourceFrameCountMismatch,
            .sources = pair,
        });
    }
    if (a.frameRate.has_value() && b.frameRate.has_value() && *a.frameRate != *b.frameRate) {
        report.add(CompatibilityFinding{
            .severity = CompatibilitySeverity::kAlignmentRequired,
            .code = MediaErrorCode::kSourceFrameRateMismatch,
            .sources = pair,
        });
    }
    if (a.duration != b.duration) {
        report.add(CompatibilityFinding{
            .severity = CompatibilitySeverity::kAlignmentRequired,
            .code = MediaErrorCode::kSourceDurationMismatch,
            .sources = pair,
        });
    }
    if (a.extent.width != b.extent.width || a.extent.height != b.extent.height) {
        report.add(CompatibilityFinding{
            .severity = CompatibilitySeverity::kWarning,
            .code = MediaErrorCode::kSourceResolutionMismatch,
            .sources = pair,
        });
    }
    if (a.rotationDegrees != b.rotationDegrees || a.sampleAspectRatio != b.sampleAspectRatio) {
        report.add(CompatibilityFinding{
            .severity = CompatibilitySeverity::kWarning,
            .code = MediaErrorCode::kSourceResolutionMismatch,
            .sources = pair,
        });
    }
    if (a.colorMetadata.matrix != b.colorMetadata.matrix ||
        a.colorMetadata.range != b.colorMetadata.range ||
        a.colorMetadata.transfer != b.colorMetadata.transfer) {
        report.add(CompatibilityFinding{
            .severity = CompatibilitySeverity::kWarning,
            .code = MediaErrorCode::kSourceColorMetadataMismatch,
            .sources = pair,
        });
    }
}

} // namespace

Result<ComparisonValidation> ComparisonValidator::validate(std::vector<ComparisonSource> sources) {
    if (sources.size() < kMinimumSources || sources.size() > kMaximumSources) {
        return Result<ComparisonValidation>::failure(setFailure(
            MediaErrorCode::kInvalidArgument, "A review session requires one to three sources."));
    }

    for (std::size_t index = 0; index < sources.size(); ++index) {
        for (std::size_t other = index + 1; other < sources.size(); ++other) {
            if (sources[index].id == sources[other].id) {
                return Result<ComparisonValidation>::failure(
                    setFailure(MediaErrorCode::kInvalidArgument,
                               "Comparison source IDs must be unique within a session."));
            }
        }
    }

    std::optional<SourceId> referenceId;
    for (const ComparisonSource& source : sources) {
        if (!source.descriptor.isValid()) {
            return Result<ComparisonValidation>::failure(
                makeMediaError(MediaErrorCode::kInvalidMediaDescriptor,
                               MediaOperation::kSourcePairValidation,
                               SourceId{source.id},
                               false,
                               "Source " + std::to_string(source.id) + " descriptor is invalid."));
        }
        if (source.role == ComparisonRole::kReference) {
            if (referenceId.has_value()) {
                return Result<ComparisonValidation>::failure(
                    setFailure(MediaErrorCode::kInvalidArgument,
                               "At most one source may carry the reference role."));
            }
            referenceId = source.id;
        }
    }

    const SourceId canonicalId = referenceId.value_or(sources.front().id);

    CompatibilityReport report;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        for (std::size_t other = index + 1; other < sources.size(); ++other) {
            comparePair(sources[index], sources[other], report);
        }
    }

    return Result<ComparisonValidation>::success(ComparisonValidation{
        .set = ValidatedComparisonSet{std::move(sources), canonicalId, referenceId},
        .report = std::move(report),
    });
}

} // namespace dvs::domain
