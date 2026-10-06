#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace dvs::ui {

// A native startup splash that owns its own thread and its own HWND.
//
// Why it is not QML: the gap it has to cover is `context-ready -> qml-loaded`
// (~550 ms) plus `show-enter -> sg-initialized` (~180 ms), and the GUI thread is
// synchronously blocked for all of it. A QQuickWindow cannot paint before the
// event loop runs, so a QML splash would itself be a blank window. A worker
// thread with a plain GDI window paints while the GUI thread is still busy.
//
// Contract:
//  - show()/requestDismiss() are safe from any thread and never block the
//    caller. join() is the only blocking call and is meant for the GUI thread
//    after the event loop is up, where the worker is already idle.
//  - requestDismiss() is idempotent and safe before show(), so every early
//    return in DesktopApplication::load() can dismiss through a guard.
//  - Any failure to create the window degrades to "no splash"; it must never
//    stop or delay startup.
class StartupSplash final {
public:
    StartupSplash() = default;
    ~StartupSplash();
    StartupSplash(const StartupSplash&) = delete;
    StartupSplash& operator=(const StartupSplash&) = delete;

    // Starts the worker thread. A no-op in smoke mode and on non-Windows.
    void show(const std::string& title, const std::string& subtitle);
    // Asks the worker to close. Non-blocking, idempotent, callable from any
    // thread including the render thread.
    void requestDismiss();
    // Waits for the worker to exit. Idempotent. Only called on the GUI thread.
    void join();
    // True once a real HWND exists. Diagnostic and test seam only.
    [[nodiscard]] bool windowVisible() const noexcept;

private:
    void runWorker(const std::string& title, const std::string& subtitle);

    std::atomic<bool> dismissRequested_{false};
    std::atomic<bool> windowCreated_{false};
    std::atomic<bool> running_{false};
    // Written by the worker thread, read by requestDismiss() on the GUI or
    // render thread; atomic because a plain pointer read would be a race.
    std::atomic<void*> hwnd_{nullptr};
    std::thread worker_;
};

} // namespace dvs::ui
