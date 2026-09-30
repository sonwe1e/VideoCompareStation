#include "dvs/shell/ExplorerCommandRegistration.h"

#include "ExplorerCommand.h"
#include "ExplorerCommandSupport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <array>
#include <combaseapi.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <windows.h>

namespace dvs::shell {
namespace {

// Every key this suite writes lives under one scratch subtree per process, so a test run can never
// disturb the user's own registration - the reason registry tests are usually avoided.
[[nodiscard]] std::wstring scratchSubtree() {
    return L"Software\\CompareStationTests\\" + std::to_wstring(GetCurrentProcessId());
}

[[nodiscard]] ExplorerRegistrationRoots scratchRoots() {
    ExplorerRegistrationRoots roots;
    roots.classes = scratchSubtree() + L"\\Classes";
    roots.settings = scratchSubtree() + L"\\Settings";
    return roots;
}

void removeScratchSubtree() {
    static_cast<void>(RegDeleteTreeW(HKEY_CURRENT_USER, scratchSubtree().c_str()));
}

[[nodiscard]] std::optional<std::wstring> readString(const ExplorerRegistrationRoots& roots,
                                                     const std::wstring& relative,
                                                     const wchar_t* const name) {
    const std::wstring path = roots.classes + L"\\" + relative;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0U, KEY_READ, &key) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    DWORD type = 0U;
    DWORD bytes = 0U;
    LSTATUS status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        RegCloseKey(key);
        return std::nullopt;
    }
    std::wstring value(static_cast<std::size_t>(bytes / sizeof(wchar_t)) + 1U, L'\0');
    status =
        RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(static_cast<std::size_t>(bytes) / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

// GoogleTest messages are narrow, and an extension or a registry path only has to be readable in a
// failure report, so the diagnostic narrows it instead of failing to compile on a wide string.
[[nodiscard]] std::string narrow(const std::wstring_view text) {
    std::string narrowed;
    narrowed.reserve(text.size());
    for (const wchar_t character : text) {
        narrowed.push_back(static_cast<char>(character));
    }
    return narrowed;
}

void expectValue(const ExplorerRegistrationRoots& roots,
                 const std::wstring& relative,
                 const wchar_t* const name,
                 const std::wstring& expected) {
    const std::optional<std::wstring> actual = readString(roots, relative, name);
    ASSERT_TRUE(actual.has_value()) << narrow(relative) << " has no value '" << narrow(name) << "'";
    EXPECT_EQ(*actual, expected) << narrow(relative) << " value '" << narrow(name) << "'";
}

[[nodiscard]] std::wstring verbKey(const std::wstring_view extension) {
    return L"SystemFileAssociations\\" + std::wstring{extension} +
           L"\\shell\\CompareStation.Compare";
}

[[nodiscard]] bool keyExists(const ExplorerRegistrationRoots& roots, const std::wstring& relative) {
    HKEY key = nullptr;
    const std::wstring path = roots.classes + L"\\" + relative;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0U, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(key);
    return true;
}

// The registered CLSID is whatever the verb points at, which is also what Explorer resolves.
[[nodiscard]] std::wstring registeredClsid(const ExplorerRegistrationRoots& roots) {
    return readString(roots, verbKey(L".png"), L"ExplorerCommandHandler").value_or(L"");
}

// The command's own CLSID, for the checks that have to look at the real per-user hive.
[[nodiscard]] std::wstring clsidOfThisCommand() {
    std::array<wchar_t, 64U> buffer{};
    const int length =
        StringFromGUID2(kExplorerCommandClsid, buffer.data(), static_cast<int>(buffer.size()));
    if (length <= 1) {
        return {};
    }
    return std::wstring{buffer.data(), static_cast<std::size_t>(length - 1)};
}

// Opens a path relative to HKEY_CURRENT_USER, for the keys outside this fixture's scratch roots.
[[nodiscard]] bool keyExistsAt(const std::wstring& subkey) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0U, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(key);
    return true;
}

void writeDwordAt(const std::wstring& subkey, const wchar_t* const name, const DWORD value) {
    HKEY key = nullptr;
    ASSERT_EQ(RegCreateKeyExW(HKEY_CURRENT_USER,
                              subkey.c_str(),
                              0U,
                              nullptr,
                              REG_OPTION_NON_VOLATILE,
                              KEY_WRITE,
                              nullptr,
                              &key,
                              nullptr),
              ERROR_SUCCESS)
        << narrow(subkey);
    ASSERT_EQ(RegSetValueExW(
                  key, name, 0U, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)),
              ERROR_SUCCESS);
    RegCloseKey(key);
}

[[nodiscard]] std::optional<DWORD> readDwordAt(const std::wstring& subkey,
                                               const wchar_t* const name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0U, KEY_READ, &key) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    DWORD type = 0U;
    DWORD bytes = sizeof(DWORD);
    DWORD value = 0U;
    const LSTATUS status =
        RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_DWORD) {
        return std::nullopt;
    }
    return value;
}

void writeStringValue(const ExplorerRegistrationRoots& roots,
                      const std::wstring& relative,
                      const wchar_t* const name,
                      const std::wstring& value) {
    HKEY key = nullptr;
    const std::wstring path = roots.classes + L"\\" + relative;
    ASSERT_EQ(RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0U, KEY_SET_VALUE, &key),
              ERROR_SUCCESS)
        << narrow(path);
    const DWORD bytes = static_cast<DWORD>((value.size() + 1U) * sizeof(wchar_t));
    ASSERT_EQ(
        RegSetValueExW(key, name, 0U, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes),
        ERROR_SUCCESS);
    RegCloseKey(key);
}

// The keys a test writes by hand are the ones an earlier layout, another tool, or a user would have
// left behind, so they are created the way the system would create them rather than through the
// registration this suite is testing.
void createScratchKey(const std::wstring& relative) {
    HKEY key = nullptr;
    const std::wstring path = scratchSubtree() + L"\\Classes\\" + relative;
    ASSERT_EQ(RegCreateKeyExW(HKEY_CURRENT_USER,
                              path.c_str(),
                              0U,
                              nullptr,
                              REG_OPTION_NON_VOLATILE,
                              KEY_WRITE,
                              nullptr,
                              &key,
                              nullptr),
              ERROR_SUCCESS)
        << narrow(path);
    RegCloseKey(key);
}

void writeScratchString(const std::wstring& relative,
                        const wchar_t* const name,
                        const std::wstring& value) {
    HKEY key = nullptr;
    const std::wstring path = scratchSubtree() + L"\\Classes\\" + relative;
    ASSERT_EQ(RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0U, KEY_SET_VALUE, &key),
              ERROR_SUCCESS)
        << narrow(path);
    const DWORD bytes = static_cast<DWORD>((value.size() + 1U) * sizeof(wchar_t));
    ASSERT_EQ(
        RegSetValueExW(key, name, 0U, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes),
        ERROR_SUCCESS);
    RegCloseKey(key);
}

class ExplorerCommandRegistrationTests : public ::testing::Test {
protected:
    void SetUp() override {
        removeScratchSubtree();
        roots_ = scratchRoots();
        // The registration refuses a directory whose files are not there, so the fixture has to
        // hold real ones; empty files are enough, because only their existence is read.
        const std::filesystem::path scratch = std::filesystem::temp_directory_path() /
                                              "CompareStationRegistrationTests" /
                                              std::to_wstring(GetCurrentProcessId());
        installRoot_ = scratch / "install";
        movedRoot_ = scratch / "moved";
        std::error_code ignored;
        std::filesystem::remove_all(scratch, ignored);
        for (const std::filesystem::path& root : {installRoot_, movedRoot_}) {
            std::filesystem::create_directories(root, ignored);
            std::ofstream{root / "CompareStation.exe"};
            std::ofstream{root / "CompareStationShell-9.9.dll"};
        }
        targets_.executable = installRoot_ / "CompareStation.exe";
        targets_.shellLibrary = installRoot_ / "CompareStationShell-9.9.dll";
    }

    void TearDown() override {
        std::error_code ignored;
        const std::filesystem::path container = installRoot_.parent_path().parent_path();
        std::filesystem::remove_all(installRoot_.parent_path(), ignored);
        // The container is shared by every process running this suite, so a plain remove is used:
        // it fails harmlessly while another process still has a scratch directory in it.
        static_cast<void>(std::filesystem::remove(container, ignored));
        removeScratchSubtree();
        // Leave no empty container key behind either.
        static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\CompareStationTests"));
    }

    ExplorerRegistrationRoots roots_;
    ExplorerRegistrationTargets targets_;
    std::filesystem::path installRoot_;
    std::filesystem::path movedRoot_;
};

TEST_F(ExplorerCommandRegistrationTests, OffersTheEntryOnAFreshExtractWithoutSetup) {
    // An absent marker means enabled: unpacking the ZIP has to be enough on its own.
    EXPECT_TRUE(isExplorerCommandEnabled(roots_));
}

TEST_F(ExplorerCommandRegistrationTests, RegistersEveryExtensionTheHandlerAccepts) {
    const ExplorerRegistrationResult result = ensureExplorerCommandRegistered(targets_, roots_);
    ASSERT_EQ(result.state, ExplorerRegistrationState::Registered) << result.error;

    const std::wstring clsid = registeredClsid(roots_);
    ASSERT_FALSE(clsid.empty());
    const std::wstring inproc = L"CLSID\\" + clsid + L"\\InprocServer32";
    expectValue(roots_, inproc, nullptr, targets_.shellLibrary.wstring());
    expectValue(roots_, inproc, L"ThreadingModel", L"Apartment");
    expectValue(roots_, L"CLSID\\" + clsid, nullptr, L"CompareStation Explorer comparison command");

    std::size_t checked = 0U;
    for (const std::wstring_view extension : supportedVideoExtensions()) {
        const std::wstring relative = verbKey(extension);
        ASSERT_TRUE(keyExists(roots_, relative)) << narrow(extension);
        expectValue(roots_, relative, L"MUIVerb", L"Compare with CompareStation");
        expectValue(roots_, relative, L"Icon", targets_.executable.wstring() + L",0");
        expectValue(roots_, relative, L"ExplorerCommandHandler", clsid);
        expectValue(roots_, relative, L"MultiSelectModel", L"Player");
        ++checked;
    }
    for (const std::wstring_view extension : supportedImageExtensions()) {
        const std::wstring relative = verbKey(extension);
        ASSERT_TRUE(keyExists(roots_, relative)) << narrow(extension);
        expectValue(roots_, relative, L"MUIVerb", L"Compare with CompareStation");
        expectValue(roots_, relative, L"Icon", targets_.executable.wstring() + L",0");
        expectValue(roots_, relative, L"ExplorerCommandHandler", clsid);
        expectValue(roots_, relative, L"MultiSelectModel", L"Player");
        ++checked;
    }
    // The entry is offered for exactly the extensions the handler accepts, no copy of the list.
    EXPECT_EQ(checked, supportedVideoExtensions().size() + supportedImageExtensions().size());
}

TEST_F(ExplorerCommandRegistrationTests, LeavesACurrentRegistrationUntouched) {
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);

    // A value this registration never writes is the proof that the next call changed nothing: a
    // delete-and-recreate would take it along.
    const std::wstring relative = verbKey(L".png");
    HKEY key = nullptr;
    const std::wstring path = roots_.classes + L"\\" + relative;
    ASSERT_EQ(RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0U, KEY_SET_VALUE, &key),
              ERROR_SUCCESS);
    const DWORD sentinel = 0x5A5A5A5AU;
    ASSERT_EQ(RegSetValueExW(key,
                             L"Sentinel",
                             0U,
                             REG_DWORD,
                             reinterpret_cast<const BYTE*>(&sentinel),
                             sizeof(sentinel)),
              ERROR_SUCCESS);
    RegCloseKey(key);

    EXPECT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::AlreadyCurrent);

    key = nullptr;
    ASSERT_EQ(RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0U, KEY_READ, &key), ERROR_SUCCESS);
    DWORD type = 0U;
    DWORD bytes = sizeof(DWORD);
    DWORD stored = 0U;
    EXPECT_EQ(RegQueryValueExW(
                  key, L"Sentinel", nullptr, &type, reinterpret_cast<BYTE*>(&stored), &bytes),
              ERROR_SUCCESS);
    EXPECT_EQ(stored, sentinel);
    RegCloseKey(key);
}

TEST_F(ExplorerCommandRegistrationTests, RepairsATamperedValue) {
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    const std::wstring relative = verbKey(L".png");
    writeStringValue(roots_, relative, L"MUIVerb", L"Someone else's menu text");

    EXPECT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    expectValue(roots_, relative, L"MUIVerb", L"Compare with CompareStation");
}

TEST_F(ExplorerCommandRegistrationTests, RepairsAnInstallationThatMoved) {
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);

    ExplorerRegistrationTargets moved;
    moved.executable = movedRoot_ / "CompareStation.exe";
    moved.shellLibrary = movedRoot_ / "CompareStationShell-9.9.dll";
    EXPECT_EQ(ensureExplorerCommandRegistered(moved, roots_).state,
              ExplorerRegistrationState::Registered);

    const std::wstring clsid = registeredClsid(roots_);
    ASSERT_FALSE(clsid.empty());
    expectValue(
        roots_, L"CLSID\\" + clsid + L"\\InprocServer32", nullptr, moved.shellLibrary.wstring());
    expectValue(roots_, verbKey(L".png"), L"Icon", moved.executable.wstring() + L",0");
}

TEST_F(ExplorerCommandRegistrationTests, RefusesToRegisterADirectoryWithoutItsShellLibrary) {
    // Registering a library that is not there would point Explorer at nothing, and because the keys
    // would then match, every later launch would report "already current" and never repair it.
    ExplorerRegistrationTargets missingLibrary = targets_;
    missingLibrary.shellLibrary = installRoot_ / "CompareStationShell-1.0.dll";
    const ExplorerRegistrationResult result =
        ensureExplorerCommandRegistered(missingLibrary, roots_);
    EXPECT_EQ(result.state, ExplorerRegistrationState::Failed);
    EXPECT_FALSE(result.error.empty());
    EXPECT_FALSE(keyExists(roots_, L"CLSID"));

    ExplorerRegistrationTargets missingExecutable = targets_;
    missingExecutable.executable = installRoot_ / "CompareStationCli.exe";
    EXPECT_EQ(ensureExplorerCommandRegistered(missingExecutable, roots_).state,
              ExplorerRegistrationState::Failed);
    EXPECT_FALSE(keyExists(roots_, L"CLSID"));
}

TEST_F(ExplorerCommandRegistrationTests, ReadsADisabledMarkerWrittenAsText) {
    // A value written by hand with RegEdit is a string, and treating it as "enabled" would silently
    // undo the user's decision.
    HKEY key = nullptr;
    ASSERT_EQ(RegCreateKeyExW(HKEY_CURRENT_USER,
                              roots_.settings.c_str(),
                              0U,
                              nullptr,
                              REG_OPTION_NON_VOLATILE,
                              KEY_WRITE,
                              nullptr,
                              &key,
                              nullptr),
              ERROR_SUCCESS);
    const std::wstring off = L"0";
    ASSERT_EQ(RegSetValueExW(key,
                             L"ExplorerContextMenu",
                             0U,
                             REG_SZ,
                             reinterpret_cast<const BYTE*>(off.c_str()),
                             static_cast<DWORD>((off.size() + 1U) * sizeof(wchar_t))),
              ERROR_SUCCESS);
    RegCloseKey(key);

    EXPECT_FALSE(isExplorerCommandEnabled(roots_));
    EXPECT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::DisabledByUser);
    EXPECT_FALSE(keyExists(roots_, L"CLSID"));
}

TEST_F(ExplorerCommandRegistrationTests, PrefersTheUsersDecisionOverAMissingFile) {
    // The user's "off" has to win over a directory that lost its library: nothing can be done about
    // that file, and reporting a failure on every launch would blame the user for it.
    ASSERT_EQ(setExplorerCommandEnabled(false, roots_).state,
              ExplorerRegistrationState::DisabledByUser);
    ExplorerRegistrationTargets gone;
    gone.executable = installRoot_ / "CompareStation-gone.exe";
    gone.shellLibrary = installRoot_ / "CompareStationShell-gone.dll";
    EXPECT_EQ(ensureExplorerCommandRegistered(gone, roots_).state,
              ExplorerRegistrationState::DisabledByUser);
    EXPECT_FALSE(keyExists(roots_, L"CLSID"));

    // With the entry wanted again the missing library is a real failure.
    ASSERT_EQ(setExplorerCommandEnabled(true, roots_).state, ExplorerRegistrationState::Registered);
    EXPECT_EQ(ensureExplorerCommandRegistered(gone, roots_).state,
              ExplorerRegistrationState::Failed);
}

TEST_F(ExplorerCommandRegistrationTests, DerivesTheVersionedShellLibrary) {
    const ExplorerRegistrationTargets derived = installationTargetsFor(
        LR"(C:\apps\CompareStation-2.0.0-windows-x64\CompareStation.exe)", "2.0.0");
    EXPECT_EQ(derived.shellLibrary,
              LR"(C:\apps\CompareStation-2.0.0-windows-x64\CompareStationShell-2.0.dll)");
    EXPECT_EQ(
        installationTargetsFor(LR"(C:\a\CompareStation.exe)", "10.11.12").shellLibrary.filename(),
        L"CompareStationShell-10.11.dll");
    EXPECT_EQ(installationTargetsFor(LR"(C:\a\CompareStation.exe)", "2.0").shellLibrary.filename(),
              L"CompareStationShell-2.0.dll");
    // A version that says nothing about a major.minor cannot name a library, and the registration
    // then refuses instead of guessing.
    EXPECT_TRUE(installationTargetsFor(LR"(C:\a\CompareStation.exe)", "2").shellLibrary.empty());
    EXPECT_TRUE(installationTargetsFor(LR"(C:\a\CompareStation.exe)", "2.x").shellLibrary.empty());
    // The running installation is this test binary, so the library sits next to it.
    const ExplorerRegistrationTargets running = runningInstallationTargets("9.9");
    EXPECT_FALSE(running.executable.empty());
    EXPECT_EQ(running.shellLibrary.filename(), L"CompareStationShell-9.9.dll");
}

TEST_F(ExplorerCommandRegistrationTests, HonoursTheEnvironmentEscapeHatch) {
    static_cast<void>(SetEnvironmentVariableW(L"DVS_DISABLE_SHELL_REGISTRATION", nullptr));
    EXPECT_FALSE(isShellRegistrationDisabledByEnvironment());
    static_cast<void>(SetEnvironmentVariableW(L"DVS_DISABLE_SHELL_REGISTRATION", L"1"));
    EXPECT_TRUE(isShellRegistrationDisabledByEnvironment());
    // The escape hatch has to stop the startup path before it writes anything, and it must leave
    // the user's own registration exactly as it found it.
    const bool realClsidBefore = keyExistsAt(L"Software\\Classes\\CLSID\\" + clsidOfThisCommand());
    EXPECT_EQ(ensureRunningInstallationRegistered("9.9").state,
              ExplorerRegistrationState::DisabledByUser);
    EXPECT_EQ(keyExistsAt(L"Software\\Classes\\CLSID\\" + clsidOfThisCommand()), realClsidBefore);
    static_cast<void>(SetEnvironmentVariableW(L"DVS_DISABLE_SHELL_REGISTRATION", L"0"));
    EXPECT_FALSE(isShellRegistrationDisabledByEnvironment());
    static_cast<void>(SetEnvironmentVariableW(L"DVS_DISABLE_SHELL_REGISTRATION", nullptr));
    EXPECT_FALSE(isShellRegistrationDisabledByEnvironment());
}

TEST_F(ExplorerCommandRegistrationTests, StopsTouchingTheRegistryOnceTheUserSaysNo) {
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    const std::wstring clsid = registeredClsid(roots_);
    ASSERT_FALSE(clsid.empty());
    ASSERT_EQ(removeExplorerCommandRegistration(roots_).state, ExplorerRegistrationState::Removed);
    ASSERT_EQ(setExplorerCommandEnabled(false, roots_).state,
              ExplorerRegistrationState::DisabledByUser);
    ASSERT_FALSE(isExplorerCommandEnabled(roots_));

    // Without this the documented removal would come back on the next launch.
    EXPECT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::DisabledByUser);
    EXPECT_FALSE(keyExists(roots_, verbKey(L".png")));
    EXPECT_FALSE(keyExists(roots_, L"CLSID\\" + clsid));

    // And turning it back on has to make the entry reappear.
    ASSERT_EQ(setExplorerCommandEnabled(true, roots_).state, ExplorerRegistrationState::Registered);
    EXPECT_TRUE(isExplorerCommandEnabled(roots_));
    EXPECT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    EXPECT_TRUE(keyExists(roots_, verbKey(L".png")));
}

TEST_F(ExplorerCommandRegistrationTests, RemovalPrunesOnlyTheKeysItOwns) {
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    const std::wstring clsid = registeredClsid(roots_);
    ASSERT_FALSE(clsid.empty());

    // Three shapes, because the interesting cases sit on different extensions. The values go on one
    // that has no foreign verb: while another verb exists on the same extension the parent cannot
    // be deleted anyway, so a value kept alive next to it would prove nothing. The pruning check
    // needs a third extension with nothing foreign on it, or "the parent is still there" is true
    // for the wrong reason.
    const std::wstring neighbourVerb = L"SystemFileAssociations\\.png\\shell\\Other";
    const std::wstring neighbourClsid = L"CLSID\\{11111111-2222-3333-4444-555555555555}";
    for (const std::wstring& relative : {neighbourVerb, neighbourClsid}) {
        HKEY key = nullptr;
        const std::wstring path = roots_.classes + L"\\" + relative;
        ASSERT_EQ(RegCreateKeyExW(HKEY_CURRENT_USER,
                                  path.c_str(),
                                  0U,
                                  nullptr,
                                  REG_OPTION_NON_VOLATILE,
                                  KEY_WRITE,
                                  nullptr,
                                  &key,
                                  nullptr),
                  ERROR_SUCCESS);
        RegCloseKey(key);
    }
    // RegDeleteKeyW refuses a key that still has subkeys but happily deletes one that only carries
    // values, so pruning without checking values loses another tool's data.
    const std::wstring valuesParent = roots_.classes + L"\\SystemFileAssociations\\.tiff";
    const std::wstring valuesShellParent = valuesParent + L"\\shell";
    writeDwordAt(valuesParent, L"SomeOtherToolValue", 0x11111111U);
    writeDwordAt(valuesShellParent, L"SomeOtherToolValue", 0x22222222U);

    ASSERT_EQ(removeExplorerCommandRegistration(roots_).state, ExplorerRegistrationState::Removed);

    // A foreign verb under the same extension survives and keeps its parents alive.
    EXPECT_TRUE(keyExists(roots_, neighbourVerb));
    EXPECT_TRUE(keyExists(roots_, L"SystemFileAssociations\\.png\\shell"));
    EXPECT_TRUE(keyExists(roots_, neighbourClsid));
    // Another tool's values on a parent this registration shares survive with their keys.
    EXPECT_EQ(readDwordAt(valuesParent, L"SomeOtherToolValue"), 0x11111111U);
    EXPECT_EQ(readDwordAt(valuesShellParent, L"SomeOtherToolValue"), 0x22222222U);
    EXPECT_FALSE(keyExists(roots_, verbKey(L".tiff")));
    // With nothing of another tool in the way, the parents this registration alone created do go.
    EXPECT_FALSE(keyExists(roots_, L"SystemFileAssociations\\.bmp\\shell"));
    EXPECT_FALSE(keyExists(roots_, L"SystemFileAssociations\\.bmp"));
    EXPECT_FALSE(keyExists(roots_, verbKey(L".png")));
    EXPECT_FALSE(keyExists(roots_, L"CLSID\\" + clsid));
    // The shared parent itself is never removed.
    EXPECT_TRUE(keyExists(roots_, L"CLSID"));
}

TEST_F(ExplorerCommandRegistrationTests, RefusesToWriteWithoutBothTargets) {
    ExplorerRegistrationTargets incomplete;
    incomplete.executable = targets_.executable;
    const ExplorerRegistrationResult result = ensureExplorerCommandRegistered(incomplete, roots_);
    EXPECT_EQ(result.state, ExplorerRegistrationState::Failed);
    EXPECT_FALSE(result.error.empty());
    // A refused attempt must not leave a half-registered command behind. Nothing has ever written
    // to this scratch root here, so even the shared CLSID parent key has to be absent.
    EXPECT_FALSE(keyExists(roots_, L"CLSID"));
    EXPECT_FALSE(keyExists(roots_, verbKey(L".png")));
}

TEST_F(ExplorerCommandRegistrationTests, RemovesASecondCopyOfTheCommandUnderTheSameExtension) {
    // A file's menu is built from every verb in its association chain, and the per-extension key
    // is part of that chain next to the SystemFileAssociations one. A copy left there is therefore
    // a second, identical entry in the menu - the one a user cannot tell from the first.
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    const std::wstring leftover = L".mp4\\shell\\CompareStation.Open";
    createScratchKey(leftover);
    writeScratchString(leftover, L"ExplorerCommandHandler", registeredClsid(roots_));
    ASSERT_TRUE(keyExists(roots_, leftover));

    // The second launch is the one that repairs it, so the repair cannot depend on the run that
    // wrote the registration.
    EXPECT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::AlreadyCurrent);
    EXPECT_FALSE(keyExists(roots_, leftover));
    // The parents this registration alone created go with it...
    EXPECT_FALSE(keyExists(roots_, L".mp4\\shell"));
    EXPECT_FALSE(keyExists(roots_, L".mp4"));
    // ...and the entry the user is meant to see is untouched.
    EXPECT_TRUE(keyExists(roots_, verbKey(L".mp4")));
}

TEST_F(ExplorerCommandRegistrationTests, RemovesALeftoverCommandLineUnderTheSameExtension) {
    // An earlier layout registered a plain command line instead of the COM handler. It launches
    // the same application and shows the same title, so it is a second entry by any measure that
    // matters to the user.
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    const std::wstring leftover = L".mkv\\shell\\CompareStation.Legacy\\command";
    createScratchKey(leftover);
    writeScratchString(leftover, nullptr, L"\"" + targets_.executable.wstring() + LR"( " "%1")");

    EXPECT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::AlreadyCurrent);
    EXPECT_FALSE(keyExists(roots_, L".mkv\\shell\\CompareStation.Legacy"));
    EXPECT_FALSE(keyExists(roots_, L".mkv\\shell"));
    EXPECT_TRUE(keyExists(roots_, verbKey(L".mkv")));
}

TEST_F(ExplorerCommandRegistrationTests, LeavesAnotherToolsVerbOnTheSameExtensionAlone) {
    // The sweep is allowed to remove a copy of this command and nothing else: a neighbouring verb
    // on the same extension belongs to whoever registered it, and the entry this registration owns
    // has to survive the sweep that runs on every launch - a sweep that took it with it would
    // delete the menu entry it is supposed to keep.
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    const std::wstring neighbour = L".avi\\shell\\SomeOtherPlayer";
    createScratchKey(neighbour);
    writeScratchString(
        neighbour, L"ExplorerCommandHandler", L"{99999999-8888-7777-6666-555555555555}");

    EXPECT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::AlreadyCurrent);
    EXPECT_TRUE(keyExists(roots_, neighbour));
    EXPECT_TRUE(keyExists(roots_, L".avi\\shell"));
    expectValue(roots_, verbKey(L".avi"), L"ExplorerCommandHandler", registeredClsid(roots_));
}

TEST_F(ExplorerCommandRegistrationTests, RemovalTakesTheSecondCopyOfTheCommandWithIt) {
    // A documented removal that left a copy behind would only hide the entry again the next time
    // anybody registered it, and the user would be right to call that a lie.
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    const std::wstring leftover = L".png\\shell\\CompareStation.Open";
    createScratchKey(leftover);
    writeScratchString(leftover, L"ExplorerCommandHandler", registeredClsid(roots_));

    ASSERT_EQ(removeExplorerCommandRegistration(roots_).state, ExplorerRegistrationState::Removed);
    EXPECT_FALSE(keyExists(roots_, leftover));
    EXPECT_FALSE(keyExists(roots_, L".png\\shell"));
    EXPECT_FALSE(keyExists(roots_, verbKey(L".png")));
}

TEST_F(ExplorerCommandRegistrationTests, UpgradeRemovesSameNamedAndVersionedAssociationVerbs) {
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    const std::wstring clsid = registeredClsid(roots_);
    const std::array<std::wstring, 3U> leftovers{
        L".mp4\\shell\\CompareStation.Compare",
        L"CompareStation.Video.2.0.1\\shell\\CompareStation.Compare",
        L"SystemFileAssociations\\.mp4\\shell\\CompareStation.Compare-2.0.1",
    };
    for (const std::wstring& leftover : leftovers) {
        createScratchKey(leftover);
        writeScratchString(leftover, L"ExplorerCommandHandler", clsid);
        ASSERT_EQ(readString(roots_, leftover, L"ExplorerCommandHandler"), clsid);
    }
    const ExplorerRegistrationTargets upgraded{movedRoot_ / "CompareStation.exe",
                                               movedRoot_ / "CompareStationShell-9.9.dll"};
    ASSERT_EQ(ensureExplorerCommandRegistered(upgraded, roots_).state,
              ExplorerRegistrationState::Registered);
    for (const std::wstring& leftover : leftovers) {
        EXPECT_FALSE(keyExists(roots_, leftover)) << narrow(leftover);
    }
    expectValue(roots_, verbKey(L".mp4"), L"Icon", upgraded.executable.wstring() + L",0");
    expectValue(
        roots_, L"CLSID\\" + clsid + L"\\InprocServer32", nullptr, upgraded.shellLibrary.wstring());

    // Repeated upgrades replace the one registration, not a new entry per ZIP directory.
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    expectValue(roots_, verbKey(L".mp4"), L"Icon", targets_.executable.wstring() + L",0");
}

TEST_F(ExplorerCommandRegistrationTests, UninstallRemovesSameNamedAssociationVerbs) {
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    const std::wstring leftover = L".mp4\\shell\\CompareStation.Compare";
    createScratchKey(leftover);
    writeScratchString(leftover, L"ExplorerCommandHandler", registeredClsid(roots_));
    ASSERT_EQ(removeExplorerCommandRegistration(roots_).state, ExplorerRegistrationState::Removed);
    EXPECT_FALSE(keyExists(roots_, leftover));
}

TEST_F(ExplorerCommandRegistrationTests, RepairsALegacyCommandOnTheCanonicalVerb) {
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    const std::wstring command = verbKey(L".mp4") + L"\\command";
    createScratchKey(command);
    writeScratchString(command, nullptr, LR"("C:\gone\CompareStation.exe" "%1")");
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    EXPECT_FALSE(keyExists(roots_, command));
    EXPECT_TRUE(keyExists(roots_, verbKey(L".mp4")));
}

TEST_F(ExplorerCommandRegistrationTests, MatchesLegacyExecutableNotArgumentsOrSimilarNames) {
    const std::array<std::wstring, 3U> verbs{
        L".mp4\\shell\\OldVCStation",
        L".mp4\\shell\\OtherTool",
        L".mp4\\shell\\SimilarName",
    };
    const std::array<std::wstring, 3U> commands{
        LR"("C:\old\VCStation.exe" "%1")",
        LR"("C:\other\Player.exe" "C:\CompareStation.exe")",
        LR"("C:\other\NotCompareStation.exe" "%1")",
    };
    for (std::size_t index = 0U; index < verbs.size(); ++index) {
        createScratchKey(verbs[index] + L"\\command");
        writeScratchString(verbs[index] + L"\\command", nullptr, commands[index]);
    }
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    EXPECT_FALSE(keyExists(roots_, verbs[0]));
    EXPECT_EQ(readString(roots_, verbs[1] + L"\\command", nullptr), commands[1]);
    EXPECT_EQ(readString(roots_, verbs[2] + L"\\command", nullptr), commands[2]);
}

TEST_F(ExplorerCommandRegistrationTests, SweepsNothingWhileTheUserHasTurnedTheEntryOff) {
    // A user who removed the entry did not ask for their other keys to be rewritten, and a launch
    // that found the marker has to leave the whole per-user surface exactly as it found it.
    ASSERT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::Registered);
    const std::wstring leftover = L".webp\\shell\\CompareStation.Open";
    createScratchKey(leftover);
    writeScratchString(leftover, L"ExplorerCommandHandler", registeredClsid(roots_));
    ASSERT_EQ(setExplorerCommandEnabled(false, roots_).state,
              ExplorerRegistrationState::DisabledByUser);

    EXPECT_EQ(ensureExplorerCommandRegistered(targets_, roots_).state,
              ExplorerRegistrationState::DisabledByUser);
    EXPECT_TRUE(keyExists(roots_, leftover));
    EXPECT_TRUE(keyExists(roots_, verbKey(L".png")));
}

} // namespace
} // namespace dvs::shell
