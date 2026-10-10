#pragma once

#include "dvs/application/SampleExport.h"
#include "dvs/domain/Identifiers.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dvs::application {

// Identity of one sample-triplet export. The owner mints the value, the worker echoes it back,
// and the owner drops any completion whose request id or session epoch is stale. The same id also
// names the exporter's staging directory, so a repeated run of one request replaces that request's
// own leftovers instead of accumulating them.
using SampleExportRequestId = std::uint64_t;
inline constexpr SampleExportRequestId kInvalidSampleExportRequestId = 0U;

enum class SampleExportOutcome {
    kCompleted,
    kCanceled,
    kFailed,
};

struct SampleExportJob final {
    SampleExportRequestId requestId = kInvalidSampleExportRequestId;
    domain::SessionId sessionId{0};
    domain::SessionEpoch sessionEpoch{0};
    // Names both the published sample directory and the manifest's sample_id. Must be a safe
    // single path segment (no separators, no leading dot), because the exporter publishes the
    // staged directory under exactly this name.
    std::string sampleId;
    // What to extract, produced by planSampleTriplet. The writer re-validates the crop against
    // the probed source geometry: a job is data, not proof that the planner ran.
    SampleTripletPlan plan;
    std::filesystem::path gtAssetPath;
    std::filesystem::path candidateAssetPath;
    std::filesystem::path outputDirectory;
};

struct SampleExportReport final {
    SampleExportRequestId requestId = kInvalidSampleExportRequestId;
    SampleExportOutcome outcome = SampleExportOutcome::kFailed;
    // Absolute paths of the four published member PNGs, in input0/gt/input1/prediction order.
    // Empty unless the outcome is kCompleted.
    std::vector<std::filesystem::path> publishedMembers;
    std::filesystem::path manifestPath;
    // Diagnostic only; never shown as a user-visible label.
    std::string technicalDetail;
};

// Executes one planned training-sample triplet: decodes the exact display-order frames named by
// the plan, converts them to PNG members with an honest pixel-conversion policy, and publishes
// the sample directory atomically. Export is not part of the playback pipeline: perform() blocks
// by design and belongs on a worker thread the caller owns.
//
// Implementations must publish transactionally. A canceled or failed export never leaves a
// partially populated sample directory, and the published directory only ever contains a set of
// members that passed read-back validation.
class ISampleExporter {
public:
    ISampleExporter() = default;
    ISampleExporter(const ISampleExporter&) = delete;
    ISampleExporter& operator=(const ISampleExporter&) = delete;
    ISampleExporter(ISampleExporter&&) = delete;
    ISampleExporter& operator=(ISampleExporter&&) = delete;
    virtual ~ISampleExporter() = default;

    // Runs on a worker thread. Honours cooperative cancellation through `cancelRequested` and
    // reports failure in the returned report instead of throwing.
    [[nodiscard]] virtual SampleExportReport perform(const SampleExportJob& job,
                                                     const std::atomic_bool& cancelRequested) = 0;
};

} // namespace dvs::application
