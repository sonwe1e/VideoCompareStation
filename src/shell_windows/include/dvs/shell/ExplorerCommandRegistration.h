#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace dvs::shell {

// Where the per-user Explorer command lives. The defaults are the real per-user locations; tests
// point both members at a scratch key so a run can never touch the user's own registration.
struct ExplorerRegistrationRoots {
    // Relative to HKEY_CURRENT_USER. Explorer merges this into HKCR.
    std::wstring classes{L"Software\\Classes"};

    // Relative to HKEY_CURRENT_USER. Holds the one value that records the user's own decision, so a
    // directory that was registered keeps following that decision.
    std::wstring settings{L"Software\\CompareStation"};
};

// What the command has to point at: the executable Explorer launches and the shell library Explorer
// loads to decide whether the entry is offered at all.
struct ExplorerRegistrationTargets {
    std::filesystem::path executable;
    std::filesystem::path shellLibrary;
};

enum class ExplorerRegistrationState {
    // The keys already describe this installation, so nothing was written.
    AlreadyCurrent,

    // The keys were missing or described another directory; they were written and read back.
    Registered,

    // The user turned the entry off, so nothing was written. Without this a removed entry would
    // come back on the next launch and the documented uninstall would be a lie. A run that was told
    // to stay out of the registry through the environment reports this state too, because from the
    // caller's side both mean "this launch deliberately registered nothing".
    DisabledByUser,

    // Every key this registration owns was removed.
    Removed,

    // A registry call failed. `error` names the key and the failure.
    Failed,
};

struct ExplorerRegistrationResult {
    ExplorerRegistrationState state{ExplorerRegistrationState::Failed};
    std::string error;
};

// True when this user still wants the entry. The marker is absent on a fresh extract, and an absent
// marker means enabled, so an unpacked ZIP offers the command without running anything first.
[[nodiscard]] bool
isExplorerCommandEnabled(const ExplorerRegistrationRoots& roots = ExplorerRegistrationRoots{});

// Records the user's decision. Passing false is what makes "remove the entry" survive later
// launches; passing true is what lets the registration script turn it back on.
[[nodiscard]] ExplorerRegistrationResult
setExplorerCommandEnabled(const bool enabled,
                          const ExplorerRegistrationRoots& roots = ExplorerRegistrationRoots{});

// Removes every key this registration owns for the current user.
[[nodiscard]] ExplorerRegistrationResult removeExplorerCommandRegistration(
    const ExplorerRegistrationRoots& roots = ExplorerRegistrationRoots{});

// Makes the per-user keys describe `targets`. Writes nothing when they already do, which is the
// common case after the first run, and repairs them when the installation directory moved. Both
// files have to exist: registering a path that Explorer cannot load would survive as
// "already current" and never repair itself.
[[nodiscard]] ExplorerRegistrationResult ensureExplorerCommandRegistered(
    const ExplorerRegistrationTargets& targets,
    const ExplorerRegistrationRoots& roots = ExplorerRegistrationRoots{});

// What an installation at `executable` points at: the shell library next to it, named for the
// product version, so a build directory that also holds older releases still registers its own
// library instead of a stale one. A version without a leading major.minor leaves the library empty
// and the registration then refuses, rather than writing a path that cannot work. The version is
// accepted as narrow text because DVS_PROJECT_VERSION is a narrow string throughout the project.
[[nodiscard]] ExplorerRegistrationTargets
installationTargetsFor(const std::filesystem::path& executable, std::string_view version);

// The same, for the installation this process runs from.
[[nodiscard]] ExplorerRegistrationTargets runningInstallationTargets(std::string_view version);

// True when this run was asked to leave the registry alone through DVS_DISABLE_SHELL_REGISTRATION.
[[nodiscard]] bool isShellRegistrationDisabledByEnvironment();

// The startup entry point: derives the running installation's targets, honours that environment
// escape hatch, and makes the per-user registration match. Never throws, never blocks a start.
[[nodiscard]] ExplorerRegistrationResult
ensureRunningInstallationRegistered(std::string_view version);

} // namespace dvs::shell
