#pragma once

#include <QKeyEvent>
#include <QObject>
#include <QSet>

namespace dvs::ui {

// E04 window-level key watcher for held-key frame jogging. Installed as an event filter on the
// review window, it converts bare A/D and Left/Right presses (plus the player preset's ,/.)
// into direction press/release events, and consumes OS auto-repeat so repeats never re-trigger
// a jog start. It owns these sequences completely while enabled — the equivalent per-repeat
// Shortcuts are gone — and passes every key through untouched while disabled, so dialogs, text
// fields, menus and the image workspace keep their own keys. Window deactivation (Alt+Tab)
// releases held keys so focus loss always ends a jog. Not final: QML registration derives
// QQmlElement from the registered type.
class JogKeyHandler : public QObject {
    Q_OBJECT

    // The watched window. Changing it releases held keys and re-installs the filter.
    Q_PROPERTY(QObject* window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool forwardEnabled READ forwardEnabled WRITE setForwardEnabled NOTIFY
                   forwardEnabledChanged)
    Q_PROPERTY(bool backwardEnabled READ backwardEnabled WRITE setBackwardEnabled NOTIFY
                   backwardEnabledChanged)
    // Player preset adds the ,/. frame-step aliases alongside A/D and Left/Right.
    Q_PROPERTY(bool playerPresetKeys READ playerPresetKeys WRITE setPlayerPresetKeys NOTIFY
                   playerPresetKeysChanged)

public:
    explicit JogKeyHandler(QObject* parent = nullptr);
    ~JogKeyHandler() override;

    JogKeyHandler(const JogKeyHandler&) = delete;
    JogKeyHandler& operator=(const JogKeyHandler&) = delete;

    [[nodiscard]] QObject* window() const noexcept;
    void setWindow(QObject* window);
    [[nodiscard]] bool enabled() const noexcept;
    void setEnabled(bool enabled);
    [[nodiscard]] bool forwardEnabled() const noexcept;
    void setForwardEnabled(bool enabled);
    [[nodiscard]] bool backwardEnabled() const noexcept;
    void setBackwardEnabled(bool enabled);
    [[nodiscard]] bool playerPresetKeys() const noexcept;
    void setPlayerPresetKeys(bool enabled);

Q_SIGNALS:
    void jogPressed(int direction);
    void jogReleased(int direction);
    void windowChanged();
    void enabledChanged();
    void forwardEnabledChanged();
    void backwardEnabledChanged();
    void playerPresetKeysChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // Returns the jog direction for a key event, or 0 when the key is not a jog key for the
    // current preset. Only unmodified (and numpad) variants map.
    [[nodiscard]] int directionFor(const QKeyEvent& event) const noexcept;
    // Emits jogReleased for every held key and forgets them. Used on disable, window change,
    // window deactivation and destruction.
    void releaseHeldKeys();
    void uninstallFilter();

    QObject* window_ = nullptr;
    bool enabled_ = false;
    bool forwardEnabled_ = false;
    bool backwardEnabled_ = false;
    bool playerPresetKeys_ = false;
    QSet<int> pressedKeys_;
};

} // namespace dvs::ui
