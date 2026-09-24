#include "dvs/application/ComparisonExactness.h"

#include "dvs/application/SessionSnapshot.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace dvs::application {
namespace {

[[nodiscard]] bool preservesNormalizedPlaneCodes(const std::string_view pixelFormat) noexcept {
    return pixelFormat == "yuv420p" || pixelFormat == "yuvj420p" || pixelFormat == "yuv420p10le" ||
           pixelFormat == "nv12" || pixelFormat == "p010le";
}

} // namespace

// T6: multi-dimensional decomposition of comparison exactness (see header).
ComparisonExactnessDimensions comparisonExactnessDimensions(const SessionSnapshot& snapshot,
                                                            const domain::SourceId first,
                                                            const domain::SourceId second) {
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

    std::vector<std::string> reasons;
    if (!dimensions.temporalExact) {
        if (presentedA->presentationTime != presentedB->presentationTime) {
            const auto diffUs = std::abs(presentedA->presentationTime.microseconds() -
                                         presentedB->presentationTime.microseconds());
            reasons.push_back("时间戳不一致 (相差 " + std::to_string(diffUs / 1000) + " ms)");
        }
        if (presentedA->matchKind != FrameMatchKind::ExactIndex ||
            presentedB->matchKind != FrameMatchKind::ExactIndex) {
            if (presentedA->matchKind == FrameMatchKind::TimeAligned ||
                presentedB->matchKind == FrameMatchKind::TimeAligned) {
                reasons.push_back("按时间映射 (非严格帧号对应)");
            } else {
                reasons.push_back("非基准严格索引映射");
            }
        }
    }
    if (!dimensions.spatialExact) {
        if (descriptorA.extent != descriptorB.extent) {
            reasons.push_back("尺寸不同 (" + std::to_string(descriptorA.extent.width) + "x" +
                              std::to_string(descriptorA.extent.height) + " 与 " +
                              std::to_string(descriptorB.extent.width) + "x" +
                              std::to_string(descriptorB.extent.height) + "，非同尺寸像素对应)");
        } else {
            reasons.push_back("像素宽高比或旋转不同，几何对应不一致");
        }
    }
    if (!dimensions.pixelExact) {
        if (descriptorA.pixelFormatId != descriptorB.pixelFormatId ||
            descriptorA.bitDepth != descriptorB.bitDepth) {
            reasons.push_back("像素格式或位深不同 (" + descriptorA.pixelFormatId + " " +
                              std::to_string(descriptorA.bitDepth) + "-bit 与 " +
                              descriptorB.pixelFormatId + " " +
                              std::to_string(descriptorB.bitDepth) + "-bit)");
        } else {
            reasons.push_back("显示空间转换 (非原生直通码值)");
        }
    }
    for (std::size_t i = 0; i < reasons.size(); ++i) {
        if (i > 0) {
            dimensions.inexactReason += " · ";
        }
        dimensions.inexactReason += reasons[i];
    }

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
