// The air of a room (rv/scene/atmosphere.hpp, I12 step 6, ADR-0021 §4): the emitters a scene's lights give where their
// area (or the scene) has an atmosphere, their counts, reach and seeds; the motes' drift inside their cone or pool,
// alike on every run; the atmosphere read from a scene file and refused out of its ranges.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/scene/atmosphere.hpp>
#include <rv/scene/scene_description.hpp>

using rv::scene::AtmosphereEmitter;
using rv::scene::AtmosphereKind;
using rv::scene::AtmosphereSetup;
using rv::scene::Light;
using rv::scene::LightType;
using rv::scene::SceneDescription;

namespace {

Light spot(std::string name, glm::vec3 position, glm::vec3 direction, float outer = 35.0f) {
    Light light;
    light.name = std::move(name);
    light.type = LightType::Spot;
    light.position = position;
    light.direction = glm::normalize(direction);
    light.outerConeDeg = outer;
    light.innerConeDeg = outer - 10.0f;
    light.range = 8.0f;
    return light;
}

Light point(std::string name, glm::vec3 position) {
    Light light;
    light.name = std::move(name);
    light.type = LightType::Point;
    light.position = position;
    light.range = 6.0f;
    return light;
}

/// A room's lights: its key light, its pendant, a washer, a display case's light, a lamp.
SceneDescription room(std::optional<AtmosphereSetup> atmosphere) {
    SceneDescription scene;
    scene.environment.atmosphere = atmosphere;
    scene.lights = {spot("fx:hero.key-light", {0.0f, 3.0f, -3.0f}, {0.0f, -1.0f, 0.3f}),
                    point("fx:room.pendant", {0.0f, 2.6f, 0.0f}),
                    spot("fx:hw-e.washer", {3.0f, 2.9f, 0.0f}, {0.6f, -1.0f, 0.0f}),
                    spot("accent:case_light:dc01", {1.0f, 2.8f, 1.0f}, {0.0f, -1.0f, 0.0f}),
                    point("fx:hero.lamp-light", {-1.0f, 1.5f, -2.0f})};
    return scene;
}

std::vector<AtmosphereKind> kindsOf(const std::vector<AtmosphereEmitter>& emitters) {
    std::vector<AtmosphereKind> kinds;
    for (const AtmosphereEmitter& emitter : emitters) {
        kinds.push_back(emitter.kind);
    }
    return kinds;
}

} // namespace

TEST_CASE("scene/atmosphere: a room's key light and pendant give dust, its spots cones where the pack asks",
          "[scene][atmosphere]") {
    // No atmosphere: nothing (the test room, the hub, the halls).
    CHECK(rv::scene::deriveAtmosphere(room(std::nullopt)).empty());

    // Dust alone (the default): the key light's cone and the pendant's pool.
    const std::vector<AtmosphereEmitter> dust = rv::scene::deriveAtmosphere(room(AtmosphereSetup{}));
    REQUIRE(kindsOf(dust) == std::vector{AtmosphereKind::DustCone, AtmosphereKind::DustPool});
    const AtmosphereEmitter& cone = dust[0];
    CHECK(cone.light == 0);
    CHECK(cone.motes == rv::scene::kDustConeMotes);
    CHECK_FALSE(cone.area.has_value());
    // From the light down its axis to 2 cm above the floor.
    CHECK((cone.apex + cone.axis * cone.length).y == Catch::Approx(0.02f).margin(1e-4));
    CHECK(cone.tanHalfAngle == Catch::Approx(std::tan(glm::radians(35.0f))));
    const AtmosphereEmitter& pool = dust[1];
    CHECK(pool.light == 1);
    CHECK(pool.motes == rv::scene::kDustPoolMotes);
    CHECK(pool.radius == rv::scene::kDustPoolRadiusM);
    CHECK((pool.apex + pool.axis * pool.length).y == Catch::Approx(rv::scene::kDustPoolFloorM).margin(1e-4));
    CHECK(cone.seed != pool.seed);

    // Twice the dust, twice the motes; none at 0.
    AtmosphereSetup thick;
    thick.dust = 2.0f;
    CHECK(rv::scene::deriveAtmosphere(room(thick))[0].motes == 2 * rv::scene::kDustConeMotes);
    AtmosphereSetup clear;
    clear.dust = 0.0f;
    CHECK(rv::scene::deriveAtmosphere(room(clear)).empty());

    // Light cones: the key light's, the washer's and the case light's (never the lamp's point light).
    AtmosphereSetup coned;
    coned.lightCones = true;
    coned.coneStrength = 0.05f;
    const std::vector<AtmosphereEmitter> all = rv::scene::deriveAtmosphere(room(coned));
    CHECK(kindsOf(all) == std::vector{AtmosphereKind::DustCone, AtmosphereKind::LightCone, AtmosphereKind::DustPool,
                                      AtmosphereKind::LightCone, AtmosphereKind::LightCone});
    CHECK(all[1].strength == 0.05f);
    CHECK(all[3].light == 2);
    CHECK(all[4].light == 3);
    // A light cone ends 5 cm above the floor.
    CHECK((all[4].apex + all[4].axis * all[4].length).y == Catch::Approx(rv::scene::kLightConeFloorM).margin(1e-4));

    // Alike on every run.
    CHECK(rv::scene::deriveAtmosphere(room(coned)) == all);
}

TEST_CASE("scene/atmosphere: in a floor's scene each room's own air, the hall's none, the floor its area's",
          "[scene][atmosphere]") {
    SceneDescription floor = room(std::nullopt);
    floor.areas.resize(2);
    floor.areas[0].id = "room:snes";
    floor.areas[0].boxes = {rv::core::Aabb::fromMinMax({-5.0f, 0.0f, -5.0f}, {5.0f, 3.2f, 5.0f})};
    floor.areas[0].environment.atmosphere = AtmosphereSetup{};
    floor.areas[1].id = "hall:gen4";
    floor.areas[1].boxes = {rv::core::Aabb::fromMinMax({-5.0f, 0.0f, 5.0f}, {5.0f, 3.2f, 9.0f})};
    for (Light& light : floor.lights) {
        light.area = "room:snes";
    }
    floor.lights.push_back(spot("hall:gen4/fx:hero.key-light", {0.0f, 3.0f, 7.0f}, {0.0f, -1.0f, 0.0f}));
    floor.lights.back().area = "hall:gen4";
    const std::vector<AtmosphereEmitter> emitters = rv::scene::deriveAtmosphere(floor);
    REQUIRE(emitters.size() == 2);
    CHECK(emitters[0].area == std::optional<std::size_t>(0));
    CHECK(emitters[1].area == std::optional<std::size_t>(0));
    CHECK(rv::scene::atmosphereOf(floor, 1) == std::nullopt);
}

TEST_CASE("scene/atmosphere: the motes drift within their cone and pool, fade where they wrap, alike on every run",
          "[scene][atmosphere]") {
    const std::vector<AtmosphereEmitter> emitters = rv::scene::deriveAtmosphere(room(AtmosphereSetup{}));
    REQUIRE(emitters.size() == 2);
    for (const AtmosphereEmitter& emitter : emitters) {
        CAPTURE(static_cast<int>(emitter.kind));
        std::size_t moved = 0;
        for (std::uint32_t index = 0; index < emitter.motes; ++index) {
            for (const float time : {0.0f, 0.5f, 30.0f, 1800.0f}) {
                float fade = -1.0f;
                const glm::vec3 p = rv::scene::motePosition(emitter, index, time, &fade);
                CHECK(fade >= 0.0f);
                CHECK(fade <= 1.0f);
                // Within the emitter's bounds, and within its cone or pool (the wobble aside).
                CHECK(emitter.bounds.contains(p));
                const glm::vec3 fromApex = p - emitter.apex;
                const float along = glm::dot(fromApex, emitter.axis);
                CHECK(along >= -rv::scene::kMoteWobbleM * 2.0f);
                CHECK(along <= emitter.length + rv::scene::kMoteWobbleM * 2.0f);
                const float off = glm::length(fromApex - emitter.axis * along);
                const float reach = emitter.kind == AtmosphereKind::DustPool
                                        ? emitter.radius
                                        : std::max(along, 0.0f) * emitter.tanHalfAngle;
                CHECK(off <= reach + rv::scene::kMoteWobbleM * 2.0f);
                // The same place again.
                CHECK(rv::scene::motePosition(emitter, index, time) == p);
            }
            // Half a second moves a mote, by less than a few centimetres (a drift, not a swarm).
            const float step = glm::length(rv::scene::motePosition(emitter, index, 10.5f) -
                                           rv::scene::motePosition(emitter, index, 10.0f));
            CHECK(step < 0.05f);
            moved += step > 1e-5f ? 1 : 0;
        }
        CHECK(moved == emitter.motes);
    }
    // The hash the shader mirrors (atmosphere/dust.vert): fixed values.
    CHECK(rv::scene::atmosphereHash(0u) == 0u);
    CHECK(rv::scene::atmosphereHash(1u) == 0x688990c0u);
}

TEST_CASE("scene/atmosphere: read from a scene file, refused out of its ranges", "[scene][atmosphere]") {
    const auto parse = [](const nlohmann::json& atmosphere) {
        nlohmann::json document{{"schemaVersion", 1},
                                {"name", "air"},
                                {"environment", {{"atmosphere", atmosphere}}},
                                {"areas", nlohmann::json::array({{{"id", "room:a"},
                                                                  {"boxes", {{{"min", {0, 0, 0}}, {"max", {1, 1, 1}}}}},
                                                                  {"environment", {{"atmosphere", atmosphere}}}}})}};
        return rv::scene::parseSceneDescription(document, "air.json");
    };
    const auto read = parse(
        {{"dust", 1.5}, {"lightCones", true}, {"coneStrength", 0.04}, {"neonFlicker", false}, {"crtBreathing", false}});
    REQUIRE(read.has_value());
    REQUIRE(read->environment.atmosphere.has_value());
    CHECK(read->environment.atmosphere->dust == 1.5f);
    CHECK(read->environment.atmosphere->lightCones);
    CHECK(read->environment.atmosphere->coneStrength == Catch::Approx(0.04f));
    CHECK_FALSE(read->environment.atmosphere->neonFlicker);
    CHECK_FALSE(read->environment.atmosphere->crtBreathing);
    CHECK(read->areas[0].environment.atmosphere == read->environment.atmosphere);
    // The defaults of an empty block.
    const auto defaults = parse(nlohmann::json::object());
    REQUIRE(defaults.has_value());
    CHECK(*defaults->environment.atmosphere == AtmosphereSetup{});
    // Out of range or unknown keys.
    CHECK_FALSE(parse({{"dust", 5}}).has_value());
    CHECK_FALSE(parse({{"dust", -1}}).has_value());
    CHECK_FALSE(parse({{"coneStrength", 0.5}}).has_value());
    CHECK_FALSE(parse({{"fog", true}}).has_value());
}
