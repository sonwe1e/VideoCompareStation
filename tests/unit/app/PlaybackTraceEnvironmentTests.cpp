#include "PlaybackTraceEnvironment.h"

#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>

namespace dvs::app {
namespace {

constexpr wchar_t kTraceEnvironmentName[] = L"DVS_PLAYBACK_TRACE";

class ScopedTraceEnvironment final {
public:
    ScopedTraceEnvironment() {
        wchar_t* value = nullptr;
        std::size_t valueLength = 0U;
        const errno_t status = _wdupenv_s(&value, &valueLength, kTraceEnvironmentName);
        if (status != 0) {
            return;
        }
        captured_ = true;
        const std::unique_ptr<wchar_t, decltype(&std::free)> ownedValue{value, &std::free};
        if (valueLength > 1U) {
            originalValue_ = ownedValue.get();
        }
    }

    ~ScopedTraceEnvironment() {
        if (!captured_) {
            return;
        }
        static_cast<void>(_wputenv_s(kTraceEnvironmentName, originalValue_.value_or(L"").c_str()));
    }

    ScopedTraceEnvironment(const ScopedTraceEnvironment&) = delete;
    ScopedTraceEnvironment& operator=(const ScopedTraceEnvironment&) = delete;

    [[nodiscard]] bool captured() const noexcept {
        return captured_;
    }

private:
    std::optional<std::wstring> originalValue_;
    bool captured_ = false;
};

TEST(PlaybackTraceEnvironmentTests, PreservesUnicodeTracePathWithoutNarrowing) {
    ScopedTraceEnvironment environment;
    ASSERT_TRUE(environment.captured());
    const std::filesystem::path expected{L"跟踪 目录\\播放记录.jsonl"};
    ASSERT_EQ(_wputenv_s(kTraceEnvironmentName, expected.c_str()), 0);

    const std::optional<std::filesystem::path> actual = playbackTracePathFromEnvironment();

    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(actual->native(), expected.native());
}

TEST(PlaybackTraceEnvironmentTests, TreatsAnUnsetTracePathAsDisabled) {
    ScopedTraceEnvironment environment;
    ASSERT_TRUE(environment.captured());
    ASSERT_EQ(_wputenv_s(kTraceEnvironmentName, L""), 0);

    EXPECT_FALSE(playbackTracePathFromEnvironment().has_value());
}

} // namespace
} // namespace dvs::app
