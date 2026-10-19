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
//     alone casts shadows, with the profile's strength;
//   - the profile's environment and a camera at the doorway, at eye height, looking north.
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
#include <rv/world/room_template.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// How the room is lit when it is drawn (ADR-0010 §4, style §4.2, §8.1): the plan places the lights, the profile gives
/// them colour, intensity, range and cones, so that switching profiles moves nothing (step 16 adds the second profile
/// and reads it from world.json).
struct LightStyle {
    /// sRGB "#RRGGBB".
    std::string color = "#FFE0C0";
    double intensity = 1.0;
    /// Meters, > 0.
    double range = 5.0;
    /// Spots only: the outer cone is the half angle the span subtends at the target plus `coneMarginDeg`, at least
    /// `minOuterConeDeg`, at most kMaxOuterConeDeg; the inner one `innerConeRatio` of it.
    double minOuterConeDeg = 30.0;
    double coneMarginDeg = 4.0;
    double innerConeRatio = 0.55;
};

/// The widest outer cone a profile gives a spot (the scene format takes less than 90 degrees).
inline constexpr double kMaxOuterConeDeg = 80.0;

struct LightingProfile {
    std::string name;
    LightStyle key;
    LightStyle pendant;
    LightStyle lamp;
    LightStyle picture;
    LightStyle wallWasher;
    /// The key light's shadowStrength, [0, 1].
    double keyShadowStrength = 1.0;
    /// The scene's environment (scene_description.hpp's keys): colours as sRGB "#RRGGBB".
    std::string clearColor = "#221E1A";
    std::string ambientSky = "#E6E2DC";
    std::string ambientGround = "#4A4642";
    double ambientIntensity = 0.25;
    double exposure = 1.5;
    double bloomStrength = 0.18;
    double bloomRadius = 0.5;
    double vignette = 0.05;

    [[nodiscard]] const LightStyle& style(LightRole role) const noexcept;
};

/// The evening profile (style §8.1's default): the test room's tungsten light, warm white ceiling spots and amber
/// lamps, its environment and post-processing (assets/style_tests/reference_scenes/m0_testroom.json).
[[nodiscard]] const LightingProfile& eveningLightingProfile();

/// The theme's library materials per material role of a prefab kind (the theme pack classic_16bit_warm: the test
/// room's materials; step 16 tunes them), the collection item's ("cartridge": plastic_shell, label, spine) included;
/// empty for a kind the theme does not dress.
[[nodiscard]] std::map<std::string, std::string> themeMaterials(std::string_view kind);

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

/// What drawing a room of a plan reads besides the plan: its template (wall thickness, doorway, the sockets' walls and
/// order), the system's platform manifest (the sign's text), its media family (the item), the prefabs and the lighting
/// profile.
struct SceneData {
    RoomTemplate roomTemplate;
    PlatformManifest platform;
    MediaFamily mediaFamily;
    ScenePrefabs prefabs;
    LightingProfile lighting;
};

/// Reads the scene data of `room` of `plan` from the asset root: the room's template (by its templateId), the
/// platform manifest of its system, the room's media family, the families and every prefab scenePrefabRefs() names;
/// the evening lighting profile. Errors: NotFound for a room the plan lacks; those of the readers; a template whose
/// version is not the room's (ValidationFailed); a media family the registry lacks (NotFound).
[[nodiscard]] Expected<SceneData> loadSceneData(const std::filesystem::path& assetRoot, const WorldPlan& plan,
                                                const RoomId& room);

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
