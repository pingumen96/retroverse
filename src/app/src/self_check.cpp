#include <rv/app/self_check.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <rv/core/math.hpp>

namespace rv::app {

std::string_view toString(SelfCheck check) noexcept {
    switch (check) {
    case SelfCheck::Srgb:
        return "srgb";
    case SelfCheck::Bloom:
        return "bloom";
    }
    return "srgb";
}

std::optional<SelfCheck> selfCheckFromString(std::string_view name) noexcept {
    for (const SelfCheck check : {SelfCheck::Srgb, SelfCheck::Bloom}) {
        if (name == toString(check)) {
            return check;
        }
    }
    return std::nullopt;
}

int expectedSrgbByte(float linear) noexcept {
    return static_cast<int>(std::lround(core::linearToSrgb(linear) * 255.0f));
}

SrgbCheckResult evaluateSrgbCheck(std::span<const std::uint8_t> rgbaPixels) noexcept {
    SrgbCheckResult result;
    result.expected = expectedSrgbByte(kSrgbCheckLinearValue);
    result.pixelCount = rgbaPixels.size() / 4;
    if (result.pixelCount == 0) {
        return result;
    }
    result.minimum = 255;
    result.maximum = 0;
    for (std::size_t pixel = 0; pixel < result.pixelCount; ++pixel) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const int value = rgbaPixels[pixel * 4 + channel];
            result.minimum = std::min(result.minimum, value);
            result.maximum = std::max(result.maximum, value);
        }
    }
    result.passed = std::abs(result.minimum - result.expected) <= kSrgbCheckTolerance &&
                    std::abs(result.maximum - result.expected) <= kSrgbCheckTolerance;
    return result;
}

BloomCheckResult evaluateBloomCheck(const BloomCheckMeasurements& measurements) noexcept {
    BloomCheckResult result;
    result.litStaysOut = measurements.litBloomPeak == 0.0f;
    result.energyKept = std::isfinite(measurements.emitterEnergyRatio) &&
                        std::abs(measurements.emitterEnergyRatio - 1.0f) <= kBloomCheckEnergyTolerance;
    const std::vector<float>& energies = measurements.fireflyEnergies;
    double sum = 0.0;
    bool finite = true;
    for (const float energy : energies) {
        finite = finite && std::isfinite(energy);
        sum += static_cast<double>(energy);
    }
    if (energies.size() >= 2 && finite && sum > 0.0) {
        const double mean = sum / static_cast<double>(energies.size());
        double spread = 0.0;
        for (const float energy : energies) {
            spread = std::max(spread, std::abs(static_cast<double>(energy) - mean) / mean);
        }
        result.fireflySpread = static_cast<float>(spread);
        result.firefliesSteady = result.fireflySpread <= kBloomCheckFireflyTolerance;
    }
    result.passed = result.litStaysOut && result.energyKept && result.firefliesSteady;
    return result;
}

} // namespace rv::app
