#include <rv/screenshot_diff/screenshot_diff.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include <fmt/format.h>
#include <glm/common.hpp>

#include <rv/assets/image_io.hpp>
#include <rv/core/text.hpp>

namespace rv::screenshot_diff {
namespace {

/// Channel difference from which the heatmap shows pure yellow.
constexpr int kHeatmapSaturation = 64;

Status validateImage(const assets::ImageData& image, std::string_view which) {
    if (!image.isValid()) {
        return makeError(ErrorCode::InvalidArgument, "invalid image", std::string(which));
    }
    return ok();
}

Status validateThreshold(int channelThreshold) {
    if (channelThreshold < 0 || channelThreshold > 255) {
        return makeError(ErrorCode::InvalidArgument, "the channel threshold must be between 0 and 255",
                         fmt::format("threshold {}", channelThreshold));
    }
    return ok();
}

/// Largest difference of the three channels of pixel `index` of two RGB images.
int pixelDifference(const assets::ImageData& a, const assets::ImageData& b, std::size_t index) noexcept {
    const std::size_t offset = index * 3;
    int largest = 0;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        largest = std::max(largest, std::abs(static_cast<int>(a.pixels[offset + channel]) -
                                             static_cast<int>(b.pixels[offset + channel])));
    }
    return largest;
}

std::uint8_t toByte(int value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

} // namespace

double Comparison::differingPercent() const noexcept {
    return pixels == 0 ? 0.0 : 100.0 * static_cast<double>(differingPixels) / static_cast<double>(pixels);
}

std::string Comparison::describe(const Tolerance& tolerance) const {
    if (!sameSize()) {
        return fmt::format("the sizes differ: expected {}x{}, actual {}x{}", expectedSize.x, expectedSize.y,
                           actualSize.x, actualSize.y);
    }
    std::string where;
    if (differingPixels > 0) {
        where = fmt::format(", in ({}, {}) - ({}, {})", differingMin.x, differingMin.y, differingMax.x, differingMax.y);
    }
    return fmt::format("{} of {} pixels ({:.3f}%) differ by more than {}/255 (largest difference {}/255{}): {}, at "
                       "most {:g}% may differ",
                       differingPixels, pixels, differingPercent(), tolerance.channelThreshold, maxChannelDifference,
                       where, matches ? "match" : "MISMATCH", tolerance.maxDifferingPercent);
}

Expected<Comparison> compareImages(const assets::ImageData& expected, const assets::ImageData& actual,
                                   const Tolerance& tolerance) {
    if (auto valid = validateImage(expected, "expected image"); !valid) {
        return propagate(valid.error());
    }
    if (auto valid = validateImage(actual, "actual image"); !valid) {
        return propagate(valid.error());
    }
    if (auto valid = validateThreshold(tolerance.channelThreshold); !valid) {
        return propagate(valid.error());
    }
    if (!std::isfinite(tolerance.maxDifferingPercent) || tolerance.maxDifferingPercent < 0.0 ||
        tolerance.maxDifferingPercent > 100.0) {
        return makeError(ErrorCode::InvalidArgument, "the share of differing pixels must be between 0 and 100%",
                         fmt::format("{}%", tolerance.maxDifferingPercent));
    }
    Comparison comparison;
    comparison.expectedSize = glm::ivec2(expected.width, expected.height);
    comparison.actualSize = glm::ivec2(actual.width, actual.height);
    if (!comparison.sameSize()) {
        return comparison;
    }
    const assets::ImageData a = assets::convertChannels(expected, 3);
    const assets::ImageData b = assets::convertChannels(actual, 3);
    comparison.pixels = static_cast<std::size_t>(a.width) * static_cast<std::size_t>(a.height);
    glm::ivec2 low(a.width, a.height);
    glm::ivec2 high(0);
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width) +
                                      static_cast<std::size_t>(x);
            const int difference = pixelDifference(a, b, index);
            if (difference == 0) {
                continue;
            }
            ++comparison.changedPixels;
            comparison.maxChannelDifference = std::max(comparison.maxChannelDifference, difference);
            if (difference > tolerance.channelThreshold) {
                ++comparison.differingPixels;
                low = glm::min(low, glm::ivec2(x, y));
                high = glm::max(high, glm::ivec2(x + 1, y + 1));
            }
        }
    }
    if (comparison.differingPixels > 0) {
        comparison.differingMin = low;
        comparison.differingMax = high;
    }
    comparison.matches = comparison.differingPercent() <= tolerance.maxDifferingPercent;
    return comparison;
}

Expected<assets::ImageData> makeHeatmap(const assets::ImageData& expected, const assets::ImageData& actual,
                                        int channelThreshold) {
    if (auto valid = validateImage(expected, "expected image"); !valid) {
        return propagate(valid.error());
    }
    if (auto valid = validateImage(actual, "actual image"); !valid) {
        return propagate(valid.error());
    }
    if (auto valid = validateThreshold(channelThreshold); !valid) {
        return propagate(valid.error());
    }
    if (expected.width != actual.width || expected.height != actual.height) {
        return makeError(ErrorCode::InvalidArgument, "a heatmap needs two images of the same size",
                         fmt::format("{}x{} and {}x{}", expected.width, expected.height, actual.width, actual.height));
    }
    const assets::ImageData a = assets::convertChannels(expected, 3);
    const assets::ImageData b = assets::convertChannels(actual, 3);
    const assets::ImageData grey = assets::convertChannels(expected, 1);
    assets::ImageData heatmap;
    heatmap.width = a.width;
    heatmap.height = a.height;
    heatmap.channels = 3;
    heatmap.pixels.resize(a.pixels.size());
    const std::size_t pixels = static_cast<std::size_t>(a.width) * static_cast<std::size_t>(a.height);
    for (std::size_t index = 0; index < pixels; ++index) {
        const int difference = pixelDifference(a, b, index);
        std::uint8_t* out = &heatmap.pixels[index * 3];
        if (difference == 0) {
            const std::uint8_t context = toByte(grey.pixels[index] / 4);
            out[0] = context;
            out[1] = context;
            out[2] = context;
        } else if (difference <= channelThreshold) {
            out[0] = 0;
            out[1] = 0;
            out[2] = 110;
        } else {
            const int span = std::max(1, kHeatmapSaturation - channelThreshold);
            out[0] = 255;
            out[1] = toByte((difference - channelThreshold) * 255 / span);
            out[2] = 0;
        }
    }
    return heatmap;
}

Expected<Comparison> compareFiles(const std::filesystem::path& expected, const std::filesystem::path& actual,
                                  const Tolerance& tolerance,
                                  const std::optional<std::filesystem::path>& heatmapOnMismatch) {
    auto expectedImage = assets::loadImage(expected);
    if (!expectedImage) {
        return propagate(expectedImage.error());
    }
    auto actualImage = assets::loadImage(actual);
    if (!actualImage) {
        return propagate(actualImage.error());
    }
    auto comparison = compareImages(*expectedImage, *actualImage, tolerance);
    if (!comparison) {
        return propagate(comparison.error());
    }
    if (!comparison->matches && comparison->sameSize() && heatmapOnMismatch) {
        auto heatmap = makeHeatmap(*expectedImage, *actualImage, tolerance.channelThreshold);
        if (!heatmap) {
            return propagate(heatmap.error());
        }
        if (auto written = assets::writePng(*heatmapOnMismatch, *heatmap); !written) {
            return propagate(written.error());
        }
    }
    return comparison;
}

} // namespace rv::screenshot_diff
