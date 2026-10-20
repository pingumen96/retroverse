#include <rv/world/where_is.hpp>

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

namespace rv::world {

PlanIndex::PlanIndex(const WorldPlan& plan) {
    // The section of every level, by fixture and level.
    std::map<FixtureId, const Fixture*> fixtures;
    for (const Fixture& fixture : plan.fixtures) {
        fixtures.emplace(fixture.id, &fixture);
    }
    m_placed.reserve(plan.placements.size());
    m_slots.reserve(plan.placements.size());
    for (const Placement& placement : plan.placements) {
        PlacedEntry entry;
        entry.game = placement.game;
        entry.room = placement.slot.fixture().room();
        entry.place.slot = placement.slot;
        if (const auto fixture = fixtures.find(placement.slot.fixture()); fixture != fixtures.end()) {
            entry.place.role = fixture->second->role;
            entry.place.prefab = fixture->second->prefab;
            for (const FixtureLevel& level : fixture->second->levels) {
                if (level.level == placement.slot.level()) {
                    entry.place.section = level.section;
                }
            }
        }
        m_placed.push_back(std::move(entry));
        m_slots.push_back({placement.slot, placement.game});
    }
    // Ties ordered too, so that a game listed twice (or a slot holding two) answers alike whatever the lists' order.
    std::sort(m_placed.begin(), m_placed.end(), [](const PlacedEntry& a, const PlacedEntry& b) {
        return std::tie(a.game, a.place.slot) < std::tie(b.game, b.place.slot);
    });
    std::sort(m_slots.begin(), m_slots.end(), [](const SlotEntry& a, const SlotEntry& b) {
        return std::tie(a.slot, a.game) < std::tie(b.slot, b.game);
    });
    m_unplaced = plan.unplaced;
    std::sort(m_unplaced.begin(), m_unplaced.end());
    for (const Room& room : plan.rooms) {
        m_floors.emplace_back(room.id, room.floor);
    }
    std::sort(m_floors.begin(), m_floors.end());
}

std::string PlanIndex::floorOf(const RoomId& room) const {
    const auto found = std::lower_bound(m_floors.begin(), m_floors.end(), room,
                                        [](const std::pair<RoomId, std::string>& entry, const RoomId& key) {
                                            return entry.first < key;
                                        });
    return found != m_floors.end() && found->first == room ? found->second : std::string();
}

const PlanIndex::PlacedEntry* PlanIndex::placedEntry(const catalog::GameId& game) const {
    const auto found = std::lower_bound(m_placed.begin(), m_placed.end(), game,
                                        [](const PlacedEntry& entry, const catalog::GameId& key) {
                                            return entry.game < key;
                                        });
    return found != m_placed.end() && found->game == game ? &*found : nullptr;
}

std::optional<GameLocation> PlanIndex::find(const catalog::GameId& game) const {
    if (const PlacedEntry* entry = placedEntry(game)) {
        return GameLocation{game, entry->room, floorOf(entry->room), entry->place, std::nullopt, {}, std::nullopt};
    }
    const auto found = std::lower_bound(m_unplaced.begin(), m_unplaced.end(), game,
                                        [](const UnplacedGame& entry, const catalog::GameId& key) {
                                            return entry.game < key;
                                        });
    if (found == m_unplaced.end() || found->game != game) {
        return std::nullopt;
    }
    GameLocation location{game,           found->room,           floorOf(found->room), std::nullopt,
                          found->reason, found->representative, std::nullopt};
    if (found->representative.isValid()) {
        if (const PlacedEntry* representative = placedEntry(found->representative)) {
            location.representativePlace = representative->place;
        }
    }
    return location;
}

std::optional<catalog::GameId> PlanIndex::gameAt(const SlotId& slot) const {
    const auto found = std::lower_bound(m_slots.begin(), m_slots.end(), slot,
                                        [](const SlotEntry& entry, const SlotId& key) { return entry.slot < key; });
    if (found == m_slots.end() || found->slot != slot) {
        return std::nullopt;
    }
    return found->game;
}

} // namespace rv::world
