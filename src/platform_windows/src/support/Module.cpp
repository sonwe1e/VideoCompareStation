#include "dvs/platform/Module.h"

#include "dvs/application/Module.h"

namespace dvs::platform {

std::string_view moduleName() noexcept {
    if (application::moduleName() != "dvs_application") {
        return {};
    }
    return "dvs_platform_windows";
}

} // namespace dvs::platform
