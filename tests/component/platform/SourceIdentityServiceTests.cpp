#include "dvs/platform/SourceIdentityService.h"
#include "dvs/test/ScopedTemporaryDirectory.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace dvs::platform {
namespace {

class SourceIdentityServiceTests : public ::testing::Test {
protected:
    [[nodiscard]] std::filesystem::path filePath(const std::string_view name) const {
        return directory_.path() / std::string{name};
    }

    void writeFile(const std::filesystem::path& path, const std::string_view contents) const {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream.is_open());
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        ASSERT_TRUE(stream.good());
    }

private:
    dvs::test::ScopedTemporaryDirectory directory_{"dvs-source-identity"};
};

TEST_F(SourceIdentityServiceTests, ReportsMissingSourcesAsRecoverableMediaProbeErrors) {
    const auto result = SourceIdentityService::fingerprint(
        filePath("missing.mp4"), domain::SourceId{0}, domain::MediaOperation::kMediaProbe);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::MediaErrorCode::kSourceMissing);
    EXPECT_EQ(result.error().operation, domain::MediaOperation::kMediaProbe);
    EXPECT_EQ(result.error().source, domain::SourceId{0});
    EXPECT_TRUE(result.error().recoverable);
}

TEST_F(SourceIdentityServiceTests, ReportsMismatchesAsRecoverableMediaProbeErrors) {
    const std::filesystem::path source = filePath("changed.mp4");
    writeFile(source, "abc");
    const auto expected = SourceIdentityService::fingerprint(
        source, domain::SourceId{1}, domain::MediaOperation::kMediaProbe);
    ASSERT_TRUE(expected);

    writeFile(source, "xyz");
    const auto status = SourceIdentityService::verify(
        source, expected.value(), domain::SourceId{1}, domain::MediaOperation::kMediaProbe);

    ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, domain::MediaErrorCode::kSourceFingerprintMismatch);
    EXPECT_EQ(status.error().operation, domain::MediaOperation::kMediaProbe);
    EXPECT_EQ(status.error().source, domain::SourceId{1});
    EXPECT_TRUE(status.error().recoverable);
}

} // namespace
} // namespace dvs::platform
