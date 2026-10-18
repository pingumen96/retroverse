#include "allocation.hpp"

#include <algorithm>
#include <cstddef>
#include <tuple>

#include <rv/core/random.hpp>
#include <rv/world/seeds.hpp>

namespace rv::world::detail {
namespace {

/// A level of a placed fixture: indices into the layout's fixtures and into that fixture's levels.
struct LevelRef {
    std::size_t fixture = 0;
    std::size_t level = 0;
};

/// The order of titles within a section (ADR-0010 §6).
bool alphabetical(const RoomTitle* a, const RoomTitle* b) {
    return std::tie(a->sortKey, a->titleKey, a->game) < std::tie(b->sortKey, b->titleKey, b->game);
}

std::vector<const RoomTitle*> sortedSection(std::span<const RoomTitle> titles) {
    std::vector<const RoomTitle*> sorted;
    sorted.reserve(titles.size());
    for (const RoomTitle& title : titles) {
        sorted.push_back(&title);
    }
    std::sort(sorted.begin(), sorted.end(), alphabetical);
    return sorted;
}

class Allocator {
public:
    explicit Allocator(const AllocationInput& input) : m_input(input) {}

    Allocation run() {
        const CollectionCounts& counts = m_input.counts;
        const std::span<const RoomTitle> titles = m_input.titles;
        fillFaceOut(m_input.layout.displays, titles.subspan(0, counts.featured));
        fillFaceOut(m_input.layout.bandModules, titles.subspan(counts.featured, counts.band));
        fillSpine(titles.subspan(counts.featured + counts.band, counts.spine));
        // What no title took: dressed whole (the levels of another item's slots, the face-out levels of modules outside
        // the band).
        for (std::size_t fixture = 0; fixture < m_input.layout.fixtures.size(); ++fixture) {
            const PlacedFixture& placed = m_input.layout.fixtures[fixture];
            for (std::size_t level = 0; level < placed.fixture.levels.size(); ++level) {
                if (!isHandled({fixture, level})) {
                    dress({fixture, level}, 0);
                }
            }
        }
        return std::move(m_result);
    }

private:
    const FixtureLevel& levelOf(const LevelRef& ref) const {
        return m_input.layout.fixtures[ref.fixture].fixture.levels[ref.level];
    }
    const SlotLevelInfo& infoOf(const LevelRef& ref) const {
        // fixture.levels were built from the prefab's levels in their order (fixture_layout.cpp).
        return m_input.layout.fixtures[ref.fixture].info->levels[ref.level];
    }
    LevelFacing facingOf(const LevelRef& ref) const { return m_input.layout.fixtures[ref.fixture].facings[ref.level]; }

    bool isHandled(const LevelRef& ref) const {
        return std::find_if(m_handled.begin(), m_handled.end(), [&](const LevelRef& done) {
                   return done.fixture == ref.fixture && done.level == ref.level;
               }) != m_handled.end();
    }

    /// A fixture's levels of `facing` as one reads them: the top level first (the highest slots, then the highest
    /// level number).
    std::vector<LevelRef> readingOrder(std::size_t fixture, LevelFacing facing) const {
        std::vector<LevelRef> levels;
        const PlacedFixture& placed = m_input.layout.fixtures[fixture];
        for (std::size_t level = 0; level < placed.fixture.levels.size(); ++level) {
            if (placed.facings[level] == facing) {
                levels.push_back({fixture, level});
            }
        }
        std::sort(levels.begin(), levels.end(), [&](const LevelRef& a, const LevelRef& b) {
            return std::pair(infoOf(a).bottomMm, levelOf(a).level) > std::pair(infoOf(b).bottomMm, levelOf(b).level);
        });
        return levels;
    }

    /// A module's spine-out levels in band order around the eye height (allocation.hpp).
    std::vector<LevelRef> bandOrder(std::size_t fixture) const {
        std::vector<LevelRef> levels = readingOrder(fixture, LevelFacing::SpineOut);
        std::reverse(levels.begin(), levels.end()); // lowest first
        std::vector<LevelRef> order;
        if (levels.empty()) {
            return order;
        }
        const auto distance = [&](const LevelRef& ref) {
            const std::int64_t centre = infoOf(ref).centreMm();
            return centre > m_input.eyeHeightMm ? centre - m_input.eyeHeightMm : m_input.eyeHeightMm - centre;
        };
        std::size_t nearest = 0;
        for (std::size_t i = 1; i < levels.size(); ++i) {
            if (distance(levels[i]) < distance(levels[nearest])) {
                nearest = i;
            }
        }
        order.push_back(levels[nearest]);
        std::size_t above = nearest + 1;
        std::size_t below = nearest; // the next level below is below - 1
        bool takeAbove = true;
        while (above < levels.size() || below > 0) {
            if (takeAbove && above < levels.size()) {
                order.push_back(levels[above++]);
            } else if (below > 0) {
                order.push_back(levels[--below]);
            } else {
                order.push_back(levels[above++]);
            }
            takeAbove = !takeAbove;
        }
        return order;
    }

    void place(const LevelRef& ref, std::uint32_t index, const RoomTitle& title, bool faceOut) {
        const FixtureId& fixture = m_input.layout.fixtures[ref.fixture].fixture.id;
        Placement placement;
        placement.slot = *SlotId::make(fixture, levelOf(ref).level, index);
        placement.game = title.game;
        core::Random random(placementSeed(m_input.roomSeed, title.game, m_input.family.id));
        if (faceOut) {
            placement.yawCdeg = static_cast<std::int32_t>(random.between(-kMaxFaceOutYawCdeg, kMaxFaceOutYawCdeg));
        } else {
            placement.insetMm = random.between(0, kMaxInsetMm);
        }
        m_result.placements.push_back(placement);
    }

    /// The slots of a level from `firstSlot` on hold one dressing prop.
    void dress(const LevelRef& ref, std::uint32_t firstSlot) {
        const FixtureLevel& level = levelOf(ref);
        m_handled.push_back(ref);
        if (firstSlot >= level.slots || m_input.shelfDressing.empty()) {
            return;
        }
        DressedLevel dressed;
        dressed.level = *LevelId::make(m_input.layout.fixtures[ref.fixture].fixture.id, level.level);
        dressed.firstSlot = firstSlot;
        dressed.count = level.slots - firstSlot;
        core::Random random(propSeed(m_input.roomSeed, dressed.level));
        dressed.prop = m_input.shelfDressing[static_cast<std::size_t>(random.below(m_input.shelfDressing.size()))];
        m_result.dressing.push_back(std::move(dressed));
    }

    /// Face-out titles over fixtures' face-out levels, each level whole before the next; the rest dressed.
    void fillFaceOut(const std::vector<std::size_t>& fixtures, std::span<const RoomTitle> titles) {
        const std::vector<const RoomTitle*> sorted = sortedSection(titles);
        std::size_t next = 0;
        for (const std::size_t fixture : fixtures) {
            for (const LevelRef& ref : readingOrder(fixture, LevelFacing::FaceOut)) {
                const std::uint32_t slots = levelOf(ref).slots;
                std::uint32_t index = 0;
                for (; index < slots && next < sorted.size(); ++index) {
                    place(ref, index, *sorted[next++], true);
                }
                dress(ref, index);
            }
        }
    }

    void fillSpine(std::span<const RoomTitle> titles) {
        // The levels in fill order: module after module, each in band order.
        std::vector<LevelRef> fill;
        for (const std::size_t module : m_input.layout.modules) {
            const std::vector<LevelRef> band = bandOrder(module);
            fill.insert(fill.end(), band.begin(), band.end());
        }
        const auto permille = static_cast<std::uint64_t>(std::max<std::int64_t>(0, m_input.reservePermille));
        std::vector<std::uint64_t> quotas;
        std::uint64_t largest = 0;
        for (const LevelRef& ref : fill) {
            quotas.push_back(levelOf(ref).slots * 1000 / (1000 + permille));
            largest = std::max<std::uint64_t>(largest, levelOf(ref).slots);
        }
        const auto capacity = [&](std::uint64_t rise) {
            std::uint64_t total = 0;
            for (std::size_t i = 0; i < fill.size(); ++i) {
                total += std::min<std::uint64_t>(quotas[i] + rise, levelOf(fill[i]).slots);
            }
            return total;
        };
        std::uint64_t rise = 0;
        while (rise < largest && capacity(rise) < titles.size()) {
            ++rise;
        }
        // How many titles each level takes, in fill order.
        std::vector<std::uint64_t> taken(fill.size(), 0);
        std::uint64_t remaining = titles.size();
        for (std::size_t i = 0; i < fill.size() && remaining > 0; ++i) {
            taken[i] = std::min({remaining, quotas[i] + rise, std::uint64_t{levelOf(fill[i]).slots}});
            remaining -= taken[i];
        }
        // The titles in the alphabet's order, as one reads: module after module, its used levels top first.
        const std::vector<const RoomTitle*> sorted = sortedSection(titles);
        std::size_t next = 0;
        for (const std::size_t module : m_input.layout.modules) {
            for (const LevelRef& ref : readingOrder(module, LevelFacing::SpineOut)) {
                std::uint64_t count = 0;
                for (std::size_t i = 0; i < fill.size(); ++i) {
                    if (fill[i].fixture == ref.fixture && fill[i].level == ref.level) {
                        count = taken[i];
                    }
                }
                if (count == 0) {
                    dress(ref, 0);
                    continue;
                }
                for (std::uint32_t index = 0; index < count; ++index) {
                    place(ref, index, *sorted[next++], false);
                }
                // Its last slots stay free: the reserve, where new titles go (ADR-0010 §10).
                m_handled.push_back(ref);
            }
        }
    }

    const AllocationInput& m_input;
    Allocation m_result;
    std::vector<LevelRef> m_handled;
};

} // namespace

Allocation allocate(const AllocationInput& input) {
    return Allocator(input).run();
}

} // namespace rv::world::detail
