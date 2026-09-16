#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace dvs::test {

class ScopedTemporaryDirectory final {
public:
    explicit ScopedTemporaryDirectory(const std::string_view prefix) {
        validatePrefix(prefix);

        std::error_code error;
        root_ = std::filesystem::temp_directory_path(error).lexically_normal();
        if (error) {
            throw std::filesystem::filesystem_error("Could not resolve the temporary directory",
                                                    error);
        }

        std::random_device random;
        const auto entropy =
            (static_cast<std::uint64_t>(random()) << 32U) ^ static_cast<std::uint64_t>(random());
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();

        constexpr std::uint64_t kMaximumAttempts = 128U;
        for (std::uint64_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
            const std::uint64_t sequence = nextSequence_.fetch_add(1U);
            path_ = root_ / (std::string{prefix} + "-" + std::to_string(timestamp) + "-" +
                             std::to_string(entropy) + "-" + std::to_string(sequence));

            error.clear();
            if (std::filesystem::create_directory(path_, error)) {
                ownsDirectory_ = true;
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error(
                    "Could not create a temporary test directory", path_, error);
            }
        }

        throw std::runtime_error("Could not allocate a unique temporary test directory");
    }

    ~ScopedTemporaryDirectory() {
        if (!ownsDirectory_ || path_.empty() || path_.parent_path() != root_) {
            return;
        }

        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    ScopedTemporaryDirectory(const ScopedTemporaryDirectory&) = delete;
    ScopedTemporaryDirectory& operator=(const ScopedTemporaryDirectory&) = delete;
    ScopedTemporaryDirectory(ScopedTemporaryDirectory&&) = delete;
    ScopedTemporaryDirectory& operator=(ScopedTemporaryDirectory&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    static void validatePrefix(const std::string_view prefix) {
        if (prefix.empty() || prefix.size() > 64U) {
            throw std::invalid_argument(
                "A temporary directory prefix must contain 1-64 characters");
        }

        for (const char character : prefix) {
            const bool isAsciiLetter =
                (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
            const bool isDigit = character >= '0' && character <= '9';
            if (!isAsciiLetter && !isDigit && character != '-' && character != '_') {
                throw std::invalid_argument("A temporary directory prefix may contain only ASCII "
                                            "letters, digits, '-' and '_'");
            }
        }
    }

    inline static std::atomic<std::uint64_t> nextSequence_{0};

    std::filesystem::path root_;
    std::filesystem::path path_;
    bool ownsDirectory_ = false;
};

} // namespace dvs::test
