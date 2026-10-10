#pragma once

#include "dvs/domain/InterpolationProtocol.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace dvs::application {

// P03 interpolation-protocol manifest schema. Version 1 only; the persistence adapter rejects
// any other version explicitly instead of guessing a compatibility mapping.
inline constexpr std::int64_t kInterpolationProtocolSchemaVersion = 1;

struct InterpolationProtocolIoResult final {
    bool ok = false;
    // Set when ok == false. Distinguishes file-read failure, JSON parse failure, schema
    // rejection (unknown version/kind), and domain validation failure.
    std::string error;
    // Present only when ok == true. A failed load never carries a partial protocol.
    std::optional<domain::InterpolationProtocol> protocol;
};

// Import port for frame-provenance manifests: JSON document -> domain::InterpolationProtocol.
// Implemented by the persistence adapter and constructed only by the composition root, exactly
// like IIssueRecordRepository. Implementations must be safe to call from a worker thread
// (const, no shared mutable state) because the UI controller loads off the GUI thread.
class IInterpolationProtocolRepository {
public:
    virtual ~IInterpolationProtocolRepository() = default;

    // Reads and decodes the whole document before returning; a malformed document yields
    // ok == false with no partial protocol.
    [[nodiscard]] virtual InterpolationProtocolIoResult
    load(const std::filesystem::path& path) const = 0;
};

} // namespace dvs::application
