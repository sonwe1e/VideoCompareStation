// This translation unit includes <windows.h>, and that forbids the multi-line
// streaming assertion form used elsewhere in the suite:
//
//     EXPECT_TRUE(x)
//         << "message";          // C1057 once windows.h is in this TU
//
// The preprocessor loses track of the macro expansion and reports
// "unexpected end of file in macro expansion", or under -scanDependencies a
// bare C1903 with no underlying diagnostic. The assertion and its message must
// stay on one physical line here.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
// clang-format on
#endif

#include "dvs/ui/StartupSplash.h"

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace {

// The class name the splash worker registers. Kept as a literal here on purpose:
// the production constant lives in an anonymous namespace, and a test that
// imported the product's own constant could not catch a rename that left the
// window unfindable.
constexpr wchar_t kSplashClass[] = L"CompareStationStartupSplash";

int countSplashWindows() {
    int found = 0;
    for (auto window = ::FindWindowExW(nullptr, nullptr, kSplashClass, nullptr); window != nullptr;
         window = ::FindWindowExW(nullptr, window, kSplashClass, nullptr)) {
        ++found;
    }
    return found;
}

// FindWindowExW can see the splash the instant CreateWindowExW returns, which is
// a few instructions before the worker stores windowCreated_. Polling the window
// alone therefore raced the flag. Require both conditions together.
template <typename Predicate>
bool waitFor(Predicate predicate, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (predicate()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace

// The splash exists to cover a GUI thread that is synchronously blocked for
// ~730 ms. A QQuickWindow cannot paint before the event loop runs, so the splash
// has to be a real window on its own thread; this is the assertion that it
// actually is one, created while the calling thread stays put.
TEST(StartupSplashTests, ShowCreatesARealNativeWindowWithoutTouchingTheCallerThread) {
    ASSERT_EQ(countSplashWindows(), 0) << "a splash window leaked in from another test";
    {
        dvs::ui::StartupSplash splash;
        splash.show("CompareStation", "starting up");
        const auto opened =
            waitFor([&] { return countSplashWindows() > 0 && splash.windowVisible(); },
                    std::chrono::milliseconds(5000));
        ASSERT_TRUE(opened) << "show never produced a native splash window";

        splash.requestDismiss();
        splash.join();
        const auto closed =
            waitFor([&] { return countSplashWindows() == 0 && !splash.windowVisible(); },
                    std::chrono::milliseconds(5000));
        ASSERT_TRUE(closed) << "dismiss left the splash window on screen";
    }
    EXPECT_EQ(countSplashWindows(), 0);
}

// Regression guard for a defect this splash actually introduced. Adding an unowned top-level window
// made Process.CloseMainWindow() - and therefore the startup measurement harness - send WM_CLOSE to
// the splash instead of the review window. Every round then ended in a hard kill (exit code -1) and
// the playback trace was never flushed. An owned window is skipped by that search, so this asserts
// the ownership the fix depends on.
TEST(StartupSplashTests, SplashWindowIsOwnedSoItIsNeverTheProcessMainWindow) {
    dvs::ui::StartupSplash splash;
    splash.show("CompareStation", "ownership");
    const auto opened =
        waitFor([&] { return countSplashWindows() > 0; }, std::chrono::milliseconds(5000));
    ASSERT_TRUE(opened);

    const auto window = ::FindWindowExW(nullptr, nullptr, kSplashClass, nullptr);
    ASSERT_NE(window, nullptr);
    EXPECT_NE(::GetWindow(window, GW_OWNER), nullptr)
        << "an unowned splash can be picked as the process main window and swallows WM_CLOSE";

    splash.requestDismiss();
    splash.join();
    EXPECT_TRUE(
        waitFor([&] { return countSplashWindows() == 0; }, std::chrono::milliseconds(5000)));
}

// The splash must not steal focus from whatever the user was doing, and must not
// show up in alt-tab or the taskbar: the main window is shown and activated a few
// hundred milliseconds later, and a splash that appeared in either list would
// leave a phantom entry behind.
TEST(StartupSplashTests, SplashWindowDoesNotActivateAndIsExcludedFromTheShellLists) {
    // Capture the foreground window first. GetForegroundWindow() is legitimately
    // null on some session types, so comparing it against null would fail for a
    // reason that has nothing to do with the splash.
    const auto foregroundBefore = ::GetForegroundWindow();
    dvs::ui::StartupSplash splash;
    splash.show("CompareStation", "starting up");
    const auto opened = waitFor([&] { return countSplashWindows() > 0 && splash.windowVisible(); },
                                std::chrono::milliseconds(5000));
    ASSERT_TRUE(opened);

    const auto window = ::FindWindowExW(nullptr, nullptr, kSplashClass, nullptr);
    ASSERT_NE(window, nullptr);
    const auto exStyle = ::GetWindowLongW(window, GWL_EXSTYLE);
    EXPECT_NE(exStyle & WS_EX_TOOLWINDOW, 0UL) << "splash appears in the taskbar";
    EXPECT_NE(exStyle & WS_EX_NOACTIVATE, 0UL) << "splash can steal focus from the user";
    EXPECT_NE(exStyle & WS_EX_TOPMOST, 0UL) << "the splash must stay above the window it covers";
    EXPECT_EQ(::GetForegroundWindow(), foregroundBefore)
        << "showing the splash moved the foreground window";

    splash.requestDismiss();
    splash.join();
    EXPECT_TRUE(
        waitFor([&] { return countSplashWindows() == 0; }, std::chrono::milliseconds(5000)));
}

// Every early return in DesktopApplication::load() dismisses through the guard,
// which can dismiss before it has been shown. That path has to be inert, and a
// late worker must not resurrect a window after the decision to dismiss.
TEST(StartupSplashTests, DismissBeforeShowIsInertAndRepeatable) {
    dvs::ui::StartupSplash splash;
    splash.requestDismiss();
    splash.requestDismiss();
    splash.join();
    splash.join();
    EXPECT_FALSE(splash.windowVisible());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(countSplashWindows(), 0) << "a dismiss that preceded show still opened a window";

    // And a dismiss racing the worker must still leave nothing behind.
    splash.show("CompareStation", "racing dismiss");
    splash.requestDismiss();
    splash.join();
    EXPECT_TRUE(waitFor([&] { return countSplashWindows() == 0 && !splash.windowVisible(); },
                        std::chrono::milliseconds(5000)));
}

// The guard in DesktopApplication is the last line of defence, so destruction
// alone has to remove the window.
TEST(StartupSplashTests, DestructionRemovesTheWindowWithoutAnExplicitDismiss) {
    {
        dvs::ui::StartupSplash splash;
        splash.show("CompareStation", "leaking");
        ASSERT_TRUE(
            waitFor([&] { return countSplashWindows() > 0; }, std::chrono::milliseconds(5000)));
    }
    ASSERT_TRUE(waitFor([] { return countSplashWindows() == 0; }, std::chrono::milliseconds(5000)))
        << "StartupSplash left a window behind when it went out of scope";
}

// A launch can be torn down while the splash worker is still creating its window. Dropping the
// splash without waiting exercises that path repeatedly, and the invariant is that no window is
// ever left behind. Be clear about what this does and does not cover: it is a repeated-drop smoke
// test, NOT coverage for the teardown race. Creating a std::thread costs far more than the window
// between the worker's last flag check and the publication of its handle, so the destructor almost
// always wins that race and the worker takes the early return instead. Both the plain-GetMessage
// loop and a flag-blind wait pass here - see
// out/verification/a4b-foreground-handover/mutate-splash-strand.ps1, which records that. What is
// verified is the postcondition, not the race.
TEST(StartupSplashTests, RepeatedCreateAndDropNeverLeavesAWindowBehind) {
    constexpr int kIterations = 150;
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        dvs::ui::StartupSplash splash;
        splash.show("CompareStation", "repeated drop");
        // No wait on purpose: the destructor races the worker through window creation.
    }
    EXPECT_EQ(countSplashWindows(), 0) << "a dropped splash left a window behind";
}