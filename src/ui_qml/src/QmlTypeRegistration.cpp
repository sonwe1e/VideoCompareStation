#include "dvs/ui/QmlTypeRegistration.h"

#include "dvs/ui/ColorClipboardController.h"
#include "dvs/ui/ComparisonSurface.h"
#include "dvs/ui/HoldKeyHandler.h"
#include "dvs/ui/ImageReviewController.h"
#include "dvs/ui/JogKeyHandler.h"

#include <QString>
#include <QtQml/qqml.h>

#include <mutex>

namespace dvs::ui {
namespace {

constexpr auto kUiModuleUri = "Dvs.Ui";
constexpr int kUiModuleMajorVersion = 1;
constexpr int kUiModuleMinorVersion = 0;

// Reason shown when QML tries to instantiate a composition-root-owned controller.
constexpr auto kCompositionRootOwned = "provided by the CompareStation composition root";

} // namespace

void registerDvsUiTypes() {
    static std::once_flag registered;
    std::call_once(registered, [] {
        static_cast<void>(qmlRegisterType<ComparisonSurface>(
            kUiModuleUri, kUiModuleMajorVersion, kUiModuleMinorVersion, "ComparisonSurface"));
        static_cast<void>(qmlRegisterType<JogKeyHandler>(
            kUiModuleUri, kUiModuleMajorVersion, kUiModuleMinorVersion, "JogKeyHandler"));
        static_cast<void>(qmlRegisterType<HoldKeyHandler>(
            kUiModuleUri, kUiModuleMajorVersion, kUiModuleMinorVersion, "HoldKeyHandler"));
        // The controllers are registered so typed QML properties can name them; QML can
        // never create them, the composition root keeps ownership of the instances.
        static_cast<void>(qmlRegisterUncreatableType<ImageReviewController>(
            kUiModuleUri,
            kUiModuleMajorVersion,
            kUiModuleMinorVersion,
            "ImageReviewController",
            QString::fromLatin1(kCompositionRootOwned)));
        static_cast<void>(qmlRegisterUncreatableType<ColorClipboardController>(
            kUiModuleUri,
            kUiModuleMajorVersion,
            kUiModuleMinorVersion,
            "ColorClipboardController",
            QString::fromLatin1(kCompositionRootOwned)));
    });
}

} // namespace dvs::ui
