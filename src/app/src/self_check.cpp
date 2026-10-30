#include <rv/app/self_check.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>

#include <rv/core/math.hpp>

namespace rv::app {
namespace {

/// The RGB of an RGBA8 pixel, packed.
std::uint32_t rgbAt(std::span<const std::uint8_t> rgba, std::size_t pixel) noexcept {
    const std::size_t offset = pixel * 4;
    return (std::uint32_t{rgba[offset]} << 16) | (std::uint32_t{rgba[offset + 1]} << 8) | rgba[offset + 2];
}

/// Sum of the channels of a packed RGB color: orders the two colors of the FXAA check.
std::uint32_t brightness(std::uint32_t rgb) noexcept {
    return ((rgb >> 16) & 0xFFu) + ((rgb >> 8) & 0xFFu) + (rgb & 0xFFu);
}

} // namespace

std::string_view toString(SelfCheck check) noexcept {
    switch (check) {
    case SelfCheck::Srgb:
        return "srgb";
    case SelfCheck::Bloom:
        return "bloom";
    case SelfCheck::Fxaa:
        return "fxaa";
    }
    return "srgb";
}

std::optional<SelfCheck> selfCheckFromString(std::string_view name) noexcept {
    for (const SelfCheck check : kSelfChecks) {
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

FxaaCheckResult evaluateFxaaCheck(std::span<const std::uint8_t> aliased, std::span<const std::uint8_t> antialiased,
                                  int width, int height) noexcept {
    FxaaCheckResult result;
    if (width <= 0 || height <= 0) {
        return result;
    }
    const auto columns = static_cast<std::size_t>(width);
    const auto rows = static_cast<std::size_t>(height);
    const std::size_t pixels = columns * rows;
    if (aliased.size() < pixels * 4 || antialiased.size() < pixels * 4) {
        return result;
    }
    std::uint32_t dark = rgbAt(aliased, 0);
    std::uint32_t bright = dark;
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const std::uint32_t rgb = rgbAt(aliased, pixel);
        dark = brightness(rgb) < brightness(dark) ? rgb : dark;
        bright = brightness(rgb) > brightness(bright) ? rgb : bright;
    }
    result.aliasedIsHard = dark != bright;
    for (std::size_t pixel = 0; pixel < pixels && result.aliasedIsHard; ++pixel) {
        const std::uint32_t rgb = rgbAt(aliased, pixel);
        result.aliasedIsHard = rgb == dark || rgb == bright;
    }

    for (std::size_t y = 0; y < rows && dark != bright; ++y) {
        bool hasDark = false;
        bool hasBright = false;
        bool blended = false;
        for (std::size_t x = 0; x < columns; ++x) {
            const std::uint32_t before = rgbAt(aliased, y * columns + x);
            hasDark = hasDark || before == dark;
            hasBright = hasBright || before == bright;
            const std::uint32_t after = rgbAt(antialiased, y * columns + x);
            blended = blended || (after != dark && after != bright);
        }
        if (hasDark && hasBright) {
            ++result.edgeRows;
            result.blendedEdgeRows += blended ? 1 : 0;
        }
    }
    result.edgeBlended = result.edgeRows > 0 && static_cast<float>(result.blendedEdgeRows) >=
                                                    kFxaaCheckMinBlendedShare * static_cast<float>(result.edgeRows);

    // A pixel is flat when its 3x3 neighborhood (clamped to the image) holds a single color.
    for (std::size_t y = 0; y < rows; ++y) {
        for (std::size_t x = 0; x < columns; ++x) {
            const std::uint32_t center = rgbAt(aliased, y * columns + x);
            bool flat = true;
            for (std::size_t ny = (y == 0 ? 0 : y - 1); ny <= std::min(y + 1, rows - 1) && flat; ++ny) {
                for (std::size_t nx = (x == 0 ? 0 : x - 1); nx <= std::min(x + 1, columns - 1) && flat; ++nx) {
                    flat = rgbAt(aliased, ny * columns + nx) == center;
                }
            }
            if (flat) {
                ++result.flatPixels;
                result.flatChanged += rgbAt(antialiased, y * columns + x) != center ? 1 : 0;
            }
        }
    }
    result.flatCopied = result.flatPixels > 0 && result.flatChanged == 0;
    result.passed = result.aliasedIsHard && result.edgeBlended && result.flatCopied;
    return result;
}

void evaluateFxaaInPlaceWrite(FxaaCheckResult& result, std::span<const std::uint8_t> direct,
                              std::span<const std::uint8_t> inPlace, int width, int height) noexcept {
    result.inPlacePixels = 0;
    result.inPlaceChanged = 0;
    result.inPlaceExact = false;
    if (width > 0 && height > 0) {
        const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        if (direct.size() >= pixels * 4 && inPlace.size() >= pixels * 4) {
            result.inPlacePixels = pixels;
            for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
                const bool same = std::equal(direct.begin() + static_cast<std::ptrdiff_t>(pixel * 4),
                                             direct.begin() + static_cast<std::ptrdiff_t>(pixel * 4 + 4),
                                             inPlace.begin() + static_cast<std::ptrdiff_t>(pixel * 4));
                result.inPlaceChanged += same ? 0 : 1;
            }
            result.inPlaceExact = result.inPlaceChanged == 0;
        }
    }
    result.passed = result.passed && result.inPlaceExact;
}

} // namespace rv::app
