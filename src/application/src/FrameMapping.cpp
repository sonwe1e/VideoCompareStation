#include "dvs/application/FrameMapping.h"

#include <algorithm>

namespace dvs::application {

std::vector<SourceFrameOffset> resolveSourceFrameMappings(const FrameMappingContext& context,
                                                          const domain::FrameId canonicalFrame) {
    std::vector<SourceFrameOffset> mappings{context.offsets.begin(), context.offsets.end()};
    if (context.mode == AlignmentMode::Timestamp && context.canonicalTimeline.has_value() &&
        context.sources != nullptr) {
        const auto canonicalTimeRes =
            domain::canonicalFrameStartTime(*context.canonicalTimeline, canonicalFrame);
        if (canonicalTimeRes.hasValue()) {
            const auto canonicalTime = canonicalTimeRes.value();
            for (const domain::ComparisonSource& source : context.sources->sources()) {
                if (source.id == context.sources->canonicalSourceId()) {
                    continue;
                }
                SourceFrameOffset mapping{
                    .sourceId = source.id,
                    .frames = 0,
                    .matchKind = FrameMatchKind::Missing,
                    .confidence = 0.0F,
                };
                std::optional<domain::FrameId> timeMapped;
                const auto sourceTimelineIt =
                    std::find_if(context.timelines.begin(),
                                 context.timelines.end(),
                                 [&source](const SourceTimelineView& view) {
                                     return view.sourceId == source.id;
                                 });
                if (sourceTimelineIt != context.timelines.end() && sourceTimelineIt->timeline) {
                    // Parity with the CFR path below: a canonical time past the timeline's
                    // estimated end is Missing instead of a held last frame presented as
                    // a time match.
                    if (!canonicalTimeBeyondSourceTimeline(*sourceTimelineIt->timeline,
                                                           canonicalTime)) {
                        auto mapped = sourceTimelineIt->timeline->frameAtOrBefore(canonicalTime);
                        if (mapped) {
                            timeMapped = mapped.value();
                        }
                    }
                } else if (source.descriptor.frameRate.has_value()) {
                    auto mapped = source.descriptor.frameRate->frameAtOrBefore(canonicalTime);
                    if (mapped) {
                        timeMapped = mapped.value();
                    }
                }
                if (timeMapped.has_value()) {
                    const auto manualIt = std::find_if(
                        context.offsets.begin(),
                        context.offsets.end(),
                        [&](const SourceFrameOffset& o) { return o.sourceId == source.id; });
                    const std::int64_t userOffset =
                        (manualIt != context.offsets.end()) ? manualIt->frames : 0;
                    const std::int64_t targetFrame = timeMapped->value() + userOffset;
                    if (targetFrame < 0 || (source.descriptor.frameCount.value > 0 &&
                                            targetFrame >= source.descriptor.frameCount.value)) {
                        mapping.matchKind = FrameMatchKind::Missing;
                        mapping.frames = 0;
                    } else {
                        mapping.matchKind = FrameMatchKind::TimeAligned;
                        mapping.confidence = 1.0F;
                        mapping.frames = targetFrame - canonicalFrame.value();
                    }
                }
                const auto existing = std::find_if(
                    mappings.begin(), mappings.end(), [&source](const SourceFrameOffset& offset) {
                        return offset.sourceId == source.id;
                    });
                if (existing == mappings.end()) {
                    mappings.push_back(mapping);
                } else {
                    *existing = mapping;
                }
            }
        }
    } else {
        for (const SequenceAlignmentResult& map : context.sequenceMaps) {
            if (!canonicalFrame.isValid() ||
                static_cast<std::size_t>(canonicalFrame.value()) >= map.entries.size()) {
                continue;
            }
            const auto segment =
                std::find_if(map.segments.begin(),
                             map.segments.end(),
                             [canonicalFrame](const SequenceAlignmentSegment& value) {
                                 return value.firstCanonicalFrame <= canonicalFrame &&
                                        canonicalFrame <= value.lastCanonicalFrame;
                             });
            if (segment != map.segments.end() &&
                segment->state != AlignmentSegmentState::Accepted) {
                continue;
            }
            const SequenceAlignmentEntry& entry =
                map.entries[static_cast<std::size_t>(canonicalFrame.value())];
            if (entry.canonicalFrameId != canonicalFrame) {
                continue;
            }
            SourceFrameOffset mapping{
                .sourceId = map.sourceId,
                .frames = 0,
                .matchKind = entry.matchKind,
                .confidence = entry.confidence,
            };
            if (entry.sourceFrameId.has_value()) {
                mapping.frames = entry.sourceFrameId->value() - canonicalFrame.value();
            } else {
                mapping.matchKind = FrameMatchKind::Missing;
            }
            const auto existing = std::find_if(
                mappings.begin(), mappings.end(), [&map](const SourceFrameOffset& offset) {
                    return offset.sourceId == map.sourceId;
                });
            if (existing == mappings.end()) {
                mappings.push_back(mapping);
            } else {
                *existing = mapping;
            }
        }
    }
    if (context.sources != nullptr && context.mode == AlignmentMode::ManualAnchor) {
        for (const SourceAlignmentAnchors& anchors : context.anchors) {
            const domain::ComparisonSource* const source = context.sources->find(anchors.sourceId);
            if (source == nullptr) {
                continue;
            }
            const auto mapping =
                mapFrameWithAnchors(anchors, canonicalFrame, source->descriptor.frameCount.value);
            if (!mapping.has_value()) {
                continue;
            }
            const auto existing = std::find_if(
                mappings.begin(), mappings.end(), [&anchors](const SourceFrameOffset& offset) {
                    return offset.sourceId == anchors.sourceId;
                });
            if (existing == mappings.end()) {
                mappings.push_back(*mapping);
            } else {
                *existing = *mapping;
            }
        }
    }
    return mappings;
}

} // namespace dvs::application
