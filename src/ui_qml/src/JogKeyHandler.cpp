#include "dvs/ui/JogKeyHandler.h"

#include <QEvent>

namespace dvs::ui {

namespace {
// Numpad arrows arrive with KeypadModifier when NumLock is off; every other modifier makes the
// key a different shortcut (Shift+Left five frames, Ctrl+Left one second) that must pass through.
[[nodiscard]] bool isUnmodified(const Qt::KeyboardModifiers modifiers) noexcept {
    return modifiers == Qt::NoModifier || modifiers == Qt::KeypadModifier;
}
} // namespace

JogKeyHandler::JogKeyHandler(QObject* parent) : QObject(parent) {}

JogKeyHandler::~JogKeyHandler() {
    releaseHeldKeys();
    uninstallFilter();
}

QObject* JogKeyHandler::window() const noexcept {
    return window_;
}

void JogKeyHandler::setWindow(QObject* window) {
    if (window_ == window) {
        return;
    }
    releaseHeldKeys();
    uninstallFilter();
    window_ = window;
    if (window_ != nullptr) {
        window_->installEventFilter(this);
    }
    Q_EMIT windowChanged();
}

bool JogKeyHandler::enabled() const noexcept {
    return enabled_;
}

void JogKeyHandler::setEnabled(const bool enabled) {
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    if (!enabled_) {
        releaseHeldKeys();
    }
    Q_EMIT enabledChanged();
}

bool JogKeyHandler::forwardEnabled() const noexcept {
    return forwardEnabled_;
}

void JogKeyHandler::setForwardEnabled(const bool enabled) {
    if (forwardEnabled_ == enabled) {
        return;
    }
    forwardEnabled_ = enabled;
    Q_EMIT forwardEnabledChanged();
}

bool JogKeyHandler::backwardEnabled() const noexcept {
    return backwardEnabled_;
}

void JogKeyHandler::setBackwardEnabled(const bool enabled) {
    if (backwardEnabled_ == enabled) {
        return;
    }
    backwardEnabled_ = enabled;
    Q_EMIT backwardEnabledChanged();
}

bool JogKeyHandler::playerPresetKeys() const noexcept {
    return playerPresetKeys_;
}

void JogKeyHandler::setPlayerPresetKeys(const bool enabled) {
    if (playerPresetKeys_ == enabled) {
        return;
    }
    playerPresetKeys_ = enabled;
    Q_EMIT playerPresetKeysChanged();
}

int JogKeyHandler::directionFor(const QKeyEvent& event) const noexcept {
    if (!isUnmodified(event.modifiers())) {
        return 0;
    }
    switch (event.key()) {
    case Qt::Key_A:
    case Qt::Key_Left:
        return -1;
    case Qt::Key_D:
    case Qt::Key_Right:
        return 1;
    case Qt::Key_Comma:
        return playerPresetKeys_ ? -1 : 0;
    case Qt::Key_Period:
        return playerPresetKeys_ ? 1 : 0;
    default:
        return 0;
    }
}

bool JogKeyHandler::eventFilter(QObject* watched, QEvent* event) {
    if (watched != window_) {
        return QObject::eventFilter(watched, event);
    }
    if (event->type() == QEvent::WindowDeactivate) {
        // OS focus loss (Alt+Tab, another application) must end a held jog even though no key
        // release will arrive while the window is inactive.
        releaseHeldKeys();
        return false;
    }
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) {
        return false;
    }
    const auto& keyEvent = *static_cast<QKeyEvent*>(event);
    const int direction = directionFor(keyEvent);
    if (direction == 0) {
        return false;
    }
    if (event->type() == QEvent::KeyPress) {
        // Repeats never re-trigger a jog start; consuming them also keeps any surviving
        // per-repeat shortcut from firing.
        if (keyEvent.isAutoRepeat()) {
            return true;
        }
        if (!enabled_ || !(direction > 0 ? forwardEnabled_ : backwardEnabled_)) {
            return false;
        }
        if (pressedKeys_.contains(keyEvent.key())) {
            return true;
        }
        pressedKeys_.insert(keyEvent.key());
        Q_EMIT jogPressed(direction);
        return true;
    }
    // A repeat's synthetic release must not end the hold.
    if (keyEvent.isAutoRepeat() || !pressedKeys_.remove(keyEvent.key())) {
        return keyEvent.isAutoRepeat();
    }
    // A staggered multi-key hold releases one direction at a time: the walk survives until the
    // last key of that direction is up, because the consumer owns a single jog slot and would
    // otherwise stop early while another key of the same direction is still held.
    if (!anyKeyHeldFor(direction)) {
        Q_EMIT jogReleased(direction);
    }
    return true;
}

bool JogKeyHandler::anyKeyHeldFor(const int direction) const noexcept {
    for (const int key : pressedKeys_) {
        if (directionFor(QKeyEvent{QEvent::KeyRelease, key, Qt::NoModifier}) == direction) {
            return true;
        }
    }
    return false;
}

void JogKeyHandler::releaseHeldKeys() {
    // One release per direction that still had a key down: a simultaneous multi-key hold must
    // not look like several separate walks ending.
    const QSet<int> held = pressedKeys_;
    pressedKeys_.clear();
    for (const int direction : {-1, 1}) {
        bool anyHeld = false;
        for (const int key : held) {
            if (directionFor(QKeyEvent{QEvent::KeyRelease, key, Qt::NoModifier}) == direction) {
                anyHeld = true;
                break;
            }
        }
        if (anyHeld) {
            Q_EMIT jogReleased(direction);
        }
    }
}

void JogKeyHandler::uninstallFilter() {
    if (window_ != nullptr) {
        window_->removeEventFilter(this);
    }
}

} // namespace dvs::ui
