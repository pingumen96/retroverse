#pragma once

// The air of a room (I12 step 6, ADR-0021 §4): what the renderer's atmosphere pass draws, derived once from a scene
// description's lights and the atmosphere of their areas. GPU-free: the renderer consumes this plain data.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <glm/vec3.hpp>

#include <rv/core/aabb.hpp>
#include <rv/scene/light.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::scene {

/// What an emitter draws.
enum class AtmosphereKind : std::uint8_t {
    /// Motes drifting in a spot light's cone (the key light's), from near its apex to the floor.
    DustCone,
    /// Motes drifting in a cylinder under a point light (the room's pendant), down to kDustPoolFloorM above the floor.
    DustPool,
    /// A faint cone of light under a spot light, ending kLightConeFloorM above the floor.
    LightCone,
};

/// Motes of a key light's cone and of a pendant's pool at dust 1.
inline constexpr std::uint32_t kDustConeMotes = 320;
inline constexpr std::uint32_t kDustPoolMotes = 160;
/// The pool's radius and how far above the floor it ends; where a light cone ends above the floor.
inline constexpr float kDustPoolRadiusM = 0.9f;
inline constexpr float kDustPoolFloorM = 0.8f;
inline constexpr float kLightConeFloorM = 0.05f;
/// The longest an emitter reaches along its axis.
inline constexpr float kMaxAtmosphereReachM = 4.0f;

/// One emitter, in world space.
struct AtmosphereEmitter {
    AtmosphereKind kind = AtmosphereKind::DustCone;
    /// Index of the light in the description's lights (its colour and intensity are read from it at each view).
    std::uint32_t light = 0;
    /// Index of the light's area in the description's areas; none in a scene without areas.
    std::optional<std::size_t> area;
    /// Where it starts (the light's position) and the unit direction it reaches along (a pool's: straight down).
    glm::vec3 apex{0.0f};
    glm::vec3 axis{0.0f, -1.0f, 0.0f};
    /// Meters along the axis it reaches (> 0).
    float length = 1.0f;
    /// A cone's half-angle tangent (its radius at `length` is length * tanHalfAngle); a pool's radius instead.
    float tanHalfAngle = 0.0f;
    float radius = 0.0f;
    /// Motes (dust), 0 for a light cone.
    std::uint32_t motes = 0;
    /// A light cone's strength (AtmosphereSetup::coneStrength).
    float strength = 0.0f;
    /// The motes' seed: deriveSeed("atmosphere", light name), low 32 bits.
    std::uint32_t seed = 0;
    /// World bounds of everything it may draw (the motes' drift included).
    core::Aabb bounds;

    bool operator==(const AtmosphereEmitter&) const = default;
};

/// The emitters of `description`: for every light whose atmosphere (its area's, else the scene's; absent: none) is set,
/// a DustCone for a spot whose name ends in "key-light", a DustPool for a point light whose name ends in
/// "fx:room.pendant" (dust > 0, motes rounded from the counts above times dust, at least 1), and with lightCones a
/// LightCone for the key light, the washers ("*.washer") and the display cases' and wall shelf's lights
/// ("*accent:case_light:*", "*accent:shelf_light:*"). The floor is the light's area's lowest box bottom (0 in a scene
/// without areas). In the lights' order, a light's dust before its cone.
[[nodiscard]] std::vector<AtmosphereEmitter> deriveAtmosphere(const SceneDescription& description);

/// The atmosphere that holds for the lights of area `area` (none: a scene without areas): the area's, else the
/// scene's.
[[nodiscard]] std::optional<AtmosphereSetup> atmosphereOf(const SceneDescription& description,
                                                          std::optional<std::size_t> area);

/// The integer hash of the motes (lowbias32), mirrored by atmosphere/dust.vert: change both together.
[[nodiscard]] constexpr std::uint32_t atmosphereHash(std::uint32_t x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// Where mote `index` of `emitter` is at `time` seconds (the CPU mirror of atmosphere/dust.vert's motePosition, which
/// the unit tests check): its place along the axis drifts down a few millimetres a second and wraps, its offset from
/// the axis stays within the cone (the pool's cylinder) and it wobbles within kMoteWobbleM. `fade` (when given) is the
/// share of its brightness its place along the axis leaves (0 at the ends, where it wraps).
inline constexpr float kMoteWobbleM = 0.03f;
[[nodiscard]] glm::vec3 motePosition(const AtmosphereEmitter& emitter, std::uint32_t index, float time,
                                     float* fade = nullptr) noexcept;

} // namespace rv::scene
