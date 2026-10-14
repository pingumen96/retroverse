#pragma once

#include <string_view>

#include <glm/glm.hpp>

#include <rv/core/expected.hpp>

namespace rv::core {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 2.0f * kPi;
inline constexpr float kHalfPi = 0.5f * kPi;

/// Half-line `origin + t * direction`, t >= 0. The direction does not need to be normalized; parametric
/// distances are then expressed in multiples of its length.
struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};

    [[nodiscard]] glm::vec3 at(float t) const noexcept { return origin + t * direction; }
};

[[nodiscard]] constexpr float degreesToRadians(float degrees) noexcept {
    return degrees * (kPi / 180.0f);
}

[[nodiscard]] constexpr float radiansToDegrees(float radians) noexcept {
    return radians * (180.0f / kPi);
}

/// Absolute-or-relative tolerance comparison.
[[nodiscard]] bool nearlyEqual(float a, float b, float epsilon = 1e-5f) noexcept;
[[nodiscard]] bool nearlyEqual(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-5f) noexcept;
[[nodiscard]] bool nearlyEqual(const glm::vec4& a, const glm::vec4& b, float epsilon = 1e-5f) noexcept;
[[nodiscard]] bool nearlyEqual(const glm::mat4& a, const glm::mat4& b, float epsilon = 1e-5f) noexcept;

[[nodiscard]] bool isFinite(const glm::vec3& v) noexcept;
[[nodiscard]] bool isFinite(const glm::vec4& v) noexcept;
[[nodiscard]] bool isFinite(const glm::mat4& m) noexcept;

/// Wraps an angle to the interval (-pi, pi].
[[nodiscard]] float wrapAngleRadians(float radians) noexcept;

/// sRGB electro-optical transfer function (IEC 61966-2-1), per component, input clamped to [0, 1].
[[nodiscard]] float srgbToLinear(float encoded) noexcept;
[[nodiscard]] float linearToSrgb(float linear) noexcept;
[[nodiscard]] glm::vec3 srgbToLinear(const glm::vec3& encoded) noexcept;
[[nodiscard]] glm::vec3 linearToSrgb(const glm::vec3& linear) noexcept;

/// Parses "#RRGGBB", "RRGGBB", "#RRGGBBAA" or "RRGGBBAA" into sRGB-encoded components in [0, 1]
/// (alpha defaults to 1). Hex digits are case-insensitive.
[[nodiscard]] Expected<glm::vec4> parseHexColor(std::string_view text);

/// Art-direction colors are authored as sRGB hex codes (style guide §3); shading happens in linear space.
/// Example: "#808080" -> (0.2158, 0.2158, 0.2158).
[[nodiscard]] Expected<glm::vec3> hexToLinearRgb(std::string_view text);

} // namespace rv::core
