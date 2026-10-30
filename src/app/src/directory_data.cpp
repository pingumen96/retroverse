#include <rv/app/directory_data.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <map>
#include <tuple>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/room_items.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/world/building.hpp>
#include <rv/world/title_key.hpp>

namespace rv::app {
namespace {

/// The systems a room holds: its sections, else its id's system.
[[nodiscard]] std::vector<catalog::SystemName> systemsOf(const world::Room& room) {
    if (!room.sections.empty()) {
        return room.sections;
    }
    return {room.id.system()};
}

/// The placements of `plan` per room.
[[nodiscard]] std::map<world::RoomId, std::uint32_t> placedPerRoom(const world::WorldPlan& plan) {
    std::map<world::RoomId, std::uint32_t> placed;
    for (const world::Placement& placement : plan.placements) {
        ++placed[placement.slot.fixture().room()];
    }
    return placed;
}

/// The plan's floors in the elevator's order (Floor::order, then the id).
[[nodiscard]] std::vector<const world::Floor*> floorsInOrder(const world::WorldPlan& plan) {
    std::vector<const world::Floor*> floors;
    for (const world::Floor& floor : plan.floors) {
        floors.push_back(&floor);
    }
    std::sort(floors.begin(), floors.end(), [](const world::Floor* a, const world::Floor* b) {
        return std::tie(a->order, a->id) < std::tie(b->order, b->id);
    });
    return floors;
}

/// The rooms of `floor` along its hall from the elevator: by the middle of their footprint on the floor (x), the north
/// row (yaw 0) first on a tie, then by id.
[[nodiscard]] std::vector<const world::Room*> roomsAlongHall(const world::WorldPlan& plan, std::string_view floor) {
    std::vector<std::pair<std::int64_t, const world::Room*>> rooms;
    const world::BuildingLayout layout; // the middle of a footprint does not depend on the wall allowance
    for (const world::Room& room : plan.rooms) {
        if (room.floor != floor) {
            continue;
        }
        const world::Footprint footprint = world::floorFootprint(room, layout);
        rooms.emplace_back(footprint.minXMm + footprint.maxXMm, &room);
    }
    std::sort(rooms.begin(), rooms.end(), [](const auto& a, const auto& b) {
        return std::tie(a.first, a.second->placement.yawHalfTurns, a.second->id) <
               std::tie(b.first, b.second->placement.yawHalfTurns, b.second->id);
    });
    std::vector<const world::Room*> ordered;
    ordered.reserve(rooms.size());
    for (const auto& [middle, room] : rooms) {
        ordered.push_back(room);
    }
    return ordered;
}

/// How many rooms each chain of `plan` has, by its head.
[[nodiscard]] std::map<world::RoomId, std::uint32_t> chainSizes(const world::WorldPlan& plan) {
    std::map<world::RoomId, std::uint32_t> sizes;
    for (const world::Room& room : plan.rooms) {
        ++sizes[room.id.head()];
    }
    return sizes;
}

/// `along` (a floor's rooms along its hall) with each chain's rooms together (I4b step 8): a head where it stands, then
/// its expansions on the floor by number; an expansion whose head is not on the floor where it stands.
[[nodiscard]] std::vector<const world::Room*> chainsTogether(const std::vector<const world::Room*>& along) {
    const auto onFloor = [&](const world::RoomId& id) {
        return std::any_of(along.begin(), along.end(), [&](const world::Room* room) { return room->id == id; });
    };
    std::vector<const world::Room*> ordered;
    ordered.reserve(along.size());
    for (const world::Room* room : along) {
        if (room->id.isExpansion()) {
            if (!onFloor(room->id.head())) {
                ordered.push_back(room); // never in a valid plan (expansion-chain): listed where it stands
            }
            continue;
        }
        ordered.push_back(room);
        std::vector<const world::Room*> expansions;
        for (const world::Room* other : along) {
            if (other->id.isExpansion() && other->id.head() == room->id) {
                expansions.push_back(other);
            }
        }
        std::sort(expansions.begin(), expansions.end(),
                  [](const world::Room* a, const world::Room* b) { return a->id.index() < b->id.index(); });
        ordered.insert(ordered.end(), expansions.begin(), expansions.end());
    }
    return ordered;
}

[[nodiscard]] WorldMove refuse(std::string_view notice, std::string why) {
    WorldMove move;
    move.kind = WorldMoveKind::Refuse;
    move.notice = std::string(notice);
    move.why = std::move(why);
    return move;
}

[[nodiscard]] WorldMove ignore(std::string why) {
    WorldMove move;
    move.kind = WorldMoveKind::Ignore;
    move.why = std::move(why);
    return move;
}

/// The refusal common to every move (none: the world can move now), `stateOk` whether the interaction's state allows
/// it, `applyingRefuses` whether a floor being applied does (a ride or a teleport: yes; the Focus pose in the room the
/// interaction is in: no, as I3's "Mostra nella stanza" went there while the persisted start's preview showed).
[[nodiscard]] std::optional<WorldMove> refusalOf(const WorldMoveConditions& conditions, bool havePlan, bool stateOk,
                                                 bool applyingRefuses = true) {
    if (!havePlan || !conditions.planReady) {
        return refuse(kNoticeBuildingPending, "the building is not planned yet");
    }
    if (conditions.applying && applyingRefuses) {
        return refuse(kNoticeRideUnavailable, "the floor shown is being applied");
    }
    if (conditions.riding) {
        return refuse(kNoticeRideUnavailable, "a ride runs");
    }
    if (conditions.launchActive) {
        return refuse(kNoticeRideUnavailable, "a launch is active");
    }
    if (!stateOk) {
        return refuse(kNoticeRideUnavailable, fmt::format("the world is in {}", toString(conditions.state)));
    }
    return std::nullopt;
}

} // namespace

std::string rangeInitial(std::string_view sortKey) {
    if (sortKey.empty()) {
        return "#";
    }
    const char c = sortKey.front();
    if (c >= 'a' && c <= 'z') {
        return std::string(1, static_cast<char>(c - 'a' + 'A'));
    }
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return std::string(1, c);
    }
    return "#";
}

std::string rangePrefix(std::string_view sortKey) {
    std::string prefix = rangeInitial(sortKey);
    if (prefix != "#" && sortKey.size() > 1) {
        const char c = sortKey[1];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            prefix += c;
        } else if (c >= 'A' && c <= 'Z') {
            prefix += static_cast<char>(c - 'A' + 'a');
        }
    }
    return prefix;
}

std::map<world::RoomId, ChainRoomRanges> chainRangesOf(const world::WorldPlan& plan, const ChainTitleOf& titleOf) {
    std::map<world::RoomId, std::vector<const world::Room*>> chains;
    for (const world::Room& room : plan.rooms) {
        chains[room.id.head()].push_back(&room);
    }
    std::map<world::RoomId, std::vector<catalog::GameId>> placed;
    for (const world::Placement& placement : plan.placements) {
        placed[placement.slot.fixture().room()].push_back(placement.game);
    }
    struct Entry {
        std::size_t section = 0;
        catalog::SystemName system;
        std::string sortKey;
        std::string titleKey;
        catalog::GameId game;
        world::RoomId room;
    };
    std::map<world::RoomId, ChainRoomRanges> result;
    for (const auto& [head, rooms] : chains) {
        if (rooms.size() < 2) {
            continue;
        }
        // The head's sections order the chain (its rooms share them); the titles of a system outside them go after
        // them, a section of their own per system, by name.
        const auto headRoom =
            std::find_if(rooms.begin(), rooms.end(), [&](const world::Room* room) { return room->id == head; });
        const std::vector<catalog::SystemName> sections =
            systemsOf(**(headRoom != rooms.end() ? headRoom : rooms.begin()));
        std::vector<Entry> entries;
        for (const world::Room* room : rooms) {
            result[room->id] = ChainRoomRanges{};
            const auto games = placed.find(room->id);
            if (games == placed.end() || !titleOf) {
                continue;
            }
            for (const catalog::GameId& game : games->second) {
                const std::optional<ChainTitle> title = titleOf(game);
                if (!title) {
                    continue;
                }
                const auto section = std::find(sections.begin(), sections.end(), title->system);
                entries.push_back(Entry{static_cast<std::size_t>(section - sections.begin()), title->system,
                                        title->sortKey, title->titleKey, game, room->id});
            }
        }
        // The generator's order (alphabeticalLess(): section, sort key, title key, GameId), so that titles of one sort
        // key at a boundary stand as the split shared them.
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return std::tie(a.section, a.system, a.sortKey, a.titleKey, a.game) <
                   std::tie(b.section, b.system, b.sortKey, b.titleKey, b.game);
        });
        // Section by section (the entries stand by section in order): each room's first and last title there, the
        // rooms in the order of their first; a boundary two rooms share the initial of shows two letters.
        std::size_t begin = 0;
        while (begin < entries.size()) {
            std::size_t end = begin;
            while (end < entries.size() && entries[end].section == entries[begin].section &&
                   entries[end].system == entries[begin].system) {
                ++end;
            }
            struct Span {
                world::RoomId room;
                std::size_t first = 0;
                std::size_t last = 0;
                std::string firstText;
                std::string lastText;
            };
            std::vector<Span> spans;
            for (const world::Room* room : rooms) {
                std::optional<std::size_t> first;
                std::size_t last = 0;
                for (std::size_t at = begin; at < end; ++at) {
                    if (entries[at].room == room->id) {
                        if (!first) {
                            first = at;
                        }
                        last = at;
                    }
                }
                if (!first) {
                    continue;
                }
                spans.push_back(Span{room->id, *first, last, rangeInitial(entries[*first].sortKey),
                                     rangeInitial(entries[last].sortKey)});
                for (std::size_t at = *first; at < last; ++at) {
                    if (entries[at].room != room->id) {
                        result[room->id].scattered = true;
                    }
                }
            }
            std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.first < b.first; });
            for (std::size_t index = 0; index + 1 < spans.size(); ++index) {
                Span& before = spans[index];
                Span& after = spans[index + 1];
                if (before.lastText == after.firstText) {
                    before.lastText = rangePrefix(entries[before.last].sortKey);
                    after.firstText = rangePrefix(entries[after.first].sortKey);
                }
            }
            // In the rooms' own order (the chain's), so that each room's ranges follow its sections.
            for (const world::Room* room : rooms) {
                for (const Span& span : spans) {
                    if (span.room == room->id) {
                        result[room->id].ranges.push_back(
                            ui::DirectoryRange{ui::DirectorySystem{entries[span.first].system.str(), {}},
                                               span.firstText, span.lastText});
                    }
                }
            }
            begin = end;
        }
    }
    return result;
}

std::vector<catalog::SystemName> chainSystemsOf(const world::WorldPlan& plan) {
    const std::map<world::RoomId, std::uint32_t> sizes = chainSizes(plan);
    std::vector<catalog::SystemName> systems;
    for (const world::Room& room : plan.rooms) {
        if (sizes.at(room.id.head()) < 2) {
            continue;
        }
        for (const catalog::SystemName& system : systemsOf(room)) {
            if (std::find(systems.begin(), systems.end(), system) == systems.end()) {
                systems.push_back(system);
            }
        }
    }
    return systems;
}

std::map<catalog::GameId, ChainTitle> chainTitlesOf(const std::filesystem::path& cacheDir,
                                                    const catalog::LibraryId& library,
                                                    const std::vector<catalog::SystemName>& systems) {
    std::map<catalog::GameId, ChainTitle> titles;
    for (const catalog::SystemName& system : systems) {
        auto items = readRoomItems(roomItemsFile(cacheDir, library, system));
        if (!items) {
            if (items.error().code != ErrorCode::NotFound) {
                spdlog::warn("Directory: the titles of {} are not known for its rooms' ranges: {}", system.str(),
                             items.error().message);
            }
            continue;
        }
        for (const RoomItemText& item : items->items) {
            // A shared or arcade room's items file lists its other sections' games too, without their text
            // (snapshotRoomItems()): a game's title is its own section's, never overwritten by another's listing.
            if (!hasText(item) || titles.contains(item.game)) {
                continue;
            }
            const std::string& shown = item.title.empty() ? item.fallbackTitle : item.title;
            std::string sortKey;
            if (item.screen && !item.screen->sortKey.empty()) {
                sortKey = item.screen->sortKey;
            } else if (auto key = catalog::sortKeyOf(shown)) {
                sortKey = std::move(*key);
            }
            auto key = world::titleKey(shown);
            titles.emplace(item.game, ChainTitle{system, std::move(sortKey), key ? std::move(*key) : std::string()});
        }
    }
    return titles;
}

ui::DirectoryBuilding directoryBuildingOf(const world::WorldPlan& plan, const SystemFullName& fullName,
                                          const std::map<world::RoomId, ChainRoomRanges>* ranges) {
    const std::map<world::RoomId, std::uint32_t> placed = placedPerRoom(plan);
    const std::map<world::RoomId, std::uint32_t> chains = chainSizes(plan);
    const auto shownSystem = [&](const catalog::SystemName& system) {
        return ui::DirectorySystem{system.str(), fullName ? fullName(system) : std::string()};
    };
    ui::DirectoryBuilding building;
    for (const world::Floor* floor : floorsInOrder(plan)) {
        ui::DirectoryFloor entry;
        entry.id = floor->id;
        entry.order = floor->order;
        for (const world::Room* room : chainsTogether(roomsAlongHall(plan, floor->id))) {
            ui::DirectoryRoom shown;
            shown.id = room->id.toString();
            shown.kind = std::string(world::toString(room->kind));
            for (const catalog::SystemName& system : systemsOf(*room)) {
                shown.systems.push_back(shownSystem(system));
            }
            const auto count = placed.find(room->id);
            shown.games = count != placed.end() ? count->second : 0;
            // I4b step 8: its place in its chain, its titles' ranges (their systems named as the room's are).
            shown.chainIndex = room->id.index();
            shown.chainRooms = chains.at(room->id.head());
            if (ranges != nullptr) {
                if (const auto found = ranges->find(room->id); found != ranges->end()) {
                    shown.ranges = found->second.ranges;
                    shown.scattered = found->second.scattered;
                    for (ui::DirectoryRange& range : shown.ranges) {
                        const auto system = std::find_if(shown.systems.begin(), shown.systems.end(),
                                                         [&](const ui::DirectorySystem& section) {
                                                             return section.name == range.system.name;
                                                         });
                        if (system != shown.systems.end()) {
                            range.system = *system;
                        }
                    }
                }
            }
            entry.rooms.push_back(std::move(shown));
        }
        building.floors.push_back(std::move(entry));
    }
    return building;
}

std::vector<WorldFloorRecord> floorRecordsOf(const world::WorldPlan& plan) {
    const std::map<world::RoomId, std::uint32_t> placed = placedPerRoom(plan);
    std::vector<WorldFloorRecord> records;
    for (const world::Floor* floor : floorsInOrder(plan)) {
        WorldFloorRecord record;
        record.id = floor->id;
        record.order = floor->order;
        for (const world::Room& room : plan.rooms) {
            if (room.floor != floor->id) {
                continue;
            }
            ++record.rooms;
            if (const auto count = placed.find(room.id); count != placed.end()) {
                record.placed += count->second;
            }
        }
        records.push_back(std::move(record));
    }
    return records;
}

std::optional<ui::DirectoryDate> machineLocalDate(catalog::UtcTime time) {
    using namespace std::chrono;
    const sys_seconds whole = floor<std::chrono::seconds>(time);
    try {
        const local_seconds local = current_zone()->to_local(whole);
        const year_month_day day{floor<days>(local)};
        return ui::DirectoryDate{static_cast<int>(day.year()), static_cast<int>(static_cast<unsigned>(day.month())),
                                 static_cast<int>(static_cast<unsigned>(day.day()))};
    } catch (const std::exception&) {
        const year_month_day day{floor<days>(whole)};
        return ui::DirectoryDate{static_cast<int>(day.year()), static_cast<int>(static_cast<unsigned>(day.month())),
                                 static_cast<int>(static_cast<unsigned>(day.day()))};
    }
}

ui::DirectoryGame directoryGameOf(const catalog::RecentGame& game, const LocalDate& localDate) {
    ui::DirectoryGame shown;
    shown.gameId = game.id.toString();
    shown.title = game.title;
    shown.system = game.system.str();
    shown.systemName = game.systemFullName;
    shown.favorite = game.favorite;
    if (game.lastPlayed && localDate) {
        shown.played = localDate(*game.lastPlayed);
    }
    return shown;
}

std::string dateText(const std::optional<ui::DirectoryDate>& date) {
    if (!date) {
        return {};
    }
    return fmt::format("{:04}-{:02}-{:02}", date->year, date->month, date->day);
}

std::string_view toString(WorldMoveKind kind) noexcept {
    switch (kind) {
    case WorldMoveKind::Ignore:
        return "ignore";
    case WorldMoveKind::Refuse:
        return "refuse";
    case WorldMoveKind::Ride:
        return "ride";
    case WorldMoveKind::Teleport:
        return "teleport";
    case WorldMoveKind::ShowHere:
        return "show-here";
    }
    return "unknown";
}

WorldMove routeDirectoryCommand(const ui::DirectoryCommand& command, const WorldMoveConditions& conditions,
                                const world::WorldPlan* plan, const world::PlanIndex* index,
                                std::string_view currentFloor, std::string_view eyeRoom) {
    switch (command.kind) {
    case ui::DirectoryCommandKind::Launch:
    case ui::DirectoryCommandKind::Close:
        return ignore("the product UI's command");
    case ui::DirectoryCommandKind::EditRoom:
        return ignore("the editor's command (I4b step 3: WorldSession::beginEdit)");
    case ui::DirectoryCommandKind::ShowInRoom: {
        const auto game = catalog::GameId::parse(command.id);
        if (!game) {
            return ignore("not a GameId: " + command.id);
        }
        return routeShowInRoom(*game, conditions, plan != nullptr ? index : nullptr, currentFloor, eyeRoom);
    }
    case ui::DirectoryCommandKind::ChooseFloor:
    case ui::DirectoryCommandKind::ChooseRoom:
        break;
    }
    if (std::optional<WorldMove> refused =
            refusalOf(conditions, plan != nullptr, conditions.state == WorldState::FreeRoam)) {
        return *refused;
    }
    WorldMove move;
    if (command.kind == ui::DirectoryCommandKind::ChooseFloor) {
        if (std::none_of(plan->floors.begin(), plan->floors.end(),
                         [&](const world::Floor& floor) { return floor.id == command.id; })) {
            return ignore("the building has no floor " + command.id);
        }
        move.reason = RideReason::Floor;
        move.floor = command.id;
        // The walker's own floor: back to its elevator, no ride (docs/i4/step-7a.md's contract).
        move.kind = command.id == currentFloor ? WorldMoveKind::Teleport : WorldMoveKind::Ride;
        move.why = move.kind == WorldMoveKind::Ride ? "to the floor's elevator" : "back to the elevator of the floor";
        return move;
    }
    const auto room = world::RoomId::parse(command.id);
    const auto found = room ? std::find_if(plan->rooms.begin(), plan->rooms.end(),
                                           [&](const world::Room& candidate) { return candidate.id == *room; })
                            : plan->rooms.end();
    if (found == plan->rooms.end()) {
        return ignore("the building has no room " + command.id);
    }
    move.reason = RideReason::Room;
    move.floor = found->floor;
    move.room = found->id.toString();
    move.kind = found->floor == currentFloor ? WorldMoveKind::Teleport : WorldMoveKind::Ride;
    move.why = move.kind == WorldMoveKind::Ride ? "to the room's floor, then its doorway" : "to the room's doorway";
    return move;
}

WorldMove routeShowInRoom(const catalog::GameId& game, const WorldMoveConditions& conditions,
                          const world::PlanIndex* index, std::string_view currentFloor, std::string_view eyeRoom) {
    if (index == nullptr || !conditions.planReady) {
        return refuse(kNoticeBuildingPending, "the building is not planned yet");
    }
    const std::optional<world::GameLocation> location = index->find(game);
    if (!location || !location->placed() || location->floor.empty()) {
        return ignore("the building places no item of " + game.toString());
    }
    WorldMove move;
    move.reason = RideReason::ShowInRoom;
    move.floor = location->floor;
    move.room = location->room.toString();
    move.game = game.toString();
    if (location->floor != currentFloor) {
        move.kind = WorldMoveKind::Ride;
        move.why = "the game's room is on another floor";
    } else if (move.room != eyeRoom) {
        move.kind = WorldMoveKind::Teleport;
        move.why = "the game's room is another room of the floor";
    } else {
        move.kind = WorldMoveKind::ShowHere;
        move.why = "the game's room is the one the interaction is in";
    }
    // Focus is left first; a floor being applied refuses the ride and the teleport, not the Focus pose where the walker
    // is (I3's "Mostra nella stanza").
    const bool stateOk = conditions.state == WorldState::FreeRoam || conditions.state == WorldState::Focus;
    if (std::optional<WorldMove> refused =
            refusalOf(conditions, true, stateOk, move.kind != WorldMoveKind::ShowHere)) {
        return *refused;
    }
    return move;
}

} // namespace rv::app
