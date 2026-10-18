// Repo-built Qt Quick Test runner for the image-workspace suite (ui.ImageWorkspaceManual).
//
// The stock qmltestrunner cannot register the Dvs.Ui types, and a typed ImageWorkspace
// rejects a QtObject stand-in controller at component compile time. This runner therefore
// mirrors the production composition root:
//   - registers the real Dvs.Ui types before any engine exists,
//   - owns a real ImageReviewController and ColorClipboardController,
//   - exposes a fixture that fabricates small deterministic inputs through the controller's
//     real injection/open paths (a marker-based still-image loader serves high-depth pairs
//     with unquantized 16-bit sidecars),
//   - serves the review image provider the workspace displays through.
//
// The suite keeps driving real mouse/keyboard events; only the material source moved from a
// hand-rolled mock to the real controller.

#include "dvs/ui/ColorClipboardController.h"
#include "dvs/ui/GraphicsBackend.h"
#include "dvs/ui/ImageReviewController.h"
#include "dvs/ui/QmlTypeRegistration.h"
#include "dvs/ui/ReviewImageProvider.h"

#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QtQuickTest/quicktest.h>

#include <algorithm>
#include <chrono>

namespace {

using dvs::ui::ColorClipboardController;
using dvs::ui::ImageReviewController;
using dvs::ui::StillImageSourceInfo;

// Marker format written by the fixture: "dvs-ws-image,<w>,<h>,<r16>,<g16>,<b16>,<a16>".
constexpr auto kSyntheticMarker = "dvs-ws-image";

[[nodiscard]] QImage solidImage(const int width, const int height, const QRgb pixel) {
    QImage image(width, height, QImage::Format_ARGB32);
    if (!image.isNull()) {
        image.fill(pixel);
    }
    return image;
}

// Fabricates a display-converted high-depth source from the marker: the display buffer is
// the 8-bit view of the values, the sidecar carries the unquantized 16-bit codes - the
// same shape the FFmpeg-backed loader reports for a >8-bit file.
bool syntheticImageLoader(const QByteArray& bytes,
                          QImage* image,
                          QImage* nativeImage,
                          StillImageSourceInfo* info,
                          std::string*) {
    if (image == nullptr) {
        return false;
    }
    const QList<QByteArray> parts = bytes.split(',');
    if (parts.size() != 7 || parts.at(0).trimmed() != kSyntheticMarker) {
        return false;
    }
    bool ok = true;
    int values[6] = {0, 0, 0, 0, 0, 0};
    for (int index = 0; index < 6; ++index) {
        values[index] = parts.at(1 + index).trimmed().toInt(&ok);
        if (!ok) {
            return false;
        }
    }
    const int width = values[0];
    const int height = values[1];
    const int r16 = values[2];
    const int g16 = values[3];
    const int b16 = values[4];
    const int a16 = values[5];
    if (width <= 0 || height <= 0) {
        return false;
    }
    *image = solidImage(width, height, qRgba(r16 >> 8, g16 >> 8, b16 >> 8, a16 >> 8));
    if (image->isNull()) {
        return false;
    }
    if (nativeImage != nullptr) {
        QImage native(width, height, QImage::Format_RGBA64);
        if (native.isNull()) {
            return false;
        }
        auto* const samples = reinterpret_cast<QRgba64*>(native.bits());
        std::fill(samples,
                  samples + native.sizeInBytes() / sizeof(QRgba64),
                  qRgba64(static_cast<quint16>(r16),
                          static_cast<quint16>(g16),
                          static_cast<quint16>(b16),
                          static_cast<quint16>(a16)));
        *nativeImage = std::move(native);
    }
    if (info != nullptr) {
        info->bitDepth = 16;
        info->channels = 4;
        info->hasAlpha = true;
        info->sourceFormat = QStringLiteral("rgba64le");
        info->displayConverted = true;
    }
    return true;
}

// Small material factory for the suite. Everything routes through the controller's real
// APIs: injected solids for the standard pairs, a marker file + the synthetic loader for
// the high-depth pair.
class ImageWorkspaceFixture final : public QObject {
    Q_OBJECT
    Q_PROPERTY(dvs::ui::ImageReviewController* controller READ controller CONSTANT)
    Q_PROPERTY(dvs::ui::ColorClipboardController* clipboard READ clipboard CONSTANT)

public:
    explicit ImageWorkspaceFixture(QObject* parent = nullptr) : QObject(parent) {}

    [[nodiscard]] ImageReviewController* controller() noexcept {
        return &controller_;
    }
    [[nodiscard]] ColorClipboardController* clipboard() noexcept {
        return &clipboard_;
    }

    // The suite's baseline pair: 64x64 solids A=(120,130,140,255), B=(122,128,140,255).
    Q_INVOKABLE bool openStandardPair() {
        return openPair(64, 64, {120, 130, 140, 255}, 64, 64, {122, 128, 140, 255});
    }

    // Explicit [r, g, b, a] integer channels: no hex-string colour-order ambiguity (Qt
    // parses 8-digit hex as #AARRGGBB, the readout prints #RRGGBBAA).
    Q_INVOKABLE bool openPair(const int primaryWidth,
                              const int primaryHeight,
                              const QVariantList& primaryRgba,
                              const int secondaryWidth,
                              const int secondaryHeight,
                              const QVariantList& secondaryRgba) {
        const QColor primary = channelColor(primaryRgba);
        const QColor secondary = channelColor(secondaryRgba);
        if (!primary.isValid() || !secondary.isValid()) {
            return false;
        }
        return controller_.openPairImages(
            solidImage(primaryWidth, primaryHeight, primary.rgba()),
            QStringLiteral("fixture-a"),
            solidImage(secondaryWidth, secondaryHeight, secondary.rgba()),
            QStringLiteral("fixture-b"));
    }

    // A display-converted high-depth pair: both sides uniform white, 65535 in every 16-bit
    // channel, served through the marker loader so the controller carries real RGBA64
    // sidecars (the readout's "converted 16-bit" details come from the unquantized codes).
    Q_INVOKABLE bool openHighDepthPair(const int width, const int height) {
        if (width <= 0 || height <= 0 || !tempDir_.isValid()) {
            return false;
        }
        const QString marker = QStringLiteral("%1,%2,%3,65535,65535,65535,65535")
                                   .arg(QString::fromLatin1(kSyntheticMarker))
                                   .arg(width)
                                   .arg(height);
        const QString primaryPath =
            tempDir_.filePath(QStringLiteral("highdepth-%1-a.bin").arg(highDepthSerial_));
        const QString secondaryPath =
            tempDir_.filePath(QStringLiteral("highdepth-%1-b.bin").arg(highDepthSerial_));
        ++highDepthSerial_;
        for (const QString& path : {primaryPath, secondaryPath}) {
            QFile file{path};
            if (!file.open(QIODevice::WriteOnly)) {
                return false;
            }
            static_cast<void>(file.write(marker.toUtf8()));
            file.close();
        }
        const int requestId = controller_.requestOpenPair(QUrl::fromLocalFile(primaryPath),
                                                          QUrl::fromLocalFile(secondaryPath));
        if (requestId <= 0) {
            return false;
        }
        if (!waitForIdle()) {
            return false;
        }
        return controller_.hasPair();
    }

    // The exact text the real clipboard adapter wrote and verified.
    Q_INVOKABLE QString clipboardText() const {
        const QClipboard* const clipboard = QGuiApplication::clipboard();
        return clipboard != nullptr ? clipboard->text() : QString();
    }

    // Writes text straight to the system clipboard and verifies it, independent of the copy
    // controller, so a test can tell an absent/unwritable clipboard (skip the success case)
    // apart from a copy that fails to write (a hard failure).
    Q_INVOKABLE bool setClipboardText(const QString& text) {
        QClipboard* const clipboard = QGuiApplication::clipboard();
        if (clipboard == nullptr) {
            return false;
        }
        clipboard->setText(text);
        return clipboard->text() == text;
    }

private:
    [[nodiscard]] static QColor channelColor(const QVariantList& rgba) {
        if (rgba.size() != 4) {
            return {};
        }
        bool ok = true;
        const int r = rgba.at(0).toInt(&ok);
        if (!ok) {
            return {};
        }
        const int g = rgba.at(1).toInt(&ok);
        if (!ok) {
            return {};
        }
        const int b = rgba.at(2).toInt(&ok);
        if (!ok) {
            return {};
        }
        const int a = rgba.at(3).toInt(&ok);
        if (!ok) {
            return {};
        }
        return QColor(r, g, b, a);
    }

    [[nodiscard]] bool waitForIdle() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while ((controller_.openPending() || controller_.diffPending()) &&
               std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
            QThread::msleep(1U);
        }
        return !controller_.openPending() && !controller_.diffPending();
    }

    ImageReviewController controller_;
    ColorClipboardController clipboard_;
    QTemporaryDir tempDir_;
    int highDepthSerial_ = 0;
};

class ImageWorkspaceQmlTestSetup final : public QObject {
    Q_OBJECT

public:
    explicit ImageWorkspaceQmlTestSetup(QObject* parent = nullptr) : QObject(parent) {}

public slots:
    // Runs after QGuiApplication exists and before any test file is parsed.
    void applicationAvailable() {
        dvs::ui::registerDvsUiTypes();
        ImageReviewController::setProcessStillImageLoader(&syntheticImageLoader);
        ImageReviewController::setProcessStillImageProbe(nullptr);
    }

    // Runs before each test file's engine loads it: everything the suite reads as an
    // unqualified name is installed here, exactly once per engine. The deployed Qt QML
    // modules live under <applicationDirPath>/qml (same as the production engine).
    void qmlEngineAvailable(QQmlEngine* engine) {
        if (engine == nullptr) {
            return;
        }
        engine->addImportPath(
            QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
        engine->rootContext()->setContextProperty(QStringLiteral("fixture"), &fixture_);
        engine->addImageProvider(QStringLiteral("vcs-review"),
                                 new dvs::ui::ReviewImageProvider(fixture_.controller()));
    }

    void cleanupTestCase() {
        ImageReviewController::setProcessStillImageLoader(nullptr);
    }

private:
    ImageWorkspaceFixture fixture_;
};

} // namespace

int main(int argc, char** argv) {
    dvs::ui::configureGraphicsBackend();
    QGuiApplication application{argc, argv};
    ImageWorkspaceQmlTestSetup setup;
    return quick_test_main_with_setup(argc, argv, "image_workspace_manual", nullptr, &setup);
}

#include "ImageWorkspaceQmlTestMain.moc"
