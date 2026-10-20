#include <rv/render/light_packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

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

/// The key light `light`, packed at `index` (a punctual light index or kDirectionalShadowLight) with the unit
/// `direction`.
ShadowLight keyLightOf(const scene::Light& light, std::int32_t index, const glm::vec3& direction) {
    ShadowLight key;
    key.index = index;
    key.type = light.type;
    key.position = light.position;
    key.direction = direction;
    key.outerConeDeg = light.outerConeDeg;
    key.range = light.range;
    key.strength = std::isfinite(light.shadowStrength) ? std::clamp(light.shadowStrength, 0.0f, 1.0f) : 1.0f;
    return key;
}

} // namespace

glm::vec2 spotConeScaleOffset(float innerConeDeg, float outerConeDeg) noexcept {
    const float cosOuter = std::cos(core::degreesToRadians(outerConeDeg));
    const float cosInner = std::max(std::cos(core::degreesToRadians(innerConeDeg)), cosOuter);
    const float scale = 1.0f / std::max(cosInner - cosOuter, 1e-4f);
    return {scale, -cosOuter * scale};
}

namespace {

/// packLights() over the lights at `indexOf(0)`, ..., `indexOf(count - 1)` of `lights`.
template <class IndexOf>
PackedLights packIndexed(std::span<const scene::Light> lights, std::size_t count, IndexOf indexOf,
                         const scene::EnvironmentSetup& environment) {
    PackedLights packed;
    LightsUniforms& block = packed.uniforms;
    bool directionalUsed = false;
    bool keyLightSeen = false;
    // Drops `light` for `reason`; a dropped key light casts no shadow.
    const auto drop = [&packed](const scene::Light& light, std::size_t index, std::string reason, bool key) {
        const std::string name = lightName(light, index);
        if (key) {
            packed.shadowNote = fmt::format("light '{}' casts shadows but is not drawn ({})", name, reason);
        }
        packed.dropped.push_back({name, std::move(reason)});
    };
    for (std::size_t position = 0; position < count; ++position) {
        const std::size_t index = indexOf(position);
        if (index >= lights.size()) {
            continue;
        }
        const scene::Light& light = lights[index];
        // The first light that asks for shadows is the key light, whether or not it can have them.
        const bool key = light.castsShadows && !keyLightSeen;
        keyLightSeen = keyLightSeen || light.castsShadows;
        if (const auto reason = rejection(light)) {
            drop(light, index, *reason, key);
            continue;
        }
        if (light.type == scene::LightType::Directional) {
            if (directionalUsed) {
                drop(light, index, "only one directional light is supported", key);
                continue;
            }
            directionalUsed = true;
            block.directionalDirection = *unitDirection(light.direction);
            block.directionalColor = light.color;
            block.directionalIntensity = light.intensity;
            if (key) {
                packed.shadowLight = keyLightOf(light, kDirectionalShadowLight, block.directionalDirection);
            }
            continue;
        }
        if (block.punctualLightCount >= static_cast<std::int32_t>(kMaxPunctualLights)) {
            drop(light, index, fmt::format("more than {} point and spot lights", kMaxPunctualLights), key);
            continue;
        }
        if (key && light.type == scene::LightType::Point) {
            packed.shadowNote = fmt::format(
                "light '{}' casts shadows but is a point light: point-light shadows are not supported, make the key "
                "light a spot",
                lightName(light, index));
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
            if (key) {
                packed.shadowLight = keyLightOf(light, block.punctualLightCount - 1, entry.direction);
                packed.shadowLight.range = entry.range;
            }
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

} // namespace

PackedLights packLights(std::span<const scene::Light> lights, const scene::EnvironmentSetup& environment) {
    return packIndexed(
        lights, lights.size(), [](std::size_t position) { return position; }, environment);
}

PackedLights packLights(std::span<const scene::Light> lights, std::span<const std::uint32_t> selection,
                        const scene::EnvironmentSetup& environment) {
    return packIndexed(
        lights, selection.size(), [selection](std::size_t position) { return static_cast<std::size_t>(selection[position]); },
        environment);
}

void requireShadowCasters(PackedLights& packed, std::span<const scene::Light> lights, const core::Aabb& casterBounds) {
    if (!packed.shadowLight.casts() || packed.shadowLight.type != scene::LightType::Directional ||
        !casterBounds.isEmpty()) {
        return;
    }
    const auto key =
        std::find_if(lights.begin(), lights.end(), [](const scene::Light& light) { return light.castsShadows; });
    const std::string name =
        key != lights.end() ? lightName(*key, static_cast<std::size_t>(key - lights.begin())) : std::string("?");
    packed.shadowNote = fmt::format("directional light '{}' casts shadows but no object does: its map is fitted to "
                                    "the objects that cast shadows",
                                    name);
    packed.shadowLight = ShadowLight{};
}

} // namespace rv::gl
