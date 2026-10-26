#pragma once

// A plan's static part as a scene (ADR-0010 §1, ADR-0011 §1; I3 step 10a): toSceneFile() writes what a room of the
// plan shows besides its collection as a scene file (scene_description.hpp's format: the room shell sized to the plan,
// the collection modules, the display fixtures, the hero corner's parts, the decoration, the shelf dressing, the lights
// with the lighting profile's colours and intensities, the environment and a camera at the doorway), and
// toSceneDescription() is that file read back by scene::parseSceneDescription, so that the scene the app instantiates
// and the file `retroverse-cli worldplan show --scene` writes are one. GPU-free: parameters, transforms and names, no
// mesh; the app builds them (app::SceneInstance).
//
// Mapping (the plan's integers to the scene's meters and degrees, one division each, so that Debug and Release write
// one text):
//   - the room: one "room_shell" object "room", its interior the room's bounds (a grown room's centre is south of the
//     origin: the shell stands at the bounds' centre), its height the room's, the template's wall thickness and
//     doorway (an east or west doorway's offset taken from the base room's centre to the shell's);
//   - every fixture: one prefab object with the fixture's local id ("fx:hw-w", "fx:hero.crt"), at its pivot
//     (millimetres / 1000) turned by its yaw (hundredths of a degree / 100) about +Y, built from what its manifest
//     resolves to (assets::resolvePrefab: the manifest's generator and parameters, else the family's fallback, logged
//     once), with the theme's materials per role; a model manifest's object names its model;
//   - every dressed span: one object "dress:<level>[.<first slot>]" of the prop's generator with the manifest's
//     parameters and the span's free volume ("width" across the span's slots, the level's "height" and "depth", the
//     level's prop seed reduced to [0, 2^31 - 1] as "seed"), standing in the middle of the span on the level's
//     "Level_<n>" anchor; a span whose volume no prop of that kind can fill (thinner than its parameters allow: the
//     display case's leaning levels) is left out and reported (SceneBuild::undressed);
//   - every plan light: one point or spot light (lightKind()) named by its id within the room, the profile's colour,
//     intensity and range for its role, a spot's cones from its span (outer = atan(span / 2 / distance) plus the
//     role's margin, at least the role's minimum, inner = outer x the role's ratio, rounded to 0.01 degrees); the key
//     alone casts shadows, with the profile's strength; then the profile's accent lights on the hero corner's parts
//     while the scene's limit leaves room (I3 step 16);
//   - an empty level of a display case (too thin for a dressing prop): a riser card leaning on it, an invented cover
//     on a card the level's size (I3 step 16), so that no display level stands bare;
//   - the profile's environment and a camera at the doorway, at eye height, looking north;
//   - the pack's furniture (I6 step 3b, ADR-0014 §1 amended, room_look.hpp's "furniture"): a hero corner's part the
//     pack lists is drawn as the pack's pieces (the first with the part's id, the others with ".2", ".3", ...), each at
//     its place in the hero corner socket's frame (the part's when the piece names none), at the part's height; a
//     decoration whose kind the pack lists is drawn as one of its variants, chosen by weight from the room's seed and
//     the fixture's id, its back on the socket's back edge and centred across it as the plan's prefab stood, at the
//     plan's height. The pieces take the pack's materials of their kind (the part's own when the piece is of the
//     part's kind) under the piece's, the kind's pictures (a poster's cover, a desk's screen) under the piece's
//     patterns. The plan's fixtures, slots and lights are untouched: what the pieces cover is drawnFixtures(). A guard
//     (the step's review) keeps them within their footprints whatever the data: a decoration's variant that leaves its
//     socket's footprint, or a hero part's piece that leaves the hero corner (with the planned part's own floor where
//     the part reaches out of it, the lamp beside the TV), is not drawn; the plan's prefab is, a warning logged once.
// The collection's items are not scene objects: collection_layout.hpp places them (ADR-0011 §1).

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/assets/prefab_resolver.hpp>
#include <rv/core/expected.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/media_family.hpp>
#include <rv/world/platform_manifest.hpp>
#include <rv/world/room_look.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// A prefab the plan names, resolved as the room draws it.
struct ScenePrefab {
    PrefabRef ref;
    /// What resolvePrefab ended on (the requested manifest, the family's generic one, the family's procedural prefab).
    assets::PrefabSource source = assets::PrefabSource::Requested;
    /// Why the requested manifest could not be used (assets::ResolvedPrefab::fallbackReason; empty for Requested).
    std::string fallbackReason;
    /// A procedural prefab: its generator and parameters (an object) and the prefab they build.
    std::string generator;
    nlohmann::json params = nlohmann::json::object();
    std::optional<geometry::ProceduralPrefab> prefab;
    /// A model manifest: its asset-relative model path (no geometry is read here).
    std::string model;
};

/// The prefabs of a plan, sorted by ref, one each.
struct ScenePrefabs {
    std::vector<ScenePrefab> prefabs;

    /// The prefab of `ref`, or nullptr.
    [[nodiscard]] const ScenePrefab* find(const PrefabRef& ref) const noexcept;
};

/// Every prefab `plan` names in `room` (its fixtures' and its dressing's), and the media family's item, sorted, once.
[[nodiscard]] std::vector<PrefabRef> scenePrefabRefs(const WorldPlan& plan, const RoomId& room,
                                                     const MediaFamily& family);

/// Resolves each of `refs` through assets::resolvePrefab (a fallback logs one warning). Errors: the first ref's that
/// fails to resolve, naming it; ValidationFailed for a procedural prefab whose generator is not the kind it is named
/// with (a fallback across kinds would not fit the plan's slots).
[[nodiscard]] Expected<ScenePrefabs> loadScenePrefabs(const std::filesystem::path& assetRoot,
                                                      const assets::PrefabFamilies& families,
                                                      std::span<const PrefabRef> refs);

/// A piece of the pack's furniture as rooms draw it (I6 step 3b): the piece, its prefab resolved and built with the
/// piece's parameters over the manifest's (ScenePrefab::params the merged parameters).
struct ScenePiece {
    FurniturePiece piece;
    ScenePrefab prefab;
};

/// A pack's furniture resolved: the pieces of each hero corner part and the weighted variants of each decoration kind.
struct SceneFurniture {
    std::map<std::string, std::vector<ScenePiece>, std::less<>> heroParts;
    std::map<std::string, std::vector<std::pair<std::uint32_t, ScenePiece>>, std::less<>> decoration;
};

/// Resolves `furniture` (assets::resolvePrefab, then geometry::buildPrefab with the piece's parameters over the
/// resolved ones). A piece that fails (its manifest unresolved, a model, a generator other than its kind, parameters
/// the generator refuses) is left out with a warning; a part or kind left without pieces is drawn as the plan has it,
/// and so is a kind whose variants are left without weight. Never fails.
[[nodiscard]] SceneFurniture resolveSceneFurniture(const std::filesystem::path& assetRoot,
                                                   const assets::PrefabFamilies& families,
                                                   const ThemeFurniture& furniture);

/// What drawing a room of a plan reads besides the plan: its template (wall thickness, doorway, the sockets' walls and
/// order), the system's platform manifest (the sign's text), its media family (the item), the prefabs, the lighting
/// profile and the theme pack's materials (room_look.hpp).
struct SceneData {
    RoomTemplate roomTemplate;
    PlatformManifest platform;
    MediaFamily mediaFamily;
    ScenePrefabs prefabs;
    LightingProfile lighting = eveningLightingProfile();
    ThemeMaterials theme = builtInThemeMaterials();
    /// The theme pack's furniture (I6 step 3b), resolved.
    SceneFurniture furniture;
};

/// Reads the scene data of `room` of `plan` from the asset root: the room's template (by its templateId), the
/// platform of its first system (resolvePlatform() with `platformTags`, the system's ES-DE platform tags: a system
/// systems.json does not name takes the sign and plastics of the row its tags match, as when its room was generated;
/// none, the fallback's), the room's media family, the families and every prefab scenePrefabRefs() names; the
/// platform's theme pack, resolved on the room's floor (building.hpp's themePackOf(), never the plan's record:
/// ADR-0014 §1; builtInThemeMaterials(), logged, when it does not load); `lighting`, else the default lighting profile
/// (kDefaultLightingProfile; the built-in one, logged, when it does not load), as the pack lights it
/// (themedLighting()). Errors: NotFound for a
/// room the plan lacks; those of the readers; a template whose version is not the room's (ValidationFailed); a media
/// family the registry lacks (NotFound).
[[nodiscard]] Expected<SceneData> loadSceneData(const std::filesystem::path& assetRoot, const WorldPlan& plan,
                                                const RoomId& room,
                                                const std::optional<LightingProfile>& lighting = std::nullopt,
                                                std::span<const std::string> platformTags = {});

/// A dressed span the scene leaves bare, and why.
struct UndressedSpan {
    LevelId level;
    std::uint32_t firstSlot = 0;
    std::uint32_t count = 0;
    std::string reason;

    bool operator==(const UndressedSpan&) const = default;
};

/// The scene of a room of a plan: the scene file (scene_description.hpp's format, keys sorted) and what it left out.
struct SceneBuild {
    nlohmann::json sceneFile;
    std::vector<UndressedSpan> undressed;
};

/// The scene file of `room` of `plan` (see the file comment). Errors: NotFound for a room the plan lacks or a prefab
/// `data` lacks; ValidationFailed for a dressed level or slot the fixture's prefab lacks, or more lights than the
/// scene format takes.
[[nodiscard]] Expected<SceneBuild> toSceneFile(const WorldPlan& plan, const RoomId& room, const SceneData& data);

/// The fixtures of `room` as its scene draws them (I6 step 3b): a fixture the pack's furniture replaces covers the box
/// around its pieces as drawn (in the room's frame, rounded outwards to millimetres; its yaw 0, its prefab the first
/// piece's, its id and role the plan's), every other fixture stands as the plan has it. What the variants' footprints,
/// collisions and navigation are judged with (validatePlan() of the plan holding these fixtures). Errors: those of
/// toSceneFile().
[[nodiscard]] Expected<std::vector<Fixture>> drawnFixtures(const WorldPlan& plan, const RoomId& room,
                                                           const SceneData& data);

/// toSceneFile() read back as the scene the app instantiates (scene::parseSceneDescription, context "<room id>").
[[nodiscard]] Expected<scene::SceneDescription> toSceneDescription(const WorldPlan& plan, const RoomId& room,
                                                                   const SceneData& data);

/// The scene file's text as `worldplan show --scene` writes it: two spaces of indentation, keys sorted, a final line
/// feed.
[[nodiscard]] std::string sceneFileText(const nlohmann::json& sceneFile);

/// The empty room of `roomTemplate` at its base size (I3 step 10b): what the app shows while the room's plan is being
/// made, or when there is none to show (the media root out of reach, a plan that cannot be made): toSceneDescription()
/// of a plan holding that one room and nothing else, so the shell, its surfaces, the environment and the doorway's
/// camera are the room's own; no fixture, one light (the room's pendant over the middle of the base room, as every
/// plan has it). Errors: those of toSceneFile().
[[nodiscard]] Expected<scene::SceneDescription> shellSceneDescription(
    const RoomId& room, const RoomTemplate& roomTemplate, const LightingProfile& lighting = eveningLightingProfile());

/// The camera a room's scene starts at, its doorway camera (inside the doorway at eye height, looking into the room), in
/// the room's own frame: the eye's position in meters, the yaw and pitch in degrees. The app's teleport to a room's
/// doorway stands there too (I4 step 7b, ADR-0013 §10).
struct DoorwayCamera {
    double x = 0.0;
    double eyeY = 0.0;
    double z = 0.0;
    double yawDeg = 0.0;
    double pitchDeg = 0.0;

    bool operator==(const DoorwayCamera&) const = default;
};
[[nodiscard]] DoorwayCamera doorwayCamera(const Room& room, const RoomTemplate& roomTemplate) noexcept;

/// Where an item, a dressing prop and a fixture of a room stand: the room's frame in meters (ADR-0002), shared by the
/// scene and the collection layout.
namespace room_frames {

/// translate(x, y, z mm / 1000) * the yaw about +Y (hundredths of a degree; quarter turns exact).
[[nodiscard]] glm::mat4 fixtureTransform(const Fixture& fixture) noexcept;
/// A rotation about +Y by `yawCdeg` hundredths of a degree (glm::rotate's sense; quarter turns exact).
[[nodiscard]] glm::mat4 yawRotation(std::int32_t yawCdeg) noexcept;

/// The room shell of `room`: its "room_shell" parameters and where it stands (the interior's centre on the floor).
struct ShellPlacement {
    nlohmann::json params = nlohmann::json::object();
    glm::vec3 position{0.0f};
};
[[nodiscard]] ShellPlacement roomShell(const Room& room, const RoomTemplate& roomTemplate);

} // namespace room_frames

} // namespace rv::world
