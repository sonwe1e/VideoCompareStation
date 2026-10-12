#pragma once

#include "dvs/application/FileAssociation.h"
#include "dvs/shell/FileAssociationRegistration.h"

#include <string_view>

namespace dvs::app {

// Bridges the shell adapter's free functions to the settings-dialog port. Only the composition
// root may construct adapters, so this shim lives in the application target: the shell adapter
// stays free of application types and the UI layer sees only IFileAssociationService.
//
// The executable of the running installation is derived once at construction, the same way the
// startup Explorer registration derives it. Methods are called from one worker thread at a time
// by FileAssociationController and only ever read that path.
class FileAssociationService final : public application::IFileAssociationService {
public:
    explicit FileAssociationService(std::string_view version);

    FileAssociationService(const FileAssociationService&) = delete;
    FileAssociationService& operator=(const FileAssociationService&) = delete;

    [[nodiscard]] application::FileAssociationOutcome registerAsOptionalApplication() override;
    [[nodiscard]] application::FileAssociationOutcome openSystemDefaultAppsSettings() override;
    [[nodiscard]] application::FileAssociationStatus associationStatus() override;
    [[nodiscard]] application::FileAssociationOutcome repairRegistrationPaths() override;
    [[nodiscard]] application::FileAssociationOutcome removeRegistration() override;

private:
    shell::FileAssociationTargets targets_;
    shell::FileAssociationRoots roots_;
};

} // namespace dvs::app
