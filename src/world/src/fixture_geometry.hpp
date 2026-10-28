#pragma once

// Private to rv_world (I4b step 2): the validation's geometry of fixtures (validation.hpp's header: a fixture's box,
// rounded outwards to half millimetres, floor coverings walked over), shared with the incremental update, which skips a
// socket whose new fixture would overlap a fixture the user moved there (ADR-0015 §3), and where a hung decoration
// hangs, which the validation, the edit and the update's growth judge alike. Defined in validation.cpp.

#include <cstdint>
#include <optional>

#include <rv/world/room_template.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world::detail {

/// Whether the boxes of `a` and `b` overlap as the validation's footprints-overlap rule judges them (boxes that touch do
/// not; a floor covering overlaps nothing).
[[nodiscard]] bool fixturesOverlap(const Fixture& a, const Fixture& b) noexcept;

/// A decoration hung on a wall: one above the floor (a poster).
[[nodiscard]] bool isHungDecoration(const Fixture& fixture) noexcept;

/// The wall a fixture faces into the room from by its yaw (a quarter turn: 0 the north wall, 9000 the west, 18000 the
/// south, 27000 the east) and how far its box's back stands in front of that wall's interior face in `room`, in half
/// millimetres (negative: through the wall). Nothing for a yaw that is not a quarter turn.
struct WallMount {
    Wall wall = Wall::None;
    std::int64_t gapHalfMm = 0;
};
[[nodiscard]] std::optional<WallMount> wallMountOf(const Fixture& fixture, const Room& room) noexcept;

/// Whether `fixture` hangs on a wall of `room` (ADR-0015 §4): wallMountOf() names one, its back 0 to kMaxWallGapMm in
/// front of it (the edit's rule wall-mount, and the growth's).
[[nodiscard]] bool hangsOnWall(const Fixture& fixture, const Room& room) noexcept;

/// The wall of `room` nearest to `fixture`'s box and the gap between them, in half millimetres (negative: through it):
/// what the validation's decoration-off-wall judges, whatever the yaw (the scene's drawn fixtures are boxes, turned 0).
[[nodiscard]] WallMount nearestWallOf(const Fixture& fixture, const Room& room) noexcept;

} // namespace rv::world::detail
