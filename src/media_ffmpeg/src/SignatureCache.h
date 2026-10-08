#pragma once

#include "dvs/application/Alignment.h"
#include "dvs/domain/MediaDescriptor.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dvs::media::internal {

// Process-local analysis evidence cache. The key deliberately describes the source content and
// every normalization choice that can change a signature; request/session identity is excluded.
class SignatureCache final {
public:
    explicit SignatureCache(std::size_t maximumSignatures = 50'000U);

    [[nodiscard]] std::optional<application::FrameLumaSignature>
    find(const domain::MediaDescriptor& descriptor, domain::FrameId frameId) const;

    [[nodiscard]] std::optional<std::vector<application::FrameLumaSignature>>
    findRange(const domain::MediaDescriptor& descriptor, std::int64_t frameCount) const;

    void store(const domain::MediaDescriptor& descriptor,
               application::FrameLumaSignature signature);
    void storeRange(const domain::MediaDescriptor& descriptor,
                    const std::vector<application::FrameLumaSignature>& signatures);
    [[nodiscard]] std::size_t entryCountForTesting() const noexcept;

private:
    [[nodiscard]] static std::optional<std::string>
    makeKey(const domain::MediaDescriptor& descriptor);
    void storeLocked(const std::string& key, application::FrameLumaSignature signature);
    void evictToCapacityLocked();

    mutable std::mutex mutex_;
    std::size_t maximumSignatures_;
    std::size_t entryCount_ = 0U;
    std::map<std::string, std::map<std::int64_t, application::FrameLumaSignature>, std::less<>>
        entries_;
    std::deque<std::pair<std::string, std::int64_t>> insertionOrder_;
};

} // namespace dvs::media::internal
