#pragma once

// The navigation grid of a room's validation (ADR-0010 §11): square cells of kNavCellMm over the room's floor, a cell
// free when its centre lies at least `clearanceMm` from every obstacle and from the walls (the obstacles dilated by
// the clearance: half the clear width a passage must keep, 600 mm for the 1.2 m minimum), searched breadth-first from
// the doorway. A cell reached by the search is a place where a body of the clear width can stand after walking in, so
// a fixture whose interaction point has a reached cell near it can be walked to.
//
// Integers only, in millimetres in the room's frame (world_plan.hpp): cell (column, row) covers x from area.minXMm +
// column * kNavCellMm and z from area.minZMm + row * kNavCellMm, kNavCellMm wide; its centre is half a cell further.
// Distances are Euclidean, from the cell's centre to the nearest point of an obstacle's rectangle, compared squared,
// so the grid is the same in every build. Neighbours are the four sharing an edge: a diagonal step never slips between
// two obstacles that touch at a corner.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <rv/world/room_template.hpp>

namespace rv::world {

/// The side of a cell, 0.1 m (ADR-0010 §11).
inline constexpr std::int64_t kNavCellMm = 100;

/// A cell of the grid.
struct NavCell {
    std::int32_t column = 0;
    std::int32_t row = 0;

    auto operator<=>(const NavCell&) const = default;
};

class NavGrid {
public:
    NavGrid() = default;

    /// The grid over `area` (the room's interior; a length that is not a whole number of cells leaves its last partial
    /// cell out), each cell blocked when its centre lies closer than `clearanceMm` to one of `obstacles` (rectangles on
    /// the floor, possibly reaching outside the area) or to the area's edge (the walls). An empty or inverted area
    /// gives a grid without cells; a negative clearance counts as 0.
    NavGrid(const Footprint& area, std::span<const Footprint> obstacles, std::int64_t clearanceMm);

    [[nodiscard]] std::int32_t columns() const noexcept { return m_columns; }
    [[nodiscard]] std::int32_t rows() const noexcept { return m_rows; }
    [[nodiscard]] const Footprint& area() const noexcept { return m_area; }
    [[nodiscard]] std::int64_t clearanceMm() const noexcept { return m_clearanceMm; }

    /// True for a cell of the grid.
    [[nodiscard]] bool contains(NavCell cell) const noexcept;
    /// True for a cell of the grid that no obstacle or wall blocks.
    [[nodiscard]] bool isFree(NavCell cell) const noexcept;
    /// The cell holding the point (x, z), a point on an edge in the cell after it; nothing outside the grid.
    [[nodiscard]] std::optional<NavCell> cellAt(std::int64_t xMm, std::int64_t zMm) const noexcept;
    /// The centre of a cell (also for a cell outside the grid).
    [[nodiscard]] std::int64_t centreXMm(NavCell cell) const noexcept;
    [[nodiscard]] std::int64_t centreZMm(NavCell cell) const noexcept;

    /// How many cells are free.
    [[nodiscard]] std::size_t freeCount() const noexcept;

private:
    [[nodiscard]] std::size_t indexOf(NavCell cell) const noexcept;

    Footprint m_area;
    std::int64_t m_clearanceMm = 0;
    std::int32_t m_columns = 0;
    std::int32_t m_rows = 0;
    /// 1 for a free cell, row by row.
    std::vector<std::uint8_t> m_free;

    friend class NavReach;
};

/// The cells a breadth-first search from one cell reaches through free cells (ADR-0010 §11: from the doorway). It
/// refers to its grid, which must outlive it.
class NavReach {
public:
    NavReach() = default;

    /// Searches `grid` from `start`; nothing is reached when `start` is outside the grid or blocked.
    NavReach(const NavGrid& grid, NavCell start);

    [[nodiscard]] bool reached(NavCell cell) const noexcept;
    /// How many cells were reached (the start included).
    [[nodiscard]] std::size_t reachedCount() const noexcept { return m_count; }
    /// True when a reached cell's centre lies within `radiusMm` of the point (x, z), the distance compared squared.
    [[nodiscard]] bool reachesNear(std::int64_t xMm, std::int64_t zMm, std::int64_t radiusMm) const noexcept;

private:
    const NavGrid* m_grid = nullptr;
    std::vector<std::uint8_t> m_reached;
    std::size_t m_count = 0;
};

} // namespace rv::world
