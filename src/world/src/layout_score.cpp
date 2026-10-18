#include "layout_score.hpp"

#include <algorithm>

namespace rv::world::detail {
namespace {

std::int64_t clampPermille(std::int64_t value) noexcept {
    return std::clamp<std::int64_t>(value, 0, 1000);
}

/// count * penalty, saturated at the whole score (a plan with a million collisions scores 0, without overflow).
std::int64_t penalty(std::uint64_t count, std::int64_t perProblem) noexcept {
    const std::uint64_t cap = 1000 / static_cast<std::uint64_t>(perProblem) + 1;
    return static_cast<std::int64_t>(std::min(count, cap)) * perProblem;
}

} // namespace

std::int64_t layoutScorePermille(const LayoutScoreTerms& terms) noexcept {
    static_assert(kWeightNavigationClearance + kWeightHeroVisibility + kWeightCollectionCapacity +
                      kWeightVisualBalance + kWeightScreenVisibility + kWeightThemeMatch + kWeightDecorationVariation ==
                  1000);
    const std::int64_t weighted = kWeightNavigationClearance * clampPermille(terms.navigationClearance) +
                                  kWeightHeroVisibility * clampPermille(terms.heroVisibility) +
                                  kWeightCollectionCapacity * clampPermille(terms.collectionCapacity) +
                                  kWeightVisualBalance * clampPermille(terms.visualBalance) +
                                  kWeightScreenVisibility * clampPermille(terms.screenVisibility) +
                                  kWeightThemeMatch * clampPermille(terms.themeMatch) +
                                  kWeightDecorationVariation * clampPermille(terms.decorationVariation);
    const std::int64_t penalties = penalty(terms.collisions, kPenaltyCollision) +
                                   penalty(terms.blockedDoors, kPenaltyBlockedDoor) +
                                   penalty(terms.repeatedPatterns, kPenaltyRepeatedPattern);
    return clampPermille(weighted / 1000 - penalties);
}

} // namespace rv::world::detail
