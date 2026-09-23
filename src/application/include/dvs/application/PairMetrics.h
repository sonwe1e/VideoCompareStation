#pragma once

#include "dvs/application/Alignment.h"
#include "dvs/application/Ports.h"
#include "dvs/domain/MediaError.h"
#include "dvs/domain/PixelDifference.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dvs::application {

// One scored canonical frame position for the request's source pair. Non-comparable positions
// (alignment gap, out-of-range mapping, dimension mismatch) are published with comparable=false
// instead of being silently skipped so the UI can show an explicit "not comparable" state.
struct PairMetricsSample final {
    domain::FrameId canonicalFrameId{0};
    bool comparable = false;
    domain::PixelDifferenceMetrics metrics{};

    [[nodiscard]] bool operator==(const PairMetricsSample&) const = default;
};

// Asynchronous pair-metrics request. The service decodes both sources independently of the
// playback pipeline, so this never reserves FrameBudget bytes and never touches render
// resources. firstFrame/lastFrame are inclusive canonical frame positions.
struct PairMetricsRequest final {
    PlaybackRequestContext context;
    std::vector<domain::ComparisonSource> sources;
    std::vector<SourceFrameOffset> offsets;
    std::uint64_t alignmentRevision = 0U;
    domain::FrameId firstFrame{0};
    domain::FrameId lastFrame{0};
    std::uint8_t mismatchThreshold = 0U;

    [[nodiscard]] bool isValid() const noexcept {
        if (sources.size() != 2U || firstFrame.value() > lastFrame.value()) {
            return false;
        }
        for (const domain::ComparisonSource& source : sources) {
            bool offsetFound = false;
            for (const SourceFrameOffset& offset : offsets) {
                if (offset.sourceId == source.id) {
                    offsetFound = true;
                    break;
                }
            }
            if (!offsetFound) {
                return false;
            }
        }
        return true;
    }
};

// Progress publication for one request. The service publishes batches while decoding so a
// timeline lane can fill progressively; the batch carrying finalBatch=true ends the request.
struct PairMetricsBatch final {
    PlaybackRequestContext context;
    std::vector<domain::ComparisonSource> sources;
    std::uint64_t alignmentRevision = 0U;
    std::uint8_t mismatchThreshold = 0U;
    // Provenance-stable formula identity (kRgbAbsoluteMetricId). The UI must display this
    // instead of inventing its own formula name.
    std::string metricId;
    std::vector<PairMetricsSample> samples;
    bool finalBatch = false;

    [[nodiscard]] bool operator==(const PairMetricsBatch&) const = default;
};

struct PairMetricsFailure final {
    PlaybackRequestContext context;
    domain::MediaError error{};
};

// Sink callbacks run on the metrics worker thread. Implementations must not block and must not
// touch GUI types directly; they should queue and wake their own thread.
class IPairMetricsSink {
public:
    virtual ~IPairMetricsSink() = default;

    virtual void onPairMetricsBatch(PairMetricsBatch batch) = 0;
    virtual void onPairMetricsFailure(PairMetricsFailure failure) = 0;
};

// Adapter-neutral pair-metrics port. Submitting a new request supersedes any queued request and
// cooperatively cancels the active one; superseded work completes silently (no events) and the
// caller drops stale batches by comparing the echoed request identity.
class IPairMetricsService {
public:
    virtual ~IPairMetricsService() = default;

    [[nodiscard]] virtual PortSubmitResult submit(const PairMetricsRequest& request,
                                                  std::shared_ptr<IPairMetricsSink> sink) = 0;
    virtual void cancel(const PlaybackRequestContext& context) noexcept = 0;
};

} // namespace dvs::application
