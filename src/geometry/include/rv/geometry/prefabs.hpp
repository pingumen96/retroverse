#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>
#include <rv/geometry/mesh_data.hpp>
#include <rv/geometry/socket.hpp>

namespace rv::geometry {

/// One mesh of a procedural prefab, drawn with the material the scene assigns to `materialRole`.
struct PrefabPart {
    /// Named "<kind>/<role>"; bounds and tangents computed.
    MeshData mesh;
    std::string materialRole;
};

/// Procedural fallback prefab (style guide §7.3 tier C, §8 "Fallback" column): a few meshes, one per material role
/// (one draw per role), plus sockets in prefab-local space. Built by pure deterministic functions; GPU-free.
///
/// Conventions (ADR-0002): meters, +Y up, the front faces +Z. Pivot "floor": origin at floor level (bounds.min.y
/// = 0) in the middle of the footprint; pivot "center": origin at the bounding-box center. Socket transforms are
/// rigid (rotation + translation) relative to the prefab root; Socket::bounds is expressed in the socket's own
/// frame; mesh-backed sockets (ScreenSurface) set Socket::meshIndex to the index of their mesh in `parts`. Parts
/// appear in the role order documented for each kind, and only when they contain geometry.
struct ProceduralPrefab {
    std::string kind;
    std::vector<PrefabPart> parts;
    std::vector<Socket> sockets;
    /// Union of the part bounds.
    core::Aabb bounds;
};

/// First part with the given material role, or nullptr.
[[nodiscard]] const PrefabPart* findPart(const ProceduralPrefab& prefab, std::string_view role) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// Parameters. Lengths in meters; the JSON keys are the field names. Ranges are inclusive; violations return
// ErrorCode::OutOfRange with context "<kind>.<key>".
// ---------------------------------------------------------------------------------------------------------------

/// Wall of a room_shell, named by its direction from the room center: north = -Z, south = +Z, east = +X,
/// west = -X. JSON: "north", "south", "east", "west".
enum class WallSide { North, South, East, West };

/// Opening in a room_shell wall. The door rectangle (width x height, from the floor) stays completely open; its
/// lining and casing are built outside it.
struct DoorwayParams {
    WallSide wall = WallSide::South;
    /// Opening center relative to the wall center, along world +X for north/south walls and +Z for east/west
    /// walls. The opening and its 7 cm casing must stay 12 mm clear of the room corners.
    float offset = 0.0f;
    float width = 1.6f;  ///< [0.3, 20]
    float height = 2.3f; ///< [0.5, 50]; height + 7 cm casing <= wallHeight
};

/// "room_shell", pivot floor. The interior rectangle is exactly `size` (x, z) centered on the origin; walls are
/// built outside it, so bounds = [-x/2 - t, 0, -z/2 - t] .. [x/2 + t, wallHeight, z/2 + t].
/// Roles: "floor", "wall", "ceiling", "trim" (baseboards, door lining and casing). Interior faces point into the
/// room; floor, wall and ceiling uvs are in meters / metersPerTile (seamless across wall segments).
/// Sockets: "Doorway" (Anchor) when a doorway exists: at the center of the opening at floor level (middle of the
/// wall thickness), +Z pointing out of the room, +Y up; bounds = opening volume
/// [-width/2, 0, -t/2] .. [width/2, height, t/2].
struct RoomShellParams {
    glm::vec2 size{9.0f, 10.0f}; ///< interior x and z, each [0.5, 200]
    float wallHeight = 3.0f;     ///< [1, 50]
    float wallThickness = 0.15f; ///< t, [0.02, 2]
    float metersPerTile = 1.0f;  ///< [0.01, 100]
    bool ceiling = true;
    float baseboardHeight = 0.1f; ///< [0, 0.5]; 0 removes the baseboards
    /// nullopt (JSON null) builds a closed room.
    std::optional<DoorwayParams> doorway = DoorwayParams{};
};

/// "shelf", pivot floor, open side +Z; bounds = width x height x depth.
/// Roles: "wood_primary" (sides, boards, top, plinth), "wood_secondary" (back panel, present when `back`).
/// Sockets: "Slot_<level>_<index>" (ShelfSlot): level 0 is the lowest board, index 0 the leftmost (-X) slot.
/// innerWidth = width - 2 * boardThickness; slotsPerLevel = floor((innerWidth - 2 * slotMargin + slotGap) /
/// (slotSize.x + slotGap)) (1e-4 tolerance for exact fits); slots are left-aligned from x = -innerWidth / 2 +
/// slotMargin. The `levels` boards are evenly spaced between the plinth top and the top board (each compartment
/// is (height - boardThickness - plinthHeight) / levels tall, board included). Socket transform = translation to
/// the slot's bottom-center point on the board, centered in the board depth; Socket::bounds = item volume
/// [-w/2, 0, -d/2] .. [w/2, h, d/2].
struct ShelfParams {
    float width = 1.2f;            ///< [0.2, 10]
    float height = 2.0f;           ///< [0.2, 5]
    float depth = 0.35f;           ///< [0.08, 2]
    int levels = 5;                ///< [1, 100]
    float boardThickness = 0.025f; ///< [0.005, 0.1]
    bool back = true;
    /// [0, 0.5] and below height - 2 * boardThickness, so that one compartment fits (an excess is reported on
    /// "plinthHeight", or on "boardThickness" without a plinth).
    float plinthHeight = 0.08f;
    /// Item standing upright, front facing +Z: (w, h, d), each [0.005, 2]. Every level must hold at least one item:
    /// an item too tall for the compartments is reported on "levels" (on "slotSize" when even a single level
    /// could not hold it), a too wide or too deep item on "slotSize".
    glm::vec3 slotSize{0.125f, 0.085f, 0.020f};
    float slotGap = 0.004f;   ///< [0, 0.5]
    float slotMargin = 0.03f; ///< [0, 1]
};

/// "cartridge", pivot center, label on the front (+Z); bounds = size centered on the origin. A chamfered shell
/// with a recessed label pocket and optional grip grooves below the label (generic family, no brand shapes).
/// Roles: "plastic_shell", "label" (quad over the label area, uv (0, 0) top-left, `labelInset` below the front
/// and 0.5 mm above the pocket floor, so the pocket is labelInset + 0.5 mm deep). Sockets: "Label" (Other) at the
/// label center facing +Z, bounds = label rectangle [-lw/2, -lh/2, 0] .. [lw/2, lh/2, 0].
struct CartridgeParams {
    glm::vec3 size{0.125f, 0.085f, 0.020f}; ///< (w, h, d): w, h in [0.02, 0.5], d in [0.004, 0.2]
    float chamfer = 0.002f;                 ///< [0, min(size) / 4]
    /// Label borders (x, y); each at least chamfer + 0.5 mm, leaving a label of at least 4 mm.
    glm::vec2 labelMargin{0.012f, 0.012f};
    float labelInset = 0.001f; ///< [0.0005, min(0.005, d / 4)]
    bool gripRidges = true;
};

/// "crt", pivot floor, screen facing +Z (consumer 4:3 television); bounds = width x height x depth. A front
/// cabinet with a bezel whose inner walls slope down to a recessed, slightly bulging screen, a dark control strip
/// under the bezel, and an octagonal rear housing tapered by `backTaper` with vents on top.
/// Roles: "plastic_body" (cabinet and rear housing), "plastic_bezel" (front frame), "screen" (largest
/// screenAspect rectangle inside the bezel, uv 0..1 with (0, 0) top-left), "plastic_dark" (control strip,
/// buttons, vents).
/// Sockets: "ScreenSurface" (ScreenSurface) at the center of the screen rim plane facing +Z, bounds = screen
/// rectangle (z from 0 to the bulge), meshIndex = index of the "screen" part; "TV_Stand" (Anchor) at the origin;
/// "ScreenLook" (Anchor) at (0, screen center y, depth/2 + 0.9) looking at the screen along its local -Z (glTF
/// camera convention); "PowerButton" (Other) at the front center of the power button.
struct CrtParams {
    float width = 0.71f;             ///< [0.2, 2]
    float height = 0.61f;            ///< [0.2, 2]
    float depth = 0.56f;             ///< [0.15, 2]
    float screenAspect = 1.3333333f; ///< width / height, [0.5, 3]
    float bezel = 0.06f;             ///< minimum frame width around the screen, [0.01, min(width, height) / 4]
    /// Screen rim depth behind the bezel front, [0, 0.05] and at most 0.38 * depth - 0.028: the bezel (8 mm
    /// deeper than the inset, at least 2 cm) and 2 cm of front cabinet fill the front 38% of the depth.
    float screenInset = 0.012f;
    float backTaper = 0.35f; ///< rear face = (1 - backTaper) x the front section, [0, 0.8]
};

/// "tv_cabinet", pivot floor, front +Z; bounds = width x height x depth (handles included). A low media console on
/// legs: `drawers` stacked drawers on the left (a door when 0), an open compartment in the middle, a door on
/// the right; the top overhangs the carcass.
/// Roles: "wood_primary" (carcass and top), "wood_secondary" (drawer and door fronts), "metal_trim" (handles,
/// legs). Sockets: "Top" (Anchor) at the center of the top surface; "ConsoleShelf" (Anchor) on the floor of the
/// open compartment, centered, bounds = free compartment volume [-w/2, 0, -d/2] .. [w/2, h, d/2].
struct TvCabinetParams {
    float width = 1.2f;   ///< [0.4, 4]
    float height = 0.55f; ///< [0.2, 2]
    float depth = 0.5f;   ///< [0.2, 1.5]
    int drawers = 2;      ///< [0, 8]; each drawer front at least 5 cm tall
    /// [0, 0.5]; leaves a carcass of at least 15 cm. Up to 0.1 mm no legs are built and the carcass rests on the
    /// floor.
    float legHeight = 0.08f;
};

/// "console_cartridge", pivot floor, front +Z; bounds = width x height x depth. Generic 16-bit cartridge console
/// (original design): a low chamfered base, a raised rear deck with the cartridge slot, power and reset buttons,
/// vents and two controller ports on the front.
/// Roles: "plastic_body", "plastic_dark" (cartridge slot, vents, controller ports), "plastic_accent" (power and
/// reset buttons). Sockets: "CartridgeSlot" (Anchor) at the center of the slot opening on top, +Y up, bounds =
/// opening rectangle; "Front" (Other) at the center of the front face facing +Z, bounds = front rectangle.
struct ConsoleCartridgeParams {
    float width = 0.24f;  ///< [0.12, 1]
    float height = 0.07f; ///< [0.03, 0.3]
    float depth = 0.2f;   ///< [0.1, 1]
};

/// "poster_frame", pivot at the back center (z = 0 is the wall surface), front +Z; bounds =
/// [-width/2, -height/2, 0] .. [width/2, height/2, depth].
/// Roles: "frame" (bars and backing board), "picture" (quad inside the frame, uv (0, 0) top-left). Sockets:
/// "Picture" (Other) at the picture center facing +Z, bounds = picture rectangle.
struct PosterFrameParams {
    float width = 0.5f;       ///< [0.05, 5]
    float height = 0.7f;      ///< [0.05, 5]
    float frameWidth = 0.03f; ///< [0.005, 0.5], leaving a picture of at least 1 cm
    float depth = 0.02f;      ///< [0.005, 0.2]
};

/// "rug", pivot floor; bounds = width x thickness x depth. Role "fabric": uv 0..1 over the top face with (0, 0)
/// at the -X/-Z corner; the edges continue the border of the image. No sockets.
struct RugParams {
    float width = 2.4f;       ///< [0.1, 20]
    float depth = 1.6f;       ///< [0.1, 20]
    float thickness = 0.012f; ///< [0.001, 0.1]
};

/// "floor_lamp", pivot floor; bounds = [-r, 0, -r] .. [r, height, r] with r = max(baseRadius, shadeRadius).
/// Roles: "metal" (base, pole, bulb holder, shade spider), "lamp_shade" (open shade built as a thin closed shell
/// with a 2 mm wall: renders from inside and outside with or without back-face culling). Sockets: "LightSource"
/// (Anchor) at the bulb inside the shade.
struct FloorLampParams {
    float height = 1.6f;       ///< [0.5, 3]
    float baseRadius = 0.16f;  ///< [0.05, 1]
    float shadeRadius = 0.22f; ///< [0.05, 1]
    float shadeHeight = 0.3f;  ///< [0.05, 1], at most height / 2
};

// ---------------------------------------------------------------------------------------------------------------
// Builders. Invalid parameters return an error and never crash.
// ---------------------------------------------------------------------------------------------------------------

[[nodiscard]] Expected<ProceduralPrefab> buildRoomShell(const RoomShellParams& params = {});
[[nodiscard]] Expected<ProceduralPrefab> buildShelf(const ShelfParams& params = {});
[[nodiscard]] Expected<ProceduralPrefab> buildCartridge(const CartridgeParams& params = {});
[[nodiscard]] Expected<ProceduralPrefab> buildCrt(const CrtParams& params = {});
[[nodiscard]] Expected<ProceduralPrefab> buildTvCabinet(const TvCabinetParams& params = {});
[[nodiscard]] Expected<ProceduralPrefab> buildConsoleCartridge(const ConsoleCartridgeParams& params = {});
[[nodiscard]] Expected<ProceduralPrefab> buildPosterFrame(const PosterFrameParams& params = {});
[[nodiscard]] Expected<ProceduralPrefab> buildRug(const RugParams& params = {});
[[nodiscard]] Expected<ProceduralPrefab> buildFloorLamp(const FloorLampParams& params = {});

/// JSON factory used by scene files and prefab manifests ("runtime": "procedural:<kind>"). `params` is null or an
/// object whose keys are the field names of the kind's params struct (vectors as arrays, WallSide as a string,
/// "doorway" as an object or null). Missing keys keep their defaults. Errors: unknown kind -> NotFound; params
/// that are not an object, unknown keys and wrong value types -> InvalidArgument; out-of-range or inconsistent
/// values -> OutOfRange. Error contexts name the parameter: "<kind>.<key>" (nested: "room_shell.doorway.width").
[[nodiscard]] Expected<ProceduralPrefab> buildPrefab(std::string_view kind, const nlohmann::json& params);

/// Single-material primitive centered on the origin, named after its kind (same error rules as buildPrefab):
///   "box"      {"size": [1, 1, 1], "uv": "face" | "meters", "metersPerTile": 1.0}
///   "plane"    {"size": [1, 1] (x, z), "uvTiling": [1, 1]}, facing +Y
///   "quad"     {"size": [1, 1] (x, y)}, facing +Z, uv (0, 0) at the top-left corner
///   "sphere"   {"radius": 0.5, "segments": 32, "rings": 16}
///   "cylinder" {"radius": 0.5, "height": 1.0, "segments": 32, "capped": true}, axis +Y
/// Lengths in [0.001, 1000], metersPerTile in [0.01, 100], uvTiling in [0.001, 10000], segments in [3, 256],
/// rings in [2, 128].
[[nodiscard]] Expected<MeshData> buildPrimitive(std::string_view kind, const nlohmann::json& params);

/// Kinds accepted by buildPrefab, in documentation order: room_shell, shelf, cartridge, crt, tv_cabinet,
/// console_cartridge, poster_frame, rug, floor_lamp.
[[nodiscard]] std::span<const std::string_view> prefabKinds() noexcept;

/// Kinds accepted by buildPrimitive: box, plane, quad, sphere, cylinder.
[[nodiscard]] std::span<const std::string_view> primitiveKinds() noexcept;

} // namespace rv::geometry
