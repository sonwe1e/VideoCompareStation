#include "dvs/domain/ComparisonSelection.h"

#include <algorithm>

namespace dvs::domain {

namespace {

[[nodiscard]] bool containsSource(const std::span<const ComparisonSource> sources,
                                  const SourceId id) noexcept {
    return std::any_of(sources.begin(), sources.end(), [id](const ComparisonSource& source) {
        return source.id == id;
    });
}

[[nodiscard]] ComparisonPair lastTwoActive(std::span<const ComparisonSource> sources) noexcept {
    if (sources.size() < 2U) {
        return ComparisonPair{sources.empty() ? SourceId{0} : sources.front().id,
                              sources.empty() ? SourceId{0} : sources.front().id};
    }
    return ComparisonPair{sources[sources.size() - 2U].id, sources.back().id};
}

[[nodiscard]] ComparisonPair
referenceAndFirstCandidate(const std::span<const ComparisonSource> sources,
                           const std::optional<SourceId> referenceId) noexcept {
    if (sources.empty()) {
        return ComparisonPair{};
    }
    if (!referenceId.has_value() || !containsSource(sources, *referenceId)) {
        return lastTwoActive(sources);
    }
    for (const ComparisonSource& source : sources) {
        if (source.id != *referenceId) {
            return ComparisonPair{*referenceId, source.id};
        }
    }
    return ComparisonPair{*referenceId, *referenceId};
}

} // namespace

ComparisonPair resolveComparisonPair(const std::span<const ComparisonSource> sources,
                                     const std::optional<SourceId> referenceId,
                                     const std::optional<ComparisonPair> preferred,
                                     const DefaultPairPolicy policy) noexcept {
    if (sources.empty()) {
        return ComparisonPair{};
    }
    const auto preferredUsable = [&]() {
        return preferred.has_value() && preferred->isValid() &&
               containsSource(sources, preferred->first) &&
               containsSource(sources, preferred->second);
    };
    if (preferredUsable() && (policy == DefaultPairPolicy::PreserveIfAvailable ||
                              policy == DefaultPairPolicy::ReferenceAndFirstCandidate)) {
        // PreserveIfAvailable honors a live preferred pair. Reference policy only accepts a
        // preferred pair that still includes the current reference (or has no reference).
        if (policy == DefaultPairPolicy::PreserveIfAvailable || !referenceId.has_value() ||
            preferred->contains(*referenceId)) {
            return *preferred;
        }
    }
    switch (policy) {
    case DefaultPairPolicy::ReferenceAndFirstCandidate:
        return referenceAndFirstCandidate(sources, referenceId);
    case DefaultPairPolicy::LastTwoActiveSources:
        return lastTwoActive(sources);
    case DefaultPairPolicy::PreserveIfAvailable:
        return preferredUsable() ? *preferred : lastTwoActive(sources);
    }
    return lastTwoActive(sources);
}

bool sameMediaIdentity(const ComparisonSource& left, const ComparisonSource& right) noexcept {
    const auto& leftIdentity = left.descriptor.sourceIdentity;
    const auto& rightIdentity = right.descriptor.sourceIdentity;
    const bool leftComplete = leftIdentity.has_value() && leftIdentity->isComplete();
    const bool rightComplete = rightIdentity.has_value() && rightIdentity->isComplete();
    if (leftComplete && rightComplete) {
        return leftIdentity->byteSize == rightIdentity->byteSize &&
               leftIdentity->modifiedUtcMilliseconds == rightIdentity->modifiedUtcMilliseconds &&
               leftIdentity->fingerprintSha256 == rightIdentity->fingerprintSha256;
    }
    return left.descriptor.normalizedPath == right.descriptor.normalizedPath;
}

std::optional<ComparisonPair>
remapComparisonPairByMediaIdentity(const std::span<const ComparisonSource> previousSources,
                                   const std::optional<ComparisonPair> preferred,
                                   const std::span<const ComparisonSource> nextSources) noexcept {
    if (!preferred.has_value() || !preferred->isValid()) {
        return std::nullopt;
    }
    const ComparisonSource* previousFirst = nullptr;
    const ComparisonSource* previousSecond = nullptr;
    for (const ComparisonSource& source : previousSources) {
        if (source.id == preferred->first) {
            previousFirst = &source;
        } else if (source.id == preferred->second) {
            previousSecond = &source;
        }
    }
    if (previousFirst == nullptr || previousSecond == nullptr) {
        return std::nullopt;
    }
    const ComparisonSource* nextFirst = nullptr;
    const ComparisonSource* nextSecond = nullptr;
    for (const ComparisonSource& source : nextSources) {
        if (nextFirst == nullptr && sameMediaIdentity(*previousFirst, source)) {
            nextFirst = &source;
            continue;
        }
        if (nextSecond == nullptr && sameMediaIdentity(*previousSecond, source)) {
            nextSecond = &source;
        }
    }
    if (nextFirst == nullptr || nextSecond == nullptr || nextFirst->id == nextSecond->id) {
        return std::nullopt;
    }
    return ComparisonPair{nextFirst->id, nextSecond->id};
}

std::optional<std::uint8_t>
comparisonPairEdgeOrdinal(const std::span<const ComparisonSource> sources,
                          const ComparisonPair& pair) noexcept {
    if (!pair.isValid() || !containsSource(sources, pair.first) ||
        !containsSource(sources, pair.second)) {
        return std::nullopt;
    }
    std::size_t firstIndex = sources.size();
    std::size_t secondIndex = sources.size();
    for (std::size_t index = 0; index < sources.size(); ++index) {
        if (sources[index].id == pair.first) {
            firstIndex = index;
        } else if (sources[index].id == pair.second) {
            secondIndex = index;
        }
    }
    if (firstIndex == sources.size() || secondIndex == sources.size() ||
        firstIndex == secondIndex) {
        return std::nullopt;
    }
    if (firstIndex > secondIndex) {
        std::swap(firstIndex, secondIndex);
    }
    // Session-order indices projected onto the compact 0↔1 / 0↔2 / 1↔2 renderer edge enum.
    if (firstIndex == 0U && secondIndex == 1U) {
        return std::uint8_t{0U};
    }
    if (firstIndex == 0U && secondIndex == 2U) {
        return std::uint8_t{1U};
    }
    if (firstIndex == 1U && secondIndex == 2U) {
        return std::uint8_t{2U};
    }
    return std::nullopt;
}

} // namespace dvs::domain
