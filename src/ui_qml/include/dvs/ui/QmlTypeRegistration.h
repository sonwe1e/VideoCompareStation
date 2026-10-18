#pragma once

namespace dvs::ui {

// Registers every QML type of the Dvs.Ui module exactly once per process.
//
// The composition root and every embedding that loads Main.qml (the desktop app, the
// contract-test harnesses) must call this before the first QQmlEngine exists; nothing else
// registers Dvs.Ui members. The registration is the only place where the module's runtime
// surface is spelled out:
//
//   - ComparisonSurface, JogKeyHandler and HoldKeyHandler are creatable from QML;
//   - ImageReviewController and ColorClipboardController are uncreatable: the composition
//     root owns the instances and hands them to QML through typed properties.
//
// The tooling-side description of the same module (qmllint, qmlls) is generated at build
// time from src/ui_qml/qmltypes/DvsQmlTypes.h into <build>/qml-imports/Dvs/Ui; see
// docs/adr/0006-qml-type-registration.md. Keep both surfaces in sync when a type is added.
void registerDvsUiTypes();

} // namespace dvs::ui
