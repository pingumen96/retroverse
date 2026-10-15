#include <rv/render/ssao_settings.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/vec3.hpp>

namespace rv::gl {
namespace {

/// Shortest sample offset, as a share of the radius.
constexpr float kNearestSample = 0.1f;
/// Coprime with kSsaoSampleCount: (i * kLengthStride) % count visits every length once, out of step with i.
constexpr std::uint32_t kLengthStride = 7;
constexpr float kTwoPi = 6.28318530717959f;

static_assert(kSsaoSampleCount % 2 == 0 && kLengthStride % 2 == 1, "the length order must be a permutation");

/// Base-2 radical inverse of `index` (van der Corput): its bits mirrored behind the binary point.
float radicalInverse(std::uint32_t index) noexcept {
    std::uint32_t bits = index;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f; // / 2^32
}

/// `value` clamped to [minimum, maximum]; non-finite values become `fallback`.
float clampedOr(float value, float minimum, float maximum, float fallback) noexcept {
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

} // namespace

std::array<glm::vec4, kSsaoSampleCount> ssaoKernel() noexcept {
    std::array<glm::vec4, kSsaoSampleCount> kernel{};
    const float count = static_cast<float>(kSsaoSampleCount);
    for (std::uint32_t index = 0; index < kSsaoSampleCount; ++index) {
        // Hammersley point (u stratified, v the radical inverse), mapped to a cosine-weighted direction: u = sin^2.
        const float u = (static_cast<float>(index) + 0.5f) / count;
        const float phi = kTwoPi * radicalInverse(index);
        const float sinTheta = std::sqrt(u);
        const float cosTheta = std::sqrt(1.0f - u);
        const glm::vec3 direction(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta);
        const float step = static_cast<float>((index * kLengthStride) % kSsaoSampleCount + 1) / count;
        const float length = kNearestSample + (1.0f - kNearestSample) * step * step;
        kernel[index] = glm::vec4(direction * length, 0.0f);
    }
    return kernel;
}

SsaoUniforms makeSsaoUniforms(const scene::SsaoSetup& setup) noexcept {
    SsaoUniforms uniforms;
    const std::array<glm::vec4, kSsaoSampleCount> kernel = ssaoKernel();
    std::copy(kernel.begin(), kernel.end(), uniforms.kernel);
    uniforms.radius = clampedOr(setup.radius, kMinSsaoRadius, scene::kMaxSsaoRadius, scene::SsaoSetup{}.radius);
    uniforms.intensity = clampedOr(setup.intensity, 0.0f, scene::kMaxSsaoIntensity, 1.0f);
    uniforms.bias = kSsaoBiasRadiusFraction * uniforms.radius;
    uniforms.depthTolerance = kSsaoBlurDepthTolerance;
    return uniforms;
}

glm::ivec2 ssaoTargetSize(glm::ivec2 viewSize) noexcept {
    const glm::ivec2 size(std::max(viewSize.x, 1), std::max(viewSize.y, 1));
    return {size.x / 2 + size.x % 2, size.y / 2 + size.y % 2};
}

} // namespace rv::gl
