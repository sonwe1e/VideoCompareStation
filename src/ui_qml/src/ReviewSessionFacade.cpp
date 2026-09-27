#include "dvs/ui/ReviewSessionFacade.h"

#include "dvs/ui/ReviewController.h"
#include "dvs/ui/ReviewPreferencesController.h"
#include "dvs/ui/ReviewShellController.h"

namespace dvs::ui {

ReviewSessionFacade::ReviewSessionFacade(ReviewController& review,
                                         ReviewPreferencesController& preferences,
                                         ReviewShellController& shell,
                                         QObject* const parent)
    : QObject(parent), review_(review), preferences_(preferences), shell_(shell) {}

ReviewController* ReviewSessionFacade::playback() const noexcept {
    return &review_;
}

ReviewPreferencesController* ReviewSessionFacade::comparison() const noexcept {
    return &preferences_;
}

ReviewShellController* ReviewSessionFacade::shell() const noexcept {
    return &shell_;
}

} // namespace dvs::ui
