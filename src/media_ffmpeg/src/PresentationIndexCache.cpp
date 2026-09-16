#include "PresentationIndexCache.h"

#include "dvs/platform/AtomicFilePublisher.h"
#include "dvs/platform/WindowsPaths.h"

extern "C" {
#include <libavformat/version.h>
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dvs::media::internal {
namespace {

constexpr std::array<std::byte, 8U> kMagic{
    std::byte{'D'},
    std::byte{'V'},
    std::byte{'S'},
    std::byte{'P'},
    std::byte{'T'},
    std::byte{'S'},
    std::byte{'I'},
    std::byte{'1'},
};
constexpr std::uint32_t kSchemaVersion = 1U;
constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;
constexpr std::size_t kMaximumFingerprintBytes = 128U;
constexpr std::uint64_t kMaximumFrameCount = 100'000'000ULL;

struct CacheState final {
    std::mutex mutex;
    std::unordered_map<std::string, std::weak_ptr<const std::vector<std::int64_t>>> indexes;
};

[[nodiscard]] CacheState& cacheState() {
    static CacheState state;
    return state;
}

[[nodiscard]] bool cacheable(const TimestampIndexRequest& request) noexcept {
    return request.sourceIdentity.has_value() && request.sourceIdentity->isComplete() &&
           request.streamIndex.has_value() && *request.streamIndex >= 0 &&
           request.timeBase.has_value() && request.timeBase->numerator > 0 &&
           request.timeBase->denominator > 0 &&
           request.sourceIdentity->fingerprintSha256.size() <= kMaximumFingerprintBytes &&
           std::all_of(request.sourceIdentity->fingerprintSha256.begin(),
                       request.sourceIdentity->fingerprintSha256.end(),
                       [](const unsigned char character) {
                           return (character >= '0' && character <= '9') ||
                                  (character >= 'a' && character <= 'f') ||
                                  (character >= 'A' && character <= 'F');
                       });
}

[[nodiscard]] std::string memoryKey(const TimestampIndexRequest& request) {
    const domain::SourceFileIdentity& identity = *request.sourceIdentity;
    return identity.fingerprintSha256 + ":" + std::to_string(identity.byteSize) + ":" +
           std::to_string(identity.modifiedUtcMilliseconds) + ":" +
           std::to_string(*request.streamIndex) + ":" +
           std::to_string(request.timeBase->numerator) + ":" +
           std::to_string(request.timeBase->denominator) + ":" +
           std::to_string(LIBAVFORMAT_VERSION_MAJOR) + ":" +
           std::to_string(LIBAVFORMAT_VERSION_MINOR) + ":" + std::to_string(kSchemaVersion);
}

[[nodiscard]] std::optional<std::filesystem::path> cachePath(const TimestampIndexRequest& request) {
    const auto paths = platform::WindowsPaths::applicationDataPaths();
    if (!paths) {
        return std::nullopt;
    }
    const std::filesystem::path directory =
        paths.value().proxyCacheDirectory / L"PresentationIndexes";
    if (!platform::WindowsPaths::ensureDirectory(directory)) {
        return std::nullopt;
    }
    const std::string filename =
        request.sourceIdentity->fingerprintSha256 + "-s" + std::to_string(*request.streamIndex) +
        "-ff" + std::to_string(LIBAVFORMAT_VERSION_MAJOR) + "." +
        std::to_string(LIBAVFORMAT_VERSION_MINOR) + "-v" + std::to_string(kSchemaVersion) + ".dvsi";
    return directory / filename;
}

template <typename Value> void append(std::vector<std::byte>& bytes, const Value value) {
    static_assert(std::is_integral_v<Value>);
    using Unsigned = std::make_unsigned_t<Value>;
    Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0U; index < sizeof(Value); ++index) {
        bytes.push_back(static_cast<std::byte>((bits >> (index * 8U)) & 0xFFU));
    }
}

template <typename Value>
[[nodiscard]] bool read(const std::span<const std::byte> bytes,
                        std::size_t* const cursor,
                        Value* const value) noexcept {
    static_assert(std::is_integral_v<Value>);
    if (*cursor > bytes.size() || bytes.size() - *cursor < sizeof(Value)) {
        return false;
    }
    using Unsigned = std::make_unsigned_t<Value>;
    Unsigned bits = 0U;
    for (std::size_t index = 0U; index < sizeof(Value); ++index) {
        bits |= static_cast<Unsigned>(std::to_integer<unsigned int>(bytes[*cursor + index]))
                << (index * 8U);
    }
    *cursor += sizeof(Value);
    *value = static_cast<Value>(bits);
    return true;
}

[[nodiscard]] std::uint64_t checksum(const std::span<const std::byte> bytes) noexcept {
    std::uint64_t value = kFnvOffset;
    for (const std::byte byte : bytes) {
        value ^= std::to_integer<std::uint8_t>(byte);
        value *= kFnvPrime;
    }
    return value;
}

[[nodiscard]] std::vector<std::byte> serialize(const TimestampIndexRequest& request,
                                               const std::vector<std::int64_t>& timestamps) {
    const std::string& fingerprint = request.sourceIdentity->fingerprintSha256;
    std::vector<std::byte> bytes;
    bytes.reserve(80U + fingerprint.size() + timestamps.size() * sizeof(std::int64_t));
    bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
    append(bytes, kSchemaVersion);
    append(bytes, static_cast<std::uint32_t>(LIBAVFORMAT_VERSION_MAJOR));
    append(bytes, static_cast<std::uint32_t>(LIBAVFORMAT_VERSION_MINOR));
    append(bytes, static_cast<std::int32_t>(*request.streamIndex));
    append(bytes, static_cast<std::int32_t>(request.timeBase->numerator));
    append(bytes, static_cast<std::int32_t>(request.timeBase->denominator));
    append(bytes, request.sourceIdentity->byteSize);
    append(bytes, request.sourceIdentity->modifiedUtcMilliseconds);
    append(bytes, static_cast<std::uint32_t>(fingerprint.size()));
    append(bytes, static_cast<std::uint64_t>(timestamps.size()));
    for (const char character : fingerprint) {
        bytes.push_back(static_cast<std::byte>(character));
    }
    for (const std::int64_t timestamp : timestamps) {
        append(bytes, timestamp);
    }
    append(bytes, checksum(bytes));
    return bytes;
}

[[nodiscard]] std::optional<PresentationTimestampIndex>
deserialize(const TimestampIndexRequest& request, const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size < 64U || size > (kMaximumFrameCount * sizeof(std::int64_t) + 512U) ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return std::nullopt;
    }

    std::size_t cursor = kMagic.size();
    std::uint32_t schema = 0U;
    std::uint32_t ffmpegMajor = 0U;
    std::uint32_t ffmpegMinor = 0U;
    std::int32_t streamIndex = -1;
    std::int32_t timeBaseNumerator = 0;
    std::int32_t timeBaseDenominator = 0;
    std::uint64_t byteSize = 0U;
    std::int64_t modified = 0;
    std::uint32_t fingerprintSize = 0U;
    std::uint64_t frameCount = 0U;
    if (!read(bytes, &cursor, &schema) || !read(bytes, &cursor, &ffmpegMajor) ||
        !read(bytes, &cursor, &ffmpegMinor) || !read(bytes, &cursor, &streamIndex) ||
        !read(bytes, &cursor, &timeBaseNumerator) || !read(bytes, &cursor, &timeBaseDenominator) ||
        !read(bytes, &cursor, &byteSize) || !read(bytes, &cursor, &modified) ||
        !read(bytes, &cursor, &fingerprintSize) || !read(bytes, &cursor, &frameCount) ||
        schema != kSchemaVersion || ffmpegMajor != LIBAVFORMAT_VERSION_MAJOR ||
        ffmpegMinor != LIBAVFORMAT_VERSION_MINOR || streamIndex != *request.streamIndex ||
        timeBaseNumerator != request.timeBase->numerator ||
        timeBaseDenominator != request.timeBase->denominator ||
        byteSize != request.sourceIdentity->byteSize ||
        modified != request.sourceIdentity->modifiedUtcMilliseconds ||
        fingerprintSize != request.sourceIdentity->fingerprintSha256.size() ||
        fingerprintSize > kMaximumFingerprintBytes || frameCount == 0U ||
        frameCount > kMaximumFrameCount ||
        (request.expectedFrameCount.has_value() &&
         *request.expectedFrameCount != static_cast<std::int64_t>(frameCount))) {
        return std::nullopt;
    }

    const std::uint64_t payloadBytes = frameCount * sizeof(std::int64_t);
    const std::uint64_t expectedSize =
        cursor + fingerprintSize + payloadBytes + sizeof(std::uint64_t);
    if (expectedSize != bytes.size()) {
        return std::nullopt;
    }
    const std::string fingerprint{reinterpret_cast<const char*>(bytes.data() + cursor),
                                  fingerprintSize};
    cursor += fingerprintSize;
    if (fingerprint != request.sourceIdentity->fingerprintSha256) {
        return std::nullopt;
    }

    auto timestamps = std::make_shared<std::vector<std::int64_t>>();
    timestamps->reserve(static_cast<std::size_t>(frameCount));
    for (std::uint64_t index = 0U; index < frameCount; ++index) {
        std::int64_t timestamp = 0;
        if (!read(bytes, &cursor, &timestamp)) {
            return std::nullopt;
        }
        timestamps->push_back(timestamp);
    }
    std::uint64_t storedChecksum = 0U;
    if (!read(bytes, &cursor, &storedChecksum) ||
        storedChecksum != checksum(std::span<const std::byte>{bytes}.first(
                              bytes.size() - sizeof(std::uint64_t)))) {
        return std::nullopt;
    }
    return timestamps;
}

} // namespace

std::optional<PresentationTimestampIndex>
PresentationIndexCache::load(const TimestampIndexRequest& request) noexcept {
    if (!cacheable(request)) {
        return std::nullopt;
    }
    try {
        const std::string key = memoryKey(request);
        {
            CacheState& state = cacheState();
            std::scoped_lock lock{state.mutex};
            const auto found = state.indexes.find(key);
            if (found != state.indexes.end()) {
                if (PresentationTimestampIndex index = found->second.lock()) {
                    return index;
                }
                state.indexes.erase(found);
            }
        }
        const std::optional<std::filesystem::path> path = cachePath(request);
        if (!path.has_value()) {
            return std::nullopt;
        }
        std::optional<PresentationTimestampIndex> index = deserialize(request, *path);
        if (index.has_value()) {
            CacheState& state = cacheState();
            std::scoped_lock lock{state.mutex};
            state.indexes.insert_or_assign(key, *index);
        }
        return index;
    } catch (...) {
        return std::nullopt;
    }
}

void PresentationIndexCache::store(const TimestampIndexRequest& request,
                                   PresentationTimestampIndex index) noexcept {
    if (!cacheable(request) || !index || index->empty()) {
        return;
    }
    try {
        const std::string key = memoryKey(request);
        {
            CacheState& state = cacheState();
            std::scoped_lock lock{state.mutex};
            state.indexes.insert_or_assign(key, index);
        }
        const std::optional<std::filesystem::path> path = cachePath(request);
        if (!path.has_value()) {
            return;
        }
        const std::vector<std::byte> bytes = serialize(request, *index);
        auto publisher = platform::AtomicFilePublisher::begin(
            *path,
            platform::TemporaryFileIdentity{
                .operation = "pts-index",
                .ownerId = request.sourceIdentity->fingerprintSha256.substr(0U, 16U),
                .revision = kSchemaVersion,
            });
        if (!publisher || !publisher.value()->write(bytes) || !publisher.value()->flush() ||
            !deserialize(request, publisher.value()->temporaryPath()).has_value()) {
            return;
        }
        std::error_code error;
        if (std::filesystem::exists(*path, error) && !error) {
            static_cast<void>(publisher.value()->publishReplacingExisting());
        } else {
            static_cast<void>(publisher.value()->publishNew());
        }
    } catch (...) {
    }
}

} // namespace dvs::media::internal
