#include <rv/app/directory_data.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <map>
#include <tuple>
#include <utility>

#include <fmt/format.h>

#include <rv/world/building.hpp>

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
/// it.
[[nodiscard]] std::optional<WorldMove> refusalOf(const WorldMoveConditions& conditions, bool havePlan, bool stateOk) {
    if (!havePlan || !conditions.planReady) {
        return refuse(kNoticeBuildingPending, "the building is not planned yet");
    }
    if (conditions.applying) {
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

ui::DirectoryBuilding directoryBuildingOf(const world::WorldPlan& plan, const SystemFullName& fullName) {
    const std::map<world::RoomId, std::uint32_t> placed = placedPerRoom(plan);
    ui::DirectoryBuilding building;
    for (const world::Floor* floor : floorsInOrder(plan)) {
        ui::DirectoryFloor entry;
        entry.id = floor->id;
        entry.order = floor->order;
        for (const world::Room* room : roomsAlongHall(plan, floor->id)) {
            ui::DirectoryRoom shown;
            shown.id = room->id.toString();
            shown.kind = std::string(world::toString(room->kind));
            for (const catalog::SystemName& system : systemsOf(*room)) {
                shown.systems.push_back(ui::DirectorySystem{system.str(), fullName ? fullName(system) : std::string()});
            }
            const auto count = placed.find(room->id);
            shown.games = count != placed.end() ? count->second : 0;
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
    const bool stateOk = conditions.state == WorldState::FreeRoam || conditions.state == WorldState::Focus;
    if (std::optional<WorldMove> refused = refusalOf(conditions, index != nullptr, stateOk)) {
        return *refused;
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
    return move;
}

} // namespace rv::app
