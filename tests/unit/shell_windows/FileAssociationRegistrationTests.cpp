#include "dvs/shell/FileAssociationRegistration.h"

#include "ExplorerCommandSupport.h"
#include "FileAssociationTestHooks.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace dvs::shell {
namespace {

// Every key this suite writes lives under one scratch subtree per process, so a test run can
// never disturb the user's own registration - the same isolation ExplorerCommandRegistrationTests
// uses. The scratch roots also cover the UserChoice tree, which these tests only ever write to
// simulate the system's own record.
[[nodiscard]] std::wstring scratchSubtree() {
    return L"Software\\CompareStationTests\\" + std::to_wstring(GetCurrentProcessId());
}

[[nodiscard]] FileAssociationRoots scratchRoots() {
    FileAssociationRoots roots;
    const std::wstring scratch = scratchSubtree();
    roots.classes = scratch + L"\\Classes";
    roots.fileExts = scratch + L"\\FileExts";
    roots.registeredApplications = scratch + L"\\RegisteredApplications";
    roots.capabilities = scratch + L"\\Capabilities";
    return roots;
}

void removeScratchSubtree() {
    static_cast<void>(RegDeleteTreeW(HKEY_CURRENT_USER, scratchSubtree().c_str()));
}

// GoogleTest messages are narrow; a wide string only has to be readable in a failure report.
[[nodiscard]] std::string narrow(const std::wstring_view text) {
    std::string narrowed;
    narrowed.reserve(text.size());
    for (const wchar_t character : text) {
        narrowed.push_back(static_cast<char>(character));
    }
    return narrowed;
}

[[nodiscard]] std::optional<std::wstring> readStringAt(const std::wstring& path,
                                                       const wchar_t* const name) {
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

[[nodiscard]] bool keyExistsAt(const std::wstring& path) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0U, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(key);
    return true;
}

void expectStringAt(const std::wstring& path,
                    const wchar_t* const name,
                    const std::wstring& expected) {
    const std::optional<std::wstring> actual = readStringAt(path, name);
    ASSERT_TRUE(actual.has_value())
        << narrow(path) << " has no string value '" << narrow(name ? name : L"(default)") << "'";
    EXPECT_EQ(*actual, expected) << narrow(path) << " value '" << narrow(name ? name : L"(default)")
                                 << "'";
}

// Creates a key and writes one string value the way the system itself would, for the records a
// user, another application, or Windows leaves behind.
void writeScratchString(const std::wstring& path,
                        const wchar_t* const name,
                        const std::wstring& value) {
    HKEY key = nullptr;
    ASSERT_EQ(RegCreateKeyExW(HKEY_CURRENT_USER,
                              path.c_str(),
                              0U,
                              nullptr,
                              REG_OPTION_NON_VOLATILE,
                              KEY_SET_VALUE,
                              nullptr,
                              &key,
                              nullptr),
              ERROR_SUCCESS)
        << narrow(path);
    const DWORD bytes = static_cast<DWORD>((value.size() + 1U) * sizeof(wchar_t));
    ASSERT_EQ(
        RegSetValueExW(key, name, 0U, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes),
        ERROR_SUCCESS);
    RegCloseKey(key);
}

[[nodiscard]] std::size_t valueCountAt(const std::wstring& path) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0U, KEY_READ, &key) != ERROR_SUCCESS) {
        return 0U;
    }
    DWORD values = 0U;
    RegQueryInfoKeyW(key,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     &values,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr);
    RegCloseKey(key);
    return values;
}

class FileAssociationRegistrationTests : public ::testing::Test {
protected:
    void SetUp() override {
        removeScratchSubtree();
        roots_ = scratchRoots();
        // A directory name with spaces and non-ASCII characters is the shape the quoted command
        // has to survive, so the default fixture uses one instead of an ASCII-only path.
        const std::filesystem::path scratch = std::filesystem::temp_directory_path() /
                                              "CompareStationFileAssociationTests" /
                                              std::to_wstring(GetCurrentProcessId());
        installRoot_ = scratch / L"安装 目录 One";
        movedRoot_ = scratch / L"安装 目录 Two";
        std::error_code ignored;
        std::filesystem::remove_all(scratch, ignored);
        for (const std::filesystem::path& root : {installRoot_, movedRoot_}) {
            std::filesystem::create_directories(root, ignored);
            std::ofstream{root / "CompareStation.exe"};
        }
        targets_.executable = installRoot_ / "CompareStation.exe";
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(installRoot_.parent_path(), ignored);
        static_cast<void>(
            std::filesystem::remove(installRoot_.parent_path().parent_path(), ignored));
        removeScratchSubtree();
        static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\CompareStationTests"));
    }

    [[nodiscard]] std::wstring openCommandFor(const std::filesystem::path& executable) const {
        return L"\"" + executable.wstring() + L"\" \"%1\"";
    }

    FileAssociationRoots roots_;
    FileAssociationTargets targets_;
    std::filesystem::path installRoot_;
    std::filesystem::path movedRoot_;
};

TEST_F(FileAssociationRegistrationTests, WritesTheDocumentedPerUserShape) {
    const FileAssociationResult result = registerFileAssociation(targets_, roots_);
    ASSERT_EQ(result.state, FileAssociationState::Ok) << result.error;

    const std::wstring exe = targets_.executable.wstring();
    const std::wstring command = openCommandFor(targets_.executable);

    // The two ProgIDs, with video and image registered separately.
    for (const auto& [progId, title] : std::vector<std::pair<std::wstring, std::wstring>>{
             {roots_.classes + L"\\CompareStation.Video", L"CompareStation Video"},
             {roots_.classes + L"\\CompareStation.Image", L"CompareStation Image"},
         }) {
        expectStringAt(progId, nullptr, title.c_str());
        expectStringAt(progId + L"\\DefaultIcon", nullptr, exe + L",0");
        expectStringAt(progId + L"\\shell\\open\\command", nullptr, command);
    }

    // The Applications entry: one SupportedTypes value per extension and the same open command.
    const std::wstring applications = roots_.classes + L"\\Applications\\CompareStation.exe";
    expectStringAt(applications + L"\\shell\\open\\command", nullptr, command);
    expectStringAt(applications + L"\\DefaultIcon", nullptr, exe + L",0");
    const std::wstring supportedTypes = applications + L"\\SupportedTypes";
    for (const std::wstring_view extension : supportedVideoExtensions()) {
        expectStringAt(supportedTypes, extension.data(), L"");
        expectStringAt(roots_.classes + L"\\" + std::wstring{extension} + L"\\OpenWithProgids",
                       L"CompareStation.Video",
                       L"");
    }
    for (const std::wstring_view extension : supportedImageExtensions()) {
        expectStringAt(supportedTypes, extension.data(), L"");
        expectStringAt(roots_.classes + L"\\" + std::wstring{extension} + L"\\OpenWithProgids",
                       L"CompareStation.Image",
                       L"");
    }

    // The Default Apps registration: Capabilities with one FileAssociations value per extension,
    // and RegisteredApplications pointing the system at it.
    expectStringAt(roots_.capabilities, L"ApplicationName", L"CompareStation");
    const std::optional<std::wstring> description =
        readStringAt(roots_.capabilities, L"ApplicationDescription");
    ASSERT_TRUE(description.has_value());
    EXPECT_NE(description->find(L"silent"), std::wstring::npos)
        << "the registered description must say that playback is silent";
    for (const std::wstring_view extension : supportedVideoExtensions()) {
        expectStringAt(
            roots_.capabilities + L"\\FileAssociations", extension.data(), L"CompareStation.Video");
    }
    for (const std::wstring_view extension : supportedImageExtensions()) {
        expectStringAt(
            roots_.capabilities + L"\\FileAssociations", extension.data(), L"CompareStation.Image");
    }
    expectStringAt(roots_.registeredApplications, L"CompareStation", roots_.capabilities);
}

TEST_F(FileAssociationRegistrationTests, RegistersExactlyTheValidatedExtensionSets) {
    ASSERT_EQ(registerFileAssociation(targets_, roots_).state, FileAssociationState::Ok);

    EXPECT_EQ(supportedVideoExtensions().size(), 5U);
    EXPECT_EQ(supportedImageExtensions().size(), 13U);
    // SupportedTypes holds exactly the union: one value per extension, no audio formats.
    EXPECT_EQ(valueCountAt(roots_.classes + L"\\Applications\\CompareStation.exe\\SupportedTypes"),
              18U);
    EXPECT_EQ(valueCountAt(roots_.capabilities + L"\\FileAssociations"), 18U);
    // No audio extension is ever registered, not even as a stray value.
    for (const std::wstring_view audio : {L".mp3", L".wav", L".flac", L".aac", L".m4a"}) {
        const std::wstring key = roots_.classes + L"\\" + std::wstring{audio};
        EXPECT_FALSE(keyExistsAt(key)) << narrow(audio);
    }
}

TEST_F(FileAssociationRegistrationTests, QuotesChineseAndSpacePathsExactly) {
    // The fixture path already contains spaces and Chinese characters; this test pins the exact
    // registry content so a later refactor cannot quietly drop the quotes or re-encode the path.
    ASSERT_TRUE(targets_.executable.wstring().find(L"安装") != std::wstring::npos);
    ASSERT_EQ(registerFileAssociation(targets_, roots_).state, FileAssociationState::Ok);

    const std::wstring expected = L"\"" + targets_.executable.wstring() + L"\" \"%1\"";
    expectStringAt(
        roots_.classes + L"\\CompareStation.Video\\shell\\open\\command", nullptr, expected);
    expectStringAt(
        roots_.classes + L"\\CompareStation.Image\\shell\\open\\command", nullptr, expected);
    expectStringAt(roots_.classes + L"\\Applications\\CompareStation.exe\\shell\\open\\command",
                   nullptr,
                   expected);
    expectStringAt(roots_.classes + L"\\CompareStation.Video\\DefaultIcon",
                   nullptr,
                   targets_.executable.wstring() + L",0");
}

TEST_F(FileAssociationRegistrationTests, NeverWritesUserChoice) {
    // A user default that already points at another application must survive registration
    // untouched: taking over a format silently is the one thing this registration must not do.
    const std::wstring userChoice = roots_.fileExts + L"\\.mp4\\UserChoice";
    writeScratchString(userChoice, L"ProgID", L"OtherPlayer.mp4");

    ASSERT_EQ(registerFileAssociation(targets_, roots_).state, FileAssociationState::Ok);
    expectStringAt(userChoice, L"ProgID", L"OtherPlayer.mp4");
}

TEST_F(FileAssociationRegistrationTests, RefusesAMissingExecutableWithoutWriting) {
    FileAssociationTargets missing;
    missing.executable = installRoot_ / L"NotFound.exe";
    const FileAssociationResult result = registerFileAssociation(missing, roots_);
    EXPECT_EQ(result.state, FileAssociationState::Failed);
    EXPECT_FALSE(result.error.empty());
    EXPECT_NE(result.error.find("NotFound.exe"), std::string::npos);
    EXPECT_NE(result.error.find("安装 目录 One"), std::string::npos);
    EXPECT_FALSE(keyExistsAt(roots_.classes));
    EXPECT_FALSE(keyExistsAt(roots_.capabilities));
    EXPECT_FALSE(keyExistsAt(roots_.registeredApplications));
}

TEST_F(FileAssociationRegistrationTests, RepairsEveryPathBearingKeyAfterAMove) {
    ASSERT_EQ(registerFileAssociation(targets_, roots_).state, FileAssociationState::Ok);

    FileAssociationTargets moved;
    moved.executable = movedRoot_ / "CompareStation.exe";
    const FileAssociationResult repair = repairFileAssociationPaths(moved, roots_);
    ASSERT_EQ(repair.state, FileAssociationState::Ok) << repair.error;

    const std::wstring command = openCommandFor(moved.executable);
    expectStringAt(
        roots_.classes + L"\\CompareStation.Video\\shell\\open\\command", nullptr, command);
    expectStringAt(
        roots_.classes + L"\\CompareStation.Image\\shell\\open\\command", nullptr, command);
    expectStringAt(roots_.classes + L"\\CompareStation.Video\\DefaultIcon",
                   nullptr,
                   moved.executable.wstring() + L",0");
    expectStringAt(roots_.classes + L"\\CompareStation.Image\\DefaultIcon",
                   nullptr,
                   moved.executable.wstring() + L",0");
    expectStringAt(roots_.classes + L"\\Applications\\CompareStation.exe\\shell\\open\\command",
                   nullptr,
                   command);
    expectStringAt(roots_.classes + L"\\Applications\\CompareStation.exe\\DefaultIcon",
                   nullptr,
                   moved.executable.wstring() + L",0");
    expectStringAt(roots_.registeredApplications, L"CompareStation", roots_.capabilities);
    // The path-independent parts of the registration are not rewritten out of shape.
    expectStringAt(roots_.classes + L"\\CompareStation.Video", nullptr, L"CompareStation Video");
    expectStringAt(roots_.capabilities + L"\\FileAssociations", L".mp4", L"CompareStation.Video");

    const FileAssociationStatus status = readFileAssociationStatus(moved, roots_);
    EXPECT_EQ(status.health, AssociationHealth::Current);
}

TEST_F(FileAssociationRegistrationTests, RepairNeedsAnExistingRegistration) {
    const FileAssociationResult repair = repairFileAssociationPaths(targets_, roots_);
    EXPECT_EQ(repair.state, FileAssociationState::Failed);
    EXPECT_FALSE(repair.error.empty());
    EXPECT_FALSE(keyExistsAt(roots_.classes));
}

TEST_F(FileAssociationRegistrationTests, AMidWriteFailureRollsBackToThePreviousRegistration) {
    ASSERT_EQ(registerFileAssociation(targets_, roots_).state, FileAssociationState::Ok);

    // Fail a late value write (the ProgID command is one of the first values, the
    // RegisteredApplications pointer the last): the transaction has already rewritten several
    // values when it fails, so only a real rollback can return to the previous registration.
    FileAssociationTargets moved;
    moved.executable = movedRoot_ / "CompareStation.exe";
    {
        const testing::ScopedFileAssociationWriteFailure failLate{12U};
        const FileAssociationResult result = registerFileAssociation(moved, roots_);
        EXPECT_EQ(result.state, FileAssociationState::Failed);
    }

    // The previous registration is intact, value for value.
    const FileAssociationStatus status = readFileAssociationStatus(targets_, roots_);
    EXPECT_EQ(status.health, AssociationHealth::Current);
    expectStringAt(roots_.classes + L"\\CompareStation.Video\\shell\\open\\command",
                   nullptr,
                   openCommandFor(targets_.executable));
    expectStringAt(roots_.registeredApplications, L"CompareStation", roots_.capabilities);
}

TEST_F(FileAssociationRegistrationTests, AMidWriteFailureOnAFreshRegistrationLeavesNoResidue) {
    {
        const testing::ScopedFileAssociationWriteFailure failEarly{3U};
        const FileAssociationResult result = registerFileAssociation(targets_, roots_);
        EXPECT_EQ(result.state, FileAssociationState::Failed);
    }
    // Not a half registration: no ProgIDs, no Applications entry, no per-extension values, no
    // Capabilities and no RegisteredApplications pointer. The keys this operation created on the
    // way to the failing write are gone again.
    EXPECT_FALSE(keyExistsAt(roots_.classes + L"\\CompareStation.Video"));
    EXPECT_FALSE(keyExistsAt(roots_.classes + L"\\CompareStation.Image"));
    EXPECT_FALSE(keyExistsAt(roots_.classes + L"\\Applications\\CompareStation.exe"));
    EXPECT_FALSE(keyExistsAt(roots_.capabilities));
    EXPECT_FALSE(keyExistsAt(roots_.registeredApplications));
    for (const std::wstring_view extension : supportedVideoExtensions()) {
        EXPECT_FALSE(keyExistsAt(roots_.classes + L"\\" + std::wstring{extension}))
            << narrow(extension);
    }
    EXPECT_EQ(readFileAssociationStatus(targets_, roots_).health, AssociationHealth::NotRegistered);
}

TEST_F(FileAssociationRegistrationTests, RemovalDeletesOnlyWhatThisApplicationOwns) {
    ASSERT_EQ(registerFileAssociation(targets_, roots_).state, FileAssociationState::Ok);

    // Records another tool or Windows itself would own: a second registered application, a
    // second ProgID in the shared OpenWithProgids, a foreign value on the extension key itself,
    // and the user's own defaults.
    writeScratchString(
        roots_.registeredApplications, L"Other Application", L"Software\\Other\\Capabilities");
    writeScratchString(roots_.classes + L"\\.mp4\\OpenWithProgids", L"Other.Video", L"");
    writeScratchString(roots_.classes + L"\\.png", L"Content Type", L"image/png");
    writeScratchString(roots_.fileExts + L"\\.mp4\\UserChoice", L"ProgID", L"CompareStation.Video");
    writeScratchString(roots_.fileExts + L"\\.png\\UserChoice", L"ProgID", L"Other.Image");

    const FileAssociationResult removed = removeFileAssociation(roots_);
    ASSERT_EQ(removed.state, FileAssociationState::Ok) << removed.error;

    // This application's keys are gone.
    EXPECT_FALSE(keyExistsAt(roots_.classes + L"\\CompareStation.Video"));
    EXPECT_FALSE(keyExistsAt(roots_.classes + L"\\CompareStation.Image"));
    EXPECT_FALSE(keyExistsAt(roots_.classes + L"\\Applications\\CompareStation.exe"));
    EXPECT_FALSE(keyExistsAt(roots_.capabilities));
    // The shared RegisteredApplications key survives with the other application's value.
    expectStringAt(
        roots_.registeredApplications, L"Other Application", L"Software\\Other\\Capabilities");
    EXPECT_FALSE(readStringAt(roots_.registeredApplications, L"CompareStation").has_value());
    // Foreign neighbours keep their values, and with them their shared keys.
    expectStringAt(roots_.classes + L"\\.mp4\\OpenWithProgids", L"Other.Video", L"");
    expectStringAt(roots_.classes + L"\\.png", L"Content Type", L"image/png");
    // The user's UserChoice is never touched, not even when it points at this application.
    expectStringAt(roots_.fileExts + L"\\.mp4\\UserChoice", L"ProgID", L"CompareStation.Video");
    expectStringAt(roots_.fileExts + L"\\.png\\UserChoice", L"ProgID", L"Other.Image");
    // An extension key this registration alone used is pruned.
    EXPECT_FALSE(keyExistsAt(roots_.classes + L"\\.bmp"));
    EXPECT_EQ(readFileAssociationStatus(targets_, roots_).health, AssociationHealth::NotRegistered);
}

TEST_F(FileAssociationRegistrationTests, RemovalIsIdempotent) {
    ASSERT_EQ(removeFileAssociation(roots_).state, FileAssociationState::Ok);
    EXPECT_EQ(removeFileAssociation(roots_).state, FileAssociationState::Ok);
}

TEST_F(FileAssociationRegistrationTests, StatusReportsTheThreeOwnershipStates) {
    FileAssociationStatus status = readFileAssociationStatus(targets_, roots_);
    EXPECT_EQ(status.health, AssociationHealth::NotRegistered);
    EXPECT_EQ(status.videoDefault, DefaultOwner::NotSet);
    EXPECT_EQ(status.imageDefault, DefaultOwner::NotSet);

    // Every video extension granted to this application: the video kind is ours.
    for (const std::wstring_view extension : supportedVideoExtensions()) {
        writeScratchString(roots_.fileExts + L"\\" + std::wstring{extension} + L"\\UserChoice",
                           L"ProgID",
                           L"CompareStation.Video");
    }
    status = readFileAssociationStatus(targets_, roots_);
    EXPECT_EQ(status.videoDefault, DefaultOwner::ThisApplication);
    EXPECT_EQ(status.imageDefault, DefaultOwner::NotSet);

    // Another application on one image extension: the image kind is not ours.
    writeScratchString(roots_.fileExts + L"\\.png\\UserChoice", L"ProgID", L"Other.Image");
    status = readFileAssociationStatus(targets_, roots_);
    EXPECT_EQ(status.imageDefault, DefaultOwner::OtherApplication);

    // Health follows the registration, independent of who owns the defaults.
    ASSERT_EQ(registerFileAssociation(targets_, roots_).state, FileAssociationState::Ok);
    status = readFileAssociationStatus(targets_, roots_);
    EXPECT_EQ(status.health, AssociationHealth::Current);
    EXPECT_EQ(status.videoDefault, DefaultOwner::ThisApplication);
    EXPECT_EQ(status.imageDefault, DefaultOwner::OtherApplication);

    // A tampered path makes the registration stale rather than current.
    writeScratchString(roots_.classes + L"\\CompareStation.Video\\shell\\open\\command",
                       nullptr,
                       L"\"C:\\gone\\CompareStation.exe\" \"%1\"");
    status = readFileAssociationStatus(targets_, roots_);
    EXPECT_EQ(status.health, AssociationHealth::StalePath);
}

TEST_F(FileAssociationRegistrationTests, DeepLinkSupportFollowsTheWindowsBuild) {
    EXPECT_FALSE(supportsRegisteredAppUserParameter(19045U));
    EXPECT_FALSE(supportsRegisteredAppUserParameter(0U));
    EXPECT_TRUE(supportsRegisteredAppUserParameter(22000U));
    EXPECT_TRUE(supportsRegisteredAppUserParameter(22631U));
}

TEST_F(FileAssociationRegistrationTests, BuildsTheSettingsUriWithEscaping) {
    EXPECT_EQ(defaultAppsSettingsUri(L"CompareStation", false), L"ms-settings:defaultapps");
    EXPECT_EQ(defaultAppsSettingsUri(L"CompareStation", true),
              L"ms-settings:defaultapps?registeredAppUser=CompareStation");
    EXPECT_EQ(defaultAppsSettingsUri(L"Compare Station", true),
              L"ms-settings:defaultapps?registeredAppUser=Compare%20Station");
}

TEST_F(FileAssociationRegistrationTests, OpensTheApplicationPageAndFallsBackToTheGenericPage) {
    std::vector<std::wstring> launched;
    DefaultAppsLaunchOverrides overrides;
    overrides.buildNumber = 22631U;
    overrides.launch = [&launched](const std::wstring& uri) {
        launched.push_back(uri);
        return true;
    };
    ASSERT_EQ(openDefaultAppsSettings(roots_, overrides).state, FileAssociationState::Ok);
    ASSERT_EQ(launched.size(), 1U);
    EXPECT_EQ(launched.front(), L"ms-settings:defaultapps?registeredAppUser=CompareStation");

    // The parameterized page is refused (an unpatched Windows 11 or a policy): the generic page
    // is opened next and that is still a success for the jump itself.
    launched.clear();
    overrides.launch = [&launched](const std::wstring& uri) {
        launched.push_back(uri);
        return uri == L"ms-settings:defaultapps";
    };
    ASSERT_EQ(openDefaultAppsSettings(roots_, overrides).state, FileAssociationState::Ok);
    ASSERT_EQ(launched.size(), 2U);
    EXPECT_EQ(launched.front(), L"ms-settings:defaultapps?registeredAppUser=CompareStation");
    EXPECT_EQ(launched.back(), L"ms-settings:defaultapps");

    // Windows 10 never receives the parameter: exactly one launch, the generic page.
    launched.clear();
    overrides.buildNumber = 19045U;
    overrides.launch = [&launched](const std::wstring& uri) {
        launched.push_back(uri);
        return true;
    };
    ASSERT_EQ(openDefaultAppsSettings(roots_, overrides).state, FileAssociationState::Ok);
    ASSERT_EQ(launched.size(), 1U);
    EXPECT_EQ(launched.front(), L"ms-settings:defaultapps");

    // Nothing can be opened at all: an honest failure, because "the settings page opened" is
    // never a stand-in for success.
    launched.clear();
    overrides.buildNumber = 22631U;
    overrides.launch = [&launched](const std::wstring& uri) {
        launched.push_back(uri);
        return false;
    };
    EXPECT_EQ(openDefaultAppsSettings(roots_, overrides).state, FileAssociationState::Failed);
    EXPECT_EQ(launched.size(), 2U);
}

} // namespace
} // namespace dvs::shell
