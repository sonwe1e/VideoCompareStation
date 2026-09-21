#pragma once

#include "dvs/domain/Identifiers.h"
#include "dvs/domain/MediaDescriptor.h"

#include <string>

namespace dvs::domain {

// Role is user-assigned comparison meaning, not timeline ownership (C-01). A session may run
// with zero references or exactly one; two references are rejected. Assigning Reference does
// not change which source owns the canonical timeline (session-order first, unless an open
// command names an explicit timeline master later).
enum class ComparisonRole {
    kReference,
    kPrediction,
};

struct ComparisonSource final {
    SourceId id = 0;
    ComparisonRole role = ComparisonRole::kPrediction;
    MediaDescriptor descriptor;
    std::string displayName;
};

} // namespace dvs::domain
