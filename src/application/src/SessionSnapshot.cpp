#include "dvs/application/SessionSnapshot.h"

#include <algorithm>
#include <cstddef>

namespace dvs::application {

bool SessionSnapshot::isConsistent() const noexcept {
    // Device availability is independent of session state: it may become ready before opening a
    // pair and may be revoked after a pair has been presented. The coordinator gates media
    // commands on graphicsReady while these structural snapshot checks remain valid during both
    // transitions.
    const auto validFrame = [this](const std::optional<domain::FrameId>& frame) {
        return !frame.has_value() ||
               (canonicalFrameCount != 0U && frame->isValid() &&
                static_cast<std::uint64_t>(frame->value()) < canonicalFrameCount);
    };
    if (!validFrame(displayedFrame) || !validFrame(requestedFrame)) {
        return false;
    }
    if (playbackRangeIn.has_value() != playbackRangeOut.has_value()) {
        return false;
    }
    if (playbackRangeIn.has_value()) {
        if (!playbackRangeIn->isValid() || !playbackRangeOut->isValid() ||
            playbackRangeIn->value() > playbackRangeOut->value()) {
            return false;
        }
        if (!validFrame(playbackRangeIn) || !validFrame(playbackRangeOut)) {
            return false;
        }
        if (playbackRangeLoop && !playbackRangeIn.has_value()) {
            return false;
        }
    } else if (playbackRangeLoop || playbackRangeLoopActive) {
        return false;
    }
    if (sources.size() > 3U) {
        return false;
    }
    for (std::size_t index = 0U; index < sources.size(); ++index) {
        if (sources[index].displayName.empty()) {
            return false;
        }
        for (std::size_t other = index + 1U; other < sources.size(); ++other) {
            if (sources[index].sourceId == sources[other].sourceId) {
                return false;
            }
        }
    }
    if (validatedComparison &&
        (sources.size() != validatedComparison->sourceCount() ||
         canonicalFrameCount !=
             static_cast<std::uint64_t>(validatedComparison->canonicalFrameCount()))) {
        return false;
    }
    for (std::size_t index = 0U; index < alignmentOffsets.size(); ++index) {
        const SourceFrameOffset& offset = alignmentOffsets[index];
        const bool known =
            std::any_of(sources.begin(), sources.end(), [&offset](const SessionSourceView& source) {
                return source.sourceId == offset.sourceId;
            });
        const bool duplicate =
            std::any_of(alignmentOffsets.begin() + static_cast<std::ptrdiff_t>(index + 1U),
                        alignmentOffsets.end(),
                        [&offset](const SourceFrameOffset& other) {
                            return other.sourceId == offset.sourceId;
                        });
        if (!known || duplicate) {
            return false;
        }
    }
    if (compatibilityFindings.size() > kMaximumCompatibilityFindings) {
        return false;
    }
    for (const CompatibilityFindingView& finding : compatibilityFindings) {
        if (finding.sources.size() != 2U || finding.sources[0] == finding.sources[1]) {
            return false;
        }
        for (const domain::SourceId sourceId : finding.sources) {
            if (!std::any_of(
                    sources.begin(), sources.end(), [sourceId](const SessionSourceView& source) {
                        return source.sourceId == sourceId;
                    })) {
                return false;
            }
        }
    }
    for (std::size_t index = 0U; index < presentedSources.size(); ++index) {
        const PresentedSourceState& source = presentedSources[index];
        const bool sourceKnown =
            std::any_of(sources.begin(), sources.end(), [&source](const SessionSourceView& value) {
                return value.sourceId == source.sourceId;
            });
        if ((source.matchKind == FrameMatchKind::Missing) != !source.sourceFrameId.has_value() ||
            (source.sourceFrameId.has_value() && !source.sourceFrameId->isValid()) ||
            (source.matchKind == FrameMatchKind::Missing) != source.missingReason.has_value() ||
            (!sources.empty() && !sourceKnown)) {
            return false;
        }
        for (std::size_t other = index + 1U; other < presentedSources.size(); ++other) {
            if (presentedSources[other].sourceId == source.sourceId) {
                return false;
            }
        }
    }
    if (!displayedFrame.has_value() && !presentedSources.empty()) {
        return false;
    }
    if (alignmentAnalysisJobId.has_value() != alignmentAnalysisKind.has_value() ||
        (alignmentAnalysisJobId.has_value() && alignmentAnalysisJobId->value == 0U) ||
        (!alignmentAnalysisJobId.has_value() &&
         (alignmentAnalysisPhase.has_value() || alignmentAnalysisCompletedUnits != 0U ||
          alignmentAnalysisWork.totalUnits != 0U || !alignmentAnalysisWork.unitName.empty())) ||
        (alignmentAnalysisCompletedUnits > alignmentAnalysisWork.totalUnits) ||
        (alignmentAnalysisPhase.has_value() && alignmentAnalysisWork.totalUnits == 0U)) {
        return false;
    }
    if (canConfirmAutomaticAlignment && !automaticAlignmentPending) {
        return false;
    }

    switch (sessionState) {
    case domain::SessionState::kEmpty:
        return playbackState == domain::PlaybackState::kPaused && !displayedFrame.has_value() &&
               !requestedFrame.has_value() && canonicalFrameCount == 0U && !alignmentRequired &&
               !automaticAlignmentPending && !canConfirmAutomaticAlignment &&
               !canUndoAutomaticAlignment && sources.empty() && !validatedComparison &&
               alignmentOffsets.empty() && compatibilityFindings.empty();
    case domain::SessionState::kLoading:
        return !displayedFrame.has_value() && !validatedComparison;
    case domain::SessionState::kReady:
        return canonicalFrameCount != 0U && !sources.empty();
    case domain::SessionState::kInvalid:
    case domain::SessionState::kError:
        return playbackState == domain::PlaybackState::kPaused;
    }
    return false;
}

} // namespace dvs::application
