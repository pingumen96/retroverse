#pragma once

// Where-is (ADR-0010 §9; I3 step 9): the index of a plan, computed from the plan alone, no table: GameId to its slot
// and the fixture it stands in, slot to GameId, and the unplaced games with their reasons (a duplicate title with its
// representative and where that one stands). `retroverse-cli where-is <gameId>` reads the plan and asks it; the app
// keeps the index of the plan it shows (I3 step 15a's "Mostra nella stanza"). Built in a few milliseconds for the
// reference room's 890 placements and 6,907 unplaced games: sorted vectors searched by binary search, nothing
// iterated from an unordered container.

#include <cstddef>
#include <optional>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// Where a placed game stands.
struct GamePlace {
    SlotId slot;
    /// Its fixture's role and prefab, and the section of its level (face-out on the display fixtures and in the hero
    /// band, spine-out elsewhere).
    FixtureRole role = FixtureRole::SideModule;
    PrefabRef prefab;
    Section section = Section::Spine;

    bool operator==(const GamePlace&) const = default;
};

/// The answer for one game of the plan.
struct GameLocation {
    catalog::GameId game;
    RoomId room;
    /// Set for a placed game.
    std::optional<GamePlace> place;
    /// Set for an unplaced one, with the representative of a duplicate title and where it stands (when placed).
    std::optional<UnplacedReason> reason;
    catalog::GameId representative;
    std::optional<GamePlace> representativePlace;

    [[nodiscard]] bool placed() const noexcept { return place.has_value(); }

    bool operator==(const GameLocation&) const = default;
};

class PlanIndex {
public:
    PlanIndex() = default;
    /// Indexes `plan` (any order of its lists). A placement whose fixture or level the plan lacks is indexed with the
    /// fixture's role and section unknown (SideModule, Spine): the validation reports such a plan, the index never
    /// fails. A game placed twice is answered by its first slot (ids' order), a placed game also listed unplaced by its
    /// placement, a slot holding two games by the first GameId.
    explicit PlanIndex(const WorldPlan& plan);

    /// Where `game` stands or why it does not; nothing for a game the plan does not hold.
    [[nodiscard]] std::optional<GameLocation> find(const catalog::GameId& game) const;
    /// The game standing in `slot`, if any.
    [[nodiscard]] std::optional<catalog::GameId> gameAt(const SlotId& slot) const;

    [[nodiscard]] std::size_t placedCount() const noexcept { return m_placed.size(); }
    [[nodiscard]] std::size_t unplacedCount() const noexcept { return m_unplaced.size(); }

private:
    struct PlacedEntry {
        catalog::GameId game;
        RoomId room;
        GamePlace place;
    };
    struct SlotEntry {
        SlotId slot;
        catalog::GameId game;
    };

    [[nodiscard]] const PlacedEntry* placedEntry(const catalog::GameId& game) const;

    std::vector<PlacedEntry> m_placed;    ///< by game
    std::vector<SlotEntry> m_slots;       ///< by slot
    std::vector<UnplacedGame> m_unplaced; ///< by game
};

} // namespace rv::world
