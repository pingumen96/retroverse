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
//
// The hall's dressing (I6 step 2, look.md "Halls"): pieces on its two long edges, the north edge's wall at
// z = -width / 2 and the south edge's at +width / 2 (a room's doorway wall where one stands, the hall's own elsewhere):
//   - the slots: x = kHallSlotFirstMm + i * kHallSlotPitchMm from the west end (i from 0), the same on both edges, so
//     that a longer hall appends pieces and moves none (the lights' rule). A slot's wall piece alternates: on the north
//     edge a sconce at even i and a framed picture at odd i, on the south edge the reverse (a sconce faces a picture);
//     the pictures of an edge alternate portrait and landscape (landscape where i / 2 is odd).
//     Every kHallFloorPeriod slots the floor holds a bench with a plant beside it, on each edge in turn every 6 m
//     (north: a plant at i % 8 == 2 and a bench at 3; south: a bench at 6 and a plant at 7) against its wall;
//   - a slot's piece is left out where it would not fit: within kHallEndClearMm of the hall's ends (wall pieces) or
//     before kHallFloorFirstMm and within kHallFloorEndClearMm of the east end (floor pieces), a floor piece that would
//     leave less than kHallMinClearMm across the hall (beside what the other edge holds there: a hall too narrow holds
//     none); a wall piece within its
//     margin of a doorway's opening on its edge (kHallSconceDoorMarginMm for a sconce, kHallPictureDoorMarginMm for a
//     picture), a floor piece within kHallDoorApproachMm of any doorway's opening along x, on either edge (the doorway's
//     approach and the place across the hall stay clear). A room that enters later may so take a slot's piece away
//     (its doorway's margin); it never moves one;
//   - each doorway that opens onto the hall is flanked by two sconces on its wall, kHallFlankSconceMm from the opening's
//     sides (they come and go with the room, whose doorway never moves).
// Benches and plants stand on the floor: they collide and are obstacles of the floor's navigation (validation.hpp's
// floor-unreachable); they keep the hall's middle clear, at least kHallMinClearMm wide wherever they stand. Sconces and
// pictures hang on the wall (sconces above the head).
//
// The ground floor's hall is the hub (I6 step 5, ADR-0014 §3, style §5.2, concept panel 1): building.json's hub, at
// least HubLayout::lengthMm long (18 m: further when its rows reach further east), HubLayout::widthMm wide (14 m) and
// HubLayout::heightMm high (5 m), its rooms on its long walls (building.hpp's rowStartOf()). Where a room stands on an
// edge its doorway wall is the hub's wall up to the room's height, and a wall of the hub's own closes the edge above it
// (HallWall::bottomMm). Its corridor dressing is the slots' wall pieces only (no bench or plant: Hub's pieces stand
// instead), and its own (Hub, all of it fixed around the hub's core, the first HubLayout::lengthMm, or along the
// balcony's edges at a fixed pitch, so that a longer hub appends columns and moves none):
//   - a balcony kHubBalconyDepthMm deep along the north, south and east walls (decorative: nothing walks on it), its
//     edge on columns every kHubColumnPitchMm from x = kHubColumnPitchMm along the long edges (while
//     kHubColumnEndClearMm short of the east balcony's edge) and at the east edge's corners and kHubEastColumnZMm either
//     side of its middle; a column within kHubColumnClearMm of a doorway's or the showcase's middle along its edge is
//     left out (the portal and the showcase open onto the hub);
//   - the landmark in the core's middle (kHubLandmarkRadiusMm: a glowing globe on a pedestal in a railed circular
//     showcase of generic consoles), the directory's totem west of it facing the elevator (its proximity zone in front
//     of it: Hub::totemZone, like the elevator's), the favourites' showcase against the south wall in the core's middle
//     (its cases are runtime proxies, never plan placements: ADR-0013 §10) with an information screen above it, two
//     sofas facing the landmark on rugs, a runner of rugs from the elevator to the landmark and on to the entrance, a
//     rug before the core's north wall, plants by the elevator, the entrance, the showcase and the core's north portal,
//     a lit bookcase of books against each long wall in the middle of every column bay off the core's middle;
//   - its lights: point lights kHubLightDropMm below the ceiling over the core (Hall::lights) and under the balcony
//     (Hall::lowLights, kHubLowLightHeightMm high).
// A piece within a doorway's approach (kHubApproachMm along the edge beside the opening, kHubApproachDepthMm into the
// hub) is left out, so that a room entering the hub later never has its doorway blocked (it may take a piece away, as
// a hall's doorway takes a slot's piece). The hub's solid pieces (columns, the landmark, the totem, the showcase, the
// sofas, the plants) collide and are obstacles of the floor's navigation; the totem's zone and the showcase's front are
// reached from the arrival point (validation.hpp's floor-unreachable).

#include <cstdint>
#include <optional>
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
inline constexpr std::int64_t kHallLightDropMm = 600;
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

/// A box of a hall's walls on the floor, standing its whole height (above `bottomMm`).
struct HallWall {
    /// "west", "east", "north-<n>" and "south-<n>" (n from 0, west to east); the hub's walls above its rooms
    /// "north-upper-<n>" and "south-upper-<n>".
    std::string id;
    Footprint footprint;
    /// 0, or the height of the room below it (the hub's walls above its rooms, I6 step 5).
    std::int64_t bottomMm = 0;

    bool operator==(const HallWall&) const = default;
};

/// The hall's dressing (see the file comment): the slots' pitch and the first slot's x from the west end.
inline constexpr std::int64_t kHallSlotPitchMm = 1500;
inline constexpr std::int64_t kHallSlotFirstMm = 750;
/// The period of the benches and plants, in slots, and the slots they stand at on each edge.
inline constexpr std::int64_t kHallFloorPeriod = 8;
inline constexpr std::int64_t kHallNorthBenchSlot = 3;
inline constexpr std::int64_t kHallNorthPlantSlot = 2;
inline constexpr std::int64_t kHallSouthBenchSlot = 6;
inline constexpr std::int64_t kHallSouthPlantSlot = 7;
/// What a wall piece keeps clear of the hall's ends, what a floor piece keeps clear of the elevator's landing (its x
/// from the west end) and of the east end.
inline constexpr std::int64_t kHallEndClearMm = 300;
inline constexpr std::int64_t kHallFloorFirstMm = 2500;
inline constexpr std::int64_t kHallFloorEndClearMm = 500;
/// How far from a doorway's opening (along x) a slot's sconce, picture and floor piece keep, and how far beside the
/// opening the doorway's own sconces hang (their centres).
inline constexpr std::int64_t kHallSconceDoorMarginMm = 1500;
inline constexpr std::int64_t kHallPictureDoorMarginMm = 800;
inline constexpr std::int64_t kHallDoorApproachMm = 900;
inline constexpr std::int64_t kHallFlankSconceMm = 400;
/// The pieces' sizes: a sconce's width and depth (from the wall), a picture's width (portrait, landscape) and depth,
/// a bench's length and depth (with its gap to the wall), a plant's pot centre from the wall and its spread.
inline constexpr std::int64_t kHallSconceWidthMm = 130;
inline constexpr std::int64_t kHallSconceDepthMm = 160;
inline constexpr std::int64_t kHallPictureWidthMm = 560;
inline constexpr std::int64_t kHallPictureLandscapeWidthMm = 760;
inline constexpr std::int64_t kHallPictureDepthMm = 35;
inline constexpr std::int64_t kHallBenchLengthMm = 1200;
inline constexpr std::int64_t kHallBenchDepthMm = 450;
inline constexpr std::int64_t kHallPlantOffsetMm = 330;
inline constexpr std::int64_t kHallPlantSpreadMm = 300;
/// The clear width the dressing leaves in the hall's middle (style §6.2's main passage).
inline constexpr std::int64_t kHallMinClearMm = 1400;

/// What a piece of a hall's dressing is.
enum class HallPieceKind : std::uint8_t {
    Sconce,  ///< a wall light (an emissive fixture: no light of its own)
    Picture, ///< a framed print
    Bench,   ///< a bench against the wall
    Plant,   ///< a potted plant against the wall
};

/// A piece of a hall's dressing (see the file comment), in the floor's frame.
struct HallPiece {
    /// "<edge>-<slot>/<kind>" for a slot's piece ("north-3/picture", and the bench below it "north-3/bench"),
    /// "portal:<room id>/sconce-west" and "-east" for a doorway's sconces.
    std::string id;
    HallPieceKind kind = HallPieceKind::Sconce;
    /// On the north edge (else the south edge).
    bool north = true;
    /// Its slot (i), -1 for a doorway's sconce.
    std::int64_t slot = -1;
    /// A picture hung landscape (else portrait).
    bool landscape = false;
    /// Its centre along the hall.
    std::int64_t xMm = 0;
    /// What it takes on the floor: from the wall's face into the hall by its depth, its width along x.
    Footprint footprint;
    /// A sconce's centre's height; 0: the hall's (the hub's gallery sconces kHubGallerySconceMm).
    std::int64_t centreMm = 0;

    /// It stands on the floor: a collision and navigation obstacle.
    [[nodiscard]] bool solid() const noexcept { return kind == HallPieceKind::Bench || kind == HallPieceKind::Plant; }

    bool operator==(const HallPiece&) const = default;
};

/// The hub's sizes and places (see the file comment), millimetres.
/// The balcony: how deep from the wall to its edge, its walking surface's height, its slab's thickness under it, the
/// fascia along its edge (its bottom; its top the walking surface's), its railing's height.
inline constexpr std::int64_t kHubBalconyDepthMm = 1200;
inline constexpr std::int64_t kHubBalconyFloorMm = 3150;
inline constexpr std::int64_t kHubBalconySlabMm = 200;
inline constexpr std::int64_t kHubFasciaBottomMm = 2850;
inline constexpr std::int64_t kHubRailingHeightMm = 950;
/// The columns: their pitch along the long edges, how far short of the east balcony's edge they stop, how far from a
/// doorway's or the showcase's middle none stands, the east edge's columns either side of its middle, the plinth's side.
inline constexpr std::int64_t kHubColumnPitchMm = 3000;
inline constexpr std::int64_t kHubColumnEndClearMm = 1500;
inline constexpr std::int64_t kHubColumnClearMm = 2000;
inline constexpr std::int64_t kHubEastColumnZMm = 2900;
inline constexpr std::int64_t kHubColumnSizeMm = 560;
/// The landmark's radius (its railed showcase's outer rim) in the core's middle.
inline constexpr std::int64_t kHubLandmarkRadiusMm = 2100;
/// The directory's totem: its place west of the landmark (from the core's middle along x, and z), its width and depth,
/// its zone's depth before it and width.
inline constexpr std::int64_t kHubTotemFromCentreMm = 5400;
inline constexpr std::int64_t kHubTotemZMm = 2400;
inline constexpr std::int64_t kHubTotemWidthMm = 600;
inline constexpr std::int64_t kHubTotemDepthMm = 320;
inline constexpr std::int64_t kHubTotemZoneDepthMm = 1500;
inline constexpr std::int64_t kHubTotemZoneWidthMm = 1600;
/// The favourites' showcase (a display case of kHubShowcaseSlots face-out cases) against the south wall in the core's
/// middle: its width, depth and gap to the wall; the information screen above it on the wall.
inline constexpr std::int64_t kHubShowcaseWidthMm = 2000;
inline constexpr std::int64_t kHubShowcaseDepthMm = 500;
inline constexpr std::int64_t kHubShowcaseGapMm = 30;
inline constexpr std::size_t kHubShowcaseSlots = 12;
/// Where the walker stands before the showcase to pick a case: this far before its front.
inline constexpr std::int64_t kHubShowcaseFrontMm = 700;
inline constexpr std::int64_t kHubScreenWidthMm = 1800;
inline constexpr std::int64_t kHubScreenHeightMm = 1000;
inline constexpr std::int64_t kHubScreenCentreMm = 2100;
/// A sofa facing the landmark (from the core's middle along x, its z), its width and depth; its rug.
inline constexpr std::int64_t kHubSofaFromCentreMm = 2700;
inline constexpr std::int64_t kHubSofaZMm = 4200;
inline constexpr std::int64_t kHubSofaWidthMm = 2000;
inline constexpr std::int64_t kHubSofaDepthMm = 900;
inline constexpr std::int64_t kHubSofaRugWidthMm = 3000;
inline constexpr std::int64_t kHubSofaRugDepthMm = 2000;
inline constexpr std::int64_t kHubSofaRugZMm = 3900;
/// The runner's and the portal rug's size (the halls' runner rug), the portal rug's distance from its wall.
inline constexpr std::int64_t kHubRugLengthMm = 2400;
inline constexpr std::int64_t kHubRugWidthMm = 1600;
inline constexpr std::int64_t kHubPortalRugFromWallMm = 2500;
/// A plant's spread (the halls'), how far its pot stands from its wall, and from a portal's or the showcase's middle.
inline constexpr std::int64_t kHubPlantSpreadMm = 300;
inline constexpr std::int64_t kHubPlantFromWallMm = 450;
inline constexpr std::int64_t kHubPlantBesideMm = 2000;
inline constexpr std::int64_t kHubPlantBesideShowcaseMm = 1700;
/// A doorway's approach that no piece takes: beside its opening along its wall, and into the hub from it.
inline constexpr std::int64_t kHubApproachMm = 900;
inline constexpr std::int64_t kHubApproachDepthMm = 2500;
/// The bookcases against the long walls (the museum-library, style §5.2): one in the middle of every column bay, this
/// wide, deep and far from the wall, none within kHubBookcaseCoreClearMm of the core's middle (its portal, its
/// showcase, their plants).
inline constexpr std::int64_t kHubBookcaseWidthMm = 1800;
inline constexpr std::int64_t kHubBookcaseDepthMm = 340;
inline constexpr std::int64_t kHubBookcaseGapMm = 30;
inline constexpr std::int64_t kHubBookcaseCoreClearMm = 3000;
/// The runner: from this far east of the elevator (the halls' runner start) to this far short of the landmark's rim,
/// on from as far east of it, while this far short of the east balcony's edge.
inline constexpr std::int64_t kHubRunnerStartMm = 1800;
inline constexpr std::int64_t kHubLandmarkClearMm = 300;
inline constexpr std::int64_t kHubRunnerEndClearMm = 600;
/// The hub's lights: how far below the ceiling the core's hang (one by the elevator kHubEndLightXMm from the west wall
/// and its mirror by the entrance, kHubSideLightXMm either side of the core's middle kHubInnerLightZMm off the centre
/// line, three over the middle at 0 and kHubOuterLightZMm either side, a longer hub's along its centre line every
/// kHallLightPitchMm), how high those under the balcony are and how far from their wall (kHubSideLightXMm either side
/// of the core's middle).
inline constexpr std::int64_t kHubLightDropMm = 800;
inline constexpr std::int64_t kHubEndLightXMm = 1500;
inline constexpr std::int64_t kHubSideLightXMm = 4500;
inline constexpr std::int64_t kHubInnerLightZMm = 1450;
inline constexpr std::int64_t kHubOuterLightZMm = 4350;
inline constexpr std::int64_t kHubLowLightHeightMm = 2600;
inline constexpr std::int64_t kHubLowLightFromWallMm = 600;
/// The gallery's sconces (above the balcony on the hub's long walls, one every column bay): their centre's height.
inline constexpr std::int64_t kHubGallerySconceMm = 4250;

/// What a piece of the hub is (see the file comment).
enum class HubPieceKind : std::uint8_t {
    Column,   ///< a column under the balcony's edge, rising to the ceiling
    Landmark, ///< the glowing globe in its railed circular showcase
    Totem,    ///< the directory's totem
    Showcase, ///< the favourites' showcase (its cases are runtime proxies)
    Screen,   ///< an information screen on a wall (flat: nothing on the floor)
    Sofa,     ///< a sofa facing the landmark
    Plant,    ///< a large potted plant
    Rug,      ///< a rug (walked over)
    Bookcase, ///< a lit bookcase of books against a long wall, under the balcony (the museum-library)
};

/// A piece of the hub (see the file comment), in the floor's frame.
struct HubPiece {
    /// "landmark", "totem", "showcase", "screen", "column-north-<x>", "column-east-<z>", "sofa-west", "rug-runner-<k>",
    /// "plant-elevator-north", ... (x and z in millimetres).
    std::string id;
    HubPieceKind kind = HubPieceKind::Column;
    /// Its middle on the floor, and where its front faces (hundredths of a degree about +Y, the scene's: 0 towards +Z,
    /// 9000 towards +X, 18000 towards -Z, 27000 towards -X).
    FloorPoint centre;
    std::int32_t yawCdeg = 0;
    /// What it takes on the floor (a screen: its width along its wall, its depth off it).
    Footprint footprint;

    /// It stands on the floor: a collision and navigation obstacle.
    [[nodiscard]] bool solid() const noexcept { return kind != HubPieceKind::Screen && kind != HubPieceKind::Rug; }

    bool operator==(const HubPiece&) const = default;
};

/// The hub (see the file comment): the balcony's edges, its pieces, the totem's zone.
struct Hub {
    /// The balcony's edges: the long ones' z (north negative), the east one's x.
    std::int64_t northEdgeZMm = 0;
    std::int64_t southEdgeZMm = 0;
    std::int64_t eastEdgeXMm = 0;
    /// The core's middle along x (HubLayout::lengthMm / 2): the landmark's, the showcase's.
    std::int64_t centreXMm = 0;
    /// Columns (north edge, south edge, east edge), then the landmark, the totem, the showcase, the screen, the sofas,
    /// the rugs and the plants; those a doorway's approach takes left out.
    std::vector<HubPiece> pieces;
    /// The totem's proximity zone (before its screen, like the elevator's) and the middle of its screen on the floor.
    Footprint totemZone;
    FloorPoint totemFace;

    /// The piece `id`, or null.
    [[nodiscard]] const HubPiece* find(std::string_view id) const noexcept;

    bool operator==(const Hub&) const = default;
};

/// A floor's hall (see the file comment).
struct Hall {
    std::string floor;
    /// The ground floor's hall, the hub (I6 step 5; I4's lobby).
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
    /// The lights' places (x, z) on the centre line, west to east; their height is heightMm - lightDropMm. The hub's
    /// over its core (lightDropMm kHubLightDropMm), and its lights under the balcony (kHubLowLightHeightMm high).
    std::vector<FloorPoint> lights;
    std::int64_t lightDropMm = kHallLightDropMm;
    std::vector<FloorPoint> lowLights;
    /// The elevator's frame on the west wall (what stands proud of it), its proximity zone, and the arrival point.
    Footprint elevator;
    Footprint elevatorZone;
    FloorPoint arrival;
    /// The lobby's entrance doors in its east wall (z from minZMm to maxZMm); empty for a hall.
    Footprint entrance;
    /// The dressing (see the file comment): the north edge's slot pieces west to east, the south edge's, then each
    /// doorway's sconces by room id.
    std::vector<HallPiece> dressing;
    /// The hub's own (the ground floor's hall only).
    std::optional<Hub> hub;

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
/// the floor's frame, from the floor up to `heightMm`, a doorway's passage up to its head (`passageHeightMm`), so that
/// the portal it shares with the hall is the opening (I6 step 1: the lights through it are clipped to it).
struct FloorArea {
    /// A room's id ("room:snes"), or hallAreaId().
    std::string id;
    std::vector<Footprint> footprints;
    std::int64_t heightMm = 0;
    /// The height of the footprints after the first (a room's doorway passage); 0: heightMm.
    std::int64_t passageHeightMm = 0;

    bool operator==(const FloorArea&) const = default;
};

/// The areas of a floor: its rooms' (those with a template, by id: the interior on the floor, then the passage of the
/// doorway) and the hall's, last.
[[nodiscard]] std::vector<FloorArea> floorAreas(const Hall& hall, std::span<const FloorRoom> rooms);

/// The room's frame to its floor's, in meters (ADR-0002): a half turn about +Y for the south row, then the placement's
/// translation (x and z in millimetres / 1000). Quarter turns exact.
[[nodiscard]] glm::mat4 roomToFloorMatrix(const RoomPlacement& placement) noexcept;

} // namespace rv::world
