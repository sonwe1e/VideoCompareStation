#pragma once

#include <QObject>

namespace dvs::ui {

// Window-level hold-key watcher for "press and hold to peek" semantics (D4). Installed as an
// event filter on the review window, it converts one configurable key into press/release
// pairs and consumes OS auto-repeat so repeats never re-trigger the hold. The video
// difference-mode backtick peek uses it: holding shows the reference original, releasing
// restores the difference view. While disabled it passes every key through untouched, so
// dialogs, text fields and menus keep their own keys; window deactivation releases the hold
// so focus loss never pins the raw view on screen.
class HoldKeyHandler : public QObject {
    Q_OBJECT

    // The watched window. Changing it releases the hold and re-installs the filter.
    Q_PROPERTY(QObject* window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // The Qt::Key code watched while enabled; 0 watches nothing.
    Q_PROPERTY(int holdKey READ holdKey WRITE setHoldKey NOTIFY holdKeyChanged)

public:
    explicit HoldKeyHandler(QObject* parent = nullptr);
    ~HoldKeyHandler() override;

    HoldKeyHandler(const HoldKeyHandler&) = delete;
    HoldKeyHandler& operator=(const HoldKeyHandler&) = delete;

    [[nodiscard]] QObject* window() const noexcept;
    void setWindow(QObject* window);
    [[nodiscard]] bool enabled() const noexcept;
    void setEnabled(bool enabled);
    [[nodiscard]] int holdKey() const noexcept;
    void setHoldKey(int key);

Q_SIGNALS:
    // Exactly one press per physical hold (auto-repeats are consumed silently); the release
    // also fires once, including the forced release from disable, window change, window
    // deactivation and destruction.
    void holdPressed();
    void holdReleased();
    void windowChanged();
    void enabledChanged();
    void holdKeyChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void releaseHold();
    void uninstallFilter();

    QObject* window_ = nullptr;
    bool enabled_ = false;
    int holdKey_ = 0;
    bool holding_ = false;
};

} // namespace dvs::ui
