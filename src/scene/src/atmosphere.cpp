#include <rv/scene/atmosphere.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>

#include <glm/geometric.hpp>

#include <rv/core/seed.hpp>

namespace rv::scene {
namespace {

/// Below a pendant, where its pool starts.
constexpr float kPoolBelowLightM = 0.15f;
/// Above the floor, where a key light's motes end.
constexpr float kDustConeFloorM = 0.02f;
/// The shortest emitter worth drawing.
constexpr float kMinLengthM = 0.2f;

float floorOf(const SceneDescription& description, std::optional<std::size_t> area) {
    if (!area || description.areas[*area].boxes.empty()) {
        return 0.0f;
    }
    float floor = description.areas[*area].boxes.front().min.y;
    for (const core::Aabb& box : description.areas[*area].boxes) {
        floor = std::min(floor, box.min.y);
    }
    return floor;
}

/// Meters along a downward `axis` from `apex` to `height` above `floor`; nothing for an axis that does not point down
/// enough or a reach too short.
std::optional<float> reachDown(const Light& light, float floor, float height) {
    if (light.direction.y > -0.05f) {
        return std::nullopt;
    }
    const float reach = (light.position.y - (floor + height)) / -light.direction.y;
    const float length = std::min({reach, light.range, kMaxAtmosphereReachM});
    if (!(length >= kMinLengthM)) {
        return std::nullopt;
    }
    return length;
}

std::uint32_t seedOf(const Light& light) {
    return static_cast<std::uint32_t>(core::deriveSeed("atmosphere", light.name) & 0xffffffffu);
}

std::uint32_t motesOf(std::uint32_t base, float dust) {
    return std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(std::lround(static_cast<double>(base) * dust)));
}

/// The bounds of a cylinder or cone from `apex` along `axis` for `length`, `radius` wide at its end (`top` wide at its
/// start), grown by the motes' wobble.
core::Aabb boundsOf(const glm::vec3& apex, const glm::vec3& axis, float length, float top, float radius) {
    core::Aabb bounds;
    const glm::vec3 end = apex + axis * length;
    bounds.expand(core::Aabb::fromCenterHalfExtents(apex, glm::vec3(top + kMoteWobbleM)));
    bounds.expand(core::Aabb::fromCenterHalfExtents(end, glm::vec3(radius + kMoteWobbleM)));
    return bounds;
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.ends_with(suffix);
}

float unit(std::uint32_t seed, std::uint32_t index, std::uint32_t k) noexcept {
    return static_cast<float>(atmosphereHash(seed ^ atmosphereHash(index * 8u + k))) * (1.0f / 4294967296.0f);
}

float smoothStep(float edge0, float edge1, float x) noexcept {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

std::optional<AtmosphereSetup> atmosphereOf(const SceneDescription& description, std::optional<std::size_t> area) {
    if (area && description.areas[*area].environment.atmosphere) {
        return description.areas[*area].environment.atmosphere;
    }
    return description.environment.atmosphere;
}

std::vector<AtmosphereEmitter> deriveAtmosphere(const SceneDescription& description) {
    std::vector<AtmosphereEmitter> emitters;
    for (std::uint32_t index = 0; index < description.lights.size(); ++index) {
        const Light& light = description.lights[index];
        if (light.type == LightType::Directional) {
            continue;
        }
        const std::optional<std::size_t> area = light.area.empty() ? std::nullopt : description.findArea(light.area);
        const std::optional<AtmosphereSetup> atmosphere = atmosphereOf(description, area);
        if (!atmosphere) {
            continue;
        }
        const float floor = floorOf(description, area);
        const std::string_view name = light.name;
        const bool spot = light.type == LightType::Spot;
        const float tanOuter = std::tan(glm::radians(light.outerConeDeg));
        const bool key = spot && endsWith(name, "key-light");
        if (key && atmosphere->dust > 0.0f) {
            if (const std::optional<float> length = reachDown(light, floor, kDustConeFloorM)) {
                AtmosphereEmitter dust;
                dust.kind = AtmosphereKind::DustCone;
                dust.light = index;
                dust.area = area;
                dust.apex = light.position;
                dust.axis = light.direction;
                dust.length = *length;
                dust.tanHalfAngle = tanOuter;
                dust.motes = motesOf(kDustConeMotes, atmosphere->dust);
                dust.seed = seedOf(light);
                dust.bounds = boundsOf(dust.apex, dust.axis, dust.length, 0.0f, dust.length * tanOuter);
                emitters.push_back(dust);
            }
        }
        if (light.type == LightType::Point && endsWith(name, "fx:room.pendant") && atmosphere->dust > 0.0f) {
            const glm::vec3 apex = light.position - glm::vec3(0.0f, kPoolBelowLightM, 0.0f);
            const float length = std::min(apex.y - (floor + kDustPoolFloorM), kMaxAtmosphereReachM);
            if (length >= kMinLengthM) {
                AtmosphereEmitter pool;
                pool.kind = AtmosphereKind::DustPool;
                pool.light = index;
                pool.area = area;
                pool.apex = apex;
                pool.axis = glm::vec3(0.0f, -1.0f, 0.0f);
                pool.length = length;
                pool.radius = kDustPoolRadiusM;
                pool.motes = motesOf(kDustPoolMotes, atmosphere->dust);
                pool.seed = seedOf(light);
                pool.bounds = boundsOf(pool.apex, pool.axis, pool.length, pool.radius, pool.radius);
                emitters.push_back(pool);
            }
        }
        const bool coned = key || endsWith(name, ".washer") ||
                           name.find("accent:case_light:") != std::string_view::npos ||
                           name.find("accent:shelf_light:") != std::string_view::npos;
        if (spot && coned && atmosphere->lightCones && atmosphere->coneStrength > 0.0f) {
            if (const std::optional<float> length = reachDown(light, floor, kLightConeFloorM)) {
                AtmosphereEmitter cone;
                cone.kind = AtmosphereKind::LightCone;
                cone.light = index;
                cone.area = area;
                cone.apex = light.position;
                cone.axis = light.direction;
                cone.length = *length;
                cone.tanHalfAngle = tanOuter;
                cone.strength = atmosphere->coneStrength;
                cone.seed = seedOf(light);
                cone.bounds = boundsOf(cone.apex, cone.axis, cone.length, 0.0f, cone.length * tanOuter);
                emitters.push_back(cone);
            }
        }
    }
    return emitters;
}

glm::vec3 motePosition(const AtmosphereEmitter& emitter, std::uint32_t index, float time, float* fade) noexcept {
    const std::uint32_t seed = emitter.seed;
    float r[8];
    for (std::uint32_t k = 0; k < 8; ++k) {
        r[k] = unit(seed, index, k);
    }
    constexpr float kTau = 6.2831853f;
    const float speed = (0.0015f + 0.0025f * r[4]) / emitter.length;
    float t = r[0] + time * speed;
    t -= std::floor(t);
    if (fade != nullptr) {
        *fade = smoothStep(0.0f, 0.08f, t) * (1.0f - smoothStep(0.85f, 1.0f, t));
    }
    const bool pool = emitter.kind == AtmosphereKind::DustPool;
    const float along = pool ? emitter.length * t : emitter.length * (0.06f + 0.94f * t);
    const float reach = pool ? emitter.radius : along * emitter.tanHalfAngle;
    const float offset = std::sqrt(r[1]) * 0.92f * reach;
    const float angle = kTau * r[2];
    const glm::vec3 side = std::abs(emitter.axis.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 u = glm::normalize(glm::cross(emitter.axis, side));
    const glm::vec3 v = glm::cross(emitter.axis, u);
    const glm::vec3 wobble = kMoteWobbleM * glm::vec3(std::sin(time * (0.31f + 0.40f * r[5]) + kTau * r[1]),
                                                      std::sin(time * (0.23f + 0.30f * r[6]) + kTau * r[2]),
                                                      std::sin(time * (0.27f + 0.35f * r[7]) + kTau * r[3]));
    return emitter.apex + emitter.axis * along + (u * std::cos(angle) + v * std::sin(angle)) * offset + wobble;
}

} // namespace rv::scene
