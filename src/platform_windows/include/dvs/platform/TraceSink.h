#pragma once

#include "dvs/application/PlaybackTrace.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace dvs::platform {

class AtomicFilePublisher;

// File-backed trace sink. Writes JSONL to a same-directory transaction when the trace buffer is
// drained, then atomically publishes the final path only after finalize() flushes and closes the
// complete transaction. All calls must come from one export thread, never a producer thread.
class FileTraceSink final : public application::ITraceSink {
public:
    explicit FileTraceSink(std::filesystem::path path);
    ~FileTraceSink() override;

    FileTraceSink(const FileTraceSink&) = delete;
    FileTraceSink& operator=(const FileTraceSink&) = delete;

    [[nodiscard]] bool append(const application::TraceEvent& event) noexcept override;
    [[nodiscard]] bool recordOverflow(std::uint64_t lostCount) noexcept override;
    [[nodiscard]] bool finalize() noexcept override;

    [[nodiscard]] std::uint64_t overflowCount() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    static constexpr std::size_t kWriteBufferCapacity = 64U * 1024U;

    [[nodiscard]] bool ensureOpen() noexcept;
    [[nodiscard]] bool writeHeader() noexcept;
    [[nodiscard]] bool writeBytes(const char* data, std::size_t size) noexcept;
    [[nodiscard]] bool flushPendingBytes() noexcept;

    std::filesystem::path path_;
    std::unique_ptr<AtomicFilePublisher> publisher_;
    std::array<char, kWriteBufferCapacity> writeBuffer_{};
    std::size_t pendingBytes_ = 0U;
    std::uint64_t overflow_ = 0U;
    bool headerWritten_ = false;
    bool failed_ = false;
    bool finalized_ = false;
    bool replaceExisting_ = false;
};

} // namespace dvs::platform
