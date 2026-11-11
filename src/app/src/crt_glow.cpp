#include <rv/app/crt_glow.hpp>

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>

namespace rv::app {
namespace {

float finiteOrZero(float value) noexcept {
    return std::isfinite(value) ? value : 0.0f;
}

} // namespace

CrtGlow crtGlowFor(const CrtGlow& base, const std::array<float, 3>& picture) noexcept {
    const glm::vec3 colour(std::max(finiteOrZero(picture[0]), 0.0f), std::max(finiteOrZero(picture[1]), 0.0f),
                           std::max(finiteOrZero(picture[2]), 0.0f));
    const float brightest = std::max({colour.r, colour.g, colour.b});
    const float luminance = 0.2126f * colour.r + 0.7152f * colour.g + 0.0722f * colour.b;
    CrtGlow glow;
    glow.color = brightest > 1e-4f ? glm::mix(base.color, colour / brightest, kCrtGlowHue) : base.color;
    glow.intensity =
        finiteOrZero(base.intensity) * std::clamp(0.35f + 2.6f * luminance, kCrtGlowDimmest, kCrtGlowBrightest);
    return glow;
}

CrtGlow easedCrtGlow(const CrtGlow& current, const CrtGlow& target, float seconds, bool instant) noexcept {
    if (instant || !std::isfinite(seconds)) {
        return target;
    }
    const float t = 1.0f - std::exp(-std::max(seconds, 0.0f) / kCrtGlowSeconds);
    CrtGlow eased;
    eased.color = glm::mix(current.color, target.color, t);
    eased.intensity = current.intensity + (target.intensity - current.intensity) * t;
    return eased;
}

} // namespace rv::app
