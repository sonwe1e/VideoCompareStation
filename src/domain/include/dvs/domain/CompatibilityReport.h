#pragma once

#include "dvs/domain/ComparisonSource.h"
#include "dvs/domain/MediaError.h"

#include <span>
#include <vector>

namespace dvs::domain {

// Findings grade cross-source differences (USERPLAN section 3.3). Opening a session is refused
// only for per-source fatals, which surface as Result failures; a report therefore normally
// carries warnings and alignment-required notices. The fatal severity stays in the vocabulary so
// later alignment stages can escalate without a schema change.
enum class CompatibilitySeverity {
    kFatal,
    kWarning,
    kAlignmentRequired,
};

struct CompatibilityFinding final {
    CompatibilitySeverity severity = CompatibilitySeverity::kWarning;
    MediaErrorCode code = MediaErrorCode::kInvalidArgument;
    std::vector<SourceId> sources;
};

class CompatibilityReport final {
public:
    void add(CompatibilityFinding finding);

    [[nodiscard]] std::span<const CompatibilityFinding> findings() const noexcept;
    [[nodiscard]] bool isEmpty() const noexcept;
    [[nodiscard]] bool hasFatal() const noexcept;
    [[nodiscard]] bool hasAlignmentRequired() const noexcept;

private:
    std::vector<CompatibilityFinding> findings_;
};

} // namespace dvs::domain
