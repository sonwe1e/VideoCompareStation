#include "dvs/persistence/IssueRecordRepository.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace dvs::persistence {
namespace {

namespace fs = std::filesystem;

class IssueRecordRepositoryTests : public ::testing::Test {
protected:
    [[nodiscard]] fs::path documentPath() const {
        return temp_ / "issues.json";
    }

    void SetUp() override {
        temp_ = fs::temp_directory_path() /
                ("dvs_issue_record_tests_" +
                 std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        fs::create_directories(temp_);
    }

    void TearDown() override {
        std::error_code errorCode;
        fs::remove_all(temp_, errorCode);
    }

    [[nodiscard]] application::IssueRecord sampleRecord() const {
        application::IssueRecord record;
        record.schemaVersion = application::kIssueRecordSchemaVersion;
        record.kind = application::IssueRecordKind::Video;
        record.hasValidPresentation = true;
        record.note = "sample";
        application::IssueSourceRef source;
        source.path = "C:/media/a.mp4";
        source.byteSize = 10;
        source.modifiedUtcMilliseconds = 20;
        source.hasPresentation = true;
        source.displayIndex = 3;
        source.presentationTimestampTicks = 133'000;
        source.timeBaseNumerator = 1;
        source.timeBaseDenominator = 1'000'000;
        source.presentationMatchKind = 2; // AutoAligned
        source.presentationMissingReason = 0;
        record.sources.push_back(source);
        application::IssueSourceRef missing;
        missing.path = "C:/media/b.mp4";
        missing.byteSize = 11;
        missing.modifiedUtcMilliseconds = 21;
        missing.hasPresentation = false;
        missing.displayIndex = -1;
        missing.presentationMatchKind = 4;     // Missing
        missing.presentationMissingReason = 1; // AlignmentGap
        record.sources.push_back(missing);
        return record;
    }

    fs::path temp_;
};

TEST_F(IssueRecordRepositoryTests, RoundTripsVersionedDocument) {
    IssueRecordRepository repository{documentPath()};
    std::string error;
    ASSERT_TRUE(repository.save(documentPath(), {sampleRecord()}, &error)) << error;
    const application::IssueRecordIoResult loaded = repository.load(documentPath());
    ASSERT_TRUE(loaded.ok) << loaded.error;
    ASSERT_EQ(loaded.records.size(), 1U);
    EXPECT_EQ(loaded.records.front().note, "sample");
    EXPECT_TRUE(loaded.records.front().hasValidPresentation);
    ASSERT_EQ(loaded.records.front().sources.size(), 2U);
    const application::IssueSourceRef& first = loaded.records.front().sources.front();
    EXPECT_EQ(first.displayIndex, 3);
    EXPECT_EQ(first.presentationTimestampTicks, 133'000);
    EXPECT_EQ(first.timeBaseNumerator, 1);
    EXPECT_EQ(first.timeBaseDenominator, 1'000'000);
    EXPECT_EQ(first.presentationMatchKind, 2);
    EXPECT_EQ(first.presentationMissingReason, 0);
    const application::IssueSourceRef& missing = loaded.records.front().sources.back();
    EXPECT_FALSE(missing.hasPresentation);
    EXPECT_EQ(missing.displayIndex, -1);
    EXPECT_EQ(missing.presentationMatchKind, 4);
    EXPECT_EQ(missing.presentationMissingReason, 1);
}

TEST_F(IssueRecordRepositoryTests, RejectsUnsupportedSchemaWithoutTouchingFile) {
    IssueRecordRepository repository{documentPath()};
    std::error_code errorCode;
    fs::create_directories(documentPath().parent_path(), errorCode);
    {
        std::ofstream stream{documentPath(), std::ios::binary | std::ios::trunc};
        stream << R"({"schemaVersion":0,"kind":"comparestation-issue-log","records":[]})" << '\n';
    }
    const auto before = fs::file_size(documentPath());
    const application::IssueRecordIoResult loaded = repository.load(documentPath());
    EXPECT_FALSE(loaded.ok);
    EXPECT_NE(loaded.error.find("unsupported issue-record schemaVersion"), std::string::npos);
    EXPECT_EQ(fs::file_size(documentPath()), before);
}

TEST_F(IssueRecordRepositoryTests, RejectsCorruptJsonWithExplanation) {
    IssueRecordRepository repository{documentPath()};
    std::error_code errorCode;
    fs::create_directories(documentPath().parent_path(), errorCode);
    {
        std::ofstream stream{documentPath(), std::ios::binary | std::ios::trunc};
        stream << "not-json";
    }
    const application::IssueRecordIoResult loaded = repository.load(documentPath());
    EXPECT_FALSE(loaded.ok);
    EXPECT_NE(loaded.error.find("not valid UTF-8 JSON"), std::string::npos);
}

TEST_F(IssueRecordRepositoryTests, RoundTripsIntervalContext) {
    IssueRecordRepository repository{documentPath()};
    application::IssueRecord record = sampleRecord();
    record.interval.anchorFrame = 41;
    record.interval.inFrame = 37;
    record.interval.outFrame = 45;
    record.interval.sessionEpoch = 7U;
    record.interval.playbackGeneration = 12U;
    record.interval.loopSeamAtOut = true;
    std::string error;
    ASSERT_TRUE(repository.save(documentPath(), {record}, &error)) << error;
    const application::IssueRecordIoResult loaded = repository.load(documentPath());
    ASSERT_TRUE(loaded.ok) << loaded.error;
    ASSERT_EQ(loaded.records.size(), 1U);
    const application::IssueIntervalContext& interval = loaded.records.front().interval;
    EXPECT_EQ(interval.anchorFrame, 41);
    EXPECT_EQ(interval.inFrame, 37);
    EXPECT_EQ(interval.outFrame, 45);
    EXPECT_EQ(interval.sessionEpoch, 7U);
    EXPECT_EQ(interval.playbackGeneration, 12U);
    EXPECT_TRUE(interval.loopSeamAtOut);
}

TEST_F(IssueRecordRepositoryTests, RecordsWithoutAnIntervalStayLegacyShaped) {
    IssueRecordRepository repository{documentPath()};
    // No interval set: the anchor default is "not captured".
    const application::IssueRecord record = sampleRecord();
    const std::string text = repository.encodeDocument({record});
    EXPECT_EQ(text.find("\"interval\""), std::string::npos);
    const IssueRecordDocument document = repository.decodeDocument(text);
    ASSERT_TRUE(document.ok) << document.error;
    ASSERT_EQ(document.records.size(), 1U);
    EXPECT_EQ(document.records.front().interval.anchorFrame, -1);
    EXPECT_FALSE(document.records.front().interval.loopSeamAtOut);
}

// E1: the wipe pair round-trips exactly when valid, and the three non-valid shapes behave
// differently on purpose - a legacy record keeps -1/-1 ("no state"), while present-but-
// invalid data (half-pair, orientation outside {0,1} including the wide-integer narrowing
// trap, ratio outside [0,1]) is rejected to -2/-2 so a re-save writes the explicit invalid
// marker instead of masquerading as legacy.
TEST_F(IssueRecordRepositoryTests, WipePairRoundTripsAndRejectsInvalidShapes) {
    IssueRecordRepository repository{documentPath()};

    // Legacy record: no wipe fields at all -> -1 sentinels, and nothing wipe-shaped is
    // written back.
    const std::string legacyText = repository.encodeDocument({sampleRecord()});
    EXPECT_EQ(legacyText.find("\"wipeOrientation\""), std::string::npos);
    const IssueRecordDocument legacy = repository.decodeDocument(legacyText);
    ASSERT_TRUE(legacy.ok) << legacy.error;
    EXPECT_EQ(legacy.records.front().view.wipeOrientation, -1);
    EXPECT_EQ(legacy.records.front().view.wipePosition, -1.0);

    // Valid pair round-trips bit-exact through save/load.
    application::IssueRecord valid = sampleRecord();
    valid.view.wipeOrientation = 1;
    valid.view.wipePosition = 0.25;
    std::string error;
    ASSERT_TRUE(repository.save(documentPath(), {valid}, &error)) << error;
    const IssueRecordDocument loadedValid =
        repository.decodeDocument(repository.encodeDocument({valid}));
    ASSERT_TRUE(loadedValid.ok) << loadedValid.error;
    EXPECT_EQ(loadedValid.records.front().view.wipeOrientation, 1);
    EXPECT_DOUBLE_EQ(loadedValid.records.front().view.wipePosition, 0.25);

    // Half-pair: position without orientation is rejected to -2, and a re-save writes the
    // explicit -2 marker rather than silently dropping the fields.
    std::string halfPair = legacyText;
    halfPair.insert(halfPair.find("\"resampleAllowed\""), R"("wipePosition": 0.25,)");
    const IssueRecordDocument half = repository.decodeDocument(halfPair);
    ASSERT_TRUE(half.ok) << half.error;
    EXPECT_EQ(half.records.front().view.wipeOrientation, -2);
    EXPECT_EQ(half.records.front().view.wipePosition, -2.0);
    const std::string halfResaved = repository.encodeDocument({half.records.front()});
    EXPECT_NE(halfResaved.find("\"wipeOrientation\": -2"), std::string::npos);

    // Wide integer: 4294967296 would narrow to 0 through a 32-bit int and sneak past a
    // {0,1} membership check - it must be rejected before any narrowing.
    std::string wideInteger = legacyText;
    wideInteger.insert(wideInteger.find("\"resampleAllowed\""),
                       R"("wipeOrientation": 4294967296, "wipePosition": 0.25,)");
    const IssueRecordDocument wide = repository.decodeDocument(wideInteger);
    ASSERT_TRUE(wide.ok) << wide.error;
    EXPECT_EQ(wide.records.front().view.wipeOrientation, -2);

    // Ratio out of range: orientation is valid alone, but the pair is refused.
    std::string outOfRange = legacyText;
    outOfRange.insert(outOfRange.find("\"resampleAllowed\""),
                      R"("wipeOrientation": 1, "wipePosition": 1.5,)");
    const IssueRecordDocument range = repository.decodeDocument(outOfRange);
    ASSERT_TRUE(range.ok) << range.error;
    EXPECT_EQ(range.records.front().view.wipeOrientation, -2);
}

TEST_F(IssueRecordRepositoryTests, EncodeDecodeHelpersRejectOldEntrySchema) {
    const std::string text =
        R"({"schemaVersion":1,"kind":"comparestation-issue-log","records":[{"schemaVersion":0,"kind":"video"}]})";
    const IssueRecordDocument document = IssueRecordRepository::decodeDocument(text);
    EXPECT_FALSE(document.ok);
    EXPECT_TRUE(document.records.empty());
}

} // namespace
} // namespace dvs::persistence
