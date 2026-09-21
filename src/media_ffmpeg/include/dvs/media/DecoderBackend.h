#pragma once

#include "dvs/domain/Identifiers.h"

#include <cstdint>
#include <string>

namespace dvs::media {

enum class DecoderBackend {
    Software,
    D3d11Va,
};

struct DecoderBackendStatus final {
    domain::SourceId sourceId = 0U;
    DecoderBackend backend = DecoderBackend::Software;
    std::string fallbackReason;
    domain::DeviceGeneration deviceGeneration{0U};
    std::uint64_t completedDecodeCount = 0U;
    std::uint64_t cacheHitCount = 0U;
    std::uint64_t exactSeekCount = 0U;
    std::uint64_t totalDecodeMicroseconds = 0U;
    std::uint64_t maximumDecodeMicroseconds = 0U;
    // Reverse GOP Window (ADR-003): hit = reverse target already covered by a retained window;
    // build = one seed-seek + sequential walk; fallback = window skipped, per-step Exact remains.
    std::uint64_t reverseWindowHitCount = 0U;
    std::uint64_t reverseWindowBuildCount = 0U;
    std::uint64_t reverseWindowBuiltFrameCount = 0U;
    std::uint64_t reverseWindowBuildMicroseconds = 0U;
    std::uint64_t reverseWindowBuildMaximumMicroseconds = 0U;
    std::uint64_t reverseExactFallbackCount = 0U;

    [[nodiscard]] bool operator==(const DecoderBackendStatus&) const = default;
};

} // namespace dvs::media
