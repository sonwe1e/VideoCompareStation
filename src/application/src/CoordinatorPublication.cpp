#include "CoordinatorPublication.h"

#include <utility>

namespace dvs::application {

std::shared_ptr<const SessionSnapshot> CoordinatorPublication::snapshot() const {
    std::scoped_lock lock(mutex_);
    return snapshot_;
}

std::vector<CommandTerminal> CoordinatorPublication::takeCompletedCommands() {
    std::scoped_lock lock(mutex_);
    std::vector<CommandTerminal> terminals = std::move(completedCommands_);
    completedCommands_.clear();
    return terminals;
}

std::shared_ptr<const std::vector<SequenceAlignmentResult>>
CoordinatorPublication::acceptedSequenceAlignments() const {
    std::scoped_lock lock(mutex_);
    return sequenceAlignmentMaps_;
}

void CoordinatorPublication::publish(
    const SessionSnapshot& state,
    const std::vector<SequenceAlignmentResult>& sequenceAlignmentMaps) {
    auto snapshot = std::make_shared<SessionSnapshot>(state);

    std::scoped_lock lock(mutex_);
    if (sequenceAlignmentRevision_ != state.alignmentRevision) {
        sequenceAlignmentMaps_ =
            std::make_shared<const std::vector<SequenceAlignmentResult>>(sequenceAlignmentMaps);
        sequenceAlignmentRevision_ = state.alignmentRevision;
    }
    snapshot->sequenceAlignmentMaps = sequenceAlignmentMaps_;
    snapshot_ = std::move(snapshot);
}

void CoordinatorPublication::complete(CommandTerminal terminal) {
    std::scoped_lock lock(mutex_);
    completedCommands_.push_back(std::move(terminal));
}

} // namespace dvs::application
