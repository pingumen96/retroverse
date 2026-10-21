#pragma once

// A floor of the building as its scene and its validation see it (ADR-0013 §6, §7, §9, §11; I4 step 3): the hall
// derived from the floor's rooms, each room's doorway onto it, the elevator stand-in, the areas and the room-to-floor
// transform. GPU-free and pure: integers in millimetres in the floor's frame (building.hpp: +X east from the elevator at
// the hall's west end, north = -Z, the hall's centre line on z = 0), and meters where a matrix says so.
//
// The hall (the ground floor's lobby) is derived, never stored in the plan:
//   - its interior runs east from x = 0 (the elevator) to the floor's east end, the easternmost edge of its rows'
//     footprints, at least kMinHallLengthMm, between z = -width / 2 and +width / 2 (building.json's hall or lobby
//     width), kHallHeightMm high;
//   - its walls, as thick as the building's wall allowance and as high as the hall, close it where no room does: the
//     west wall behind the elevator, the east wall at its end, and along each edge the stretches its row's rooms'
//     footprints leave free (a room's doorway wall, its south wall, stands on the edge: its rooms' walls are the
//     hall's);
//   - its lights hang on its centre line kHallLightDropMm below the ceiling at a fixed pitch from the west end
//     (x = pitch / 2, 3 pitch / 2, ... while below the east end): a longer hall appends lights and moves none. The
//     hall has no light limit (ADR-0013 §9: the renderer packs those nearest the eye).
//   - the elevator, a stand-in: its doors in the middle of the west wall (kElevatorDoorWidthMm by
//     kElevatorDoorHeightMm, a frame kElevatorFrameMm wide standing kElevatorFrameDepthMm proud of the wall), the
//     proximity zone before them (step 7's directory opens there) and the arrival point in front of them, the walker
//     facing east (the floor scene's camera);
//   - the lobby's entrance at its east end: closed doors (decoration) in the middle of the east wall.
// A room's doorway on its floor is its template's at the room's growth step, through the room's wall (the template's
// wall thickness); it opens onto the hall when it is in the room's south wall (its doorway wall) and that wall's outer
// face stands on the hall's edge: the room abuts the hall (building.hpp's followDepth()).
// The areas (ADR-0013 §9): each room's interior extended through its doorway's passage, and the hall's interior.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <rv/world/building.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// The hall's height (ADR-0013 §6: 3.2 m, the rooms' too).
inline constexpr std::int64_t kHallHeightMm = 3200;
/// The pitch of the hall's lights from its west end, and how far below the ceiling they hang.
inline constexpr std::int64_t kHallLightPitchMm = 6000;
inline constexpr std::int64_t kHallLightDropMm = 300;
/// The shortest hall (the elevator's landing and a little more), so that a floor of few rooms, or none, reads as one.
inline constexpr std::int64_t kMinHallLengthMm = 6000;
/// The elevator's doors (two closed leaves), the frame around them and how far it stands proud of the west wall.
inline constexpr std::int64_t kElevatorDoorWidthMm = 1200;
inline constexpr std::int64_t kElevatorDoorHeightMm = 2200;
inline constexpr std::int64_t kElevatorFrameMm = 100;
inline constexpr std::int64_t kElevatorFrameDepthMm = 50;
/// The elevator's proximity zone: before its doors, this deep and this wide (I4 step 7's prompt and directory).
inline constexpr std::int64_t kElevatorZoneDepthMm = 1500;
inline constexpr std::int64_t kElevatorZoneWidthMm = 2000;
/// Where the walker arrives on a floor: this far east of the elevator on the hall's centre line, facing east.
inline constexpr std::int64_t kArrivalXMm = 1200;
/// The lobby's entrance doors at its east end (decoration): two leaves this wide in all, this high.
inline constexpr std::int64_t kEntranceWidthMm = 2400;
inline constexpr std::int64_t kEntranceHeightMm = 2400;

/// A room of a floor with its template (whose doorway and wall thickness it is built with); a room whose template is
/// not known (null) stands on its row, its doorway and walls left out.
struct FloorRoom {
    const Room* room = nullptr;
    const RoomTemplate* roomTemplate = nullptr;
};

/// A room's doorway on its floor.
struct FloorDoorway {
    RoomId room;
    /// The row of its room, and whether the doorway is in the room's south wall (its doorway wall, facing the hall).
    Row row = Row::North;
    bool inDoorwayWall = false;
    /// The opening, along the hall (x) for a doorway in the doorway wall, along z otherwise; its height.
    std::int64_t minMm = 0;
    std::int64_t maxMm = 0;
    std::int64_t heightMm = 0;
    /// The passage through the room's wall: from the room's interior to the wall's outer face (floor frame).
    Footprint passage;
    /// The z of the doorway wall's outer face (its x for a doorway in a side wall), and the z of the hall's edge it
    /// should stand on (-width / 2 for the north row, +width / 2 for the south row).
    std::int64_t outerMm = 0;
    std::int64_t hallEdgeZMm = 0;
    /// The centre of the clear area inside the doorway (where the room's own walk starts, validation.hpp).
    FloorPoint clearCentre;

    /// It opens onto the hall: in the doorway wall, whose outer face stands on the hall's edge.
    [[nodiscard]] bool opensOntoHall() const noexcept { return inDoorwayWall && outerMm == hallEdgeZMm; }

    bool operator==(const FloorDoorway&) const = default;
};

/// A box of a hall's walls on the floor, standing its whole height.
struct HallWall {
    /// "west", "east", "north-<n>" and "south-<n>" (n from 0, west to east).
    std::string id;
    Footprint footprint;

    bool operator==(const HallWall&) const = default;
};

/// A floor's hall (see the file comment).
struct Hall {
    std::string floor;
    /// The ground floor's hall is the lobby.
    bool lobby = false;
    std::int64_t widthMm = 0;
    std::int64_t heightMm = kHallHeightMm;
    std::int64_t wallMm = 0;
    /// x from 0 to the floor's east end, z from -width / 2 to +width / 2.
    Footprint interior;
    /// West, east, then the north edge's stretches and the south edge's, west to east.
    std::vector<HallWall> walls;
    /// The doorways of the floor's rooms that have a template, by room id.
    std::vector<FloorDoorway> doorways;
    /// The lights' places (x, z) on the centre line, west to east; their height is heightMm - kHallLightDropMm.
    std::vector<FloorPoint> lights;
    /// The elevator's frame on the west wall (what stands proud of it), its proximity zone, and the arrival point.
    Footprint elevator;
    Footprint elevatorZone;
    FloorPoint arrival;
    /// The lobby's entrance doors in its east wall (z from minZMm to maxZMm); empty for a hall.
    Footprint entrance;

    /// The hall with its walls and the elevator's frame (what a room of the floor must not overlap).
    [[nodiscard]] Footprint footprint() const noexcept;

    bool operator==(const Hall&) const = default;
};

/// The area id of a floor's hall: "hall:<floor>".
[[nodiscard]] std::string hallAreaId(std::string_view floor);

/// A room's walls on its floor: its shell's four walls (the template's thickness) around its interior, the doorway's
/// wall split around the opening (the passage left free), as footprints in the floor's frame, in the order north,
/// south, east, west (a split wall's two parts in a row).
[[nodiscard]] std::vector<Footprint> floorRoomWalls(const Room& room, const RoomTemplate& roomTemplate);

/// The doorway of `room` (standing on a floor of `layout`) on its floor (see the file comment).
[[nodiscard]] FloorDoorway floorDoorway(const Room& room, const RoomTemplate& roomTemplate,
                                        const BuildingLayout& layout);

/// The hall of `floor` (see the file comment), derived from the rooms of `rooms` that stand on it (the others are
/// ignored): their footprints set its east end and where its edges need walls, their doorways (those with a template)
/// are listed. A floor without rooms has the shortest hall.
[[nodiscard]] Hall deriveHall(std::string_view floor, std::span<const FloorRoom> rooms, const BuildingLayout& layout);

/// An area of a floor (ADR-0013 §9): a room's interior and its doorway's passage, or the hall's interior; footprints in
/// the floor's frame, from the floor up to `heightMm`.
struct FloorArea {
    /// A room's id ("room:snes"), or hallAreaId().
    std::string id;
    std::vector<Footprint> footprints;
    std::int64_t heightMm = 0;

    bool operator==(const FloorArea&) const = default;
};

/// The areas of a floor: its rooms' (those with a template, by id: the interior on the floor, then the passage of the
/// doorway) and the hall's, last.
[[nodiscard]] std::vector<FloorArea> floorAreas(const Hall& hall, std::span<const FloorRoom> rooms);

/// The room's frame to its floor's, in meters (ADR-0002): a half turn about +Y for the south row, then the placement's
/// translation (x and z in millimetres / 1000). Quarter turns exact.
[[nodiscard]] glm::mat4 roomToFloorMatrix(const RoomPlacement& placement) noexcept;

} // namespace rv::world
