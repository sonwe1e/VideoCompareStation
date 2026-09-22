#include "ExplorerCommand.h"

#include "ExplorerCommandSupport.h"

#include <filesystem>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <string_view>
#include <vector>

namespace dvs::shell {
namespace {

[[nodiscard]] std::vector<std::filesystem::path> selectedPaths(IShellItemArray* selection) {
    std::vector<std::filesystem::path> paths;
    if (selection == nullptr) {
        return paths;
    }
    DWORD count = 0U;
    if (FAILED(selection->GetCount(&count)) || count < 1U || count > 3U) {
        return paths;
    }
    paths.reserve(count);
    for (DWORD index = 0U; index < count; ++index) {
        IShellItem* item = nullptr;
        if (FAILED(selection->GetItemAt(index, &item)) || item == nullptr) {
            paths.clear();
            return paths;
        }
        PWSTR rawPath = nullptr;
        const HRESULT displayResult = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
        item->Release();
        if (FAILED(displayResult) || rawPath == nullptr) {
            CoTaskMemFree(rawPath);
            paths.clear();
            return paths;
        }
        std::filesystem::path path{rawPath};
        CoTaskMemFree(rawPath);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
            PathIsNetworkPathW(path.c_str()) != FALSE || !hasSupportedVideoExtension(path)) {
            paths.clear();
            return paths;
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

[[nodiscard]] std::filesystem::path executablePath() {
    std::wstring modulePath(32768U, L'\0');
    const DWORD length =
        GetModuleFileNameW(gModule, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (length == 0U || length >= modulePath.size()) {
        return {};
    }
    modulePath.resize(length);
    return std::filesystem::path{modulePath}.parent_path() / L"CompareStation.exe";
}

} // namespace

ExplorerCommand::ExplorerCommand() noexcept {
    gModuleReferences.fetch_add(1U, std::memory_order_relaxed);
}

ExplorerCommand::~ExplorerCommand() {
    gModuleReferences.fetch_sub(1U, std::memory_order_relaxed);
}

HRESULT ExplorerCommand::QueryInterface(REFIID interfaceId, void** const object) noexcept {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (interfaceId == IID_IUnknown || interfaceId == IID_IExplorerCommand) {
        *object = static_cast<IExplorerCommand*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG ExplorerCommand::AddRef() noexcept {
    return references_.fetch_add(1U, std::memory_order_relaxed) + 1U;
}

ULONG ExplorerCommand::Release() noexcept {
    const ULONG remaining = references_.fetch_sub(1U, std::memory_order_acq_rel) - 1U;
    if (remaining == 0U) {
        delete this;
    }
    return remaining;
}

HRESULT ExplorerCommand::GetTitle(IShellItemArray* const selection, LPWSTR* const title) noexcept {
    if (title == nullptr) {
        return E_POINTER;
    }
    const std::size_t count = selectedPaths(selection).size();
    const wchar_t* value = count == 1U   ? L"Open in CompareStation"
                           : count == 3U ? L"Compare 3 videos with CompareStation"
                                         : L"Compare with CompareStation";
    return SHStrDupW(value, title);
}

HRESULT ExplorerCommand::GetIcon(IShellItemArray*, LPWSTR* const icon) noexcept {
    if (icon == nullptr) {
        return E_POINTER;
    }
    try {
        const std::wstring value = executablePath().wstring() + L",0";
        return SHStrDupW(value.c_str(), icon);
    } catch (...) {
        *icon = nullptr;
        return E_OUTOFMEMORY;
    }
}

HRESULT ExplorerCommand::GetToolTip(IShellItemArray* const selection,
                                    LPWSTR* const toolTip) noexcept {
    if (toolTip == nullptr) {
        return E_POINTER;
    }
    const std::size_t count = selectedPaths(selection).size();
    const wchar_t* value = count == 1U   ? L"Open the selected video for visual review"
                           : count == 3U ? L"Compare the three selected videos frame by frame"
                                         : L"Compare the two selected videos frame by frame";
    return SHStrDupW(value, toolTip);
}

HRESULT ExplorerCommand::GetCanonicalName(GUID* const commandName) noexcept {
    if (commandName == nullptr) {
        return E_POINTER;
    }
    *commandName = kExplorerCommandClsid;
    return S_OK;
}

HRESULT ExplorerCommand::GetState(IShellItemArray* const selection,
                                  BOOL,
                                  EXPCMDSTATE* const state) noexcept {
    if (state == nullptr) {
        return E_POINTER;
    }
    try {
        const std::size_t count = selectedPaths(selection).size();
        *state = count >= 1U && count <= 3U ? ECS_ENABLED : ECS_HIDDEN;
    } catch (...) {
        *state = ECS_HIDDEN;
    }
    return S_OK;
}

HRESULT ExplorerCommand::Invoke(IShellItemArray* const selection, IBindCtx*) noexcept {
    try {
        const std::vector<std::filesystem::path> paths = selectedPaths(selection);
        if (paths.empty() || paths.size() > 3U) {
            return E_INVALIDARG;
        }
        const std::filesystem::path executable = executablePath();
        if (executable.empty()) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        std::wstring commandLine = buildReviewCommandLine(executable, paths);
        STARTUPINFOW startupInfo{
            .cb = sizeof(STARTUPINFOW),
        };
        PROCESS_INFORMATION processInformation{};
        if (CreateProcessW(executable.c_str(),
                           commandLine.data(),
                           nullptr,
                           nullptr,
                           FALSE,
                           0U,
                           nullptr,
                           executable.parent_path().c_str(),
                           &startupInfo,
                           &processInformation) == FALSE) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        CloseHandle(processInformation.hThread);
        CloseHandle(processInformation.hProcess);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HRESULT ExplorerCommand::GetFlags(EXPCMDFLAGS* const flags) noexcept {
    if (flags == nullptr) {
        return E_POINTER;
    }
    *flags = ECF_DEFAULT;
    return S_OK;
}

HRESULT ExplorerCommand::EnumSubCommands(IEnumExplorerCommand** const commands) noexcept {
    if (commands == nullptr) {
        return E_POINTER;
    }
    *commands = nullptr;
    return E_NOTIMPL;
}

} // namespace dvs::shell
