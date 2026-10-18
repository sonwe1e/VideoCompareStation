#pragma once

// Foreign QML registrations that describe the Dvs.Ui module to QML tooling.
//
// This file is deliberately not part of any runtime translation unit's behaviour: the
// wrappers carry only metadata (QML type names, creatable/uncreatable, property and method
// descriptions) that qmltyperegistrar reads from the moc JSON to generate a .qmltypes for
// qmllint and qmlls. The runtime registration stays hand-written in
// src/ui_qml/src/QmlTypeRegistration.cpp, because the controllers are non-default-constructible
// in QML terms and the generated registration code would require dropping their `final`
// (a compile-time error provoked by QQmlPrivate::QQmlElement<T>); see
// docs/adr/0006-qml-type-registration.md for the evidence.
//
// Keep the names and creatability here identical to registerDvsUiTypes():
//   - ComparisonSurface / JogKeyHandler / HoldKeyHandler: creatable;
//   - ImageReviewController / ColorClipboardController: uncreatable, owned by the
//     composition root.

#include "dvs/ui/ColorClipboardController.h"
#include "dvs/ui/ComparisonSurface.h"
#include "dvs/ui/HoldKeyHandler.h"
#include "dvs/ui/ImageReviewController.h"
#include "dvs/ui/JogKeyHandler.h"

#include <QtQml/qqmlregistration.h>

namespace dvs::ui::qmltypes {

struct ComparisonSurfaceForeign {
    Q_GADGET
    QML_FOREIGN(dvs::ui::ComparisonSurface)
    QML_NAMED_ELEMENT(ComparisonSurface)
};

struct JogKeyHandlerForeign {
    Q_GADGET
    QML_FOREIGN(dvs::ui::JogKeyHandler)
    QML_NAMED_ELEMENT(JogKeyHandler)
};

struct HoldKeyHandlerForeign {
    Q_GADGET
    QML_FOREIGN(dvs::ui::HoldKeyHandler)
    QML_NAMED_ELEMENT(HoldKeyHandler)
};

struct ImageReviewControllerForeign {
    Q_GADGET
    QML_FOREIGN(dvs::ui::ImageReviewController)
    QML_NAMED_ELEMENT(ImageReviewController)
    QML_UNCREATABLE("provided by the CompareStation composition root")
};

struct ColorClipboardControllerForeign {
    Q_GADGET
    QML_FOREIGN(dvs::ui::ColorClipboardController)
    QML_NAMED_ELEMENT(ColorClipboardController)
    QML_UNCREATABLE("provided by the CompareStation composition root")
};

} // namespace dvs::ui::qmltypes
