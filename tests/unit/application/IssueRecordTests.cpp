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

TEST(IssueRecordTests, ClampsIntervalAroundAnInteriorAnchor) {
    const IssueIntervalContext interval = clampIssueInterval(10, 4, 30);
    EXPECT_EQ(interval.anchorFrame, 10);
    EXPECT_EQ(interval.inFrame, 6);
    EXPECT_EQ(interval.outFrame, 14);
}

TEST(IssueRecordTests, TruncatesIntervalAtTimelineEdgesWithoutShiftingTheAnchor) {
    const IssueIntervalContext head = clampIssueInterval(1, 4, 30);
    EXPECT_EQ(head.anchorFrame, 1);
    EXPECT_EQ(head.inFrame, 0);
    EXPECT_EQ(head.outFrame, 5);

    const IssueIntervalContext tail = clampIssueInterval(28, 4, 30);
    EXPECT_EQ(tail.anchorFrame, 28);
    EXPECT_EQ(tail.inFrame, 24);
    EXPECT_EQ(tail.outFrame, 29);

    // A radius larger than the timeline covers it whole; the anchor stays presented-centered.
    const IssueIntervalContext whole = clampIssueInterval(2, 50, 5);
    EXPECT_EQ(whole.anchorFrame, 2);
    EXPECT_EQ(whole.inFrame, 0);
    EXPECT_EQ(whole.outFrame, 4);
}

TEST(IssueRecordTests, IntervalWithoutAPresentedAnchorStaysUncaptured) {
    const IssueIntervalContext noAnchor = clampIssueInterval(-1, 4, 30);
    EXPECT_EQ(noAnchor.anchorFrame, -1);
    EXPECT_EQ(noAnchor.inFrame, -1);
    EXPECT_EQ(noAnchor.outFrame, -1);
    const IssueIntervalContext pastEnd = clampIssueInterval(30, 4, 30);
    EXPECT_EQ(pastEnd.anchorFrame, -1);
    EXPECT_EQ(pastEnd.inFrame, -1);
    EXPECT_EQ(pastEnd.outFrame, -1);
    const IssueIntervalContext noTimeline = clampIssueInterval(5, 4, 0);
    EXPECT_EQ(noTimeline.anchorFrame, -1);
    EXPECT_EQ(noTimeline.inFrame, -1);
    EXPECT_EQ(noTimeline.outFrame, -1);
    // A negative radius degenerates to the anchor-only interval rather than widening it.
    const IssueIntervalContext degenerate = clampIssueInterval(5, -3, 30);
    EXPECT_EQ(degenerate.anchorFrame, 5);
    EXPECT_EQ(degenerate.inFrame, 5);
    EXPECT_EQ(degenerate.outFrame, 5);
}

} // namespace
} // namespace dvs::application
