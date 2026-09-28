#pragma once

#include <string_view>

namespace dvs::app {

[[nodiscard]] int reportFatalStartup(std::string_view technicalDetail,
                                     bool suppressDialog) noexcept;

// Records a startup problem the process survives. A request that could not be opened belongs here
// rather than in reportFatalStartup: the window is up and usable, the application did start, and
// killing it would throw away the file the user picked along with the reason. The detail goes to
// the same log and to standard error, so a support question still has something to read.
void logStartupProblem(std::string_view technicalDetail) noexcept;

} // namespace dvs::app
