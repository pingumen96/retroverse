// I3 step 5: BC1 encoding with stb_dxt and the reference decoder (rv/assets/bc1.hpp, ADR-0011 §4).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

#include <rv/assets/bc1.hpp>
#include <rv/assets/cover_fit.hpp>
#include <rv/assets/mip_chain.hpp>

#include "cover_test_support.hpp"

using namespace rv::assets;
using rv::test::covers::blankImage;
using rv::test::covers::makeCover;

namespace {

std::vector<std::byte> block(std::array<std::uint8_t, 8> bytes) {
    std::vector<std::byte> out;
    for (const std::uint8_t value : bytes) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

const std::uint8_t* pixelAt(const ImageData& image, int x, int y) {
    return image.pixels.data() + (static_cast<std::size_t>(y) * image.width + x) * image.channels;
}

} // namespace

TEST_CASE("assets/bc1: level sizes are whole blocks", "[assets][bc1]") {
    CHECK(bc1BlocksAlong(256) == 64);
    CHECK(bc1BlocksAlong(94) == 24);
    CHECK(bc1BlocksAlong(47) == 12);
    CHECK(bc1BlocksAlong(1) == 1);
    CHECK(bc1BlocksAlong(0) == 0);
    CHECK(bc1LevelBytes(256, 188) == 64 * 47 * 8);
    CHECK(bc1LevelBytes(5, 2) == 2 * 1 * 8);
    CHECK(bc1LevelBytes(1, 1) == 8);
}

TEST_CASE("assets/bc1: the reference decoder follows the S3TC rules", "[assets][bc1]") {
    // Four-colour block: c0 = pure red (0xF800) > c1 = pure blue (0x001F); indices 0, 1, 2, 3 along the first row.
    const ImageData four = decodeBc1(block({0x00, 0xF8, 0x1F, 0x00, 0b11100100, 0, 0, 0}), 4, 4);
    REQUIRE(four.isValid());
    const std::array<std::array<int, 4>, 4> expectedFour = {{
        {255, 0, 0, 255},
        {0, 0, 255, 255},
        {170, 0, 85, 255}, // (2 c0 + c1) / 3
        {85, 0, 170, 255}, // (c0 + 2 c1) / 3
    }};
    for (int x = 0; x < 4; ++x) {
        for (int c = 0; c < 4; ++c) {
            CHECK(pixelAt(four, x, 0)[c] == expectedFour[static_cast<std::size_t>(x)][static_cast<std::size_t>(c)]);
        }
    }
    CHECK(pixelAt(four, 0, 3)[0] == 255); // index 0 elsewhere

    // Three-colour block: c0 <= c1, index 2 the middle, index 3 transparent black.
    const ImageData three = decodeBc1(block({0x1F, 0x00, 0x00, 0xF8, 0b11100100, 0, 0, 0}), 4, 4);
    REQUIRE(three.isValid());
    CHECK(pixelAt(three, 2, 0)[0] == 127);
    CHECK(pixelAt(three, 2, 0)[2] == 127);
    CHECK(pixelAt(three, 3, 0)[0] == 0);
    CHECK(pixelAt(three, 3, 0)[3] == 0);

    // 5:6:5 expanded by bit replication: green 0x07E0 is 255, 0x0020 is 4.
    CHECK(pixelAt(decodeBc1(block({0xE0, 0x07, 0x20, 0x00, 0, 0, 0, 0}), 1, 1), 0, 0)[1] == 255);
    CHECK(pixelAt(decodeBc1(block({0xE0, 0x07, 0x20, 0x00, 1, 0, 0, 0}), 1, 1), 0, 0)[1] == 4);

    CHECK_FALSE(decodeBc1(block({0, 0, 0, 0, 0, 0, 0, 0}), 8, 4).isValid()); // the wrong length
}

TEST_CASE("assets/bc1: flat colours and two-colour blocks survive within the 5:6:5 quantization", "[assets][bc1]") {
    ImageData flat = blankImage(8, 8, 4, 255);
    for (std::size_t i = 0; i < flat.pixels.size(); i += 4) {
        flat.pixels[i + 0] = 0x68;
        flat.pixels[i + 1] = 0x3D;
        flat.pixels[i + 2] = 0x42;
    }
    const ImageData flatBack = decodeBc1(encodeBc1(flat), 8, 8);
    REQUIRE(flatBack.isValid());
    for (std::size_t i = 0; i < flatBack.pixels.size(); i += 4) {
        // stb_dxt's single-colour tables pick endpoints whose interpolated colour matches within one code.
        CHECK(std::abs(flatBack.pixels[i + 0] - 0x68) <= 1);
        CHECK(std::abs(flatBack.pixels[i + 1] - 0x3D) <= 1);
        CHECK(std::abs(flatBack.pixels[i + 2] - 0x42) <= 1);
    }

    // Black text on white: the two colours are the endpoints, exactly.
    ImageData text = blankImage(4, 4, 4, 255);
    for (int y = 0; y < 4; ++y) {
        std::uint8_t* pixel = text.pixels.data() + (static_cast<std::size_t>(y) * 4 + 1) * 4;
        pixel[0] = pixel[1] = pixel[2] = 0;
    }
    const ImageData textBack = decodeBc1(encodeBc1(text), 4, 4);
    CHECK(textBack.pixels == text.pixels);
}

TEST_CASE("assets/bc1: levels whose sides are not multiples of 4 repeat their edge pixels", "[assets][bc1]") {
    // A 5 x 3 image: the right column is red, the rest white. The second block column holds only that red column,
    // repeated: it decodes to red (the padding never mixes white into it).
    ImageData image = blankImage(5, 3, 4, 255);
    for (int y = 0; y < 3; ++y) {
        std::uint8_t* pixel = image.pixels.data() + (static_cast<std::size_t>(y) * 5 + 4) * 4;
        pixel[1] = pixel[2] = 0;
    }
    const std::vector<std::byte> blocks = encodeBc1(image);
    REQUIRE(blocks.size() == 2 * 8);
    const ImageData back = decodeBc1(blocks, 5, 3);
    REQUIRE(back.isValid());
    CHECK(back.pixels == image.pixels);
    // Decoded as a whole 4 x 4 block, the padding holds the edge's colour too.
    const ImageData padded = decodeBc1(std::span<const std::byte>(blocks).subspan(8, 8), 4, 4);
    for (std::size_t i = 0; i < padded.pixels.size(); i += 4) {
        CHECK(padded.pixels[i + 0] == 255);
        CHECK(padded.pixels[i + 1] == 0);
    }
}

TEST_CASE("assets/bc1: generated covers keep at least 32 dB against RGBA8", "[assets][bc1]") {
    // ADR-0011 §4 and the roadmap's row 5: BC1 against RGBA8 on the fixtures' covers (original procedural pictures:
    // gradients, flat shapes, fine bars and noise, like step 1c's generator).
    double worst = std::numeric_limits<double>::infinity();
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        const bool portrait = seed % 4 == 0;
        const ImageData source = makeCover(portrait ? 476 : 680, portrait ? 864 : 497, seed);
        auto fitted = fitCover(source, 256, 188);
        REQUIRE(fitted.has_value());
        const ImageData& reference = fitted->picture;
        const ImageData encoded = decodeBc1(encodeBc1(reference), reference.width, reference.height);
        const double psnr = psnrRgb(reference, encoded);
        INFO("seed " << seed << ": " << psnr << " dB");
        CHECK(psnr >= 32.0);
        worst = std::min(worst, psnr);
    }
    INFO("worst of 12 generated covers: " << worst << " dB");
    CHECK(worst >= 32.0);
    CHECK(psnrRgb(blankImage(4, 4, 4), blankImage(4, 4, 4)) == std::numeric_limits<double>::infinity());
    CHECK(psnrRgb(blankImage(4, 4, 4), blankImage(4, 5, 4)) == 0.0);
}

TEST_CASE("assets/bc1: encoding is deterministic and both qualities decode", "[assets][bc1]") {
    const ImageData cover = makeCover(64, 47, 3, 4);
    CHECK(encodeBc1(cover) == encodeBc1(cover));
    const ImageData normal = decodeBc1(encodeBc1(cover, Bc1Quality::Normal), 64, 47);
    const ImageData high = decodeBc1(encodeBc1(cover, Bc1Quality::High), 64, 47);
    CHECK(psnrRgb(cover, high) >= psnrRgb(cover, normal) - 0.05);
    CHECK(encodeBc1(blankImage(4, 4, 3)).empty()); // RGBA only
}
