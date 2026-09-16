#include "dvs/test/ScopedTemporaryDirectory.h"

#include "ExplorerCommand.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <shlwapi.h>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>

namespace dvs::shell {
namespace {

class TestShellItem final : public IShellItem {
public:
    explicit TestShellItem(std::filesystem::path path) : path_(std::move(path)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interfaceId == IID_IUnknown || interfaceId == IID_IShellItem) {
            *object = static_cast<IShellItem*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return references_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = references_.fetch_sub(1U, std::memory_order_acq_rel) - 1U;
        if (remaining == 0U) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx*, REFGUID, REFIID, void**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetParent(IShellItem**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetDisplayName(SIGDN, LPWSTR* name) override {
        return name == nullptr ? E_POINTER : SHStrDupW(path_.c_str(), name);
    }

    HRESULT STDMETHODCALLTYPE GetAttributes(SFGAOF, SFGAOF*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Compare(IShellItem*, SICHINTF, int*) override {
        return E_NOTIMPL;
    }

private:
    ~TestShellItem() = default;

    std::atomic<ULONG> references_{1U};
    std::filesystem::path path_;
};

class TestShellItemArray final : public IShellItemArray {
public:
    explicit TestShellItemArray(std::vector<std::filesystem::path> paths) {
        items_.reserve(paths.size());
        for (std::filesystem::path& path : paths) {
            items_.push_back(new TestShellItem{std::move(path)});
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interfaceId == IID_IUnknown || interfaceId == IID_IShellItemArray) {
            *object = static_cast<IShellItemArray*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return references_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = references_.fetch_sub(1U, std::memory_order_acq_rel) - 1U;
        if (remaining == 0U) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx*, REFGUID, REFIID, void**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyStore(GETPROPERTYSTOREFLAGS, REFIID, void**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyDescriptionList(REFPROPERTYKEY, REFIID, void**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetAttributes(SIATTRIBFLAGS, SFGAOF, SFGAOF*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetCount(DWORD* count) override {
        if (count == nullptr) {
            return E_POINTER;
        }
        *count = static_cast<DWORD>(items_.size());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetItemAt(DWORD index, IShellItem** item) override {
        if (item == nullptr) {
            return E_POINTER;
        }
        *item = nullptr;
        if (index >= items_.size()) {
            return E_BOUNDS;
        }
        items_[index]->AddRef();
        *item = items_[index];
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumItems(IEnumShellItems**) override {
        return E_NOTIMPL;
    }

private:
    ~TestShellItemArray() {
        for (IShellItem* item : items_) {
            item->Release();
        }
    }

    std::atomic<ULONG> references_{1U};
    std::vector<IShellItem*> items_;
};

class LoadedExplorerCommand final {
public:
    explicit LoadedExplorerCommand(const std::filesystem::path& dllPath) {
        module_ = LoadLibraryW(dllPath.c_str());
        if (module_ == nullptr) {
            return;
        }
        using DllGetClassObjectFunction = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
        const auto getClassObject = reinterpret_cast<DllGetClassObjectFunction>(
            GetProcAddress(module_, "DllGetClassObject"));
        if (getClassObject == nullptr ||
            FAILED(getClassObject(
                kExplorerCommandClsid, IID_IClassFactory, reinterpret_cast<void**>(&factory_)))) {
            return;
        }
        static_cast<void>(factory_->CreateInstance(
            nullptr, IID_IExplorerCommand, reinterpret_cast<void**>(&command_)));
    }

    ~LoadedExplorerCommand() {
        if (command_ != nullptr) {
            command_->Release();
        }
        if (factory_ != nullptr) {
            factory_->Release();
        }
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }

    [[nodiscard]] IExplorerCommand* command() const noexcept {
        return command_;
    }

private:
    HMODULE module_ = nullptr;
    IClassFactory* factory_ = nullptr;
    IExplorerCommand* command_ = nullptr;
};

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(const wchar_t* name, const wchar_t* value) : name_(name) {
        SetLastError(ERROR_SUCCESS);
        const DWORD required = GetEnvironmentVariableW(name_.c_str(), nullptr, 0U);
        hadValue_ = required != 0U || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        if (required > 0U) {
            previousValue_.resize(required);
            const DWORD copied = GetEnvironmentVariableW(
                name_.c_str(), previousValue_.data(), static_cast<DWORD>(previousValue_.size()));
            previousValue_.resize(copied);
        }
        installed_ = SetEnvironmentVariableW(name_.c_str(), value) != FALSE;
    }

    ~ScopedEnvironmentVariable() {
        if (installed_) {
            static_cast<void>(SetEnvironmentVariableW(
                name_.c_str(), hadValue_ ? previousValue_.c_str() : nullptr));
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

    [[nodiscard]] bool installed() const noexcept {
        return installed_;
    }

private:
    std::wstring name_;
    std::wstring previousValue_;
    bool hadValue_ = false;
    bool installed_ = false;
};

[[nodiscard]] std::filesystem::path environmentPath(const wchar_t* name) {
    std::wstring value(32768U, L'\0');
    const DWORD length =
        GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (length == 0U || length >= value.size()) {
        return {};
    }
    value.resize(length);
    return value;
}

[[nodiscard]] std::vector<std::wstring> readCapturedArguments(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    std::uint32_t count = 0U;
    stream.read(reinterpret_cast<char*>(&count), sizeof(count));
    std::vector<std::wstring> arguments;
    arguments.reserve(count);
    for (std::uint32_t index = 0U; index < count && stream; ++index) {
        std::uint32_t length = 0U;
        stream.read(reinterpret_cast<char*>(&length), sizeof(length));
        std::wstring argument(length, L'\0');
        stream.read(reinterpret_cast<char*>(argument.data()),
                    static_cast<std::streamsize>(length * sizeof(wchar_t)));
        arguments.push_back(std::move(argument));
    }
    return stream ? arguments : std::vector<std::wstring>{};
}

void writeFixture(const std::filesystem::path& path) {
    std::ofstream stream{path, std::ios::binary};
    stream << "fixture";
}

TEST(ExplorerCommandSmokeTests, LoadsRealComServerAndFiltersSelectionCardinality) {
    const dvs::test::ScopedTemporaryDirectory temporaryDirectory{"dvs-shell-smoke"};
    const std::filesystem::path& directory = temporaryDirectory.path();
    const std::filesystem::path first = directory / L"甲 视频.mp4";
    const std::filesystem::path second = directory / L"乙 视频.MKV";
    const std::filesystem::path unsupported = directory / L"notes.txt";
    writeFixture(first);
    writeFixture(second);
    writeFixture(unsupported);

    LoadedExplorerCommand loaded{environmentPath(L"DVS_SHELL_TEST_DLL")};
    ASSERT_NE(loaded.command(), nullptr);

    const auto stateFor = [&loaded](std::vector<std::filesystem::path> paths) {
        auto* selection = new TestShellItemArray{std::move(paths)};
        EXPCMDSTATE state = ECS_DISABLED;
        const HRESULT result = loaded.command()->GetState(selection, FALSE, &state);
        selection->Release();
        EXPECT_EQ(result, S_OK);
        return state;
    };
    EXPECT_EQ(stateFor({first}), ECS_ENABLED);
    EXPECT_EQ(stateFor({first, second}), ECS_ENABLED);
    EXPECT_EQ(stateFor({first, second, first}), ECS_ENABLED);
    EXPECT_EQ(stateFor({first, unsupported}), ECS_HIDDEN);
}

TEST(ExplorerCommandSmokeTests, InvokesRealComServerWithUnicodeCompareArguments) {
    const dvs::test::ScopedTemporaryDirectory temporaryDirectory{"dvs-shell-smoke"};
    const std::filesystem::path& directory = temporaryDirectory.path();
    const std::filesystem::path first = directory / L"甲 视频.mp4";
    const std::filesystem::path second = directory / L"乙 视频.MKV";
    const std::filesystem::path captured = directory / L"captured-arguments.bin";
    const std::filesystem::path shellDll = environmentPath(L"DVS_SHELL_TEST_DLL");
    const std::filesystem::path copiedDll = directory / shellDll.filename();
    const std::filesystem::path copiedProbe = directory / L"VCStation.exe";
    writeFixture(first);
    writeFixture(second);
    ASSERT_TRUE(CopyFileW(shellDll.c_str(), copiedDll.c_str(), FALSE));
    ASSERT_TRUE(CopyFileW(
        environmentPath(L"DVS_SHELL_TEST_PROBE_EXE").c_str(), copiedProbe.c_str(), FALSE));
    const ScopedEnvironmentVariable captureEnvironment{L"DVS_SHELL_TEST_CAPTURE_FILE",
                                                       captured.c_str()};
    ASSERT_TRUE(captureEnvironment.installed());

    {
        LoadedExplorerCommand loaded{copiedDll};
        ASSERT_NE(loaded.command(), nullptr);
        auto* selection = new TestShellItemArray{{first, second}};
        EXPECT_EQ(loaded.command()->Invoke(selection, nullptr), S_OK);
        selection->Release();

        for (int attempt = 0; attempt < 250 && !std::filesystem::exists(captured); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        const std::vector<std::wstring> arguments = readCapturedArguments(captured);
        ASSERT_EQ(arguments.size(), 3U);
        EXPECT_EQ(std::filesystem::path{arguments[0]}.filename(), L"VCStation.exe");
        EXPECT_EQ(arguments[1], first.wstring());
        EXPECT_EQ(arguments[2], second.wstring());
    }
}

} // namespace
} // namespace dvs::shell
