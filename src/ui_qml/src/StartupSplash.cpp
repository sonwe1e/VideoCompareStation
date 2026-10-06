#include "dvs/ui/StartupSplash.h"

#include "dvs/ui/StartupMilestone.h"

#include <algorithm>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
// clang-format on
#endif

namespace dvs::ui {
namespace {

#if defined(_WIN32)
// The review shell clears to this colour (see Theme.background in
// src/ui_qml/qml/VcsTheme.js). Painting the splash the same colour means the
// handover from splash to main window is not a visible brightness jump.
constexpr COLORREF kSplashBackground = RGB(0x09, 0x0d, 0x14);
constexpr COLORREF kSplashTitleColor = RGB(0xe2, 0xe8, 0xf0);
constexpr COLORREF kSplashSubtitleColor = RGB(0x94, 0xa3, 0xb8);
constexpr wchar_t kSplashClassName[] = L"CompareStationStartupSplash";
// The splash must be an *owned* window. Process.CloseMainWindow() - and anything else that guesses
// "the process main window" by walking top-level windows in enumeration order - takes the first
// visible unowned window, and the splash exists long before the review window does. An owned splash
// is skipped by that search, so closing the process closes the review window and the app exits
// gracefully instead of being hard-killed with its playback trace unflushed. That regression was
// measured, not assumed: exit code -1 on every round instead of 0, and no trace file.
constexpr wchar_t kSplashOwnerClassName[] = L"CompareStationStartupSplashOwner";
constexpr int kSplashWidth = 360;
constexpr int kSplashHeight = 180;
// How long the worker parks before looking at the dismiss flag again. Short enough that a teardown
// during startup is not noticeable, long enough that an idle splash costs nothing measurable.
constexpr DWORD kPollMilliseconds = 50;

struct SplashState final {
    std::wstring title;
    std::wstring subtitle;
    HBRUSH background = nullptr;
    HFONT titleFont = nullptr;
    HFONT subtitleFont = nullptr;
};

void destroySplashState(SplashState* const state) {
    if (state == nullptr) {
        return;
    }
    if (state->background != nullptr) {
        ::DeleteObject(state->background);
    }
    if (state->titleFont != nullptr) {
        ::DeleteObject(state->titleFont);
    }
    if (state->subtitleFont != nullptr) {
        ::DeleteObject(state->subtitleFont);
    }
    delete state;
}

SplashState* stateOf(const HWND window) {
    return reinterpret_cast<SplashState*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
}

std::wstring toWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

void drawCentered(
    HDC dc, HFONT font, COLORREF color, const std::wstring& text, const int top, const int bottom) {
    if (font == nullptr || text.empty()) {
        return;
    }
    const auto previous = ::SelectObject(dc, font);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, color);
    auto rect = ::RECT{0, top, static_cast<LONG>(::GetDeviceCaps(dc, HORZRES)), bottom};
    ::DrawTextW(dc, text.c_str(), -1, &rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    ::SelectObject(dc, previous);
}

LRESULT CALLBACK splashWndProc(HWND window,
                               const UINT message,
                               const WPARAM wparam,
                               const LPARAM lparam) {
    switch (message) {
    case WM_NCCREATE: {
        // The create parameters arrive here, not in WM_PAINT. Stash the state on
        // the window so later paint and destroy messages can reach it.
        const auto* const create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        ::SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = ::BeginPaint(window, &paint);
        if (dc != nullptr) {
            if (const auto* const state = stateOf(window); state != nullptr) {
                RECT client{};
                static_cast<void>(::GetClientRect(window, &client));
                const auto rect = client;
                static_cast<void>(::FillRect(dc, &rect, state->background));
                const int height = client.bottom - client.top;
                drawCentered(dc,
                             state->titleFont,
                             kSplashTitleColor,
                             state->title,
                             height / 2 - 34,
                             height / 2 - 2);
                drawCentered(dc,
                             state->subtitleFont,
                             kSplashSubtitleColor,
                             state->subtitle,
                             height / 2 + 2,
                             height / 2 + 26);
            }
            ::EndPaint(window, &paint);
        }
        return 0;
    }
    case WM_CLOSE:
        ::DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        destroySplashState(stateOf(window));
        ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
}
#endif

} // namespace

StartupSplash::~StartupSplash() {
    requestDismiss();
    join();
}

#if defined(_WIN32)
void StartupSplash::runWorker(const std::string& title, const std::string& subtitle) {
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = splashWndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kSplashClassName;
    if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return;
    }

    // Centre on the desktop. The main window picks a screen later; a splash that
    // briefly lands elsewhere is less bad than one that appears off-screen.
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (const auto desktop = ::GetDesktopWindow(); desktop != nullptr) {
        RECT bounds{};
        if (::GetWindowRect(desktop, &bounds) != 0) {
            x = bounds.left + ((bounds.right - bounds.left) - kSplashWidth) / 2;
            y = bounds.top + ((bounds.bottom - bounds.top) - kSplashHeight) / 2;
        }
    }

    // A dismiss that arrived between the caller's check and this CreateWindow must not leave an
    // orphan window behind.
    if (dismissRequested_.load(std::memory_order_acquire)) {
        return;
    }

    // A hidden owner so the splash is owned and therefore skipped by any "main window of the
    // process" search. It is never shown.
    WNDCLASSEXW ownerClass{};
    ownerClass.cbSize = sizeof(ownerClass);
    ownerClass.lpfnWndProc = ::DefWindowProcW;
    ownerClass.hInstance = instance;
    ownerClass.lpszClassName = kSplashOwnerClassName;
    if (::RegisterClassExW(&ownerClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return;
    }
    const auto owner = ::CreateWindowExW(
        0, kSplashOwnerClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (owner == nullptr) {
        return;
    }

    auto* const state = new SplashState{
        .title = toWide(title),
        .subtitle = toWide(subtitle),
        .background = ::CreateSolidBrush(kSplashBackground),
        .titleFont = ::CreateFontW(-20,
                                   0,
                                   0,
                                   0,
                                   FW_SEMIBOLD,
                                   FALSE,
                                   FALSE,
                                   FALSE,
                                   DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE,
                                   L"Segoe UI"),
        .subtitleFont = ::CreateFontW(-13,
                                      0,
                                      0,
                                      0,
                                      FW_NORMAL,
                                      FALSE,
                                      FALSE,
                                      FALSE,
                                      DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE,
                                      L"Segoe UI"),
    };

    const auto window = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                          kSplashClassName,
                                          L"CompareStation",
                                          WS_POPUP,
                                          x,
                                          y,
                                          kSplashWidth,
                                          kSplashHeight,
                                          owner,
                                          nullptr,
                                          instance,
                                          state);
    if (window == nullptr) {
        destroySplashState(state);
        ::DestroyWindow(owner);
        return;
    }
    hwnd_.store(window, std::memory_order_release);
    windowCreated_.store(true, std::memory_order_release);

    // WS_EX_NOACTIVATE plus the direct paint keeps the splash from taking focus
    // away from whatever the user was doing.
    ::ShowWindow(window, SW_SHOWNOACTIVATE);
    ::UpdateWindow(window);
    // The harness measures "spawn -> window" by walking the process's unowned top-level windows,
    // and the splash is deliberately owned so it is never mistaken for the review window. That
    // makes this milestone the only place the moment the first pixels actually reach the screen is
    // recorded.
    markStartupMilestone("splash-painted");

    // A dismiss that lands between the re-check above and the store of hwnd_ is one the caller
    // could not deliver: it read a null handle and posted nothing, so a plain GetMessage would
    // leave this thread waiting for a message nobody is going to send. That is a hang, not a late
    // splash - it surfaced as a test timeout in
    // DestructionRemovesTheWindowWithoutAnExplicitDismiss, and in production it would freeze a
    // launch torn down during startup. The loop re-reads the flag on every pass and waits with a
    // timeout rather than indefinitely, so teardown no longer depends on the caller and the window
    // handle being visible at the same instant. It also subsumes the re-check before
    // CreateWindowExW for anything that gets this far.
    MSG message{};
    while (true) {
        if (dismissRequested_.load(std::memory_order_acquire)) {
            ::PostMessageW(window, WM_CLOSE, 0, 0);
        }
        const DWORD wait =
            ::MsgWaitForMultipleObjects(0, nullptr, FALSE, kPollMilliseconds, QS_ALLINPUT);
        if (wait == WAIT_TIMEOUT) {
            continue;
        }
        if (wait != WAIT_OBJECT_0) {
            break;
        }
        if (::GetMessageW(&message, nullptr, 0, 0) <= 0) {
            break;
        }
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    hwnd_.store(nullptr, std::memory_order_release);
    windowCreated_.store(false, std::memory_order_release);
    // WM_DESTROY above called PostQuitMessage, which is why the owner outlives
    // the loop by this one statement. Destroying it releases the only handle the
    // hidden owner held.
    ::DestroyWindow(owner);
}
#else
void StartupSplash::runWorker(const std::string& title, const std::string& subtitle) {
    static_cast<void>(title);
    static_cast<void>(subtitle);
}
#endif

void StartupSplash::show(const std::string& title, const std::string& subtitle) {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    dismissRequested_.store(false, std::memory_order_release);
    worker_ = std::thread{[this, title, subtitle] { runWorker(title, subtitle); }};
}

void StartupSplash::requestDismiss() {
    dismissRequested_.store(true, std::memory_order_release);
    // The worker may not have created the window yet; it re-checks the flag
    // immediately before CreateWindowExW, so this is enough to suppress a late
    // splash and nothing is left behind.
    if (const auto window = static_cast<HWND>(hwnd_.load(std::memory_order_acquire));
        window != nullptr) {
        ::PostMessageW(window, WM_CLOSE, 0, 0);
    }
}

void StartupSplash::join() {
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false, std::memory_order_release);
}

bool StartupSplash::windowVisible() const noexcept {
    return windowCreated_.load(std::memory_order_acquire);
}

} // namespace dvs::ui
