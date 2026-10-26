#include <rv/world/floor_layout.hpp>

#include <algorithm>
#include <cstdlib>
#include <optional>
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

std::string_view pieceName(HallPieceKind kind) noexcept {
    switch (kind) {
    case HallPieceKind::Sconce:
        return "sconce";
    case HallPieceKind::Picture:
        return "picture";
    case HallPieceKind::Bench:
        return "bench";
    case HallPieceKind::Plant:
        return "plant";
    }
    return "sconce";
}

/// A piece against an edge's wall at `x`: `width` along x, `depth` into the hall from the wall's face (from `gap` off
/// it), on the north edge (z = -half) or the south edge (z = +half).
Footprint againstWall(bool north, std::int64_t half, std::int64_t x, std::int64_t width, std::int64_t gap,
                      std::int64_t depth) noexcept {
    const std::int64_t near = north ? -half + gap : half - gap;
    const std::int64_t far = north ? -half + gap + depth : half - gap - depth;
    return {x - width / 2, std::min(near, far), x - width / 2 + width, std::max(near, far)};
}

/// True when [from, to] comes within `margin` of the opening of a doorway in `doorways` (those that open onto the
/// hall; on the edge `north` only unless `anyEdge`).
bool nearDoorway(std::span<const FloorDoorway> doorways, bool north, bool anyEdge, std::int64_t from, std::int64_t to,
                 std::int64_t margin) noexcept {
    return std::any_of(doorways.begin(), doorways.end(), [&](const FloorDoorway& doorway) {
        if (!doorway.opensOntoHall() || (!anyEdge && (doorway.hallEdgeZMm < 0) != north)) {
            return false;
        }
        return from < doorway.maxMm + margin && to > doorway.minMm - margin;
    });
}

/// How deep a floor piece's footprint reaches into the hall from its edge's wall.
std::int64_t reach(const Footprint& footprint, bool north, std::int64_t half) noexcept {
    return north ? footprint.maxZMm + half : half - footprint.minZMm;
}

/// True when [from, to] on the edge `north` comes within `margin` of a hub piece that stands against that wall (the
/// showcase, the screen).
bool nearHubWallPiece(const Hall& hall, bool north, std::int64_t from, std::int64_t to, std::int64_t margin) noexcept {
    if (!hall.hub) {
        return false;
    }
    const std::int64_t half = hall.widthMm / 2;
    return std::any_of(hall.hub->pieces.begin(), hall.hub->pieces.end(), [&](const HubPiece& piece) {
        const bool againstWall = north ? piece.footprint.minZMm <= -half + kHubShowcaseGapMm
                                       : piece.footprint.maxZMm >= half - kHubShowcaseGapMm;
        const bool onWall = piece.kind == HubPieceKind::Showcase || piece.kind == HubPieceKind::Screen ||
                            piece.kind == HubPieceKind::Bookcase;
        return onWall && againstWall && from < piece.footprint.maxXMm + margin && to > piece.footprint.minXMm - margin;
    });
}

/// The hall's dressing (see the header's file comment), from its east end and its doorways (the hub: the slots' wall
/// pieces off its showcase and screen, no floor piece, and a sconce on the gallery's walls every column bay).
std::vector<HallPiece> hallDressing(const Hall& hall) {
    std::vector<HallPiece> pieces;
    const std::int64_t half = hall.widthMm / 2;
    const std::int64_t east = hall.interior.maxXMm;
    for (const bool north : {true, false}) {
        const std::string edge = north ? "north-" : "south-";
        for (std::int64_t slot = 0;; ++slot) {
            const std::int64_t x = kHallSlotFirstMm + slot * kHallSlotPitchMm;
            if (x + kHallEndClearMm >= east) {
                break;
            }
            const std::string id = edge + std::to_string(slot);
            // The wall piece: a sconce and a picture in turn, the south edge the reverse of the north; an edge's
            // pictures portrait and landscape in turn.
            const bool sconce = (slot % 2 == 0) == north;
            const bool landscape = !sconce && (slot / 2) % 2 == 1;
            const HallPieceKind wallKind = sconce ? HallPieceKind::Sconce : HallPieceKind::Picture;
            const std::int64_t width =
                sconce ? kHallSconceWidthMm : (landscape ? kHallPictureLandscapeWidthMm : kHallPictureWidthMm);
            const std::int64_t margin = sconce ? kHallSconceDoorMarginMm : kHallPictureDoorMarginMm;
            const std::int64_t from = x - width / 2;
            const std::int64_t to = from + width;
            if (from >= kHallEndClearMm && to + kHallEndClearMm <= east &&
                !nearDoorway(hall.doorways, north, false, from, to, margin) &&
                !nearHubWallPiece(hall, north, from, to, kHallPictureDoorMarginMm)) {
                pieces.push_back(HallPiece{id + "/" + std::string(pieceName(wallKind)), wallKind, north, slot,
                                           landscape, x,
                                           againstWall(north, half, x, width, 0,
                                                       sconce ? kHallSconceDepthMm : kHallPictureDepthMm)});
            }
            // The floor piece, every kHallFloorPeriod slots: a bench with a plant beside it, on each edge in turn.
            const std::int64_t phase = slot % kHallFloorPeriod;
            std::optional<HallPieceKind> floorKind;
            if (phase == (north ? kHallNorthBenchSlot : kHallSouthBenchSlot)) {
                floorKind = HallPieceKind::Bench;
            } else if (phase == (north ? kHallNorthPlantSlot : kHallSouthPlantSlot)) {
                floorKind = HallPieceKind::Plant;
            }
            if (!floorKind || hall.hub) {
                continue;
            }
            const bool bench = *floorKind == HallPieceKind::Bench;
            const std::int64_t length = bench ? kHallBenchLengthMm : 2 * kHallPlantSpreadMm;
            const Footprint footprint =
                bench ? againstWall(north, half, x, length, 0, kHallBenchDepthMm)
                      : againstWall(north, half, x, length, kHallPlantOffsetMm - kHallPlantSpreadMm,
                                    2 * kHallPlantSpreadMm);
            // The main passage stays kHallMinClearMm wide: from the wall, or from what the other edge holds beside it
            // (the north edge's pieces are laid first).
            std::int64_t across = 0;
            for (const HallPiece& other : pieces) {
                if (other.solid() && other.north != north && other.footprint.minXMm < footprint.maxXMm &&
                    other.footprint.maxXMm > footprint.minXMm) {
                    across = std::max(across, reach(other.footprint, other.north, half));
                }
            }
            if (footprint.minXMm >= kHallFloorFirstMm && footprint.maxXMm + kHallFloorEndClearMm <= east &&
                hall.widthMm - reach(footprint, north, half) - across >= kHallMinClearMm &&
                !nearDoorway(hall.doorways, north, true, footprint.minXMm, footprint.maxXMm, kHallDoorApproachMm)) {
                pieces.push_back(HallPiece{id + "/" + std::string(pieceName(*floorKind)), *floorKind, north, slot,
                                           false, x, footprint});
            }
        }
    }
    // Each doorway's sconces beside its opening (doorways by room id).
    for (const FloorDoorway& doorway : hall.doorways) {
        if (!doorway.opensOntoHall()) {
            continue;
        }
        const bool north = doorway.hallEdgeZMm < 0;
        const std::string prefix = "portal:" + doorway.room.toString() + "/sconce-";
        for (const auto& [side, x] : {std::pair{std::string_view("west"), doorway.minMm - kHallFlankSconceMm},
                                      std::pair{std::string_view("east"), doorway.maxMm + kHallFlankSconceMm}}) {
            pieces.push_back(HallPiece{prefix + std::string(side), HallPieceKind::Sconce, north, -1, false, x,
                                       againstWall(north, half, x, kHallSconceWidthMm, 0, kHallSconceDepthMm)});
        }
    }
    // The hub's gallery: a sconce on each long wall above the balcony in the middle of every column bay, while short of
    // the east balcony.
    if (hall.hub) {
        for (const bool north : {true, false}) {
            for (std::int64_t x = kHubColumnPitchMm / 2; x + kHubColumnPitchMm / 2 <= hall.hub->eastEdgeXMm;
                 x += kHubColumnPitchMm) {
                HallPiece piece{std::string(north ? "gallery-north-" : "gallery-south-") + std::to_string(x) +
                                    "/sconce",
                                HallPieceKind::Sconce,
                                north,
                                -1,
                                false,
                                x,
                                againstWall(north, half, x, kHallSconceWidthMm, 0, kHallSconceDepthMm)};
                piece.centreMm = kHubGallerySconceMm;
                pieces.push_back(std::move(piece));
            }
        }
    }
    return pieces;
}

/// A square footprint of side `side` around (x, z).
Footprint square(std::int64_t x, std::int64_t z, std::int64_t side) noexcept {
    return {x - side / 2, z - side / 2, x - side / 2 + side, z - side / 2 + side};
}

/// A footprint `alongX` wide along x and `alongZ` along z around (x, z).
Footprint around(std::int64_t x, std::int64_t z, std::int64_t alongX, std::int64_t alongZ) noexcept {
    return {x - alongX / 2, z - alongZ / 2, x - alongX / 2 + alongX, z - alongZ / 2 + alongZ};
}

/// The hub's own (see the header's file comment), from its interior and its doorways.
Hub deriveHub(const Hall& hall, const HubLayout& layout) {
    Hub hub;
    const std::int64_t half = hall.widthMm / 2;
    const std::int64_t east = hall.interior.maxXMm;
    hub.northEdgeZMm = -half + kHubBalconyDepthMm;
    hub.southEdgeZMm = half - kHubBalconyDepthMm;
    hub.eastEdgeXMm = east - kHubBalconyDepthMm;
    hub.centreXMm = layout.lengthMm / 2;
    const std::int64_t cx = hub.centreXMm;

    // The doorways' approaches: no solid piece and no wall piece stands in one.
    std::vector<Footprint> approaches;
    for (const FloorDoorway& doorway : hall.doorways) {
        if (!doorway.opensOntoHall()) {
            continue;
        }
        const std::int64_t inward = doorway.hallEdgeZMm < 0 ? 1 : -1;
        const std::int64_t far = doorway.hallEdgeZMm + inward * kHubApproachDepthMm;
        approaches.push_back({doorway.minMm - kHubApproachMm, std::min(doorway.hallEdgeZMm, far),
                              doorway.maxMm + kHubApproachMm, std::max(doorway.hallEdgeZMm, far)});
    }
    const auto add = [&](HubPiece piece) {
        const bool blocked =
            piece.kind != HubPieceKind::Rug && std::any_of(approaches.begin(), approaches.end(), [&](const Footprint& a) {
                return a.overlaps(piece.footprint);
            });
        if (!blocked) {
            hub.pieces.push_back(std::move(piece));
        }
    };

    // The columns under the balcony's edges: along the long edges at the pitch, none before a doorway or the
    // showcase (which stands between two of them); the east edge's corners and either side of the entrance.
    const std::int64_t showcaseX = cx - kHubShowcaseOffsetMm;
    const auto opening = [&](bool north, std::int64_t x) {
        const bool doorway = std::any_of(hall.doorways.begin(), hall.doorways.end(), [&](const FloorDoorway& door) {
            return door.opensOntoHall() && (door.hallEdgeZMm < 0) == north &&
                   std::abs((door.minMm + door.maxMm) / 2 - x) < kHubColumnClearMm;
        });
        return doorway || (!north && std::abs(showcaseX - x) < (kHubShowcaseWidthMm + kHubColumnSizeMm) / 2);
    };
    for (const bool north : {true, false}) {
        const std::int64_t z = north ? hub.northEdgeZMm : hub.southEdgeZMm;
        for (std::int64_t x = kHubColumnPitchMm; x + kHubColumnEndClearMm <= hub.eastEdgeXMm; x += kHubColumnPitchMm) {
            if (!opening(north, x)) {
                add({std::string(north ? "column-north-" : "column-south-") + std::to_string(x), HubPieceKind::Column,
                     {x, z}, 0, square(x, z, kHubColumnSizeMm)});
            }
        }
    }
    for (const std::int64_t z : {hub.northEdgeZMm, -kHubEastColumnZMm, kHubEastColumnZMm, hub.southEdgeZMm}) {
        add({"column-east-" + std::to_string(z), HubPieceKind::Column, {hub.eastEdgeXMm, z}, 0,
             square(hub.eastEdgeXMm, z, kHubColumnSizeMm)});
    }

    // The landmark, the totem facing the elevator, the favourites' showcase and the screen above it.
    add({"landmark", HubPieceKind::Landmark, {cx, 0}, 0, square(cx, 0, 2 * kHubLandmarkRadiusMm)});
    const FloorPoint totem{cx - kHubTotemFromCentreMm, kHubTotemZMm};
    add({"totem", HubPieceKind::Totem, totem, 27000,
         around(totem.xMm, totem.zMm, kHubTotemDepthMm, kHubTotemWidthMm)});
    hub.totemFace = {totem.xMm - kHubTotemDepthMm / 2, totem.zMm};
    hub.totemZone = {hub.totemFace.xMm - kHubTotemZoneDepthMm, totem.zMm - kHubTotemZoneWidthMm / 2, hub.totemFace.xMm,
                     totem.zMm + kHubTotemZoneWidthMm / 2};
    const std::int64_t showcaseZ = half - kHubShowcaseGapMm - kHubShowcaseDepthMm / 2;
    add({"showcase", HubPieceKind::Showcase, {showcaseX, showcaseZ}, 18000,
         around(showcaseX, showcaseZ, kHubShowcaseWidthMm, kHubShowcaseDepthMm)});
    add({"screen", HubPieceKind::Screen, {showcaseX, half}, 18000,
         {showcaseX - kHubScreenWidthMm / 2, half - kHallPictureDepthMm, showcaseX + kHubScreenWidthMm / 2, half}});

    // The sofas facing the landmark on their rugs, the runner from the elevator to the landmark and on to the entrance,
    // the rug before the core's north wall.
    for (const auto& [side, x] : {std::pair{std::string_view("west"), cx - kHubSofaFromCentreMm},
                                  std::pair{std::string_view("east"), cx + kHubSofaFromCentreMm}}) {
        add({"sofa-" + std::string(side), HubPieceKind::Sofa, {x, kHubSofaZMm}, 18000,
             around(x, kHubSofaZMm, kHubSofaWidthMm, kHubSofaDepthMm)});
    }
    for (const auto& [side, x] : {std::pair{std::string_view("west"), cx - kHubSofaFromCentreMm},
                                  std::pair{std::string_view("east"), cx + kHubSofaFromCentreMm}}) {
        add({"rug-sofa-" + std::string(side), HubPieceKind::Rug, {x, kHubSofaRugZMm}, 0,
             around(x, kHubSofaRugZMm, kHubSofaRugWidthMm, kHubSofaRugDepthMm)});
    }
    std::int64_t runner = 0;
    const std::int64_t landmarkWest = cx - kHubLandmarkRadiusMm - kHubLandmarkClearMm;
    for (std::int64_t from = kHubRunnerStartMm; from + kHubRugLengthMm <= landmarkWest; from += kHubRugLengthMm) {
        const std::int64_t x = from + kHubRugLengthMm / 2;
        add({"rug-runner-" + std::to_string(runner++), HubPieceKind::Rug, {x, 0}, 0,
             around(x, 0, kHubRugLengthMm, kHubRugWidthMm)});
    }
    for (std::int64_t from = cx + kHubLandmarkRadiusMm + kHubLandmarkClearMm;
         from + kHubRugLengthMm + kHubRunnerEndClearMm <= hub.eastEdgeXMm; from += kHubRugLengthMm) {
        const std::int64_t x = from + kHubRugLengthMm / 2;
        add({"rug-runner-" + std::to_string(runner++), HubPieceKind::Rug, {x, 0}, 0,
             around(x, 0, kHubRugLengthMm, kHubRugWidthMm)});
    }
    const std::int64_t northRugZ = -half + kHubPortalRugFromWallMm;
    add({"rug-north", HubPieceKind::Rug, {cx, northRugZ}, 0, around(cx, northRugZ, kHubRugLengthMm, kHubRugWidthMm)});

    // The bookcases against the long walls, in the middle of every column bay off the core's middle.
    for (const bool north : {true, false}) {
        const std::int64_t z = north ? -half + kHubBookcaseGapMm + kHubBookcaseDepthMm / 2
                                     : half - kHubBookcaseGapMm - kHubBookcaseDepthMm / 2;
        for (std::int64_t x = kHubColumnPitchMm / 2; x + kHubBookcaseWidthMm / 2 + kHallEndClearMm <= east;
             x += kHubColumnPitchMm) {
            if (std::abs(x - cx) < kHubBookcaseCoreClearMm) {
                continue;
            }
            add({std::string(north ? "bookcase-north-" : "bookcase-south-") + std::to_string(x),
                 HubPieceKind::Bookcase, {x, z}, north ? 0 : 18000,
                 around(x, z, kHubBookcaseWidthMm, kHubBookcaseDepthMm)});
        }
    }
    // The plants: by the elevator, by the entrance, beside the showcase, beside the core's north wall's middle.
    const std::int64_t plantSide = 2 * kHubPlantSpreadMm;
    const auto plant = [&](std::string id, std::int64_t x, std::int64_t z) {
        add({"plant-" + std::move(id), HubPieceKind::Plant, {x, z}, 0, square(x, z, plantSide)});
    };
    plant("elevator-north", kHubPlantFromWallMm, -kHubPlantBesideMm);
    plant("elevator-south", kHubPlantFromWallMm, kHubPlantBesideMm);
    plant("entrance-north", east - kHubPlantFromWallMm, -kHubPlantBesideMm);
    plant("entrance-south", east - kHubPlantFromWallMm, kHubPlantBesideMm);
    plant("showcase-west", showcaseX - kHubPlantBesideShowcaseMm, half - kHubPlantFromWallMm);
    plant("showcase-east", showcaseX + kHubPlantBesideShowcaseMm, half - kHubPlantFromWallMm);
    plant("north-west", cx - kHubPlantBesideMm, -half + kHubPlantFromWallMm);
    plant("north-east", cx + kHubPlantBesideMm, -half + kHubPlantFromWallMm);
    return hub;
}

/// The hub's lights (see the header's file comment): over its core (and along a longer hub's middle every pitch), and
/// under the balcony.
void hubLights(Hall& hall) {
    const Hub& hub = *hall.hub;
    const std::int64_t cx = hub.centreXMm;
    const std::int64_t half = hall.widthMm / 2;
    hall.lightDropMm = kHubLightDropMm;
    hall.lights = {{kHubEndLightXMm, 0},
                   {cx - kHubSideLightXMm, -kHubInnerLightZMm},
                   {cx - kHubSideLightXMm, kHubInnerLightZMm},
                   {cx, -kHubOuterLightZMm},
                   {cx, 0},
                   {cx, kHubOuterLightZMm},
                   {cx + kHubSideLightXMm, -kHubInnerLightZMm},
                   {cx + kHubSideLightXMm, kHubInnerLightZMm}};
    for (std::int64_t x = 2 * cx - kHubEndLightXMm; x < hub.eastEdgeXMm; x += kHallLightPitchMm) {
        hall.lights.push_back({x, 0});
    }
    for (const std::int64_t x : {cx - kHubSideLightXMm, cx + kHubSideLightXMm}) {
        for (const std::int64_t z : {-half + kHubLowLightFromWallMm, half - kHubLowLightFromWallMm}) {
            hall.lowLights.push_back({x, z});
        }
    }
}

} // namespace

const HubPiece* Hub::find(std::string_view id) const noexcept {
    const auto found = std::find_if(pieces.begin(), pieces.end(), [&](const HubPiece& piece) { return piece.id == id; });
    return found != pieces.end() ? &*found : nullptr;
}

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

    if (hall.lobby) {
        hall.heightMm = layout.hub.heightMm;
    }
    std::int64_t east = std::max(kMinHallLengthMm, rowStartOf(floor, layout));
    if (hall.lobby) {
        east = std::max(east, layout.hub.lengthMm);
    }
    std::vector<std::pair<std::int64_t, std::int64_t>> north;
    std::vector<std::pair<std::int64_t, std::int64_t>> south;
    // The hub's walls above its rooms: each room's span on its edge and its height.
    struct Upper {
        bool north = true;
        std::int64_t from = 0;
        std::int64_t to = 0;
        std::int64_t bottom = 0;
    };
    std::vector<Upper> uppers;
    for (const FloorRoom& entry : rooms) {
        if (entry.room == nullptr || entry.room->floor != floor) {
            continue;
        }
        const Footprint footprint = floorFootprint(*entry.room, layout);
        east = std::max(east, footprint.maxXMm);
        const bool northRow = rowOf(entry.room->placement) == Row::North;
        (northRow ? north : south).emplace_back(footprint.minXMm, footprint.maxXMm);
        uppers.push_back({northRow, footprint.minXMm, footprint.maxXMm, entry.room->heightMm});
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
    // The hub closes its edges above its rooms (a hall is as high as its rooms).
    if (hall.lobby) {
        std::sort(uppers.begin(), uppers.end(),
                  [](const Upper& a, const Upper& b) { return std::pair(!a.north, a.from) < std::pair(!b.north, b.from); });
        std::size_t northUpper = 0;
        std::size_t southUpper = 0;
        for (const Upper& upper : uppers) {
            if (upper.bottom >= hall.heightMm) {
                continue;
            }
            const std::string id = upper.north ? "north-upper-" + std::to_string(northUpper++)
                                               : "south-upper-" + std::to_string(southUpper++);
            const Footprint footprint = upper.north ? Footprint{upper.from, -half - wall, upper.to, -half}
                                                    : Footprint{upper.from, half, upper.to, half + wall};
            hall.walls.push_back({id, footprint, upper.bottom});
        }
    } else {
        for (std::int64_t x = kHallLightPitchMm / 2; x < east; x += kHallLightPitchMm) {
            hall.lights.push_back({x, 0});
        }
    }
    const std::int64_t frameHalf = kElevatorDoorWidthMm / 2 + kElevatorFrameMm;
    hall.elevator = {0, -frameHalf, kElevatorFrameDepthMm, frameHalf};
    hall.elevatorZone = {0, -kElevatorZoneWidthMm / 2, kElevatorZoneDepthMm, kElevatorZoneWidthMm / 2};
    hall.arrival = {kArrivalXMm, 0};
    if (hall.lobby) {
        hall.entrance = {east, -kEntranceWidthMm / 2, east + wall, kEntranceWidthMm / 2};
        hall.hub = deriveHub(hall, layout.hub);
        hubLights(hall);
    }
    hall.dressing = hallDressing(hall);
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
                area.passageHeightMm = doorway.heightMm;
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
