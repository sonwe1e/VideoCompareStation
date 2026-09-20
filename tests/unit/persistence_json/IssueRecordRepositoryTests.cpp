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
        record.sources.push_back(source);
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
    ASSERT_EQ(loaded.records.front().sources.size(), 1U);
    EXPECT_EQ(loaded.records.front().sources.front().displayIndex, 3);
}

TEST_F(IssueRecordRepositoryTests, RejectsUnsupportedSchemaWithoutTouchingFile) {
    IssueRecordRepository repository{documentPath()};
    std::error_code errorCode;
    fs::create_directories(documentPath().parent_path(), errorCode);
    {
        std::ofstream stream{documentPath(), std::ios::binary | std::ios::trunc};
        stream << R"({"schemaVersion":0,"kind":"vcstation-issue-log","records":[]})" << '\n';
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

TEST_F(IssueRecordRepositoryTests, EncodeDecodeHelpersRejectOldEntrySchema) {
    const std::string text =
        R"({"schemaVersion":1,"kind":"vcstation-issue-log","records":[{"schemaVersion":0,"kind":"video"}]})";
    const IssueRecordDocument document = IssueRecordRepository::decodeDocument(text);
    EXPECT_FALSE(document.ok);
    EXPECT_TRUE(document.records.empty());
}

} // namespace
} // namespace dvs::persistence
