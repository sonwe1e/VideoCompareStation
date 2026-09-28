#include "dvs/shell/ExplorerCommandRegistration.h"

#include "ExplorerCommand.h"
#include "ExplorerCommandSupport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <array>
#include <combaseapi.h>
#include <cstddef>
#include <cwctype>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>
#include <windows.h>

namespace dvs::shell {
namespace {

constexpr wchar_t kVerbKeyName[] = L"CompareStation.Compare";
constexpr wchar_t kMenuText[] = L"Compare with CompareStation";
constexpr wchar_t kCommandTitle[] = L"CompareStation Explorer comparison command";
constexpr wchar_t kSelectionModel[] = L"Player";
constexpr wchar_t kThreadingModel[] = L"Apartment";

// Records the user's own decision. Everything else under the registration is derived state that a
// launch may rewrite; this value is the one thing a launch must never invent.
constexpr wchar_t kEnabledValueName[] = L"ExplorerContextMenu";

// Owns one opened or created key. The registry has no value type worth wrapping, so every helper
// below takes a raw HKEY and the callers keep this alive for the duration.
class RegistryKey final {
public:
    RegistryKey() noexcept = default;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    ~RegistryKey() {
        close();
    }

    // Clears the current handle and hands out the slot for RegCreateKeyExW.
    [[nodiscard]] HKEY* receive() noexcept {
        close();
        return &handle_;
    }

    [[nodiscard]] HKEY get() const noexcept {
        return handle_;
    }

private:
    void close() noexcept {
        if (handle_ != nullptr) {
            RegCloseKey(handle_);
            handle_ = nullptr;
        }
    }

    HKEY handle_{nullptr};
};

[[nodiscard]] std::string toUtf8(const std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string converted(static_cast<std::size_t>(length), '\0');
    static_cast<void>(WideCharToMultiByte(CP_UTF8,
                                          0,
                                          text.data(),
                                          static_cast<int>(text.size()),
                                          converted.data(),
                                          length,
                                          nullptr,
                                          nullptr));
    return converted;
}

[[nodiscard]] std::string describeFailure(const std::wstring& where, const LSTATUS status) {
    return toUtf8(where) + " (registry error " + std::to_string(static_cast<long>(status)) + ")";
}

// Reads one environment variable. A variable that is absent and a variable that is empty look the
// same to a caller here, which is what "not set" means for the escape hatch below.
[[nodiscard]] std::wstring environmentValue(const wchar_t* const name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0U);
    if (needed == 0U) {
        return {};
    }
    std::wstring value(static_cast<std::size_t>(needed), L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0U || written >= needed) {
        return {};
    }
    value.resize(written);
    return value;
}

// The product version reaches this module as narrow text, because that is how the project defines
// DVS_PROJECT_VERSION; the registry and the file names here are wide, so it is widened once at the
// boundary. Version strings are ASCII digits and dots by construction.
[[nodiscard]] std::wstring widenAscii(const std::string_view text) {
    std::wstring widened;
    widened.reserve(text.size());
    for (const char character : text) {
        widened.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
    }
    return widened;
}

// The one reading of "switch this off" shared by the registry marker and the environment escape
// hatch, so a value written by hand as text means the same thing as the DWord both writers use.
[[nodiscard]] bool isDisabledText(const std::wstring_view text) {
    std::wstring lowered;
    lowered.reserve(text.size());
    for (const wchar_t character : text) {
        lowered.push_back(static_cast<wchar_t>(std::towlower(character)));
    }
    return lowered == L"0" || lowered == L"false" || lowered == L"no";
}

// The shell library is part of the binary name: CompareStationShell-<major>.<minor>.dll. Anything
// that is not a leading major.minor yields no name, and the caller then registers nothing rather
// than pointing the shell at a library that does not exist.
[[nodiscard]] std::wstring versionedShellLibraryName(const std::wstring_view version) {
    const std::size_t firstDot = version.find(L'.');
    if (firstDot == std::wstring_view::npos) {
        return {};
    }
    const std::size_t secondDot = version.find(L'.', firstDot + 1U);
    const std::wstring_view major = version.substr(0U, firstDot);
    const std::wstring_view minor = secondDot == std::wstring_view::npos
                                        ? version.substr(firstDot + 1U)
                                        : version.substr(firstDot + 1U, secondDot - firstDot - 1U);
    if (major.empty() || minor.empty()) {
        return {};
    }
    for (const wchar_t character : major) {
        if (character < L'0' || character > L'9') {
            return {};
        }
    }
    for (const wchar_t character : minor) {
        if (character < L'0' || character > L'9') {
            return {};
        }
    }
    return L"CompareStationShell-" + std::wstring{major} + L"." + std::wstring{minor} + L".dll";
}

[[nodiscard]] std::wstring clsidText() {
    std::array<wchar_t, 64U> buffer{};
    const int length =
        StringFromGUID2(kExplorerCommandClsid, buffer.data(), static_cast<int>(buffer.size()));
    if (length <= 1) {
        return {};
    }
    // StringFromGUID2 counts the terminating null in its return value.
    return std::wstring{buffer.data(), static_cast<std::size_t>(length - 1)};
}

[[nodiscard]] std::wstring clsidKeyPath(const ExplorerRegistrationRoots& roots,
                                        const std::wstring_view clsid) {
    std::wstring path = roots.classes;
    path += L"\\CLSID\\";
    path.append(clsid);
    return path;
}

[[nodiscard]] std::wstring inprocKeyPath(const ExplorerRegistrationRoots& roots,
                                         const std::wstring_view clsid) {
    std::wstring path = clsidKeyPath(roots, clsid);
    path += L"\\InprocServer32";
    return path;
}

[[nodiscard]] std::wstring verbKeyPath(const ExplorerRegistrationRoots& roots,
                                       const std::wstring_view extension) {
    std::wstring path = roots.classes;
    path += L"\\SystemFileAssociations\\";
    path.append(extension);
    path += L"\\shell\\";
    path += kVerbKeyName;
    return path;
}

[[nodiscard]] std::wstring shellKeyPath(const ExplorerRegistrationRoots& roots,
                                        const std::wstring_view extension) {
    std::wstring path = roots.classes;
    path += L"\\SystemFileAssociations\\";
    path.append(extension);
    path += L"\\shell";
    return path;
}

[[nodiscard]] std::wstring extensionKeyPath(const ExplorerRegistrationRoots& roots,
                                            const std::wstring_view extension) {
    std::wstring path = roots.classes;
    path += L"\\SystemFileAssociations\\";
    path.append(extension);
    return path;
}

// The second place a verb for the same extension can bind this command. A key here is part of the
// same association chain as the SystemFileAssociations one, so a leftover copy of the command
// there shows up as a second entry in the menu for every file of that type - the two entries are
// indistinguishable to the user because both carry the same title.
[[nodiscard]] std::wstring perExtensionShellKeyPath(const ExplorerRegistrationRoots& roots,
                                                    const std::wstring_view extension) {
    std::wstring path = roots.classes;
    path += L"\\";
    path.append(extension);
    path += L"\\shell";
    return path;
}

// True only when a key holds neither subkeys nor values, including its unnamed default value. This
// is the condition for removing a key this registration shares with other tools.
[[nodiscard]] bool isEmptyKey(const std::wstring& path) {
    RegistryKey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0U, KEY_READ, key.receive()) !=
        ERROR_SUCCESS) {
        return false;
    }
    DWORD subkeys = 0U;
    DWORD values = 0U;
    if (RegQueryInfoKeyW(key.get(),
                         nullptr,
                         nullptr,
                         nullptr,
                         &subkeys,
                         nullptr,
                         nullptr,
                         &values,
                         nullptr,
                         nullptr,
                         nullptr,
                         nullptr) != ERROR_SUCCESS) {
        return false;
    }
    return subkeys == 0U && values == 0U;
}

// Applies `function` to every extension the shipped command accepts. The list comes from the shell
// adapter itself rather than a copy, so the entry can never be offered for a file the handler then
// refuses, which is exactly how the registration script and the handler drifted apart before.
template <typename Function> void forEachRegisteredExtension(Function&& function) {
    for (const std::wstring_view extension : supportedVideoExtensions()) {
        function(extension);
    }
    for (const std::wstring_view extension : supportedImageExtensions()) {
        function(extension);
    }
}

[[nodiscard]] LSTATUS
writeStringValue(const HKEY key, const wchar_t* const name, const std::wstring_view value) {
    const DWORD bytes = static_cast<DWORD>((value.size() + 1U) * sizeof(wchar_t));
    return RegSetValueExW(
        key, name, 0U, REG_SZ, reinterpret_cast<const BYTE*>(value.data()), bytes);
}

[[nodiscard]] LSTATUS
writeDwordValue(const HKEY key, const wchar_t* const name, const DWORD value) {
    const DWORD stored = value;
    return RegSetValueExW(
        key, name, 0U, REG_DWORD, reinterpret_cast<const BYTE*>(&stored), sizeof(stored));
}

[[nodiscard]] std::optional<std::wstring> readStringValue(const HKEY key,
                                                          const wchar_t* const name) {
    DWORD type = 0U;
    DWORD bytes = 0U;
    LSTATUS status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return std::nullopt;
    }
    std::wstring value(static_cast<std::size_t>(bytes / sizeof(wchar_t)) + 1U, L'\0');
    status =
        RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes);
    if (status != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(static_cast<std::size_t>(bytes) / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] std::optional<DWORD> readDwordValue(const HKEY key, const wchar_t* const name) {
    DWORD type = 0U;
    DWORD bytes = sizeof(DWORD);
    DWORD value = 0U;
    const LSTATUS status =
        RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes);
    if (status != ERROR_SUCCESS || type != REG_DWORD || bytes != sizeof(DWORD)) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool
valueEquals(const HKEY key, const wchar_t* const name, const std::wstring_view expected) {
    const std::optional<std::wstring> actual = readStringValue(key, name);
    return actual.has_value() && std::wstring_view{*actual} == expected;
}

// True when a verb key is a copy of this command rather than another tool's entry. Two shapes have
// pointed at this application: the COM handler this build writes, and a plain command line left by
// an earlier layout that named the executable. Both are recognised by what they point at, never by
// the key's name, so a leftover is recognised whatever it happens to be called.
[[nodiscard]] bool bindsThisCommand(const std::wstring& verbPath) {
    RegistryKey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, verbPath.c_str(), 0U, KEY_READ, key.receive()) !=
        ERROR_SUCCESS) {
        return false;
    }
    if (valueEquals(key.get(), L"ExplorerCommandHandler", clsidText())) {
        return true;
    }
    RegistryKey command;
    const std::wstring commandPath = verbPath + L"\\command";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, commandPath.c_str(), 0U, KEY_READ, command.receive()) !=
        ERROR_SUCCESS) {
        return false;
    }
    const std::optional<std::wstring> line = readStringValue(command.get(), nullptr);
    if (!line.has_value()) {
        return false;
    }
    std::wstring lowered;
    lowered.reserve(line->size());
    for (const wchar_t character : *line) {
        lowered.push_back(static_cast<wchar_t>(std::towlower(character)));
    }
    return lowered.find(L"comparestation.exe") != std::wstring::npos ||
           lowered.find(L"vcstation.exe") != std::wstring::npos;
}

// Every verb name under one shell key, so a parent can be swept without guessing at names.
[[nodiscard]] std::vector<std::wstring> verbNamesUnder(const std::wstring& shellPath) {
    std::vector<std::wstring> names;
    RegistryKey shell;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, shellPath.c_str(), 0U, KEY_READ, shell.receive()) !=
        ERROR_SUCCESS) {
        return names;
    }
    for (DWORD index = 0U;; ++index) {
        wchar_t name[256]{};
        DWORD length = static_cast<DWORD>(std::size(name));
        const LSTATUS status =
            RegEnumKeyExW(shell.get(), index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            return names;
        }
        if (status != ERROR_SUCCESS) {
            return names;
        }
        names.emplace_back(name, length);
    }
}

// Removes every verb under `shellPath` that binds this command except `kVerbKeyName`, and drops the
// parents it emptied. A file's menu is built from every verb in its association chain, so a second
// copy of this command is a second identical entry there - the one the user cannot tell apart from
// the first. The sweep runs over the two parents a verb for one extension can live under, because
// both are part of that chain.
ExplorerRegistrationResult sweepDuplicateVerbs(const ExplorerRegistrationRoots& roots) {
    ExplorerRegistrationResult result{ExplorerRegistrationState::Registered, {}};
    forEachRegisteredExtension([&](const std::wstring_view extension) {
        if (result.state != ExplorerRegistrationState::Registered) {
            return;
        }
        for (const std::wstring& shellPath :
             {shellKeyPath(roots, extension), perExtensionShellKeyPath(roots, extension)}) {
            for (const std::wstring& name : verbNamesUnder(shellPath)) {
                if (name == kVerbKeyName) {
                    continue;
                }
                const std::wstring verbPath = shellPath + L"\\" + name;
                if (!bindsThisCommand(verbPath)) {
                    continue;
                }
                const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, verbPath.c_str());
                if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND &&
                    status != ERROR_PATH_NOT_FOUND) {
                    result.state = ExplorerRegistrationState::Failed;
                    result.error = describeFailure(verbPath, status);
                    return;
                }
                if (isEmptyKey(shellPath)) {
                    static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, shellPath.c_str()));
                }
                const std::wstring extensionParent = extensionKeyPath(roots, extension);
                if (isEmptyKey(extensionParent)) {
                    static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, extensionParent.c_str()));
                }
                const std::wstring classesParent = roots.classes + L"\\" + std::wstring{extension};
                if (isEmptyKey(classesParent)) {
                    static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, classesParent.c_str()));
                }
            }
        }
    });
    return result;
}

// True only when every key already describes this installation, including the extension list. A
// half-written or stale registration has to be repaired rather than trusted.
[[nodiscard]] bool registrationMatches(const ExplorerRegistrationTargets& targets,
                                       const ExplorerRegistrationRoots& roots) {
    const std::wstring clsid = clsidText();
    if (clsid.empty() || targets.executable.empty() || targets.shellLibrary.empty()) {
        return false;
    }
    const std::wstring executable = targets.executable.wstring();
    const std::wstring shellLibrary = targets.shellLibrary.wstring();
    {
        RegistryKey key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          clsidKeyPath(roots, clsid).c_str(),
                          0U,
                          KEY_READ,
                          key.receive()) != ERROR_SUCCESS) {
            return false;
        }
        if (!valueEquals(key.get(), nullptr, kCommandTitle)) {
            return false;
        }
    }
    {
        RegistryKey key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          inprocKeyPath(roots, clsid).c_str(),
                          0U,
                          KEY_READ,
                          key.receive()) != ERROR_SUCCESS) {
            return false;
        }
        if (!valueEquals(key.get(), nullptr, shellLibrary) ||
            !valueEquals(key.get(), L"ThreadingModel", kThreadingModel)) {
            return false;
        }
    }
    const std::wstring icon = executable + L",0";
    bool allExtensionsMatch = true;
    forEachRegisteredExtension([&](const std::wstring_view extension) {
        if (!allExtensionsMatch) {
            return;
        }
        RegistryKey key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          verbKeyPath(roots, extension).c_str(),
                          0U,
                          KEY_READ,
                          key.receive()) != ERROR_SUCCESS) {
            allExtensionsMatch = false;
            return;
        }
        allExtensionsMatch = valueEquals(key.get(), L"MUIVerb", kMenuText) &&
                             valueEquals(key.get(), L"Icon", icon) &&
                             valueEquals(key.get(), L"ExplorerCommandHandler", clsid) &&
                             valueEquals(key.get(), L"MultiSelectModel", kSelectionModel);
    });
    return allExtensionsMatch;
}

[[nodiscard]] ExplorerRegistrationResult
writeRegistration(const ExplorerRegistrationTargets& targets,
                  const ExplorerRegistrationRoots& roots) {
    const std::wstring clsid = clsidText();
    const std::wstring executable = targets.executable.wstring();
    const std::wstring shellLibrary = targets.shellLibrary.wstring();
    const std::wstring icon = executable + L",0";

    {
        RegistryKey key;
        const std::wstring path = clsidKeyPath(roots, clsid);
        LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER,
                                         path.c_str(),
                                         0U,
                                         nullptr,
                                         REG_OPTION_NON_VOLATILE,
                                         KEY_READ | KEY_WRITE,
                                         nullptr,
                                         key.receive(),
                                         nullptr);
        if (status == ERROR_SUCCESS) {
            status = writeStringValue(key.get(), nullptr, kCommandTitle);
        }
        if (status != ERROR_SUCCESS) {
            return {ExplorerRegistrationState::Failed, describeFailure(path, status)};
        }
    }
    {
        RegistryKey key;
        const std::wstring path = inprocKeyPath(roots, clsid);
        LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER,
                                         path.c_str(),
                                         0U,
                                         nullptr,
                                         REG_OPTION_NON_VOLATILE,
                                         KEY_READ | KEY_WRITE,
                                         nullptr,
                                         key.receive(),
                                         nullptr);
        if (status == ERROR_SUCCESS) {
            status = writeStringValue(key.get(), nullptr, shellLibrary);
        }
        if (status == ERROR_SUCCESS) {
            status = writeStringValue(key.get(), L"ThreadingModel", kThreadingModel);
        }
        if (status != ERROR_SUCCESS) {
            return {ExplorerRegistrationState::Failed, describeFailure(path, status)};
        }
    }
    ExplorerRegistrationResult result{ExplorerRegistrationState::Registered, {}};
    forEachRegisteredExtension([&](const std::wstring_view extension) {
        if (result.state == ExplorerRegistrationState::Failed) {
            return;
        }
        RegistryKey key;
        const std::wstring path = verbKeyPath(roots, extension);
        LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER,
                                         path.c_str(),
                                         0U,
                                         nullptr,
                                         REG_OPTION_NON_VOLATILE,
                                         KEY_READ | KEY_WRITE,
                                         nullptr,
                                         key.receive(),
                                         nullptr);
        if (status == ERROR_SUCCESS) {
            status = writeStringValue(key.get(), L"MUIVerb", kMenuText);
        }
        if (status == ERROR_SUCCESS) {
            status = writeStringValue(key.get(), L"Icon", icon);
        }
        if (status == ERROR_SUCCESS) {
            status = writeStringValue(key.get(), L"ExplorerCommandHandler", clsid);
        }
        if (status == ERROR_SUCCESS) {
            status = writeStringValue(key.get(), L"MultiSelectModel", kSelectionModel);
        }
        if (status != ERROR_SUCCESS) {
            result.state = ExplorerRegistrationState::Failed;
            result.error = describeFailure(path, status);
        }
    });
    if (result.state == ExplorerRegistrationState::Failed) {
        return result;
    }
    // Read the keys back: a silent write failure must not be reported as a working menu entry, and
    // this is the same check the registration script performs.
    if (!registrationMatches(targets, roots)) {
        result.state = ExplorerRegistrationState::Failed;
        result.error = "the registry did not keep the values just written";
    }
    return result;
}

} // namespace

bool isExplorerCommandEnabled(const ExplorerRegistrationRoots& roots) {
    RegistryKey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, roots.settings.c_str(), 0U, KEY_READ, key.receive()) !=
        ERROR_SUCCESS) {
        // No settings key yet: this is a fresh extract, which starts enabled.
        return true;
    }
    const std::optional<DWORD> stored = readDwordValue(key.get(), kEnabledValueName);
    if (stored.has_value()) {
        return *stored != 0U;
    }
    // A value written by hand with RegEdit is a string; reading it as "enabled" would silently undo
    // the user's decision, so the text forms of "off" count as well.
    const std::optional<std::wstring> text = readStringValue(key.get(), kEnabledValueName);
    if (text.has_value()) {
        return !isDisabledText(*text);
    }
    return true;
}

ExplorerRegistrationResult setExplorerCommandEnabled(const bool enabled,
                                                     const ExplorerRegistrationRoots& roots) {
    RegistryKey key;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER,
                                     roots.settings.c_str(),
                                     0U,
                                     nullptr,
                                     REG_OPTION_NON_VOLATILE,
                                     KEY_READ | KEY_WRITE,
                                     nullptr,
                                     key.receive(),
                                     nullptr);
    if (status == ERROR_SUCCESS) {
        status = writeDwordValue(key.get(), kEnabledValueName, enabled ? 1U : 0U);
    }
    if (status != ERROR_SUCCESS) {
        return {ExplorerRegistrationState::Failed, describeFailure(roots.settings, status)};
    }
    return {enabled ? ExplorerRegistrationState::Registered
                    : ExplorerRegistrationState::DisabledByUser,
            {}};
}

ExplorerRegistrationResult
removeExplorerCommandRegistration(const ExplorerRegistrationRoots& roots) {
    const std::wstring clsid = clsidText();
    if (clsid.empty()) {
        return {ExplorerRegistrationState::Failed, "the command CLSID could not be formatted"};
    }
    ExplorerRegistrationResult result{ExplorerRegistrationState::Removed, {}};
    forEachRegisteredExtension([&](const std::wstring_view extension) {
        if (result.state == ExplorerRegistrationState::Failed) {
            return;
        }
        const std::wstring path = verbKeyPath(roots, extension);
        const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND &&
            status != ERROR_PATH_NOT_FOUND) {
            result.state = ExplorerRegistrationState::Failed;
            result.error = describeFailure(path, status);
            return;
        }
        // Drop the parents only when nothing else uses them. RegDeleteKeyW refuses a key that still
        // has subkeys but happily deletes one that only carries values, and deleting a key another
        // tool wrote values on would take its data with it, so "empty" has to mean neither.
        const std::wstring shellPath = shellKeyPath(roots, extension);
        if (isEmptyKey(shellPath)) {
            static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, shellPath.c_str()));
        }
        const std::wstring extensionPath = extensionKeyPath(roots, extension);
        if (isEmptyKey(extensionPath)) {
            static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, extensionPath.c_str()));
        }
    });
    if (result.state == ExplorerRegistrationState::Failed) {
        return result;
    }
    // A leftover copy of the command is still this command, so a documented removal that left one
    // behind would only hide the entry again the next time somebody registered it.
    const ExplorerRegistrationResult sweep = sweepDuplicateVerbs(roots);
    if (sweep.state == ExplorerRegistrationState::Failed) {
        return sweep;
    }
    const std::wstring path = clsidKeyPath(roots, clsid);
    const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND &&
        status != ERROR_PATH_NOT_FOUND) {
        return {ExplorerRegistrationState::Failed, describeFailure(path, status)};
    }
    return result;
}

ExplorerRegistrationResult
ensureExplorerCommandRegistered(const ExplorerRegistrationTargets& targets,
                                const ExplorerRegistrationRoots& roots) {
    // The user's own decision comes first: it is the cheapest check and the only one that makes a
    // missing file irrelevant. With the order reversed, a user who had turned the entry off while
    // the directory had lost its shell library would be told on every launch that registration
    // failed - there is nothing to do about that, and nothing was written either.
    if (!isExplorerCommandEnabled(roots)) {
        return {ExplorerRegistrationState::DisabledByUser, {}};
    }
    // Then validate before touching anything: a half-written registration is worse than none, the
    // read-back below would turn this into a failure only after the first keys already existed, and
    // a path Explorer cannot load would be reported as "already current" on every later launch.
    if (targets.executable.empty() || targets.shellLibrary.empty()) {
        return {ExplorerRegistrationState::Failed,
                "the command needs both CompareStation.exe and its shell library"};
    }
    std::error_code existenceError;
    if (!std::filesystem::exists(targets.executable, existenceError) ||
        !std::filesystem::exists(targets.shellLibrary, existenceError)) {
        return {ExplorerRegistrationState::Failed,
                "the command can only be registered from a directory that holds CompareStation.exe "
                "and its shell library"};
    }
    // The sweep runs on every launch, not only when something had to be written: a second copy of
    // the command can appear at any time - a key an earlier layout wrote, or one copied by hand -
    // and the promise this registration makes is that the entry it owns is the entry the user sees.
    const ExplorerRegistrationResult registration =
        registrationMatches(targets, roots)
            ? ExplorerRegistrationResult{ExplorerRegistrationState::AlreadyCurrent, {}}
            : writeRegistration(targets, roots);
    if (registration.state == ExplorerRegistrationState::Failed) {
        return registration;
    }
    const ExplorerRegistrationResult sweep = sweepDuplicateVerbs(roots);
    return sweep.state == ExplorerRegistrationState::Failed ? sweep : registration;
}

ExplorerRegistrationTargets installationTargetsFor(const std::filesystem::path& executable,
                                                   const std::string_view version) {
    ExplorerRegistrationTargets targets;
    targets.executable = executable;
    const std::wstring library = versionedShellLibraryName(widenAscii(version));
    if (!library.empty()) {
        targets.shellLibrary = executable.parent_path() / library;
    }
    return targets;
}

ExplorerRegistrationTargets runningInstallationTargets(const std::string_view version) {
    std::wstring modulePath(32768U, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (length == 0U || static_cast<std::size_t>(length) >= modulePath.size()) {
        return {};
    }
    modulePath.resize(length);
    return installationTargetsFor(std::filesystem::path{modulePath}, version);
}

bool isShellRegistrationDisabledByEnvironment() {
    const std::wstring value = environmentValue(L"DVS_DISABLE_SHELL_REGISTRATION");
    return !value.empty() && !isDisabledText(value);
}

ExplorerRegistrationResult ensureRunningInstallationRegistered(const std::string_view version) {
    if (isShellRegistrationDisabledByEnvironment()) {
        return {ExplorerRegistrationState::DisabledByUser, {}};
    }
    try {
        const ExplorerRegistrationTargets targets = runningInstallationTargets(version);
        if (targets.executable.empty() || targets.shellLibrary.empty()) {
            return {ExplorerRegistrationState::Failed,
                    "the running installation could not be located"};
        }
        return ensureExplorerCommandRegistered(targets);
    } catch (...) {
        return {ExplorerRegistrationState::Failed,
                "the Explorer command registration raised an unexpected error"};
    }
}

} // namespace dvs::shell
