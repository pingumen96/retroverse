#pragma once

// Helpers of the cover and label tests (I3 step 5): generated covers (original procedural pictures, never a real
// cover), a palette PNG writer (stb_image_write writes no indexed PNG), the bundled fonts.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <rv/assets/image_data.hpp>
#include <rv/assets/text_raster.hpp>
#include <rv/core/random.hpp>
#include <rv/core/sha256.hpp>

namespace rv::test::covers {

inline const std::filesystem::path kAssetRoot = RV_TEST_ASSET_DIR;

inline assets::ImageData blankImage(int width, int height, int channels, std::uint8_t value = 0) {
    assets::ImageData image;
    image.width = width;
    image.height = height;
    image.channels = channels;
    image.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                            static_cast<std::size_t>(channels),
                        value);
    return image;
}

/// An original procedural "cover": a two-colour diagonal gradient, a few flat discs and bars, a band of fine detail
/// and mild noise, from `seed`. RGB unless `channels` says otherwise (4: alpha 255).
inline assets::ImageData makeCover(int width, int height, std::uint64_t seed, int channels = 3) {
    core::Random random(seed);
    const auto channel = [&] { return static_cast<int>(random.below(256)); };
    const std::array<int, 3> from = {channel(), channel(), channel()};
    const std::array<int, 3> to = {channel(), channel(), channel()};
    struct Disc {
        double x, y, r;
        std::array<int, 3> colour;
    };
    std::vector<Disc> discs;
    for (int i = 0; i < 5; ++i) {
        discs.push_back({random.unit() * width, random.unit() * height, (0.05 + random.unit() * 0.2) * height,
                         {channel(), channel(), channel()}});
    }
    const std::array<int, 3> bar = {channel(), channel(), channel()};
    assets::ImageData image = blankImage(width, height, channels, 255);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double t = (static_cast<double>(x) / width + static_cast<double>(y) / height) / 2.0;
            std::array<double, 3> colour{};
            for (std::size_t c = 0; c < 3; ++c) {
                colour[c] = from[c] + (to[c] - from[c]) * t;
            }
            for (const Disc& disc : discs) {
                if (std::hypot(x - disc.x, y - disc.y) < disc.r) {
                    for (std::size_t c = 0; c < 3; ++c) {
                        colour[c] = disc.colour[c];
                    }
                }
            }
            if (y > height * 8 / 10 && y < height * 9 / 10 && (x / 3) % 4 != 0) {
                for (std::size_t c = 0; c < 3; ++c) {
                    colour[c] = bar[c];
                }
            }
            const double noise = (static_cast<double>(random.below(17)) - 8.0);
            std::uint8_t* pixel = image.pixels.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                                         static_cast<std::size_t>(x)) *
                                                            static_cast<std::size_t>(channels);
            for (int c = 0; c < std::min(channels, 3); ++c) {
                pixel[c] = static_cast<std::uint8_t>(std::clamp(colour[static_cast<std::size_t>(c)] + noise, 0.0, 255.0));
            }
        }
    }
    return image;
}

inline std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0) {
    crc = ~crc;
    for (const std::uint8_t value : bytes) {
        crc ^= value;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

/// An indexed-colour PNG (colour type 3, 8 bits) with its palette and, when `alpha` is not empty, a tRNS chunk;
/// the image data in stored (uncompressed) deflate blocks.
inline std::vector<std::byte> makePalettePng(int width, int height, std::span<const std::array<std::uint8_t, 3>> palette,
                                             std::span<const std::uint8_t> alpha,
                                             std::span<const std::uint8_t> indices) {
    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto be32 = [](std::vector<std::uint8_t>& v, std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            v.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    };
    const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
        be32(out, static_cast<std::uint32_t>(data.size()));
        std::vector<std::uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        out.insert(out.end(), body.begin(), body.end());
        be32(out, crc32(body));
    };
    std::vector<std::uint8_t> header;
    be32(header, static_cast<std::uint32_t>(width));
    be32(header, static_cast<std::uint32_t>(height));
    header.insert(header.end(), {8, 3, 0, 0, 0});
    chunk("IHDR", header);
    std::vector<std::uint8_t> plte;
    for (const auto& entry : palette) {
        plte.insert(plte.end(), entry.begin(), entry.end());
    }
    chunk("PLTE", plte);
    if (!alpha.empty()) {
        chunk("tRNS", std::vector<std::uint8_t>(alpha.begin(), alpha.end()));
    }
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < height; ++y) {
        raw.push_back(0); // filter: none
        const auto row = indices.subspan(static_cast<std::size_t>(y) * static_cast<std::size_t>(width),
                                         static_cast<std::size_t>(width));
        raw.insert(raw.end(), row.begin(), row.end());
    }
    std::vector<std::uint8_t> zlib = {0x78, 0x01};
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t value : raw) {
        a = (a + value) % 65521U;
        b = (b + a) % 65521U;
    }
    for (std::size_t at = 0; at < raw.size(); at += 65535) {
        const std::size_t length = std::min<std::size_t>(65535, raw.size() - at);
        zlib.push_back(at + length == raw.size() ? 1 : 0);
        zlib.push_back(static_cast<std::uint8_t>(length));
        zlib.push_back(static_cast<std::uint8_t>(length >> 8));
        zlib.push_back(static_cast<std::uint8_t>(~length));
        zlib.push_back(static_cast<std::uint8_t>(~length >> 8));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(at),
                    raw.begin() + static_cast<std::ptrdiff_t>(at + length));
    }
    be32(zlib, (b << 16) | a);
    chunk("IDAT", zlib);
    chunk("IEND", {});
    std::vector<std::byte> bytes(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        bytes[i] = static_cast<std::byte>(out[i]);
    }
    return bytes;
}

/// The bundled fonts, loaded once per test executable through the registry.
struct BundledFonts {
    assets::FontFace title;
    assets::FontFace detail;
};

inline const BundledFonts& bundledFonts() {
    static const BundledFonts fonts = [] {
        auto title = assets::FontFace::loadBundled(kAssetRoot, assets::kLabelTitleFontPath);
        auto detail = assets::FontFace::loadBundled(kAssetRoot, assets::kLabelDetailFontPath);
        REQUIRE(title.has_value());
        REQUIRE(detail.has_value());
        return BundledFonts{std::move(*title), std::move(*detail)};
    }();
    return fonts;
}

inline std::string pixelHash(const assets::ImageData& image) {
    return core::sha256Hex(std::span<const std::byte>(reinterpret_cast<const std::byte*>(image.pixels.data()),
                                                      image.pixels.size()));
}

} // namespace rv::test::covers
