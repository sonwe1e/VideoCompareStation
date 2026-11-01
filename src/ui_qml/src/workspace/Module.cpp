#include "dvs/ui/Module.h"

#include "dvs/application/Module.h"
#include "dvs/platform/Module.h"

namespace dvs::ui {

std::string_view moduleName() noexcept {
    if (application::moduleName() != "dvs_application" ||
        platform::moduleName() != "dvs_platform_windows") {
        return {};
    }
    return "dvs_ui_qml";
}

} // namespace dvs::ui
