#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace dvs::shell {

// Where the per-user default-app registration lives. The defaults are the real per-user
// locations; tests point every member at a scratch key so a run can never touch the user's own
// registration - the same isolation ExplorerRegistrationRoots uses.
struct FileAssociationRoots {
    // Relative to HKEY_CURRENT_USER. Holds the ProgIDs, Applications\<exe> and the per-extension
    // OpenWithProgids values. Explorer merges this into HKCR.
    std::wstring classes{L"Software\\Classes"};

    // Relative to HKEY_CURRENT_USER. The per-extension UserChoice values live here; this
    // registration only ever *reads* them, because Windows protects them from programmatic
    // writes and because silently taking over a format is exactly what it must not do.
    std::wstring fileExts{L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts"};

    // Relative to HKEY_CURRENT_USER. One value per registered application; the key is shared
    // with every other application, so this registration only ever touches its own value.
    std::wstring registeredApplications{L"Software\\RegisteredApplications"};

    // Relative to HKEY_CURRENT_USER. The Capabilities key this registration owns.
    std::wstring capabilities{L"Software\\CompareStation\\Capabilities"};

    // The value name under RegisteredApplications, and the name the ms-settings deep link uses.
    std::wstring registeredName{L"CompareStation"};
};

// What the registration has to point at. Only the executable: the default-open command is a
// plain command line, unlike the Explorer verb which also needs the shell library.
struct FileAssociationTargets {
    std::filesystem::path executable;
};

enum class FileAssociationState {
    // Every key and value this action owns is in the requested shape, read back after writing.
    Ok,

    // A registry call or a validation failed. `error` names the key or step.
    Failed,
};

struct FileAssociationResult {
    FileAssociationState state{FileAssociationState::Failed};
    std::string error;
};

enum class DefaultOwner {
    NotSet,
    ThisApplication,
    OtherApplication,
};

enum class AssociationHealth {
    NotRegistered,
    Current,
    StalePath,
};

struct FileAssociationStatus {
    AssociationHealth health{AssociationHealth::NotRegistered};
    DefaultOwner videoDefault{DefaultOwner::NotSet};
    DefaultOwner imageDefault{DefaultOwner::NotSet};
};

// Optional seams for openDefaultAppsSettings so the launch decision is testable without opening
// the real Settings app. The defaults query the running Windows build and call ShellExecuteW.
struct DefaultAppsLaunchOverrides {
    // Zero means "query the running build at call time".
    unsigned long buildNumber{0UL};

    // Empty means "launch with ShellExecuteW". Returns whether the URI was launched.
    std::function<bool(const std::wstring&)> launch;
};

// Registers this installation as a choosable per-user application for exactly the extensions the
// shipped handler accepts (five video, thirteen still image, read from ExplorerCommandSupport,
// never a copy here). Video and image get separate ProgIDs so the user can grant the default for
// one kind without the other; audio is not registered at all. Writes only HKEY_CURRENT_USER,
// never UserChoice, and nothing outside the roots. The write is whole-or-nothing: every value is
// captured before the first change and the previous state is restored if any step fails.
[[nodiscard]] FileAssociationResult
registerFileAssociation(const FileAssociationTargets& targets,
                        const FileAssociationRoots& roots = FileAssociationRoots{});

// Rewrites the executable paths in this application's ProgID and Applications keys (and re-points
// the RegisteredApplications value) after the ZIP moved or was upgraded, with the same
// whole-or-nothing semantics. Fails without writing when no registration exists to repair.
[[nodiscard]] FileAssociationResult
repairFileAssociationPaths(const FileAssociationTargets& targets,
                           const FileAssociationRoots& roots = FileAssociationRoots{});

// Removes exactly the keys and values this registration owns: the two ProgIDs,
// Applications\CompareStation.exe, this application's OpenWithProgids values, the Capabilities
// subtree and the RegisteredApplications value. Shared keys (.mp4, RegisteredApplications,
// OpenWithProgids of other tools, UserChoice) survive with whatever else is in them.
[[nodiscard]] FileAssociationResult
removeFileAssociation(const FileAssociationRoots& roots = FileAssociationRoots{});

// Reads back this application's key health plus who owns the video and image defaults. UserChoice
// is only read; "NotSet" is reported as such and never guessed around.
[[nodiscard]] FileAssociationStatus
readFileAssociationStatus(const FileAssociationTargets& targets,
                          const FileAssociationRoots& roots = FileAssociationRoots{});

// The `registeredAppUser` deep link needs Windows 11 21H2 with the 2023-04 cumulative update or
// later; the patch level is not discoverable, so the build gate is Windows 11 (22000) and an
// older patch level harmlessly lands on the generic page - the same page the fallback opens.
[[nodiscard]] bool supportsRegisteredAppUserParameter(unsigned long buildNumber);

// The URI openDefaultAppsSettings launches: the application's own defaults page when the running
// Windows accepts the deep-link parameter, otherwise the default-apps page itself.
[[nodiscard]] std::wstring defaultAppsSettingsUri(const std::wstring_view registeredName,
                                                  const bool deepLink);

// Opens the default-apps settings page, deep-linking when supported and falling back to the
// generic page when the parameterized launch is refused. Opening a page proves nothing about the
// association itself; readFileAssociationStatus is the read-back that counts.
[[nodiscard]] FileAssociationResult
openDefaultAppsSettings(const FileAssociationRoots& roots = FileAssociationRoots{},
                        const DefaultAppsLaunchOverrides& overrides = DefaultAppsLaunchOverrides{});

} // namespace dvs::shell
