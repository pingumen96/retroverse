#include <rv/world/floor_layout.hpp>

#include <algorithm>
#include <utility>

namespace rv::world {
namespace {

/// The parts of [low, high] that none of `taken` (intervals, in any order, possibly overlapping) covers, west to east.
std::vector<std::pair<std::int64_t, std::int64_t>> uncovered(std::int64_t low, std::int64_t high,
                                                             std::vector<std::pair<std::int64_t, std::int64_t>> taken) {
    std::sort(taken.begin(), taken.end());
    std::vector<std::pair<std::int64_t, std::int64_t>> free;
    std::int64_t cursor = low;
    for (const auto& [from, to] : taken) {
        const std::int64_t start = std::max(from, low);
        const std::int64_t end = std::min(to, high);
        if (end <= start) {
            continue;
        }
        if (start > cursor) {
            free.emplace_back(cursor, start);
        }
        cursor = std::max(cursor, end);
    }
    if (cursor < high) {
        free.emplace_back(cursor, high);
    }
    return free;
}

/// The footprint (room frame) of a wall stretch: [a0, a1] along the wall, between `inner` and `outer` across it.
Footprint wallPart(Wall wall, std::int64_t a0, std::int64_t a1, std::int64_t inner, std::int64_t outer) noexcept {
    const std::int64_t low = std::min(inner, outer);
    const std::int64_t high = std::max(inner, outer);
    if (wall == Wall::North || wall == Wall::South) {
        return {a0, low, a1, high};
    }
    return {low, a0, high, a1};
}

} // namespace

Footprint Hall::footprint() const noexcept {
    return {interior.minXMm - wallMm, interior.minZMm, interior.maxXMm + wallMm, interior.maxZMm};
}

std::string hallAreaId(std::string_view floor) {
    return "hall:" + std::string(floor);
}

std::vector<Footprint> floorRoomWalls(const Room& room, const RoomTemplate& roomTemplate) {
    const std::int64_t t = roomTemplate.wallThicknessMm;
    const Doorway& door = roomTemplate.doorway;
    struct Side {
        Wall wall;
        std::int64_t along0;
        std::int64_t along1;
        std::int64_t inner;
        std::int64_t outer;
    };
    // North and south walls cover the corners; east and west ones stand between them.
    const Side sides[] = {
        {Wall::North, room.minXMm - t, room.maxXMm + t, room.minZMm, room.minZMm - t},
        {Wall::South, room.minXMm - t, room.maxXMm + t, room.maxZMm, room.maxZMm + t},
        {Wall::East, room.minZMm, room.maxZMm, room.maxXMm, room.maxXMm + t},
        {Wall::West, room.minZMm, room.maxZMm, room.minXMm, room.minXMm - t},
    };
    std::vector<Footprint> walls;
    for (const Side& side : sides) {
        if (door.wall != side.wall) {
            walls.push_back(roomToFloor(room.placement, wallPart(side.wall, side.along0, side.along1, side.inner,
                                                                 side.outer)));
            continue;
        }
        const std::int64_t open0 = door.offsetMm - door.widthMm / 2;
        const std::int64_t open1 = open0 + door.widthMm;
        if (open0 > side.along0) {
            walls.push_back(roomToFloor(room.placement, wallPart(side.wall, side.along0, open0, side.inner, side.outer)));
        }
        if (side.along1 > open1) {
            walls.push_back(roomToFloor(room.placement, wallPart(side.wall, open1, side.along1, side.inner, side.outer)));
        }
    }
    return walls;
}

FloorDoorway floorDoorway(const Room& room, const RoomTemplate& roomTemplate, const BuildingLayout& layout) {
    const Doorway& door = roomTemplate.doorway;
    const std::int64_t t = roomTemplate.wallThicknessMm;
    FloorDoorway result;
    result.room = room.id;
    result.row = rowOf(room.placement);
    result.inDoorwayWall = door.wall == Wall::South;
    result.heightMm = door.heightMm;
    const std::int64_t half = hallWidthOf(room.floor, layout) / 2;
    result.hallEdgeZMm = result.row == Row::North ? -half : half;
    const std::int64_t open0 = door.offsetMm - door.widthMm / 2;
    const std::int64_t open1 = open0 + door.widthMm;
    Footprint passage;
    std::int64_t outerX = 0;
    std::int64_t outerZ = 0;
    switch (door.wall) {
    case Wall::North:
        passage = {open0, room.minZMm - t, open1, room.minZMm};
        outerZ = room.minZMm - t;
        break;
    case Wall::East:
        passage = {room.maxXMm, open0, room.maxXMm + t, open1};
        outerX = room.maxXMm + t;
        break;
    case Wall::West:
        passage = {room.minXMm - t, open0, room.minXMm, open1};
        outerX = room.minXMm - t;
        break;
    case Wall::South:
    case Wall::None:
        passage = {open0, room.maxZMm, open1, room.maxZMm + t};
        outerZ = room.maxZMm + t;
        break;
    }
    result.passage = roomToFloor(room.placement, passage);
    const FloorPoint outer = roomToFloor(room.placement, outerX, outerZ);
    const bool alongX = door.wall == Wall::North || door.wall == Wall::South || door.wall == Wall::None;
    result.outerMm = alongX ? outer.zMm : outer.xMm;
    result.minMm = alongX ? result.passage.minXMm : result.passage.minZMm;
    result.maxMm = alongX ? result.passage.maxXMm : result.passage.maxZMm;
    const Footprint clear = doorwayClearance(roomTemplate, room.growthSteps);
    result.clearCentre = roomToFloor(room.placement, (clear.minXMm + clear.maxXMm) / 2, (clear.minZMm + clear.maxZMm) / 2);
    return result;
}

Hall deriveHall(std::string_view floor, std::span<const FloorRoom> rooms, const BuildingLayout& layout) {
    Hall hall;
    hall.floor = std::string(floor);
    hall.lobby = floor == kGroundFloor;
    hall.widthMm = hallWidthOf(floor, layout);
    hall.wallMm = layout.wallAllowanceMm;
    const std::int64_t half = hall.widthMm / 2;
    const std::int64_t wall = hall.wallMm;

    std::int64_t east = std::max(kMinHallLengthMm, layout.rowStartXMm);
    std::vector<std::pair<std::int64_t, std::int64_t>> north;
    std::vector<std::pair<std::int64_t, std::int64_t>> south;
    for (const FloorRoom& entry : rooms) {
        if (entry.room == nullptr || entry.room->floor != floor) {
            continue;
        }
        const Footprint footprint = floorFootprint(*entry.room, layout);
        east = std::max(east, footprint.maxXMm);
        (rowOf(entry.room->placement) == Row::North ? north : south).emplace_back(footprint.minXMm, footprint.maxXMm);
        if (entry.roomTemplate != nullptr) {
            hall.doorways.push_back(floorDoorway(*entry.room, *entry.roomTemplate, layout));
        }
    }
    std::sort(hall.doorways.begin(), hall.doorways.end(),
              [](const FloorDoorway& a, const FloorDoorway& b) { return a.room < b.room; });

    hall.interior = {0, -half, east, half};
    hall.walls.push_back({"west", {-wall, -half - wall, 0, half + wall}});
    hall.walls.push_back({"east", {east, -half - wall, east + wall, half + wall}});
    std::size_t index = 0;
    for (const auto& [from, to] : uncovered(0, east, north)) {
        hall.walls.push_back({"north-" + std::to_string(index++), {from, -half - wall, to, -half}});
    }
    index = 0;
    for (const auto& [from, to] : uncovered(0, east, south)) {
        hall.walls.push_back({"south-" + std::to_string(index++), {from, half, to, half + wall}});
    }
    for (std::int64_t x = kHallLightPitchMm / 2; x < east; x += kHallLightPitchMm) {
        hall.lights.push_back({x, 0});
    }
    const std::int64_t frameHalf = kElevatorDoorWidthMm / 2 + kElevatorFrameMm;
    hall.elevator = {0, -frameHalf, kElevatorFrameDepthMm, frameHalf};
    hall.elevatorZone = {0, -kElevatorZoneWidthMm / 2, kElevatorZoneDepthMm, kElevatorZoneWidthMm / 2};
    hall.arrival = {kArrivalXMm, 0};
    if (hall.lobby) {
        hall.entrance = {east, -kEntranceWidthMm / 2, east + wall, kEntranceWidthMm / 2};
    }
    return hall;
}

std::vector<FloorArea> floorAreas(const Hall& hall, std::span<const FloorRoom> rooms) {
    std::vector<const FloorRoom*> sorted;
    for (const FloorRoom& entry : rooms) {
        if (entry.room != nullptr && entry.roomTemplate != nullptr && entry.room->floor == hall.floor) {
            sorted.push_back(&entry);
        }
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const FloorRoom* a, const FloorRoom* b) { return a->room->id < b->room->id; });
    std::vector<FloorArea> areas;
    for (const FloorRoom* entry : sorted) {
        const Room& room = *entry->room;
        FloorArea area;
        area.id = room.id.toString();
        area.heightMm = room.heightMm;
        area.footprints.push_back(
            roomToFloor(room.placement, Footprint{room.minXMm, room.minZMm, room.maxXMm, room.maxZMm}));
        for (const FloorDoorway& doorway : hall.doorways) {
            if (doorway.room == room.id) {
                area.footprints.push_back(doorway.passage);
            }
        }
        areas.push_back(std::move(area));
    }
    areas.push_back(FloorArea{hallAreaId(hall.floor), {hall.interior}, hall.heightMm});
    return areas;
}

glm::mat4 roomToFloorMatrix(const RoomPlacement& placement) noexcept {
    const float sign = placement.yawHalfTurns % 2 == 0 ? 1.0f : -1.0f;
    glm::mat4 matrix(1.0f);
    matrix[0] = glm::vec4(sign, 0.0f, 0.0f, 0.0f);
    matrix[2] = glm::vec4(0.0f, 0.0f, sign, 0.0f);
    matrix[3] = glm::vec4(static_cast<float>(static_cast<double>(placement.xMm) / 1000.0), 0.0f,
                          static_cast<float>(static_cast<double>(placement.zMm) / 1000.0), 1.0f);
    return matrix;
}

} // namespace rv::world
