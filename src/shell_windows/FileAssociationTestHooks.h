#pragma once

#include <atomic>
#include <cstdint>

namespace dvs::shell::testing {

// Private deterministic seam for the file-association transaction. Real per-user registry writes
// cannot be made to fail on demand, and the rollback contract - a value write that fails midway
// leaves the previous registration exactly as it was - has to be provable in a test. While a
// scope holds an index, that one-based staged write of the next registry transaction fails with
// ERROR_ACCESS_DENIED; the scope's destructor always clears the seam again.
class ScopedFileAssociationWriteFailure final {
public:
    // Fails the writeIndex-th staged value write of each subsequent transaction until the
    // scope ends; zero is refused because it would mean "never fail" and silently test nothing.
    explicit ScopedFileAssociationWriteFailure(std::uint32_t writeIndex) noexcept;
    ~ScopedFileAssociationWriteFailure();

    ScopedFileAssociationWriteFailure(const ScopedFileAssociationWriteFailure&) = delete;
    ScopedFileAssociationWriteFailure& operator=(const ScopedFileAssociationWriteFailure&) = delete;

private:
    bool installed_{false};
};

// The index installed by the scope above, read by the transaction before every staged write.
inline std::atomic<std::uint32_t> fileAssociationFailingWriteIndex{0U};

} // namespace dvs::shell::testing
