#pragma once

#include "dvs/domain/Identifiers.h"
#include "dvs/domain/Result.h"

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace dvs::domain {

// Zero-based ordinal of a frame in the ground-truth sequence. Distinct from FrameId, which
// addresses the canonical playback timeline; the two identity spaces must never be mixed.
class GtFrameIndex final {
public:
    constexpr explicit GtFrameIndex(const std::int64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::int64_t value() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return value_ >= 0;
    }

    [[nodiscard]] constexpr auto operator<=>(const GtFrameIndex&) const noexcept = default;

private:
    std::int64_t value_;
};

// How an output-to-GT mapping came to exist. Suggestions stay non-authoritative until the user
// confirms them; only imported manifests and manual entries describe a committed mapping.
enum class MappingOrigin {
    kImportedManifest,
    kManual,
    kSuggestion,
};

// One output position copied through decimation from a single GT frame. Position equality is not
// a bit-level identity claim: re-encoding may still have altered the pixels.
struct RetainedFrame final {
    GtFrameIndex gtFrame{0};

    [[nodiscard]] constexpr bool operator==(const RetainedFrame&) const noexcept = default;
};

// One output position synthesized between two GT inputs. targetTimeRatio is the GT-relative
// position strictly between the inputs, stored in lowest terms (0 < numerator < denominator).
struct GeneratedFrame final {
    GtFrameIndex input0GtFrame{0};
    GtFrameIndex input1GtFrame{0};
    std::int64_t targetRatioNumerator = 1;
    std::int64_t targetRatioDenominator = 2;

    [[nodiscard]] constexpr bool operator==(const GeneratedFrame&) const noexcept = default;
};

// Present in the output but with unknown provenance. Never coerced into retained or generated.
struct UnknownFrame final {
    [[nodiscard]] constexpr bool operator==(const UnknownFrame&) const noexcept = default;
};

// Expected at this output position but absent from the asset. Missing positions are never filled
// with a neighbour; expectedGtFrame is carried when the expected target is known.
struct MissingFrame final {
    std::optional<GtFrameIndex> expectedGtFrame;

    [[nodiscard]] constexpr bool operator==(const MissingFrame&) const noexcept = default;
};

using OutputFrameOrigin = std::variant<RetainedFrame, GeneratedFrame, UnknownFrame, MissingFrame>;

// Immutable description of how every frame of an interpolated output sequence maps back to the
// ground-truth sequence. Internal indices are zero-based; importers and exporters carry their own
// index base explicitly. Creation validates the whole mapping: present frames (retained and
// generated) must occupy strictly increasing GT positions, and generated targets must lie
// strictly between their inputs.
class InterpolationProtocol final {
public:
    [[nodiscard]] static Result<InterpolationProtocol>
    create(std::int64_t gtFrameCount,
           MappingOrigin mappingOrigin,
           std::vector<OutputFrameOrigin> origins);

    // Number of output positions described by the protocol.
    [[nodiscard]] std::int64_t frameCount() const noexcept;
    // Total frames in the GT sequence; 0 means unknown and disables range checks only.
    [[nodiscard]] std::int64_t gtFrameCount() const noexcept;
    [[nodiscard]] MappingOrigin mappingOrigin() const noexcept;

    [[nodiscard]] Result<OutputFrameOrigin> originAt(FrameId outputFrame) const;

    // First generated output position strictly after `after`; a negative id searches from the
    // start. Positions that are missing or of unknown provenance are skipped because there is no
    // frame to show at them. Returns nullopt when no generated frame exists.
    [[nodiscard]] std::optional<FrameId> nextGeneratedFrame(FrameId after) const;
    // Mirror of nextGeneratedFrame; an id at or past frameCount() searches from the end.
    [[nodiscard]] std::optional<FrameId> previousGeneratedFrame(FrameId before) const;

    [[nodiscard]] bool operator==(const InterpolationProtocol&) const noexcept = default;

private:
    InterpolationProtocol(std::int64_t gtFrameCount,
                          MappingOrigin mappingOrigin,
                          std::vector<OutputFrameOrigin> origins) noexcept;

    std::int64_t gtFrameCount_;
    MappingOrigin mappingOrigin_;
    std::vector<OutputFrameOrigin> origins_;
};

// What happens to the final partial decimation interval when the GT sequence does not end exactly
// on a retained frame.
enum class TailPolicy {
    // The partial interval is dropped; its GT frames have no output position.
    kTrimPartialInterval,
    // Each expected output position of the partial interval is kept and marked missing.
    kMarkPartialIntervalMissing,
};

// Declarative decimation: GT frames at phase, phase + stride, phase + 2 * stride, ... are
// retained, and every GT frame between two consecutive retained frames becomes one generated
// output targeting it. GT frames before the first retained frame have no output position.
struct DecimationPlan final {
    std::int64_t gtFrameCount = 0;
    std::int64_t stride = 2;
    std::int64_t phase = 0;
};

[[nodiscard]] Result<InterpolationProtocol> buildDecimatedProtocol(const DecimationPlan& plan,
                                                                   MappingOrigin mappingOrigin,
                                                                   TailPolicy tailPolicy);

} // namespace dvs::domain
