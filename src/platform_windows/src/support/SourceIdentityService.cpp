#include "dvs/platform/SourceIdentityService.h"

#ifndef _WIN32
#error "SourceIdentityService is only supported on Windows."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "dvs/platform/WindowsPaths.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <windows.h>

namespace dvs::platform {
namespace {

constexpr std::uint64_t kOneMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kWholeFileLimit = 2ULL * kOneMiB;
constexpr std::size_t kReadChunkBytes = 64U * 1024U;
constexpr std::uint64_t kWindowsEpochOffset100Nanoseconds = 116444736000000000ULL;
constexpr std::uint64_t kHundredNanosecondsPerMillisecond = 10000ULL;

struct FileMetadata final {
    std::uint64_t byteSize = 0;
    std::uint64_t lastWriteFileTime = 0;
    std::filesystem::path absolutePath;
};

[[nodiscard]] domain::MediaError fingerprintError(const domain::MediaErrorCode code,
                                                  const domain::SourceId sourceId,
                                                  const domain::MediaOperation operation,
                                                  std::string technicalDetail) {
    return domain::makeMediaError(code,
                                  operation,
                                  sourceId,
                                  code == domain::MediaErrorCode::kSourceMissing ||
                                      code == domain::MediaErrorCode::kSourceFingerprintMismatch,
                                  std::move(technicalDetail));
}

template <typename TValue>
[[nodiscard]] domain::Result<TValue> fingerprintFailure(const domain::MediaErrorCode code,
                                                        const domain::SourceId sourceId,
                                                        const domain::MediaOperation operation,
                                                        std::string technicalDetail) {
    return domain::Result<TValue>::failure(
        fingerprintError(code, sourceId, operation, std::move(technicalDetail)));
}

[[nodiscard]] domain::Status fingerprintFailureStatus(const domain::MediaErrorCode code,
                                                      const domain::SourceId sourceId,
                                                      const domain::MediaOperation operation,
                                                      std::string technicalDetail) {
    return domain::Status::failure(
        fingerprintError(code, sourceId, operation, std::move(technicalDetail)));
}

[[nodiscard]] bool isMissingFileError(const DWORD errorCode) noexcept {
    return errorCode == ERROR_FILE_NOT_FOUND || errorCode == ERROR_PATH_NOT_FOUND ||
           errorCode == ERROR_INVALID_NAME;
}

[[nodiscard]] std::string systemFailure(const char* const api, const DWORD errorCode) {
    return std::string{api} + " failed with Windows error " + std::to_string(errorCode) + ".";
}

[[nodiscard]] domain::Result<FileMetadata> readMetadata(const std::filesystem::path& sourcePath,
                                                        const domain::SourceId sourceId,
                                                        const domain::MediaOperation operation) {
    const auto absolutePath = WindowsPaths::absolutePath(sourcePath);
    if (!absolutePath) {
        return fingerprintFailure<FileMetadata>(domain::MediaErrorCode::kFileIo,
                                                sourceId,
                                                operation,
                                                "Could not resolve the source path: " +
                                                    absolutePath.error().technicalDetail);
    }

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(absolutePath.value().c_str(), GetFileExInfoStandard, &attributes)) {
        const DWORD errorCode = GetLastError();
        const domain::MediaErrorCode code = isMissingFileError(errorCode)
                                                ? domain::MediaErrorCode::kSourceMissing
                                                : domain::MediaErrorCode::kFileIo;
        return fingerprintFailure<FileMetadata>(
            code, sourceId, operation, systemFailure("GetFileAttributesExW", errorCode));
    }
    if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        return fingerprintFailure<FileMetadata>(
            domain::MediaErrorCode::kFileIo,
            sourceId,
            operation,
            "Source path names a directory rather than a media file.");
    }

    const std::uint64_t byteSize = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32U) |
                                   static_cast<std::uint64_t>(attributes.nFileSizeLow);
    const std::uint64_t lastWriteFileTime =
        (static_cast<std::uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32U) |
        static_cast<std::uint64_t>(attributes.ftLastWriteTime.dwLowDateTime);
    if (byteSize == 0U) {
        return fingerprintFailure<FileMetadata>(
            domain::MediaErrorCode::kFileIo,
            sourceId,
            operation,
            "An empty source file cannot produce a valid media identity.");
    }

    return domain::Result<FileMetadata>::success(FileMetadata{
        .byteSize = byteSize,
        .lastWriteFileTime = lastWriteFileTime,
        .absolutePath = absolutePath.value(),
    });
}

[[nodiscard]] domain::Result<std::int64_t> utcMilliseconds(const std::uint64_t fileTime,
                                                           const domain::SourceId sourceId,
                                                           const domain::MediaOperation operation) {
    const std::uint64_t delta = fileTime >= kWindowsEpochOffset100Nanoseconds
                                    ? fileTime - kWindowsEpochOffset100Nanoseconds
                                    : kWindowsEpochOffset100Nanoseconds - fileTime;
    const std::uint64_t milliseconds = delta / kHundredNanosecondsPerMillisecond;
    if (milliseconds > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return fingerprintFailure<std::int64_t>(
            domain::MediaErrorCode::kFileIo,
            sourceId,
            operation,
            "Source modified time cannot be represented as UTC milliseconds.");
    }

    const std::int64_t signedMilliseconds = static_cast<std::int64_t>(milliseconds);
    return domain::Result<std::int64_t>::success(
        fileTime >= kWindowsEpochOffset100Nanoseconds ? signedMilliseconds : -signedMilliseconds);
}

[[nodiscard]] constexpr std::uint32_t rotateRight(const std::uint32_t value,
                                                  const std::uint32_t shift) noexcept {
    return (value >> shift) | (value << (32U - shift));
}

class Sha256 final {
public:
    void update(const std::span<const char> bytes) noexcept {
        for (const char byte : bytes) {
            buffer_[bufferSize_] = static_cast<std::uint8_t>(static_cast<unsigned char>(byte));
            ++bufferSize_;
            if (bufferSize_ == buffer_.size()) {
                transform(buffer_);
                bitCount_ += 512U;
                bufferSize_ = 0U;
            }
        }
    }

    [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept {
        bitCount_ += static_cast<std::uint64_t>(bufferSize_) * 8U;
        buffer_[bufferSize_] = 0x80U;
        ++bufferSize_;

        if (bufferSize_ > 56U) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(bufferSize_),
                      buffer_.end(),
                      std::uint8_t{0});
            transform(buffer_);
            bufferSize_ = 0U;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(bufferSize_),
                  buffer_.begin() + 56,
                  std::uint8_t{0});
        for (std::size_t index = 0; index < 8U; ++index) {
            const std::size_t shift = (7U - index) * 8U;
            buffer_[56U + index] = static_cast<std::uint8_t>(bitCount_ >> shift);
        }
        transform(buffer_);

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t index = 0; index < state_.size(); ++index) {
            for (std::size_t byte = 0; byte < 4U; ++byte) {
                const std::size_t shift = (3U - byte) * 8U;
                digest[index * 4U + byte] = static_cast<std::uint8_t>(state_[index] >> shift);
            }
        }
        return digest;
    }

private:
    void transform(const std::array<std::uint8_t, 64>& block) noexcept {
        constexpr std::array<std::uint32_t, 64> kRoundConstants{
            0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U,
            0x923F82A4U, 0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
            0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U,
            0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
            0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U,
            0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
            0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
            0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
            0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU,
            0x5B9CCA4FU, 0x682E6FF3U, 0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
            0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
        };

        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const std::size_t offset = index * 4U;
            words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                           (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                           (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                           static_cast<std::uint32_t>(block[offset + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            const std::uint32_t lowerSigma0 = rotateRight(words[index - 15U], 7U) ^
                                              rotateRight(words[index - 15U], 18U) ^
                                              (words[index - 15U] >> 3U);
            const std::uint32_t lowerSigma1 = rotateRight(words[index - 2U], 17U) ^
                                              rotateRight(words[index - 2U], 19U) ^
                                              (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + lowerSigma0 + words[index - 7U] + lowerSigma1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t upperSigma1 =
                rotateRight(e, 6U) ^ rotateRight(e, 11U) ^ rotateRight(e, 25U);
            const std::uint32_t choose = (e & f) ^ ((~e) & g);
            const std::uint32_t temporary1 =
                h + upperSigma1 + choose + kRoundConstants[index] + words[index];
            const std::uint32_t upperSigma0 =
                rotateRight(a, 2U) ^ rotateRight(a, 13U) ^ rotateRight(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = upperSigma0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint8_t, 64> buffer_{};
    std::size_t bufferSize_ = 0U;
    std::uint64_t bitCount_ = 0U;
    std::array<std::uint32_t, 8> state_{
        0x6A09E667U,
        0xBB67AE85U,
        0x3C6EF372U,
        0xA54FF53AU,
        0x510E527FU,
        0x9B05688CU,
        0x1F83D9ABU,
        0x5BE0CD19U,
    };
};

[[nodiscard]] bool hashRange(std::ifstream& stream,
                             const std::uint64_t offset,
                             const std::uint64_t byteCount,
                             Sha256* const hasher) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        byteCount > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }

    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream) {
        return false;
    }

    std::array<char, kReadChunkBytes> buffer{};
    std::uint64_t remaining = byteCount;
    while (remaining != 0U) {
        const std::size_t requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
        stream.read(buffer.data(), static_cast<std::streamsize>(requested));
        if (stream.gcount() != static_cast<std::streamsize>(requested)) {
            return false;
        }
        hasher->update(std::span<const char>{buffer.data(), requested});
        remaining -= requested;
    }
    return true;
}

[[nodiscard]] std::string lowercaseHex(const std::array<std::uint8_t, 32>& digest) {
    constexpr std::array<char, 16> kHex{
        '0',
        '1',
        '2',
        '3',
        '4',
        '5',
        '6',
        '7',
        '8',
        '9',
        'a',
        'b',
        'c',
        'd',
        'e',
        'f',
    };

    std::string result;
    result.reserve(digest.size() * 2U);
    for (const std::uint8_t byte : digest) {
        result.push_back(kHex[byte >> 4U]);
        result.push_back(kHex[byte & 0x0FU]);
    }
    return result;
}

[[nodiscard]] bool sameFingerprint(const std::string_view left,
                                   const std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto leftCharacter = static_cast<unsigned char>(left[index]);
        const auto rightCharacter = static_cast<unsigned char>(right[index]);
        if (std::tolower(leftCharacter) != std::tolower(rightCharacter)) {
            return false;
        }
    }
    return true;
}

} // namespace

domain::Result<domain::SourceFileIdentity>
SourceIdentityService::fingerprint(const std::filesystem::path& sourcePath,
                                   const domain::SourceId sourceId,
                                   const domain::MediaOperation operation) {
    auto before = readMetadata(sourcePath, sourceId, operation);
    if (!before) {
        return domain::Result<domain::SourceFileIdentity>::failure(before.error());
    }

    const auto modifiedUtcMilliseconds =
        utcMilliseconds(before.value().lastWriteFileTime, sourceId, operation);
    if (!modifiedUtcMilliseconds) {
        return domain::Result<domain::SourceFileIdentity>::failure(modifiedUtcMilliseconds.error());
    }

    std::ifstream stream(before.value().absolutePath, std::ios::binary);
    if (!stream) {
        return fingerprintFailure<domain::SourceFileIdentity>(
            domain::MediaErrorCode::kFileIo,
            sourceId,
            operation,
            "Could not open the source file for fingerprinting.");
    }

    Sha256 hasher;
    const std::uint64_t byteSize = before.value().byteSize;
    const bool readSucceeded = byteSize <= kWholeFileLimit
                                   ? hashRange(stream, 0U, byteSize, &hasher)
                                   : hashRange(stream, 0U, kOneMiB, &hasher) &&
                                         hashRange(stream, byteSize - kOneMiB, kOneMiB, &hasher);
    if (!readSucceeded) {
        return fingerprintFailure<domain::SourceFileIdentity>(
            domain::MediaErrorCode::kFileIo,
            sourceId,
            operation,
            "Could not read the required source bytes for fingerprinting.");
    }

    auto after = readMetadata(before.value().absolutePath, sourceId, operation);
    if (!after) {
        return domain::Result<domain::SourceFileIdentity>::failure(after.error());
    }
    if (after.value().byteSize != byteSize ||
        after.value().lastWriteFileTime != before.value().lastWriteFileTime) {
        return fingerprintFailure<domain::SourceFileIdentity>(
            domain::MediaErrorCode::kFileIo,
            sourceId,
            operation,
            "Source changed while its identity was being computed.");
    }

    return domain::Result<domain::SourceFileIdentity>::success(domain::SourceFileIdentity{
        .byteSize = byteSize,
        .modifiedUtcMilliseconds = modifiedUtcMilliseconds.value(),
        .fingerprintSha256 = lowercaseHex(hasher.finish()),
    });
}

domain::Status SourceIdentityService::verify(const std::filesystem::path& sourcePath,
                                             const domain::SourceFileIdentity& expected,
                                             const domain::SourceId sourceId,
                                             const domain::MediaOperation operation) {
    if (!expected.isComplete()) {
        return fingerprintFailureStatus(domain::MediaErrorCode::kInvalidArgument,
                                        sourceId,
                                        operation,
                                        "Persisted source identity is incomplete.");
    }

    auto actual = fingerprint(sourcePath, sourceId, operation);
    if (!actual) {
        return domain::Status::failure(actual.error());
    }
    if (actual.value().byteSize != expected.byteSize ||
        actual.value().modifiedUtcMilliseconds != expected.modifiedUtcMilliseconds ||
        !sameFingerprint(actual.value().fingerprintSha256, expected.fingerprintSha256)) {
        return fingerprintFailureStatus(
            domain::MediaErrorCode::kSourceFingerprintMismatch,
            sourceId,
            operation,
            "Source size, UTC modified time, or SHA-256 fingerprint does not match the project.");
    }
    return domain::Status::success();
}

} // namespace dvs::platform
