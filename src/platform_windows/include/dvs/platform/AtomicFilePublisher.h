#pragma once

#include "dvs/platform/PlatformResult.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace dvs::platform {

// The generated partial name embeds the operation, durable owner identity, and revision/attempt.
// This lets recovery code target only files owned by its project or job rather than broad scans.
struct TemporaryFileIdentity final {
    std::string operation;
    std::string ownerId;
    std::uint64_t revision = 0;
};

// Owns a same-directory temporary file. The public interface deliberately contains no HANDLE,
// DWORD, or other Win32 type. A flushed temporary can be atomically published once.
class AtomicFilePublisher final {
public:
    [[nodiscard]] static PlatformResult<std::unique_ptr<AtomicFilePublisher>>
    begin(const std::filesystem::path& destination, TemporaryFileIdentity identity);

    ~AtomicFilePublisher();

    AtomicFilePublisher(const AtomicFilePublisher&) = delete;
    AtomicFilePublisher& operator=(const AtomicFilePublisher&) = delete;
    AtomicFilePublisher(AtomicFilePublisher&&) = delete;
    AtomicFilePublisher& operator=(AtomicFilePublisher&&) = delete;

    [[nodiscard]] PlatformStatus write(std::span<const std::byte> bytes);

    // Releases the owned temporary's handle for a seekable external writer. The caller must
    // exclusively own this transaction and close that writer before flush() reopens the file.
    // No publish is allowed until flush succeeds; abandonment still removes only owned artifacts.
    [[nodiscard]] PlatformStatus prepareForExternalWrite();
    [[nodiscard]] PlatformStatus flush();

    // Uses ReplaceFileW and therefore fails if destination does not exist at publish time. A
    // same-directory backup is used to make documented partial ReplaceFileW failures recoverable.
    [[nodiscard]] PlatformStatus publishReplacingExisting();

    // Uses a same-volume rename without replacement and therefore fails if a target already
    // exists. Callers must allocate a new name rather than silently overwriting user output.
    [[nodiscard]] PlatformStatus publishNew();

    // Deletes owned temporary/backup artifacts when safe. It deliberately refuses to discard
    // artifacts retained after an unrecoverable partial replacement.
    [[nodiscard]] PlatformStatus abandon();

    [[nodiscard]] const std::filesystem::path& destinationPath() const noexcept;
    [[nodiscard]] const std::filesystem::path& temporaryPath() const noexcept;

    // Meaningful only when publishReplacingExisting() reports recovery_required. The backup is
    // retained in the destination directory with the original target contents.
    [[nodiscard]] const std::filesystem::path& recoveryBackupPath() const noexcept;

private:
    class Impl;

    explicit AtomicFilePublisher(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::platform
