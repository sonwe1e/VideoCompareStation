#pragma once

#include "dvs/application/Alignment.h"
#include "dvs/application/RequestContext.h"

#include <memory>
#include <vector>

namespace dvs::application {

struct SetAlignmentOffsetsCommand final {
    CommandContext context;
    std::vector<SourceFrameOffset> sourceOffsets;
};

struct EstimateAlignmentCommand final {
    CommandContext context;
};

struct AnalyzeSequenceAlignmentCommand final {
    CommandContext context;
};

struct CancelAlignmentAnalysisCommand final {
    CommandContext context;
};

struct ConfirmAutomaticAlignmentCommand final {
    CommandContext context;
};

struct UndoAutomaticAlignmentCommand final {
    CommandContext context;
};

struct RestoreSequenceAlignmentCommand final {
    CommandContext context;
    std::shared_ptr<const std::vector<SequenceAlignmentResult>> sequenceResults;
};

struct SetManualAlignmentAnchorCommand final {
    CommandContext context;
    domain::SourceId sourceId = 0;
    ManualAlignmentAnchor anchor;
};

struct ClearManualAlignmentAnchorsCommand final {
    CommandContext context;
};

struct SetAlignmentModeCommand final {
    CommandContext context;
    AlignmentMode mode = AlignmentMode::FrameIndex;
};

} // namespace dvs::application
