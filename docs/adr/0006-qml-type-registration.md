# ADR 0006: QML type registration entry and the typed-interface pilot

Status: accepted; extended 2026-10-17 with the real-material test runner (R1), the
production dependency contract (R2) and the generated-artifact contract (R3).

Plan references: interface-reliability workstream P1 pilot — the image probe's numeric-format
switch and colour copy are the first feature carried by a typed QML boundary.

## Context

`Main.qml` and its siblings read `Dvs.Ui` types through context properties registered by hand
(`qmlRegisterType<ComparisonSurface>` …). That surface is invisible to QML tooling:

- `qmllint` cannot resolve `import Dvs.Ui 1.0`, so every use site was wrapped in
  `qmllint disable import` / `unqualified` suppressions. A typo such as
  `ComparisonSurface.Wipee` silently evaluates to `undefined` at runtime and no automated gate
  sees it.
- The pilot feature (image-probe numeric format and colour copy) lives on
  `ImageReviewController`, likewise reachable only through untyped `var` properties.

Constraints discovered while validating on the pinned Qt 6.11.1 (vcpkg):

- `dvs_ui_models` must stay Qt Core-only (`core-dev`/`ui-dev` boundaries); the controllers live
  in `dvs_ui_d3d11_bridge`.
- `QML_FOREIGN` + `qt_add_qml_module` cannot be used as-is: the registrar-generated C++
  registration instantiates `QQmlPrivate::QQmlElement<T>` even for `QML_UNCREATABLE` types, and
  the controllers are `final` (MSVC error C3246). Generating and compiling that code would force
  dropping `final` from production classes.
- QML-only harnesses (stock `qmltestrunner`) instantiate `ImageWorkspace` with `QtObject`
  stand-ins. A typed QObject property rejects a stand-in **at component compile time**
  ("Cannot assign object of type QtObject to property of type …"), proven with a scratch probe.

## Decision

1. **Single runtime registration entry.** `dvs::ui::registerDvsUiTypes()`
   (`src/ui_qml/src/QmlTypeRegistration.cpp`, called by `DesktopApplication` and every C++
   harness) is the only place `Dvs.Ui` is registered. `ComparisonSurface`, `JogKeyHandler` and
   `HoldKeyHandler` stay creatable; `ImageReviewController` and `ColorClipboardController` are
   registered as uncreatable — the composition root owns the instances.
2. **Tooling description module.** `dvs_ui_qml_types` (static, `src/ui_qml/qmltypes/`) carries
   `QML_FOREIGN` wrappers; `qmltyperegistrar` generates `dvs.qmltypes` into
   `<build>/qml-imports/Dvs/Ui/` from the bridge's and the wrapper target's moc JSON. Only the
   description is generated — the generated registration C++ is deliberately not compiled.
   `cmake/NormalizeQmlTypes.cmake` rewrites moc's `qreal` spelling to the QML tooling spelling
   `double` and fails if an unresolvable spelling survives. The hand-written `qmldir` declares
   `depends QtQuick auto` (same shape as the Qt installation's own modules), which is what makes
   the `ComparisonSurface → QQuickItem` prototype chain resolvable.
3. **Lint integration.** `dvs_lint_qml` gains `-I <DVS_QML_TYPES_IMPORT_DIR>` and depends on the
   generation target; the five import suppressions are removed. `qmllint` now checks the module's
   enum namespaces and every member access through a typed property.
4. **Typed pilot boundary.** `Main.qml` declares its two pilot services as *required,
   typed* root properties (`stillImageController`, `colorClipboardService`); the
   composition root passes the real adapters as the root's initial properties. A missing
   property fails creation ("Required property … was not initialized"); an explicit null
   or a wrong-typed object only warns and degrades the property to null, so
   `verifyMainWindowPilotServices` (src/ui_qml/src/MainWindowDependencies.cpp) re-checks
   identity before `DesktopApplication` shows the window. Tests build the same map through
   one helper (`pilotWindowProperties` / `defaultPilotWindowProperties` in the contract
   suite), and `MainWindowRequiredPilotServicesAreEnforced` pins the five creation
   outcomes against the real root.
5. **ImageWorkspace is typed, and its mock suite runs on real services.** The workspace's
   `controller` / `colorClipboard` are required typed properties and the pilot's
   `typeof … === "function"` guards and duplicate formatting fallbacks are gone (the
   explicit `numericFloat` read remains as the binding dependency that a C++ invokable
   cannot provide). Because a typed property rejects a QtObject stand-in at component
   compile time, `ui.ImageWorkspaceManual` moved from stock `qmltestrunner` to the
   repo-built QuickTest runner `tests/component/ui/ImageWorkspaceQmlTestMain.cpp`: it
   registers the real types, owns a real `ImageReviewController` and
   `ColorClipboardController`, fabricates the suite's small inputs through the real
   injection/open paths (including a marker-based loader for display-converted high-depth
   pairs) and serves the review image provider the workspace displays through.

### Deliberately not adopted (probe findings)

- Compiling the qmltyperegistrar-generated registration C++: rejected together with the
  `QQmlElement` constraint above.
- Loosening the typed boundaries again for stand-in harnesses: the manual suite was
  migrated to real services instead; a QtObject stand-in is rejected at component compile
  time, and dual typed/untyped code paths would put the pilot's formatting and copy calls
  back outside static checking.

## Consequences

- Adding a QML type now means touching both `registerDvsUiTypes()` and
  `src/ui_qml/qmltypes/DvsQmlTypes.h`; the registration contract test
  (`ui.QmlTypeRegistrationTests`) fails if the runtime entry no longer answers for a type,
  and `dvs_lint_qml` fails if the description drifts from the QML files' expectations.
  `GeneratedDescriptionMatchesTheRuntimeRegistration` additionally reads the generated
  artifact and compares names and creatability against the runtime registry, so the two
  declarations cannot drift apart silently.
- Constructions like `ComparisonSurface.Wipe` are checked again; member typos through
  `stillImageController` (Main.qml) and through the workspace's controller/clipboard
  (`channelText`, `copyText`, …) are lint errors (mutation evidence in
  `out/verification/`).
- Requires of the pilot services fail loudly: missing root properties fail creation,
  null/wrong-typed services are rejected before the window is shown, and the workspace
  receives real adapters in its own suite.
- The generation step validates the artifact contract (component set, creatability,
  key members, spellings) with counted checks before publishing, and
  `quality.qmltypes_contract` runs the same script against a valid baseline plus eight
  counterexamples (missing/duplicate component, missing member, flipped creatability,
  leftover C++ spelling, empty, truncated, already-normalized).
- The tooling description is build-tree-only: no new runtime files, DLLs or install rules;
  the release payload is unchanged by construction. The verification record lists the
  packaging checks explicitly instead of inferring them.

See [QML interface pilot verification](../engineering/qml-interface-pilot.md) for the probe
logs, mutation evidence and the residual gaps.
