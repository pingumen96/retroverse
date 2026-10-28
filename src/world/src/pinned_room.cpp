// A room generated afresh honouring its pins (I4b step 5, ADR-0015 §3; regeneration.hpp's rules 1 to 4): the pinned
// game the representative of its title, the pinned fixtures placed first, the sockets of pinned slots and modules
// always taking their module, the pinned games reserved in their slots before the allocation. Everything else as a
// fresh room of generation.cpp's generateRoomContent() stands.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <rv/world/capacity.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/seeds.hpp>

#include "allocation.hpp"
#include "fixture_geometry.hpp"
#include "fixture_layout.hpp"
#include "generation_parts.hpp"

namespace rv::world::detail {
namespace {

using catalog::GameId;

OverrideConflict gameConflict(const GamePin& pin, ConflictReason reason, std::string message) {
    return OverrideConflict{pinText(pin), pin.game, {}, reason, std::move(message)};
}

OverrideConflict fixtureConflict(const FixturePin& pin, ConflictReason reason, std::string message) {
    return OverrideConflict{pinText(pin), {}, pin.fixture, reason, std::move(message)};
}

/// Whether a socket of `kind` holds a fixture of `role`.
bool socketTakes(SocketKind kind, FixtureRole role) noexcept {
    switch (role) {
    case FixtureRole::SideModule:
        return kind == SocketKind::SideWall;
    case FixtureRole::Display:
        return kind == SocketKind::Display;
    case FixtureRole::Decoration:
        return kind == SocketKind::SideWall || kind == SocketKind::Decoration;
    default:
        return false;
    }
}

/// The height a decoration of `prefab` stands at in a socket (the template's choice of it), or nothing.
std::optional<std::int64_t> mountHeightOf(const RoomTemplate& roomTemplate, const PrefabRef& prefab) {
    for (const DecorationChoice& choice : roomTemplate.decoration) {
        if (choice.prefab == prefab) {
            return choice.mountHeightMm;
        }
    }
    return std::nullopt;
}

/// The pinned game takes its title's place when another version represents it (rule 4); false when it cannot.
bool takeRepresentation(RoomSelection& selection, SectionIndex& index, const GameId& game,
                        const std::set<GameId>& pinnedGames, std::set<GameId>& titleGames, std::string& why) {
    const auto entry = std::find_if(selection.unplaced.begin(), selection.unplaced.end(),
                                    [&](const UnplacedGame& unplaced) { return unplaced.game == game; });
    if (entry == selection.unplaced.end()) {
        why = "the room's selection does not hold the game";
        return false;
    }
    if (entry->reason != UnplacedReason::DuplicateTitle) {
        why = "the game is unplaced (" + std::string(toString(entry->reason)) + ")";
        return false;
    }
    const GameId representative = entry->representative;
    if (!titleGames.contains(representative)) {
        why = "its title's representative " + representative.toString() + " is not a title of the room";
        return false;
    }
    if (pinnedGames.contains(representative)) {
        why = "another version of its title, " + representative.toString() + ", is pinned and represents it";
        return false;
    }
    for (RoomTitle& title : selection.titles) {
        if (title.game == representative) {
            title.game = game; // the title's place, its priority data and sort key kept
        }
    }
    const RoomId room = entry->room;
    selection.unplaced.erase(entry);
    for (UnplacedGame& other : selection.unplaced) {
        if (other.reason == UnplacedReason::DuplicateTitle && other.representative == representative) {
            other.representative = game;
        }
    }
    selection.unplaced.push_back({representative, room, UnplacedReason::DuplicateTitle, game});
    if (const auto section = index.find(representative); section != index.end()) {
        index[game] = section->second;
    }
    titleGames.erase(representative);
    titleGames.insert(game);
    return true;
}

} // namespace

Expected<WorldPlan> generatePinnedRoom(const RoomSelection& selection, const WorldConfig& config,
                                       std::uint64_t worldSeed, const WorldData& data, const SectionIndex* sections,
                                       const RoomPins& pins, RoomPinOutcome& outcome) {
    const RoomTemplate& roomTemplate = data.roomTemplate;
    const RoomId& room = selection.room;
    const std::uint64_t seedOfRoom = roomSeed(worldSeed, room);

    // The previous plan's room and fixtures.
    const Room* previousRoom = nullptr;
    std::map<FixtureId, const Fixture*> previousFixtures;
    if (pins.previous != nullptr) {
        for (const Room& candidate : pins.previous->rooms) {
            if (candidate.id == room) {
                previousRoom = &candidate;
            }
        }
        for (const Fixture& fixture : pins.previous->fixtures) {
            if (fixture.id.room() == room) {
                previousFixtures.emplace(fixture.id, &fixture);
            }
        }
    }

    // Rule 4: a pinned game is its title's representative.
    RoomSelection adjusted = selection;
    SectionIndex index = sections != nullptr ? *sections : SectionIndex{};
    std::set<GameId> titleGames;
    for (const RoomTitle& title : adjusted.titles) {
        titleGames.insert(title.game);
    }
    std::vector<GamePin> gamePins = pins.games;
    std::sort(gamePins.begin(), gamePins.end(), [](const GamePin& a, const GamePin& b) { return a.game < b.game; });
    std::set<GameId> pinnedGames;
    for (const GamePin& pin : gamePins) {
        pinnedGames.insert(pin.game);
    }
    std::vector<const GamePin*> titlePins;
    for (const GamePin& pin : gamePins) {
        std::string why;
        if (titleGames.contains(pin.game) ||
            takeRepresentation(adjusted, index, pin.game, pinnedGames, titleGames, why)) {
            titlePins.push_back(&pin);
            continue;
        }
        outcome.conflicts.push_back(gameConflict(pin, ConflictReason::GameGone, why));
    }
    std::sort(adjusted.titles.begin(), adjusted.titles.end(), priorityLess);
    std::sort(adjusted.unplaced.begin(), adjusted.unplaced.end());

    auto capacity = collectionCapacity(roomTemplate, data.prefabs, data.mediaFamily);
    if (!capacity) {
        return propagate(capacity.error());
    }
    const CollectionCounts counts =
        countCollection(*capacity, adjusted.titles.size(), config.growthReservePermille);
    std::uint32_t growth = counts.growthSteps;

    // Rule 2: the sockets of pinned slots take their module (the room grown to them).
    LayoutPins layoutPins;
    std::vector<const GamePin*> slotPins;
    for (const GamePin* pin : titlePins) {
        const FixtureId& fixture = pin->slot.fixture();
        const TemplateSocket* socket = fixture.part().empty() ? roomTemplate.socket(fixture.socket()) : nullptr;
        if (socket == nullptr || (socket->kind != SocketKind::SideWall && socket->kind != SocketKind::HeroModule &&
                                  socket->kind != SocketKind::Display)) {
            outcome.conflicts.push_back(gameConflict(*pin, ConflictReason::SlotGone,
                                                     "the room's template holds no collection socket " +
                                                         fixture.socket()));
            continue;
        }
        if (socket->kind == SocketKind::SideWall) {
            layoutPins.moduleSockets.insert(socket->id);
        }
        growth = std::max(growth, socket->growthStep);
        slotPins.push_back(pin);
    }

    // Rule 1: the pinned fixtures, before the layout fills the other sockets. Two passes: the room's growth first, from
    // the pinned slots, modules and display fixtures (whatever order their sockets sort in), then the pinned decoration
    // judged against it (a decoration never grows the room: its socket's growth step must be one the room takes).
    struct FixtureCandidate {
        const FixturePin* pin = nullptr;
        const TemplateSocket* socket = nullptr;
        PinnedFixture pinned;
    };
    std::vector<FixtureCandidate> candidates;
    for (const FixturePin& pin : pins.fixtures) {
        if (pins.excluded.contains(pin.fixture)) {
            continue;
        }
        const TemplateSocket* socket = roomTemplate.socket(pin.fixture.socket());
        if (socket == nullptr) {
            outcome.conflicts.push_back(fixtureConflict(pin, ConflictReason::SlotGone,
                                                        "the room's template holds no socket " +
                                                            pin.fixture.socket()));
            continue;
        }
        const auto standing = previousFixtures.find(pin.fixture);
        const Fixture* before = standing != previousFixtures.end() ? standing->second : nullptr;
        // A fixed fixture (the hero corner, the hero-wall modules), or one whose role the previous plan does not say,
        // is honoured only as the fresh layout stands it (judged by the regeneration afterwards).
        if (!pin.fixture.part().empty() || before == nullptr || !isMovableRole(before->role) ||
            !socketTakes(socket->kind, before->role)) {
            continue;
        }
        PinnedFixture pinned;
        pinned.role = before->role;
        pinned.transform = pin.transform;
        pinned.prefab = pin.prefab;
        if (!pinned.prefab && pinned.role == FixtureRole::Decoration) {
            pinned.prefab = before->prefab;
        }
        if (pinned.prefab) {
            const PrefabInfo* info = data.prefabs.find(*pinned.prefab);
            if (info == nullptr) {
                outcome.conflicts.push_back(fixtureConflict(
                    pin, ConflictReason::Invalid,
                    "its prefab " + pinned.prefab->manifest + " is not among the prefabs the room measures"));
                continue;
            }
            const bool inSocket = !pinned.transform || pinned.role != FixtureRole::Decoration;
            if (inSocket && !prefabFitsSocket(*info, *socket)) {
                outcome.conflicts.push_back(fixtureConflict(pin, ConflictReason::Invalid,
                                                            "its prefab " + pinned.prefab->manifest +
                                                                " does not fit the socket " + socket->id));
                continue;
            }
        }
        if (pinned.role == FixtureRole::Decoration) {
            pinned.liftMm = pinned.transform ? before->yMm
                                             : mountHeightOf(roomTemplate, *pinned.prefab).value_or(before->yMm);
        } else {
            growth = std::max(growth, socket->growthStep);
        }
        candidates.push_back({&pin, socket, std::move(pinned)});
    }
    growth = std::min(growth, roomTemplate.maxGrowthSteps());
    std::vector<const FixturePin*> appliedFixtures;
    for (FixtureCandidate& candidate : candidates) {
        if (candidate.pinned.role == FixtureRole::Decoration && candidate.socket->growthStep > growth) {
            outcome.conflicts.push_back(fixtureConflict(*candidate.pin, ConflictReason::SlotGone,
                                                        "its socket " + candidate.socket->id +
                                                            " comes with a growth step the room no longer takes"));
            continue;
        }
        layoutPins.fixtures.emplace(candidate.socket->id, std::move(candidate.pinned));
        appliedFixtures.push_back(candidate.pin);
    }

    // The layout at `steps` of growth: a hung decoration whose back stood on the south wall follows the wall to the
    // room's new depth (ADR-0015 §3 as amended: as an update's growth moves it), its pin's transform rewritten.
    std::map<FixtureId, FixtureTransform> followed;
    const auto layOutAt = [&](std::uint32_t steps) -> Expected<RoomLayout> {
        followed.clear();
        LayoutPins grownPins = layoutPins;
        const Footprint interior = interiorFootprint(roomTemplate, steps);
        for (const FixturePin* pin : appliedFixtures) {
            PinnedFixture& pinned = grownPins.fixtures.at(pin->fixture.socket());
            const auto standing = previousFixtures.find(pin->fixture);
            if (!pinned.transform || previousRoom == nullptr || standing == previousFixtures.end() ||
                pinned.role != FixtureRole::Decoration) {
                continue;
            }
            Fixture atPin = *standing->second;
            atPin.xMm = pinned.transform->xMm;
            atPin.zMm = pinned.transform->zMm;
            atPin.yawCdeg = pinned.transform->yawCdeg;
            const auto mount = wallMountOf(atPin, *previousRoom);
            const std::int64_t shift = interior.maxZMm - previousRoom->maxZMm;
            if (shift != 0 && isHungDecoration(atPin) && hangsOnWall(atPin, *previousRoom) && mount &&
                mount->wall == Wall::South) {
                pinned.transform->zMm += shift;
                followed[pin->fixture] = *pinned.transform;
            }
        }
        CollectionCounts grown = counts;
        grown.growthSteps = steps;
        return layOutRoom({roomTemplate, data.platform, data.mediaFamily, data.prefabs, room, seedOfRoom, grown,
                           config.decorationDensityPermille, grownPins.empty() ? nullptr : &grownPins});
    };
    // The side-wall sockets the pins take or block (a pinned decoration's, those a moved pinned fixture overlaps) send
    // their modules to the next sockets: the room grows, within the template's limit, until the modules the count
    // needs stand (a room without pins places them at the count's growth).
    auto layout = layOutAt(growth);
    const auto sideModulesOf = [](const RoomLayout& laid) {
        std::uint32_t modules = 0;
        for (const std::size_t index : laid.modules) {
            modules += laid.fixtures[index].fixture.role == FixtureRole::SideModule ? 1 : 0;
        }
        return modules;
    };
    while (layout && sideModulesOf(*layout) < counts.sideModules && growth < roomTemplate.maxGrowthSteps()) {
        ++growth;
        layout = layOutAt(growth);
    }
    if (!layout) {
        return propagate(layout.error());
    }
    for (const auto& [fixture, transform] : followed) {
        outcome.followed[fixture] = transform;
    }
    CollectionCounts grown = counts;
    grown.growthSteps = growth;

    // Rule 3: the pinned games reserved in their slots (a slot of the layout's, on a level of the family's items).
    std::map<FixtureId, const PlacedFixture*> placedFixtures;
    for (const PlacedFixture& placed : layout->fixtures) {
        placedFixtures.emplace(placed.fixture.id, &placed);
    }
    std::map<SlotId, GameId> reserved;
    std::set<GameId> reservedGames;
    for (const GamePin* pin : slotPins) {
        const auto placed = placedFixtures.find(pin->slot.fixture());
        bool holds = false;
        if (placed != placedFixtures.end()) {
            const PlacedFixture& fixture = *placed->second;
            for (std::size_t level = 0; level < fixture.fixture.levels.size(); ++level) {
                holds = holds || (fixture.fixture.levels[level].level == pin->slot.level() &&
                                  pin->slot.index() < fixture.fixture.levels[level].slots &&
                                  level < fixture.facings.size() && fixture.facings[level] != LevelFacing::Other);
            }
        }
        if (!holds) {
            outcome.conflicts.push_back(gameConflict(*pin, ConflictReason::SlotGone,
                                                     "the new layout holds no such slot for the room's items"));
            continue;
        }
        reserved.emplace(pin->slot, pin->game);
        reservedGames.insert(pin->game);
    }

    // The titles placed: the pinned ones, then the others by priority up to the count.
    std::vector<RoomTitle> rest;
    for (const RoomTitle& title : adjusted.titles) {
        if (!reservedGames.contains(title.game)) {
            rest.push_back(title);
        }
    }
    const std::uint64_t placedRest = counts.placed - std::min<std::uint64_t>(counts.placed, reservedGames.size());
    // The face-out titles fill the face-out slots the pins leave (the layout's own, since a pinned display may stand
    // as another prefab).
    const auto freeFaceOut = [&](const std::vector<std::size_t>& fixtures) {
        std::uint64_t slots = 0;
        for (const std::size_t index : fixtures) {
            const PlacedFixture& placed = layout->fixtures[index];
            for (std::size_t level = 0; level < placed.fixture.levels.size(); ++level) {
                if (placed.facings[level] != LevelFacing::FaceOut) {
                    continue;
                }
                slots += placed.fixture.levels[level].slots;
                for (const auto& [slot, game] : reserved) {
                    slots -= slot.fixture() == placed.fixture.id && slot.level() == placed.fixture.levels[level].level
                                 ? 1
                                 : 0;
                }
            }
        }
        return slots;
    };
    CollectionCounts allocation = grown;
    allocation.placed = std::min<std::uint64_t>(placedRest, rest.size());
    allocation.featured = std::min(allocation.placed, freeFaceOut(layout->displays));
    allocation.band = std::min(allocation.placed - allocation.featured, freeFaceOut(layout->bandModules));
    allocation.spine = allocation.placed - allocation.featured - allocation.band;
    Allocation allocated = allocate({*layout, std::span<const RoomTitle>(rest).subspan(0, allocation.placed),
                                     allocation, config.growthReservePermille, roomTemplate.eyeHeightMm,
                                     data.mediaFamily, seedOfRoom, roomTemplate.shelfDressing,
                                     sections != nullptr ? &index : nullptr, reserved.empty() ? nullptr : &reserved});

    WorldPlan plan;
    plan.rooms.push_back(layout->room);
    for (PlacedFixture& placed : layout->fixtures) {
        plan.fixtures.push_back(std::move(placed.fixture));
    }
    plan.placements = std::move(allocated.placements);
    plan.dressing = std::move(allocated.dressing);
    plan.lights = std::move(layout->lights);
    plan.unplaced = adjusted.unplaced;
    for (std::size_t i = allocation.placed; i < rest.size(); ++i) {
        plan.unplaced.push_back({rest[i].game, room, UnplacedReason::Capacity, {}});
    }
    for (const GameId& game : allocated.overflow) {
        plan.unplaced.push_back({game, room, UnplacedReason::Capacity, {}});
    }
    return plan;
}

} // namespace rv::world::detail
