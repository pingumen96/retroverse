#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::formatNumber;
using detail::outOfRange;
using detail::translation;

constexpr std::string_view kKind = "room_shell";
/// Door lining (jambs and head) filling the rough opening around the door rectangle.
constexpr float kLiningThickness = 0.02f;
/// Casing (architrave) framing the opening on the room side.
constexpr float kCasingWidth = 0.07f;
constexpr float kCasingDepth = 0.018f;
constexpr float kCasingChamfer = 0.004f;
constexpr float kBaseboardDepth = 0.012f;
constexpr float kBaseboardChamfer = 0.003f;
constexpr float kMinWallThickness = 0.02f;
/// Depth of the doorway stop collider, in the outer face of the opening: the thinnest wall, so it stays in the wall.
constexpr float kDoorStopDepth = kMinWallThickness;
/// Pieces thinner than this are left out instead of producing degenerate boxes.
constexpr float kMinPiece = 1e-4f;

/// One wall in "along / outward" coordinates: `along` and `outward` are world axes (0 = X, 2 = Z).
struct Wall {
    WallSide side;
    int along;
    int outward;
    float sign;     ///< +1 when the wall lies on the positive side of `outward`
    float interior; ///< distance from the room center to the interior face
    float start;    ///< extent along `along` (north/south walls also cover the corners)
    float end;
};

[[nodiscard]] FaceMask faceOf(int axis, float sign) {
    switch (axis) {
    case 0:
        return sign > 0.0f ? FaceMask::PosX : FaceMask::NegX;
    case 1:
        return sign > 0.0f ? FaceMask::PosY : FaceMask::NegY;
    default:
        return sign > 0.0f ? FaceMask::PosZ : FaceMask::NegZ;
    }
}

struct Placement {
    glm::vec3 center{0.0f};
    glm::vec3 size{0.0f};
};

/// Box spanning [a0, a1] along the wall, [y0, y1] vertically and [d0, d1] of distance from the room center along
/// the wall's outward axis.
[[nodiscard]] Placement onWall(const Wall& wall, float a0, float a1, float y0, float y1, float d0, float d1) {
    Placement p;
    p.center[wall.along] = 0.5f * (a0 + a1);
    p.size[wall.along] = a1 - a0;
    p.center.y = 0.5f * (y0 + y1);
    p.size.y = y1 - y0;
    p.center[wall.outward] = wall.sign * 0.5f * (d0 + d1);
    p.size[wall.outward] = d1 - d0;
    return p;
}

[[nodiscard]] bool isAlongX(WallSide side) {
    return side == WallSide::North || side == WallSide::South;
}

[[nodiscard]] std::string_view wallName(WallSide side) {
    switch (side) {
    case WallSide::North:
        return "north";
    case WallSide::South:
        return "south";
    case WallSide::East:
        return "east";
    case WallSide::West:
        return "west";
    }
    return "wall";
}

/// Directions in which the wall parts beside a doorway extend: towards -along and +along.
[[nodiscard]] std::pair<std::string_view, std::string_view> sideNames(const Wall& wall) {
    return wall.along == 0 ? std::pair<std::string_view, std::string_view>{"west", "east"}
                           : std::pair<std::string_view, std::string_view>{"north", "south"};
}

Status validate(const RoomShellParams& p) {
    if (Status status = firstError({
            checkRange(kKind, "size", p.size, glm::vec2(0.5f), glm::vec2(200.0f)),
            checkRange(kKind, "wallHeight", p.wallHeight, 1.0f, 50.0f),
            checkRange(kKind, "wallThickness", p.wallThickness, kMinWallThickness, 2.0f),
            checkRange(kKind, "metersPerTile", p.metersPerTile, 0.01f, 100.0f),
            checkRange(kKind, "baseboardHeight", p.baseboardHeight, 0.0f, 0.5f),
        });
        !status) {
        return status;
    }
    if (!p.doorway) {
        return ok();
    }
    const DoorwayParams& door = *p.doorway;
    if (door.wall != WallSide::North && door.wall != WallSide::South && door.wall != WallSide::East &&
        door.wall != WallSide::West) {
        return outOfRange(kKind, "doorway.wall", "unknown wall");
    }
    if (Status status = firstError({
            checkRange(kKind, "doorway.offset", door.offset, -100.0f, 100.0f),
            checkRange(kKind, "doorway.width", door.width, 0.3f, 20.0f),
            checkRange(kKind, "doorway.height", door.height, 0.5f, 50.0f),
        });
        !status) {
        return status;
    }
    const float wallLength = isAlongX(door.wall) ? p.size.x : p.size.y;
    const float limit = 0.5f * wallLength - kBaseboardDepth;
    const float halfSpan = 0.5f * door.width + kCasingWidth;
    if (halfSpan > limit) {
        return outOfRange(kKind, "doorway.width",
                          "the opening with its casing (" + formatNumber(2.0f * halfSpan) + " m) does not fit the " +
                              formatNumber(wallLength) + " m wall");
    }
    if (std::fabs(door.offset) + halfSpan > limit) {
        return outOfRange(kKind, "doorway.offset",
                          "the opening with its casing reaches past the room corner (|offset| must be at most " +
                              formatNumber(limit - halfSpan) + " m)");
    }
    if (door.height + kCasingWidth > p.wallHeight) {
        return outOfRange(kKind, "doorway.height",
                          "the opening with its casing (" + formatNumber(door.height + kCasingWidth) +
                              " m) is taller than the wall (" + formatNumber(p.wallHeight) + " m)");
    }
    return ok();
}

ProceduralPrefab assemble(const RoomShellParams& p) {
    detail::PrefabAssembler out(kKind, {"floor", "wall", "ceiling", "trim"});
    MeshBuilder& floor = out.part("floor");
    MeshBuilder& walls = out.part("wall");
    MeshBuilder& trim = out.part("trim");

    const float halfX = 0.5f * p.size.x;
    const float halfZ = 0.5f * p.size.y;
    const float thickness = p.wallThickness;
    const float height = p.wallHeight;
    const float tile = p.metersPerTile;

    // Floor and ceiling are single faces at y = 0 and y = height, uv in meters.
    floor.box(glm::vec3(p.size.x, 0.0f, p.size.y), glm::mat4(1.0f), BoxUv::WorldMeters, tile, FaceMask::PosY);
    if (p.ceiling) {
        out.part("ceiling").box(glm::vec3(p.size.x, 0.0f, p.size.y), translation(glm::vec3(0.0f, height, 0.0f)),
                                BoxUv::WorldMeters, tile, FaceMask::NegY);
    }

    const std::array<Wall, 4> sides{{
        {WallSide::North, 0, 2, -1.0f, halfZ, -halfX - thickness, halfX + thickness},
        {WallSide::South, 0, 2, 1.0f, halfZ, -halfX - thickness, halfX + thickness},
        {WallSide::East, 2, 0, 1.0f, halfX, -halfZ, halfZ},
        {WallSide::West, 2, 0, -1.0f, halfX, -halfZ, halfZ},
    }};
    std::vector<PrefabCollider> colliders;
    for (const Wall& wall : sides) {
        const FaceMask endFaces = faceOf(wall.along, 1.0f) | faceOf(wall.along, -1.0f);
        // Wall bottoms rest on the floor; east and west walls end against the north and south walls.
        FaceMask wallFaces = FaceMask::All & ~FaceMask::NegY;
        if (wall.along == 2) {
            wallFaces = wallFaces & ~endFaces;
        }
        const auto addWall = [&](float a0, float a1, float y0, float y1, FaceMask faces) {
            if (a1 - a0 < kMinPiece || y1 - y0 < kMinPiece) {
                return;
            }
            const Placement b = onWall(wall, a0, a1, y0, y1, wall.interior, wall.interior + thickness);
            walls.box(b.size, translation(b.center), BoxUv::WorldMeters, tile, faces);
        };
        // Solid volume of the wall for collision: the full slab, or the parts around the door rectangle.
        const std::string wallId(wallName(wall.side));
        const auto addSolid = [&](std::string name, float a0, float a1, float y0, float y1) {
            if (a1 - a0 < kMinPiece || y1 - y0 < kMinPiece) {
                return;
            }
            const Placement b = onWall(wall, a0, a1, y0, y1, wall.interior, wall.interior + thickness);
            colliders.push_back(
                PrefabCollider{std::move(name), core::Aabb::fromCenterHalfExtents(b.center, 0.5f * b.size)});
        };
        const auto addBaseboard = [&](float a0, float a1) {
            if (p.baseboardHeight < kMinPiece || a1 - a0 < kMinPiece) {
                return;
            }
            const Placement b =
                onWall(wall, a0, a1, 0.0f, p.baseboardHeight, wall.interior - kBaseboardDepth, wall.interior);
            trim.chamferedBox(b.size, kBaseboardChamfer, translation(b.center));
        };
        // North and south baseboards run the full interior width; east and west ones stop against them.
        const float boardStart = wall.along == 0 ? -halfX : -halfZ + kBaseboardDepth;
        const float boardEnd = wall.along == 0 ? halfX : halfZ - kBaseboardDepth;

        if (!p.doorway || p.doorway->wall != wall.side) {
            addWall(wall.start, wall.end, 0.0f, height, wallFaces);
            addSolid(wallId, wall.start, wall.end, 0.0f, height);
            addBaseboard(boardStart, boardEnd);
            continue;
        }

        // Doorway: the door rectangle [open0, open1] x [0, door.height] stays empty. The wall leaves a rough
        // opening one lining thickness larger; lining boxes fill that margin and a casing frames it in the room.
        const DoorwayParams& door = *p.doorway;
        const float open0 = door.offset - 0.5f * door.width;
        const float open1 = door.offset + 0.5f * door.width;
        const float rough0 = open0 - kLiningThickness;
        const float rough1 = open1 + kLiningThickness;
        const float roughTop = door.height + kLiningThickness;
        addWall(wall.start, rough0, 0.0f, height, wallFaces & ~faceOf(wall.along, 1.0f));
        addWall(rough1, wall.end, 0.0f, height, wallFaces & ~faceOf(wall.along, -1.0f));
        addWall(rough0, rough1, roughTop, height, wallFaces & ~endFaces);
        // The lining belongs to the solid parts: only the door rectangle stays passable.
        const auto [lowSide, highSide] = sideNames(wall);
        addSolid(wallId + "_" + std::string(lowSide), wall.start, open0, 0.0f, height);
        addSolid(wallId + "_" + std::string(highSide), open1, wall.end, 0.0f, height);
        addSolid(wallId + "_above", open0, open1, door.height, height);

        const float inner = wall.interior;
        const float outer = wall.interior + thickness;
        // Nothing lies beyond the opening (no floor): a thin stop in its outer face lets the walker step into the
        // doorway but not out of the room. It is a box of its own, so connecting a room later removes only this one.
        const Placement stop = onWall(wall, open0, open1, 0.0f, door.height, outer - kDoorStopDepth, outer);
        colliders.push_back(
            PrefabCollider{wallId + "_doorway", core::Aabb::fromCenterHalfExtents(stop.center, 0.5f * stop.size)});

        const FaceMask jambFaces = FaceMask::All & ~FaceMask::NegY & ~FaceMask::PosY;
        const Placement leftJamb = onWall(wall, rough0, open0, 0.0f, door.height, inner, outer);
        trim.box(leftJamb.size, translation(leftJamb.center), BoxUv::PerFace, 1.0f,
                 jambFaces & ~faceOf(wall.along, -1.0f));
        const Placement rightJamb = onWall(wall, open1, rough1, 0.0f, door.height, inner, outer);
        trim.box(rightJamb.size, translation(rightJamb.center), BoxUv::PerFace, 1.0f,
                 jambFaces & ~faceOf(wall.along, 1.0f));
        const Placement head = onWall(wall, rough0, rough1, door.height, roughTop, inner, outer);
        trim.box(head.size, translation(head.center), BoxUv::PerFace, 1.0f,
                 FaceMask::All & ~FaceMask::PosY & ~endFaces);

        const float casingFront = inner - kCasingDepth;
        const Placement leftStile = onWall(wall, open0 - kCasingWidth, open0, 0.0f, door.height, casingFront, inner);
        trim.chamferedBox(leftStile.size, kCasingChamfer, translation(leftStile.center));
        const Placement rightStile = onWall(wall, open1, open1 + kCasingWidth, 0.0f, door.height, casingFront, inner);
        trim.chamferedBox(rightStile.size, kCasingChamfer, translation(rightStile.center));
        const Placement casingHead = onWall(wall, open0 - kCasingWidth, open1 + kCasingWidth, door.height,
                                            door.height + kCasingWidth, casingFront, inner);
        trim.chamferedBox(casingHead.size, kCasingChamfer, translation(casingHead.center));

        // The floor continues through the opening (same world-meter uvs as the room floor).
        const Placement threshold = onWall(wall, open0, open1, 0.0f, 0.0f, inner, outer);
        floor.box(threshold.size, translation(threshold.center), BoxUv::WorldMeters, tile, FaceMask::PosY);

        addBaseboard(boardStart, open0 - kCasingWidth);
        addBaseboard(open1 + kCasingWidth, boardEnd);

        glm::vec3 outward(0.0f);
        outward[wall.outward] = wall.sign;
        glm::vec3 position(0.0f);
        position[wall.along] = door.offset;
        position[wall.outward] = wall.sign * (inner + 0.5f * thickness);
        out.addSocket("Doorway", SocketKind::Anchor, detail::frameFacing(outward, position),
                      core::Aabb::fromMinMax(glm::vec3(-0.5f * door.width, 0.0f, -0.5f * thickness),
                                             glm::vec3(0.5f * door.width, door.height, 0.5f * thickness)));
    }
    out.setColliders(std::move(colliders));
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildRoomShell(const RoomShellParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
