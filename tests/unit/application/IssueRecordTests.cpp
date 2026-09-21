#include "dvs/application/IssueRecord.h"

#include <gtest/gtest.h>
#include <vector>

namespace dvs::application {
namespace {

[[nodiscard]] IssueRecord makeVideoRecord() {
    IssueRecord record;
    record.schemaVersion = kIssueRecordSchemaVersion;
    record.kind = IssueRecordKind::Video;
    record.hasValidPresentation = true;
    IssueSourceRef source;
    source.path = "C:/media/a.mp4";
    source.byteSize = 100;
    source.modifiedUtcMilliseconds = 1'000;
    source.hasPresentation = true;
    source.displayIndex = 42;
    record.sources.push_back(source);
    return record;
}

TEST(IssueRecordTests, MatchesIdentityWhenSizeAndMtimeAgree) {
    IssueSourceRef recorded;
    recorded.byteSize = 10;
    recorded.modifiedUtcMilliseconds = 20;
    ObservedSourceIdentity observed{.exists = true, .byteSize = 10, .modifiedUtcMilliseconds = 20};
    EXPECT_EQ(matchIssueSourceIdentity(recorded, observed), IssueSourceMatch::Matched);
}

TEST(IssueRecordTests, ReportsMissingAndModifiedWithoutPretendingRestore) {
    IssueSourceRef recorded;
    recorded.byteSize = 10;
    recorded.modifiedUtcMilliseconds = 20;
    EXPECT_EQ(matchIssueSourceIdentity(recorded, ObservedSourceIdentity{}),
              IssueSourceMatch::Missing);
    ObservedSourceIdentity modified{.exists = true, .byteSize = 11, .modifiedUtcMilliseconds = 20};
    EXPECT_EQ(matchIssueSourceIdentity(recorded, modified), IssueSourceMatch::Modified);
    IssueSourceRef incomplete;
    incomplete.byteSize = -1;
    EXPECT_EQ(matchIssueSourceIdentity(incomplete, ObservedSourceIdentity{.exists = true}),
              IssueSourceMatch::IncompleteIdentity);
}

TEST(IssueRecordTests, ReadyRequiresMatchingIdentitiesAndValidPresentation) {
    const IssueRecord record = makeVideoRecord();
    const std::vector<ObservedSourceIdentity> matched{
        ObservedSourceIdentity{.exists = true, .byteSize = 100, .modifiedUtcMilliseconds = 1'000}};
    const IssueRestoreEvaluation ready = evaluateIssueRestore(record, matched);
    EXPECT_EQ(ready.decision, IssueRestoreDecision::Ready);

    const std::vector<ObservedSourceIdentity> moved{
        ObservedSourceIdentity{.exists = true, .byteSize = 999, .modifiedUtcMilliseconds = 1'000}};
    const IssueRestoreEvaluation relocation = evaluateIssueRestore(record, moved);
    EXPECT_EQ(relocation.decision, IssueRestoreDecision::RelocationRequired);

    IssueRecord noPresentation = record;
    noPresentation.hasValidPresentation = false;
    const IssueRestoreEvaluation empty = evaluateIssueRestore(noPresentation, matched);
    EXPECT_EQ(empty.decision, IssueRestoreDecision::NoValidPresentation);
}

TEST(IssueRecordTests, RejectsUnsupportedSchemaAndEmptyRecords) {
    IssueRecord oldSchema = makeVideoRecord();
    oldSchema.schemaVersion = 0;
    EXPECT_EQ(evaluateIssueRestore(oldSchema, {}).decision, IssueRestoreDecision::Blocked);

    IssueRecord empty;
    empty.schemaVersion = kIssueRecordSchemaVersion;
    empty.sources.clear();
    EXPECT_EQ(evaluateIssueRestore(empty, {}).decision, IssueRestoreDecision::Blocked);
}

} // namespace
} // namespace dvs::application
