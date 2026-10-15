#include <rv/render/shadow_settings.hpp>

#include <algorithm>
#include <cmath>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <rv/core/math.hpp>

namespace rv::gl {
namespace {

/// Directions closer to +-Y than this (|y| of the unit vector) take -Z as the up vector of their light space.
constexpr float kVerticalDirection = 0.99f;

/// Unit vector of `direction`, or nothing for a zero or non-finite one.
std::optional<glm::vec3> unitDirection(const glm::vec3& direction) noexcept {
    if (!core::isFinite(direction)) {
        return std::nullopt;
    }
    const float length = glm::length(direction);
    if (!(length > 1e-6f) || !std::isfinite(length)) {
        return std::nullopt;
    }
    return direction / length;
}

/// Up vector of a light space looking along the unit `forward`: world +Y, or -Z for (nearly) vertical directions.
glm::vec3 upFor(const glm::vec3& forward) noexcept {
    return std::abs(forward.y) < kVerticalDirection ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, -1.0f);
}

/// `value` clamped to [0, maximum]; non-finite values become `fallback`.
float clampedOr(float value, float maximum, float fallback) noexcept {
    return std::isfinite(value) ? std::clamp(value, 0.0f, maximum) : fallback;
}

} // namespace

std::optional<ShadowProjection> spotShadowProjection(const ShadowLight& light, int resolution) {
    const std::optional<glm::vec3> forward = unitDirection(light.direction);
    if (light.type != scene::LightType::Spot || !forward || !core::isFinite(light.position) || resolution <= 0 ||
        !std::isfinite(light.range) || !(light.range > 0.0f) || !std::isfinite(light.outerConeDeg)) {
        return std::nullopt;
    }
    const float fovDeg = std::clamp(2.0f * light.outerConeDeg, kMinSpotShadowFovDeg, kMaxSpotShadowFovDeg);
    const float fov = core::degreesToRadians(fovDeg);
    const float nearPlane = std::min(kSpotShadowNearPlane, 0.1f * light.range);

    ShadowProjection shadow;
    shadow.perspective = true;
    shadow.eye = light.position;
    shadow.view = glm::lookAtRH(light.position, light.position + *forward, upFor(*forward));
    shadow.projection = glm::perspectiveRH_NO(fov, 1.0f, nearPlane, light.range);
    shadow.viewProjection = shadow.projection * shadow.view;
    shadow.texelWorldSize = 2.0f * std::tan(0.5f * fov) / static_cast<float>(resolution);
    return shadow;
}

std::optional<ShadowProjection> directionalShadowProjection(const glm::vec3& direction, const core::Aabb& casterBounds,
                                                            int resolution) {
    const std::optional<glm::vec3> forward = unitDirection(direction);
    if (!forward || resolution <= 0 || casterBounds.isEmpty() || !core::isFinite(casterBounds.min) ||
        !core::isFinite(casterBounds.max)) {
        return std::nullopt;
    }
    // Light space: a rotation of the world only, so that the snapped texel grid below is fixed in the world.
    const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f), *forward, upFor(*forward));
    core::Aabb lightBounds;
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point((corner & 1) != 0 ? casterBounds.max.x : casterBounds.min.x,
                              (corner & 2) != 0 ? casterBounds.max.y : casterBounds.min.y,
                              (corner & 4) != 0 ? casterBounds.max.z : casterBounds.min.z);
        lightBounds.expand(glm::vec3(view * glm::vec4(point, 1.0f)));
    }
    const glm::vec3 size = lightBounds.size();
    const float texels = static_cast<float>(resolution);
    // A square map over the larger side of the bounds, leaving the margin on both sides of it (at least a millimeter
    // per texel for point-like bounds).
    const float covered = std::max({size.x, size.y, 1e-3f * texels});
    const float texel = covered / std::max(texels - 2.0f * kDirectionalShadowMarginTexels, 1.0f);
    const float extent = texel * texels;
    const glm::vec3 center = lightBounds.center();
    // Edges on whole texels of the light space: bounds that move slide the grid by whole texels.
    const float left = std::floor((center.x - 0.5f * extent) / texel) * texel;
    const float bottom = std::floor((center.y - 0.5f * extent) / texel) * texel;
    // The light looks along -Z of its space: the bounds lie between -max.z (nearest) and -min.z (farthest), with a
    // margin against clipping by rounding.
    const float depthMargin = std::max(0.01f, 0.01f * size.z);
    const float nearPlane = -lightBounds.max.z - depthMargin;
    const float farPlane = -lightBounds.min.z + depthMargin;

    ShadowProjection shadow;
    shadow.perspective = false;
    shadow.eye = glm::vec3(0.0f);
    shadow.view = view;
    shadow.projection = glm::orthoRH_NO(left, left + extent, bottom, bottom + extent, nearPlane, farPlane);
    shadow.viewProjection = shadow.projection * shadow.view;
    shadow.texelWorldSize = texel;
    return shadow;
}

std::optional<ShadowProjection> shadowProjectionFor(const ShadowLight& light, const core::Aabb& casterBounds,
                                                    int resolution) {
    if (!light.casts()) {
        return std::nullopt;
    }
    switch (light.type) {
    case scene::LightType::Spot:
        return spotShadowProjection(light, resolution);
    case scene::LightType::Directional:
        return directionalShadowProjection(light.direction, casterBounds, resolution);
    case scene::LightType::Point:
        break;
    }
    return std::nullopt;
}

ShadowUniforms makeShadowUniforms(const ShadowProjection& projection, const ShadowLight& light,
                                  const ShadowSettings& settings, int resolution) noexcept {
    ShadowUniforms uniforms;
    uniforms.viewProjection = projection.viewProjection;
    uniforms.lightPosition = projection.eye;
    uniforms.lightIndex = light.index;
    uniforms.strength = clampedOr(light.strength, 1.0f, 1.0f);
    uniforms.texelSize = resolution > 0 ? 1.0f / static_cast<float>(resolution) : 0.0f;
    const float offset =
        clampedOr(settings.normalOffsetTexels, kMaxShadowNormalOffsetTexels, 0.0f) * projection.texelWorldSize;
    uniforms.normalOffset = projection.perspective ? glm::vec2(0.0f, offset) : glm::vec2(offset, 0.0f);
    return uniforms;
}

glm::vec2 shadowDepthBias(const ShadowSettings& settings) noexcept {
    return {clampedOr(settings.slopeBias, kMaxShadowDepthBias, 0.0f),
            clampedOr(settings.constantBias, kMaxShadowDepthBias, 0.0f)};
}

} // namespace rv::gl
