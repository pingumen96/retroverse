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
    }
    return "srgb";
}

std::optional<SelfCheck> selfCheckFromString(std::string_view name) noexcept {
    if (name == toString(SelfCheck::Srgb)) {
        return SelfCheck::Srgb;
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

} // namespace rv::app
