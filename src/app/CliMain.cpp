#include "dvs/domain/ComparisonSource.h"
#include "dvs/media/MediaProbe.h"
#include "dvs/media/Module.h"
#include "dvs/media/MultiSourceFrameProvider.h"
#include "dvs/persistence/Module.h"
#include "dvs/platform/FrameBudget.h"
#include "dvs/platform/Module.h"
#include "dvs/ui/DirectCompare.h"
#include "dvs/ui/Module.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

namespace {

void printError(const dvs::domain::MediaError& error) {
    std::cerr << "error=" << dvs::domain::stableId(error.code)
              << " operation=" << dvs::domain::stableId(error.operation)
              << " detail=" << error.technicalDetail << '\n';
}

[[nodiscard]] int runProbe(const std::filesystem::path& sourcePath) {
    const auto descriptor = dvs::media::MediaProbe::inspect(sourcePath, dvs::domain::SourceId{0});
    if (!descriptor) {
        printError(descriptor.error());
        return EXIT_FAILURE;
    }

    const dvs::domain::MediaDescriptor& value = descriptor.value();
    std::cout << "path=" << value.normalizedPath.string() << '\n'
              << "codec=" << value.codecId << " pixel-format=" << value.pixelFormatId << '\n'
              << "extent=" << value.extent.width << 'x' << value.extent.height << '\n'
              << "rate=";
    if (value.frameRate.has_value()) {
        const dvs::domain::RationalRate& rate = *value.frameRate;
        std::cout << rate.numerator() << '/' << rate.denominator();
    } else {
        std::cout << "vfr";
    }
    std::cout << '\n' << "frames=" << value.frameCount.value << '\n';
    return EXIT_SUCCESS;
}

[[nodiscard]] int runCompare(const std::filesystem::path& sourceAPath,
                             const std::filesystem::path& sourceBPath,
                             const dvs::domain::FrameId frameId) {
    const auto sourceA = dvs::media::MediaProbe::inspect(sourceAPath, dvs::domain::SourceId{0});
    if (!sourceA) {
        printError(sourceA.error());
        return EXIT_FAILURE;
    }
    const auto sourceB = dvs::media::MediaProbe::inspect(sourceBPath, dvs::domain::SourceId{1});
    if (!sourceB) {
        printError(sourceB.error());
        return EXIT_FAILURE;
    }

    constexpr std::size_t kFrameBudgetBytes = 256U * 1024U * 1024U;
    dvs::platform::FrameBudget frameBudget{kFrameBudgetBytes};
    const auto provider = std::make_shared<dvs::media::MultiSourceFrameProvider>(frameBudget);
    const auto comparison = dvs::ui::compareDirectSources(
        provider, frameBudget, sourceA.value(), sourceB.value(), frameId);
    if (!comparison) {
        printError(comparison.error());
        return EXIT_FAILURE;
    }

    const dvs::ui::DirectComparisonResult& result = comparison.value();
    std::cout << "frame=" << result.frameId.value() << '\n'
              << "a=" << result.sourceA.width << 'x' << result.sourceA.height
              << " bytes=" << result.sourceA.accountedBytes << '\n'
              << "b=" << result.sourceB.width << 'x' << result.sourceB.height
              << " bytes=" << result.sourceB.accountedBytes << '\n'
              << "reserved-bytes=" << result.reservedBytes << '\n';
    return EXIT_SUCCESS;
}

[[nodiscard]] std::optional<dvs::domain::FrameId> parseFrameId(const std::string_view text) {
    std::int64_t value = 0;
    const auto [position, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || position != text.data() + text.size()) {
        return std::nullopt;
    }
    const dvs::domain::FrameId frameId{value};
    if (!frameId.isValid()) {
        return std::nullopt;
    }
    return frameId;
}

void printUsage() {
    std::cerr << "Usage:\n"
              << "  CompareStationCli --startup-check\n"
              << "  CompareStationCli --probe <source>\n"
              << "  CompareStationCli --compare <source-a> <source-b> "
                 "[--frame <zero-based-frame>]\n";
}

[[nodiscard]] int runStartupCheck() {
    constexpr std::array kExpectedModules{
        std::string_view{"dvs_platform_windows"},
        std::string_view{"dvs_media_ffmpeg"},
        std::string_view{"dvs_persistence_json"},
        std::string_view{"dvs_ui_qml"},
    };
    const std::array kActualModules{
        dvs::platform::moduleName(),
        dvs::media::moduleName(),
        dvs::persistence::moduleName(),
        dvs::ui::moduleName(),
    };

    if (kActualModules != kExpectedModules) {
        std::cerr << "DVS_STARTUP_MODULE_CHECK_FAILED\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view{argv[1]} == "--startup-check") {
        return runStartupCheck();
    }
    if (argc == 3 && std::string_view{argv[1]} == "--probe") {
        return runProbe(std::filesystem::path{argv[2]});
    }
    if (argc == 4 && std::string_view{argv[1]} == "--compare") {
        return runCompare(std::filesystem::path{argv[2]},
                          std::filesystem::path{argv[3]},
                          dvs::domain::FrameId{0});
    }
    if (argc == 6 && std::string_view{argv[1]} == "--compare" &&
        std::string_view{argv[4]} == "--frame") {
        const auto frameId = parseFrameId(argv[5]);
        if (!frameId.has_value()) {
            std::cerr << "error=invalid-frame-id\n";
            return EXIT_FAILURE;
        }
        return runCompare(std::filesystem::path{argv[2]}, std::filesystem::path{argv[3]}, *frameId);
    }

    printUsage();
    return EXIT_FAILURE;
}
