// The only translation unit that compiles the stb_dxt implementation. STB_DXT_STATIC gives its functions internal
// linkage. stb_dxt (v1.11 and later) keeps no global state: its tables are constant, so concurrent encoding on the
// worker pool is safe.

#include <rv/assets/bc1.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#define STB_DXT_STATIC
#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

namespace rv::assets {
namespace {

std::array<std::uint8_t, 4> expand565(std::uint16_t value) noexcept {
    const int r = (value >> 11) & 0x1F;
    const int g = (value >> 5) & 0x3F;
    const int b = value & 0x1F;
    return {static_cast<std::uint8_t>((r << 3) | (r >> 2)), static_cast<std::uint8_t>((g << 2) | (g >> 4)),
            static_cast<std::uint8_t>((b << 3) | (b >> 2)), std::uint8_t{255}};
}

} // namespace

int bc1BlocksAlong(int side) noexcept {
    return side < 1 ? 0 : (side + 3) / 4;
}

std::size_t bc1LevelBytes(int width, int height) noexcept {
    return static_cast<std::size_t>(bc1BlocksAlong(width)) * static_cast<std::size_t>(bc1BlocksAlong(height)) *
           kBc1BlockBytes;
}

std::vector<std::byte> encodeBc1(const ImageData& image, Bc1Quality quality) {
    std::vector<std::byte> blocks;
    if (!image.isValid() || image.channels != 4) {
        return blocks;
    }
    const int blocksX = bc1BlocksAlong(image.width);
    const int blocksY = bc1BlocksAlong(image.height);
    blocks.resize(bc1LevelBytes(image.width, image.height));
    const int mode = quality == Bc1Quality::High ? STB_DXT_HIGHQUAL : STB_DXT_NORMAL;
    std::array<unsigned char, 64> source{};
    auto* out = reinterpret_cast<unsigned char*>(blocks.data());
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            for (int py = 0; py < 4; ++py) {
                const int y = std::min(by * 4 + py, image.height - 1);
                for (int px = 0; px < 4; ++px) {
                    const int x = std::min(bx * 4 + px, image.width - 1);
                    const std::uint8_t* pixel =
                        image.pixels.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                                               static_cast<std::size_t>(x)) *
                                                  4;
                    unsigned char* target = source.data() + (py * 4 + px) * 4;
                    target[0] = pixel[0];
                    target[1] = pixel[1];
                    target[2] = pixel[2];
                    target[3] = 255; // stb_dxt needs a constant alpha when it stores none
                }
            }
            stb_compress_dxt_block(out, source.data(), 0, mode);
            out += kBc1BlockBytes;
        }
    }
    return blocks;
}

ImageData decodeBc1(std::span<const std::byte> blocks, int width, int height) {
    if (width < 1 || height < 1 || blocks.size() != bc1LevelBytes(width, height)) {
        return {};
    }
    ImageData image;
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    const int blocksX = bc1BlocksAlong(width);
    const int blocksY = bc1BlocksAlong(height);
    const auto* in = reinterpret_cast<const unsigned char*>(blocks.data());
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            const auto c0 = static_cast<std::uint16_t>(in[0] | (in[1] << 8));
            const auto c1 = static_cast<std::uint16_t>(in[2] | (in[3] << 8));
            const std::uint32_t indices = static_cast<std::uint32_t>(in[4]) | (static_cast<std::uint32_t>(in[5]) << 8) |
                                          (static_cast<std::uint32_t>(in[6]) << 16) |
                                          (static_cast<std::uint32_t>(in[7]) << 24);
            std::array<std::array<std::uint8_t, 4>, 4> palette{};
            palette[0] = expand565(c0);
            palette[1] = expand565(c1);
            for (std::size_t c = 0; c < 3; ++c) {
                const int a = palette[0][c];
                const int b = palette[1][c];
                if (c0 > c1) {
                    palette[2][c] = static_cast<std::uint8_t>((2 * a + b) / 3);
                    palette[3][c] = static_cast<std::uint8_t>((a + 2 * b) / 3);
                } else {
                    palette[2][c] = static_cast<std::uint8_t>((a + b) / 2);
                    palette[3][c] = 0;
                }
            }
            palette[2][3] = 255;
            palette[3][3] = c0 > c1 ? std::uint8_t{255} : std::uint8_t{0};
            for (int py = 0; py < 4; ++py) {
                const int y = by * 4 + py;
                for (int px = 0; px < 4; ++px) {
                    const int x = bx * 4 + px;
                    if (x >= width || y >= height) {
                        continue;
                    }
                    const auto index = static_cast<std::size_t>((indices >> (2 * (py * 4 + px))) & 0x3U);
                    std::uint8_t* pixel =
                        image.pixels.data() +
                        (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) *
                            4;
                    std::copy(palette[index].begin(), palette[index].end(), pixel);
                }
            }
            in += kBc1BlockBytes;
        }
    }
    return image;
}

double psnrRgb(const ImageData& expected, const ImageData& actual) {
    if (!expected.isValid() || !actual.isValid() || expected.width != actual.width ||
        expected.height != actual.height || expected.channels < 3 || actual.channels < 3) {
        return 0.0;
    }
    const std::size_t pixels = static_cast<std::size_t>(expected.width) * static_cast<std::size_t>(expected.height);
    double squared = 0.0;
    for (std::size_t i = 0; i < pixels; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            const double difference =
                static_cast<double>(expected.pixels[i * static_cast<std::size_t>(expected.channels) + c]) -
                static_cast<double>(actual.pixels[i * static_cast<std::size_t>(actual.channels) + c]);
            squared += difference * difference;
        }
    }
    if (squared == 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    const double mse = squared / (static_cast<double>(pixels) * 3.0);
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

} // namespace rv::assets
