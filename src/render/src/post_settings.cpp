#include <rv/render/post_settings.hpp>

#include <algorithm>
#include <cmath>

namespace rv::gl {
namespace {

/// `value` clamped to [0, 1]; non-finite values become 0.
float unitInterval(float value) noexcept {
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

/// Half of `side`, rounded up, at least 1.
int halfSide(int side) noexcept {
    return std::max(1, side / 2 + side % 2);
}

} // namespace

PostSettings postSettingsFor(const scene::EnvironmentSetup& environment) noexcept {
    PostSettings settings;
    settings.exposure = environment.exposure;
    settings.bloomStrength = environment.bloomStrength;
    settings.bloomRadius = environment.bloomRadius;
    settings.vignette = environment.vignette;
    settings.ditherAmplitude = kDitherAmplitude;
    settings.antialiasing = true;
    return settings;
}

PostUniforms makePostUniforms(const PostSettings& settings) noexcept {
    PostUniforms uniforms;
    uniforms.exposure = std::isfinite(settings.exposure) && settings.exposure > 0.0f ? settings.exposure : 1.0f;
    uniforms.bloomMix = unitInterval(settings.bloomStrength);
    uniforms.bloomThreshold = kBloomThreshold;
    uniforms.bloomKnee = kBloomKnee;
    uniforms.vignette = unitInterval(settings.vignette);
    uniforms.ditherAmplitude = unitInterval(settings.ditherAmplitude);
    return uniforms;
}

float bloomScatter(float radius) noexcept {
    return unitInterval(radius);
}

std::array<glm::ivec2, kBloomLevelCount> bloomLevelSizes(glm::ivec2 viewSize) noexcept {
    std::array<glm::ivec2, kBloomLevelCount> sizes{};
    glm::ivec2 size(std::max(viewSize.x, 1), std::max(viewSize.y, 1));
    for (glm::ivec2& level : sizes) {
        size = glm::ivec2(halfSide(size.x), halfSide(size.y));
        level = size;
    }
    return sizes;
}

} // namespace rv::gl
