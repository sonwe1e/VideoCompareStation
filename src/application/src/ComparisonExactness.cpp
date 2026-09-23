#include "dvs/application/ComparisonExactness.h"

#include "dvs/application/SessionSnapshot.h"

#include <algorithm>
#include <string_view>

namespace dvs::application {
namespace {

[[nodiscard]] bool preservesNormalizedPlaneCodes(const std::string_view pixelFormat) noexcept {
    return pixelFormat == "yuv420p" || pixelFormat == "yuvj420p" || pixelFormat == "yuv420p10le" ||
           pixelFormat == "nv12" || pixelFormat == "p010le";
}

} // namespace

// T6: multi-dimensional decomposition of comparison exactness (see header).
ComparisonExactnessDimensions
comparisonExactnessDimensions(const SessionSnapshot& snapshot,
                              const domain::SourceId first,
                              const domain::SourceId second) noexcept {
    ComparisonExactnessDimensions dimensions;
    if (first == second || !snapshot.validatedComparison || !snapshot.displayedFrame.has_value()) {
        return dimensions;
    }
    const domain::ComparisonSource* sourceA = snapshot.validatedComparison->find(first);
    const domain::ComparisonSource* sourceB = snapshot.validatedComparison->find(second);
    const auto presented = [&snapshot](const domain::SourceId sourceId) {
        return std::find_if(
            snapshot.presentedSources.begin(),
            snapshot.presentedSources.end(),
            [sourceId](const PresentedSourceState& source) { return source.sourceId == sourceId; });
    };
    const auto presentedA = presented(first);
    const auto presentedB = presented(second);
    if (sourceA == nullptr || sourceB == nullptr || presentedA == snapshot.presentedSources.end() ||
        presentedB == snapshot.presentedSources.end() || !presentedA->sourceFrameId.has_value() ||
        !presentedB->sourceFrameId.has_value()) {
        return dimensions;
    }
    dimensions.available = true;
    // Each dimension is derived independently so the projection never hides one
    // inexactness reason behind another (T6 multi-dimensional trust state).
    dimensions.temporalExact = presentedA->matchKind == FrameMatchKind::ExactIndex &&
                               presentedB->matchKind == FrameMatchKind::ExactIndex &&
                               presentedA->presentationTime == presentedB->presentationTime;
    const domain::MediaDescriptor& descriptorA = sourceA->descriptor;
    const domain::MediaDescriptor& descriptorB = sourceB->descriptor;
    dimensions.spatialExact = descriptorA.extent == descriptorB.extent &&
                              descriptorA.rotationDegrees == descriptorB.rotationDegrees &&
                              descriptorA.sampleAspectRatio == descriptorB.sampleAspectRatio;
    dimensions.pixelExact =
        descriptorA.pixelFormatId == descriptorB.pixelFormatId &&
        descriptorA.bitDepth == descriptorB.bitDepth &&
        descriptorA.colorMetadata.matrix == descriptorB.colorMetadata.matrix &&
        descriptorA.colorMetadata.range == descriptorB.colorMetadata.range &&
        descriptorA.colorMetadata.transfer == descriptorB.colorMetadata.transfer &&
        preservesNormalizedPlaneCodes(descriptorA.pixelFormatId);
    return dimensions;
}

ComparisonExactness comparisonExactness(const SessionSnapshot& snapshot,
                                        const domain::SourceId first,
                                        const domain::SourceId second) noexcept {
    const ComparisonExactnessDimensions dimensions =
        comparisonExactnessDimensions(snapshot, first, second);
    if (!dimensions.available) {
        return ComparisonExactness::Unavailable;
    }
    if (!dimensions.temporalExact) {
        return ComparisonExactness::TemporallyAligned;
    }
    if (!dimensions.spatialExact) {
        return ComparisonExactness::SpatiallyResampled;
    }
    if (!dimensions.pixelExact) {
        return ComparisonExactness::DisplaySpaceConverted;
    }
    return ComparisonExactness::ExactCodeValue;
}

} // namespace dvs::application
