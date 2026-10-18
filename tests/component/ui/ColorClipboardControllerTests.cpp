// Focused contract for the pixel-probe colour-copy adapter (dvs::ui::ColorClipboardController).
//
// The adapter is the single boundary the image workspace's copy path writes through, and it
// verifies its own write by reading the value back. These tests pin both directions as
// independent cases so neither can hide behind the other:
//   * the success case writes a distinct sentinel first, then requires a verified copy that
//     overwrites it with the payload and reports 已复制. It is skipped - never counted as a
//     pass - when the session has no GUI clipboard.
//   * the failure case injects a deterministic, controllable rejection (empty text) and
//     requires the explicit failure status with a false return and no clipboard disturbance.
// A copy implementation that always reports failure fails the success case here; a copy that
// reports success without writing fails the read-back assertion.

#include "dvs/ui/ColorClipboardController.h"
#include "dvs/ui/GraphicsBackend.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QString>
#include <QThread>

#include <gtest/gtest.h>

namespace dvs::ui {
namespace {

// The Windows OLE clipboard completes asynchronously, so a write is drained before it is
// read back; otherwise a stale value could be mistaken for the copy's result.
void pumpClipboard() {
    for (int iteration = 0; iteration < 10; ++iteration) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5U);
    }
}

constexpr auto kSentinel = "DVS-CLIPBOARD-SENTINEL-preexisting-content";
constexpr auto kSentinelKept = "DVS-CLIPBOARD-SENTINEL-must-survive";

} // namespace

// The success path is a mandatory acceptance condition: a verified copy must overwrite the
// sentinel with the exact payload and report success. The session's clipboard is confirmed
// writable first (retried against transient OLE contention); a session whose clipboard cannot
// be written at all is skipped - never counted as a pass - while a copy that always fails
// after a writable clipboard is a hard failure here.
TEST(ColorClipboardControllerTests, SuccessfulCopyOverwritesTheSentinelAndVerifies) {
    QClipboard* const clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        GTEST_SKIP() << "no GUI clipboard in this session: the copy success path cannot be "
                        "exercised here, so it is not counted as a functional pass";
    }
    ColorClipboardController controller;
    const QString sentinel = QString::fromLatin1(kSentinel);
    // Availability gate: write the sentinel DIRECTLY to the clipboard (not through the
    // controller), retried against transient OLE contention. This is deliberately independent
    // of the controller so a session with no usable clipboard is told apart from a copy that
    // fails to write: the former is skipped below, the latter is a hard failure.
    bool sentinelWritten = false;
    for (int attempt = 0; attempt < 5 && !sentinelWritten; ++attempt) {
        clipboard->setText(sentinel);
        pumpClipboard();
        sentinelWritten = (clipboard->text() == sentinel);
    }
    if (!sentinelWritten) {
        GTEST_SKIP() << "clipboard not writable in this session (transient or absent): "
                        "the copy success path is not exercised here";
    }

    // The real copy must overwrite the sentinel with the verified payload. A copy that always
    // fails keeps copied false and fails here; a transient miss is retried.
    const QString payload = QStringLiteral("rgb(0.047059, 0.133333, 0.219608)");
    bool copied = false;
    for (int attempt = 0; attempt < 3 && !copied; ++attempt) {
        copied = controller.copyText(payload);
        if (!copied) {
            pumpClipboard();
        }
    }
    EXPECT_TRUE(copied) << controller.statusText().toStdString();
    EXPECT_TRUE(controller.statusText().startsWith(QStringLiteral("已复制")))
        << controller.statusText().toStdString();
    EXPECT_EQ(clipboard->text(), payload)
        << "the copy must replace the sentinel with the verified payload";
}

// The failure path at a controllable boundary: an empty payload is rejected before any write,
// reported through statusText, returned as false, and must not disturb the clipboard.
TEST(ColorClipboardControllerTests, EmptyTextIsRejectedAndReportedWithoutWriting) {
    ColorClipboardController controller;
    QString before;
    if (QClipboard* const clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(QString::fromLatin1(kSentinelKept));
        pumpClipboard();
        before = clipboard->text();
    }
    EXPECT_FALSE(controller.copyText(QString()));
    EXPECT_EQ(controller.statusText(), QStringLiteral("没有可复制的颜色样本。"));
    if (QClipboard* const clipboard = QGuiApplication::clipboard()) {
        EXPECT_EQ(clipboard->text(), before)
            << "a rejected copy must leave the clipboard exactly as it was";
    }
}

} // namespace dvs::ui

int main(int argumentCount, char* arguments[]) {
    dvs::ui::configureGraphicsBackend();
    QGuiApplication application{argumentCount, arguments};
    testing::InitGoogleTest(&argumentCount, arguments);
    return RUN_ALL_TESTS();
}
