#include "dvs/ui/StartupSplash.h"

#include "dvs/ui/StartupMilestone.h"

#include <QFile>

#include <algorithm>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
// clang-format on
#endif

#ifndef DVS_PROJECT_VERSION
#define DVS_PROJECT_VERSION "0.0.0"
#endif

namespace dvs::ui {
namespace {

#if defined(_WIN32)
// The review shell clears to this colour (see Theme.window in
// src/ui_qml/qml/VcsTheme.js). Painting the splash the same colour means the
// handover from splash to main window is not a visible brightness jump.
constexpr COLORREF kSplashBackground = RGB(0x1c, 0x1c, 0x1e);
constexpr COLORREF kSplashTitleColor = RGB(0xf5, 0xf5, 0xf7);
constexpr COLORREF kSplashSubtitleColor = RGB(0xa5, 0xa5, 0xab);
constexpr COLORREF kSplashVersionColor = RGB(0x78, 0x78, 0x7d);
// Track and thumb of the indeterminate loading bar. The thumb is Theme.accent
// (#0a84ff); the track is a hairline above the background so the bar reads as
// one surface with the splash, not an inset control.
constexpr COLORREF kSplashTrackColor = RGB(0x3a, 0x3a, 0x3c);
constexpr COLORREF kSplashThumbColor = RGB(0x0a, 0x84, 0xff);
constexpr wchar_t kSplashClassName[] = L"CompareStationStartupSplash";
// The splash must be an *owned* window. Process.CloseMainWindow() - and anything else that guesses
// "the process main window" by walking top-level windows in enumeration order - takes the first
// visible unowned window, and the splash exists long before the review window does. An owned splash
// is skipped by that search, so closing the process closes the review window and the app exits
// gracefully instead of being hard-killed with its playback trace unflushed. That regression was
// measured, not assumed: exit code -1 on every round instead of 0, and no trace file.
constexpr wchar_t kSplashOwnerClassName[] = L"CompareStationStartupSplashOwner";
// Layout is authored at 96 DPI and scaled by the system DPI: the splash is a
// GDI window created before any Qt screen information exists, so the system
// DPI is the best available factor. Layout values refer to logical pixels.
constexpr int kSplashWidth = 480;
constexpr int kSplashHeight = 320;
constexpr int kSplashLogoSize = 72;
constexpr int kSplashLogoTop = 56;
constexpr int kSplashTitleTop = 164;
constexpr int kSplashTitleBottom = 204;
constexpr int kSplashSubtitleTop = 212;
constexpr int kSplashSubtitleBottom = 238;
constexpr int kSplashBarMarginX = 72;
constexpr int kSplashBarY = 268;
constexpr int kSplashBarHeight = 4;
constexpr int kSplashThumbWidth = 88;
constexpr int kSplashVersionInset = 20;
constexpr int kSplashVersionTop = 288;
constexpr int kSplashVersionBottom = 308;
// The loading bar is indeterminate: startup has no progress feed to bind to
// (the milestones all fire on the synchronously blocked GUI thread), so the
// thumb sweeps left-to-right and back like PyCharm's splash. One full sweep.
constexpr DWORD kBarSweepMilliseconds = 1600;
constexpr UINT kSplashAnimationTimer = 1;
constexpr DWORD kSplashAnimationInterval = 40;
// How long the worker parks before looking at the dismiss flag again. Short enough that a teardown
// during startup is not noticeable, long enough that an idle splash costs nothing measurable.
constexpr DWORD kPollMilliseconds = 50;

int scaled(const int value, const UINT dpi) noexcept {
    return static_cast<int>((value * static_cast<LONG>(dpi) + 47) / 96);
}

// Triangle wave in [0, 1]: rises for the first half of the period, falls for
// the second, so the thumb sweeps right and returns without a jump.
double sweepPhase(const ULONGLONG elapsedMilliseconds) noexcept {
    const double position =
        static_cast<double>(elapsedMilliseconds % kBarSweepMilliseconds) / kBarSweepMilliseconds;
    return position < 0.5 ? position * 2.0 : 2.0 - position * 2.0;
}

struct SplashState final {
    std::wstring title;
    std::wstring subtitle;
    std::wstring version;
    HBRUSH background = nullptr;
    HFONT titleFont = nullptr;
    HFONT subtitleFont = nullptr;
    HFONT versionFont = nullptr;
    Gdiplus::Bitmap* logo = nullptr; // Owned here, released in destroySplashState.
    // GDI+ does not take ownership of the stream a Bitmap was created from and
    // may read it lazily during drawing, so the stream must outlive the Bitmap.
    // Released in destroySplashState, after the Bitmap is deleted.
    IStream* logoStream = nullptr;
    UINT dpi = 96;
    // Animation anchor: the tick count when the splash was shown. Written once
    // by the worker before the message loop runs and only read afterwards
    // (WM_PAINT dispatches on this same thread), so a plain member is enough.
    ULONGLONG shownAt = 0;
    // Logical-pixel geometry recomputed once per worker (all inputs are const
    // after creation), so WM_PAINT only reads plain ints.
    int clientWidth = 0;
    int logoTop = 0;
    int logoSize = 0;
    int titleTop = 0;
    int titleBottom = 0;
    int subtitleTop = 0;
    int subtitleBottom = 0;
    int barX = 0;
    int barY = 0;
    int barWidth = 0;
    int thumbWidth = 0;
    int versionRight = 0;
    int versionTop = 0;
    int versionBottom = 0;
};

void destroySplashState(SplashState* const state) {
    if (state == nullptr) {
        return;
    }
    // GDI+ frees through GdiplusBase::operator delete -> GdipFree, an opaque
    // allocator clang-analyzer cannot model: it reports a false use-after-free
    // whose diagnostic primary location sits inside the Windows header, so no
    // NOLINT on this line can suppress it. The analyzer macro hides just this
    // one delete from static analysis; every real build runs it. The state is
    // destroyed exactly once (WM_NCDESTROY clears GWLP_USERDATA right after),
    // so the skipped double-free modelling costs no real coverage.
#ifndef __clang_analyzer__
    delete state->logo;
#endif
    if (state->logoStream != nullptr) {
        state->logoStream->Release();
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
    if (state->versionFont != nullptr) {
        ::DeleteObject(state->versionFont);
    }
    delete state;
}

SplashState* stateOf(const HWND window) {
    // GWLP_USERDATA is the documented Win32 pattern for per-window state.
    return reinterpret_cast<SplashState*>( // NOLINT(performance-no-int-to-ptr)
        ::GetWindowLongPtrW(window, GWLP_USERDATA));
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

// Loads the branding icon from the Qt resources into the splash state. Runs on
// the worker thread after GDI+ startup. A missing resource (for example a test
// binary that never initialised the resource module) degrades to "no logo":
// the splash contract says any failure costs decoration, never the window
// itself. On success the state owns both the Bitmap and the stream it was
// decoded from (GDI+ may keep reading the stream while drawing).
void loadLogoInto(SplashState& state) {
    QFile resource{QStringLiteral(":/branding/comparestation-icon.png")};
    if (!resource.open(QIODevice::ReadOnly)) {
        return;
    }
    const QByteArray bytes = resource.readAll();
    if (bytes.isEmpty()) {
        return;
    }
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(bytes.size()));
    if (memory == nullptr) {
        return;
    }
    void* const target = ::GlobalLock(memory);
    if (target == nullptr) {
        ::GlobalFree(memory);
        return;
    }
    std::copy(bytes.constData(), bytes.constData() + bytes.size(), static_cast<char*>(target));
    ::GlobalUnlock(memory);
    IStream* stream = nullptr;
    if (FAILED(::CreateStreamOnHGlobal(memory, TRUE, &stream)) || stream == nullptr) {
        // Stream ownership was not transferred; free the buffer here.
        ::GlobalFree(memory);
        return;
    }
    Gdiplus::Bitmap* const bitmap = Gdiplus::Bitmap::FromStream(stream, FALSE);
    if (bitmap == nullptr || bitmap->GetLastStatus() != Gdiplus::Ok) {
        // Same analyzer blind spot as in destroySplashState: GdipFree is an
        // opaque allocator, and the delete on this error path produces a false
        // use-after-free inside the Windows header. Hidden from static analysis
        // only; real builds always run it.
#ifndef __clang_analyzer__
        delete bitmap;
#endif
        stream->Release();
        return;
    }
    state.logo = bitmap;
    state.logoStream = stream;
}

HFONT createFont(const UINT dpi, const int pixelHeight, const LONG weight) {
    return ::CreateFontW(-scaled(pixelHeight, dpi),
                         0,
                         0,
                         0,
                         weight,
                         FALSE,
                         FALSE,
                         FALSE,
                         DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE,
                         L"Segoe UI");
}

void drawCentered(HDC dc,
                  HFONT font,
                  COLORREF color,
                  const std::wstring& text,
                  const int left,
                  const int right,
                  const int top,
                  const int bottom) {
    if (font == nullptr || text.empty()) {
        return;
    }
    const auto previous = ::SelectObject(dc, font);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, color);
    auto rect = ::RECT{left, top, right, bottom};
    ::DrawTextW(dc, text.c_str(), -1, &rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    ::SelectObject(dc, previous);
}

void drawBottomRight(HDC dc,
                     HFONT font,
                     COLORREF color,
                     const std::wstring& text,
                     const int right,
                     const int top,
                     const int bottom) {
    if (font == nullptr || text.empty()) {
        return;
    }
    const auto previous = ::SelectObject(dc, font);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, color);
    auto rect = ::RECT{0, top, right, bottom};
    ::DrawTextW(dc, text.c_str(), -1, &rect, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    ::SelectObject(dc, previous);
}

void drawLogo(HDC dc, const SplashState& state) {
    if (state.logo == nullptr) {
        return;
    }
    Gdiplus::Graphics graphics{dc};
    if (graphics.GetLastStatus() != Gdiplus::Ok) {
        return;
    }
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    static_cast<void>(graphics.DrawImage(state.logo,
                                         Gdiplus::Rect{(state.clientWidth - state.logoSize) / 2,
                                                       state.logoTop,
                                                       state.logoSize,
                                                       state.logoSize},
                                         0,
                                         0,
                                         static_cast<int>(state.logo->GetWidth()),
                                         static_cast<int>(state.logo->GetHeight()),
                                         Gdiplus::UnitPixel));
}

void drawLoadingBar(HDC dc, const SplashState& state, const ULONGLONG shownAt) {
    const RECT track{state.barX,
                     state.barY,
                     state.barX + state.barWidth,
                     state.barY + scaled(kSplashBarHeight, state.dpi)};
    HBRUSH const trackBrush = ::CreateSolidBrush(kSplashTrackColor);
    if (trackBrush != nullptr) {
        ::FillRect(dc, &track, trackBrush);
        ::DeleteObject(trackBrush);
    }
    const int travel = track.right - track.left - state.thumbWidth;
    const int thumbX =
        track.left + static_cast<int>(sweepPhase(::GetTickCount64() - shownAt) * travel);
    const RECT thumb{thumbX, track.top, thumbX + state.thumbWidth, track.bottom};
    HBRUSH const thumbBrush = ::CreateSolidBrush(kSplashThumbColor);
    if (thumbBrush != nullptr) {
        ::FillRect(dc, &thumb, thumbBrush);
        ::DeleteObject(thumbBrush);
    }
}

LRESULT CALLBACK splashWndProc(HWND window,
                               const UINT message,
                               const WPARAM wparam,
                               const LPARAM lparam) {
    switch (message) {
    case WM_NCCREATE: {
        // The create parameters arrive here, not in WM_PAINT. Stash the state on
        // the window so later paint and destroy messages can reach it.
        // LPARAM-to-struct is the documented Win32 message pattern.
        const auto* const create =
            reinterpret_cast<const CREATESTRUCTW*>(lparam); // NOLINT(performance-no-int-to-ptr)
        ::SetWindowLongPtrW(window,
                            GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>( // NOLINT(performance-no-int-to-ptr)
                                create->lpCreateParams));
        break;
    }
    case WM_ERASEBKGND:
        // WM_PAINT fills the whole client area; letting the default eraser run
        // first would flash a white frame between the two on every animation
        // tick. Claim the erase and paint everything in one pass.
        return 1;
    case WM_TIMER: {
        // Only the loading-bar region animates; invalidating just that strip
        // keeps the rest of the splash untouched between ticks.
        if (wparam == kSplashAnimationTimer) {
            if (const auto* const state = stateOf(window); state != nullptr) {
                const int barHeight = scaled(kSplashBarHeight, state->dpi);
                const RECT animated{state->barX,
                                    state->barY - barHeight,
                                    state->barX + state->barWidth,
                                    state->barY + barHeight * 2};
                ::InvalidateRect(window, &animated, FALSE);
            }
        }
        return 0;
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
                drawLogo(dc, *state);
                drawCentered(dc,
                             state->titleFont,
                             kSplashTitleColor,
                             state->title,
                             0,
                             client.right,
                             state->titleTop,
                             state->titleBottom);
                drawCentered(dc,
                             state->subtitleFont,
                             kSplashSubtitleColor,
                             state->subtitle,
                             0,
                             client.right,
                             state->subtitleTop,
                             state->subtitleBottom);
                drawBottomRight(dc,
                                state->versionFont,
                                kSplashVersionColor,
                                state->version,
                                state->versionRight,
                                state->versionTop,
                                state->versionBottom);
                // Animation phase is anchored to the moment the splash was
                // shown, not to paint time, so a paused animation never creeps.
                drawLoadingBar(dc, *state, state->shownAt);
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

StartupSplash::StartupSplash(const std::chrono::milliseconds maxLifetime)
    : maxLifetime_(maxLifetime) {}

StartupSplash::~StartupSplash() {
    requestDismiss();
    join();
}

#if defined(_WIN32)
void StartupSplash::runWorker(const std::string& title, const std::string& subtitle) {
    Gdiplus::GdiplusStartupInput gdiplusInput{};
    ULONG_PTR gdiplusToken = 0;
    // Startup failure only costs the logo: every GDI+ call below is guarded by
    // gdiplusReady, and the shutdown is paired at every exit of this function,
    // after every GDI+ object (the logo Bitmap lives in SplashState) is gone.
    const bool gdiplusReady =
        Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) == Gdiplus::Ok;

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = splashWndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kSplashClassName;
    if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (gdiplusReady) {
            Gdiplus::GdiplusShutdown(gdiplusToken);
        }
        return;
    }

    // Centre on the desktop. The main window picks a screen later; a splash that
    // briefly lands elsewhere is less bad than one that appears off-screen.
    const UINT dpi = ::GetDpiForSystem();
    const int width = scaled(kSplashWidth, dpi);
    const int height = scaled(kSplashHeight, dpi);
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (const auto desktop = ::GetDesktopWindow(); desktop != nullptr) {
        RECT bounds{};
        if (::GetWindowRect(desktop, &bounds) != 0) {
            x = bounds.left + ((bounds.right - bounds.left) - width) / 2;
            y = bounds.top + ((bounds.bottom - bounds.top) - height) / 2;
        }
    }

    // A dismiss that arrived between the caller's check and this CreateWindow must not leave an
    // orphan window behind.
    if (dismissRequested_.load(std::memory_order_acquire)) {
        if (gdiplusReady) {
            Gdiplus::GdiplusShutdown(gdiplusToken);
        }
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
        if (gdiplusReady) {
            Gdiplus::GdiplusShutdown(gdiplusToken);
        }
        return;
    }
    const auto owner = ::CreateWindowExW(
        0, kSplashOwnerClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (owner == nullptr) {
        if (gdiplusReady) {
            Gdiplus::GdiplusShutdown(gdiplusToken);
        }
        return;
    }

    auto* const state = new SplashState{
        .title = toWide(title),
        .subtitle = toWide(subtitle),
        .version = toWide(std::string{"v"} + DVS_PROJECT_VERSION),
        .background = ::CreateSolidBrush(kSplashBackground),
        .titleFont = createFont(dpi, 24, FW_SEMIBOLD),
        .subtitleFont = createFont(dpi, 14, FW_NORMAL),
        .versionFont = createFont(dpi, 11, FW_NORMAL),
        .dpi = dpi,
        .clientWidth = width,
        .logoTop = scaled(kSplashLogoTop, dpi),
        .logoSize = scaled(kSplashLogoSize, dpi),
        .titleTop = scaled(kSplashTitleTop, dpi),
        .titleBottom = scaled(kSplashTitleBottom, dpi),
        .subtitleTop = scaled(kSplashSubtitleTop, dpi),
        .subtitleBottom = scaled(kSplashSubtitleBottom, dpi),
        .barX = scaled(kSplashBarMarginX, dpi),
        .barY = scaled(kSplashBarY, dpi),
        .barWidth = width - scaled(kSplashBarMarginX * 2, dpi),
        .thumbWidth = scaled(kSplashThumbWidth, dpi),
        .versionRight = width - scaled(kSplashVersionInset, dpi),
        .versionTop = scaled(kSplashVersionTop, dpi),
        .versionBottom = scaled(kSplashVersionBottom, dpi),
    };

    const auto window = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                          kSplashClassName,
                                          L"CompareStation",
                                          WS_POPUP,
                                          x,
                                          y,
                                          width,
                                          height,
                                          owner,
                                          nullptr,
                                          instance,
                                          state);
    if (window == nullptr) {
        destroySplashState(state);
        ::DestroyWindow(owner);
        if (gdiplusReady) {
            Gdiplus::GdiplusShutdown(gdiplusToken);
        }
        return;
    }
    hwnd_.store(window, std::memory_order_release);
    windowCreated_.store(true, std::memory_order_release);
    // Before the first paint (UpdateWindow below): the logo Bitmap and its
    // stream must be in place when WM_PAINT reads them.
    if (gdiplusReady) {
        loadLogoInto(*state);
    }

    // WS_EX_NOACTIVATE plus the direct paint keeps the splash from taking focus
    // away from whatever the user was doing.
    ::ShowWindow(window, SW_SHOWNOACTIVATE);
    ::UpdateWindow(window);
    // The loading bar sweeps while the user waits; the timer message is a
    // queued message the existing PeekMessage loop already drains, and dies
    // with the window, so it adds no teardown obligation. The animation anchor
    // is stored before the timer can produce the first animated paint.
    state->shownAt = ::GetTickCount64();
    ::SetTimer(window, kSplashAnimationTimer, kSplashAnimationInterval, nullptr);
    // The harness measures "spawn -> window" by walking the process's unowned top-level windows,
    // and the splash is deliberately owned so it is never mistaken for the review window. That
    // makes this milestone the only place the moment the first pixels actually reach the screen is
    // recorded.
    markStartupMilestone("splash-painted");

    // A dismiss that lands between the re-check above and the store of hwnd_ is one the caller
    // could not deliver: it read a null handle and posted nothing, so a blocking wait
    // would leave this thread waiting for a message nobody is going to send. That is a
    // hang, not a late splash - it surfaced as a test timeout in
    // DestructionRemovesTheWindowWithoutAnExplicitDismiss, and in production it would
    // freeze a launch torn down during startup. The loop re-reads the flag on every
    // pass and waits with a timeout rather than indefinitely, so teardown no longer
    // depends on the caller and the window handle being visible at the same instant.
    // It also subsumes the re-check before CreateWindowExW for anything that gets
    // this far.
    // The splash never assumes a dismissal will arrive. The dismissal owner is the
    // first presented frame of the main window, which normally lands in well under a
    // second; if rendering never gets that far (graphics failure, hung launch), a
    // topmost splash parked over the desktop forever would make the broken launch
    // worse instead of better. Once maxLifetime_ has passed since the window was
    // shown, the worker therefore closes itself through the same dismiss path.
    // That deadline check only helps if the loop top is reached regularly, so the
    // queue is drained with PeekMessage after the bounded wait instead of a blocking
    // GetMessage: a MsgWait wake can be consumed by a non-queued (sent) message that
    // GetMessage never returns, and then GetMessage parks forever and the deadline at
    // the loop top is missed - measured as the window staying up until another thread
    // posted WM_CLOSE, which is exactly what the ceiling must not depend on.
    const ULONGLONG shownAt = ::GetTickCount64();
    bool quitPosted = false;
    while (!quitPosted) {
        if (!dismissRequested_.load(std::memory_order_acquire) &&
            (::GetTickCount64() - shownAt) >= static_cast<ULONGLONG>(maxLifetime_.count())) {
            dismissRequested_.store(true, std::memory_order_release);
        }
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
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quitPosted = true;
                break;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }
    hwnd_.store(nullptr, std::memory_order_release);
    windowCreated_.store(false, std::memory_order_release);
    // WM_DESTROY above called PostQuitMessage, which is why the owner outlives
    // the loop by this one statement. Destroying it releases the only handle the
    // hidden owner held.
    ::DestroyWindow(owner);
    // Every GDI+ object (the logo Bitmap went away with SplashState inside
    // WM_NCDESTROY) is released by this point, so the shutdown is safe.
    if (gdiplusReady) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }
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
    // Thread entry copies Qt strings; a splash failure is non-fatal by design and Qt
    // terminates on OOM.
    // NOLINTBEGIN(bugprone-exception-escape)
    worker_ = std::thread{[this, title, subtitle] { runWorker(title, subtitle); }};
    // NOLINTEND(bugprone-exception-escape)
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
