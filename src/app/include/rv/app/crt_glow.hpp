#pragma once

// The hero screen's spill light following its picture (I12 step 6, ADR-0021 §4): GPU-free, in rv_app_cli. The room's
// "accent:crt_glow" light takes the colour of the picture its CRT shows and grows or fades with its brightness,
// eased over kCrtGlowSeconds (at once on a clip's first frame, and under reduceMotion), from the light as the scene
// file describes it.

#include <array>
#include <optional>

#include <glm/vec3.hpp>

namespace rv::app {

/// The light's colour (linear RGB) and intensity.
struct CrtGlow {
    glm::vec3 color{1.0f};
    float intensity = 0.0f;

    bool operator==(const CrtGlow&) const = default;
};

/// How long the glow takes to follow the picture (a time constant, seconds), and how often it is written at most.
inline constexpr float kCrtGlowSeconds = 0.6f;
inline constexpr float kCrtGlowUpdateSeconds = 0.1f;
/// The glow's intensity over the light's own: at a black picture, and at most.
inline constexpr float kCrtGlowDimmest = 0.35f;
inline constexpr float kCrtGlowBrightest = 1.6f;
/// How much of the picture's hue the light takes (the rest stays its own).
inline constexpr float kCrtGlowHue = 0.65f;

/// The glow a picture of mean linear colour `picture` gives a light of `base`: its colour `base` mixed by kCrtGlowHue
/// towards the picture's hue (its colour scaled to its brightest channel; a black picture keeps `base`), its
/// intensity base's times 0.35 + 2.6 times the picture's luminance (Rec. 709), within [kCrtGlowDimmest,
/// kCrtGlowBrightest]. Colours and intensities that are not finite count as black.
[[nodiscard]] CrtGlow crtGlowFor(const CrtGlow& base, const std::array<float, 3>& picture) noexcept;

/// Eases `current` towards `target` over `seconds` with the time constant kCrtGlowSeconds (exponential); `target`
/// itself when `instant`.
[[nodiscard]] CrtGlow easedCrtGlow(const CrtGlow& current, const CrtGlow& target, float seconds, bool instant) noexcept;

} // namespace rv::app
