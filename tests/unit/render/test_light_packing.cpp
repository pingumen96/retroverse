#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rv/core/aabb.hpp>
#include <rv/core/math.hpp>
#include <rv/render/light_packing.hpp>
#include <rv/scene/scene_description.hpp>

using Catch::Approx;
using rv::gl::LightsUniforms;
using rv::gl::PackedLights;
using rv::scene::Light;
using rv::scene::LightType;

namespace {

Light pointLight(std::string name, const glm::vec3& position) {
    Light light;
    light.name = std::move(name);
    light.type = LightType::Point;
    light.position = position;
    light.color = glm::vec3(1.0f, 0.5f, 0.25f);
    light.intensity = 4.0f;
    light.range = 6.0f;
    return light;
}

/// Cone factor of the shaders (common/lights.glsl) for a surface point seen from the light at `angleDeg` off axis.
float coneFactor(const rv::gl::PunctualLightUniforms& light, float angleDeg) {
    const float t = std::clamp(std::cos(rv::core::degreesToRadians(angleDeg)) * light.spotScale + light.spotOffset,
                               0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

TEST_CASE("render/light_packing: point and spot lights keep their order and the light contract", "[render][lights]") {
    Light spot = pointLight("accent", glm::vec3(3.2f, 2.85f, -2.2f));
    spot.type = LightType::Spot;
    spot.direction = glm::vec3(0.0f, -2.0f, 0.0f); // normalized by the packer
    spot.innerConeDeg = 18.0f;
    spot.outerConeDeg = 30.0f;
    const std::vector<Light> lights{pointLight("ceiling", glm::vec3(0.0f, 2.75f, -2.4f)), spot};

    rv::scene::EnvironmentSetup environment;
    environment.ambientSky = glm::vec3(1.0f, 0.8f, 0.6f);
    environment.ambientGround = glm::vec3(0.1f, 0.05f, 0.02f);
    environment.ambientIntensity = 0.08f;
    environment.exposure = 1.4f; // belongs to the post-processing, not to the lights block
    const PackedLights packed = rv::gl::packLights(lights, environment);
    CHECK(packed.dropped.empty());
    const LightsUniforms& block = packed.uniforms;
    REQUIRE(block.punctualLightCount == 2);

    const auto& point = block.punctualLights[0];
    CHECK(point.position == lights[0].position);
    CHECK(point.color == lights[0].color);
    CHECK(point.intensity == 4.0f);
    CHECK(point.range == 6.0f);
    // A point light has no cone: the factor is 1 in every direction.
    CHECK(point.spotScale == 0.0f);
    CHECK(point.spotOffset == 1.0f);
    CHECK(coneFactor(point, 0.0f) == 1.0f);
    CHECK(coneFactor(point, 180.0f) == 1.0f);

    const auto& accent = block.punctualLights[1];
    CHECK(accent.direction == glm::vec3(0.0f, -1.0f, 0.0f));
    CHECK(coneFactor(accent, 0.0f) == Approx(1.0f));
    CHECK(coneFactor(accent, 17.9f) == Approx(1.0f));
    CHECK(coneFactor(accent, 24.0f) > 0.0f);
    CHECK(coneFactor(accent, 24.0f) < 1.0f);
    CHECK(coneFactor(accent, 30.1f) == 0.0f);
    CHECK(coneFactor(accent, 90.0f) == 0.0f);

    // No directional light; the ambient is copied as is and the padding stays zero (the renderer compares blocks
    // byte for byte).
    CHECK(block.directionalIntensity == 0.0f);
    CHECK(block.ambientSky == environment.ambientSky);
    CHECK(block.ambientGround == environment.ambientGround);
    CHECK(block.ambientIntensity == 0.08f);
    CHECK(block.padding == 0.0f);
    // Unused entries stay dark.
    for (std::uint32_t i = 2; i < rv::gl::kMaxPunctualLights; ++i) {
        CHECK(block.punctualLights[i].intensity == 0.0f);
    }
}

TEST_CASE("render/light_packing: the cone parameters follow the smoothstep contract", "[render][lights]") {
    // spotScale and spotOffset turn dot(direction, L) into t = (x - cosOuter) / (cosInner - cosOuter).
    const glm::vec2 cone = rv::gl::spotConeScaleOffset(25.0f, 35.0f);
    const float cosInner = std::cos(rv::core::degreesToRadians(25.0f));
    const float cosOuter = std::cos(rv::core::degreesToRadians(35.0f));
    CHECK(cosOuter * cone.x + cone.y == Approx(0.0f).margin(1e-5));
    CHECK(cosInner * cone.x + cone.y == Approx(1.0f).margin(1e-5));

    // Hard-edged cones (inner == outer) stay finite; an inner angle beyond the outer one is treated as equal.
    for (const glm::vec2 hard :
         {rv::gl::spotConeScaleOffset(30.0f, 30.0f), rv::gl::spotConeScaleOffset(40.0f, 30.0f)}) {
        CHECK(std::isfinite(hard.x));
        CHECK(std::isfinite(hard.y));
        CHECK(hard.x == Approx(1e4f));
    }
}

TEST_CASE("render/light_packing: one directional light, at most kMaxPunctualLights point and spot lights",
          "[render][lights]") {
    std::vector<Light> lights;
    Light sun;
    sun.name = "sun";
    sun.type = LightType::Directional;
    sun.direction = glm::vec3(-0.4f, -0.8f, -0.45f);
    sun.color = glm::vec3(1.0f, 0.95f, 0.9f);
    sun.intensity = 3.0f;
    lights.push_back(sun);
    constexpr auto kCapacity = static_cast<int>(rv::gl::kMaxPunctualLights);
    const int bulbs = kCapacity + 2;
    for (int i = 0; i < bulbs; ++i) {
        lights.push_back(pointLight(i == bulbs - 1 ? "" : "bulb" + std::to_string(i),
                                    glm::vec3(static_cast<float>(i), 2.0f, 0.0f)));
    }
    Light moon = sun;
    moon.name = "moon";
    lights.push_back(moon);

    const PackedLights packed = rv::gl::packLights(lights, rv::scene::EnvironmentSetup{});
    const LightsUniforms& block = packed.uniforms;
    CHECK(block.punctualLightCount == static_cast<std::int32_t>(rv::gl::kMaxPunctualLights));
    CHECK(block.punctualLights[kCapacity - 1].position.x == static_cast<float>(kCapacity - 1)); // list order
    CHECK(block.directionalIntensity == 3.0f);
    CHECK(block.directionalColor == sun.color);
    CHECK(glm::length(block.directionalDirection) == Approx(1.0f));
    CHECK(rv::core::nearlyEqual(block.directionalDirection, glm::normalize(sun.direction)));

    // The two bulbs beyond the capacity (the last is unnamed: its index in the list after the sun) and the second
    // directional light.
    REQUIRE(packed.dropped.size() == 3);
    CHECK(packed.dropped[0].name == "bulb" + std::to_string(kCapacity));
    CHECK(packed.dropped[1].name == "#" + std::to_string(bulbs));
    CHECK(packed.dropped[2].name == "moon");
    for (const auto& dropped : packed.dropped) {
        CHECK_FALSE(dropped.reason.empty());
    }
}

TEST_CASE("render/light_packing: lights the shaders could not use are dropped", "[render][lights]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<Light> lights;
    Light negative = pointLight("negative", glm::vec3(0.0f));
    negative.intensity = -1.0f;
    lights.push_back(negative);
    Light lost = pointLight("lost", glm::vec3(nan, 0.0f, 0.0f));
    lights.push_back(lost);
    Light pointless = pointLight("pointless", glm::vec3(0.0f));
    pointless.type = LightType::Spot;
    pointless.direction = glm::vec3(0.0f);
    lights.push_back(pointless);
    Light tiny = pointLight("tiny", glm::vec3(1.0f));
    tiny.range = 0.0f; // kept with the 1 mm minimum range
    lights.push_back(tiny);

    const PackedLights packed = rv::gl::packLights(lights, rv::scene::EnvironmentSetup{});
    REQUIRE(packed.dropped.size() == 3);
    CHECK(packed.dropped[0].name == "negative");
    CHECK(packed.dropped[1].name == "lost");
    CHECK(packed.dropped[2].name == "pointless");
    REQUIRE(packed.uniforms.punctualLightCount == 1);
    CHECK(packed.uniforms.punctualLights[0].range == Approx(0.001f));
}

TEST_CASE("render/light_packing: the first light that casts shadows is the key light", "[render][lights][shadow]") {
    // Nothing casts shadows: no key light, nothing to say about it.
    std::vector<Light> lights{pointLight("fill", glm::vec3(0.0f, 2.0f, 0.0f))};
    PackedLights packed = rv::gl::packLights(lights, rv::scene::EnvironmentSetup{});
    CHECK_FALSE(packed.shadowLight.casts());
    CHECK(packed.shadowNote.empty());
    CHECK_FALSE(packed.keyLight.has_value());

    // A spot light: its index in the block, its geometry for the shadow map and its shadow strength.
    Light key = pointLight("key", glm::vec3(-0.2f, 2.92f, -2.2f));
    key.type = LightType::Spot;
    key.direction = glm::vec3(0.0f, -2.92f, -2.4f); // normalized by the packer
    key.outerConeDeg = 50.0f;
    key.range = 12.0f;
    key.castsShadows = true;
    key.shadowStrength = 0.8f;
    Light later = key;
    later.name = "later";
    later.position.x = 3.0f;
    lights.push_back(key);
    lights.push_back(later); // a second caster is not the key light
    packed = rv::gl::packLights(lights, rv::scene::EnvironmentSetup{});
    REQUIRE(packed.shadowLight.casts());
    CHECK(packed.shadowNote.empty());
    CHECK(packed.keyLight == std::optional<std::size_t>(1));
    CHECK(packed.shadowLight.index == 1);
    CHECK(packed.shadowLight.type == LightType::Spot);
    CHECK(packed.shadowLight.position == key.position);
    CHECK(rv::core::nearlyEqual(packed.shadowLight.direction, glm::normalize(key.direction)));
    CHECK(packed.shadowLight.direction == packed.uniforms.punctualLights[1].direction);
    CHECK(packed.shadowLight.outerConeDeg == 50.0f);
    CHECK(packed.shadowLight.range == 12.0f);
    CHECK(packed.shadowLight.strength == 0.8f);

    // The directional light, wherever it is in the list.
    Light sun;
    sun.name = "sun";
    sun.type = LightType::Directional;
    sun.direction = glm::vec3(0.0f, -2.0f, -1.0f);
    sun.castsShadows = true;
    sun.shadowStrength = 3.0f; // clamped
    packed = rv::gl::packLights(std::vector<Light>{pointLight("fill", glm::vec3(0.0f, 2.0f, 0.0f)), sun, key},
                                rv::scene::EnvironmentSetup{});
    REQUIRE(packed.shadowLight.casts());
    CHECK(packed.shadowLight.index == rv::gl::kDirectionalShadowLight);
    CHECK(packed.shadowLight.type == LightType::Directional);
    CHECK(rv::core::nearlyEqual(packed.shadowLight.direction, glm::normalize(sun.direction)));
    CHECK(packed.shadowLight.strength == 1.0f);
    sun.shadowStrength = std::numeric_limits<float>::quiet_NaN();
    CHECK(rv::gl::packLights(std::vector<Light>{sun}, rv::scene::EnvironmentSetup{}).shadowLight.strength == 1.0f);
}

TEST_CASE("render/light_packing: a point or dropped key light casts no shadow and says why",
          "[render][lights][shadow]") {
    // Point-light shadows are not supported: the key light should be a spot. A later spot does not take over.
    Light bulb = pointLight("bulb", glm::vec3(0.0f, 2.0f, 0.0f));
    bulb.castsShadows = true;
    Light spot = pointLight("spot", glm::vec3(1.0f, 2.9f, 0.0f));
    spot.type = LightType::Spot;
    spot.castsShadows = true;
    PackedLights packed = rv::gl::packLights(std::vector<Light>{bulb, spot}, rv::scene::EnvironmentSetup{});
    CHECK_FALSE(packed.shadowLight.casts());
    CHECK(packed.keyLight == std::optional<std::size_t>(0)); // the key light, though it casts nothing
    CHECK(packed.shadowNote.find("'bulb'") != std::string::npos);
    CHECK(packed.shadowNote.find("point") != std::string::npos);
    CHECK(packed.dropped.empty()); // the bulb still lights the scene

    // A key light that cannot be drawn casts nothing either.
    Light broken = spot;
    broken.name = "broken";
    broken.direction = glm::vec3(0.0f);
    packed = rv::gl::packLights(std::vector<Light>{broken, spot}, rv::scene::EnvironmentSetup{});
    CHECK_FALSE(packed.shadowLight.casts());
    CHECK(packed.shadowNote.find("'broken'") != std::string::npos);
    REQUIRE(packed.dropped.size() == 1);

    // Nor does one beyond the capacity of the block.
    std::vector<Light> crowded;
    for (std::uint32_t i = 0; i < rv::gl::kMaxPunctualLights; ++i) {
        crowded.push_back(pointLight("bulb" + std::to_string(i), glm::vec3(static_cast<float>(i), 2.0f, 0.0f)));
    }
    Light beyond = spot;
    beyond.name = "beyond";
    crowded.push_back(beyond);
    packed = rv::gl::packLights(crowded, rv::scene::EnvironmentSetup{});
    CHECK_FALSE(packed.shadowLight.casts());
    CHECK(packed.shadowNote.find("'beyond'") != std::string::npos);
}

TEST_CASE("render/light_packing: a directional key light without shadow casters casts no shadow and says why",
          "[render][lights][shadow]") {
    Light sun;
    sun.name = "sun";
    sun.type = LightType::Directional;
    sun.direction = glm::vec3(-0.4f, -0.8f, -0.45f);
    sun.castsShadows = true;
    const std::vector<Light> lights{pointLight("fill", glm::vec3(0.0f, 2.0f, 0.0f)), sun};
    const rv::core::Aabb casters = rv::core::Aabb::fromMinMax(glm::vec3(-1.0f), glm::vec3(1.0f));

    // Something casts: the sun keeps its map, fitted to the casters.
    PackedLights packed = rv::gl::packLights(lights, rv::scene::EnvironmentSetup{});
    rv::gl::requireShadowCasters(packed, lights, casters);
    REQUIRE(packed.shadowLight.casts());
    CHECK(packed.shadowLight.index == rv::gl::kDirectionalShadowLight);
    CHECK(packed.shadowNote.empty());

    // Nothing casts: there is no map to fit, and the log and the Lights panel say so.
    rv::gl::requireShadowCasters(packed, lights, rv::core::Aabb{});
    CHECK_FALSE(packed.shadowLight.casts());
    CHECK(packed.shadowNote.find("'sun'") != std::string::npos);
    CHECK(packed.shadowNote.find("directional") != std::string::npos);
    CHECK(packed.uniforms.directionalIntensity == sun.intensity); // it still lights the scene

    // A spot key light's map does not depend on the casters; without a key light nothing changes.
    Light spot = pointLight("spot", glm::vec3(0.0f, 2.9f, 0.0f));
    spot.type = LightType::Spot;
    spot.outerConeDeg = 40.0f;
    spot.castsShadows = true;
    packed = rv::gl::packLights(std::vector<Light>{spot}, rv::scene::EnvironmentSetup{});
    rv::gl::requireShadowCasters(packed, std::vector<Light>{spot}, rv::core::Aabb{});
    CHECK(packed.shadowLight.casts());
    CHECK(packed.shadowNote.empty());
    const std::vector<Light> unshadowed{pointLight("fill", glm::vec3(0.0f, 2.0f, 0.0f))};
    packed = rv::gl::packLights(unshadowed, rv::scene::EnvironmentSetup{});
    rv::gl::requireShadowCasters(packed, unshadowed, rv::core::Aabb{});
    CHECK_FALSE(packed.shadowLight.casts());
    CHECK(packed.shadowNote.empty());

    // A key light that cannot cast keeps the reason it already has.
    Light bulb = pointLight("bulb", glm::vec3(0.0f, 2.0f, 0.0f));
    bulb.castsShadows = true;
    packed = rv::gl::packLights(std::vector<Light>{bulb}, rv::scene::EnvironmentSetup{});
    const std::string pointNote = packed.shadowNote;
    rv::gl::requireShadowCasters(packed, std::vector<Light>{bulb}, rv::core::Aabb{});
    CHECK(packed.shadowNote == pointNote);
}

TEST_CASE("render/light_packing: a selection packs the lights it names, its first caster the key light",
          "[render][lights][shadow][areas]") {
    // I4 step 3: the lights of the area that holds the eye (scene::AreaLighting's selection, in the scene's order).
    Light otherKey = pointLight("other key", glm::vec3(10.0f, 2.9f, 0.0f));
    otherKey.type = LightType::Spot;
    otherKey.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    otherKey.castsShadows = true;
    Light key = otherKey;
    key.name = "key";
    key.position = glm::vec3(-10.0f, 2.9f, 0.0f);
    const std::vector<Light> lights{otherKey, pointLight("a", glm::vec3(1.0f, 2.0f, 0.0f)), key,
                                    pointLight("b", glm::vec3(2.0f, 2.0f, 0.0f))};
    const std::vector<std::uint32_t> selection{1, 2, 3};
    const PackedLights packed = rv::gl::packLights(lights, selection, rv::scene::EnvironmentSetup{});
    REQUIRE(packed.uniforms.punctualLightCount == 3);
    CHECK(packed.uniforms.punctualLights[0].position == lights[1].position);
    CHECK(packed.uniforms.punctualLights[1].position == key.position);
    CHECK(packed.uniforms.punctualLights[2].position == lights[3].position);
    REQUIRE(packed.shadowLight.casts());
    CHECK(packed.shadowLight.index == 1);
    CHECK(packed.shadowLight.position == key.position);
    CHECK(packed.keyLight == std::optional<std::size_t>(2)); // its index in the scene's lights, not in the selection
    CHECK(packed.dropped.empty());

    // The whole list in its order packs as packLights() of the list; an index past the list is ignored.
    const std::vector<std::uint32_t> all{0, 1, 2, 3, 9};
    const PackedLights whole = rv::gl::packLights(lights, all, rv::scene::EnvironmentSetup{});
    const PackedLights plain = rv::gl::packLights(lights, rv::scene::EnvironmentSetup{});
    CHECK(whole.uniforms.punctualLightCount == plain.uniforms.punctualLightCount);
    CHECK(whole.shadowLight.position == plain.shadowLight.position);
    CHECK(whole.shadowLight.index == plain.shadowLight.index);

    // A dropped light is named by its index in the scene's list.
    std::vector<Light> broken = lights;
    broken[3].name.clear();
    broken[3].intensity = -1.0f;
    const PackedLights withDropped = rv::gl::packLights(broken, selection, rv::scene::EnvironmentSetup{});
    REQUIRE(withDropped.dropped.size() == 1);
    CHECK(withDropped.dropped[0].name == "#3");
    CHECK(rv::gl::lightName(broken[3], 3) == "#3");
    CHECK(rv::gl::lightName(lights[3], 3) == "b");

    // A selection whose key light is a directional light without casters: the note names the selection's key light,
    // not the scene's first caster.
    Light westSun;
    westSun.name = "west sun";
    westSun.type = LightType::Directional;
    westSun.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    westSun.castsShadows = true;
    Light eastSun = westSun;
    eastSun.name = "east sun";
    const std::vector<Light> suns{westSun, eastSun};
    const std::vector<std::uint32_t> east{1};
    PackedLights sunlit = rv::gl::packLights(suns, east, rv::scene::EnvironmentSetup{});
    CHECK(sunlit.keyLight == std::optional<std::size_t>(1));
    rv::gl::requireShadowCasters(sunlit, suns, rv::core::Aabb{});
    CHECK_FALSE(sunlit.shadowLight.casts());
    CHECK(sunlit.shadowNote.find("'east sun'") != std::string::npos);
    CHECK(sunlit.shadowNote.find("'west sun'") == std::string::npos);
}

TEST_CASE("render/light_packing: a neighbouring area's lights are packed but never the key light",
          "[render][lights][shadow][areas]") {
    // I6 step 1: the selection holds the eye's area's own lights, then its neighbours' (scene::AreaLighting's
    // ownCount()); only the first keyCandidates may be the key light.
    Light neighbourKey = pointLight("neighbour key", glm::vec3(0.0f, 2.9f, -3.0f));
    neighbourKey.type = LightType::Spot;
    neighbourKey.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    neighbourKey.castsShadows = true;
    Light ownKey = neighbourKey;
    ownKey.name = "own key";
    ownKey.position = glm::vec3(0.0f, 2.9f, 3.0f);
    const std::vector<Light> lights{neighbourKey, pointLight("own", glm::vec3(1.0f, 2.0f, 2.0f)), ownKey};

    // The own lights (1, 2) then the neighbour's (0): the own key light casts, the neighbour's is packed unshadowed.
    const std::vector<std::uint32_t> selection{1, 2, 0};
    const PackedLights packed = rv::gl::packLights(lights, selection, rv::scene::EnvironmentSetup{}, 2);
    REQUIRE(packed.uniforms.punctualLightCount == 3);
    CHECK(packed.uniforms.punctualLights[2].position == neighbourKey.position);
    REQUIRE(packed.shadowLight.casts());
    CHECK(packed.keyLight == std::optional<std::size_t>(2));
    CHECK(packed.shadowLight.index == 1);

    // An area without a key light of its own (a hall): its neighbour's key light is not the key light either.
    const std::vector<std::uint32_t> hall{1, 0};
    const PackedLights unshadowed = rv::gl::packLights(lights, hall, rv::scene::EnvironmentSetup{}, 1);
    CHECK(unshadowed.uniforms.punctualLightCount == 2);
    CHECK_FALSE(unshadowed.shadowLight.casts());
    CHECK_FALSE(unshadowed.keyLight.has_value());
    CHECK(unshadowed.shadowNote.empty());
    // Without the limit, the first caster of the selection is the key light, as before.
    CHECK(rv::gl::packLights(lights, hall, rv::scene::EnvironmentSetup{}).keyLight == std::optional<std::size_t>(0));
}

TEST_CASE("render/light_packing: the environment's fill is packed beside the hemisphere's ambient",
          "[render][lights]") {
    rv::scene::EnvironmentSetup environment;
    environment.ambientSky = glm::vec3(0.5f);
    environment.ambientGround = glm::vec3(0.25f);
    environment.ambientIntensity = 0.3f;
    environment.fillColor = glm::vec3(1.0f, 0.5f, 0.25f);
    environment.fillIntensity = 0.2f;
    const PackedLights packed = rv::gl::packLights(std::vector<Light>{}, environment);
    CHECK(packed.uniforms.ambientIntensity == 0.3f);
    CHECK(packed.uniforms.ambientFill.r == Approx(0.2f));
    CHECK(packed.uniforms.ambientFill.g == Approx(0.1f));
    CHECK(packed.uniforms.ambientFill.b == Approx(0.05f));
    // The default environment has none, so scenes that set none look as before.
    CHECK(rv::gl::packLights(std::vector<Light>{}, rv::scene::EnvironmentSetup{}).uniforms.ambientFill ==
          glm::vec3(0.0f));

    // packAmbient() rewrites the ambient terms alone (the ease between two areas' environments).
    LightsUniforms block = packed.uniforms;
    block.punctualLightCount = 3;
    rv::scene::EnvironmentSetup dimmer = environment;
    dimmer.ambientIntensity = 0.1f;
    dimmer.fillIntensity = 0.0f;
    rv::gl::packAmbient(block, dimmer);
    CHECK(block.ambientIntensity == 0.1f);
    CHECK(block.ambientFill == glm::vec3(0.0f));
    CHECK(block.punctualLightCount == 3);
    // A bad fill never reaches the shaders.
    dimmer.fillIntensity = std::numeric_limits<float>::quiet_NaN();
    rv::gl::packAmbient(block, dimmer);
    CHECK(block.ambientFill == glm::vec3(0.0f));
}

TEST_CASE("render/light_packing: a selection's weights scale its lights, its portals clip them, a given key casts",
          "[render][lights][shadow][areas]") {
    // I6 step 1: scene::AreaLighting fades each light in and out (its weight), clips a neighbour's light to the portal
    // it shines through, and names the light whose shadow fades with the eye's area's key light.
    Light key = pointLight("key", glm::vec3(0.0f, 2.9f, -3.0f));
    key.type = LightType::Spot;
    key.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    key.castsShadows = true;
    key.shadowStrength = 0.8f;
    const std::vector<Light> lights{key, pointLight("hall", glm::vec3(0.0f, 2.6f, 2.0f)),
                                    pointLight("lamp", glm::vec3(2.0f, 1.5f, -2.0f))};
    const std::vector<std::uint32_t> indices{0, 2, 1};
    const std::vector<float> weights{1.0f, 0.25f, std::numeric_limits<float>::quiet_NaN()};
    // The doorway: x in [-0.8, 0.8], y in [0, 2.3] on the plane z = -1.5; the lamp unclipped (an empty box).
    const std::vector<rv::core::Aabb> portals{
        rv::core::Aabb::fromMinMax({-0.8f, 0.0f, -1.5f}, {0.8f, 2.3f, -1.5f}), rv::core::Aabb{},
        rv::core::Aabb::fromMinMax({-0.8f, 0.0f, -1.5f}, {0.8f, 2.3f, -1.5f})};
    rv::gl::LightSelection selection;
    selection.indices = indices;
    selection.weights = weights;
    selection.portals = portals;
    selection.keyCandidates = 2;
    const PackedLights packed = rv::gl::packLights(lights, selection, rv::scene::EnvironmentSetup{});
    REQUIRE(packed.uniforms.punctualLightCount == 3);
    const auto& block = packed.uniforms.punctualLights;
    CHECK(block[0].intensity == 4.0f);
    CHECK(block[1].intensity == 1.0f); // a quarter of the lamp's 4
    CHECK(block[2].intensity == 4.0f); // a weight that is no number counts as whole
    // The portal: its thinnest axis (z) the plane's, its extent over x and y.
    CHECK(block[0].portalAxis == 2);
    CHECK(block[0].portalPlane == -1.5f);
    CHECK(block[0].portalRect == glm::vec4(-0.8f, 0.0f, 0.8f, 2.3f));
    CHECK(block[1].portalAxis == -1);
    CHECK(block[2].portalAxis == 2);
    REQUIRE(packed.punctual.size() == 3);
    CHECK(packed.punctual[0] == rv::gl::PackedLightSource{0, 1.0f, true});
    CHECK(packed.punctual[1] == rv::gl::PackedLightSource{2, 0.25f, false});
    CHECK(packed.punctual[2] == rv::gl::PackedLightSource{1, 1.0f, true});
    // The key light among the candidates casts at its strength.
    REQUIRE(packed.shadowLight.casts());
    CHECK(packed.shadowLight.strength == Approx(0.8f));

    // A given key light casts whatever the candidates (the key light the eye left, its shadow fading out): its
    // strength scaled by the shadow's weight.
    selection.keyCandidates = 0;
    selection.key = 0;
    selection.keyShadowWeight = 0.5f;
    const PackedLights fading = rv::gl::packLights(lights, selection, rv::scene::EnvironmentSetup{});
    REQUIRE(fading.shadowLight.casts());
    CHECK(fading.keyLight == std::optional<std::size_t>(0));
    CHECK(fading.shadowLight.strength == Approx(0.4f));
    // None given and no candidates: no key light.
    selection.key.reset();
    CHECK_FALSE(rv::gl::packLights(lights, selection, rv::scene::EnvironmentSetup{}).shadowLight.casts());
    // A portal across x reads its rectangle over y and z.
    const std::vector<rv::core::Aabb> side{rv::core::Aabb::fromMinMax({4.0f, 0.0f, -1.0f}, {4.0f, 2.0f, 1.0f})};
    const std::vector<std::uint32_t> one{1};
    rv::gl::LightSelection across;
    across.indices = one;
    across.portals = side;
    const PackedLights x = rv::gl::packLights(lights, across, rv::scene::EnvironmentSetup{});
    CHECK(x.uniforms.punctualLights[0].portalAxis == 0);
    CHECK(x.uniforms.punctualLights[0].portalPlane == 4.0f);
    CHECK(x.uniforms.punctualLights[0].portalRect == glm::vec4(0.0f, -1.0f, 2.0f, 1.0f));
}
