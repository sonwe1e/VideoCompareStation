#include "dvs/ui/JogKeyHandler.h"

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
    static char applicationName[] = "JogKeyHandlerTests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application{argumentCount, arguments};
    static_cast<void>(application);
}

// Signal recorder: plain lambda connections keep this test free of the QtTest dependency.
class DirectionRecorder final {
public:
    explicit DirectionRecorder(JogKeyHandler& handler) {
        QObject::connect(&handler,
                         &JogKeyHandler::jogPressed,
                         &handler,
                         [this](const int direction) { pressed_.push_back(direction); });
        QObject::connect(&handler,
                         &JogKeyHandler::jogReleased,
                         &handler,
                         [this](const int direction) { released_.push_back(direction); });
    }

    [[nodiscard]] const std::vector<int>& pressed() const noexcept {
        return pressed_;
    }

    [[nodiscard]] const std::vector<int>& released() const noexcept {
        return released_;
    }

private:
    std::vector<int> pressed_;
    std::vector<int> released_;
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

class JogKeyHandlerTests : public testing::Test {
protected:
    void SetUp() override {
        ensureCoreApplication();
    }

    static void armHandler(JogKeyHandler& handler, QObject& target) {
        handler.setWindow(&target);
        handler.setEnabled(true);
        handler.setForwardEnabled(true);
        handler.setBackwardEnabled(true);
    }
};

TEST_F(JogKeyHandlerTests, BareFrameKeysMapToDirectionsAndAreConsumed) {
    QObject target;
    JogKeyHandler handler;
    armHandler(handler, target);
    DirectionRecorder recorder{handler};

    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_Left));
    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_Right));
    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_A));
    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_D));
    EXPECT_EQ(recorder.pressed(), (std::vector<int>{-1, 1, -1, 1}));
}

TEST_F(JogKeyHandlerTests, ReleaseCompletesTheHoldAndRepeatPressesAreSuppressed) {
    QObject target;
    JogKeyHandler handler;
    armHandler(handler, target);
    DirectionRecorder recorder{handler};

    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_Right));
    // OS auto-repeat: consumed, but it must never re-trigger a jog start.
    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier, true));
    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier, true));
    EXPECT_EQ(recorder.pressed().size(), 1U);
    // A repeat's synthetic release does not end the hold either.
    EXPECT_TRUE(sendKey(target, QEvent::KeyRelease, Qt::Key_Right, Qt::NoModifier, true));
    EXPECT_TRUE(recorder.released().empty());

    EXPECT_TRUE(sendKey(target, QEvent::KeyRelease, Qt::Key_Right));
    EXPECT_EQ(recorder.released(), (std::vector<int>{1}));
}

TEST_F(JogKeyHandlerTests, DisabledHandlerPassesEveryKeyThrough) {
    QObject target;
    JogKeyHandler handler;
    handler.setWindow(&target);
    handler.setEnabled(false);
    handler.setForwardEnabled(true);
    handler.setBackwardEnabled(true);
    DirectionRecorder recorder{handler};

    EXPECT_FALSE(sendKey(target, QEvent::KeyPress, Qt::Key_Left));
    EXPECT_FALSE(sendKey(target, QEvent::KeyRelease, Qt::Key_Left));
    EXPECT_TRUE(recorder.pressed().empty());
}

TEST_F(JogKeyHandlerTests, DirectionGatePassesDisabledDirectionsThrough) {
    QObject target;
    JogKeyHandler handler;
    handler.setWindow(&target);
    handler.setEnabled(true);
    handler.setForwardEnabled(true);
    handler.setBackwardEnabled(false);
    DirectionRecorder recorder{handler};

    EXPECT_FALSE(sendKey(target, QEvent::KeyPress, Qt::Key_Left));
    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_Right));
    EXPECT_EQ(recorder.pressed(), (std::vector<int>{1}));
}

TEST_F(JogKeyHandlerTests, ModifiedKeysAreNotJogKeys) {
    QObject target;
    JogKeyHandler handler;
    armHandler(handler, target);
    DirectionRecorder recorder{handler};

    // Shift+Left is the five-frame step, Ctrl+Right the one-second step: untouched.
    EXPECT_FALSE(sendKey(target, QEvent::KeyPress, Qt::Key_Left, Qt::ShiftModifier));
    EXPECT_FALSE(sendKey(target, QEvent::KeyPress, Qt::Key_Right, Qt::ControlModifier));
    EXPECT_TRUE(recorder.pressed().empty());
}

TEST_F(JogKeyHandlerTests, PlayerPresetAliasesOnlyMapWhenEnabled) {
    QObject target;
    JogKeyHandler handler;
    armHandler(handler, target);
    DirectionRecorder recorder{handler};

    EXPECT_FALSE(sendKey(target, QEvent::KeyPress, Qt::Key_Comma));
    EXPECT_FALSE(sendKey(target, QEvent::KeyPress, Qt::Key_Period));
    EXPECT_TRUE(recorder.pressed().empty());

    handler.setPlayerPresetKeys(true);
    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_Comma));
    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_Period));
    EXPECT_EQ(recorder.pressed(), (std::vector<int>{-1, 1}));
}

TEST_F(JogKeyHandlerTests, WindowDeactivationReleasesHeldKeys) {
    QObject target;
    JogKeyHandler handler;
    armHandler(handler, target);
    DirectionRecorder recorder{handler};

    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_Left));
    QEvent deactivate{QEvent::WindowDeactivate};
    static_cast<void>(QCoreApplication::sendEvent(&target, &deactivate));
    QCoreApplication::processEvents();
    EXPECT_EQ(recorder.released(), (std::vector<int>{-1}));

    // The stale physical release after focus loss is not owned anymore: pass it through.
    EXPECT_FALSE(sendKey(target, QEvent::KeyRelease, Qt::Key_Left));
    EXPECT_EQ(recorder.released().size(), 1U);
}

TEST_F(JogKeyHandlerTests, DisablingReleasesHeldKeys) {
    QObject target;
    JogKeyHandler handler;
    armHandler(handler, target);
    DirectionRecorder recorder{handler};

    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_A));
    handler.setEnabled(false);
    EXPECT_EQ(recorder.released(), (std::vector<int>{-1}));
}

TEST_F(JogKeyHandlerTests, OnlyTheWatchedWindowIsFiltered) {
    QObject target;
    JogKeyHandler handler;
    QObject other;
    armHandler(handler, target);
    DirectionRecorder recorder{handler};

    EXPECT_FALSE(sendKey(other, QEvent::KeyPress, Qt::Key_Left));
    EXPECT_TRUE(recorder.pressed().empty());

    EXPECT_TRUE(sendKey(target, QEvent::KeyPress, Qt::Key_Left));
    EXPECT_EQ(recorder.pressed().size(), 1U);
}

} // namespace
} // namespace dvs::ui
