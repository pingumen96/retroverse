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
//   - The hall (floor_layout.hpp's deriveHall(); the ground floor's lobby): box primitives of the theme's floor, wall
//     and ceiling materials that collide by their bounds (the walls; the floor and the ceiling collide with nothing),
//     its lights (point lights of the profile's pendant style, as many as its length takes: the hall has no light
//     limit), a lamp under each, the elevator stand-in (a frame, two closed doors, a call panel with its button), the
//     floor's sign above the elevator (building.json's text), and on the ground floor the closed entrance doors at the
//     lobby's east end. Object ids "hall:<floor>/<part>"; its lights in the hall's area.
//   - The areas (floorAreas()): each room's interior with its doorway's passage, and the hall's interior, from the
//     floor up to their heights.
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
};

/// The ES-DE platform tags of the systems (classify()'s fallback for a system systems.json does not name), by system.
using PlatformTagsBySystem = std::map<catalog::SystemName, std::vector<std::string>>;

/// The rooms of `floor` in `plan`, by id.
[[nodiscard]] std::vector<const Room*> roomsOnFloor(const WorldPlan& plan, std::string_view floor);

/// Reads what drawing `floor` of `plan` reads: building.json, and loadSceneData() of each of its rooms with `lighting`
/// (else the default profile) and the platform tags of the room's first system in `platformTags`. Errors: NotFound
/// for a floor the plan does not list; those of the building's reader and of loadSceneData().
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

/// A floor scene's id of an object or light of a room: "<room id>/<id within the room>".
[[nodiscard]] std::string floorObjectId(const RoomId& room, std::string_view local);

/// `layout` (a room's collection, in the room's frame: collectionLayout()) in its floor's frame, its room standing at
/// `placement`: the items' slot frames and transforms, the clusters' bounds and the occluders moved by
/// roomToFloorMatrix() (boxes stay exact: a half turn keeps them axis-aligned); pick ids, layer keys, flags and the
/// navigation (positions along a run and heights, which a rigid move keeps) unchanged.
[[nodiscard]] CollectionLayout floorCollectionLayout(CollectionLayout layout, const RoomPlacement& placement);

} // namespace rv::world
