#include "allocation.hpp"

#include <algorithm>
#include <cstddef>
#include <tuple>
#include <utility>

#include <rv/core/random.hpp>
#include <rv/world/seeds.hpp>

namespace rv::world::detail {
namespace {

/// A level of a placed fixture: indices into the layout's fixtures and into that fixture's levels.
struct LevelRef {
    std::size_t fixture = 0;
    std::size_t level = 0;
};

/// The titles of a section of the collection in their order (ADR-0010 §6; a shared or arcade room's by its systems'
/// sections first, ADR-0013 §6).
std::vector<const RoomTitle*> sortedSection(std::span<const RoomTitle> titles, const SectionIndex* sections) {
    std::vector<const RoomTitle*> sorted;
    sorted.reserve(titles.size());
    for (const RoomTitle& title : titles) {
        sorted.push_back(&title);
    }
    std::sort(sorted.begin(), sorted.end(), [&](const RoomTitle* a, const RoomTitle* b) {
        const std::uint32_t sectionA = sectionOf(sections, a->game);
        const std::uint32_t sectionB = sectionOf(sections, b->game);
        return std::tie(sectionA, a->sortKey, a->titleKey, a->game) <
               std::tie(sectionB, b->sortKey, b->titleKey, b->game);
    });
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
        const PlacedFixture& placed = m_input.layout.fixtures[fixture];
        std::vector<LevelRef> order;
        for (const std::size_t level : detail::bandOrder(placed.info->levels, placed.facings, m_input.eyeHeightMm)) {
            order.push_back({fixture, level});
        }
        return order;
    }

    void place(const LevelRef& ref, std::uint32_t index, const RoomTitle& title, bool faceOut) {
        const FixtureId& fixture = m_input.layout.fixtures[ref.fixture].fixture.id;
        Placement placement;
        placement.slot = *SlotId::make(fixture, levelOf(ref).level, index);
        placement.game = title.game;
        setPlacementOffset(placement, m_input.roomSeed, m_input.family.id, faceOut);
        m_result.placements.push_back(placement);
    }

    /// The slots of a level from `firstSlot` on hold one dressing prop.
    void dress(const LevelRef& ref, std::uint32_t firstSlot) {
        m_handled.push_back(ref);
        dressSpan(ref, firstSlot, levelOf(ref).slots - std::min(firstSlot, levelOf(ref).slots));
    }

    /// `count` slots of a level from `firstSlot` hold one dressing prop (the level's prop: every span of a level
    /// shows the same one).
    void dressSpan(const LevelRef& ref, std::uint32_t firstSlot, std::uint32_t count) {
        const FixtureLevel& level = levelOf(ref);
        if (count == 0 || firstSlot >= level.slots || m_input.shelfDressing.empty()) {
            return;
        }
        DressedLevel dressed;
        dressed.level = *LevelId::make(m_input.layout.fixtures[ref.fixture].fixture.id, level.level);
        dressed.firstSlot = firstSlot;
        dressed.count = std::min(count, level.slots - firstSlot);
        dressed.prop = dressingProp(m_input.roomSeed, dressed.level, m_input.shelfDressing);
        m_result.dressing.push_back(std::move(dressed));
    }

    /// Face-out titles over fixtures' face-out levels, each level whole before the next; the rest dressed. A level the
    /// titles fill only partly holds them in its middle (generator 1.2: a lone title stands at the centre of its
    /// ledge, framed by the dressing on both sides, instead of at its end), the first of them left of the centre when
    /// the free slots do not split evenly.
    void fillFaceOut(const std::vector<std::size_t>& fixtures, std::span<const RoomTitle> titles) {
        const std::vector<const RoomTitle*> sorted = sortedSection(titles, m_input.sections);
        std::size_t next = 0;
        for (const std::size_t fixture : fixtures) {
            for (const LevelRef& ref : readingOrder(fixture, LevelFacing::FaceOut)) {
                const std::uint32_t slots = levelOf(ref).slots;
                const auto count =
                    static_cast<std::uint32_t>(std::min<std::size_t>(slots, sorted.size() - next));
                if (count == 0 || count == slots) {
                    for (std::uint32_t index = 0; index < count; ++index) {
                        place(ref, index, *sorted[next++], true);
                    }
                    dress(ref, count);
                    continue;
                }
                const std::uint32_t first = (slots - count) / 2;
                for (std::uint32_t index = first; index < first + count; ++index) {
                    place(ref, index, *sorted[next++], true);
                }
                m_handled.push_back(ref);
                dressSpan(ref, 0, first);
                dressSpan(ref, first + count, slots - first - count);
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
        const std::vector<const RoomTitle*> sorted = sortedSection(titles, m_input.sections);
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

std::uint32_t sectionOf(const SectionIndex* sections, const catalog::GameId& game) noexcept {
    if (sections == nullptr) {
        return 0;
    }
    const auto found = sections->find(game);
    return found != sections->end() ? found->second : 0;
}

std::vector<std::size_t> bandOrder(std::span<const SlotLevelInfo> levels, std::span<const LevelFacing> facings,
                                   std::int64_t eyeHeightMm) {
    // The spine-out levels from the lowest (the lowest slots, then the lowest level number).
    std::vector<std::size_t> rising;
    for (std::size_t i = 0; i < levels.size() && i < facings.size(); ++i) {
        if (facings[i] == LevelFacing::SpineOut) {
            rising.push_back(i);
        }
    }
    std::sort(rising.begin(), rising.end(), [&](std::size_t a, std::size_t b) {
        return std::pair(levels[a].bottomMm, levels[a].level) < std::pair(levels[b].bottomMm, levels[b].level);
    });
    std::vector<std::size_t> order;
    if (rising.empty()) {
        return order;
    }
    const auto distance = [&](std::size_t index) {
        const std::int64_t centre = levels[index].centreMm();
        return centre > eyeHeightMm ? centre - eyeHeightMm : eyeHeightMm - centre;
    };
    std::size_t nearest = 0;
    for (std::size_t i = 1; i < rising.size(); ++i) {
        if (distance(rising[i]) < distance(rising[nearest])) {
            nearest = i;
        }
    }
    order.push_back(rising[nearest]);
    std::size_t above = nearest + 1;
    std::size_t below = nearest; // the next level below is below - 1
    bool takeAbove = true;
    while (above < rising.size() || below > 0) {
        if (takeAbove && above < rising.size()) {
            order.push_back(rising[above++]);
        } else if (below > 0) {
            order.push_back(rising[--below]);
        } else {
            order.push_back(rising[above++]);
        }
        takeAbove = !takeAbove;
    }
    return order;
}

PrefabRef dressingProp(std::uint64_t roomSeed, const LevelId& level, std::span<const PrefabRef> shelfDressing) {
    if (shelfDressing.empty()) {
        return {};
    }
    core::Random random(propSeed(roomSeed, level));
    return shelfDressing[static_cast<std::size_t>(random.below(shelfDressing.size()))];
}

void setPlacementOffset(Placement& placement, std::uint64_t roomSeed, std::string_view mediaFamily, bool faceOut) {
    core::Random random(placementSeed(roomSeed, placement.game, mediaFamily));
    placement.insetMm = 0;
    placement.yawCdeg = 0;
    if (faceOut) {
        placement.yawCdeg = static_cast<std::int32_t>(random.between(-kMaxFaceOutYawCdeg, kMaxFaceOutYawCdeg));
    } else {
        placement.insetMm = random.between(0, kMaxInsetMm);
    }
}

Allocation allocate(const AllocationInput& input) {
    return Allocator(input).run();
}

} // namespace rv::world::detail
