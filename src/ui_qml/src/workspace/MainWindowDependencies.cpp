#include "dvs/ui/MainWindowDependencies.h"

#include "dvs/ui/ColorClipboardController.h"
#include "dvs/ui/ImageReviewController.h"

#include <QString>
#include <QVariant>

namespace dvs::ui {
namespace {

constexpr auto kImageReviewProperty = "stillImageController";
constexpr auto kClipboardProperty = "colorClipboardService";

// One property-level check: present, non-null and of the expected dynamic type.
[[nodiscard]] bool checkService(const QObject& root,
                                const char* propertyName,
                                const QObject* expected,
                                const char* expectedTypeName,
                                QString* error) {
    const QVariant value = root.property(propertyName);
    if (!value.isValid()) {
        if (error != nullptr) {
            *error = QStringLiteral("Main window property \"%1\" is not declared.")
                         .arg(QString::fromLatin1(propertyName));
        }
        return false;
    }
    const QObject* const installed = value.value<QObject*>();
    if (installed == nullptr) {
        if (error != nullptr) {
            *error =
                QStringLiteral("Main window did not receive its required %1: the property is null.")
                    .arg(QString::fromLatin1(expectedTypeName));
        }
        return false;
    }
    if (installed != expected) {
        if (error != nullptr) {
            *error =
                QStringLiteral(
                    "Main window property \"%1\" does not carry the composition root's %2.")
                    .arg(QString::fromLatin1(propertyName), QString::fromLatin1(expectedTypeName));
        }
        return false;
    }
    return true;
}

} // namespace

QVariantMap mainWindowPilotProperties(ImageReviewController& imageReview,
                                      ColorClipboardController& colorClipboard) {
    return {
        {QString::fromLatin1(kImageReviewProperty), QVariant::fromValue(&imageReview)},
        {QString::fromLatin1(kClipboardProperty), QVariant::fromValue(&colorClipboard)},
    };
}

bool verifyMainWindowPilotServices(const QObject& root,
                                   ImageReviewController& expectedImageReview,
                                   ColorClipboardController& expectedClipboard,
                                   QString* error) {
    if (!checkService(
            root, kImageReviewProperty, &expectedImageReview, "ImageReviewController", error)) {
        return false;
    }
    return checkService(
        root, kClipboardProperty, &expectedClipboard, "ColorClipboardController", error);
}

} // namespace dvs::ui
