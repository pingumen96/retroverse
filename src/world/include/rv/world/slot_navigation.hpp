#pragma once

// The D-pad steps of Focus (ADR-0012 §1; I3 step 7): from the focused item to its neighbour on the left or the right
// along its level, or on the level above or below, skipping the slots that hold no item (empty ones, dressed ones) and
// crossing from one module to the next along a wall. GPU-free: the slots as the walker faces them, no geometry.
//
// The room is given as runs (a wall's fixtures side by side, left to right as one faces them), each fixture as its
// levels (bottom to top) and each level as its slots (left to right):
//  - Left / Right: the nearest item that way on the same level; past the level's end, the next fixture that way with an
//    item: its level nearest in height (the lower one on a tie) among those holding an item, and on it the item at the
//    near end (stepping right enters a fixture at its left end).
//  - Up / Down: the nearest level that way within the same fixture holding an item, and on it the item nearest along
//    the level to the current one (the left one on a tie). Up and Down never leave the fixture.
// A step that finds nothing that way answers nothing: the focus stays.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/world/pick_index.hpp>

namespace rv::world {

/// A D-pad direction, as the walker faces the shelf.
enum class StepDirection : std::uint8_t { Left, Right, Up, Down };

struct NavSlot {
    /// Position along the level, meters, increasing to the right (any origin shared by the fixture's levels).
    float along = 0.0f;
    /// The item in the slot; kNoPick for an empty or dressed slot.
    PickId item = kNoPick;
    /// The slot's own pick id (collection_layout.hpp's slotPickId()) whether it holds an item or not, so that the
    /// editor's steps reach empty slots too (I4b step 3, ADR-0015 §4); kNoPick when not known (runs made by hand).
    /// SlotNavigation itself steps between items only.
    PickId slot = kNoPick;
};

struct NavLevel {
    /// Height of the level's slots (their middle), meters, in a frame shared by the run's fixtures.
    float height = 0.0f;
    /// Left to right, `along` strictly increasing.
    std::vector<NavSlot> slots;
};

struct NavFixture {
    /// Bottom to top, `height` strictly increasing.
    std::vector<NavLevel> levels;
};

struct NavRun {
    /// Left to right as one faces them.
    std::vector<NavFixture> fixtures;
};

class SlotNavigation {
public:
    /// No runs: every step answers nothing.
    SlotNavigation() = default;

    /// Validates and indexes. Errors (InvalidArgument, the item or the position as context): an item in two slots, a
    /// level's slots not strictly increasing along it, a fixture's levels not strictly increasing in height, a
    /// position or height that is not finite.
    [[nodiscard]] static Expected<SlotNavigation> build(std::vector<NavRun> runs);

    /// The item one step from `from` towards `direction`; nothing when `from` is not in a slot or no item lies that way.
    [[nodiscard]] std::optional<PickId> step(PickId from, StepDirection direction) const noexcept;

    [[nodiscard]] bool contains(PickId id) const noexcept;
    [[nodiscard]] std::span<const NavRun> runs() const noexcept { return m_runs; }

private:
    struct Location {
        std::uint32_t run = 0;
        std::uint32_t fixture = 0;
        std::uint32_t level = 0;
        std::uint32_t slot = 0;
    };

    [[nodiscard]] std::optional<Location> locate(PickId id) const noexcept;
    [[nodiscard]] std::optional<PickId> along(const Location& from, bool right) const noexcept;
    [[nodiscard]] std::optional<PickId> across(const Location& from, bool up) const noexcept;

    std::vector<NavRun> m_runs;
    /// (item, where it stands), sorted by item.
    std::vector<std::pair<PickId, Location>> m_items;
};

} // namespace rv::world
