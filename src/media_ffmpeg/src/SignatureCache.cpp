#include "SignatureCache.h"

extern "C" {
#include <libavcodec/version.h>
#include <libavutil/version.h>
}

#include <utility>

namespace dvs::media::internal {
namespace {

inline constexpr std::uint32_t kSignatureAlgorithmVersion = 2U;
inline constexpr std::uint32_t kNormalizationVersion = 2U;

} // namespace

SignatureCache::SignatureCache(const std::size_t maximumSignatures)
    : maximumSignatures_(maximumSignatures) {}

std::optional<std::string> SignatureCache::makeKey(const domain::MediaDescriptor& descriptor) {
    if (!descriptor.sourceIdentity.has_value() || !descriptor.sourceIdentity->isComplete() ||
        !descriptor.extent.isValid()) {
        return std::nullopt;
    }
    return descriptor.sourceIdentity->fingerprintSha256 +
           "|rotation=" + std::to_string(descriptor.rotationDegrees) +
           "|sar=" + std::to_string(descriptor.sampleAspectRatio.numerator) + ":" +
           std::to_string(descriptor.sampleAspectRatio.denominator) + "|" +
           std::to_string(descriptor.extent.width) + "x" +
           std::to_string(descriptor.extent.height) +
           "|algorithm=" + std::to_string(kSignatureAlgorithmVersion) +
           "|normalization=" + std::to_string(kNormalizationVersion) +
           "|avcodec=" + std::to_string(LIBAVCODEC_VERSION_MAJOR) + "." +
           std::to_string(LIBAVCODEC_VERSION_MINOR) +
           "|avutil=" + std::to_string(LIBAVUTIL_VERSION_MAJOR) + "." +
           std::to_string(LIBAVUTIL_VERSION_MINOR);
}

std::optional<application::FrameLumaSignature>
SignatureCache::find(const domain::MediaDescriptor& descriptor,
                     const domain::FrameId frameId) const {
    const std::optional<std::string> key = makeKey(descriptor);
    if (!key.has_value() || !frameId.isValid()) {
        return std::nullopt;
    }
    std::scoped_lock lock(mutex_);
    const auto source = entries_.find(*key);
    if (source == entries_.end()) {
        return std::nullopt;
    }
    const auto signature = source->second.find(frameId.value());
    return signature == source->second.end()
               ? std::nullopt
               : std::optional<application::FrameLumaSignature>{signature->second};
}

std::optional<std::vector<application::FrameLumaSignature>>
SignatureCache::findRange(const domain::MediaDescriptor& descriptor,
                          const std::int64_t frameCount) const {
    const std::optional<std::string> key = makeKey(descriptor);
    if (!key.has_value() || frameCount <= 0) {
        return std::nullopt;
    }
    std::scoped_lock lock(mutex_);
    const auto source = entries_.find(*key);
    if (source == entries_.end() || source->second.size() < static_cast<std::size_t>(frameCount)) {
        return std::nullopt;
    }
    std::vector<application::FrameLumaSignature> result;
    result.reserve(static_cast<std::size_t>(frameCount));
    for (std::int64_t frame = 0; frame < frameCount; ++frame) {
        const auto signature = source->second.find(frame);
        if (signature == source->second.end()) {
            return std::nullopt;
        }
        result.push_back(signature->second);
    }
    return result;
}

void SignatureCache::store(const domain::MediaDescriptor& descriptor,
                           application::FrameLumaSignature signature) {
    const std::optional<std::string> key = makeKey(descriptor);
    if (!key.has_value() || !signature.isValid()) {
        return;
    }
    std::scoped_lock lock(mutex_);
    storeLocked(*key, signature);
}

void SignatureCache::storeRange(const domain::MediaDescriptor& descriptor,
                                const std::vector<application::FrameLumaSignature>& signatures) {
    const std::optional<std::string> key = makeKey(descriptor);
    if (!key.has_value()) {
        return;
    }
    std::vector<application::FrameLumaSignature> validated;
    validated.reserve(signatures.size());
    for (const application::FrameLumaSignature& signature : signatures) {
        if (!signature.isValid()) {
            return;
        }
        validated.push_back(signature);
    }
    std::scoped_lock lock(mutex_);
    for (application::FrameLumaSignature& signature : validated) {
        storeLocked(*key, signature);
    }
}

std::size_t SignatureCache::entryCountForTesting() const noexcept {
    std::scoped_lock lock(mutex_);
    return entryCount_;
}

void SignatureCache::storeLocked(const std::string& key,
                                 application::FrameLumaSignature signature) {
    if (maximumSignatures_ == 0U) {
        return;
    }
    auto& source = entries_[key];
    const std::int64_t frame = signature.frameId.value();
    const bool inserted = source.find(frame) == source.end();
    source.insert_or_assign(frame, signature);
    if (inserted) {
        insertionOrder_.emplace_back(key, frame);
        ++entryCount_;
        evictToCapacityLocked();
    }
}

void SignatureCache::evictToCapacityLocked() {
    while (entryCount_ > maximumSignatures_ && !insertionOrder_.empty()) {
        auto [key, frame] = std::move(insertionOrder_.front());
        insertionOrder_.pop_front();
        const auto source = entries_.find(key);
        if (source == entries_.end() || source->second.erase(frame) == 0U) {
            continue;
        }
        --entryCount_;
        if (source->second.empty()) {
            entries_.erase(source);
        }
    }
}

} // namespace dvs::media::internal
