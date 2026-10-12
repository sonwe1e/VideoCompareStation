#pragma once

#include <string>

namespace dvs::application {

// Which application Windows currently launches for "open" on the registered extensions. The
// states are read from the per-user UserChoice values; nothing in this port writes them.
enum class DefaultAppOwnership {
    // No UserChoice exists for the kind's extensions; Windows falls back to its own probing.
    NotSet,
    // Every extension of the kind resolves to this application's ProgID.
    ThisApplication,
    // Some UserChoice values exist, but not every extension of the kind resolves to this
    // application: another application owns one, or the user granted only part of the kind.
    OtherApplication,
};

// The health of this application's own "registered as a choosable app" keys, independent of which
// application currently owns the default. StalePath is the state a moved or upgraded ZIP is in.
enum class FileAssociationHealth {
    // None of this application's ProgID/Appl/Capabilities keys exist.
    NotRegistered,
    // Every key exists and already points at the running installation.
    Current,
    // Keys exist but describe another executable path (or are incomplete); repair rewrites them.
    StalePath,
};

struct FileAssociationStatus final {
    FileAssociationHealth health{FileAssociationHealth::NotRegistered};
    DefaultAppOwnership videoDefault{DefaultAppOwnership::NotSet};
    DefaultAppOwnership imageDefault{DefaultAppOwnership::NotSet};
};

struct FileAssociationOutcome final {
    bool ok{false};
    // Empty on success; names the failing key or step for the dialog's error line on failure.
    std::string error;
};

// The settings-dialog contract for Windows "open with" integration. The port keeps Win32 out of
// the UI layer: register/repair/remove are per-user registry writes, the settings jump launches
// ms-settings:, and the status read-back is the only authority on whether the user's system
// selection actually landed on this application. Implementations are called from a worker thread
// and must not retain UI state.
class IFileAssociationService {
public:
    virtual ~IFileAssociationService() = default;

    // Makes this installation a user-selectable choice for exactly the extensions this version
    // has validated (video and still images are registered as separate ProgIDs; audio types are
    // never registered because the product is a silent visual review tool). Never writes
    // UserChoice and never takes over any format: the user's own selection in system settings is
    // the only thing that changes a default. Whole-write semantics: on any failure the previous
    // registration is left as it was, not half-replaced.
    [[nodiscard]] virtual FileAssociationOutcome registerAsOptionalApplication() = 0;

    // Opens the system default-apps page, deep-linking to this application's page when the
    // running Windows supports the registeredAppUser parameter and falling back to the generic
    // page otherwise. "The page opened" is not success for the association itself: the dialog
    // must follow this with associationStatus() once the user reports back.
    [[nodiscard]] virtual FileAssociationOutcome openSystemDefaultAppsSettings() = 0;

    // Reads back who owns the video/image defaults plus this application's own key health.
    [[nodiscard]] virtual FileAssociationStatus associationStatus() = 0;

    // Rewrites the executable paths recorded in this application's ProgIDs and Applications keys
    // after the ZIP directory moved or was upgraded, transactionally: all values are captured
    // before the first write and restored if any write fails.
    [[nodiscard]] virtual FileAssociationOutcome repairRegistrationPaths() = 0;

    // Removes exactly the keys and values this application owns. Does not restore guessed
    // previous defaults, does not touch UserChoice, and never removes another application's
    // values from shared keys.
    [[nodiscard]] virtual FileAssociationOutcome removeRegistration() = 0;
};

} // namespace dvs::application
