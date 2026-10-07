#include "dvs/application/ClipExport.h"
#include "dvs/media/ClipExportWriter.h"
#include "dvs/test/ScopedTemporaryDirectory.h"

#include "AtomicFilePublisherTestHooks.h"
#include "AvRaii.h"
#include "ClipExportWriterTestHooks.h"

#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <vector>

namespace dvs::media {
namespace {

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream{path, std::ios::binary};
    stream << text;
}

[[nodiscard]] std::string readText(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::size_t artifacts(const std::filesystem::path& directory) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        if (entry.path().filename().u8string().find(u8".partial") != std::u8string::npos) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] application::ClipExportJob jobFor(const std::filesystem::path& target) {
    application::ClipExportJob job;
    job.requestId = 41U;
    job.sourcePath = std::filesystem::path{DVS_MEDIA_FIXTURE_DIR} / "h264_a_320x180_30fps_12.mp4";
    job.outputPath = target;
    job.plan.startMicroseconds = 0;
    job.plan.endMicroseconds = 166'667;
    return job;
}

BOOL WINAPI failReplace(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID) {
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

BOOL WINAPI failMove(LPCWSTR, LPCWSTR, DWORD) {
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

BOOL WINAPI failFlush(HANDLE) {
    SetLastError(ERROR_WRITE_FAULT);
    return FALSE;
}

int closeCount = 0;
int failingClose = 0;
BOOL WINAPI failSelectedClose(const HANDLE handle) {
    if (++closeCount == failingClose) {
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return CloseHandle(handle);
}

int failAvioClose(AVIOContext** const context) {
    const int result = avio_closep(context);
    return result < 0 ? result : AVERROR(EIO);
}

std::atomic_bool* canceledFlag = nullptr;
int cancelAfterAvioClose(AVIOContext** const context) {
    const int result = avio_closep(context);
    canceledFlag->store(true);
    return result;
}

int flushCount = 0;
bool cancelOnFlush = false;
BOOL WINAPI recordFlush(const HANDLE handle) {
    ++flushCount;
    const BOOL result = FlushFileBuffers(handle);
    if (cancelOnFlush) {
        canceledFlag->store(true);
    }
    return result;
}

std::filesystem::path recoveryBackup;
BOOL WINAPI
moveOldThenFail(const LPCWSTR target, LPCWSTR, const LPCWSTR backup, DWORD, LPVOID, LPVOID) {
    recoveryBackup = backup;
    if (!MoveFileExW(target, backup, MOVEFILE_WRITE_THROUGH)) {
        return FALSE;
    }
    SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
    return FALSE;
}

class ClipExportPublicationTests : public ::testing::TestWithParam<bool> {};

TEST_P(ClipExportPublicationTests, PublishesACompleteClipWithoutArtifacts) {
    const dvs::test::ScopedTemporaryDirectory directory{"dvs-clip-publish"};
    const auto target = directory.path() / "clip.mp4";
    if (GetParam()) {
        writeText(target, "old");
    }
    auto job = jobFor(target);
    std::vector<double> progress;
    job.progress = [&progress](const double value) { progress.push_back(value); };
    const std::atomic_bool cancel{false};
    const auto report = ClipExportWriter{}.perform(job, cancel);
    EXPECT_EQ(report.outcome, application::ClipExportOutcome::kCompleted) << report.technicalDetail;
    EXPECT_EQ(report.packetsWritten, 5);
    EXPECT_NE(readText(target), "old");
    EXPECT_GT(std::filesystem::file_size(target), 0U);
    EXPECT_EQ(artifacts(directory.path()), 0U);
    ASSERT_FALSE(progress.empty());
    EXPECT_EQ(progress.back(), 1.0);
}

TEST_P(ClipExportPublicationTests, FailuresPreserveDestinationAndCleanOwnedFiles) {
    // Every failure is exercised for a new and an existing target. The old implementation
    // ignored close errors and removed an existing destination before its second rename.
    for (int fault = 0; fault != 5; ++fault) {
        SCOPED_TRACE(fault);
        const dvs::test::ScopedTemporaryDirectory directory{"dvs-clip-publish"};
        const auto target = directory.path() / "clip.mp4";
        if (GetParam()) {
            writeText(target, "old");
        }
        closeCount = 0;
        failingClose = fault == 0 ? 1 : 2;
        platform::testing::ScopedAtomicFilePublisherApiOverride nativeFault{
            fault == 4 ? &failReplace : nullptr,
            fault == 4 ? &failMove : nullptr,
            fault == 2 ? &failFlush : nullptr,
            fault == 0 || fault == 3 ? &failSelectedClose : nullptr};
        testing::ScopedClipExportCloseOverride avioFault{fault == 1 ? &failAvioClose : nullptr};
        auto job = jobFor(target);
        bool completedProgress = false;
        job.progress = [&completedProgress](const double value) {
            completedProgress = completedProgress || value == 1.0;
        };
        const std::atomic_bool cancel{false};
        const auto report = ClipExportWriter{}.perform(job, cancel);
        EXPECT_EQ(report.outcome, application::ClipExportOutcome::kFailed);
        const char* const expectedDetail = fault == 1   ? "FFmpeg could not close"
                                           : fault == 2 ? "FlushFileBuffers"
                                           : fault == 4
                                               ? (GetParam() ? "ReplaceFileW" : "MoveFileExW")
                                               : "CloseHandle";
        EXPECT_NE(report.technicalDetail.find(expectedDetail), std::string::npos);
        EXPECT_FALSE(completedProgress);
        EXPECT_EQ(std::filesystem::exists(target), GetParam());
        if (GetParam()) {
            EXPECT_EQ(readText(target), "old");
        }
        EXPECT_EQ(artifacts(directory.path()), 0U);
    }
}

TEST_P(ClipExportPublicationTests, CancellationBeforeCommitPreservesDestination) {
    for (int stage = 0; stage != 4; ++stage) {
        SCOPED_TRACE(stage);
        const dvs::test::ScopedTemporaryDirectory directory{"dvs-clip-publish"};
        const auto target = directory.path() / "clip.mp4";
        if (GetParam()) {
            writeText(target, "old");
        }
        std::atomic_bool cancel{stage == 0};
        canceledFlag = &cancel;
        flushCount = 0;
        cancelOnFlush = stage == 3;
        testing::ScopedClipExportCloseOverride closeHook{stage == 2 ? &cancelAfterAvioClose
                                                                    : nullptr};
        platform::testing::ScopedAtomicFilePublisherApiOverride flushHook{
            nullptr, nullptr, &recordFlush};
        auto job = jobFor(target);
        bool completedProgress = false;
        job.progress = [&](const double value) {
            completedProgress = completedProgress || value == 1.0;
            if (stage == 1) {
                cancel.store(true);
            }
        };
        const auto report = ClipExportWriter{}.perform(job, cancel);
        EXPECT_EQ(report.outcome, application::ClipExportOutcome::kCanceled);
        EXPECT_EQ(flushCount, stage == 3 ? 1 : 0);
        EXPECT_FALSE(completedProgress);
        EXPECT_EQ(std::filesystem::exists(target), GetParam());
        if (GetParam()) {
            EXPECT_EQ(readText(target), "old");
        }
        EXPECT_EQ(artifacts(directory.path()), 0U);
    }
    canceledFlag = nullptr;
}

INSTANTIATE_TEST_SUITE_P(NewAndExisting, ClipExportPublicationTests, ::testing::Bool());

TEST(ClipExportPublicationTests, LockedWindowsTargetKeepsItsOriginalBytes) {
    const dvs::test::ScopedTemporaryDirectory directory{"dvs-clip-publish"};
    const auto target = directory.path() / "clip.mp4";
    writeText(target, "old");
    const HANDLE lock = CreateFileW(
        target.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);
    const std::atomic_bool cancel{false};
    const auto report = ClipExportWriter{}.perform(jobFor(target), cancel);
    EXPECT_TRUE(CloseHandle(lock));
    EXPECT_EQ(report.outcome, application::ClipExportOutcome::kFailed);
    EXPECT_EQ(readText(target), "old");
    EXPECT_EQ(artifacts(directory.path()), 0U);
}

TEST(ClipExportPublicationTests, NewTargetRaceDoesNotOverwriteTheOtherFile) {
    const dvs::test::ScopedTemporaryDirectory directory{"dvs-clip-publish"};
    const auto target = directory.path() / "clip.mp4";
    auto job = jobFor(target);
    job.progress = [&](double) {
        if (!std::filesystem::exists(target)) {
            writeText(target, "other export");
        }
    };
    const std::atomic_bool cancel{false};
    const auto report = ClipExportWriter{}.perform(job, cancel);
    EXPECT_EQ(report.outcome, application::ClipExportOutcome::kFailed);
    EXPECT_EQ(readText(target), "other export");
    EXPECT_EQ(artifacts(directory.path()), 0U);
}

TEST(ClipExportPublicationTests, RestoresOldFileAfterPartialReplacementFailure) {
    const dvs::test::ScopedTemporaryDirectory directory{"dvs-clip-publish"};
    const auto target = directory.path() / "clip.mp4";
    writeText(target, "old");
    platform::testing::ScopedAtomicFilePublisherApiOverride fault{&moveOldThenFail, nullptr};
    const std::atomic_bool cancel{false};
    const auto report = ClipExportWriter{}.perform(jobFor(target), cancel);
    EXPECT_EQ(report.outcome, application::ClipExportOutcome::kFailed);
    EXPECT_EQ(readText(target), "old");
    EXPECT_NE(report.technicalDetail.find("restored"), std::string::npos);
    EXPECT_EQ(artifacts(directory.path()), 0U);
}

class ClipExportPathTests : public ::testing::TestWithParam<bool> {};

TEST_P(ClipExportPathTests, RetainsRecoveryCopiesAndReportsUtf8Paths) {
    const dvs::test::ScopedTemporaryDirectory directory{"dvs-clip-publish"};
    const auto target = directory.path() / (GetParam() ? std::filesystem::path{u8"片段-恢复.mp4"}
                                                       : std::filesystem::path{"recovery.mp4"});
    writeText(target, "old");
    platform::testing::ScopedAtomicFilePublisherApiOverride fault{&moveOldThenFail, &failMove};
    const std::atomic_bool cancel{false};
    const auto report = ClipExportWriter{}.perform(jobFor(target), cancel);
    EXPECT_EQ(report.outcome, application::ClipExportOutcome::kFailed);
    EXPECT_EQ(readText(recoveryBackup), "old");
    EXPECT_EQ(artifacts(directory.path()), 2U);
    const auto utf8 = recoveryBackup.u8string();
    const std::string backupText{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    EXPECT_NE(report.technicalDetail.find(backupText), std::string::npos);
    auto replacement = recoveryBackup;
    replacement.replace_extension();
    EXPECT_GT(std::filesystem::file_size(replacement), 0U);
}

TEST_P(ClipExportPathTests, RepeatedRequestIdsAndUnicodeNamesRemainIndependent) {
    const dvs::test::ScopedTemporaryDirectory directory{"dvs-clip-publish"};
    const auto stem =
        GetParam() ? std::filesystem::path{u8"片段-新"} : std::filesystem::path{"clip"};
    auto target = directory.path() / stem;
    target += ".mp4";
    auto job = jobFor(target);
    // A colliding name from the former request-id-only scheme belongs to somebody else.
    auto unrelated = directory.path() / stem;
    unrelated += ".41.partial.mp4";
    writeText(unrelated, "unrelated");
    const std::atomic_bool cancel{false};
    const auto first = ClipExportWriter{}.perform(job, cancel);
    ASSERT_EQ(first.outcome, application::ClipExportOutcome::kCompleted) << first.technicalDetail;
    const auto firstBytes = readText(target);
    const auto second = ClipExportWriter{}.perform(job, cancel);
    EXPECT_EQ(second.outcome, application::ClipExportOutcome::kCompleted) << second.technicalDetail;
    EXPECT_EQ(readText(target), firstBytes);
    EXPECT_EQ(readText(unrelated), "unrelated");
    EXPECT_EQ(artifacts(directory.path()), 1U);
}

INSTANTIATE_TEST_SUITE_P(AsciiAndUnicode, ClipExportPathTests, ::testing::Bool());

} // namespace
} // namespace dvs::media
