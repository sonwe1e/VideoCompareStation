#pragma once

#include "dvs/application/Alignment.h"
#include "dvs/domain/ValidatedComparisonSet.h"

#include <span>

namespace dvs::application {

// Validates immutable derived mappings before they cross a persistence or restore boundary.
// Accepted caches may contain a subset of non-canonical sources because rejected segments keep
// their strict/global fallback instead of being silently accepted.
[[nodiscard]] bool
validateDerivedSequenceAlignments(const domain::ValidatedComparisonSet& sources,
                                  std::span<const SequenceAlignmentResult> results) noexcept;

} // namespace dvs::application
