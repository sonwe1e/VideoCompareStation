#include "dvs/ui/HoldKeyHandler.h"

#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QObject>

#include <gtest/gtest.h>
#include <utility>
#include <vector>

namespace dvs::ui {
namespace {

void ensureCoreApplication() {
    if (QCoreApplication::instance() != nullptr) {
        return;
    }
    static int argumentCount = 1;
    static char applicationName[] = "HoldKeyHandlerTests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application{argumentCount, arguments};
    static_cast<void>(application);
}

// Signal recorder: plain lambda connections keep this test free of the QtTest dependency.
class HoldRecorder final {
public:
    explicit HoldRecorder(HoldKeyHandler& handler) {
        QObject::connect(
            &handler, &HoldKeyHandler::holdPressed, &handler, [this]() { pressed_ += 1; });
        QObject::connect(
            &handler, &HoldKeyHandler::holdReleased, &handler, [this]() { released_ += 1; });
    }

    [[nodiscard]] int pressed() const noexcept {
        return pressed_;
    }

    [[nodiscard]] int released() const noexcept {
        return released_;
    }

private:
    int pressed_ = 0;
    int released_ = 0;
};

// Sends a key event to the watched object and reports whether the handler consumed it.
[[nodiscard]] bool sendKey(QObject& watched,
                           const QEvent::Type type,
                           const int key,
                           const Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                           const bool autoRepeat = false) {
    QKeyEvent event{type, key, modifiers, QString(), autoRepeat};
    const bool consumed = QCoreApplication::sendEvent(&watched, &event);
    QCoreApplication::processEvents();
    return consumed;
}

class HoldKeyHandlerTests : public testing::Test {
protected:
    void SetUp() override {
        ensureCoreApplication();
    }

    static void
    armHandler(HoldKeyHandler& handler, QObject& target, const int key = Qt::Key_QuoteLeft) {
        handler.setWindow(&target);
        handler.setEnabled(true);
        handler.setHoldKey(key);
    }
};

TEST_F(HoldKeyHandlerTests, PressArmsHoldAndReleaseEndsIt) {
    QObject target;
    HoldKeyHandler handler;
    armHandler(handler, target);
    HoldRecorder recorder{handler};

    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_QuoteLeft));
    EXPECT_EQ(recorder.pressed(), 1);
    EXPECT_TRUE(sendKey(target, QEvent::KeyRelease, Qt::Key_QuoteLeft));
    EXPECT_EQ(recorder.released(), 1);
}

// The OS repeat clock emits repeat pairs of BOTH presses and releases; a repeat release is
// not the user letting go. Only the real release may end the hold.
TEST_F(HoldKeyHandlerTests, AutoRepeatPairsNeverEndOrRestartTheHold) {
    QObject target;
    HoldKeyHandler handler;
    armHandler(handler, target);
    HoldRecorder recorder{handler};

    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_QuoteLeft));
    EXPECT_EQ(recorder.pressed(), 1);
    // A full repeat cycle: repeat press, repeat release, repeat press again.
    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_QuoteLeft, Qt::NoModifier, true));
    EXPECT_TRUE(sendKey(target, QEvent::KeyRelease, Qt::Key_QuoteLeft, Qt::NoModifier, true));
    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_QuoteLeft, Qt::NoModifier, true));
    EXPECT_EQ(recorder.pressed(), 1) << "repeats must not re-arm the hold";
    EXPECT_EQ(recorder.released(), 0) << "a repeat release must not end the hold";
    // The real release still ends it exactly once.
    EXPECT_TRUE(sendKey(target, QEvent::KeyRelease, Qt::Key_QuoteLeft));
    EXPECT_EQ(recorder.released(), 1);
}

// Pressing a modifier mid-hold and releasing the hold key with it still held must end the
// hold: gating the release by "unmodified" would pin the raw view on screen forever.
TEST_F(HoldKeyHandlerTests, ModifiedReleaseStillEndsAnArmedHold) {
    QObject target;
    HoldKeyHandler handler;
    armHandler(handler, target);
    HoldRecorder recorder{handler};

    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_QuoteLeft));
    // Shift and Ctrl variants of the release both count as the real release.
    EXPECT_TRUE(sendKey(target, QEvent::KeyRelease, Qt::Key_QuoteLeft, Qt::ShiftModifier));
    EXPECT_EQ(recorder.released(), 1);
    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_QuoteLeft));
    EXPECT_TRUE(sendKey(target, QEvent::KeyRelease, Qt::Key_QuoteLeft, Qt::ControlModifier));
    EXPECT_EQ(recorder.released(), 2);
}

// Modified presses never START a hold (Shift+` is the lock, a different action) and their
// releases pass through untouched so the Shortcuts keep working.
TEST_F(HoldKeyHandlerTests, ModifiedPressNeverStartsAHoldAndPassesThrough) {
    QObject target;
    HoldKeyHandler handler;
    armHandler(handler, target);
    HoldRecorder recorder{handler};

    EXPECT_FALSE(sendKey(target, QEvent::KeyPress, Qt::Key_QuoteLeft, Qt::ShiftModifier));
    EXPECT_FALSE(sendKey(target, QEvent::KeyRelease, Qt::Key_QuoteLeft, Qt::ShiftModifier));
    EXPECT_EQ(recorder.pressed(), 0);
    EXPECT_EQ(recorder.released(), 0);
}

TEST_F(HoldKeyHandlerTests, OtherKeysAndUnarmedStatesPassThrough) {
    QObject target;
    HoldKeyHandler handler;
    armHandler(handler, target);
    HoldRecorder recorder{handler};

    EXPECT_FALSE(sendKey(target, QEvent::KeyPress, Qt::Key_A));
    EXPECT_FALSE(sendKey(target, QEvent::KeyRelease, Qt::Key_A));
    EXPECT_EQ(recorder.pressed(), 0);

    // While disabled, even the hold key belongs to someone else.
    handler.setEnabled(false);
    EXPECT_FALSE(sendKey(target, QEvent::KeyPress, Qt::Key_QuoteLeft));
    EXPECT_EQ(recorder.pressed(), 0);

    // A release for a hold that never started is not ours either.
    handler.setEnabled(true);
    EXPECT_FALSE(sendKey(target, QEvent::KeyRelease, Qt::Key_QuoteLeft));
    EXPECT_EQ(recorder.released(), 0);
}

// Losing the OS focus (Alt+Tab) ends the hold even though no key release arrives while the
// window is inactive; the raw view must never stay pinned on screen.
TEST_F(HoldKeyHandlerTests, WindowDeactivationReleasesTheHold) {
    QObject target;
    HoldKeyHandler handler;
    armHandler(handler, target);
    HoldRecorder recorder{handler};

    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_QuoteLeft));
    QEvent deactivate{QEvent::WindowDeactivate};
    QCoreApplication::sendEvent(&target, &deactivate);
    QCoreApplication::processEvents();
    EXPECT_EQ(recorder.released(), 1);
}

// Disabling the watcher (availability dropped) and reparenting to another window both end an
// armed hold instead of leaving it dangling.
TEST_F(HoldKeyHandlerTests, DisableAndWindowChangeReleaseTheHold) {
    QObject first;
    QObject second;
    HoldKeyHandler handler;
    armHandler(handler, first);
    HoldRecorder recorder{handler};

    EXPECT_TRUE(sendKey(first, QEvent::KeyPress, Qt::Key_QuoteLeft));
    handler.setEnabled(false);
    EXPECT_EQ(recorder.released(), 1);

    handler.setEnabled(true);
    EXPECT_TRUE(sendKey(first, QEvent::KeyPress, Qt::Key_QuoteLeft));
    handler.setWindow(&second);
    EXPECT_EQ(recorder.released(), 2);
    // The new window is watched from scratch: a press there arms a fresh hold.
    EXPECT_TRUE(sendKey(second, QEvent::KeyPress, Qt::Key_QuoteLeft));
    EXPECT_EQ(recorder.pressed(), 3);
}

// Changing the watched key releases an armed hold rather than reinterpreting it.
TEST_F(HoldKeyHandlerTests, HoldKeyChangeReleasesTheHold) {
    QObject target;
    HoldKeyHandler handler;
    armHandler(handler, target);
    HoldRecorder recorder{handler};

    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_QuoteLeft));
    handler.setHoldKey(Qt::Key_Tab);
    EXPECT_EQ(recorder.released(), 1);
}

} // namespace
} // namespace dvs::ui
