#pragma once

#include <QObject>
#include <QVariantMap>

class QString;

namespace dvs::ui {

class ColorClipboardController;
class ImageReviewController;

// Main.qml declares the pilot's required services as its own typed, required properties
// (stillImageController / colorClipboardService). The composition root supplies them as
// the root object's initial properties; every embedding that loads Main.qml builds the
// map through this one function, so the contract is spelled in exactly one place.
[[nodiscard]] QVariantMap mainWindowPilotProperties(ImageReviewController& imageReview,
                                                    ColorClipboardController& colorClipboard);

// Verifies that a created Main.qml root actually received exactly these services.
//
// A required property fails creation when it is not set at all, but two weaker states
// survive creation: an explicit null initial property, and an object of the wrong type
// (Qt reports "Unable to assign ..." / "Could not set initial property" and degrades the
// property to null). The production loader calls this after creation and before showing
// the window; a test can call it directly with deliberately broken dependencies.
[[nodiscard]] bool verifyMainWindowPilotServices(const QObject& root,
                                                 ImageReviewController& expectedImageReview,
                                                 ColorClipboardController& expectedClipboard,
                                                 QString* error = nullptr);

} // namespace dvs::ui
