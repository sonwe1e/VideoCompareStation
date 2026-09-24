#pragma once

#include "dvs/domain/ComparisonSource.h"

#include <cstdint>
#include <string>

namespace dvs::application {

struct SessionSnapshot;

enum class ComparisonExactness : std::uint8_t {
    ExactCodeValue,
    DisplaySpaceConverted,
    SpatiallyResampled,
    TemporallyAligned,
    Unavailable,
};

// Per-dimension decomposition of the same facts the single-enum projection summarizes.
// The enum can only name the highest-priority inexactness reason; the UI must present
// every dimension that applies (e.g. a pair can be temporally aligned *and* spatially
// resampled *and* display-space converted at once) instead of hiding the rest.
struct ComparisonExactnessDimensions final {
    bool available = false;
    // Both presented frames come from the same canonical frame index mapping.
    bool temporalExact = false;
    // Extent, rotation and sample aspect ratio match, so no spatial resampling occurs.
    bool spatialExact = false;
    // Pixel format, bit depth and color metadata match on a normalized-plane code path,
    // so the comparison is on original code values, not a display-space conversion.
    bool pixelExact = false;
    // Human-readable explanation when comparison cannot be exact.
    std::string inexactReason;
};

// Builds human-readable inexactness reasons, so it may allocate and is not noexcept.
[[nodiscard]] ComparisonExactnessDimensions comparisonExactnessDimensions(
    const SessionSnapshot& snapshot, domain::SourceId first, domain::SourceId second);

[[nodiscard]] ComparisonExactness comparisonExactness(const SessionSnapshot& snapshot,
                                                      domain::SourceId first,
                                                      domain::SourceId second) noexcept;

} // namespace dvs::application
