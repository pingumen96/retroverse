// I3 step 5: sRGB codes and mip chains in linear light (rv/assets/mip_chain.hpp, ADR-0011 §6).

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include <rv/assets/mip_chain.hpp>
#include <rv/core/math.hpp>

#include "cover_test_support.hpp"

using namespace rv::assets;
using rv::test::covers::blankImage;

namespace {

ImageData checker(int width, int height) {
    ImageData image = blankImage(width, height, 4, 255);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint8_t value = ((x + y) % 2 == 0) ? 255 : 0;
            std::uint8_t* pixel = image.pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            pixel[0] = pixel[1] = pixel[2] = value;
        }
    }
    return image;
}

} // namespace

TEST_CASE("assets/mip_chain: every sRGB code round-trips through linear light", "[assets][mip_chain]") {
    for (int code = 0; code < 256; ++code) {
        const auto value = static_cast<std::uint8_t>(code);
        CHECK(linearToSrgbByte(srgbByteToLinear(value)) == value);
        // The table agrees with the formula of rv_core.
        CHECK(std::abs(srgbByteToLinear(value) - rv::core::srgbToLinear(code / 255.0F)) < 1e-6F);
    }
    CHECK(linearToSrgbByte(-1.0F) == 0);
    CHECK(linearToSrgbByte(2.0F) == 255);
    CHECK(linearToSrgbByte(std::nanf("")) == 0);
    // Half the light reads as 188 (187.5 rounds up), as the tonemap self-check expects.
    CHECK(linearToSrgbByte(0.5F) == 188);
    CHECK(toHexCode(Rgb8{0x0A, 0xB1, 0xFF}) == "#0AB1FF");
}

TEST_CASE("assets/mip_chain: level counts and sides follow OpenGL's", "[assets][mip_chain]") {
    CHECK(mipLevelCount(256, 188) == 9);
    CHECK(mipLevelCount(40, 256) == 9);
    CHECK(mipLevelCount(128, 94) == 8);
    CHECK(mipLevelCount(20, 128) == 8);
    CHECK(mipLevelCount(1, 1) == 1);
    CHECK(mipLevelCount(0, 4) == 0);
    CHECK(mipLevelSide(188, 2) == 47);
    CHECK(mipLevelSide(188, 3) == 23);
    CHECK(mipLevelSide(40, 6) == 1);

    const std::vector<ImageData> chain = buildMipChain(checker(256, 188));
    REQUIRE(chain.size() == 9);
    const int expected[][2] = {{256, 188}, {128, 94}, {64, 47}, {32, 23}, {16, 11}, {8, 5}, {4, 2}, {2, 1}, {1, 1}};
    for (std::size_t level = 0; level < chain.size(); ++level) {
        CHECK(chain[level].width == expected[level][0]);
        CHECK(chain[level].height == expected[level][1]);
        CHECK(chain[level].channels == 4);
    }
    CHECK(buildMipChain(blankImage(4, 4, 3)).empty()); // RGBA only
}

TEST_CASE("assets/mip_chain: a black and white checker reduces to sRGB 188 on every level", "[assets][mip_chain]") {
    // Odd sides (47 -> 23, 5 -> 2) included: the exact box filter weighs every source pixel by its share.
    const std::vector<ImageData> chain = buildMipChain(checker(256, 188));
    for (std::size_t level = 1; level < chain.size(); ++level) {
        const ImageData& image = chain[level];
        int worst = 0;
        for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
            for (std::size_t c = 0; c < 3; ++c) {
                worst = std::max(worst, std::abs(static_cast<int>(image.pixels[i + c]) - 188));
            }
            CHECK(image.pixels[i + 3] == 255);
        }
        INFO("level " << level << " (" << image.width << "x" << image.height << ")");
        CHECK(worst <= 1);
    }
    // A naive average of the codes would give 128.
}

TEST_CASE("assets/mip_chain: the box filter keeps the mean light and flat colours", "[assets][mip_chain]") {
    ImageData flat = blankImage(47, 23, 4, 255);
    for (std::size_t i = 0; i < flat.pixels.size(); i += 4) {
        flat.pixels[i + 0] = 0x68;
        flat.pixels[i + 1] = 0x3D;
        flat.pixels[i + 2] = 0x42;
    }
    const ImageData reduced = reduceLinear(flat, 23, 11);
    REQUIRE(reduced.isValid());
    for (std::size_t i = 0; i < reduced.pixels.size(); i += 4) {
        CHECK(reduced.pixels[i + 0] == 0x68);
        CHECK(reduced.pixels[i + 1] == 0x3D);
        CHECK(reduced.pixels[i + 2] == 0x42);
    }

    // A white column at the left edge of a 3-wide image: in 1 pixel it weighs a third of the light.
    ImageData column = blankImage(3, 1, 4, 255);
    for (std::size_t i = 4; i < column.pixels.size(); i += 4) {
        column.pixels[i + 0] = column.pixels[i + 1] = column.pixels[i + 2] = 0;
    }
    const ImageData one = reduceLinear(column, 1, 1);
    REQUIRE(one.isValid());
    CHECK(one.pixels[0] == linearToSrgbByte(1.0F / 3.0F));

    CHECK_FALSE(reduceLinear(flat, 48, 11).isValid()); // never enlarges
    CHECK_FALSE(reduceLinear(flat, 0, 11).isValid());
}

TEST_CASE("assets/mip_chain: chains are deterministic", "[assets][mip_chain]") {
    const ImageData cover = rv::test::covers::makeCover(256, 188, 7, 4);
    const std::vector<ImageData> first = buildMipChain(cover);
    const std::vector<ImageData> second = buildMipChain(cover);
    REQUIRE(first.size() == second.size());
    for (std::size_t level = 0; level < first.size(); ++level) {
        CHECK(first[level].pixels == second[level].pixels);
    }
    CHECK(first[0].pixels == cover.pixels);
}
