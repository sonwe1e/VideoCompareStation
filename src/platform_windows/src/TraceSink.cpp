#include "dvs/platform/TraceSink.h"

#include "dvs/platform/AtomicFilePublisher.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

namespace dvs::platform {

namespace {

// Encodes a single trace event as one JSON object on one line (JSONL). Kept dependency-free:
// no JSON library, no allocator on the hot path beyond the reused `buffer`. Field order matches
// trace-schema.md. Command and request are emitted as their integer values; a nullopt command
// is emitted as null. Optional incoming identity fields are additive schema-v1 fields.
[[nodiscard]] int formatEventJson(char* const buffer,
                                  const std::size_t capacity,
                                  const application::TraceEvent& event) noexcept {
    // Schema v1 has eleven numeric fields plus five optional incoming fields; their valid
    // all-UINT64_MAX representation is larger than 256 bytes. Keep fixed storage, but size it
    // for the complete worst-case record.
    const auto command = event.identity.command;
    char commandBuffer[32];
    const char* commandText = "null";
    if (command.has_value()) {
        const int commandLength = std::snprintf(commandBuffer,
                                                sizeof(commandBuffer),
                                                "%llu",
                                                static_cast<unsigned long long>(command->value()));
        if (commandLength <= 0 || commandLength >= static_cast<int>(sizeof(commandBuffer))) {
            return false;
        }
        commandText = commandBuffer;
    }
    char incomingBuffer[160];
    incomingBuffer[0] = '\0';
    if (event.incoming.has_value()) {
        const int incomingLength =
            std::snprintf(incomingBuffer,
                          sizeof(incomingBuffer),
                          R"(,"is":%llu,"ie":%llu,"igen":%llu,"idev":%llu,"ireq":%llu)",
                          static_cast<unsigned long long>(event.incoming->session.value()),
                          static_cast<unsigned long long>(event.incoming->epoch.value()),
                          static_cast<unsigned long long>(event.incoming->generation.value()),
                          static_cast<unsigned long long>(event.incoming->device.value()),
                          static_cast<unsigned long long>(event.incoming->request.value()));
        if (incomingLength <= 0 || incomingLength >= static_cast<int>(sizeof(incomingBuffer))) {
            return false;
        }
    }
    // snprintf returns the would-be length (excluding the terminating NUL). Clamp to the buffer
    // size so a truncated line never reads past the array; the line is simply dropped to keep
    // the export well-formed rather than emitting a partial JSON object.
    const int written =
        std::snprintf(buffer,
                      capacity,
                      R"({"t":%llu,"kind":%u,"s":%llu,"e":%llu,"topo":%llu,"tl":%llu,)"
                      R"("al":%llu,"gen":%llu,"dev":%llu,"req":%llu,"cmd":%s,"p":%llu%s})"
                      "\n",
                      static_cast<unsigned long long>(event.timestampMicroseconds),
                      static_cast<unsigned>(event.kind),
                      static_cast<unsigned long long>(event.identity.session.value()),
                      static_cast<unsigned long long>(event.identity.epoch.value()),
                      static_cast<unsigned long long>(event.identity.topology.value()),
                      static_cast<unsigned long long>(event.identity.timeline.value()),
                      static_cast<unsigned long long>(event.identity.alignment.value()),
                      static_cast<unsigned long long>(event.identity.generation.value()),
                      static_cast<unsigned long long>(event.identity.device.value()),
                      static_cast<unsigned long long>(event.identity.request.value()),
                      commandText,
                      static_cast<unsigned long long>(event.payload),
                      incomingBuffer);
    return written > 0 && written < static_cast<int>(capacity) ? written : 0;
}

[[nodiscard]] int formatOverflowJson(char* const buffer,
                                     const std::size_t capacity,
                                     const std::uint64_t lostCount) noexcept {
    const int written = std::snprintf(
        buffer, capacity, "{\"overflow\":%llu}\n", static_cast<unsigned long long>(lostCount));
    return written > 0 && written < static_cast<int>(capacity) ? written : 0;
}

} // namespace

MemoryTraceSink::MemoryTraceSink(const std::size_t capacity) : capacity_(capacity) {
    events_.reserve(capacity);
}

bool MemoryTraceSink::append(const application::TraceEvent& event) noexcept {
    if (events_.size() < capacity_) {
        try {
            events_.push_back(event);
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

bool MemoryTraceSink::recordOverflow(const std::uint64_t lostCount) noexcept {
    overflow_ += lostCount;
    return true;
}

bool MemoryTraceSink::finalize() noexcept {
    return true;
}

std::uint64_t MemoryTraceSink::overflowCount() const noexcept {
    return overflow_;
}

const std::vector<application::TraceEvent>& MemoryTraceSink::events() const noexcept {
    return events_;
}

void MemoryTraceSink::clear() noexcept {
    events_.clear();
    overflow_ = 0U;
}

FileTraceSink::FileTraceSink(std::filesystem::path path) : path_(std::move(path)) {}

FileTraceSink::~FileTraceSink() = default;

bool FileTraceSink::ensureOpen() noexcept {
    if (publisher_ != nullptr) {
        return true;
    }
    if (failed_ || finalized_) {
        return false;
    }

    try {
        std::error_code error;
        if (path_.has_parent_path()) {
            std::filesystem::create_directories(path_.parent_path(), error);
            if (error) {
                failed_ = true;
                return false;
            }
        }
        replaceExisting_ = std::filesystem::exists(path_, error);
        if (error) {
            failed_ = true;
            return false;
        }
        auto transaction = AtomicFilePublisher::begin(path_,
                                                      TemporaryFileIdentity{
                                                          .operation = "trace-export",
                                                          .ownerId = "playback",
                                                          .revision = 1U,
                                                      });
        if (!transaction) {
            failed_ = true;
            return false;
        }
        publisher_ = std::move(transaction).value();
        if (writeHeader()) {
            return true;
        }
    } catch (...) {
        failed_ = true;
    }
    publisher_.reset();
    return false;
}

bool FileTraceSink::writeHeader() noexcept {
    if (headerWritten_ || publisher_ == nullptr) {
        return headerWritten_;
    }
    // 19 bytes: the literal has 19 characters; writing 20 would emit the terminating NUL.
    static constexpr char kHeader[] = "{\"traceVersion\":1}\n";
    headerWritten_ = writeBytes(kHeader, sizeof(kHeader) - 1U);
    return headerWritten_;
}

bool FileTraceSink::writeBytes(const char* const data, const std::size_t size) noexcept {
    if (publisher_ == nullptr || failed_ || finalized_) {
        return false;
    }
    if (size > writeBuffer_.size()) {
        if (!flushPendingBytes()) {
            return false;
        }
        try {
            const auto bytes = std::span{reinterpret_cast<const std::byte*>(data), size};
            if (publisher_->write(bytes)) {
                return true;
            }
        } catch (...) {
        }
        failed_ = true;
        return false;
    }
    if (size > writeBuffer_.size() - pendingBytes_ && !flushPendingBytes()) {
        return false;
    }
    std::memcpy(writeBuffer_.data() + pendingBytes_, data, size);
    pendingBytes_ += size;
    return true;
}

bool FileTraceSink::flushPendingBytes() noexcept {
    if (pendingBytes_ == 0U) {
        return true;
    }
    if (publisher_ == nullptr || failed_ || finalized_) {
        return false;
    }
    try {
        const auto bytes =
            std::span{reinterpret_cast<const std::byte*>(writeBuffer_.data()), pendingBytes_};
        if (publisher_->write(bytes)) {
            pendingBytes_ = 0U;
            return true;
        }
    } catch (...) {
    }
    failed_ = true;
    return false;
}

bool FileTraceSink::append(const application::TraceEvent& event) noexcept {
    if (!ensureOpen()) {
        return false;
    }
    char buffer[512];
    const int written = formatEventJson(buffer, sizeof(buffer), event);
    if (written <= 0) {
        failed_ = true;
        return false;
    }
    return writeBytes(buffer, static_cast<std::size_t>(written));
}

bool FileTraceSink::recordOverflow(const std::uint64_t lostCount) noexcept {
    if (lostCount == 0U) {
        return true;
    }
    if (!ensureOpen()) {
        return false;
    }
    char buffer[64];
    const int written = formatOverflowJson(buffer, sizeof(buffer), lostCount);
    if (written <= 0 || !writeBytes(buffer, static_cast<std::size_t>(written))) {
        failed_ = true;
        return false;
    }
    overflow_ += lostCount;
    return true;
}

bool FileTraceSink::finalize() noexcept {
    if (finalized_) {
        return !failed_;
    }
    if (failed_) {
        finalized_ = true;
        publisher_.reset();
        return false;
    }
    if (publisher_ == nullptr) {
        finalized_ = true;
        return true;
    }

    try {
        if (!flushPendingBytes() || !publisher_->flush()) {
            failed_ = true;
            finalized_ = true;
            publisher_.reset();
            return false;
        }
        const PlatformStatus published =
            replaceExisting_ ? publisher_->publishReplacingExisting() : publisher_->publishNew();
        if (!published) {
            failed_ = true;
            finalized_ = true;
            publisher_.reset();
            return false;
        }
        finalized_ = true;
        publisher_.reset();
        return true;
    } catch (...) {
        failed_ = true;
        finalized_ = true;
        publisher_.reset();
        return false;
    }
}

std::uint64_t FileTraceSink::overflowCount() const noexcept {
    return overflow_;
}

const std::filesystem::path& FileTraceSink::path() const noexcept {
    return path_;
}

} // namespace dvs::platform
