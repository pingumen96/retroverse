#pragma once

// A floor's static part as one scene (ADR-0013 §6, §7, §9; I4 step 3), in the floor's frame: what `retroverse-cli
// worldplan show --floor <id> --scene <file>` writes and what step 5a's floor-build job instantiates.
//
//   - Every room of the floor (by id): its scene as toSceneFile() writes it (the shell, the fixtures, the dressing, the
//     lights with the profile's accents), moved onto the floor by its placement (a half turn about +Y for the south
//     row: positions and targets turned, yaws plus 180 degrees, rounded as toSceneFile() rounds them), its doorway's
//     stop left out (geometry::DoorwayParams::stop: the walker passes into the hall), its objects' ids and its lights'
//     names prefixed with the room's id ("room:snes/fx:hw-w", "room:snes/room" for its shell), each light in the
//     room's area. A room keeps at most kMaxSceneLights lights (toSceneFile()'s rule).
//   - The hall (floor_layout.hpp's deriveHall(); the ground floor's lobby): box primitives of the theme pack's "hall"
//     materials (room_look.hpp; the floor's pack, FloorSceneData::hallTheme, the built-in one for a role the pack
//     lacks, the "plant" and "poster_frame" kinds' roles too) that collide by their bounds (the walls; the floor and
//     the ceiling collide with nothing; the plaster's uvs anchored at each box's place, so that side by side they tile
//     on), its lights (point lights of the profile's hall role, as many as its length takes: the hall has no light
//     limit), a drum pendant around each on a stem from the ceiling, a low fill spot before each doorway that opens
//     onto it aimed into the room (the hall role's doorwayFill, I6 step 1), the elevator stand-in (a frame, two closed
//     brass doors, a call panel with its button), the floor's sign above the elevator (building.json's text), and on
//     the ground floor the closed entrance doors at the lobby's east end. Its dressing (I6 step 2, look.md "Halls"),
//     none of it a light:
//       - trim on every wall, like the rooms': a baseboard, a panelled wainscot (battens at a fixed pitch) under a rail,
//         the plaster above it as a thin skin (over the rooms' doorway walls too), a crown under the ceiling;
//       - a portal at each doorway that opens onto the hall: a casing and a lintel, the room's sign above (its
//         platform's sign text on the poster frame's 'sign' pattern, lettering lit on its own; "GAME ROOM" for a room
//         without one), and in a hall at least 2.4 m wide a double-sided blade sign with the same lettering standing out
//         from the wall towards the elevator, above the head, on a graphite arm;
//       - beams across the ceiling every 3 m from the west end, and one over each doorway no beam of that grid passes
//         within 0.75 m of;
//       - deriveHall()'s dressing: sconces (a backplate, a lit shade and the wash of its light on the wall, an unlit
//         alpha-blended quad of the wall wash's picture, one mesh and material for all), framed prints (the muted
//         'print' pattern; portrait and landscape in turn; each title, from 16 invented ones, chosen by the print's
//         place along its edge and its edge's and floor's offsets, and drawn with its own seed: the prints share a
//         small pool of pictures), benches and plants (on a saucer of their own, seeded by core::deriveSeed("hall",
//         floor, piece id)); benches and plants collide, the sconces and the prints' frames collide with nothing
//         walkable (above the head, or flat on the wall);
//       - a runner of rugs end to end down the middle, from past the elevator's zone, at a fixed pitch from the west
//         end;
//       - a large print on the east end wall of a hall (not the lobby's).
//     The trim, the signs, the beams and the runner collide with nothing. A longer hall appends pieces and moves none:
//     the trim follows the walls and the end wall's print its wall, as the walls do; a room that enters later may take
//     a piece away where its doorway's portal stands. Object ids "hall:<floor>/<part>"; its lights in the hall's area.
//   - The areas (floorAreas()): each room's interior with its doorway's passage, and the hall's interior, from the
//     floor up to their heights; the hall's with the hall role's environment (exposure, ambient, fill; the lobby's
//     own), the rooms' lit as the scene (I6 step 1, ADR-0014 §4).
//   - The profile's environment, and a camera at the arrival point in front of the elevator, at eye height, facing
//     east down the hall, its far plane sized to the floor.
// The collection's items are not scene objects: floorCollectionLayout() gives each room's layout in the floor's frame.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/building.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/floor_layout.hpp>
#include <rv/world/room_look.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// What drawing a floor reads besides the plan: the building's data (the hall's and lobby's widths, the walls, the
/// floor's sign), each room's scene data (loadSceneData()) and the lighting profile (the hall's lights, the
/// environment; every room's data holds the same one).
struct FloorSceneData {
    BuildingData building;
    std::map<RoomId, SceneData> rooms;
    LightingProfile lighting = eveningLightingProfile();
    /// The floor's theme pack (building.hpp's floorThemePack(), I6 step 3): the hall's materials and light tints.
    ThemeMaterials hallTheme = builtInThemeMaterials();
};

/// The ES-DE platform tags of the systems (classify()'s fallback for a system systems.json does not name), by system.
using PlatformTagsBySystem = std::map<catalog::SystemName, std::vector<std::string>>;

/// The rooms of `floor` in `plan`, by id.
[[nodiscard]] std::vector<const Room*> roomsOnFloor(const WorldPlan& plan, std::string_view floor);

/// Reads what drawing `floor` of `plan` reads: building.json, the floor's theme pack (systems.json's defaults when the
/// floor names none; the built-in pack, logged, when it does not load), and loadSceneData() of each of its rooms with
/// `lighting` (else the default profile) and the platform tags of the room's first system in `platformTags`. Errors:
/// NotFound for a floor the plan does not list; those of the building's and systems' readers and of loadSceneData().
[[nodiscard]] Expected<FloorSceneData> loadFloorSceneData(const std::filesystem::path& assetRoot,
                                                          const WorldPlan& plan, std::string_view floor,
                                                          const std::optional<LightingProfile>& lighting = std::nullopt,
                                                          const PlatformTagsBySystem& platformTags = {});

/// The scene of a floor: the scene file (scene_description.hpp's format, keys sorted), its hall, its rooms by id with
/// their areas' ids, and the dressed spans its rooms left bare.
struct FloorSceneBuild {
    nlohmann::json sceneFile;
    Hall hall;
    std::vector<RoomId> rooms;
    std::vector<UndressedSpan> undressed;
};

/// The scene file of `floor` of `plan` (see the file comment). Errors: NotFound for a floor the plan does not list or
/// a room whose data `data` lacks; those of toSceneFile() for each room.
[[nodiscard]] Expected<FloorSceneBuild> toFloorSceneFile(const WorldPlan& plan, std::string_view floor,
                                                         const FloorSceneData& data);

/// toFloorSceneFile() read back as the scene the app instantiates (scene::parseSceneDescription, context
/// "floor:<floor>").
[[nodiscard]] Expected<scene::SceneDescription> toFloorSceneDescription(const WorldPlan& plan, std::string_view floor,
                                                                        const FloorSceneData& data);

/// The hub's favourites showcase (I6 step 5, ADR-0014 §3): the display case the hub's scene draws at its "showcase"
/// piece, and its face-out slots on the floor: their frames (geometry's "Slot_<level>_<index>" sockets: the slot's
/// bottom centre, turned back by the case's tilt, +Z out of the item's face) in the floor's frame, the front level's
/// left to right as one faces the case, then the back level's; the slot's size (the proxies' face-out item). None
/// without a showcase (a doorway's approach took it, or another floor).
struct HubShowcaseSlots {
    std::vector<glm::mat4> frames;
    glm::vec3 size{0.0f};
};
[[nodiscard]] nlohmann::json hubShowcaseParams();
/// The hub's bookcases (I6 step 5): the shelf the hub's scene draws at each "bookcase-*" piece (lit inside, a row of
/// books between bookends on each level).
[[nodiscard]] nlohmann::json hubBookcaseParams();
/// The hub's landmark (I6 step 5; its globe lower and larger, its consoles larger and more of them, risers for boxed
/// games between them since the step's review): the hub_landmark the hub's scene draws at its "landmark" piece.
[[nodiscard]] nlohmann::json hubLandmarkParams();
[[nodiscard]] HubShowcaseSlots hubShowcaseSlots(const Hall& hall);

/// A floor scene's id of an object or light of a room: "<room id>/<id within the room>".
[[nodiscard]] std::string floorObjectId(const RoomId& room, std::string_view local);

/// `layout` (a room's collection, in the room's frame: collectionLayout()) in its floor's frame, its room standing at
/// `placement`: the items' slot frames and transforms, the clusters' bounds and the occluders moved by
/// roomToFloorMatrix() (boxes stay exact: a half turn keeps them axis-aligned); pick ids, layer keys, flags and the
/// navigation (positions along a run and heights, which a rigid move keeps) unchanged.
[[nodiscard]] CollectionLayout floorCollectionLayout(CollectionLayout layout, const RoomPlacement& placement);

} // namespace rv::world
