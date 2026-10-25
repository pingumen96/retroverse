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

/// Solid box of a prefab for first-person collision (specification §14.3: furniture and rooms declare their
/// volumes).
struct PrefabCollider {
    /// Names the piece when a prefab has several (room_shell walls); empty for a prefab that is one box.
    std::string name;
    /// Prefab-local box, meters.
    core::Aabb box;
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
    /// Volumes a walker must not enter, prefab-local, inside `bounds`. Furniture, electronics, media and decor are one
    /// unnamed box equal to `bounds`; room_shell lists the solid part of each wall and a stop in the outer face of its
    /// door opening (see RoomShellParams), leaving the interior and the rest of the opening free; rug has none (it is
    /// walked over).
    std::vector<PrefabCollider> colliders;
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

/// Opening in a room_shell wall. The door rectangle (width x height, from the floor) stays completely open, without
/// geometry (a walker is stopped in its outer face, see RoomShellParams); its lining and casing are built outside it.
struct DoorwayParams {
    WallSide wall = WallSide::South;
    /// Opening center relative to the wall center, along world +X for north/south walls and +Z for east/west
    /// walls. The opening and its 7 cm casing must stay 12 mm clear of the room corners.
    float offset = 0.0f;
    float width = 1.6f;  ///< [0.3, 20]
    float height = 2.3f; ///< [0.5, 50]; height + 7 cm casing <= wallHeight
    /// The 2 cm stop in the opening's outer face (the collider "<wall>_doorway", see RoomShellParams): true for a room
    /// standing alone, whose walker must not leave it; false for a doorway that opens onto a hall (a floor's scene, I4
    /// step 3, ADR-0013 §6), so that the walker passes. JSON "stop", default true; nothing else changes.
    bool stop = true;
};

/// "room_shell", pivot floor. The interior rectangle is exactly `size` (x, z) centered on the origin; walls are
/// built outside it, so bounds = [-x/2 - t, 0, -z/2 - t] .. [x/2 + t, wallHeight, z/2 + t].
/// Roles: "floor", "wall", "ceiling", "trim" (baseboards, door lining and casing), "wainscot" (when
/// wainscotHeight > 0: vertical boards about 60 cm wide with 4 mm grooves between them above the baseboard, under a
/// chair rail, along every wall and stopping at the door casing). Interior faces point into the room; floor, wall and
/// ceiling uvs are in meters / metersPerTile (seamless across wall segments); trim and wainscot uvs are in meters along
/// each piece (BoxUv::GrainMeters, 1 m per uv unit: the grain of wood follows the piece, up the boards).
/// Sockets: "Doorway" (Anchor) when a doorway exists: at the center of the opening at floor level (middle of the
/// wall thickness), +Z pointing out of the room, +Y up; bounds = opening volume
/// [-width/2, 0, -t/2] .. [width/2, height, t/2].
/// Colliders: one box per wall over its full thickness and height, named "north", "south", "east", "west"; the
/// wall with the doorway is split around the door rectangle into the parts beside the opening, named after the
/// direction they extend to ("south_west" and "south_east" for the south wall, "east_north" and "east_south" for the
/// east wall), and the part above it ("south_above"). Nothing lies beyond a doorway, so "<wall>_doorway"
/// ("south_doorway") closes the door rectangle with a 2 cm stop in the wall's outer face: a walker can step into the
/// opening but not leave the room; a doorway onto a hall drops this one box (DoorwayParams::stop false). Floor,
/// ceiling and trim (less than 2 cm deep) have none.
struct RoomShellParams {
    glm::vec2 size{9.0f, 10.0f}; ///< interior x and z, each [0.5, 200]
    float wallHeight = 3.0f;     ///< [1, 50]
    float wallThickness = 0.15f; ///< t, [0.02, 2]
    float metersPerTile = 1.0f;  ///< [0.01, 100]
    bool ceiling = true;
    float baseboardHeight = 0.1f; ///< [0, 0.5]; 0 removes the baseboards
    /// Height of the wainscot's rail top, [0, 3]; 0 builds none. Otherwise at least 5 cm of boards above the
    /// baseboard under the 3.5 cm rail, and at most wallHeight - 0.2. The boards stand 1 cm proud of the wall, the rail
    /// 2.2 cm: furniture against a wall leaves them room.
    float wainscotHeight = 0.0f;
    /// nullopt (JSON null) builds a closed room.
    std::optional<DoorwayParams> doorway = DoorwayParams{};
};

/// "shelf", pivot floor, open side +Z; bounds = width x height x depth.
/// Roles: "wood_primary" (sides, boards, top, plinth), "wood_secondary" (back panel, present when `back`); every
/// part has uvs in meters along its boards (BoxUv::GrainMeters, 1 m per uv unit), so wood grain keeps one scale.
/// Sockets: "Slot_<level>_<index>" (ShelfSlot): level 0 is the lowest board, index 0 the leftmost (-X) slot.
/// innerWidth = width - 2 * boardThickness; slotsPerLevel = floor((innerWidth - 2 * slotMargin + slotGap) /
/// (slotSize.x + slotGap)) (1e-4 tolerance for exact fits); slots are left-aligned from x = -innerWidth / 2 +
/// slotMargin. The `levels` boards are evenly spaced between the plinth top and the top board (each compartment
/// is (height - boardThickness - plinthHeight) / levels tall, board included). Socket transform = translation to
/// the slot's bottom-center point on the board, centered in the board depth (or `slotFrontInset` behind its front
/// edge); Socket::bounds = item volume [-w/2, 0, -d/2] .. [w/2, h, d/2]. Before each level's slots, "Level_<level>"
/// (Anchor, socket.hpp's parseLevelId) at the middle of the board's top, bounds = the compartment's free volume
/// (innerWidth x free height x board depth, centered on it).
/// The levels listed in `displayLevels` lay out their slots with the display* fields instead (another item size, gap
/// and margin): the collection's modules (I3, ADR-0010 §3, §5) are shelves whose levels hold cartridges spine-out
/// (slotSize = slotSizeFor(SlotFacing::SpineOut, item)) with one display level of face-out cartridges at eye height,
/// the hero band.
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
    /// Distance from the front edge of the boards to the front of every item, [0, board depth - item depth] for each
    /// item size in use; nullopt (JSON null, the default) centers the items in the board depth.
    std::optional<float> slotFrontInset;
    /// Levels, each in [0, levels - 1] and listed once, whose slots hold displaySlotSize items spaced by
    /// displaySlotGap and displaySlotMargin (the same rules as slotSize, reported on the display* keys).
    std::vector<int> displayLevels;
    glm::vec3 displaySlotSize{0.125f, 0.085f, 0.020f}; ///< each [0.005, 2]
    float displaySlotGap = 0.006f;                     ///< [0, 0.5]
    float displaySlotMargin = 0.025f;                  ///< [0, 1]
    /// A lit interior (I6 step 3, the concept's lit bookcases): under the top board and under every level's board but
    /// the lowest, a strip (role "board_light", 3 mm thick and 12 mm deep, 20 mm behind the boards' front edge, its
    /// underside and front showing) and, with a back panel, over the back panel of each compartment a quad facing +Z
    /// 1.5 mm in front of it (role "board_glow", innerWidth x the compartment's free height, uv (0, 0) at its top-left):
    /// for emissive materials, no light of their own. False (the default) builds neither: the shelf as before.
    bool boardLight = false;
};

/// "cartridge", pivot center, label on the front (+Z); bounds = size centered on the origin. A chamfered shell
/// with a recessed label pocket and optional grip grooves below the label (generic family, no brand shapes).
/// Roles: "plastic_shell", "label" (quad over the label area, uv (0, 0) top-left, `labelInset` below the front
/// and 0.5 mm above the pocket floor, so the pocket is labelInset + 0.5 mm deep), "spine" (with `spineLabel`: the end
/// label on the top face, see below). Sockets: "Label" (Other) at the label center facing +Z, bounds = label rectangle
/// [-lw/2, -lh/2, 0] .. [lw/2, lh/2, 0]; "Spine" (Other, with `spineLabel`) at the spine label's center facing +Y.
///
/// The spine (ADR-0010 §5, ADR-0011 §4): standing spine-out (SlotFacing::SpineOut) a cartridge shows its top face, so
/// the end label lies there, a quad 0.5 mm above the shell's top face (the shell is 0.5 mm lower, so the bounds stay
/// `size`), centered along X and on the flat part of the top face across Z. Seen standing spine-out its +X end is up:
/// the Spine socket's frame has +X along the item's +Z (right as one faces the spine), +Y along the item's +X (up) and
/// +Z along the item's +Y (out of the face), its bounds [-across/2, -along/2, 0] .. [across/2, along/2, 0]; the quad's
/// uv (0, 0) is its top-left corner in that view, so a spine texture (40 x 256, ADR-0011 §4) is drawn upright.
///
/// With `spineFace` Side (I6 step 4, ADR-0014 §2: cases and boxes standing on their side, SlotFacing::SideOut) the
/// label lies on the -X face instead, upright: a quad 0.5 mm outside the shell's -X face (the shell is 0.5 mm narrower
/// on that side, so the bounds stay `size`), centered along Y and on the flat part of the face across Z (behind the
/// front pocket), with no label on the top face. The Spine socket's frame has +X along the item's +Z (right as one
/// faces the spine), +Y along the item's +Y (up) and +Z along the item's -X (out of the face); `spineLabelSize` is
/// (across Z, along Y), across at most d - labelInset - 0.5 mm - 2 * chamfer, along at most h - 2 * chamfer, and the
/// label's x border then takes the extra 0.5 mm instead of its y border.
enum class SpineFace {
    Top,  ///< on the top face (+Y): the item stands on its end (SlotFacing::SpineOut)
    Side, ///< on the -X face, upright: the item stands on its side (SlotFacing::SideOut)
};

struct CartridgeParams {
    glm::vec3 size{0.125f, 0.085f, 0.020f}; ///< (w, h, d): w, h in [0.02, 0.5], d in [0.004, 0.2]
    float chamfer = 0.002f;                 ///< [0, min(size) / 4]
    /// Label borders (x, y); each at least chamfer + 0.5 mm (the y border another 0.5 mm with `spineLabel`), leaving a
    /// label of at least 4 mm.
    glm::vec2 labelMargin{0.012f, 0.012f};
    float labelInset = 0.001f; ///< [0.0005, min(0.005, d / 4)]
    bool gripRidges = true;
    /// Builds the "spine" role and the Spine socket. Off by default: the test room's face-out cartridges have none.
    bool spineLabel = false;
    /// Spine label (across Z, along X), each at least 4 mm; across at most the flat top (d - labelInset - 0.5 mm -
    /// 2 * chamfer), along at most w - 2 * chamfer. The default keeps the spine texture's 40 x 256 aspect.
    glm::vec2 spineLabelSize{0.0135f, 0.0864f};
    /// The face the spine label lies on (JSON "top" or "side").
    SpineFace spineFace = SpineFace::Top;
};

/// How an item stands in a slot (ADR-0010 §5). Slot sockets ("Slot_<level>_<index>", SocketKind::ShelfSlot) have
/// their origin at the slot's bottom center and +Z towards the viewer; their bounds are the slot volume
/// [-w/2, 0, -d/2] .. [w/2, h, d/2].
enum class SlotFacing {
    /// Upright, its front (+Z) towards the slot's +Z: the slot is the item's (w, h, d).
    FaceOut,
    /// On its end, its top face (+Y, a cartridge's spine) towards the slot's +Z, its +X up and its front (+Z) towards
    /// the slot's +X: the slot is the item's (d, w, h), a 12.5 x 8.5 x 2.0 cm cartridge 2.0 x 12.5 x 8.5 cm.
    SpineOut,
    /// On its side, upright (I6 step 4, ADR-0014 §2): turned a quarter about +Y, its -X face (a case's spine,
    /// SpineFace::Side) towards the slot's +Z, its +Y up and its front (+Z) towards the slot's +X: the slot is the
    /// item's (d, h, w), a 14.2 x 12.5 x 1.0 cm jewel case 1.0 x 12.5 x 14.2 cm, as a book stands on a shelf.
    SideOut,
};

/// The slot volume (w, h, d) an item of `itemSize` (w, h, d) fills standing with `facing`.
[[nodiscard]] glm::vec3 slotSizeFor(SlotFacing facing, const glm::vec3& itemSize) noexcept;

/// Rigid transform from an item's prefab space (pivot center, as the cartridge's) into a slot socket's frame: the item
/// then fills the slot volume of slotSizeFor(facing, itemSize) exactly. An item's transform in the fixture is
/// socket.transform * slotItemTransform(facing, itemSize).
[[nodiscard]] glm::mat4 slotItemTransform(SlotFacing facing, const glm::vec3& itemSize) noexcept;

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
/// legs); the wood has uvs in meters along its boards (BoxUv::GrainMeters, 1 m per uv unit).
/// Sockets: "Top" (Anchor) at the center of the top surface; "ConsoleShelf" (Anchor) on the floor of the open
/// compartment, centered, bounds = free compartment volume [-w/2, 0, -d/2] .. [w/2, h, d/2].
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
/// Roles: "frame" (bars and backing board, uvs in meters along each bar: BoxUv::GrainMeters, 1 m per uv unit),
/// "picture" (quad inside the frame, uv (0, 0) top-left). Sockets:
/// "Picture" (Other) at the picture center facing +Z, bounds = picture rectangle.
struct PosterFrameParams {
    float width = 0.5f;       ///< [0.05, 5]
    float height = 0.7f;      ///< [0.05, 5]
    float frameWidth = 0.03f; ///< [0.005, 0.5], leaving a picture of at least 1 cm
    float depth = 0.02f;      ///< [0.005, 0.2]
};

/// "rug", pivot floor; bounds = width x thickness x depth. Role "fabric": uv 0..1 over the top face with (0, 0)
/// at the -X/-Z corner; the edges continue the border of the image. No sockets, no colliders.
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

/// "armchair", pivot floor, front +Z; bounds = width x height x depth. An upholstered club armchair (original design):
/// a back over the full width, two armrests, a base under the seat cushion, a back cushion leaning back against the
/// back, on four short tapered legs.
/// Roles: "fabric" (back, armrests, base, cushions), "wood" (legs, present when legHeight > 0.1 mm).
/// Sockets: "Seat" (Anchor) at the middle of the seat cushion's top, +Y up.
struct ArmchairParams {
    float width = 0.82f;      ///< [0.5, 2.5]
    float depth = 0.82f;      ///< [0.5, 1.5]
    float height = 0.84f;     ///< [0.5, 1.4]: the top of the back; at least 5 cm above armHeight
    float seatHeight = 0.44f; ///< [0.25, 0.7]: the top of the 12 cm seat cushion, at least 5 cm above the legs
    float armHeight = 0.62f;  ///< [0.3, 1]: the top of the armrests; at least 5 cm above seatHeight
    float legHeight = 0.1f;   ///< [0, 0.3]; up to 0.1 mm no legs are built
};

/// "display_case", pivot floor, front +Z; bounds = width x height x depth. A low collector's case (style §8
/// `furniture/display_case`, §6.2: 0.80-1.10 m; ADR-0010 §3's display fixtures): a closed wooden base with two doors,
/// and on it a vitrine glazed on every side but the back: a glass front, two glass sides and a glass top held by four
/// slim corner posts (18 mm), a back panel and a frame of top rails (I3 step 17a's review: no opaque side). Inside,
/// `levels` face-out levels step up from the front (level 0, lowest) to the back: on each, the items stand on a ledge
/// with a low lip and lean back by `tiltDeg` against a panel, the ledge resting on a riser that fills the level's band
/// of the vitrine's depth (seen through the side glass, the risers close the space under the tilted panels).
/// Roles: "wood_primary" (base carcass, plinth and top, corner posts and rails), "wood_secondary" (doors, back
/// panel), "metal_trim" (knobs), "fabric" (risers, ledges, lips and panels), "glass" (the front, side and top panes,
/// one sheet each facing out: draw it with a blended, double-sided material such as MAT_Glass_Display, which casts
/// no shadow).
/// Sockets: "Slot_<level>_<index>" (ShelfSlot), index 0 leftmost: slotsPerLevel as for the shelf, over the vitrine's
/// inner width (width - 2 * 18 mm); transform = translation to the slot's bottom center on its ledge, turned back by
/// tiltDeg about +X (the item's front faces up and forward); bounds = item volume [-w/2, 0, -d/2] .. [w/2, h, d/2].
/// Before each level's slots, "Level_<level>" (Anchor): the level's leaning frame at x = 0, bounds = its free volume
/// on the ledge, from the panel to the lip and up to the panel's top (2.5 cm above the items).
struct DisplayCaseParams {
    float width = 1.2f;       ///< [0.4, 4]
    float height = 0.95f;     ///< [0.5, 1.6]
    float depth = 0.5f;       ///< [0.25, 1.2]
    float baseHeight = 0.55f; ///< top of the closed base, the vitrine's floor; [0.15, height - 0.2]
    int levels = 2;           ///< [1, 4]; every level must fit in its band of the depth and under the glass top
    float tiltDeg = 30.0f;    ///< how far the items lean back from upright, [0, 60]
    float levelRise = 0.12f;  ///< height of each ledge above the one in front of it, [0, 0.5]
    glm::vec3 slotSize{0.125f, 0.085f, 0.020f}; ///< the face-out item (w, h, d), each [0.005, 0.5]
    float slotGap = 0.012f;                     ///< [0, 0.5]
    float slotMargin = 0.03f;                   ///< [0, 1]
};

/// "wall_shelf", the featured wall shelf (ADR-0010 §3, §4), front +Z, hung on a wall: pivot on the floor below the
/// middle of its footprint, bounds = [-width/2, mountHeight, -depth/2] .. [width/2, mountHeight + height, depth/2]
/// (the back at -depth/2 against the wall). A back board between two side cheeks under a top cap, with `ledges`
/// picture ledges, each with a lip along its front edge; the face-out items stand on a ledge leaning back by `tiltDeg`
/// against the back board. Roles: "wood_primary" (cheeks, cap, ledges, lips), "wood_secondary" (back board).
/// Sockets: "Slot_<ledge>_<index>" (ShelfSlot), ledge 0 lowest, index 0 leftmost, slotsPerLevel as for the shelf over
/// the inner width (width - 2 * 20 mm); transform = translation to the slot's bottom center, turned back by tiltDeg
/// about +X, placed so that the item's lowest edge rests on the ledge and its top back edge on the back board;
/// bounds = item volume [-w/2, 0, -d/2] .. [w/2, h, d/2]. Before each ledge's slots, "Level_<ledge>" (Anchor) on the
/// ledge's top, centered between the back board and the lip, bounds = the free volume up to the next ledge or the cap.
struct WallShelfParams {
    float width = 0.9f;       ///< [0.3, 3]
    float height = 0.36f;     ///< [0.15, 2]; each ledge's compartment must hold a leaning item and 1 cm
    float depth = 0.08f;      ///< [0.05, 0.4]; the ledges must hold a leaning item in front of the back board
    float mountHeight = 1.1f; ///< height of the unit's bottom above the floor, [0, 2.5]
    int ledges = 2;           ///< [1, 8]
    float tiltDeg = 12.0f;    ///< [0, 30]
    glm::vec3 slotSize{0.125f, 0.085f, 0.020f}; ///< the face-out item (w, h, d), each [0.005, 0.5]
    float slotGap = 0.012f;                     ///< [0, 0.5]
    float slotMargin = 0.02f;                   ///< [0, 1]
    /// A lit interior (I6 step 3): a strip under the cap and under every ledge but the lowest (role "board_light", 3 mm
    /// thick and 10 mm deep, 12 mm behind the front, its underside and front showing), for an emissive material. False
    /// (the default) builds none.
    bool boardLight = false;
};

/// Shelf dressing (ADR-0010 §5: the levels no title needs), three kinds sharing these parameters, each pivot floor and
/// filling the free volume of one level: bounds within [-width/2, 0, -depth/2] .. [width/2, height, depth/2], resting
/// on y = 0. `seed` (a level's propSeed reduced to [0, 2^31 - 1]) varies counts, sizes and offsets deterministically
/// (core::Random); the same parameters always give the same geometry. No sockets.
///  - "storage_box": a row of lidded storage boxes across the level, each with a label card on its front. Roles
///    "cardboard" (boxes and lids), "label" (the cards, uv (0, 0) top-left).
///  - "magazine_stack": one to four stacks of magazines lying flat, each magazine turned a little. Roles "paper" (the
///    magazines), "cover" (a quad on the top magazine of each stack, uv (0, 0) at its top-left seen from the front).
///  - "bookends": a row of books standing between two block bookends. Roles "metal" (the bookends), "book" and
///    "book_alt" (two bindings mixed along the row; a role is left out when no book uses it).
struct ShelfDressingParams {
    float width = 0.85f;  ///< [0.1, 3]
    float height = 0.17f; ///< [0.05, 1]
    float depth = 0.23f;  ///< [0.05, 1]
    int seed = 0;         ///< [0, 2147483647]
};

/// "plant", pivot floor (I3 step 16); bounds within [-spread, 0, -spread] .. [spread, height, spread] (the tallest leaf's
/// tip at `height` within a millimetre). A potted plant of original design: a rosette of upright sword leaves in a
/// tapered ceramic pot with a rolled rim on a saucer of radius `spread`, filled with soil to the brim; `seed` varies the
/// leaves' angles, heights, widths, lean and twist deterministically (core::Random). Roles: "pot" (the saucer too),
/// "soil", "leaves" and "leaves_alt" (every third leaf, a lighter shade). With `saucerRadius` (I6 step 2) the saucer is
/// that wide, a little wider than the pot rather than the leaves, in a role of its own, "saucer" (the bounds are then
/// the leaves' and the pot's, within the spread). No sockets; its bounds collide, as every fixture's do.
struct PlantParams {
    float height = 1.0f;       ///< [0.3, 2.5]
    float potRadius = 0.15f;   ///< the pot's top radius, [0.05, 0.6]
    float potHeight = 0.3f;    ///< [0.08, height / 2]
    float spread = 0.19f;      ///< how far the leaves reach from the axis, [potRadius + 8 mm, 1]
    int leaves = 14;           ///< [3, 64]
    int seed = 0;              ///< [0, 2147483647]
    float saucerRadius = 0.0f; ///< 0: the saucer as wide as the spread, in the pot's role; else [potRadius, spread]
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
[[nodiscard]] Expected<ProceduralPrefab> buildArmchair(const ArmchairParams& params = {});
[[nodiscard]] Expected<ProceduralPrefab> buildDisplayCase(const DisplayCaseParams& params = {});
[[nodiscard]] Expected<ProceduralPrefab> buildWallShelf(const WallShelfParams& params = {});
[[nodiscard]] Expected<ProceduralPrefab> buildStorageBox(const ShelfDressingParams& params = {});
[[nodiscard]] Expected<ProceduralPrefab> buildMagazineStack(const ShelfDressingParams& params = {});
[[nodiscard]] Expected<ProceduralPrefab> buildBookends(const ShelfDressingParams& params = {});
[[nodiscard]] Expected<ProceduralPrefab> buildPlant(const PlantParams& params = {});

/// JSON factory used by scene files and prefab manifests ("runtime": "procedural:<kind>"). `params` is null or an
/// object whose keys are the field names of the kind's params struct (vectors as arrays, WallSide as a string,
/// "doorway" as an object or null, "slotFrontInset" a number or null, "displayLevels" an array of integers). Missing
/// keys keep their defaults. Errors: unknown kind -> NotFound; params that are not an object, unknown keys and wrong
/// value types -> InvalidArgument; out-of-range or inconsistent values -> OutOfRange. Error contexts name the
/// parameter: "<kind>.<key>" (nested: "room_shell.doorway.width").
[[nodiscard]] Expected<ProceduralPrefab> buildPrefab(std::string_view kind, const nlohmann::json& params);

/// Single-material primitive centered on the origin, named after its kind (same error rules as buildPrefab):
///   "box"      {"size": [1, 1, 1], "uv": "face" | "meters" | "grain" (BoxUv::GrainMeters), "metersPerTile": 1.0,
///               "uvOrigin": [0, 0, 0]}: the meter layouts as if the box's centre stood at uvOrigin (its place in the
///               scene: boxes side by side, each with its own place, tile on without a seam; in [-1000, 1000])
///   "plane"    {"size": [1, 1] (x, z), "uvTiling": [1, 1]}, facing +Y
///   "quad"     {"size": [1, 1] (x, y)}, facing +Z, uv (0, 0) at the top-left corner
///   "sphere"   {"radius": 0.5, "segments": 32, "rings": 16}
///   "cylinder" {"radius": 0.5, "height": 1.0, "segments": 32, "capped": true}, axis +Y
/// Lengths in [0.001, 1000], metersPerTile in [0.01, 100], uvTiling in [0.001, 10000], segments in [3, 256],
/// rings in [2, 128].
[[nodiscard]] Expected<MeshData> buildPrimitive(std::string_view kind, const nlohmann::json& params);

/// Kinds accepted by buildPrefab, in documentation order: room_shell, shelf, cartridge, crt, tv_cabinet,
/// console_cartridge, poster_frame, rug, floor_lamp, armchair, display_case, wall_shelf, storage_box, magazine_stack,
/// bookends, plant.
[[nodiscard]] std::span<const std::string_view> prefabKinds() noexcept;

/// Kinds accepted by buildPrimitive: box, plane, quad, sphere, cylinder.
[[nodiscard]] std::span<const std::string_view> primitiveKinds() noexcept;

} // namespace rv::geometry
