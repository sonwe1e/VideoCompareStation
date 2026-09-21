#include "dvs/application/IssueRecord.h"

#include <utility>

namespace dvs::application {

IssueSourceMatch matchIssueSourceIdentity(const IssueSourceRef& recorded,
                                          const ObservedSourceIdentity& observed) noexcept {
    if (recorded.byteSize < 0 || recorded.modifiedUtcMilliseconds < 0) {
        return IssueSourceMatch::IncompleteIdentity;
    }
    if (!observed.exists) {
        return IssueSourceMatch::Missing;
    }
    if (observed.byteSize != recorded.byteSize ||
        observed.modifiedUtcMilliseconds != recorded.modifiedUtcMilliseconds) {
        return IssueSourceMatch::Modified;
    }
    return IssueSourceMatch::Matched;
}

IssueRestoreEvaluation
evaluateIssueRestore(const IssueRecord& record,
                     const std::vector<ObservedSourceIdentity>& observedIdentities) {
    IssueRestoreEvaluation evaluation;
    if (record.schemaVersion != kIssueRecordSchemaVersion) {
        evaluation.decision = IssueRestoreDecision::Blocked;
        evaluation.message = "unsupported-schema-version";
        return evaluation;
    }
    if (record.sources.empty()) {
        evaluation.decision = IssueRestoreDecision::Blocked;
        evaluation.message = "record-has-no-sources";
        return evaluation;
    }
    if (record.kind == IssueRecordKind::ImagePair) {
        if (record.leftPath.empty() && record.rightPath.empty()) {
            evaluation.decision = IssueRestoreDecision::Blocked;
            evaluation.message = "image-record-has-no-paths";
            return evaluation;
        }
    }

    bool anyMismatch = false;
    evaluation.sources.reserve(record.sources.size());
    for (std::size_t index = 0; index < record.sources.size(); ++index) {
        const IssueSourceRef& recorded = record.sources[index];
        const ObservedSourceIdentity observed = index < observedIdentities.size()
                                                    ? observedIdentities[index]
                                                    : ObservedSourceIdentity{};
        const IssueSourceMatch match = matchIssueSourceIdentity(recorded, observed);
        evaluation.sources.push_back(IssueSourceMatchResult{recorded.path, match});
        if (match != IssueSourceMatch::Matched) {
            anyMismatch = true;
        }
    }

    if (anyMismatch) {
        // Identity mismatch never pretends to restore: the user must relocate first.
        evaluation.decision = IssueRestoreDecision::RelocationRequired;
        evaluation.message = "source-moved-or-modified-relocation-required";
        return evaluation;
    }
    if (!record.hasValidPresentation) {
        evaluation.decision = IssueRestoreDecision::NoValidPresentation;
        evaluation.message = "sources-match-but-no-valid-presentation";
        return evaluation;
    }
    evaluation.decision = IssueRestoreDecision::Ready;
    evaluation.message = "ready";
    return evaluation;
}

} // namespace dvs::application
