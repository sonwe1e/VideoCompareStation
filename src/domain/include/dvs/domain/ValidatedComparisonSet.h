#pragma once

#include "dvs/domain/ComparisonSource.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dvs::domain {

// An immutable set of 1-3 validated sources sharing one canonical timeline definition.
// C-01: the timeline master (canonicalSourceId) is independent of the comparison Reference
// role. Canonical defines frame positions through its rational rate (when CFR) and its
// display-order frame count. Default master is the first source in session order; assigning
// Reference does not rebuild or reassign the timeline.
class ValidatedComparisonSet final {
public:
    [[nodiscard]] std::span<const ComparisonSource> sources() const noexcept;
    [[nodiscard]] std::size_t sourceCount() const noexcept;
    [[nodiscard]] const ComparisonSource* find(SourceId id) const noexcept;
    // Timeline master. Stable across Reference role changes.
    [[nodiscard]] SourceId canonicalSourceId() const noexcept;
    [[nodiscard]] SourceId timelineMasterSourceId() const noexcept;
    // Comparison baseline only — does not own the timeline.
    [[nodiscard]] std::optional<SourceId> referenceSourceId() const noexcept;
    [[nodiscard]] const MediaDescriptor& canonicalDescriptor() const noexcept;
    [[nodiscard]] const std::optional<RationalRate>& canonicalRate() const noexcept;
    [[nodiscard]] std::int64_t canonicalFrameCount() const noexcept;
    [[nodiscard]] bool hasEstimatedFrameCount() const noexcept;

private:
    friend class ComparisonValidator;

    ValidatedComparisonSet(std::vector<ComparisonSource> sources,
                           SourceId timelineMasterSourceId,
                           std::optional<SourceId> referenceSourceId);

    std::vector<ComparisonSource> sources_;
    SourceId timelineMasterSourceId_ = 0;
    std::optional<SourceId> referenceSourceId_;
};

} // namespace dvs::domain
