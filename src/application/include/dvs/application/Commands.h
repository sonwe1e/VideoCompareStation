#pragma once

#include "dvs/application/AlignmentCommands.h"
#include "dvs/application/PlaybackCommands.h"
#include "dvs/application/SessionCommands.h"

#include <variant>

namespace dvs::application {

// Compatibility aggregate and the coordinator's single immutable command envelope.
using PlaybackCommand = std::variant<OpenComparisonCommand,
                                     OpenDirectComparisonCommand,
                                     SeekFrameCommand,
                                     StepFramesCommand,
                                     FirstFrameCommand,
                                     LastFrameCommand,
                                     PlayCommand,
                                     PauseCommand,
                                     SetPlaybackRateCommand,
                                     SetPlaybackRangeCommand,
                                     StartRangePlaybackCommand,
                                     SetAlignmentOffsetsCommand,
                                     EstimateAlignmentCommand,
                                     AnalyzeSequenceAlignmentCommand,
                                     CancelAlignmentAnalysisCommand,
                                     ConfirmAutomaticAlignmentCommand,
                                     UndoAutomaticAlignmentCommand,
                                     RestoreSequenceAlignmentCommand,
                                     SetManualAlignmentAnchorCommand,
                                     ClearManualAlignmentAnchorsCommand,
                                     CloseSessionCommand>;

[[nodiscard]] inline const CommandContext& commandContext(const PlaybackCommand& command) noexcept {
    return std::visit([](const auto& value) -> const CommandContext& { return value.context; },
                      command);
}

} // namespace dvs::application
