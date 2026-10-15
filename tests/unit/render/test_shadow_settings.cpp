#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <optional>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/math.hpp>
#include <rv/render/shadow_settings.hpp>

using Catch::Approx;
using rv::gl::ShadowLight;
using rv::gl::ShadowProjection;
using rv::gl::ShadowSettings;

namespace {

/// A spot light at `position` along `direction` with an outer cone of `outerConeDeg`, packed at index 2.
ShadowLight spotLight(const glm::vec3& position, const glm::vec3& direction, float outerConeDeg, float range) {
    ShadowLight light;
    light.index = 2;
    light.type = rv::scene::LightType::Spot;
    light.position = position;
    light.direction = glm::normalize(direction);
    light.outerConeDeg = outerConeDeg;
    light.range = range;
    return light;
}

/// Normalized device coordinates of `point` in a shadow map.
glm::vec3 ndcOf(const ShadowProjection& shadow, const glm::vec3& point) {
    const glm::vec4 clip = shadow.viewProjection * glm::vec4(point, 1.0f);
    return glm::vec3(clip) / clip.w;
}

/// Point `distance` from `origin` along `axis` turned by `angleDeg` about the unit `pivot` (perpendicular to it).
glm::vec3 offAxis(const glm::vec3& origin, const glm::vec3& axis, const glm::vec3& pivot, float angleDeg,
                  float distance) {
    const float angle = rv::core::degreesToRadians(angleDeg);
    return origin + (axis * std::cos(angle) + pivot * std::sin(angle)) * distance;
}

bool insideClip(const glm::vec3& ndc) {
    return std::abs(ndc.x) <= 1.0f && std::abs(ndc.y) <= 1.0f && std::abs(ndc.z) <= 1.0f;
}

} // namespace

TEST_CASE("render/shadow_settings: a spot light's map looks along its axis over twice its cone", "[render][shadow]") {
    const glm::vec3 position(0.2f, 2.9f, -2.0f);
    const glm::vec3 axis = glm::normalize(glm::vec3(0.0f, -1.0f, -0.6f));
    const ShadowLight light = spotLight(position, axis, 50.0f, 10.0f);
    const std::optional<ShadowProjection> shadow = rv::gl::spotShadowProjection(light, rv::gl::kShadowMapResolution);
    REQUIRE(shadow.has_value());
    CHECK(shadow->perspective);
    CHECK(shadow->eye == position);
    CHECK(rv::core::isFinite(shadow->viewProjection));

    // The axis maps to the center of the map, at any distance within the range.
    for (const float distance : {0.5f, 3.0f, 9.0f}) {
        const glm::vec3 center = ndcOf(*shadow, position + axis * distance);
        CHECK(center.x == Approx(0.0f).margin(1e-4));
        CHECK(center.y == Approx(0.0f).margin(1e-4));
        CHECK(std::abs(center.z) < 1.0f);
    }
    // The cone's edge lies on the edges of the map (the field of view is twice the outer cone): just inside it is in
    // the map, just outside it is not, whichever way the cone is crossed.
    const glm::vec3 side = glm::normalize(glm::cross(axis, glm::vec3(1.0f, 0.0f, 0.0f)));
    for (const glm::vec3& pivot : {glm::vec3(1.0f, 0.0f, 0.0f), side}) {
        CHECK(insideClip(ndcOf(*shadow, offAxis(position, axis, pivot, 49.0f, 3.0f))));
        CHECK_FALSE(insideClip(ndcOf(*shadow, offAxis(position, axis, pivot, 51.0f, 3.0f))));
    }
    // The far plane is the range: nothing beyond the light's reach is in the map.
    CHECK(ndcOf(*shadow, position + axis * 9.9f).z < 1.0f);
    CHECK(ndcOf(*shadow, position + axis * 10.1f).z > 1.0f);
    CHECK(ndcOf(*shadow, position + axis * (rv::gl::kSpotShadowNearPlane * 0.5f)).z < -1.0f);
    // One texel spans 2 tan(fov / 2) / resolution meters per meter of distance.
    const float tanHalfFov = std::tan(rv::core::degreesToRadians(50.0f));
    CHECK(shadow->texelWorldSize == Approx(2.0f * tanHalfFov / static_cast<float>(rv::gl::kShadowMapResolution)));
}

TEST_CASE("render/shadow_settings: vertical spot lights and extreme cones still give a usable map",
          "[render][shadow]") {
    // Straight down: the up vector of the light space cannot be +Y.
    const ShadowLight down = spotLight(glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 40.0f, 8.0f);
    const std::optional<ShadowProjection> below = rv::gl::spotShadowProjection(down, 1024);
    REQUIRE(below.has_value());
    CHECK(rv::core::isFinite(below->viewProjection));
    CHECK(insideClip(ndcOf(*below, glm::vec3(0.5f, 0.0f, -0.5f))));
    const ShadowLight up = spotLight(glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 40.0f, 8.0f);
    REQUIRE(rv::gl::spotShadowProjection(up, 1024).has_value());

    // A cone wider than the widest map is shadowed within kMaxSpotShadowFovDeg.
    const ShadowLight wide = spotLight(glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 85.0f, 8.0f);
    const std::optional<ShadowProjection> clamped = rv::gl::spotShadowProjection(wide, 1024);
    REQUIRE(clamped.has_value());
    const float halfFov = 0.5f * rv::gl::kMaxSpotShadowFovDeg;
    const glm::vec3 axis(0.0f, -1.0f, 0.0f);
    const glm::vec3 pivot(1.0f, 0.0f, 0.0f);
    CHECK(insideClip(ndcOf(*clamped, offAxis(down.position, axis, pivot, halfFov - 1.0f, 2.0f))));
    CHECK_FALSE(insideClip(ndcOf(*clamped, offAxis(down.position, axis, pivot, halfFov + 1.0f, 2.0f))));
    // A pencil beam gets the narrowest map, not a degenerate one.
    const ShadowLight pencil = spotLight(glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 0.0f, 8.0f);
    const std::optional<ShadowProjection> narrow = rv::gl::spotShadowProjection(pencil, 1024);
    REQUIRE(narrow.has_value());
    CHECK(rv::core::isFinite(narrow->viewProjection));
    // A light reaching less than half a meter keeps its near plane in front of its far one.
    const ShadowLight shortReach = spotLight(glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 30.0f, 0.2f);
    const std::optional<ShadowProjection> close = rv::gl::spotShadowProjection(shortReach, 1024);
    REQUIRE(close.has_value());
    CHECK(std::abs(ndcOf(*close, glm::vec3(0.0f, 2.9f, 0.0f)).z) < 1.0f);
}

TEST_CASE("render/shadow_settings: unusable spot lights give no map", "[render][shadow]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const ShadowLight good = spotLight(glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 40.0f, 8.0f);
    REQUIRE(rv::gl::spotShadowProjection(good, 1024).has_value());
    CHECK_FALSE(rv::gl::spotShadowProjection(good, 0).has_value());

    ShadowLight light = good;
    light.direction = glm::vec3(0.0f);
    CHECK_FALSE(rv::gl::spotShadowProjection(light, 1024).has_value());
    light = good;
    light.position.x = nan;
    CHECK_FALSE(rv::gl::spotShadowProjection(light, 1024).has_value());
    light = good;
    light.range = 0.0f;
    CHECK_FALSE(rv::gl::spotShadowProjection(light, 1024).has_value());
    light = good;
    light.outerConeDeg = nan;
    CHECK_FALSE(rv::gl::spotShadowProjection(light, 1024).has_value());
    light = good;
    light.type = rv::scene::LightType::Directional;
    CHECK_FALSE(rv::gl::spotShadowProjection(light, 1024).has_value());
}

TEST_CASE("render/shadow_settings: a directional light's map is fitted to the shadow casters", "[render][shadow]") {
    const rv::core::Aabb casters =
        rv::core::Aabb::fromMinMax(glm::vec3(-3.0f, 0.0f, -3.0f), glm::vec3(3.0f, 4.6f, 3.0f));
    const glm::vec3 direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.45f));
    const std::optional<ShadowProjection> shadow =
        rv::gl::directionalShadowProjection(direction, casters, rv::gl::kShadowMapResolution);
    REQUIRE(shadow.has_value());
    CHECK_FALSE(shadow->perspective);
    // Every corner of the casters' bounds is in the map, clear of its edges by the margin (minus one snapped texel).
    const float margin = (rv::gl::kDirectionalShadowMarginTexels - 1.0f) * 2.0f /
                         static_cast<float>(rv::gl::kShadowMapResolution);
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point((corner & 1) != 0 ? casters.max.x : casters.min.x,
                              (corner & 2) != 0 ? casters.max.y : casters.min.y,
                              (corner & 4) != 0 ? casters.max.z : casters.min.z);
        const glm::vec3 ndc = ndcOf(*shadow, point);
        INFO("corner " << corner);
        CHECK(std::abs(ndc.x) <= 1.0f - margin);
        CHECK(std::abs(ndc.y) <= 1.0f - margin);
        CHECK(std::abs(ndc.z) < 1.0f);
    }
    // The light travels along -Z of its space: points further along the direction are deeper in the map.
    const glm::vec3 center = casters.center();
    CHECK(ndcOf(*shadow, center + direction).z > ndcOf(*shadow, center).z);
    // The map is square and no larger than the bounds need: the texel covers the larger side of their projection.
    const float diagonal = glm::length(casters.size());
    CHECK(shadow->texelWorldSize * static_cast<float>(rv::gl::kShadowMapResolution) <= diagonal * 1.01f);
    CHECK(shadow->texelWorldSize > 0.0f);
}

TEST_CASE("render/shadow_settings: a directional map slides by whole texels when its casters move",
          "[render][shadow]") {
    // Moving bounds (objects that move, a light fitted to a part of the world) must not slide the texel grid by a
    // fraction of a texel: the shadow edges would crawl from frame to frame (shimmering).
    const int resolution = 2048;
    const glm::vec3 direction = glm::normalize(glm::vec3(0.3f, -1.0f, 0.2f));
    const rv::core::Aabb casters =
        rv::core::Aabb::fromMinMax(glm::vec3(-4.0f, 0.0f, -5.0f), glm::vec3(4.0f, 2.5f, 5.0f));
    const std::optional<ShadowProjection> first = rv::gl::directionalShadowProjection(direction, casters, resolution);
    REQUIRE(first.has_value());
    const glm::vec3 probe(0.37f, 0.81f, -1.23f);
    for (const glm::vec3& shift : {glm::vec3(0.0013f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.0071f),
                                   glm::vec3(0.21f, 0.0f, -0.17f), glm::vec3(-1.0f, 0.0f, 0.5f)}) {
        INFO("shift " << shift.x << ", " << shift.y << ", " << shift.z);
        const rv::core::Aabb moved = rv::core::Aabb::fromMinMax(casters.min + shift, casters.max + shift);
        const std::optional<ShadowProjection> second =
            rv::gl::directionalShadowProjection(direction, moved, resolution);
        REQUIRE(second.has_value());
        CHECK(second->texelWorldSize == Approx(first->texelWorldSize).epsilon(1e-5));
        // The same world point lands on the same place within its texel in both maps.
        const glm::vec2 a = (glm::vec2(ndcOf(*first, probe)) * 0.5f + 0.5f) * static_cast<float>(resolution);
        const glm::vec2 b = (glm::vec2(ndcOf(*second, probe)) * 0.5f + 0.5f) * static_cast<float>(resolution);
        const glm::vec2 slide = b - a;
        CHECK(std::abs(slide.x - std::round(slide.x)) < 0.01f);
        CHECK(std::abs(slide.y - std::round(slide.y)) < 0.01f);
    }
}

TEST_CASE("render/shadow_settings: unusable directional fits and lights that cast no shadow give no map",
          "[render][shadow]") {
    const rv::core::Aabb casters = rv::core::Aabb::fromMinMax(glm::vec3(-1.0f), glm::vec3(1.0f));
    CHECK_FALSE(rv::gl::directionalShadowProjection(glm::vec3(0.0f), casters, 1024).has_value());
    CHECK_FALSE(rv::gl::directionalShadowProjection(glm::vec3(0.0f, -1.0f, 0.0f), rv::core::Aabb{}, 1024).has_value());
    CHECK_FALSE(rv::gl::directionalShadowProjection(glm::vec3(0.0f, -1.0f, 0.0f), casters, 0).has_value());
    // Straight down works (the light space's up vector is -Z then), and so do bounds without volume.
    REQUIRE(rv::gl::directionalShadowProjection(glm::vec3(0.0f, -1.0f, 0.0f), casters, 1024).has_value());
    const rv::core::Aabb flat = rv::core::Aabb::fromMinMax(glm::vec3(-1.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 1.0f));
    const std::optional<ShadowProjection> floor =
        rv::gl::directionalShadowProjection(glm::vec3(0.0f, -1.0f, 0.0f), flat, 1024);
    REQUIRE(floor.has_value());
    CHECK(std::abs(ndcOf(*floor, glm::vec3(0.5f, 0.0f, 0.5f)).z) < 1.0f);

    // shadowProjectionFor picks the projection of the light's type; no light, or a point light, gives none.
    ShadowLight none;
    CHECK_FALSE(none.casts());
    CHECK_FALSE(rv::gl::shadowProjectionFor(none, casters, 1024).has_value());
    ShadowLight point = spotLight(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 30.0f, 5.0f);
    point.type = rv::scene::LightType::Point;
    CHECK_FALSE(rv::gl::shadowProjectionFor(point, casters, 1024).has_value());
    const ShadowLight spot = spotLight(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 30.0f, 5.0f);
    const std::optional<ShadowProjection> perspective = rv::gl::shadowProjectionFor(spot, casters, 1024);
    REQUIRE(perspective.has_value());
    CHECK(perspective->perspective);
    ShadowLight sun = spot;
    sun.index = rv::gl::kDirectionalShadowLight;
    sun.type = rv::scene::LightType::Directional;
    const std::optional<ShadowProjection> orthographic = rv::gl::shadowProjectionFor(sun, casters, 1024);
    REQUIRE(orthographic.has_value());
    CHECK_FALSE(orthographic->perspective);
}

TEST_CASE("render/shadow_settings: the shadow block carries the map, the light and the normal offset",
          "[render][shadow]") {
    const ShadowLight light = spotLight(glm::vec3(-0.2f, 2.92f, -2.2f), glm::vec3(0.0f, -2.92f, -2.4f), 50.0f, 12.0f);
    const std::optional<ShadowProjection> shadow = rv::gl::spotShadowProjection(light, rv::gl::kShadowMapResolution);
    REQUIRE(shadow.has_value());
    ShadowSettings settings;
    rv::gl::ShadowUniforms uniforms =
        rv::gl::makeShadowUniforms(*shadow, light, settings, rv::gl::kShadowMapResolution);
    CHECK(uniforms.viewProjection == shadow->viewProjection);
    CHECK(uniforms.lightPosition == light.position);
    CHECK(uniforms.lightIndex == 2);
    CHECK(uniforms.strength == 1.0f);
    CHECK(uniforms.texelSize == Approx(1.0f / 2048.0f));
    // A perspective map: the offset grows with the distance from the light, one and a half texels by default.
    CHECK(uniforms.normalOffset.x == 0.0f);
    CHECK(uniforms.normalOffset.y == Approx(settings.normalOffsetTexels * shadow->texelWorldSize));
    // At room scale (the test room's key light reaches the hero wall 3 to 4 m away) the normal offset and the texels
    // stay at a few millimeters: no acne without visibly detached shadows.
    CHECK(uniforms.normalOffset.y * 4.0f < 0.01f);
    CHECK(shadow->texelWorldSize * 4.0f < 0.005f);

    // An orthographic map: a constant offset.
    const rv::core::Aabb casters = rv::core::Aabb::fromMinMax(glm::vec3(-2.0f), glm::vec3(2.0f));
    const std::optional<ShadowProjection> sun =
        rv::gl::directionalShadowProjection(glm::vec3(0.0f, -1.0f, -1.0f), casters, 1024);
    REQUIRE(sun.has_value());
    ShadowLight directional = light;
    directional.index = rv::gl::kDirectionalShadowLight;
    uniforms = rv::gl::makeShadowUniforms(*sun, directional, settings, 1024);
    CHECK(uniforms.lightIndex == rv::gl::kDirectionalShadowLight);
    CHECK(uniforms.normalOffset.x == Approx(settings.normalOffsetTexels * sun->texelWorldSize));
    CHECK(uniforms.normalOffset.y == 0.0f);

    // Values the shader cannot use never reach it.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    ShadowLight odd = light;
    odd.strength = 2.0f;
    CHECK(rv::gl::makeShadowUniforms(*shadow, odd, settings, 2048).strength == 1.0f);
    odd.strength = -1.0f;
    CHECK(rv::gl::makeShadowUniforms(*shadow, odd, settings, 2048).strength == 0.0f);
    odd.strength = nan;
    CHECK(rv::gl::makeShadowUniforms(*shadow, odd, settings, 2048).strength == 1.0f);
    settings.normalOffsetTexels = nan;
    CHECK(rv::gl::makeShadowUniforms(*shadow, light, settings, 2048).normalOffset == glm::vec2(0.0f));
    settings.normalOffsetTexels = 1000.0f;
    CHECK(rv::gl::makeShadowUniforms(*shadow, light, settings, 2048).normalOffset.y ==
          Approx(rv::gl::kMaxShadowNormalOffsetTexels * shadow->texelWorldSize));
}

TEST_CASE("render/shadow_settings: the depth bias of the shadow pass stays in range", "[render][shadow]") {
    ShadowSettings settings;
    CHECK(settings.enabled);
    glm::vec2 bias = rv::gl::shadowDepthBias(settings);
    CHECK(bias.x == settings.slopeBias);
    CHECK(bias.y == settings.constantBias);
    CHECK(bias.x > 0.0f);
    settings.slopeBias = -1.0f;
    settings.constantBias = 100.0f;
    bias = rv::gl::shadowDepthBias(settings);
    CHECK(bias.x == 0.0f);
    CHECK(bias.y == rv::gl::kMaxShadowDepthBias);
    settings.slopeBias = std::numeric_limits<float>::infinity();
    settings.constantBias = std::numeric_limits<float>::quiet_NaN();
    bias = rv::gl::shadowDepthBias(settings);
    CHECK(bias == glm::vec2(0.0f));
}
