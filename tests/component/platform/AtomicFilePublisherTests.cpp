#include "dvs/platform/AtomicFilePublisher.h"
#include "dvs/platform/PlatformResult.h"
#include "dvs/test/ScopedTemporaryDirectory.h"

#include "AtomicFilePublisherTestHooks.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <span>
#include <string>

namespace dvs::platform {
namespace {

void writeTextFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream << text;
}

[[nodiscard]] std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::span<const std::byte> asBytes(const std::string& text) noexcept {
    return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

[[nodiscard]] TemporaryFileIdentity testIdentity() {
    return TemporaryFileIdentity{
        .operation = "artifact",
        .ownerId = "test-output",
        .revision = 7,
    };
}

std::filesystem::path injectedBackupPath;

BOOL WINAPI failBeforeMovingReplacement(
    const LPCWSTR, const LPCWSTR, const LPCWSTR backup, const DWORD, LPVOID, LPVOID) {
    injectedBackupPath = backup;
    SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT);
    return FALSE;
}

BOOL WINAPI moveOldTargetThenFail(
    const LPCWSTR destination, const LPCWSTR, const LPCWSTR backup, const DWORD, LPVOID, LPVOID) {
    injectedBackupPath = backup;
    if (!MoveFileExW(destination, backup, MOVEFILE_WRITE_THROUGH)) {
        return FALSE;
    }

    SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
    return FALSE;
}

BOOL WINAPI failTargetRecovery(const LPCWSTR, const LPCWSTR, const DWORD) {
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

} // namespace

TEST(AtomicFilePublisherTests, ReplacesAnExistingFileOnlyAfterFlush) {
    dvs::test::ScopedTemporaryDirectory directory{"dvs-atomic-test"};
    const std::filesystem::path destination = directory.path() / "output.dat";
    writeTextFile(destination, "old");

    const std::string replacement = "new";
    std::filesystem::path temporary;
    std::filesystem::path backup;
    {
        auto transaction = AtomicFilePublisher::begin(destination, testIdentity());
        ASSERT_TRUE(transaction);
        temporary = transaction.value()->temporaryPath();
        backup = transaction.value()->recoveryBackupPath();
        ASSERT_TRUE(transaction.value()->write(asBytes(replacement)));
        ASSERT_TRUE(transaction.value()->flush());
        ASSERT_TRUE(transaction.value()->publishReplacingExisting());

        EXPECT_FALSE(std::filesystem::exists(temporary));
        EXPECT_TRUE(std::filesystem::exists(backup));
    }

    EXPECT_EQ(readTextFile(destination), replacement);
    EXPECT_FALSE(std::filesystem::exists(temporary));
    EXPECT_FALSE(std::filesystem::exists(backup));
}

TEST(AtomicFilePublisherTests, RequiresFlushBeforePublishing) {
    dvs::test::ScopedTemporaryDirectory directory{"dvs-atomic-test"};
    const std::filesystem::path destination = directory.path() / "output.dat";
    writeTextFile(destination, "old");

    auto transaction = AtomicFilePublisher::begin(destination, testIdentity());
    ASSERT_TRUE(transaction);
    const std::string replacement = "new";
    ASSERT_TRUE(transaction.value()->write(asBytes(replacement)));

    const PlatformStatus prematurePublish = transaction.value()->publishReplacingExisting();
    ASSERT_FALSE(prematurePublish);
    EXPECT_EQ(prematurePublish.error().code, PlatformErrorCode::kInvalidState);
    EXPECT_EQ(readTextFile(destination), "old");
    EXPECT_TRUE(std::filesystem::exists(transaction.value()->temporaryPath()));

    ASSERT_TRUE(transaction.value()->flush());
    ASSERT_TRUE(transaction.value()->publishReplacingExisting());
    EXPECT_EQ(readTextFile(destination), replacement);
}

TEST(AtomicFilePublisherTests, RefusesToOverwriteANewTarget) {
    dvs::test::ScopedTemporaryDirectory directory{"dvs-atomic-test"};
    const std::filesystem::path destination = directory.path() / "export.mov";
    writeTextFile(destination, "existing");

    auto transaction = AtomicFilePublisher::begin(destination, testIdentity());
    ASSERT_TRUE(transaction);
    const std::string output = "replacement";
    ASSERT_TRUE(transaction.value()->write(asBytes(output)));
    ASSERT_TRUE(transaction.value()->flush());

    const PlatformStatus publish = transaction.value()->publishNew();
    ASSERT_FALSE(publish);
    EXPECT_EQ(publish.error().code, PlatformErrorCode::kTargetAlreadyExists);
    EXPECT_EQ(readTextFile(destination), "existing");
    EXPECT_TRUE(transaction.value()->abandon());
}

TEST(AtomicFilePublisherTests, DestroysUnpublishedPartialFiles) {
    dvs::test::ScopedTemporaryDirectory directory{"dvs-atomic-test"};
    const std::filesystem::path destination = directory.path() / "settings.json";
    std::filesystem::path temporary;

    {
        auto transaction = AtomicFilePublisher::begin(destination, testIdentity());
        ASSERT_TRUE(transaction);
        temporary = transaction.value()->temporaryPath();
        EXPECT_TRUE(std::filesystem::exists(temporary));
    }

    EXPECT_FALSE(std::filesystem::exists(temporary));
}

TEST(AtomicFilePublisherTests, RetainsExistingTargetWhenReplacementCannotMove) {
    dvs::test::ScopedTemporaryDirectory directory{"dvs-atomic-test"};
    const std::filesystem::path destination = directory.path() / "output.dat";
    writeTextFile(destination, "old");
    injectedBackupPath.clear();

    std::filesystem::path temporary;
    std::filesystem::path backup;
    {
        testing::ScopedAtomicFilePublisherApiOverride faultInjection{&failBeforeMovingReplacement,
                                                                     nullptr};
        auto transaction = AtomicFilePublisher::begin(destination, testIdentity());
        ASSERT_TRUE(transaction);
        temporary = transaction.value()->temporaryPath();
        backup = transaction.value()->recoveryBackupPath();
        const std::string replacement = "new";
        ASSERT_TRUE(transaction.value()->write(asBytes(replacement)));
        ASSERT_TRUE(transaction.value()->flush());

        const PlatformStatus publish = transaction.value()->publishReplacingExisting();
        ASSERT_FALSE(publish);
        EXPECT_EQ(publish.error().code, PlatformErrorCode::kReplaceFailed);
        EXPECT_EQ(injectedBackupPath, backup);
        EXPECT_EQ(readTextFile(destination), "old");
        EXPECT_EQ(readTextFile(temporary), replacement);
        EXPECT_FALSE(std::filesystem::exists(backup));
    }

    EXPECT_EQ(readTextFile(destination), "old");
    EXPECT_FALSE(std::filesystem::exists(temporary));
    EXPECT_FALSE(std::filesystem::exists(backup));
}

TEST(AtomicFilePublisherTests, RestoresExistingTargetAfterPartialReplacementMove) {
    dvs::test::ScopedTemporaryDirectory directory{"dvs-atomic-test"};
    const std::filesystem::path destination = directory.path() / "output.dat";
    writeTextFile(destination, "old");
    injectedBackupPath.clear();

    std::filesystem::path temporary;
    std::filesystem::path backup;
    {
        testing::ScopedAtomicFilePublisherApiOverride faultInjection{&moveOldTargetThenFail,
                                                                     nullptr};
        auto transaction = AtomicFilePublisher::begin(destination, testIdentity());
        ASSERT_TRUE(transaction);
        temporary = transaction.value()->temporaryPath();
        backup = transaction.value()->recoveryBackupPath();
        const std::string replacement = "new";
        ASSERT_TRUE(transaction.value()->write(asBytes(replacement)));
        ASSERT_TRUE(transaction.value()->flush());

        const PlatformStatus publish = transaction.value()->publishReplacingExisting();
        ASSERT_FALSE(publish);
        EXPECT_EQ(publish.error().code, PlatformErrorCode::kReplaceFailed);
        EXPECT_EQ(injectedBackupPath, backup);
        EXPECT_EQ(readTextFile(destination), "old");
        EXPECT_EQ(readTextFile(temporary), replacement);
        EXPECT_FALSE(std::filesystem::exists(backup));
    }

    EXPECT_EQ(readTextFile(destination), "old");
    EXPECT_FALSE(std::filesystem::exists(temporary));
    EXPECT_FALSE(std::filesystem::exists(backup));
}

TEST(AtomicFilePublisherTests, PreservesBothCopiesWhenPartialReplacementRecoveryFails) {
    dvs::test::ScopedTemporaryDirectory directory{"dvs-atomic-test"};
    const std::filesystem::path destination = directory.path() / "output.dat";
    writeTextFile(destination, "old");
    injectedBackupPath.clear();

    std::filesystem::path temporary;
    std::filesystem::path backup;
    {
        testing::ScopedAtomicFilePublisherApiOverride faultInjection{&moveOldTargetThenFail,
                                                                     &failTargetRecovery};
        auto transaction = AtomicFilePublisher::begin(destination, testIdentity());
        ASSERT_TRUE(transaction);
        temporary = transaction.value()->temporaryPath();
        backup = transaction.value()->recoveryBackupPath();
        const std::string replacement = "new";
        ASSERT_TRUE(transaction.value()->write(asBytes(replacement)));
        ASSERT_TRUE(transaction.value()->flush());

        const PlatformStatus publish = transaction.value()->publishReplacingExisting();
        ASSERT_FALSE(publish);
        EXPECT_EQ(publish.error().code, PlatformErrorCode::kRecoveryRequired);
        EXPECT_EQ(injectedBackupPath, backup);
        EXPECT_FALSE(std::filesystem::exists(destination));
        EXPECT_EQ(readTextFile(temporary), replacement);
        EXPECT_EQ(readTextFile(backup), "old");

        const PlatformStatus abandon = transaction.value()->abandon();
        ASSERT_FALSE(abandon);
        EXPECT_EQ(abandon.error().code, PlatformErrorCode::kRecoveryRequired);
    }

    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_EQ(readTextFile(temporary), "new");
    EXPECT_EQ(readTextFile(backup), "old");
}

TEST(PlatformResultTests, CarriesPlatformErrorAsEitherValueOrFailure) {
    const PlatformError valuePayload{
        .code = PlatformErrorCode::kWriteFailed,
        .path = "C:/temp/value",
        .technicalDetail = "value payload",
    };
    const PlatformResult<PlatformError> success =
        PlatformResult<PlatformError>::success(valuePayload);
    ASSERT_TRUE(success);
    EXPECT_EQ(success.value().code, PlatformErrorCode::kWriteFailed);
    EXPECT_EQ(success.value().technicalDetail, "value payload");

    const PlatformResult<PlatformError> failure = PlatformResult<PlatformError>::failure({
        .code = PlatformErrorCode::kFlushFailed,
        .path = "C:/temp/failure",
        .technicalDetail = "failure payload",
    });
    ASSERT_FALSE(failure);
    EXPECT_EQ(failure.error().code, PlatformErrorCode::kFlushFailed);
    EXPECT_EQ(failure.error().technicalDetail, "failure payload");
}

} // namespace dvs::platform
