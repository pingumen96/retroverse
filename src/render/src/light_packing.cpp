#include <rv/render/light_packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>

#include <fmt/format.h>

#include <rv/core/math.hpp>

namespace rv::gl {
namespace {

/// Shortest range a light may have; avoids a division by zero in the window.
constexpr float kMinRange = 0.001f;

std::string lightName(const scene::Light& light, std::size_t index) {
    return light.name.empty() ? fmt::format("#{}", index) : light.name;
}

bool isFiniteNonNegative(float value) noexcept {
    return std::isfinite(value) && value >= 0.0f;
}

/// Unit direction, or nothing for a zero or non-finite vector.
std::optional<glm::vec3> unitDirection(const glm::vec3& direction) noexcept {
    if (!core::isFinite(direction)) {
        return std::nullopt;
    }
    const float length = glm::length(direction);
    if (!(length > 1e-6f)) {
        return std::nullopt;
    }
    return direction / length;
}

/// Why `light` cannot be packed, or nothing when it can.
std::optional<std::string> rejection(const scene::Light& light) {
    const bool colorOk = core::isFinite(light.color) && light.color.r >= 0.0f && light.color.g >= 0.0f &&
                         light.color.b >= 0.0f;
    if (!colorOk || !isFiniteNonNegative(light.intensity)) {
        return std::string("non-finite or negative color or intensity");
    }
    if (light.type == scene::LightType::Directional) {
        if (!unitDirection(light.direction)) {
            return std::string("no usable direction");
        }
        return std::nullopt;
    }
    if (!core::isFinite(light.position) || !isFiniteNonNegative(light.range)) {
        return std::string("non-finite position or range");
    }
    if (light.type == scene::LightType::Spot &&
        (!unitDirection(light.direction) || !isFiniteNonNegative(light.innerConeDeg) ||
         !isFiniteNonNegative(light.outerConeDeg))) {
        return std::string("no usable direction or cone");
    }
    return std::nullopt;
}

} // namespace

glm::vec2 spotConeScaleOffset(float innerConeDeg, float outerConeDeg) noexcept {
    const float cosOuter = std::cos(core::degreesToRadians(outerConeDeg));
    const float cosInner = std::max(std::cos(core::degreesToRadians(innerConeDeg)), cosOuter);
    const float scale = 1.0f / std::max(cosInner - cosOuter, 1e-4f);
    return {scale, -cosOuter * scale};
}

PackedLights packLights(std::span<const scene::Light> lights, const scene::EnvironmentSetup& environment) {
    PackedLights packed;
    LightsUniforms& block = packed.uniforms;
    bool directionalUsed = false;
    for (std::size_t index = 0; index < lights.size(); ++index) {
        const scene::Light& light = lights[index];
        if (const auto reason = rejection(light)) {
            packed.dropped.push_back({lightName(light, index), *reason});
            continue;
        }
        if (light.type == scene::LightType::Directional) {
            if (directionalUsed) {
                packed.dropped.push_back({lightName(light, index), "only one directional light is supported"});
                continue;
            }
            directionalUsed = true;
            block.directionalDirection = *unitDirection(light.direction);
            block.directionalColor = light.color;
            block.directionalIntensity = light.intensity;
            continue;
        }
        if (block.punctualLightCount >= static_cast<std::int32_t>(kMaxPunctualLights)) {
            packed.dropped.push_back(
                {lightName(light, index), fmt::format("more than {} point and spot lights", kMaxPunctualLights)});
            continue;
        }
        PunctualLightUniforms& entry = block.punctualLights[block.punctualLightCount++];
        entry.position = light.position;
        entry.range = std::max(light.range, kMinRange);
        entry.color = light.color;
        entry.intensity = light.intensity;
        if (light.type == scene::LightType::Spot) {
            entry.direction = *unitDirection(light.direction);
            const glm::vec2 cone = spotConeScaleOffset(light.innerConeDeg, light.outerConeDeg);
            entry.spotScale = cone.x;
            entry.spotOffset = cone.y;
        } else {
            entry.direction = glm::vec3(0.0f, -1.0f, 0.0f); // unused: no cone
            entry.spotScale = 0.0f;
            entry.spotOffset = 1.0f;
        }
    }
    block.ambientSky = environment.ambientSky;
    block.ambientGround = environment.ambientGround;
    block.ambientIntensity = environment.ambientIntensity;
    return packed;
}

} // namespace rv::gl
