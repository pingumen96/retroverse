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

std::string lightName(const scene::Light& light, std::size_t index) {
    return light.name.empty() ? fmt::format("#{}", index) : light.name;
}

glm::vec2 spotConeScaleOffset(float innerConeDeg, float outerConeDeg) noexcept {
    const float cosOuter = std::cos(core::degreesToRadians(outerConeDeg));
    const float cosInner = std::max(std::cos(core::degreesToRadians(innerConeDeg)), cosOuter);
    const float scale = 1.0f / std::max(cosInner - cosOuter, 1e-4f);
    return {scale, -cosOuter * scale};
}

namespace {

/// The portal `box` as the shaders take it (PunctualLightUniforms::portalAxis): the plane across its thinnest axis
/// through its middle, its extent over the other two. Nothing for an empty or non-finite box.
void packPortal(PunctualLightUniforms& entry, const core::Aabb& box) noexcept {
    entry.portalAxis = -1;
    entry.portalPlane = 0.0f;
    entry.portalRect = glm::vec4(0.0f);
    if (box.isEmpty() || !core::isFinite(box.min) || !core::isFinite(box.max)) {
        return;
    }
    const glm::vec3 size = box.max - box.min;
    int axis = 0;
    for (int i = 1; i < 3; ++i) {
        axis = size[i] < size[axis] ? i : axis;
    }
    const int u = axis == 0 ? 1 : 0;
    const int v = axis == 2 ? 1 : 2;
    entry.portalAxis = axis;
    entry.portalPlane = (box.min[axis] + box.max[axis]) * 0.5f;
    entry.portalRect = glm::vec4(box.min[u], box.min[v], box.max[u], box.max[v]);
}

/// packLights() over the lights at `indexOf(0)`, ..., `indexOf(count - 1)` of `lights`, the first `keyCandidates` of
/// them the only ones that may be the key light (or `key`, the position of the key light, when set), each scaled by
/// `weightOf(position)` and clipped to `portalOf(position)`.
template <class IndexOf, class WeightOf, class PortalOf>
PackedLights packIndexed(std::span<const scene::Light> lights, std::size_t count, IndexOf indexOf,
                         const scene::EnvironmentSetup& environment, std::size_t keyCandidates,
                         std::optional<std::size_t> key, float keyShadowWeight, WeightOf weightOf,
                         PortalOf portalOf) {
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
        // The first light that asks for shadows is the key light, whether or not it can have them; a neighbouring
        // area's light (past the candidates) never is. A given key light is the one asking.
        const bool asksForShadows = key ? position == *key : light.castsShadows && position < keyCandidates;
        const bool isKey = asksForShadows && !keyLightSeen;
        keyLightSeen = keyLightSeen || asksForShadows;
        if (isKey) {
            packed.keyLight = index;
        }
        if (const auto reason = rejection(light)) {
            drop(light, index, *reason, isKey);
            continue;
        }
        if (light.type == scene::LightType::Directional) {
            if (directionalUsed) {
                drop(light, index, "only one directional light is supported", isKey);
                continue;
            }
            directionalUsed = true;
            block.directionalDirection = *unitDirection(light.direction);
            block.directionalColor = light.color;
            block.directionalIntensity = light.intensity;
            if (isKey) {
                packed.shadowLight = keyLightOf(light, kDirectionalShadowLight, block.directionalDirection);
                packed.shadowLight.strength *=
                    std::isfinite(keyShadowWeight) ? std::clamp(keyShadowWeight, 0.0f, 1.0f) : 1.0f;
            }
            continue;
        }
        if (block.punctualLightCount >= static_cast<std::int32_t>(kMaxPunctualLights)) {
            drop(light, index, fmt::format("more than {} point and spot lights", kMaxPunctualLights), isKey);
            continue;
        }
        if (isKey && light.type == scene::LightType::Point) {
            packed.shadowNote = fmt::format(
                "light '{}' casts shadows but is a point light: point-light shadows are not supported, make the key "
                "light a spot",
                lightName(light, index));
        }
        PunctualLightUniforms& entry = block.punctualLights[block.punctualLightCount++];
        entry.position = light.position;
        entry.range = std::max(light.range, kMinRange);
        entry.color = light.color;
        const float given = weightOf(position);
        const float weight = std::isfinite(given) ? std::clamp(given, 0.0f, 1.0f) : 1.0f;
        entry.intensity = light.intensity * weight;
        const core::Aabb* portal = portalOf(position);
        packPortal(entry, portal != nullptr ? *portal : core::Aabb{});
        packed.punctual.push_back({index, weight, entry.portalAxis >= 0});
        if (light.type == scene::LightType::Spot) {
            entry.direction = *unitDirection(light.direction);
            const glm::vec2 cone = spotConeScaleOffset(light.innerConeDeg, light.outerConeDeg);
            entry.spotScale = cone.x;
            entry.spotOffset = cone.y;
            if (isKey) {
                packed.shadowLight = keyLightOf(light, block.punctualLightCount - 1, entry.direction);
                packed.shadowLight.range = entry.range;
                packed.shadowLight.strength *=
                    std::isfinite(keyShadowWeight) ? std::clamp(keyShadowWeight, 0.0f, 1.0f) : 1.0f;
            }
        } else {
            entry.direction = glm::vec3(0.0f, -1.0f, 0.0f); // unused: no cone
            entry.spotScale = 0.0f;
            entry.spotOffset = 1.0f;
        }
    }
    packAmbient(block, environment);
    return packed;
}

} // namespace

PackedLights packLights(std::span<const scene::Light> lights, const scene::EnvironmentSetup& environment) {
    return packIndexed(
        lights, lights.size(), [](std::size_t position) { return position; }, environment, lights.size(),
        std::nullopt, 1.0f, [](std::size_t) { return 1.0f; }, [](std::size_t) { return static_cast<const core::Aabb*>(nullptr); });
}

PackedLights packLights(std::span<const scene::Light> lights, std::span<const std::uint32_t> selection,
                        const scene::EnvironmentSetup& environment, std::size_t keyCandidates) {
    LightSelection chosen;
    chosen.indices = selection;
    chosen.keyCandidates = keyCandidates;
    return packLights(lights, chosen, environment);
}

PackedLights packLights(std::span<const scene::Light> lights, const LightSelection& selection,
                        const scene::EnvironmentSetup& environment) {
    const std::span<const std::uint32_t> indices = selection.indices;
    const std::span<const float> weights = selection.weights;
    const std::span<const core::Aabb> portals = selection.portals;
    return packIndexed(
        lights, indices.size(), [indices](std::size_t position) { return static_cast<std::size_t>(indices[position]); },
        environment, selection.keyCandidates, selection.key, selection.keyShadowWeight,
        [weights](std::size_t position) { return position < weights.size() ? weights[position] : 1.0f; },
        [portals](std::size_t position) { return position < portals.size() ? &portals[position] : nullptr; });
}

void packAmbient(LightsUniforms& block, const scene::EnvironmentSetup& environment) noexcept {
    block.ambientSky = environment.ambientSky;
    block.ambientGround = environment.ambientGround;
    block.ambientIntensity = environment.ambientIntensity;
    block.ambientFill = environment.fillColor * environment.fillIntensity;
    if (!core::isFinite(block.ambientFill) || block.ambientFill.r < 0.0f || block.ambientFill.g < 0.0f ||
        block.ambientFill.b < 0.0f) {
        block.ambientFill = glm::vec3(0.0f);
    }
}

void requireShadowCasters(PackedLights& packed, std::span<const scene::Light> lights, const core::Aabb& casterBounds) {
    if (!packed.shadowLight.casts() || packed.shadowLight.type != scene::LightType::Directional ||
        !casterBounds.isEmpty()) {
        return;
    }
    const std::optional<std::size_t> key = packed.keyLight;
    const std::string name = key && *key < lights.size() ? lightName(lights[*key], *key) : std::string("?");
    packed.shadowNote = fmt::format("directional light '{}' casts shadows but no object does: its map is fitted to "
                                    "the objects that cast shadows",
                                    name);
    packed.shadowLight = ShadowLight{};
}

} // namespace rv::gl
