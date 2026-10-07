#include "dvs/platform/AtomicFilePublisher.h"

#ifndef _WIN32
#error "AtomicFilePublisher is only supported on Windows."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "dvs/platform/WindowsPaths.h"

#include "AtomicFilePublisherTestHooks.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <string_view>
#include <utility>
#include <windows.h>

namespace dvs::platform {
namespace {

constexpr std::size_t kMaximumIdentityTokenLength = 96;
constexpr std::uint32_t kTemporaryNameAttempts = 128;

std::atomic<std::uint64_t> temporarySequence{0};
std::atomic<testing::ReplaceFileCallback> replaceFileCallback{&ReplaceFileW};
std::atomic<testing::MoveFileExCallback> moveFileExCallback{&MoveFileExW};
std::atomic<testing::FlushFileBuffersCallback> flushFileBuffersCallback{&FlushFileBuffers};
std::atomic<testing::CloseHandleCallback> closeHandleCallback{&CloseHandle};

[[nodiscard]] bool isSafeIdentityToken(const std::string_view token) noexcept {
    if (token.empty() || token.size() > kMaximumIdentityTokenLength) {
        return false;
    }

    return std::all_of(token.begin(), token.end(), [](const unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' || character == '_';
    });
}

[[nodiscard]] std::wstring toWideAscii(const std::string_view value) {
    return {value.begin(), value.end()};
}

[[nodiscard]] std::string systemFailure(const char* const api, const DWORD errorCode) {
    return std::string{api} + " failed with Windows error " + std::to_string(errorCode) + ".";
}

[[nodiscard]] PlatformError
makeError(const PlatformErrorCode code, std::filesystem::path path, std::string technicalDetail) {
    return PlatformError{
        .code = code,
        .path = std::move(path),
        .technicalDetail = std::move(technicalDetail),
    };
}

[[nodiscard]] PlatformStatus invalidState(const std::filesystem::path& path) {
    return PlatformStatus::failure(
        makeError(PlatformErrorCode::kInvalidState, path, "Operation is not valid in this state."));
}

[[nodiscard]] bool isCollisionError(const DWORD errorCode) noexcept {
    return errorCode == ERROR_FILE_EXISTS || errorCode == ERROR_ALREADY_EXISTS;
}

[[nodiscard]] bool isMissingFileError(const DWORD errorCode) noexcept {
    return errorCode == ERROR_FILE_NOT_FOUND || errorCode == ERROR_PATH_NOT_FOUND;
}

[[nodiscard]] BOOL invokeReplaceFile(const LPCWSTR destination,
                                     const LPCWSTR replacement,
                                     const LPCWSTR backup,
                                     const DWORD flags) noexcept {
    return replaceFileCallback.load(std::memory_order_acquire)(
        destination, replacement, backup, flags, nullptr, nullptr);
}

[[nodiscard]] BOOL
invokeMoveFileEx(const LPCWSTR source, const LPCWSTR destination, const DWORD flags) noexcept {
    return moveFileExCallback.load(std::memory_order_acquire)(source, destination, flags);
}

[[nodiscard]] BOOL invokeCloseHandle(const HANDLE handle) noexcept {
    return closeHandleCallback.load(std::memory_order_acquire)(handle);
}

[[nodiscard]] std::string describePath(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return "'" + std::string{reinterpret_cast<const char*>(utf8.data()), utf8.size()} + "'";
}

} // namespace

class AtomicFilePublisher::Impl final {
public:
    enum class State {
        kOpen,
        kExternalWrite,
        kFlushed,
        kPublished,
        kAbandoned,
        kRecoveryRequired,
    };

    Impl(const HANDLE handle,
         std::filesystem::path destination,
         std::filesystem::path temporary,
         std::filesystem::path backup) noexcept
        : handle(handle), destination(std::move(destination)), temporary(std::move(temporary)),
          backup(std::move(backup)) {}

    ~Impl() {
        if (handle != INVALID_HANDLE_VALUE) {
            static_cast<void>(invokeCloseHandle(handle));
        }

        if (state == State::kRecoveryRequired) {
            return;
        }

        if (!temporary.empty()) {
            DeleteFileW(temporary.c_str());
        }

        if (ownsBackup && !backup.empty()) {
            DeleteFileW(backup.c_str());
        }
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
    std::filesystem::path destination;
    std::filesystem::path temporary;
    std::filesystem::path backup;
    bool ownsBackup = false;
    State state = State::kOpen;
};

PlatformResult<std::unique_ptr<AtomicFilePublisher>>
AtomicFilePublisher::begin(const std::filesystem::path& destination,
                           TemporaryFileIdentity identity) {
    if (!isSafeIdentityToken(identity.operation) || !isSafeIdentityToken(identity.ownerId)) {
        return PlatformResult<std::unique_ptr<AtomicFilePublisher>>::failure(
            makeError(PlatformErrorCode::kInvalidTransactionIdentity,
                      destination,
                      "Operation and ownerId must contain only ASCII letters, digits, hyphens, or "
                      "underscores."));
    }

    const auto absoluteResult = WindowsPaths::absolutePath(destination);
    if (!absoluteResult) {
        return PlatformResult<std::unique_ptr<AtomicFilePublisher>>::failure(
            absoluteResult.error());
    }

    const std::filesystem::path absoluteDestination = absoluteResult.value();
    const std::filesystem::path parent = absoluteDestination.parent_path();
    if (parent.empty() || absoluteDestination.filename().empty()) {
        return PlatformResult<std::unique_ptr<AtomicFilePublisher>>::failure(
            makeError(PlatformErrorCode::kInvalidPath,
                      absoluteDestination,
                      "Destination must name a file in an existing directory."));
    }

    const DWORD attributes = GetFileAttributesW(parent.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
        const DWORD errorCode =
            attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_PATH_NOT_FOUND;
        return PlatformResult<std::unique_ptr<AtomicFilePublisher>>::failure(
            makeError(PlatformErrorCode::kDirectoryUnavailable,
                      parent,
                      systemFailure("GetFileAttributesW", errorCode)));
    }

    const std::wstring operation = toWideAscii(identity.operation);
    const std::wstring ownerId = toWideAscii(identity.ownerId);
    const std::wstring destinationName = absoluteDestination.filename().wstring();
    const std::wstring revision = std::to_wstring(identity.revision);

    for (std::uint32_t attempt = 0; attempt < kTemporaryNameAttempts; ++attempt) {
        const std::uint64_t sequence =
            temporarySequence.fetch_add(1, std::memory_order_relaxed) + 1U;
        const std::wstring temporaryName =
            L"." + destinationName + L".dvs-" + operation + L"-" + ownerId + L"-" + revision +
            L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence) + L".partial";
        const std::filesystem::path temporary = parent / temporaryName;
        std::filesystem::path backup = temporary;
        backup += L".backup";

        const HANDLE handle = CreateFileW(temporary.c_str(),
                                          GENERIC_WRITE,
                                          0,
                                          nullptr,
                                          CREATE_NEW,
                                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                          nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            const DWORD backupAttributes = GetFileAttributesW(backup.c_str());
            if (backupAttributes != INVALID_FILE_ATTRIBUTES) {
                if (!CloseHandle(handle)) {
                    const DWORD cleanupError = GetLastError();
                    return PlatformResult<std::unique_ptr<AtomicFilePublisher>>::failure(
                        makeError(PlatformErrorCode::kTemporaryFileCreateFailed,
                                  temporary,
                                  systemFailure("CloseHandle", cleanupError)));
                }
                if (!DeleteFileW(temporary.c_str())) {
                    const DWORD cleanupError = GetLastError();
                    return PlatformResult<std::unique_ptr<AtomicFilePublisher>>::failure(
                        makeError(PlatformErrorCode::kTemporaryFileCreateFailed,
                                  temporary,
                                  systemFailure("DeleteFileW", cleanupError)));
                }
                continue;
            }

            const DWORD backupCheckError = GetLastError();
            if (backupCheckError != ERROR_FILE_NOT_FOUND) {
                if (!CloseHandle(handle)) {
                    const DWORD cleanupError = GetLastError();
                    return PlatformResult<std::unique_ptr<AtomicFilePublisher>>::failure(
                        makeError(PlatformErrorCode::kTemporaryFileCreateFailed,
                                  temporary,
                                  systemFailure("CloseHandle", cleanupError)));
                }
                if (!DeleteFileW(temporary.c_str())) {
                    const DWORD cleanupError = GetLastError();
                    return PlatformResult<std::unique_ptr<AtomicFilePublisher>>::failure(
                        makeError(PlatformErrorCode::kTemporaryFileCreateFailed,
                                  temporary,
                                  systemFailure("DeleteFileW", cleanupError)));
                }
                return PlatformResult<std::unique_ptr<AtomicFilePublisher>>::failure(
                    makeError(PlatformErrorCode::kTemporaryFileCreateFailed,
                              backup,
                              systemFailure("GetFileAttributesW", backupCheckError)));
            }

            auto impl =
                std::make_unique<Impl>(handle, absoluteDestination, temporary, std::move(backup));
            auto publisher =
                std::unique_ptr<AtomicFilePublisher>(new AtomicFilePublisher(std::move(impl)));
            return PlatformResult<std::unique_ptr<AtomicFilePublisher>>::success(
                std::move(publisher));
        }

        const DWORD errorCode = GetLastError();
        if (!isCollisionError(errorCode)) {
            return PlatformResult<std::unique_ptr<AtomicFilePublisher>>::failure(
                makeError(PlatformErrorCode::kTemporaryFileCreateFailed,
                          temporary,
                          systemFailure("CreateFileW", errorCode)));
        }
    }

    return PlatformResult<std::unique_ptr<AtomicFilePublisher>>::failure(
        makeError(PlatformErrorCode::kTemporaryFileCreateFailed,
                  absoluteDestination,
                  "Could not allocate a unique same-directory temporary file name."));
}

AtomicFilePublisher::AtomicFilePublisher(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

AtomicFilePublisher::~AtomicFilePublisher() {
    static_cast<void>(abandon());
}

PlatformStatus AtomicFilePublisher::write(const std::span<const std::byte> bytes) {
    if (impl_->state != Impl::State::kOpen) {
        return invalidState(impl_->temporary);
    }

    std::size_t remaining = bytes.size();
    const std::byte* cursor = bytes.data();
    while (remaining != 0U) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!WriteFile(impl_->handle, cursor, chunk, &written, nullptr)) {
            const DWORD errorCode = GetLastError();
            return PlatformStatus::failure(makeError(PlatformErrorCode::kWriteFailed,
                                                     impl_->temporary,
                                                     systemFailure("WriteFile", errorCode)));
        }

        if (written == 0U) {
            return PlatformStatus::failure(
                makeError(PlatformErrorCode::kWriteFailed,
                          impl_->temporary,
                          "WriteFile completed without writing the requested bytes."));
        }

        cursor += written;
        remaining -= written;
    }

    return PlatformStatus::success();
}

PlatformStatus AtomicFilePublisher::prepareForExternalWrite() {
    if (impl_->state != Impl::State::kOpen) {
        return invalidState(impl_->temporary);
    }
    if (!invokeCloseHandle(impl_->handle)) {
        return PlatformStatus::failure(makeError(PlatformErrorCode::kCloseFailed,
                                                 impl_->temporary,
                                                 systemFailure("CloseHandle", GetLastError())));
    }
    impl_->handle = INVALID_HANDLE_VALUE;
    impl_->state = Impl::State::kExternalWrite;
    return PlatformStatus::success();
}

PlatformStatus AtomicFilePublisher::flush() {
    if (impl_->state != Impl::State::kOpen && impl_->state != Impl::State::kExternalWrite) {
        return invalidState(impl_->temporary);
    }
    if (impl_->handle == INVALID_HANDLE_VALUE) {
        // Do not recreate a missing external output as an empty successful file.
        impl_->handle = CreateFileW(impl_->temporary.c_str(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                    nullptr);
        if (impl_->handle == INVALID_HANDLE_VALUE) {
            return PlatformStatus::failure(makeError(PlatformErrorCode::kFlushFailed,
                                                     impl_->temporary,
                                                     systemFailure("CreateFileW", GetLastError())));
        }
    }

    if (!flushFileBuffersCallback.load(std::memory_order_acquire)(impl_->handle)) {
        const DWORD errorCode = GetLastError();
        return PlatformStatus::failure(makeError(PlatformErrorCode::kFlushFailed,
                                                 impl_->temporary,
                                                 systemFailure("FlushFileBuffers", errorCode)));
    }

    if (!invokeCloseHandle(impl_->handle)) {
        const DWORD errorCode = GetLastError();
        return PlatformStatus::failure(makeError(PlatformErrorCode::kCloseFailed,
                                                 impl_->temporary,
                                                 systemFailure("CloseHandle", errorCode)));
    }

    impl_->handle = INVALID_HANDLE_VALUE;
    impl_->state = Impl::State::kFlushed;
    return PlatformStatus::success();
}

PlatformStatus AtomicFilePublisher::publishReplacingExisting() {
    if (impl_->state != Impl::State::kFlushed) {
        return invalidState(impl_->temporary);
    }

    if (!invokeReplaceFile(
            impl_->destination.c_str(), impl_->temporary.c_str(), impl_->backup.c_str(), 0)) {
        const DWORD errorCode = GetLastError();
        if (errorCode == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
            // ReplaceFileW has moved the old destination to backup and retained the replacement
            // at its original temporary path. Restore the old target without overwriting any
            // concurrently recreated destination.
            impl_->ownsBackup = true;
            if (invokeMoveFileEx(
                    impl_->backup.c_str(), impl_->destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
                impl_->ownsBackup = false;
                return PlatformStatus::failure(
                    makeError(PlatformErrorCode::kReplaceFailed,
                              impl_->destination,
                              systemFailure("ReplaceFileW", errorCode) +
                                  " The original destination was restored from " +
                                  describePath(impl_->backup) + "."));
            }

            const DWORD recoveryError = GetLastError();
            impl_->state = Impl::State::kRecoveryRequired;
            return PlatformStatus::failure(makeError(
                PlatformErrorCode::kRecoveryRequired,
                impl_->destination,
                systemFailure("ReplaceFileW", errorCode) + " The original target is retained at " +
                    describePath(impl_->backup) + " and the replacement is retained at " +
                    describePath(impl_->temporary) + "; automatic restoration failed: " +
                    systemFailure("MoveFileExW", recoveryError)));
        }

        const PlatformErrorCode code = isMissingFileError(errorCode)
                                           ? PlatformErrorCode::kTargetMissing
                                           : PlatformErrorCode::kReplaceFailed;
        return PlatformStatus::failure(
            makeError(code, impl_->destination, systemFailure("ReplaceFileW", errorCode)));
    }

    impl_->ownsBackup = true;
    impl_->state = Impl::State::kPublished;
    return PlatformStatus::success();
}

PlatformStatus AtomicFilePublisher::publishNew() {
    if (impl_->state != Impl::State::kFlushed) {
        return invalidState(impl_->temporary);
    }

    if (!invokeMoveFileEx(
            impl_->temporary.c_str(), impl_->destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD errorCode = GetLastError();
        const PlatformErrorCode code = isCollisionError(errorCode)
                                           ? PlatformErrorCode::kTargetAlreadyExists
                                           : PlatformErrorCode::kPublishFailed;
        return PlatformStatus::failure(
            makeError(code, impl_->destination, systemFailure("MoveFileExW", errorCode)));
    }

    impl_->state = Impl::State::kPublished;
    return PlatformStatus::success();
}

PlatformStatus AtomicFilePublisher::abandon() {
    if (impl_->state == Impl::State::kAbandoned) {
        return PlatformStatus::success();
    }

    if (impl_->state == Impl::State::kRecoveryRequired) {
        return PlatformStatus::failure(makeError(
            PlatformErrorCode::kRecoveryRequired,
            impl_->destination,
            "Recovery artifacts are intentionally retained at " + describePath(impl_->backup) +
                " and " + describePath(impl_->temporary) + "."));
    }

    if (impl_->handle != INVALID_HANDLE_VALUE) {
        if (!invokeCloseHandle(impl_->handle)) {
            const DWORD errorCode = GetLastError();
            return PlatformStatus::failure(makeError(PlatformErrorCode::kCleanupFailed,
                                                     impl_->temporary,
                                                     systemFailure("CloseHandle", errorCode)));
        }
        impl_->handle = INVALID_HANDLE_VALUE;
    }

    if (!DeleteFileW(impl_->temporary.c_str())) {
        const DWORD errorCode = GetLastError();
        if (!isMissingFileError(errorCode)) {
            return PlatformStatus::failure(makeError(PlatformErrorCode::kCleanupFailed,
                                                     impl_->temporary,
                                                     systemFailure("DeleteFileW", errorCode)));
        }
    }

    if (impl_->ownsBackup && !DeleteFileW(impl_->backup.c_str())) {
        const DWORD errorCode = GetLastError();
        if (!isMissingFileError(errorCode)) {
            return PlatformStatus::failure(makeError(PlatformErrorCode::kCleanupFailed,
                                                     impl_->backup,
                                                     systemFailure("DeleteFileW", errorCode)));
        }
    }

    impl_->state = Impl::State::kAbandoned;
    return PlatformStatus::success();
}

const std::filesystem::path& AtomicFilePublisher::destinationPath() const noexcept {
    return impl_->destination;
}

const std::filesystem::path& AtomicFilePublisher::temporaryPath() const noexcept {
    return impl_->temporary;
}

const std::filesystem::path& AtomicFilePublisher::recoveryBackupPath() const noexcept {
    return impl_->backup;
}

namespace testing {

ScopedAtomicFilePublisherApiOverride::ScopedAtomicFilePublisherApiOverride(
    const ReplaceFileCallback replaceFile,
    const MoveFileExCallback moveFileEx,
    const FlushFileBuffersCallback flushFileBuffers,
    const CloseHandleCallback closeHandle) noexcept
    : previousReplaceFile_(replaceFileCallback.exchange(
          replaceFile == nullptr ? &ReplaceFileW : replaceFile, std::memory_order_acq_rel)),
      previousMoveFileEx_(moveFileExCallback.exchange(
          moveFileEx == nullptr ? &MoveFileExW : moveFileEx, std::memory_order_acq_rel)),
      previousFlushFileBuffers_(flushFileBuffersCallback.exchange(
          flushFileBuffers == nullptr ? &FlushFileBuffers : flushFileBuffers,
          std::memory_order_acq_rel)),
      previousCloseHandle_(closeHandleCallback.exchange(
          closeHandle == nullptr ? &CloseHandle : closeHandle, std::memory_order_acq_rel)) {}

ScopedAtomicFilePublisherApiOverride::~ScopedAtomicFilePublisherApiOverride() {
    closeHandleCallback.store(previousCloseHandle_, std::memory_order_release);
    flushFileBuffersCallback.store(previousFlushFileBuffers_, std::memory_order_release);
    moveFileExCallback.store(previousMoveFileEx_, std::memory_order_release);
    replaceFileCallback.store(previousReplaceFile_, std::memory_order_release);
}

} // namespace testing

} // namespace dvs::platform
