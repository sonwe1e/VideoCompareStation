#include "ExplorerCommandSupport.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <string_view>

namespace dvs::shell {
namespace {

constexpr std::array<std::wstring_view, 5U> kSupportedExtensions{
    L".mp4",
    L".mkv",
    L".mov",
    L".avi",
    L".m4v",
};

[[nodiscard]] std::wstring quoteWindowsArgument(const std::wstring_view argument) {
    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t slashes = 0U;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(slashes * 2U + 1U, L'\\');
            quoted.push_back(L'"');
            slashes = 0U;
            continue;
        }
        quoted.append(slashes, L'\\');
        slashes = 0U;
        quoted.push_back(character);
    }
    quoted.append(slashes * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

} // namespace

bool hasSupportedVideoExtension(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    for (wchar_t& character : extension) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return std::ranges::find(kSupportedExtensions, extension) != kSupportedExtensions.end();
}

std::wstring buildReviewCommandLine(const std::filesystem::path& executable,
                                    const std::span<const std::filesystem::path> selectedPaths) {
    if (executable.empty() || selectedPaths.empty() || selectedPaths.size() > 3U) {
        return {};
    }
    std::wstring commandLine = quoteWindowsArgument(executable.wstring());
    for (const std::filesystem::path& path : selectedPaths) {
        commandLine.push_back(L' ');
        commandLine.append(quoteWindowsArgument(path.wstring()));
    }
    return commandLine;
}

} // namespace dvs::shell
