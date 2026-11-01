#include "dvs/ui/HoldKeyHandler.h"

#include <QEvent>
#include <QKeyEvent>

namespace dvs::ui {

HoldKeyHandler::HoldKeyHandler(QObject* parent) : QObject(parent) {}

HoldKeyHandler::~HoldKeyHandler() {
    releaseHold();
    uninstallFilter();
}

QObject* HoldKeyHandler::window() const noexcept {
    return window_;
}

void HoldKeyHandler::setWindow(QObject* window) {
    if (window_ == window) {
        return;
    }
    releaseHold();
    uninstallFilter();
    window_ = window;
    if (window_ != nullptr) {
        window_->installEventFilter(this);
    }
    Q_EMIT windowChanged();
}

bool HoldKeyHandler::enabled() const noexcept {
    return enabled_;
}

void HoldKeyHandler::setEnabled(const bool enabled) {
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    if (!enabled_) {
        releaseHold();
    }
    Q_EMIT enabledChanged();
}

int HoldKeyHandler::holdKey() const noexcept {
    return holdKey_;
}

void HoldKeyHandler::setHoldKey(const int key) {
    if (holdKey_ == key) {
        return;
    }
    releaseHold();
    holdKey_ = key;
    Q_EMIT holdKeyChanged();
}

void HoldKeyHandler::releaseHold() {
    if (!holding_) {
        return;
    }
    holding_ = false;
    Q_EMIT holdReleased();
}

void HoldKeyHandler::uninstallFilter() {
    if (window_ != nullptr) {
        window_->removeEventFilter(this);
    }
}

bool HoldKeyHandler::eventFilter(QObject* watched, QEvent* event) {
    if (watched != window_) {
        return QObject::eventFilter(watched, event);
    }
    if (event->type() == QEvent::WindowDeactivate) {
        // OS focus loss must end the hold even though no key release arrives while the
        // window is inactive; the raw view must never stay pinned on screen.
        releaseHold();
        return false;
    }
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) {
        return QObject::eventFilter(watched, event);
    }
    auto* const keyEvent = static_cast<QKeyEvent*>(event);
    if (!enabled_ || holdKey_ == 0 || keyEvent->key() != holdKey_) {
        return QObject::eventFilter(watched, event);
    }
    // Auto-repeat pairs (press and release alike) never change the hold: only the first
    // physical press arms it and only the real release ends it. Repeats are consumed either
    // way so nothing downstream re-triggers on the OS repeat clock.
    if (keyEvent->isAutoRepeat()) {
        return true;
    }
    if (event->type() == QEvent::KeyPress) {
        // Only the unmodified key may START a hold; Shift+` (the lock) and any other
        // modified variant belongs to the Shortcuts and passes through untouched.
        if (!(keyEvent->modifiers() == Qt::NoModifier ||
              keyEvent->modifiers() == Qt::KeypadModifier)) {
            return QObject::eventFilter(watched, event);
        }
        if (!holding_) {
            holding_ = true;
            Q_EMIT holdPressed();
        }
        return true;
    }
    // KeyRelease: the real release of an armed hold ends it regardless of which modifiers
    // were pressed in the meantime - gating the release by "unmodified" would leave the raw
    // view pinned when the user happened to hold Shift while letting go.
    if (holding_) {
        releaseHold();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace dvs::ui
