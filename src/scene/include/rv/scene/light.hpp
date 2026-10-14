#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

namespace rv::scene {

enum class LightType : std::uint8_t { Point, Spot, Directional };

/// Stable lowercase names used by scene files: "point", "spot", "directional".
[[nodiscard]] std::string_view toString(LightType type) noexcept;
[[nodiscard]] std::optional<LightType> lightTypeFromString(std::string_view name) noexcept;

/// Renderer-agnostic punctual light (style §4: practical and accent lights).
///
/// Renderer contract (shading in linear space, see ADR-0002):
///  - Radiance arriving at a surface point = color * intensity * attenuation * spotFactor; `intensity` simply
///    scales the linear `color` (no photometric units).
///  - Point and spot lights: with d = distance from `position` and r = `range`,
///      attenuation = window(d) / max(d * d, 0.0001), window(d) = clamp(1 - (d / r)^4, 0, 1)^2,
///    i.e. inverse square multiplied by a smooth window that reaches exactly 0 at the range, so lights beyond
///    their range can be skipped without a visible cut.
///  - Spot lights: with x = dot(direction, normalize(p - position)), cosInner = cos(innerConeDeg) and
///    cosOuter = cos(outerConeDeg),
///      t = clamp((x - cosOuter) / max(cosInner - cosOuter, 1e-4), 0, 1),  spotFactor = t * t * (3 - 2 * t),
///    i.e. full intensity inside innerConeDeg and zero outside outerConeDeg (half-angles from the axis). The
///    max() keeps hard-edged cones (inner == outer, valid) defined: GLSL smoothstep is undefined for equal edges.
///  - Directional lights: attenuation = spotFactor = 1; light travels along `direction`; position and range are
///    ignored (position may serve as an anchor for shadow-map placement and debug drawing).
struct Light {
    std::string name;
    LightType type = LightType::Point;
    /// World position in meters (point and spot).
    glm::vec3 position{0.0f};
    /// Unit vector the light travels along: from a spot light towards its target, from a directional light
    /// towards the scene (e.g. (0, -1, 0) shines straight down).
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    /// Linear RGB (hex art-direction colors are converted with rv::core::hexToLinearRgb).
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    /// Meters; point and spot lights contribute nothing beyond it.
    float range = 10.0f;
    /// Spot cone half-angles in degrees, 0 <= inner <= outer < 90.
    float innerConeDeg = 25.0f;
    float outerConeDeg = 35.0f;
    bool castsShadows = false;
};

} // namespace rv::scene
