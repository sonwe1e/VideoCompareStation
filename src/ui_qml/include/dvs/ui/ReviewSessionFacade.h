#pragma once

#include "dvs/ui/ReviewController.h"
#include "dvs/ui/ReviewPreferencesController.h"
#include "dvs/ui/ReviewShellController.h"

#include <QObject>

namespace dvs::ui {

// Composition object for QML: the three capabilities the interface actually reads. The facade once
// exposed one property per migration step and carried a PlaybackViewModel projection on top of the
// review controller; nothing ever read either, so they are gone instead of waiting for code that
// was never written against them. Add a property back together with its first reader.
class ReviewSessionFacade final : public QObject {
    Q_OBJECT

    Q_PROPERTY(ReviewController* playback READ playback CONSTANT)
    Q_PROPERTY(ReviewPreferencesController* comparison READ comparison CONSTANT)
    Q_PROPERTY(ReviewShellController* shell READ shell CONSTANT)

public:
    ReviewSessionFacade(ReviewController& review,
                        ReviewPreferencesController& preferences,
                        ReviewShellController& shell,
                        QObject* parent = nullptr);

    [[nodiscard]] ReviewController* playback() const noexcept;
    [[nodiscard]] ReviewPreferencesController* comparison() const noexcept;
    [[nodiscard]] ReviewShellController* shell() const noexcept;

private:
    ReviewController& review_;
    ReviewPreferencesController& preferences_;
    ReviewShellController& shell_;
};

} // namespace dvs::ui
