#include <rv/world/nav_grid.hpp>

#include <algorithm>
#include <deque>

namespace rv::world {
namespace {

constexpr std::int64_t kHalfCellMm = kNavCellMm / 2;

/// Division rounding towards negative infinity, and towards positive infinity (the divisor positive).
std::int64_t floorDiv(std::int64_t value, std::int64_t divisor) noexcept {
    const std::int64_t quotient = value / divisor;
    return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
}
std::int64_t ceilDiv(std::int64_t value, std::int64_t divisor) noexcept {
    const std::int64_t quotient = value / divisor;
    return (value % divisor != 0 && value > 0) ? quotient + 1 : quotient;
}

/// The distance along one axis from a coordinate to an interval, 0 inside it.
std::int64_t gap(std::int64_t value, std::int64_t low, std::int64_t high) noexcept {
    if (value < low) {
        return low - value;
    }
    return value > high ? value - high : 0;
}

/// The cells whose centres lie in [low, high] along an axis that starts at `origin` and holds `count` cells, clamped;
/// first > last when there are none.
struct CellRange {
    std::int64_t first = 0;
    std::int64_t last = -1;
};
CellRange cellsWithCentresIn(std::int64_t low, std::int64_t high, std::int64_t origin, std::int32_t count) noexcept {
    CellRange range;
    range.first = std::max<std::int64_t>(0, ceilDiv(low - origin - kHalfCellMm, kNavCellMm));
    range.last = std::min<std::int64_t>(count - 1, floorDiv(high - origin - kHalfCellMm, kNavCellMm));
    return range;
}

} // namespace

NavGrid::NavGrid(const Footprint& area, std::span<const Footprint> obstacles, std::int64_t clearanceMm)
    : m_area(area), m_clearanceMm(std::max<std::int64_t>(0, clearanceMm)) {
    if (area.maxXMm <= area.minXMm || area.maxZMm <= area.minZMm) {
        return;
    }
    // A room is metres across: the counts fit easily, and a degenerate area larger than that is refused as empty.
    const std::int64_t columns = (area.maxXMm - area.minXMm) / kNavCellMm;
    const std::int64_t rows = (area.maxZMm - area.minZMm) / kNavCellMm;
    constexpr std::int64_t kMaxCellsPerAxis = 100'000;
    if (columns <= 0 || rows <= 0 || columns > kMaxCellsPerAxis || rows > kMaxCellsPerAxis) {
        return;
    }
    m_columns = static_cast<std::int32_t>(columns);
    m_rows = static_cast<std::int32_t>(rows);
    m_free.assign(static_cast<std::size_t>(columns * rows), 1);

    const std::int64_t clearance = m_clearanceMm;
    const std::int64_t clearanceSquared = clearance * clearance;
    // The walls: a centre closer than the clearance to the area's edge.
    for (std::int32_t row = 0; row < m_rows; ++row) {
        for (std::int32_t column = 0; column < m_columns; ++column) {
            const NavCell cell{column, row};
            const std::int64_t x = centreXMm(cell);
            const std::int64_t z = centreZMm(cell);
            const std::int64_t toWall = std::min({x - area.minXMm, area.maxXMm - x, z - area.minZMm, area.maxZMm - z});
            if (toWall < clearance) {
                m_free[indexOf(cell)] = 0;
            }
        }
    }
    // Each obstacle blocks only the cells around it, so the grid costs its cells plus the obstacles' dilated areas.
    for (const Footprint& obstacle : obstacles) {
        if (obstacle.maxXMm < obstacle.minXMm || obstacle.maxZMm < obstacle.minZMm) {
            continue;
        }
        const CellRange columnRange =
            cellsWithCentresIn(obstacle.minXMm - clearance, obstacle.maxXMm + clearance, area.minXMm, m_columns);
        const CellRange rowRange =
            cellsWithCentresIn(obstacle.minZMm - clearance, obstacle.maxZMm + clearance, area.minZMm, m_rows);
        for (std::int64_t row = rowRange.first; row <= rowRange.last; ++row) {
            for (std::int64_t column = columnRange.first; column <= columnRange.last; ++column) {
                const NavCell cell{static_cast<std::int32_t>(column), static_cast<std::int32_t>(row)};
                const std::int64_t dx = gap(centreXMm(cell), obstacle.minXMm, obstacle.maxXMm);
                const std::int64_t dz = gap(centreZMm(cell), obstacle.minZMm, obstacle.maxZMm);
                if (dx * dx + dz * dz < clearanceSquared || (clearance == 0 && dx == 0 && dz == 0)) {
                    m_free[indexOf(cell)] = 0;
                }
            }
        }
    }
}

bool NavGrid::contains(NavCell cell) const noexcept {
    return cell.column >= 0 && cell.row >= 0 && cell.column < m_columns && cell.row < m_rows;
}

bool NavGrid::isFree(NavCell cell) const noexcept {
    return contains(cell) && m_free[indexOf(cell)] != 0;
}

std::optional<NavCell> NavGrid::cellAt(std::int64_t xMm, std::int64_t zMm) const noexcept {
    if (xMm < m_area.minXMm || zMm < m_area.minZMm) {
        return std::nullopt;
    }
    const std::int64_t column = (xMm - m_area.minXMm) / kNavCellMm;
    const std::int64_t row = (zMm - m_area.minZMm) / kNavCellMm;
    if (column >= m_columns || row >= m_rows) {
        return std::nullopt;
    }
    return NavCell{static_cast<std::int32_t>(column), static_cast<std::int32_t>(row)};
}

std::int64_t NavGrid::centreXMm(NavCell cell) const noexcept {
    return m_area.minXMm + static_cast<std::int64_t>(cell.column) * kNavCellMm + kHalfCellMm;
}

std::int64_t NavGrid::centreZMm(NavCell cell) const noexcept {
    return m_area.minZMm + static_cast<std::int64_t>(cell.row) * kNavCellMm + kHalfCellMm;
}

std::size_t NavGrid::freeCount() const noexcept {
    return static_cast<std::size_t>(std::count(m_free.begin(), m_free.end(), std::uint8_t{1}));
}

std::size_t NavGrid::indexOf(NavCell cell) const noexcept {
    return static_cast<std::size_t>(cell.row) * static_cast<std::size_t>(m_columns) +
           static_cast<std::size_t>(cell.column);
}

NavReach::NavReach(const NavGrid& grid, NavCell start) : m_grid(&grid), m_reached(grid.m_free.size(), 0) {
    if (!grid.isFree(start)) {
        return;
    }
    std::deque<NavCell> queue;
    m_reached[grid.indexOf(start)] = 1;
    m_count = 1;
    queue.push_back(start);
    constexpr NavCell kSteps[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    while (!queue.empty()) {
        const NavCell cell = queue.front();
        queue.pop_front();
        for (const NavCell& step : kSteps) {
            const NavCell next{cell.column + step.column, cell.row + step.row};
            if (!grid.isFree(next)) {
                continue;
            }
            std::uint8_t& reached = m_reached[grid.indexOf(next)];
            if (reached == 0) {
                reached = 1;
                ++m_count;
                queue.push_back(next);
            }
        }
    }
}

bool NavReach::reached(NavCell cell) const noexcept {
    return m_grid != nullptr && m_grid->contains(cell) && m_reached[m_grid->indexOf(cell)] != 0;
}

bool NavReach::reachesNear(std::int64_t xMm, std::int64_t zMm, std::int64_t radiusMm) const noexcept {
    if (m_grid == nullptr || m_count == 0 || radiusMm < 0) {
        return false;
    }
    const Footprint& area = m_grid->area();
    const CellRange columns = cellsWithCentresIn(xMm - radiusMm, xMm + radiusMm, area.minXMm, m_grid->columns());
    const CellRange rows = cellsWithCentresIn(zMm - radiusMm, zMm + radiusMm, area.minZMm, m_grid->rows());
    const std::int64_t radiusSquared = radiusMm * radiusMm;
    for (std::int64_t row = rows.first; row <= rows.last; ++row) {
        for (std::int64_t column = columns.first; column <= columns.last; ++column) {
            const NavCell cell{static_cast<std::int32_t>(column), static_cast<std::int32_t>(row)};
            const std::int64_t dx = m_grid->centreXMm(cell) - xMm;
            const std::int64_t dz = m_grid->centreZMm(cell) - zMm;
            if (dx * dx + dz * dz <= radiusSquared && reached(cell)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace rv::world
