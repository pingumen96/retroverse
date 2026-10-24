#include <rv/world/scene_builder.hpp>

#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_transform.hpp>

#include <rv/core/math.hpp>
#include <rv/core/random.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/world/seeds.hpp>

#include "room_parts.hpp"

namespace rv::world {
namespace {

using nlohmann::json;

/// The walker's eye above the floor (scene::FirstPersonController's default) and where the scene's camera stands
/// inside the doorway.
constexpr double kEyeHeight = 1.65;
constexpr double kCameraInsideDoorway = 0.8;
constexpr double kCameraPitchDeg = -6.0;

/// The room shell's surfaces (the test room's: oak planks and painted plaster repeating every 2 m, a varnished
/// wainscot under a rail at 1 m, walnut trim).
constexpr double kShellMetersPerTile = 2.0;
constexpr double kShellBaseboard = 0.1;
constexpr double kShellWainscot = 1.0;

/// The shelf dressing's parameter ranges (geometry::ShelfDressingParams), in millimetres.
constexpr std::int64_t kDressingMinWidthMm = 100;
constexpr std::int64_t kDressingMaxWidthMm = 3000;
constexpr std::int64_t kDressingMinSideMm = 50;
constexpr std::int64_t kDressingMaxSideMm = 1000;

/// Invented titles for the posters and the magazines' covers (style §17.2: no real title or brand).
constexpr std::array<std::string_view, 10> kPictureTitles{
    "PIXEL PILOTS",     "SUNSET SPEEDWAY", "CASTLE OF LANTERNS", "MOONLIGHT DERBY", "NEON HARBOR",
    "STARLIGHT SAFARI", "CRYSTAL CANYON",  "TURBO TIDES",        "COMET CARNIVAL",  "LANTERN LAGOON"};

double meters(std::int64_t millimetres) noexcept {
    return static_cast<double>(millimetres) / 1000.0;
}

double degrees(std::int32_t centidegrees) noexcept {
    return static_cast<double>(centidegrees) / 100.0;
}

/// `value` rounded to 1 / `perUnit` (perUnit a power of ten), so that a float computed from the prefabs writes one text.
double rounded(double value, double perUnit) noexcept {
    const double result = std::round(value * perUnit) / perUnit;
    return result == 0.0 ? 0.0 : result; // no "-0.0"
}

json vec3Json(double x, double y, double z) {
    return json::array({x, y, z});
}

json pointJson(std::int64_t xMm, std::int64_t yMm, std::int64_t zMm) {
    return vec3Json(meters(xMm), meters(yMm), meters(zMm));
}

std::int64_t toMm(float value) noexcept {
    return static_cast<std::int64_t>(std::llround(static_cast<double>(value) * 1000.0));
}

json coverTexture(std::uint64_t seed) {
    core::Random random(seed);
    const auto title = kPictureTitles[static_cast<std::size_t>(random.below(kPictureTitles.size()))];
    return json{{"pattern", "cover"}, {"title", std::string(title)}, {"seed", random.below(1000000)}};
}

/// The textures of a prefab kind's roles: the CRT's idle colour bars, a poster's or a magazine's invented cover, the
/// sign's neon lettering (I6 step 3: a neon panel in the pack's accent, its material's colour).
json themeTextures(std::string_view kind, bool sign, const PlatformManifest& platform, std::uint64_t seed) {
    if (kind == "crt") {
        return json{{"screen", json{{"pattern", "color_bars"}}}};
    }
    if (kind == "poster_frame") {
        return sign ? json{{"picture", json{{"pattern", "neon"}, {"title", platform.signText}}}}
                    : json{{"picture", coverTexture(seed)}};
    }
    if (kind == "magazine_stack") {
        return json{{"cover", coverTexture(seed)}};
    }
    return json::object();
}

json materialsJson(const std::map<std::string, std::string>& materials) {
    json object = json::object();
    for (const auto& [role, material] : materials) {
        object[role] = material;
    }
    return object;
}

const Room* findRoom(const WorldPlan& plan, const RoomId& id) noexcept {
    for (const Room& room : plan.rooms) {
        if (room.id == id) {
            return &room;
        }
    }
    return nullptr;
}

/// A prefab object of the scene file: the prefab as it resolved (with `params` instead of its own when given), its
/// theme's materials (those of the hero corner's part `heroPart` when the theme lists it), where it stands.
json prefabObject(std::string id, const ScenePrefab& prefab, const json* params, json position, json rotationDeg,
                  std::string_view heroPart, const SceneData& data, std::uint64_t seed) {
    const bool sign = heroPart == "sign";
    json object{{"id", std::move(id)}, {"position", std::move(position)}, {"rotationDeg", std::move(rotationDeg)}};
    if (!prefab.prefab) {
        object["model"] = prefab.model;
        return object;
    }
    object["prefab"] = prefab.generator;
    object["params"] = params != nullptr ? *params : prefab.params;
    const std::map<std::string, std::string> materials = data.theme.materials(prefab.generator, heroPart);
    // A pack that lights its shelves (I6 step 3, room_look.hpp: the "board_light" role) draws them lit inside.
    if ((prefab.generator == "shelf" || prefab.generator == "wall_shelf") && materials.contains("board_light") &&
        object["params"].is_object()) {
        object["params"]["boardLight"] = true;
    }
    object["materials"] = materialsJson(materials);
    if (json textures = themeTextures(prefab.generator, sign, data.platform, seed); !textures.empty()) {
        object["textures"] = std::move(textures);
    }
    // Thin geometry on walls casts no shadow (its shadow would be narrower than the filter); the rug lies on the floor
    // and is walked over.
    if (prefab.generator == "poster_frame" || prefab.generator == "rug") {
        object["castsShadows"] = false;
    }
    if (prefab.generator == "rug") {
        object["collides"] = false;
    }
    return object;
}

/// R = Ry * Rx * Rz (the scene format's order) of a rigid transform, in degrees rounded to 1e-4.
json rotationOf(const glm::mat4& m) {
    // glm is column-major: element (row r, column c) is m[c][r].
    const double r02 = m[2][0];
    const double r22 = m[2][2];
    const double r12 = m[2][1];
    const double r10 = m[0][1];
    const double r11 = m[1][1];
    constexpr double kToDegrees = 180.0 / 3.14159265358979323846;
    const double x = std::asin(std::clamp(-r12, -1.0, 1.0)) * kToDegrees;
    const double y = std::atan2(r02, r22) * kToDegrees;
    const double z = std::atan2(r10, r11) * kToDegrees;
    return vec3Json(rounded(x, 1e4), rounded(y, 1e4), rounded(z, 1e4));
}

json positionOf(const glm::mat4& m) {
    return vec3Json(rounded(m[3][0], 1e6), rounded(m[3][1], 1e6), rounded(m[3][2], 1e6));
}

json lightJson(const PlanLight& light, const LightingProfile& profile) {
    const LightStyle& style = profile.style(light.role);
    json object{{"name", light.id.localText()},
                {"position", pointJson(light.xMm, light.yMm, light.zMm)},
                {"color", style.color},
                {"intensity", style.intensity},
                {"range", style.range}};
    if (lightKind(light.role) == LightKind::Point) {
        object["type"] = "point";
    } else {
        object["type"] = "spot";
        object["target"] = pointJson(light.targetXMm, light.targetYMm, light.targetZMm);
        const double dx = meters(light.targetXMm - light.xMm);
        const double dy = meters(light.targetYMm - light.yMm);
        const double dz = meters(light.targetZMm - light.zMm);
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        constexpr double kToDegrees = 180.0 / 3.14159265358979323846;
        const double subtended = std::atan2(0.5 * meters(light.spanMm), distance) * kToDegrees;
        const double outer =
            std::clamp(std::max(subtended + style.coneMarginDeg, style.minOuterConeDeg), 1.0, kMaxOuterConeDeg);
        object["outerConeDeg"] = rounded(outer, 100.0);
        object["innerConeDeg"] = rounded(outer * std::clamp(style.innerConeRatio, 0.0, 1.0), 100.0);
    }
    if (light.role == LightRole::Key) {
        object["castsShadows"] = true;
        object["shadowStrength"] = profile.keyShadowStrength;
    }
    return object;
}

/// The camera inside the doorway at eye height, looking into the room (doorwayCamera()).
json cameraJson(const Room& room, const RoomTemplate& roomTemplate) {
    const DoorwayCamera camera = doorwayCamera(room, roomTemplate);
    return json{{"position", vec3Json(camera.x, camera.eyeY, camera.z)},
                {"yawDeg", camera.yawDeg},
                {"pitchDeg", camera.pitchDeg},
                {"horizontalFovDeg", 75.0},
                {"near", 0.05},
                {"far", 100.0}};
}

json environmentJson(const LightingProfile& profile) {
    return json{{"clearColor", profile.clearColor},
                {"ambientSky", profile.ambientSky},
                {"ambientGround", profile.ambientGround},
                {"ambientIntensity", profile.ambientIntensity},
                {"fillColor", profile.fillColor},
                {"fillIntensity", profile.fillIntensity},
                {"exposure", profile.exposure},
                {"bloomStrength", profile.bloomStrength},
                {"bloomRadius", profile.bloomRadius},
                {"vignette", profile.vignette},
                {"ssao", json{{"enabled", profile.ssaoEnabled},
                              {"radius", profile.ssaoRadius},
                              {"intensity", profile.ssaoIntensity}}}};
}

/// An accent light of the profile at its anchor's pivot and yaw (meters rounded to micrometres).
json accentJson(const AccentLight& accent, const Fixture& anchor) {
    const glm::mat4 frame = room_frames::fixtureTransform(anchor);
    const auto toRoom = [&](const glm::dvec3& local) {
        const glm::vec4 p = frame * glm::vec4(glm::vec3(local), 1.0f);
        return vec3Json(rounded(p.x, 1e6), rounded(p.y, 1e6), rounded(p.z, 1e6));
    };
    json object{{"name", "accent:" + accent.name},
                {"position", toRoom(accent.position)},
                {"color", accent.color},
                {"intensity", accent.intensity},
                {"range", accent.range}};
    if (accent.spot) {
        object["type"] = "spot";
        object["target"] = toRoom(accent.target);
        object["outerConeDeg"] = accent.outerConeDeg;
        object["innerConeDeg"] = accent.innerConeDeg;
    } else {
        object["type"] = "point";
    }
    return object;
}

/// A riser card (detail::riserCard) as a poster frame of the theme's "riser_card" materials with an invented cover.
json riserCardObject(std::string id, const detail::RiserCard& card, const SceneData& data, std::uint64_t seed) {
    json object{{"id", std::move(id)},
                {"prefab", "poster_frame"},
                {"params", card.params},
                {"position", positionOf(card.transform)},
                {"rotationDeg", rotationOf(card.transform)},
                {"materials", materialsJson(data.theme.materials("riser_card"))},
                {"textures", json{{"picture", coverTexture(seed)}}},
                {"castsShadows", false},
                {"collides", false}};
    return object;
}

/// The dressed span's object id: "dress:fx:w03/l:2", with ".<first slot>" for a span that does not start at slot 0.
std::string dressingId(const DressedLevel& dressed) {
    std::string id = "dress:" + dressed.level.localText();
    if (dressed.firstSlot != 0) {
        id += "." + std::to_string(dressed.firstSlot);
    }
    return id;
}

} // namespace

const ScenePrefab* ScenePrefabs::find(const PrefabRef& ref) const noexcept {
    const auto it = std::lower_bound(prefabs.begin(), prefabs.end(), ref,
                                     [](const ScenePrefab& prefab, const PrefabRef& key) { return prefab.ref < key; });
    return it != prefabs.end() && it->ref == ref ? &*it : nullptr;
}

std::vector<PrefabRef> scenePrefabRefs(const WorldPlan& plan, const RoomId& room, const MediaFamily& family) {
    std::vector<PrefabRef> refs;
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() == room) {
            refs.push_back(fixture.prefab);
        }
    }
    for (const DressedLevel& dressed : plan.dressing) {
        if (dressed.level.fixture().room() == room) {
            refs.push_back(dressed.prop);
        }
    }
    refs.push_back(family.item);
    std::sort(refs.begin(), refs.end());
    refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
    return refs;
}

Expected<ScenePrefabs> loadScenePrefabs(const std::filesystem::path& assetRoot, const assets::PrefabFamilies& families,
                                        std::span<const PrefabRef> refs) {
    ScenePrefabs result;
    for (const PrefabRef& ref : refs) {
        auto resolved = assets::resolvePrefab(assetRoot, families, ref.manifest);
        if (!resolved) {
            Error error = resolved.error();
            error.context = error.context.empty() ? ref.manifest : ref.manifest + ": " + error.context;
            return propagate(std::move(error));
        }
        ScenePrefab prefab;
        prefab.ref = ref;
        prefab.source = resolved->source;
        prefab.fallbackReason = resolved->fallbackReason;
        if (resolved->prefab) {
            if (resolved->generator != ref.kind) {
                return makeError(ErrorCode::ValidationFailed,
                                 fmt::format("resolves to a '{}' prefab, not the '{}' the plan names",
                                             resolved->generator, ref.kind),
                                 ref.manifest);
            }
            prefab.generator = resolved->generator;
            prefab.params = resolved->params.is_object() ? resolved->params : json::object();
            prefab.prefab = std::move(resolved->prefab);
        } else if (resolved->manifest && resolved->manifest->runtime.model) {
            prefab.model = *resolved->manifest->runtime.model;
        } else {
            return makeError(ErrorCode::ValidationFailed, "resolves to neither a procedural prefab nor a model",
                             ref.manifest);
        }
        result.prefabs.push_back(std::move(prefab));
    }
    std::sort(result.prefabs.begin(), result.prefabs.end(),
              [](const ScenePrefab& a, const ScenePrefab& b) { return a.ref < b.ref; });
    result.prefabs.erase(std::unique(result.prefabs.begin(), result.prefabs.end(),
                                     [](const ScenePrefab& a, const ScenePrefab& b) { return a.ref == b.ref; }),
                         result.prefabs.end());
    return result;
}

Expected<SceneData> loadSceneData(const std::filesystem::path& assetRoot, const WorldPlan& plan, const RoomId& roomId,
                                  const std::optional<LightingProfile>& lighting,
                                  std::span<const std::string> platformTags) {
    const Room* room = findRoom(plan, roomId);
    if (room == nullptr) {
        return makeError(ErrorCode::NotFound, "the plan has no such room", roomId.toString());
    }
    SceneData data;
    auto roomTemplate = loadRoomTemplate(assetRoot, room->templateId);
    if (!roomTemplate) {
        return propagate(roomTemplate.error());
    }
    if (roomTemplate->version != room->templateVersion) {
        return makeError(ErrorCode::ValidationFailed,
                         fmt::format("the room was generated from version {} of its template, the asset root holds "
                                     "version {}",
                                     room->templateVersion, roomTemplate->version),
                         room->templateId);
    }
    data.roomTemplate = std::move(*roomTemplate);
    // The platform of the room's first system (its manifest, or what systems.json gives it, by its platform tags for a
    // system without a row), with the room's recorded family, which is authoritative (ADR-0013 §3), and the theme pack
    // resolved now on the room's floor (ADR-0014 §1: a look, never the plan's record).
    auto building = loadBuildingData(assetRoot);
    if (!building) {
        return propagate(building.error());
    }
    auto systems = loadSystemsData(assetRoot);
    if (!systems) {
        return propagate(systems.error());
    }
    const catalog::SystemName system = room->sections.empty() ? roomId.system() : room->sections.front();
    if (room->kind == RoomKind::Console) {
        auto platform = resolvePlatform(assetRoot, system, platformTags, *systems, *building, room->floor);
        if (!platform) {
            return propagate(platform.error());
        }
        data.platform = std::move(*platform);
    } else {
        // A shared or arcade room stands as its floor's first kind, without a console (ADR-0013 §6).
        data.platform = sharedRoomPlatform(room->kind, room->floor, system, *systems, *building);
    }
    data.platform.mediaFamily = room->mediaFamily;
    auto registry = loadMediaFamilies(assetRoot);
    if (!registry) {
        return propagate(registry.error());
    }
    const MediaFamily* family = registry->find(room->mediaFamily);
    if (family == nullptr) {
        return makeError(ErrorCode::NotFound, "the media family registry has no such family", room->mediaFamily);
    }
    data.mediaFamily = *family;
    // The template as the room's family stands it (ADR-0013 §4); the plan's fixtures name their prefabs themselves.
    data.roomTemplate = withFamilyFixtures(std::move(data.roomTemplate), data.mediaFamily);
    auto families = assets::loadPrefabFamilies(assetRoot);
    if (!families) {
        return propagate(families.error());
    }
    const std::vector<PrefabRef> refs = scenePrefabRefs(plan, roomId, data.mediaFamily);
    auto prefabs = loadScenePrefabs(assetRoot, *families, refs);
    if (!prefabs) {
        return propagate(prefabs.error());
    }
    data.prefabs = std::move(*prefabs);
    if (auto theme = loadThemeMaterials(assetRoot, data.platform.themePack)) {
        data.theme = std::move(*theme);
    } else {
        spdlog::warn("World: the theme pack \"{}\" does not load ({}); the built-in materials are used",
                     data.platform.themePack, theme.error().toString());
        data.theme = builtInThemeMaterials();
    }
    if (lighting) {
        data.lighting = *lighting;
    } else if (auto profile = loadLightingProfile(assetRoot, kDefaultLightingProfile)) {
        data.lighting = std::move(*profile);
    } else {
        spdlog::warn("World: the lighting profile \"{}\" does not load ({}); the built-in evening profile is used",
                     kDefaultLightingProfile, profile.error().toString());
        data.lighting = eveningLightingProfile();
    }
    // The pack's light: its tints of the profile's roles, the sign's accent in its neon (I6 step 3).
    data.lighting = themedLighting(data.lighting, data.theme);
    return data;
}

Expected<SceneBuild> toSceneFile(const WorldPlan& plan, const RoomId& roomId, const SceneData& data) {
    const Room* room = findRoom(plan, roomId);
    if (room == nullptr) {
        return makeError(ErrorCode::NotFound, "the plan has no such room", roomId.toString());
    }
    SceneBuild build;
    json objects = json::array();

    const room_frames::ShellPlacement shell = room_frames::roomShell(*room, data.roomTemplate);
    objects.push_back(json{{"id", "room"},
                           {"prefab", "room_shell"},
                           {"params", shell.params},
                           {"position", vec3Json(shell.position.x, shell.position.y, shell.position.z)},
                           {"materials", materialsJson(data.theme.materials("room_shell"))}});

    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() != roomId) {
            continue;
        }
        const ScenePrefab* prefab = data.prefabs.find(fixture.prefab);
        if (prefab == nullptr) {
            return makeError(ErrorCode::NotFound, "no prefab for the fixture's " + fixture.prefab.manifest,
                             fixture.id.toString());
        }
        const std::string heroPart = fixture.role == FixtureRole::HeroCorner ? fixture.id.part() : std::string();
        objects.push_back(prefabObject(fixture.id.localText(), *prefab, nullptr,
                                       pointJson(fixture.xMm, fixture.yMm, fixture.zMm),
                                       vec3Json(0.0, degrees(fixture.yawCdeg), 0.0), heroPart, data,
                                       fixtureSeed(room->seed, fixture.id)));
    }

    for (const DressedLevel& dressed : plan.dressing) {
        if (dressed.level.fixture().room() != roomId) {
            continue;
        }
        const Fixture* fixture = nullptr;
        for (const Fixture& candidate : plan.fixtures) {
            if (candidate.id == dressed.level.fixture()) {
                fixture = &candidate;
                break;
            }
        }
        if (fixture == nullptr) {
            return makeError(ErrorCode::NotFound, "the plan has no fixture of the dressed level",
                             dressed.level.toString());
        }
        const ScenePrefab* fixturePrefab = data.prefabs.find(fixture->prefab);
        const ScenePrefab* prop = data.prefabs.find(dressed.prop);
        if (fixturePrefab == nullptr || prop == nullptr) {
            return makeError(ErrorCode::NotFound, "no prefab for the dressed level or its prop",
                             dressed.level.toString());
        }
        std::string reason;
        auto placed = detail::dressingProp(*room, *fixture, *fixturePrefab, dressed, *prop, reason);
        if (!placed) {
            return propagate(placed.error());
        }
        if (!*placed) {
            // A level too thin for any prop (a display case's leaning level) shows a riser card instead.
            if (auto card = detail::riserCard(*fixture, *fixturePrefab, dressed)) {
                objects.push_back(riserCardObject(dressingId(dressed), *card, data, propSeed(room->seed, dressed.level)));
                continue;
            }
            build.undressed.push_back(UndressedSpan{dressed.level, dressed.firstSlot, dressed.count, std::move(reason)});
            continue;
        }
        objects.push_back(prefabObject(dressingId(dressed), *prop, &(*placed)->params, positionOf((*placed)->transform),
                                       rotationOf((*placed)->transform), {}, data,
                                       propSeed(room->seed, dressed.level)));
    }

    json lights = json::array();
    for (const PlanLight& light : plan.lights) {
        if (light.id.room() == roomId) {
            lights.push_back(lightJson(light, data.lighting));
        }
    }
    if (lights.size() > kMaxSceneLights) {
        return makeError(ErrorCode::ValidationFailed,
                         fmt::format("{} lights, more than the {} a scene takes", lights.size(),
                                     kMaxSceneLights),
                         roomId.toString());
    }
    // The profile's accents on the hero corner's parts and the display fixtures, while the scene's limit leaves room
    // for them.
    for (const AccentLight& accent : data.lighting.accents) {
        for (const Fixture& fixture : plan.fixtures) {
            if (lights.size() >= kMaxSceneLights) {
                break;
            }
            if (fixture.id.room() != roomId) {
                continue;
            }
            if (accent.on == AccentAnchor::HeroCorner && fixture.role == FixtureRole::HeroCorner &&
                fixture.id.part() == accent.anchor) {
                lights.push_back(accentJson(accent, fixture));
                break;
            }
            if (accent.on == AccentAnchor::Display && fixture.role == FixtureRole::Display &&
                fixture.prefab.kind == accent.anchor) {
                json light = accentJson(accent, fixture);
                light["name"] = "accent:" + accent.name + ":" + fixture.id.socket();
                lights.push_back(std::move(light));
            }
        }
    }

    build.sceneFile = json{{"schemaVersion", scene::kSceneSchemaVersion},
                           {"name", roomId.toString()},
                           {"camera", cameraJson(*room, data.roomTemplate)},
                           {"environment", environmentJson(data.lighting)},
                           {"lights", std::move(lights)},
                           {"objects", std::move(objects)}};
    return build;
}

Expected<scene::SceneDescription> toSceneDescription(const WorldPlan& plan, const RoomId& room, const SceneData& data) {
    auto build = toSceneFile(plan, room, data);
    if (!build) {
        return propagate(build.error());
    }
    return scene::parseSceneDescription(build->sceneFile, room.toString());
}

std::string sceneFileText(const nlohmann::json& sceneFile) {
    return sceneFile.dump(2) + "\n";
}

Expected<scene::SceneDescription> shellSceneDescription(const RoomId& roomId, const RoomTemplate& roomTemplate,
                                                        const LightingProfile& lighting) {
    const Footprint interior = interiorFootprint(roomTemplate, 0);
    Room room;
    room.id = roomId;
    room.templateId = roomTemplate.id;
    room.templateVersion = roomTemplate.version;
    room.minXMm = interior.minXMm;
    room.maxXMm = interior.maxXMm;
    room.minZMm = interior.minZMm;
    room.maxZMm = interior.maxZMm;
    room.heightMm = roomTemplate.heightMm;
    WorldPlan plan;
    // The pendant every room has (I3 step 3: over the middle of the base room, 1.1 m below the ceiling), so that the
    // empty room reads as a room and not as a dark box.
    constexpr std::int64_t kShellPendantBelowCeilingMm = 1100;
    auto pendant = FixtureId::make(roomId, "room", "pendant");
    if (!pendant) {
        return propagate(pendant.error());
    }
    PlanLight light;
    light.id = *pendant;
    light.role = LightRole::Pendant;
    light.xMm = interior.minXMm + (interior.maxXMm - interior.minXMm) / 2;
    light.yMm = roomTemplate.heightMm - kShellPendantBelowCeilingMm;
    light.zMm = interior.minZMm + (interior.maxZMm - interior.minZMm) / 2;
    light.targetXMm = light.xMm;
    light.targetYMm = light.yMm;
    light.targetZMm = light.zMm;
    plan.lights.push_back(light);
    plan.rooms.push_back(std::move(room));
    SceneData data;
    data.roomTemplate = roomTemplate;
    data.lighting = lighting;
    return toSceneDescription(plan, roomId, data);
}

DoorwayCamera doorwayCamera(const Room& room, const RoomTemplate& roomTemplate) noexcept {
    const Doorway& door = roomTemplate.doorway;
    DoorwayCamera camera;
    camera.eyeY = kEyeHeight;
    camera.pitchDeg = kCameraPitchDeg;
    switch (door.wall) {
    case Wall::North:
        camera.x = meters(door.offsetMm);
        camera.z = meters(room.minZMm) + kCameraInsideDoorway;
        camera.yawDeg = 180.0;
        break;
    case Wall::East:
        camera.x = meters(room.maxXMm) - kCameraInsideDoorway;
        camera.z = meters(door.offsetMm);
        camera.yawDeg = -90.0;
        break;
    case Wall::West:
        camera.x = meters(room.minXMm) + kCameraInsideDoorway;
        camera.z = meters(door.offsetMm);
        camera.yawDeg = 90.0;
        break;
    case Wall::South:
    case Wall::None:
        camera.x = meters(door.offsetMm);
        camera.z = meters(room.maxZMm) - kCameraInsideDoorway;
        break;
    }
    return camera;
}

namespace room_frames {

glm::mat4 yawRotation(std::int32_t yawCdeg) noexcept {
    const std::int32_t normalized = ((yawCdeg % 36000) + 36000) % 36000;
    glm::mat4 rotation(1.0f);
    float c = 1.0f;
    float s = 0.0f;
    switch (normalized) {
    case 0:
        break;
    case 9000:
        c = 0.0f;
        s = 1.0f;
        break;
    case 18000:
        c = -1.0f;
        s = 0.0f;
        break;
    case 27000:
        c = 0.0f;
        s = -1.0f;
        break;
    default: {
        const double radians = static_cast<double>(yawCdeg) / 100.0 * (3.14159265358979323846 / 180.0);
        c = static_cast<float>(std::cos(radians));
        s = static_cast<float>(std::sin(radians));
        break;
    }
    }
    // glm::rotate about +Y: x' = c x + s z, z' = -s x + c z (counter-clockwise seen from above).
    rotation[0] = glm::vec4(c, 0.0f, -s, 0.0f);
    rotation[2] = glm::vec4(s, 0.0f, c, 0.0f);
    return rotation;
}

glm::mat4 fixtureTransform(const Fixture& fixture) noexcept {
    glm::mat4 transform = yawRotation(fixture.yawCdeg);
    transform[3] = glm::vec4(static_cast<float>(meters(fixture.xMm)), static_cast<float>(meters(fixture.yMm)),
                             static_cast<float>(meters(fixture.zMm)), 1.0f);
    return transform;
}

ShellPlacement roomShell(const Room& room, const RoomTemplate& roomTemplate) {
    ShellPlacement shell;
    const std::int64_t centreXMm = (room.minXMm + room.maxXMm) / 2;
    const std::int64_t centreZMm = (room.minZMm + room.maxZMm) / 2;
    shell.position = glm::vec3(static_cast<float>(meters(centreXMm)), 0.0f, static_cast<float>(meters(centreZMm)));
    json doorway = nullptr;
    const Doorway& door = roomTemplate.doorway;
    if (door.wall != Wall::None) {
        // The template's offset is along +X from the base room's centre for the north and south walls, along +Z for
        // the east and west walls; the shell's is from its own wall's centre.
        const std::int64_t offsetMm =
            door.wall == Wall::North || door.wall == Wall::South ? door.offsetMm - centreXMm : door.offsetMm - centreZMm;
        doorway = json{{"wall", std::string(toString(door.wall))},
                       {"offset", meters(offsetMm)},
                       {"width", meters(door.widthMm)},
                       {"height", meters(door.heightMm)}};
    }
    shell.params = json{{"size", json::array({meters(room.maxXMm - room.minXMm), meters(room.maxZMm - room.minZMm)})},
                        {"wallHeight", meters(room.heightMm)},
                        {"wallThickness", meters(roomTemplate.wallThicknessMm)},
                        {"metersPerTile", kShellMetersPerTile},
                        {"ceiling", true},
                        {"baseboardHeight", kShellBaseboard},
                        {"wainscotHeight", kShellWainscot},
                        {"doorway", std::move(doorway)}};
    return shell;
}

} // namespace room_frames

namespace detail {

const geometry::Socket* slotSocket(const geometry::ProceduralPrefab& prefab, std::uint32_t level,
                                   std::uint32_t index) noexcept {
    for (const geometry::Socket& socket : prefab.sockets) {
        if (socket.kind != geometry::SocketKind::ShelfSlot) {
            continue;
        }
        const auto address = geometry::parseSlotId(socket.id);
        if (address && static_cast<std::uint32_t>(address->level) == level &&
            static_cast<std::uint32_t>(address->index) == index) {
            return &socket;
        }
    }
    return nullptr;
}

const geometry::Socket* levelSocket(const geometry::ProceduralPrefab& prefab, std::uint32_t level) noexcept {
    for (const geometry::Socket& socket : prefab.sockets) {
        const auto parsed = geometry::parseLevelId(socket.id);
        if (parsed && static_cast<std::uint32_t>(*parsed) == level) {
            return &socket;
        }
    }
    return nullptr;
}

std::uint32_t slotCount(const geometry::ProceduralPrefab& prefab, std::uint32_t level) noexcept {
    std::uint32_t count = 0;
    for (const geometry::Socket& socket : prefab.sockets) {
        if (socket.kind != geometry::SocketKind::ShelfSlot) {
            continue;
        }
        const auto address = geometry::parseSlotId(socket.id);
        if (address && static_cast<std::uint32_t>(address->level) == level) {
            ++count;
        }
    }
    return count;
}

Expected<std::optional<DressingProp>> dressingProp(const Room& room, const Fixture& fixture,
                                                   const ScenePrefab& fixturePrefab, const DressedLevel& dressed,
                                                   const ScenePrefab& prop, std::string& reason) {
    if (!fixturePrefab.prefab) {
        reason = "the fixture is a model without level sockets";
        return std::optional<DressingProp>{};
    }
    if (!prop.prefab) {
        reason = "the prop is a model, which the dressing cannot size";
        return std::optional<DressingProp>{};
    }
    const geometry::ProceduralPrefab& built = *fixturePrefab.prefab;
    const std::uint32_t levelNumber = dressed.level.level();
    const geometry::Socket* level = levelSocket(built, levelNumber);
    const std::uint32_t slots = slotCount(built, levelNumber);
    if (level == nullptr || dressed.count == 0 || dressed.firstSlot + dressed.count > slots) {
        return makeError(ErrorCode::ValidationFailed,
                         fmt::format("the fixture's prefab has no level {} holding slots {} to {}", levelNumber,
                                     dressed.firstSlot, dressed.firstSlot + dressed.count),
                         dressed.level.toString());
    }
    const core::Aabb& volume = level->bounds;
    float left = volume.min.x;
    float right = volume.max.x;
    if (dressed.firstSlot != 0 || dressed.count != slots) {
        // A part of the level: from the first slot's left edge to the last one's right edge, in the level's frame.
        const glm::mat4 toLevel = glm::inverse(level->transform);
        const geometry::Socket* first = slotSocket(built, levelNumber, dressed.firstSlot);
        const geometry::Socket* last = slotSocket(built, levelNumber, dressed.firstSlot + dressed.count - 1);
        if (first == nullptr || last == nullptr) {
            return makeError(ErrorCode::ValidationFailed, "the fixture's prefab lacks a slot of the dressed span",
                             dressed.level.toString());
        }
        const float firstX = (toLevel * first->transform)[3].x;
        const float lastX = (toLevel * last->transform)[3].x;
        left = std::max(volume.min.x, firstX + first->bounds.min.x);
        right = std::min(volume.max.x, lastX + last->bounds.max.x);
    }
    const std::int64_t widthMm = toMm(right - left);
    const std::int64_t heightMm = toMm(volume.size().y);
    const std::int64_t depthMm = toMm(volume.size().z);
    if (widthMm < kDressingMinWidthMm || widthMm > kDressingMaxWidthMm || heightMm < kDressingMinSideMm ||
        heightMm > kDressingMaxSideMm || depthMm < kDressingMinSideMm || depthMm > kDressingMaxSideMm) {
        reason = fmt::format("its free volume, {} x {} x {} mm, is outside what a {} prop takes", widthMm, heightMm,
                             depthMm, prop.generator);
        return std::optional<DressingProp>{};
    }
    DressingProp result;
    result.generator = prop.generator;
    result.params = prop.params;
    result.params["width"] = meters(widthMm);
    result.params["height"] = meters(heightMm);
    result.params["depth"] = meters(depthMm);
    result.params["seed"] = static_cast<std::int64_t>(propSeed(room.seed, dressed.level) & 0x7FFFFFFFu);
    result.size = glm::vec3(static_cast<float>(meters(widthMm)), static_cast<float>(meters(heightMm)),
                            static_cast<float>(meters(depthMm)));
    // Pivot floor in the middle of the span, on the level's surface, centred in its depth.
    const float centreX = static_cast<float>(meters(toMm(0.5f * (left + right))));
    const float centreZ = static_cast<float>(meters(toMm(volume.center().z)));
    const float floorY = static_cast<float>(meters(toMm(volume.min.y)));
    result.transform = room_frames::fixtureTransform(fixture) * level->transform *
                       glm::translate(glm::mat4(1.0f), glm::vec3(centreX, floorY, centreZ));
    return std::optional<DressingProp>{std::move(result)};
}

std::optional<RiserCard> riserCard(const Fixture& fixture, const ScenePrefab& fixturePrefab,
                                   const DressedLevel& dressed) {
    if (!fixturePrefab.prefab || fixturePrefab.generator != "display_case") {
        return std::nullopt;
    }
    const geometry::ProceduralPrefab& built = *fixturePrefab.prefab;
    const std::uint32_t levelNumber = dressed.level.level();
    const geometry::Socket* level = levelSocket(built, levelNumber);
    const std::uint32_t slots = slotCount(built, levelNumber);
    if (level == nullptr || dressed.count == 0 || dressed.firstSlot + dressed.count > slots) {
        return std::nullopt;
    }
    const geometry::Socket* first = slotSocket(built, levelNumber, dressed.firstSlot);
    const geometry::Socket* last = slotSocket(built, levelNumber, dressed.firstSlot + dressed.count - 1);
    if (first == nullptr || last == nullptr) {
        return std::nullopt;
    }
    // The card spans the slots' width less a margin and stands as tall as their items, leaning on the level's panel
    // like the items it stands in for; a card no wider than kMinCardWidthMm is not worth showing.
    const glm::mat4 toLevel = glm::inverse(level->transform);
    const glm::mat4 firstFrame = toLevel * first->transform;
    const glm::mat4 lastFrame = toLevel * last->transform;
    const float left = firstFrame[3].x + first->bounds.min.x;
    const float right = lastFrame[3].x + last->bounds.max.x;
    constexpr float kMarginM = 0.01f;
    constexpr std::int64_t kMinCardWidthMm = 60;
    constexpr std::int64_t kCardDepthMm = 5; // poster_frame's thinnest
    constexpr std::int64_t kCardFrameMm = 5;
    const std::int64_t widthMm = toMm(right - left - 2.0f * kMarginM);
    const std::int64_t heightMm = toMm(first->bounds.size().y);
    if (widthMm < kMinCardWidthMm || heightMm < 3 * kCardFrameMm) {
        return std::nullopt;
    }
    RiserCard card;
    card.params = json{{"width", meters(widthMm)},
                       {"height", meters(heightMm)},
                       {"frameWidth", meters(kCardFrameMm)},
                       {"depth", meters(kCardDepthMm)}};
    // The poster frame's pivot is its back's centre: on the first slot's back plane, half the card up from its floor.
    const glm::vec3 slotBack = glm::vec3(firstFrame * glm::vec4(0.0f, 0.0f, first->bounds.min.z, 1.0f));
    const float centreX = static_cast<float>(meters(toMm(0.5f * (left + right))));
    const glm::mat4 slotInLevel = firstFrame;
    glm::mat4 frame = slotInLevel;
    frame[3] = glm::vec4(centreX, slotBack.y, slotBack.z, 1.0f);
    card.transform = room_frames::fixtureTransform(fixture) * level->transform * frame *
                     glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, static_cast<float>(meters(heightMm)) * 0.5f, 0.0f));
    return card;
}

} // namespace detail

} // namespace rv::world
