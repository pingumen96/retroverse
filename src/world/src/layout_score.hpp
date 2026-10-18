#pragma once

// Private to rv_world: style §13.5's LayoutScore, computed by the validation (validation.cpp) and reported as a
// diagnostic that chooses nothing in I3 (ADR-0010 §4: one layout candidate). Integers in permille, so that every build
// reports the same score.

#include <cstdint>

namespace rv::world::detail {

/// The score's terms, each in permille (0 to 1000; values outside are clamped), and the counts its penalties follow.
struct LayoutScoreTerms {
    std::int64_t navigationClearance = 1000; ///< the targets reachable through the main passage width
    std::int64_t heroVisibility = 1000;      ///< the hero TV in line of sight from the doorway
    std::int64_t collectionCapacity = 1000;  ///< the titles placed among those the room was asked to hold
    std::int64_t visualBalance = 1000;       ///< the side-wall modules shared between the two side walls
    std::int64_t screenVisibility = 1000;    ///< the room's screens in line of sight (the hero TV alone in I3)
    std::int64_t themeMatch = 1000;          ///< one theme pack per room in I3: not judged
    std::int64_t decorationVariation = 1000; ///< distinct decoration prefabs among the decoration fixtures
    std::uint64_t collisions = 0;            ///< overlapping fixture pairs
    std::uint64_t blockedDoors = 0;     ///< the doorway's clear area, door-reserved sockets entered, a blocked door
    std::uint64_t repeatedPatterns = 0; ///< adjacent levels of one fixture dressed with the same prop
};

/// The weights of style §13.5, in permille (they sum to 1000).
inline constexpr std::int64_t kWeightNavigationClearance = 240;
inline constexpr std::int64_t kWeightHeroVisibility = 190;
inline constexpr std::int64_t kWeightCollectionCapacity = 170;
inline constexpr std::int64_t kWeightVisualBalance = 150;
inline constexpr std::int64_t kWeightScreenVisibility = 100;
inline constexpr std::int64_t kWeightThemeMatch = 80;
inline constexpr std::int64_t kWeightDecorationVariation = 70;
/// The penalties per counted problem, in permille of the score.
inline constexpr std::int64_t kPenaltyCollision = 100;
inline constexpr std::int64_t kPenaltyBlockedDoor = 200;
inline constexpr std::int64_t kPenaltyRepeatedPattern = 20;

/// The weighted sum of the terms (rounded down) minus the penalties, clamped to 0..1000.
[[nodiscard]] std::int64_t layoutScorePermille(const LayoutScoreTerms& terms) noexcept;

} // namespace rv::world::detail
