#pragma once

#include "dvs/ui/ReviewController.h"

#include <QObject>

namespace dvs::ui {

// Narrow playback projection. Media truth stays on the application SessionSnapshot; this object
// only re-exposes playback/range state and forwards transport commands so QML range loop no
// longer owns seek/play authority.
class PlaybackViewModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(ReviewController* controller READ controller NOTIFY controllerChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(qint64 currentFrame READ currentFrame NOTIFY stateChanged)
    Q_PROPERTY(qint64 rangeIn READ rangeIn NOTIFY stateChanged)
    Q_PROPERTY(qint64 rangeOut READ rangeOut NOTIFY stateChanged)
    Q_PROPERTY(bool rangeLoop READ rangeLoop NOTIFY stateChanged)
    Q_PROPERTY(bool rangeLoopActive READ rangeLoopActive NOTIFY stateChanged)

public:
    explicit PlaybackViewModel(ReviewController* controller, QObject* parent = nullptr);

    [[nodiscard]] ReviewController* controller() const noexcept;
    [[nodiscard]] bool playing() const noexcept;
    [[nodiscard]] qint64 currentFrame() const noexcept;
    [[nodiscard]] qint64 rangeIn() const noexcept;
    [[nodiscard]] qint64 rangeOut() const noexcept;
    [[nodiscard]] bool rangeLoop() const noexcept;
    [[nodiscard]] bool rangeLoopActive() const noexcept;

    Q_INVOKABLE bool play();
    Q_INVOKABLE bool pause();
    Q_INVOKABLE bool seekFrame(qint64 frame);
    Q_INVOKABLE bool setPlaybackRange(qint64 inFrame, qint64 outFrame, bool loop);
    Q_INVOKABLE bool playRange(qint64 inFrame, qint64 outFrame);
    Q_INVOKABLE bool stopRangeLoop();

Q_SIGNALS:
    void controllerChanged();
    void stateChanged();

private:
    ReviewController* controller_ = nullptr;
};

} // namespace dvs::ui
