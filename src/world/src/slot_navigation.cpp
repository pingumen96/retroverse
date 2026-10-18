#include <rv/world/slot_navigation.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include <fmt/format.h>

namespace rv::world {

namespace {

[[nodiscard]] bool hasItem(const NavLevel& level) noexcept {
    return std::any_of(level.slots.begin(), level.slots.end(),
                       [](const NavSlot& slot) { return slot.item != kNoPick; });
}

/// The item at the left end (`fromLeft`) or the right end of `level`, if any.
[[nodiscard]] std::optional<PickId> endItem(const NavLevel& level, bool fromLeft) noexcept {
    if (fromLeft) {
        for (const NavSlot& slot : level.slots) {
            if (slot.item != kNoPick) {
                return slot.item;
            }
        }
    } else {
        for (auto slot = level.slots.rbegin(); slot != level.slots.rend(); ++slot) {
            if (slot->item != kNoPick) {
                return slot->item;
            }
        }
    }
    return std::nullopt;
}

} // namespace

Expected<SlotNavigation> SlotNavigation::build(std::vector<NavRun> runs) {
    SlotNavigation navigation;
    for (std::size_t r = 0; r < runs.size(); ++r) {
        for (std::size_t f = 0; f < runs[r].fixtures.size(); ++f) {
            const NavFixture& fixture = runs[r].fixtures[f];
            for (std::size_t l = 0; l < fixture.levels.size(); ++l) {
                const NavLevel& level = fixture.levels[l];
                const std::string where = fmt::format("run {} fixture {} level {}", r, f, l);
                if (!std::isfinite(level.height)) {
                    return makeError(ErrorCode::InvalidArgument, "a level's height is not finite", where);
                }
                if (l > 0 && !(level.height > fixture.levels[l - 1].height)) {
                    return makeError(ErrorCode::InvalidArgument, "a fixture's levels must rise strictly", where);
                }
                for (std::size_t s = 0; s < level.slots.size(); ++s) {
                    const NavSlot& slot = level.slots[s];
                    if (!std::isfinite(slot.along)) {
                        return makeError(ErrorCode::InvalidArgument, "a slot's position is not finite",
                                         fmt::format("{} slot {}", where, s));
                    }
                    if (s > 0 && !(slot.along > level.slots[s - 1].along)) {
                        return makeError(ErrorCode::InvalidArgument, "a level's slots must go strictly to the right",
                                         fmt::format("{} slot {}", where, s));
                    }
                    if (slot.item != kNoPick) {
                        navigation.m_items.emplace_back(
                            slot.item, Location{static_cast<std::uint32_t>(r), static_cast<std::uint32_t>(f),
                                                static_cast<std::uint32_t>(l), static_cast<std::uint32_t>(s)});
                    }
                }
            }
        }
    }
    std::sort(navigation.m_items.begin(), navigation.m_items.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    const auto twice = std::adjacent_find(navigation.m_items.begin(), navigation.m_items.end(),
                                          [](const auto& a, const auto& b) { return a.first == b.first; });
    if (twice != navigation.m_items.end()) {
        return makeError(ErrorCode::InvalidArgument, "an item stands in two slots", fmt::format("item {}", twice->first));
    }
    navigation.m_runs = std::move(runs);
    return navigation;
}

std::optional<SlotNavigation::Location> SlotNavigation::locate(PickId id) const noexcept {
    if (id == kNoPick) {
        return std::nullopt;
    }
    const auto found = std::lower_bound(m_items.begin(), m_items.end(), id,
                                        [](const auto& pair, PickId value) { return pair.first < value; });
    if (found == m_items.end() || found->first != id) {
        return std::nullopt;
    }
    return found->second;
}

bool SlotNavigation::contains(PickId id) const noexcept {
    return locate(id).has_value();
}

std::optional<PickId> SlotNavigation::along(const Location& from, bool right) const noexcept {
    const NavRun& run = m_runs[from.run];
    const NavLevel& level = run.fixtures[from.fixture].levels[from.level];
    // The same level first.
    if (right) {
        for (std::size_t s = from.slot + 1; s < level.slots.size(); ++s) {
            if (level.slots[s].item != kNoPick) {
                return level.slots[s].item;
            }
        }
    } else {
        for (std::size_t s = from.slot; s-- > 0;) {
            if (level.slots[s].item != kNoPick) {
                return level.slots[s].item;
            }
        }
    }
    // Then the next fixtures that way, entered at their near end on the level nearest in height.
    const float height = level.height;
    const auto crossInto = [&](const NavFixture& fixture) -> std::optional<PickId> {
        const NavLevel* best = nullptr;
        float bestDistance = std::numeric_limits<float>::infinity();
        for (const NavLevel& candidate : fixture.levels) {
            const float distance = std::fabs(candidate.height - height);
            if (hasItem(candidate) && distance < bestDistance) {
                best = &candidate;
                bestDistance = distance;
            }
        }
        return best ? endItem(*best, right) : std::nullopt;
    };
    if (right) {
        for (std::size_t f = from.fixture + 1; f < run.fixtures.size(); ++f) {
            if (const auto item = crossInto(run.fixtures[f])) {
                return item;
            }
        }
    } else {
        for (std::size_t f = from.fixture; f-- > 0;) {
            if (const auto item = crossInto(run.fixtures[f])) {
                return item;
            }
        }
    }
    return std::nullopt;
}

std::optional<PickId> SlotNavigation::across(const Location& from, bool up) const noexcept {
    const NavFixture& fixture = m_runs[from.run].fixtures[from.fixture];
    const float position = fixture.levels[from.level].slots[from.slot].along;
    const auto nearestOn = [&](const NavLevel& level) -> std::optional<PickId> {
        std::optional<PickId> best;
        float bestDistance = std::numeric_limits<float>::infinity();
        // Left to right: a tie keeps the left one.
        for (const NavSlot& slot : level.slots) {
            const float distance = std::fabs(slot.along - position);
            if (slot.item != kNoPick && distance < bestDistance) {
                best = slot.item;
                bestDistance = distance;
            }
        }
        return best;
    };
    if (up) {
        for (std::size_t l = from.level + 1; l < fixture.levels.size(); ++l) {
            if (const auto item = nearestOn(fixture.levels[l])) {
                return item;
            }
        }
    } else {
        for (std::size_t l = from.level; l-- > 0;) {
            if (const auto item = nearestOn(fixture.levels[l])) {
                return item;
            }
        }
    }
    return std::nullopt;
}

std::optional<PickId> SlotNavigation::step(PickId from, StepDirection direction) const noexcept {
    const auto location = locate(from);
    if (!location) {
        return std::nullopt;
    }
    switch (direction) {
    case StepDirection::Left:
        return along(*location, false);
    case StepDirection::Right:
        return along(*location, true);
    case StepDirection::Up:
        return across(*location, true);
    case StepDirection::Down:
        return across(*location, false);
    }
    return std::nullopt;
}

} // namespace rv::world
