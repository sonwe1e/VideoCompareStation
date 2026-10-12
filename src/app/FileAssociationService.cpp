#include "FileAssociationService.h"

#include "dvs/shell/ExplorerCommandRegistration.h"

namespace dvs::app {
namespace {

[[nodiscard]] application::FileAssociationOutcome
translate(const shell::FileAssociationResult& result) {
    return application::FileAssociationOutcome{
        result.state == shell::FileAssociationState::Ok,
        result.error,
    };
}

} // namespace

FileAssociationService::FileAssociationService(const std::string_view version) {
    // The default-open registration points at the application itself, so the same running-
    // installation derivation the startup Explorer registration uses decides the path here.
    targets_.executable = shell::runningInstallationTargets(version).executable;
}

application::FileAssociationOutcome FileAssociationService::registerAsOptionalApplication() {
    return translate(shell::registerFileAssociation(targets_, roots_));
}

application::FileAssociationOutcome FileAssociationService::openSystemDefaultAppsSettings() {
    return translate(shell::openDefaultAppsSettings(roots_));
}

application::FileAssociationStatus FileAssociationService::associationStatus() {
    const shell::FileAssociationStatus status = shell::readFileAssociationStatus(targets_, roots_);
    application::FileAssociationStatus translated;
    switch (status.health) {
    case shell::AssociationHealth::NotRegistered:
        translated.health = application::FileAssociationHealth::NotRegistered;
        break;
    case shell::AssociationHealth::Current:
        translated.health = application::FileAssociationHealth::Current;
        break;
    case shell::AssociationHealth::StalePath:
        translated.health = application::FileAssociationHealth::StalePath;
        break;
    }
    switch (status.videoDefault) {
    case shell::DefaultOwner::NotSet:
        translated.videoDefault = application::DefaultAppOwnership::NotSet;
        break;
    case shell::DefaultOwner::ThisApplication:
        translated.videoDefault = application::DefaultAppOwnership::ThisApplication;
        break;
    case shell::DefaultOwner::OtherApplication:
        translated.videoDefault = application::DefaultAppOwnership::OtherApplication;
        break;
    }
    switch (status.imageDefault) {
    case shell::DefaultOwner::NotSet:
        translated.imageDefault = application::DefaultAppOwnership::NotSet;
        break;
    case shell::DefaultOwner::ThisApplication:
        translated.imageDefault = application::DefaultAppOwnership::ThisApplication;
        break;
    case shell::DefaultOwner::OtherApplication:
        translated.imageDefault = application::DefaultAppOwnership::OtherApplication;
        break;
    }
    return translated;
}

application::FileAssociationOutcome FileAssociationService::repairRegistrationPaths() {
    return translate(shell::repairFileAssociationPaths(targets_, roots_));
}

application::FileAssociationOutcome FileAssociationService::removeRegistration() {
    return translate(shell::removeFileAssociation(roots_));
}

} // namespace dvs::app
