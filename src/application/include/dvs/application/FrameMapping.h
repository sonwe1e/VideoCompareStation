#pragma once

#include "dvs/application/SessionSnapshot.h"

#include <span>
#include <vector>

namespace dvs::application {

// Shared by playback and analysis. Sequence maps are immutable and never copied per frame.
struct FrameMappingContext final {
    const domain::ValidatedComparisonSet* sources;
    const std::optional<domain::CanonicalTimeline>& canonicalTimeline;
    std::span<const SourceFrameOffset> offsets;
    AlignmentMode mode;
    std::span<const SequenceAlignmentResult> sequenceMaps;
    std::span<const SourceAlignmentAnchors> anchors;
    std::span<const SourceTimelineView> timelines;
};

[[nodiscard]] std::vector<SourceFrameOffset>
resolveSourceFrameMappings(const FrameMappingContext& context, domain::FrameId canonicalFrame);

} // namespace dvs::application
