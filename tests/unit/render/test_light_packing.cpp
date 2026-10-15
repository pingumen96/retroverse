#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

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
    environment.exposure = 1.4f;
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

    // No directional light; the environment is copied as is.
    CHECK(block.directionalIntensity == 0.0f);
    CHECK(block.ambientSky == environment.ambientSky);
    CHECK(block.ambientGround == environment.ambientGround);
    CHECK(block.ambientIntensity == 0.08f);
    CHECK(block.exposure == 1.4f);
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

TEST_CASE("render/light_packing: one directional light, at most eight point and spot lights", "[render][lights]") {
    std::vector<Light> lights;
    Light sun;
    sun.name = "sun";
    sun.type = LightType::Directional;
    sun.direction = glm::vec3(-0.4f, -0.8f, -0.45f);
    sun.color = glm::vec3(1.0f, 0.95f, 0.9f);
    sun.intensity = 3.0f;
    lights.push_back(sun);
    for (int i = 0; i < 10; ++i) {
        lights.push_back(
            pointLight(i == 9 ? "" : "bulb" + std::to_string(i), glm::vec3(static_cast<float>(i), 2.0f, 0.0f)));
    }
    Light moon = sun;
    moon.name = "moon";
    lights.push_back(moon);

    const PackedLights packed = rv::gl::packLights(lights, rv::scene::EnvironmentSetup{});
    const LightsUniforms& block = packed.uniforms;
    CHECK(block.punctualLightCount == static_cast<std::int32_t>(rv::gl::kMaxPunctualLights));
    CHECK(block.punctualLights[7].position.x == 7.0f); // list order
    CHECK(block.directionalIntensity == 3.0f);
    CHECK(block.directionalColor == sun.color);
    CHECK(glm::length(block.directionalDirection) == Approx(1.0f));
    CHECK(rv::core::nearlyEqual(block.directionalDirection, glm::normalize(sun.direction)));

    // The ninth and tenth bulbs (the tenth is unnamed: index 10 in the list) and the second directional light.
    REQUIRE(packed.dropped.size() == 3);
    CHECK(packed.dropped[0].name == "bulb8");
    CHECK(packed.dropped[1].name == "#10");
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
