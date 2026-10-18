#include <rv/assets/mip_chain.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>

namespace rv::assets {
namespace {

double decodeSrgb(double encoded) noexcept {
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

struct SrgbTables {
    std::array<float, 256> toLinear{};
    /// thresholds[k]: the linear value of the encoded (k + 0.5) / 255, the boundary between codes k and k + 1.
    std::array<float, 255> thresholds{};

    SrgbTables() noexcept {
        for (int code = 0; code < 256; ++code) {
            toLinear[static_cast<std::size_t>(code)] = static_cast<float>(decodeSrgb(code / 255.0));
        }
        for (int code = 0; code < 255; ++code) {
            thresholds[static_cast<std::size_t>(code)] = static_cast<float>(decodeSrgb((code + 0.5) / 255.0));
        }
    }
};

const SrgbTables& tables() noexcept {
    static const SrgbTables kTables; // thread-safe initialization (C++11 magic statics)
    return kTables;
}

/// The source pixels a destination pixel covers along one axis, with their weights (their overlap with it, divided by
/// its footprint, so that the weights sum to 1).
struct Tap {
    int first = 0;
    std::vector<float> weights;
};

std::vector<Tap> boxTaps(int source, int destination) {
    std::vector<Tap> taps(static_cast<std::size_t>(destination));
    const double footprint = static_cast<double>(source) / destination;
    for (int i = 0; i < destination; ++i) {
        const double begin = i * footprint;
        const double end = (i + 1) * footprint;
        const int first = static_cast<int>(std::floor(begin));
        const int last = std::min(source - 1, static_cast<int>(std::ceil(end)) - 1);
        Tap& tap = taps[static_cast<std::size_t>(i)];
        tap.first = first;
        for (int j = first; j <= last; ++j) {
            const double overlap = std::min(end, j + 1.0) - std::max(begin, static_cast<double>(j));
            tap.weights.push_back(static_cast<float>(overlap / footprint));
        }
    }
    return taps;
}

} // namespace

float srgbByteToLinear(std::uint8_t code) noexcept {
    return tables().toLinear[code];
}

std::uint8_t linearToSrgbByte(float linear) noexcept {
    if (!(linear > 0.0F)) { // NaN included
        return 0;
    }
    const auto& thresholds = tables().thresholds;
    // The code is the number of thresholds the value reaches (a value on a boundary rounds up, as round() does).
    const auto it = std::upper_bound(thresholds.begin(), thresholds.end(), linear);
    return static_cast<std::uint8_t>(std::distance(thresholds.begin(), it));
}

LinearRgb toLinear(Rgb8 colour) noexcept {
    return {srgbByteToLinear(colour.r), srgbByteToLinear(colour.g), srgbByteToLinear(colour.b)};
}

Rgb8 toSrgb8(LinearRgb colour) noexcept {
    return {linearToSrgbByte(colour.r), linearToSrgbByte(colour.g), linearToSrgbByte(colour.b)};
}

std::string toHexCode(Rgb8 colour) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string text = "#";
    for (const std::uint8_t channel : {colour.r, colour.g, colour.b}) {
        text += kDigits[channel >> 4];
        text += kDigits[channel & 0x0F];
    }
    return text;
}

int mipLevelCount(int width, int height) noexcept {
    if (width < 1 || height < 1) {
        return 0;
    }
    int levels = 1;
    for (int side = std::max(width, height); side > 1; side >>= 1) {
        ++levels;
    }
    return levels;
}

int mipLevelSide(int side, int level) noexcept {
    if (level < 0 || level >= 31) {
        return 1;
    }
    return std::max(1, side >> level);
}

ImageData reduceLinear(const ImageData& image, int width, int height) {
    if (!image.isValid() || image.channels != 4 || width < 1 || height < 1 || width > image.width ||
        height > image.height) {
        return {};
    }
    const auto& toLinearTable = tables().toLinear;
    const std::size_t sourceWidth = static_cast<std::size_t>(image.width);
    const std::size_t sourceHeight = static_cast<std::size_t>(image.height);
    const std::size_t targetWidth = static_cast<std::size_t>(width);
    const std::size_t targetHeight = static_cast<std::size_t>(height);

    // Horizontal pass on linear values: sourceHeight rows of targetWidth pixels.
    const std::vector<Tap> columns = boxTaps(image.width, width);
    std::vector<float> rows(sourceHeight * targetWidth * 4);
    std::vector<float> line(sourceWidth * 4);
    for (std::size_t y = 0; y < sourceHeight; ++y) {
        const std::uint8_t* in = image.pixels.data() + y * sourceWidth * 4;
        for (std::size_t x = 0; x < sourceWidth; ++x) {
            line[x * 4 + 0] = toLinearTable[in[x * 4 + 0]];
            line[x * 4 + 1] = toLinearTable[in[x * 4 + 1]];
            line[x * 4 + 2] = toLinearTable[in[x * 4 + 2]];
            line[x * 4 + 3] = in[x * 4 + 3] / 255.0F;
        }
        float* out = rows.data() + y * targetWidth * 4;
        for (std::size_t x = 0; x < targetWidth; ++x) {
            const Tap& tap = columns[x];
            std::array<float, 4> sum{};
            for (std::size_t k = 0; k < tap.weights.size(); ++k) {
                const float* pixel = line.data() + (static_cast<std::size_t>(tap.first) + k) * 4;
                for (std::size_t c = 0; c < 4; ++c) {
                    sum[c] += pixel[c] * tap.weights[k];
                }
            }
            std::copy(sum.begin(), sum.end(), out + x * 4);
        }
    }

    // Vertical pass, then back to sRGB (alpha as it is).
    const std::vector<Tap> taps = boxTaps(image.height, height);
    ImageData result;
    result.width = width;
    result.height = height;
    result.channels = 4;
    result.pixels.resize(targetWidth * targetHeight * 4);
    for (std::size_t y = 0; y < targetHeight; ++y) {
        const Tap& tap = taps[y];
        std::uint8_t* out = result.pixels.data() + y * targetWidth * 4;
        for (std::size_t x = 0; x < targetWidth; ++x) {
            std::array<float, 4> sum{};
            for (std::size_t k = 0; k < tap.weights.size(); ++k) {
                const float* pixel = rows.data() + ((static_cast<std::size_t>(tap.first) + k) * targetWidth + x) * 4;
                for (std::size_t c = 0; c < 4; ++c) {
                    sum[c] += pixel[c] * tap.weights[k];
                }
            }
            out[x * 4 + 0] = linearToSrgbByte(sum[0]);
            out[x * 4 + 1] = linearToSrgbByte(sum[1]);
            out[x * 4 + 2] = linearToSrgbByte(sum[2]);
            out[x * 4 + 3] = static_cast<std::uint8_t>(std::clamp(std::lround(sum[3] * 255.0F), 0L, 255L));
        }
    }
    return result;
}

std::vector<ImageData> buildMipChain(const ImageData& image) {
    std::vector<ImageData> levels;
    if (!image.isValid() || image.channels != 4) {
        return levels;
    }
    const int count = mipLevelCount(image.width, image.height);
    levels.reserve(static_cast<std::size_t>(count));
    levels.push_back(image);
    for (int level = 1; level < count; ++level) {
        const ImageData& previous = levels.back();
        levels.push_back(reduceLinear(previous, mipLevelSide(image.width, level), mipLevelSide(image.height, level)));
    }
    return levels;
}

} // namespace rv::assets
