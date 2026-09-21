#include "dvs/ui/PlaybackViewModel.h"

namespace dvs::ui {

PlaybackViewModel::PlaybackViewModel(ReviewController* controller, QObject* parent)
    : QObject(parent), controller_(controller) {
    if (controller_ != nullptr) {
        connect(controller_,
                &ReviewController::stateChanged,
                this,
                &PlaybackViewModel::stateChanged);
        connect(controller_,
                &ReviewController::frameStateChanged,
                this,
                &PlaybackViewModel::stateChanged);
    }
}

ReviewController* PlaybackViewModel::controller() const noexcept {
    return controller_;
}

bool PlaybackViewModel::playing() const noexcept {
    return controller_ != nullptr && controller_->playing();
}

qint64 PlaybackViewModel::currentFrame() const noexcept {
    return controller_ != nullptr ? controller_->currentFrame() : -1;
}

qint64 PlaybackViewModel::rangeIn() const noexcept {
    return controller_ != nullptr ? controller_->playbackRangeIn() : -1;
}

qint64 PlaybackViewModel::rangeOut() const noexcept {
    return controller_ != nullptr ? controller_->playbackRangeOut() : -1;
}

bool PlaybackViewModel::rangeLoop() const noexcept {
    return controller_ != nullptr && controller_->playbackRangeLoop();
}

bool PlaybackViewModel::rangeLoopActive() const noexcept {
    return controller_ != nullptr && controller_->playbackRangeLoopActive();
}

bool PlaybackViewModel::play() {
    return controller_ != nullptr && controller_->play();
}

bool PlaybackViewModel::pause() {
    return controller_ != nullptr && controller_->pause();
}

bool PlaybackViewModel::seekFrame(const qint64 frame) {
    return controller_ != nullptr && controller_->seekFrame(frame);
}

bool PlaybackViewModel::setPlaybackRange(const qint64 inFrame,
                                         const qint64 outFrame,
                                         const bool loop) {
    return controller_ != nullptr && controller_->setPlaybackRange(inFrame, outFrame, loop);
}

bool PlaybackViewModel::playRange(const qint64 inFrame, const qint64 outFrame) {
    return controller_ != nullptr && controller_->playRange(inFrame, outFrame);
}

bool PlaybackViewModel::stopRangeLoop() {
    return controller_ != nullptr && controller_->stopRangeLoop();
}

} // namespace dvs::ui
