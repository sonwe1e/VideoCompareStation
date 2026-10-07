#pragma once

#ifndef _WIN32
#error "AtomicFilePublisher test hooks are only supported on Windows."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace dvs::platform::testing {

// This private seam lets component tests emulate documented ReplaceFileW partial failures that
// cannot be induced deterministically on a live filesystem. It is intentionally not installed as
// part of the platform adapter's public interface.
using ReplaceFileCallback = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
using MoveFileExCallback = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
using FlushFileBuffersCallback = BOOL(WINAPI*)(HANDLE);
using CloseHandleCallback = BOOL(WINAPI*)(HANDLE);

class ScopedAtomicFilePublisherApiOverride final {
public:
    ScopedAtomicFilePublisherApiOverride(ReplaceFileCallback replaceFile,
                                         MoveFileExCallback moveFileEx,
                                         FlushFileBuffersCallback flushFileBuffers = nullptr,
                                         CloseHandleCallback closeHandle = nullptr) noexcept;
    ~ScopedAtomicFilePublisherApiOverride();

    ScopedAtomicFilePublisherApiOverride(const ScopedAtomicFilePublisherApiOverride&) = delete;
    ScopedAtomicFilePublisherApiOverride&
    operator=(const ScopedAtomicFilePublisherApiOverride&) = delete;

private:
    ReplaceFileCallback previousReplaceFile_ = nullptr;
    MoveFileExCallback previousMoveFileEx_ = nullptr;
    FlushFileBuffersCallback previousFlushFileBuffers_ = nullptr;
    CloseHandleCallback previousCloseHandle_ = nullptr;
};

} // namespace dvs::platform::testing
