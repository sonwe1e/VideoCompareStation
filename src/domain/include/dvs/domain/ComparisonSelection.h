#pragma once

#include "dvs/domain/ComparisonSource.h"
#include "dvs/domain/Identifiers.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace dvs::domain {

// C-02: active comparison pair is expressed as stable source identities, not A/B/C slots.
struct ComparisonPair final {
    SourceId first = 0;
    SourceId second = 0;

    [[nodiscard]] constexpr bool operator==(const ComparisonPair&) const noexcept = default;
    [[nodiscard]] constexpr bool contains(const SourceId id) const noexcept {
        return id == first || id == second;
    }
    [[nodiscard]] constexpr bool isValid() const noexcept {
        return first != second;
    }
};

// Preferences store only the policy; the session stores the resolved ComparisonPair.
enum class DefaultPairPolicy : std::uint8_t {
    // Baseline + first non-reference candidate (or first two when no reference).
    ReferenceAndFirstCandidate = 0,
    // Last two sources in session order.
    LastTwoActiveSources = 1,
    // Keep the previous session pair when both members remain; else LastTwoActiveSources.
    PreserveIfAvailable = 2,
};

// Resolves the effective pair for the current source list. Preferred is used when the policy
// can honor it (PreserveIfAvailable) or when both members are still loaded.
[[nodiscard]] ComparisonPair resolveComparisonPair(std::span<const ComparisonSource> sources,
                                                   std::optional<SourceId> referenceId,
                                                   std::optional<ComparisonPair> preferred,
                                                   DefaultPairPolicy policy) noexcept;

// Projects a pair onto the compact renderer DifferenceEdge ordinal (0↔1 / 0↔2 / 1↔2).
// Returns nullopt when either member is missing or the pair is not two distinct loaded sources.
[[nodiscard]] std::optional<std::uint8_t>
comparisonPairEdgeOrdinal(std::span<const ComparisonSource> sources,
                          const ComparisonPair& pair) noexcept;

} // namespace dvs::domain
