#include "procedural_noise.hpp"

#include <algorithm>

namespace rv::assets::detail {
namespace {

/// Quintic fade 6t^5 - 15t^4 + 10t^3 (Perlin's improved noise): zero first and second derivatives at the lattice.
float fade(float t) noexcept {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/// x modulo `period` in [0, period).
std::int32_t wrap(std::int32_t x, std::int32_t period) noexcept {
    const std::int32_t r = x % period;
    return r < 0 ? r + period : r;
}

} // namespace

std::uint32_t hashPoint(std::int32_t x, std::int32_t y, std::uint32_t seed) noexcept {
    // Two rounds of a murmur3-style finalizer over the packed coordinates: neighbors decorrelate fully.
    std::uint32_t h = seed ^ 0x9E3779B9u;
    h ^= static_cast<std::uint32_t>(x) * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h ^= static_cast<std::uint32_t>(y) * 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

float hashUnit(std::int32_t x, std::int32_t y, std::uint32_t seed) noexcept {
    return static_cast<float>(hashPoint(x, y, seed) >> 8) * (1.0f / 16777216.0f);
}

std::uint32_t subSeed(std::uint32_t seed, std::uint32_t index) noexcept {
    return hashPoint(static_cast<std::int32_t>(index), 0x51ED27, seed);
}

std::int32_t floorToInt(float x) noexcept {
    const auto truncated = static_cast<std::int32_t>(x);
    return static_cast<float>(truncated) > x ? truncated - 1 : truncated;
}

float fract(float x) noexcept {
    return x - static_cast<float>(floorToInt(x));
}

float smoothStep(float edge0, float edge1, float x) noexcept {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float valueNoise(float x, float y, std::int32_t periodX, std::int32_t periodY, std::uint32_t seed) noexcept {
    const std::int32_t cellX = floorToInt(x);
    const std::int32_t cellY = floorToInt(y);
    const float tx = fade(x - static_cast<float>(cellX));
    const float ty = fade(y - static_cast<float>(cellY));
    const std::int32_t x0 = wrap(cellX, periodX);
    const std::int32_t y0 = wrap(cellY, periodY);
    const std::int32_t x1 = x0 + 1 == periodX ? 0 : x0 + 1;
    const std::int32_t y1 = y0 + 1 == periodY ? 0 : y0 + 1;
    const float a = hashUnit(x0, y0, seed);
    const float b = hashUnit(x1, y0, seed);
    const float c = hashUnit(x0, y1, seed);
    const float d = hashUnit(x1, y1, seed);
    const float top = a + (b - a) * tx;
    const float bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

float fbm(float u, float v, std::int32_t cellsX, std::int32_t cellsY, int octaves, float gain,
          std::uint32_t seed) noexcept {
    float sum = 0.0f;
    float weight = 1.0f;
    float weights = 0.0f;
    std::int32_t periodX = std::max(cellsX, 1);
    std::int32_t periodY = std::max(cellsY, 1);
    for (int octave = 0; octave < octaves; ++octave) {
        const float noise = valueNoise(u * static_cast<float>(periodX), v * static_cast<float>(periodY), periodX,
                                       periodY, subSeed(seed, static_cast<std::uint32_t>(octave)));
        sum += weight * (2.0f * noise - 1.0f);
        weights += weight;
        weight *= gain;
        periodX *= 2;
        periodY *= 2;
    }
    return weights > 0.0f ? sum / weights : 0.0f;
}

} // namespace rv::assets::detail
