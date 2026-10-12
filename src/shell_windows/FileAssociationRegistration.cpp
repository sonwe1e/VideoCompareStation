#include "dvs/shell/FileAssociationRegistration.h"

#include "ExplorerCommandSupport.h"
#include "FileAssociationTestHooks.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <combaseapi.h>
#include <functional>
#include <optional>
#include <shellapi.h>
#include <shlobj.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <windows.h>

namespace dvs::shell {
namespace {

constexpr std::wstring_view kVideoProgId{L"CompareStation.Video"};
constexpr std::wstring_view kImageProgId{L"CompareStation.Image"};
// The Applications subkey is named for the shipped executable; Windows offers this entry in
// "Open with" for exactly that program, whatever directory it currently lives in.
constexpr std::wstring_view kApplicationsKey{L"Applications\\CompareStation.exe"};
constexpr wchar_t kVideoTypeTitle[] = L"CompareStation Video";
constexpr wchar_t kImageTypeTitle[] = L"CompareStation Image";
// The name shown on the system default-apps page and the value name under RegisteredApplications.
constexpr wchar_t kApplicationName[] = L"CompareStation";
// Stated up front because it is a promise about behaviour, not marketing: video playback in this
// product is silent, so making it the video default takes the sound away from double-click opens.
constexpr wchar_t kApplicationDescription[] =
    L"Silent visual review and comparison of videos and still images. Video playback is silent; "
    L"audio file types are not registered.";

// Owns one opened or created key for the lifetime of one operation. The registry has no value
// type worth wrapping, so every helper below takes a raw HKEY and callers keep this alive.
class RegistryKey final {
public:
    RegistryKey() noexcept = default;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    RegistryKey(RegistryKey&& other) noexcept : handle_{other.release()} {}
    RegistryKey& operator=(RegistryKey&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.release();
        }
        return *this;
    }
    ~RegistryKey() {
        close();
    }

    // Clears the current handle and hands out the slot for RegCreateKeyExW / RegOpenKeyExW.
    [[nodiscard]] HKEY* receive() noexcept {
        close();
        return &handle_;
    }

    [[nodiscard]] HKEY get() const noexcept {
        return handle_;
    }

private:
    [[nodiscard]] HKEY release() noexcept {
        const HKEY handle = handle_;
        handle_ = nullptr;
        return handle;
    }

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

[[nodiscard]] LSTATUS
writeStringValue(const HKEY key, const wchar_t* const name, const std::wstring_view value) {
    const DWORD bytes = static_cast<DWORD>((value.size() + 1U) * sizeof(wchar_t));
    return RegSetValueExW(
        key, name, 0U, REG_SZ, reinterpret_cast<const BYTE*>(value.data()), bytes);
}

// Reads one REG_SZ/REG_EXPAND_SZ value straight from its HKCU-relative path. Absent key, absent
// value and a non-string type all read as "no value".
[[nodiscard]] std::optional<std::wstring> readStringValueAt(const std::wstring& path,
                                                            const wchar_t* const name) {
    RegistryKey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0U, KEY_READ, key.receive()) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    DWORD type = 0U;
    DWORD bytes = 0U;
    LSTATUS status = RegQueryValueExW(key.get(), name, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return std::nullopt;
    }
    std::wstring value(static_cast<std::size_t>(bytes / sizeof(wchar_t)) + 1U, L'\0');
    status = RegQueryValueExW(
        key.get(), name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes);
    if (status != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(static_cast<std::size_t>(bytes) / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] bool keyExistsAt(const std::wstring& path) {
    RegistryKey key;
    return RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0U, KEY_READ, key.receive()) ==
           ERROR_SUCCESS;
}

// True only when a key holds neither subkeys nor values, including its unnamed default value.
// This is the condition for removing a key this registration shares with other tools.
[[nodiscard]] bool isEmptyKeyAt(const std::wstring& path) {
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

void deleteKeyIfEmpty(const std::wstring& path) {
    if (isEmptyKeyAt(path)) {
        static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str()));
    }
}

void notifyAssociationsChanged() {
    // Explorer caches associations; without this the new choice only appears after re-login.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
}

[[nodiscard]] std::wstring appendPath(const std::wstring& base, const std::wstring_view leaf) {
    std::wstring path = base;
    path += L'\\';
    path.append(leaf);
    return path;
}

[[nodiscard]] std::wstring videoProgIdPath(const FileAssociationRoots& roots) {
    return appendPath(roots.classes, kVideoProgId);
}

[[nodiscard]] std::wstring imageProgIdPath(const FileAssociationRoots& roots) {
    return appendPath(roots.classes, kImageProgId);
}

[[nodiscard]] std::wstring applicationsPath(const FileAssociationRoots& roots) {
    return appendPath(roots.classes, kApplicationsKey);
}

// Which ProgID an extension is registered under: video and image are separate registrations so
// the user can grant the default for one kind without the other.
[[nodiscard]] std::wstring_view progIdForExtension(const std::wstring_view extension) {
    for (const std::wstring_view candidate : supportedVideoExtensions()) {
        if (candidate == extension) {
            return kVideoProgId;
        }
    }
    return kImageProgId;
}

// One REG_SZ value of the registration shape: the HKCU-relative key, the value (an empty name
// is the default value) and the exact content a current registration holds.
struct ShapeValue {
    std::wstring keyPath;
    std::wstring valueName;
    std::wstring content;
};

// Every value a complete registration owns. Registration writes it, the read-back verifies it,
// and the status health compares against it - one description of "correct", not three.
[[nodiscard]] std::vector<ShapeValue> registrationShape(const FileAssociationTargets& targets,
                                                        const FileAssociationRoots& roots) {
    const std::wstring executable = targets.executable.wstring();
    // The single-file default-open command. The path and the %1 placeholder are explicitly
    // quoted, so a directory with spaces or non-ASCII characters cannot split into arguments.
    const std::wstring openCommand = quoteWindowsArgument(executable) + L" \"%1\"";
    const std::wstring icon = executable + L",0";

    std::vector<ShapeValue> shape;
    const auto add =
        [&shape](std::wstring keyPath, const std::wstring_view valueName, std::wstring content) {
            shape.push_back(
                ShapeValue{std::move(keyPath), std::wstring{valueName}, std::move(content)});
        };

    for (const auto& [progIdPath, title] : std::array<std::pair<std::wstring, const wchar_t*>, 2U>{
             std::pair<std::wstring, const wchar_t*>{videoProgIdPath(roots), kVideoTypeTitle},
             std::pair<std::wstring, const wchar_t*>{imageProgIdPath(roots), kImageTypeTitle},
         }) {
        add(progIdPath, L"", title);
        add(appendPath(progIdPath, L"DefaultIcon"), L"", icon);
        add(appendPath(progIdPath, L"shell\\open\\command"), L"", openCommand);
    }

    const std::wstring applications = applicationsPath(roots);
    add(appendPath(applications, L"DefaultIcon"), L"", icon);
    add(appendPath(applications, L"shell\\open\\command"), L"", openCommand);
    const std::wstring supportedTypes = appendPath(applications, L"SupportedTypes");

    const std::wstring fileAssociations = appendPath(roots.capabilities, L"FileAssociations");
    const auto addExtension = [&](const std::wstring_view extension) {
        const std::wstring_view progId = progIdForExtension(extension);
        add(supportedTypes, extension, L"");
        add(appendPath(appendPath(roots.classes, extension), L"OpenWithProgids"), progId, L"");
        add(fileAssociations, extension, std::wstring{progId});
    };
    for (const std::wstring_view extension : supportedVideoExtensions()) {
        addExtension(extension);
    }
    for (const std::wstring_view extension : supportedImageExtensions()) {
        addExtension(extension);
    }

    add(roots.capabilities, L"ApplicationName", kApplicationName);
    add(roots.capabilities, L"ApplicationDescription", kApplicationDescription);
    add(roots.registeredApplications, roots.registeredName, roots.capabilities);
    return shape;
}

// The value writes of a path repair: only the entries that embed the executable path plus the
// RegisteredApplications pointer, because that pointer is what makes the paths reachable at all.
[[nodiscard]] std::vector<ShapeValue> repairShape(const FileAssociationTargets& targets,
                                                  const FileAssociationRoots& roots) {
    const std::wstring executable = targets.executable.wstring();
    std::vector<ShapeValue> repair;
    for (ShapeValue& value : registrationShape(targets, roots)) {
        const bool carriesPath = value.content.find(executable) != std::wstring::npos;
        if (carriesPath || value.keyPath == roots.registeredApplications) {
            repair.push_back(std::move(value));
        }
    }
    return repair;
}

// A whole-or-nothing registry write: every key (ancestors included) is opened first - most real
// failures happen there, before anything is written - every value write captures the previous
// state, and rollback restores values and deletes keys this transaction created. The registry
// has no supported transaction anymore (KTM registry transactions are deprecated), so the undo
// is explicit.
class RegistryWriteTransaction final {
public:
    RegistryWriteTransaction() = default;
    RegistryWriteTransaction(const RegistryWriteTransaction&) = delete;
    RegistryWriteTransaction& operator=(const RegistryWriteTransaction&) = delete;
    ~RegistryWriteTransaction() = default;

    // Phase A: create or open every key the writes will touch. Fails before any value changes.
    [[nodiscard]] LSTATUS prepare(const std::vector<std::wstring>& keyPaths,
                                  std::wstring* const failurePath) {
        for (const std::wstring& path : keyPaths) {
            Entry entry;
            entry.path = path;
            DWORD disposition = 0U;
            const LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER,
                                                   path.c_str(),
                                                   0U,
                                                   nullptr,
                                                   REG_OPTION_NON_VOLATILE,
                                                   KEY_READ | KEY_WRITE,
                                                   nullptr,
                                                   entry.key.receive(),
                                                   &disposition);
            if (status != ERROR_SUCCESS) {
                if (failurePath != nullptr) {
                    *failurePath = path;
                }
                // Earlier iterations may already have created keys; undo that before returning.
                static_cast<void>(rollback());
                return status;
            }
            entry.createdNew = disposition == REG_CREATED_NEW_KEY;
            keys_.push_back(std::move(entry));
        }
        return ERROR_SUCCESS;
    }

    // Phase B: write one REG_SZ value through a prepared key, recording undo data first.
    [[nodiscard]] LSTATUS writeString(const std::wstring& keyPath,
                                      const std::wstring& valueName,
                                      const std::wstring& content) {
        Entry* const entry = findKey(keyPath);
        if (entry == nullptr) {
            return ERROR_PATH_NOT_FOUND;
        }
        // Test-only seam (see FileAssociationTestHooks.h): the storage always exists; without
        // an installed index this is one relaxed atomic load of zero on a write the registry is
        // about to do anyway.
        const std::uint32_t writeIndex = ++stagedWrites_;
        if (const std::uint32_t failAt =
                testing::fileAssociationFailingWriteIndex.load(std::memory_order_relaxed);
            failAt != 0U && writeIndex == failAt) {
            return ERROR_ACCESS_DENIED;
        }
        const wchar_t* const name = valueName.empty() ? nullptr : valueName.c_str();
        // Capture the previous state whatever its type is: rollback has to restore a REG_DWORD
        // an earlier layout left behind just as faithfully as a REG_SZ.
        DWORD type = 0U;
        DWORD bytes = 0U;
        LSTATUS status = RegQueryValueExW(entry->key.get(), name, nullptr, &type, nullptr, &bytes);
        ValueUndo undo;
        undo.keyPath = keyPath;
        undo.valueName = valueName;
        if (status == ERROR_SUCCESS) {
            undo.existed = true;
            undo.type = type;
            undo.bytes.resize(bytes);
            status =
                RegQueryValueExW(entry->key.get(), name, nullptr, &type, undo.bytes.data(), &bytes);
            if (status != ERROR_SUCCESS) {
                return status;
            }
        } else if (status != ERROR_FILE_NOT_FOUND) {
            return status;
        }
        status = writeStringValue(entry->key.get(), name, content);
        if (status != ERROR_SUCCESS) {
            return status;
        }
        undos_.push_back(std::move(undo));
        return ERROR_SUCCESS;
    }

    // Restores every value written so far, in reverse, and removes keys created by prepare().
    // Returns false when a restore step itself failed; the caller reports the residue honestly.
    [[nodiscard]] bool rollback() {
        bool clean = true;
        for (auto undo = undos_.rbegin(); undo != undos_.rend(); ++undo) {
            Entry* const entry = findKey(undo->keyPath);
            if (entry == nullptr) {
                clean = false;
                continue;
            }
            const wchar_t* const name = undo->valueName.empty() ? nullptr : undo->valueName.c_str();
            LSTATUS status = ERROR_SUCCESS;
            if (undo->existed) {
                status = RegSetValueExW(entry->key.get(),
                                        name,
                                        0U,
                                        undo->type,
                                        undo->bytes.data(),
                                        static_cast<DWORD>(undo->bytes.size()));
            } else {
                status = RegDeleteValueW(entry->key.get(), name);
                if (status == ERROR_FILE_NOT_FOUND) {
                    status = ERROR_SUCCESS;
                }
            }
            clean = clean && status == ERROR_SUCCESS;
        }
        // Keys created by this transaction can only hold values written after prepare(), and
        // every one of those has just been removed, so reverse creation order deletes exactly
        // what this transaction added. Pre-existing keys keep their restored state.
        for (auto entry = keys_.rbegin(); entry != keys_.rend(); ++entry) {
            if (!entry->createdNew) {
                continue;
            }
            const LSTATUS status = RegDeleteKeyW(HKEY_CURRENT_USER, entry->path.c_str());
            clean = clean && (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ||
                              status == ERROR_PATH_NOT_FOUND);
        }
        undos_.clear();
        return clean;
    }

private:
    struct Entry final {
        RegistryKey key;
        std::wstring path;
        bool createdNew{false};
    };

    struct ValueUndo final {
        std::wstring keyPath;
        std::wstring valueName;
        bool existed{false};
        DWORD type{0U};
        std::vector<BYTE> bytes;
    };

    [[nodiscard]] Entry* findKey(const std::wstring& path) {
        for (Entry& entry : keys_) {
            if (entry.path == path) {
                return &entry;
            }
        }
        return nullptr;
    }

    std::vector<Entry> keys_;
    std::vector<ValueUndo> undos_;
    std::uint32_t stagedWrites_{0U};
};

// Every key the shape's values live in, ancestors first and without duplicates: prepare() has
// to know which intermediate keys it created so rollback can remove them again.
void collectKeyPaths(const std::vector<ShapeValue>& shape, std::vector<std::wstring>& paths) {
    const auto addOnce = [&paths](const std::wstring& path) {
        if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
            paths.push_back(path);
        }
    };
    for (const ShapeValue& value : shape) {
        std::wstring ancestor;
        const std::wstring& path = value.keyPath;
        for (std::size_t position = 0U; position < path.size();) {
            const std::size_t separator = path.find(L'\\', position);
            const std::size_t end = separator == std::wstring::npos ? path.size() : separator;
            ancestor.append(path, position, end - position);
            addOnce(ancestor);
            if (separator == std::wstring::npos) {
                break;
            }
            ancestor += L'\\';
            position = separator + 1U;
        }
    }
}

[[nodiscard]] bool shapeMatches(const std::vector<ShapeValue>& shape,
                                std::wstring* const firstMismatch) {
    for (const ShapeValue& value : shape) {
        const std::optional<std::wstring> actual = readStringValueAt(
            value.keyPath, value.valueName.empty() ? nullptr : value.valueName.c_str());
        if (!actual.has_value() || *actual != value.content) {
            if (firstMismatch != nullptr) {
                *firstMismatch = value.keyPath;
            }
            return false;
        }
    }
    return true;
}

[[nodiscard]] FileAssociationResult applyShape(const std::vector<ShapeValue>& shape) {
    std::vector<std::wstring> keyPaths;
    collectKeyPaths(shape, keyPaths);
    RegistryWriteTransaction transaction;
    std::wstring failurePath;
    LSTATUS status = transaction.prepare(keyPaths, &failurePath);
    if (status != ERROR_SUCCESS) {
        return {FileAssociationState::Failed,
                describeFailure(failurePath, status) + " (rolled back)"};
    }
    for (const ShapeValue& value : shape) {
        status = transaction.writeString(value.keyPath, value.valueName, value.content);
        if (status != ERROR_SUCCESS) {
            const bool clean = transaction.rollback();
            return {FileAssociationState::Failed,
                    describeFailure(value.keyPath, status) +
                        (clean ? " (rolled back)" : " (rollback incomplete)")};
        }
    }
    // Read the shape back: a write the registry silently dropped must not be reported as
    // success, and a registration that cannot verify itself is rolled back like a failure.
    std::wstring mismatch;
    if (!shapeMatches(shape, &mismatch)) {
        const bool clean = transaction.rollback();
        return {FileAssociationState::Failed,
                toUtf8(mismatch) + " did not keep the value just written" +
                    (clean ? " (rolled back)" : " (rollback incomplete)")};
    }
    return {FileAssociationState::Ok, {}};
}

[[nodiscard]] bool anyOwnKeyExists(const FileAssociationRoots& roots) {
    return keyExistsAt(videoProgIdPath(roots)) || keyExistsAt(imageProgIdPath(roots)) ||
           keyExistsAt(applicationsPath(roots)) || keyExistsAt(roots.capabilities);
}

[[nodiscard]] DefaultOwner ownerForExtension(const std::wstring_view extension,
                                             const FileAssociationRoots& roots) {
    // Read-only by design: Windows protects UserChoice against programmatic writes, and
    // silently taking over a format is exactly what this registration must never do.
    const std::wstring userChoice =
        appendPath(appendPath(roots.fileExts, extension), L"UserChoice");
    const std::optional<std::wstring> progId = readStringValueAt(userChoice, L"ProgID");
    if (!progId.has_value()) {
        return DefaultOwner::NotSet;
    }
    const std::wstring own{progIdForExtension(extension)};
    return _wcsicmp(progId->c_str(), own.c_str()) == 0 ? DefaultOwner::ThisApplication
                                                       : DefaultOwner::OtherApplication;
}

// NotSet means no UserChoice exists for the kind at all; ThisApplication means every extension
// of the kind resolves to this ProgID; anything else - another application, or a partial grant -
// is OtherApplication, because for the user's question "is it my default now?" only all of it
// counts as yes.
[[nodiscard]] DefaultOwner ownerForKind(const std::span<const std::wstring_view> extensions,
                                        const FileAssociationRoots& roots) {
    bool anySet = false;
    bool allOurs = true;
    for (const std::wstring_view extension : extensions) {
        switch (ownerForExtension(extension, roots)) {
        case DefaultOwner::NotSet:
            allOurs = false;
            break;
        case DefaultOwner::ThisApplication:
            anySet = true;
            break;
        case DefaultOwner::OtherApplication:
            anySet = true;
            allOurs = false;
            break;
        }
    }
    if (!anySet) {
        return DefaultOwner::NotSet;
    }
    return allOurs ? DefaultOwner::ThisApplication : DefaultOwner::OtherApplication;
}

[[nodiscard]] bool launchWithShellExecute(const std::wstring& uri) {
    const HINSTANCE result =
        ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

[[nodiscard]] unsigned long runningWindowsBuildNumber() {
    // RtlGetVersion is the one version query that does not lie to an unmanifested process.
    using RtlGetVersionPtr = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return 0UL;
    }
    // Function-pointer to function-pointer: casting through void* is what clang-tidy's
    // bugprone-casting-through-void rejects, and a direct reinterpret_cast between function
    // pointer types is the supported spelling for GetProcAddress results on MSVC.
    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(
        reinterpret_cast<void (*)()>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (rtlGetVersion == nullptr) {
        return 0UL;
    }
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0) {
        return 0UL;
    }
    return info.dwBuildNumber;
}

[[nodiscard]] bool deleteTreeIfPresent(const std::wstring& path, std::string* const error) {
    const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ||
        status == ERROR_PATH_NOT_FOUND) {
        return true;
    }
    if (error != nullptr) {
        *error = describeFailure(path, status);
    }
    return false;
}

} // namespace

namespace testing {

ScopedFileAssociationWriteFailure::ScopedFileAssociationWriteFailure(
    const std::uint32_t writeIndex) noexcept {
    if (writeIndex != 0U) {
        fileAssociationFailingWriteIndex.store(writeIndex, std::memory_order_relaxed);
        installed_ = true;
    }
}

ScopedFileAssociationWriteFailure::~ScopedFileAssociationWriteFailure() {
    if (installed_) {
        fileAssociationFailingWriteIndex.store(0U, std::memory_order_relaxed);
    }
}

} // namespace testing

FileAssociationResult registerFileAssociation(const FileAssociationTargets& targets,
                                              const FileAssociationRoots& roots) {
    if (targets.executable.empty()) {
        return {FileAssociationState::Failed, "the registration needs CompareStation.exe"};
    }
    std::error_code existenceError;
    if (!std::filesystem::exists(targets.executable, existenceError)) {
        // Registering a path Windows cannot launch would leave a dead entry that looks current,
        // exactly the ZIP-moved failure this registration is supposed to repair.
        return {FileAssociationState::Failed,
                "the executable does not exist: " + targets.executable.string()};
    }
    const FileAssociationResult result = applyShape(registrationShape(targets, roots));
    if (result.state == FileAssociationState::Ok) {
        notifyAssociationsChanged();
    }
    return result;
}

FileAssociationResult repairFileAssociationPaths(const FileAssociationTargets& targets,
                                                 const FileAssociationRoots& roots) {
    if (!anyOwnKeyExists(roots)) {
        return {FileAssociationState::Failed, "there is no registration to repair; register first"};
    }
    if (targets.executable.empty()) {
        return {FileAssociationState::Failed, "the repair needs the running executable path"};
    }
    const FileAssociationResult result = applyShape(repairShape(targets, roots));
    if (result.state == FileAssociationState::Ok) {
        notifyAssociationsChanged();
    }
    return result;
}

FileAssociationResult removeFileAssociation(const FileAssociationRoots& roots) {
    std::string error;
    for (const std::wstring& path : {videoProgIdPath(roots),
                                     imageProgIdPath(roots),
                                     applicationsPath(roots),
                                     roots.capabilities}) {
        if (!deleteTreeIfPresent(path, &error)) {
            return {FileAssociationState::Failed, error};
        }
    }
    // The per-extension keys and RegisteredApplications are shared with other applications:
    // remove only this application's value, then the key itself only when nothing else uses it.
    // UserChoice is never touched - Windows resolves a removed ProgID on its own, and restoring
    // a guessed "previous default" is exactly what an uninstall must not invent.
    const auto releaseExtension = [&](const std::wstring_view extension) {
        const std::wstring openWithProgids =
            appendPath(appendPath(roots.classes, extension), L"OpenWithProgids");
        {
            RegistryKey key;
            if (RegOpenKeyExW(HKEY_CURRENT_USER,
                              openWithProgids.c_str(),
                              0U,
                              KEY_READ | KEY_SET_VALUE,
                              key.receive()) == ERROR_SUCCESS) {
                const std::wstring progId{progIdForExtension(extension)};
                const LSTATUS status = RegDeleteValueW(key.get(), progId.c_str());
                if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
                    error = describeFailure(openWithProgids, status);
                    return false;
                }
            }
        }
        deleteKeyIfEmpty(openWithProgids);
        deleteKeyIfEmpty(appendPath(roots.classes, extension));
        return true;
    };
    for (const std::wstring_view extension : supportedVideoExtensions()) {
        if (!releaseExtension(extension)) {
            return {FileAssociationState::Failed, error};
        }
    }
    for (const std::wstring_view extension : supportedImageExtensions()) {
        if (!releaseExtension(extension)) {
            return {FileAssociationState::Failed, error};
        }
    }
    {
        RegistryKey key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          roots.registeredApplications.c_str(),
                          0U,
                          KEY_READ | KEY_SET_VALUE,
                          key.receive()) == ERROR_SUCCESS) {
            const LSTATUS status = RegDeleteValueW(key.get(), roots.registeredName.c_str());
            if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
                return {FileAssociationState::Failed,
                        describeFailure(roots.registeredApplications, status)};
            }
        }
    }
    deleteKeyIfEmpty(roots.registeredApplications);
    notifyAssociationsChanged();
    return {FileAssociationState::Ok, {}};
}

FileAssociationStatus readFileAssociationStatus(const FileAssociationTargets& targets,
                                                const FileAssociationRoots& roots) {
    FileAssociationStatus status;
    if (!anyOwnKeyExists(roots)) {
        status.health = AssociationHealth::NotRegistered;
    } else {
        status.health = shapeMatches(registrationShape(targets, roots), nullptr)
                            ? AssociationHealth::Current
                            : AssociationHealth::StalePath;
    }
    status.videoDefault = ownerForKind(supportedVideoExtensions(), roots);
    status.imageDefault = ownerForKind(supportedImageExtensions(), roots);
    return status;
}

bool supportsRegisteredAppUserParameter(const unsigned long buildNumber) {
    // Windows 11 (build 22000) is the gate; the parameter additionally needs the 2023-04
    // cumulative update on older Windows 11 releases, which no API reports. An unpatched
    // system ignores the parameter and lands on the generic page - the page the fallback
    // opens anyway - so gating on the build cannot strand the user anywhere worse.
    return buildNumber >= 22000UL;
}

std::wstring defaultAppsSettingsUri(const std::wstring_view registeredName, const bool deepLink) {
    if (!deepLink) {
        return L"ms-settings:defaultapps";
    }
    std::wstring uri = L"ms-settings:defaultapps?registeredAppUser=";
    // Escape the name the way a URI query value must be, so a registered name with spaces or
    // non-ASCII characters still resolves to one application.
    char utf8[8]{};
    static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
    for (const wchar_t character : registeredName) {
        const bool unreserved = (character >= L'0' && character <= L'9') ||
                                (character >= L'a' && character <= L'z') ||
                                (character >= L'A' && character <= L'Z') || character == L'-' ||
                                character == L'_' || character == L'.' || character == L'~';
        if (unreserved) {
            uri.push_back(character);
            continue;
        }
        const int bytes = WideCharToMultiByte(
            CP_UTF8, 0, &character, 1, utf8, static_cast<int>(std::size(utf8)), nullptr, nullptr);
        for (int index = 0; index < bytes; ++index) {
            const auto byte = static_cast<unsigned char>(utf8[index]);
            uri.push_back(L'%');
            uri.push_back(kHex[byte >> 4U]);
            uri.push_back(kHex[byte & 0x0FU]);
        }
    }
    return uri;
}

FileAssociationResult openDefaultAppsSettings(const FileAssociationRoots& roots,
                                              const DefaultAppsLaunchOverrides& overrides) {
    const unsigned long build =
        overrides.buildNumber != 0UL ? overrides.buildNumber : runningWindowsBuildNumber();
    const bool deepLink = build != 0UL && supportsRegisteredAppUserParameter(build);
    const std::function<bool(const std::wstring&)> launch =
        overrides.launch ? overrides.launch
                         : static_cast<bool (*)(const std::wstring&)>(launchWithShellExecute);

    // A refused or unavailable deep link is downgraded to the generic default-apps page, which
    // is the documented behaviour for Windows versions without the parameter.
    if (launch(defaultAppsSettingsUri(roots.registeredName, deepLink))) {
        return {FileAssociationState::Ok, {}};
    }
    if (deepLink && launch(defaultAppsSettingsUri(roots.registeredName, false))) {
        return {FileAssociationState::Ok, {}};
    }
    return {FileAssociationState::Failed, "the default-apps settings page could not be opened"};
}

} // namespace dvs::shell
