// The EXIF orientation of the user's pictures (the fix of 2026-10-29): exifOrientation(), orientImage() and
// decodeUprightImage() on PNG and JPEG files generated here, one per orientation tag (never a user's image).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include <rv/assets/cover_fit.hpp>
#include <rv/assets/image_io.hpp>

#include "cover_test_support.hpp"

using rv::assets::ImageData;

namespace {

using Pixel = std::array<std::uint8_t, 3>;

/// An RGB image whose every pixel differs (its index in its red and green bytes).
ImageData distinctPixels(int width, int height) {
    ImageData image;
    image.width = width;
    image.height = height;
    image.channels = 3;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.pixels.push_back(static_cast<std::uint8_t>(x * 16 + 8));
            image.pixels.push_back(static_cast<std::uint8_t>(y * 16 + 8));
            image.pixels.push_back(static_cast<std::uint8_t>(200));
        }
    }
    return image;
}

/// Four flat quadrants (red, green / blue, white), JPEG-proof.
ImageData quadrants(int width, int height) {
    static constexpr Pixel kColours[4] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
    ImageData image;
    image.width = width;
    image.height = height;
    image.channels = 3;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Pixel& colour = kColours[(y < height / 2 ? 0 : 2) + (x < width / 2 ? 0 : 1)];
            image.pixels.insert(image.pixels.end(), colour.begin(), colour.end());
        }
    }
    return image;
}

Pixel pixelAt(const ImageData& image, int x, int y) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) *
        static_cast<std::size_t>(image.channels);
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2]};
}

bool near(const Pixel& a, const Pixel& b, int tolerance) {
    for (std::size_t c = 0; c < 3; ++c) {
        if (std::abs(static_cast<int>(a[c]) - static_cast<int>(b[c])) > tolerance) {
            return false;
        }
    }
    return true;
}

/// An edge of the upright picture: horizontal (the top or the bottom) or vertical, and whether it is the far one (the
/// bottom, the right).
struct Edge {
    bool horizontal = true;
    bool far = false;
};
constexpr Edge kTop{true, false};
constexpr Edge kBottom{true, true};
constexpr Edge kLeft{false, false};
constexpr Edge kRight{false, true};

/// EXIF 2.3's table (TIFF 6.0's Orientation): the upright picture's edge that holds the stored 0th row, and the one
/// that holds the stored 0th column, for tags 1 to 8.
struct Placement {
    Edge row0;
    Edge column0;
};
constexpr std::array<Placement, 9> kPlacements = {{
    {},                // unused
    {kTop, kLeft},     // 1
    {kTop, kRight},    // 2
    {kBottom, kRight}, // 3
    {kBottom, kLeft},  // 4
    {kLeft, kTop},     // 5
    {kRight, kTop},    // 6
    {kRight, kBottom}, // 7
    {kLeft, kBottom},  // 8
}};

constexpr Edge opposite(Edge edge) {
    return Edge{edge.horizontal, !edge.far};
}

/// The upright corner where two edges (one horizontal, one vertical) meet, `inset` pixels inside.
std::array<int, 2> corner(Edge a, Edge b, int width, int height, int inset) {
    const Edge horizontal = a.horizontal ? a : b;
    const Edge vertical = a.horizontal ? b : a;
    return {vertical.far ? width - 1 - inset : inset, horizontal.far ? height - 1 - inset : inset};
}

/// A TIFF block with IFD0 holding one Orientation entry.
std::vector<std::uint8_t> tiffWithOrientation(std::uint32_t value, bool littleEndian, std::uint16_t type = 3) {
    std::vector<std::uint8_t> out;
    const auto u16 = [&](std::uint32_t v) {
        if (littleEndian) {
            out.push_back(static_cast<std::uint8_t>(v));
            out.push_back(static_cast<std::uint8_t>(v >> 8));
        } else {
            out.push_back(static_cast<std::uint8_t>(v >> 8));
            out.push_back(static_cast<std::uint8_t>(v));
        }
    };
    const auto u32 = [&](std::uint32_t v) {
        if (littleEndian) {
            u16(v & 0xFFFFu);
            u16(v >> 16);
        } else {
            u16(v >> 16);
            u16(v & 0xFFFFu);
        }
    };
    out.push_back(littleEndian ? 'I' : 'M');
    out.push_back(littleEndian ? 'I' : 'M');
    u16(42);
    u32(8);
    // IFD0: an ImageDescription-like entry first (another tag), then the Orientation.
    u16(2);
    u16(0x010E); // ImageDescription, ASCII, 4 bytes inline: "Ab\0\0"
    u16(2);
    u32(4);
    out.insert(out.end(), {'A', 'b', 0, 0});
    u16(0x0112);
    u16(type);
    u32(1);
    if (type == 3) {
        u16(value);
        u16(0);
    } else {
        u32(value);
    }
    u32(0); // no next IFD
    return out;
}

std::vector<std::byte> withJpegExif(const std::vector<std::byte>& jpeg, const std::vector<std::uint8_t>& tiff) {
    std::vector<std::byte> out(jpeg.begin(), jpeg.begin() + 2); // SOI
    const std::size_t length = 2 + 6 + tiff.size();
    const std::array<int, 10> header = {
        0xFF, 0xE1, static_cast<int>(length >> 8), static_cast<int>(length & 0xFF), 'E', 'x', 'i', 'f', 0, 0};
    for (const int value : header) {
        out.push_back(static_cast<std::byte>(value));
    }
    for (const std::uint8_t value : tiff) {
        out.push_back(static_cast<std::byte>(value));
    }
    out.insert(out.end(), jpeg.begin() + 2, jpeg.end());
    return out;
}

std::vector<std::byte> withPngChunk(const std::vector<std::byte>& png, const char* type,
                                    const std::vector<std::uint8_t>& data, bool afterData = false) {
    // After the signature and IHDR (8 + 25 bytes), or just before IEND (the last 12 bytes).
    const std::size_t at = afterData ? png.size() - 12 : 33;
    std::vector<std::byte> out(png.begin(), png.begin() + static_cast<std::ptrdiff_t>(at));
    const auto be32 = [&](std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::byte>(value >> shift));
        }
    };
    be32(static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    for (const std::uint8_t value : body) {
        out.push_back(static_cast<std::byte>(value));
    }
    be32(rv::test::covers::crc32(body));
    out.insert(out.end(), png.begin() + static_cast<std::ptrdiff_t>(at), png.end());
    return out;
}

/// The stored image's corners (0, 0), (w - 1, 0) and (0, h - 1) stand where EXIF's table puts them once upright.
void checkUpright(const ImageData& stored, const ImageData& upright, int orientation, int inset, int tolerance) {
    const bool swaps = orientation >= 5;
    REQUIRE(upright.width == (swaps ? stored.height : stored.width));
    REQUIRE(upright.height == (swaps ? stored.width : stored.height));
    const Placement& placement = kPlacements[static_cast<std::size_t>(orientation)];
    const auto origin = corner(placement.row0, placement.column0, upright.width, upright.height, inset);
    const auto rowEnd = corner(placement.row0, opposite(placement.column0), upright.width, upright.height, inset);
    const auto columnEnd = corner(opposite(placement.row0), placement.column0, upright.width, upright.height, inset);
    CHECK(near(pixelAt(upright, origin[0], origin[1]), pixelAt(stored, inset, inset), tolerance));
    CHECK(near(pixelAt(upright, rowEnd[0], rowEnd[1]), pixelAt(stored, stored.width - 1 - inset, inset), tolerance));
    CHECK(near(pixelAt(upright, columnEnd[0], columnEnd[1]), pixelAt(stored, inset, stored.height - 1 - inset),
               tolerance));
}

} // namespace

TEST_CASE("assets/image_orientation: orientImage follows EXIF's table for each tag", "[assets][image_orientation]") {
    const ImageData stored = distinctPixels(5, 3);
    const int orientation = GENERATE(1, 2, 3, 4, 5, 6, 7, 8);
    CAPTURE(orientation);
    const ImageData upright = rv::assets::orientImage(stored, orientation);
    checkUpright(stored, upright, orientation, 0, 0);
    // Every pixel is kept once: the transforms are permutations.
    std::vector<std::uint8_t> before = stored.pixels;
    std::vector<std::uint8_t> after = upright.pixels;
    std::sort(before.begin(), before.end());
    std::sort(after.begin(), after.end());
    CHECK(before == after);
}

TEST_CASE("assets/image_orientation: 6 turns clockwise and 8 counter-clockwise", "[assets][image_orientation]") {
    // Stored 2 x 1: a then b. Turned clockwise the picture stands 1 x 2, a on top (a camera held on its right side).
    ImageData stored;
    stored.width = 2;
    stored.height = 1;
    stored.channels = 1;
    stored.pixels = {10, 20};
    const ImageData clockwise = rv::assets::orientImage(stored, 6);
    CHECK(clockwise.width == 1);
    CHECK(clockwise.height == 2);
    CHECK(clockwise.pixels == std::vector<std::uint8_t>{10, 20});
    const ImageData counter = rv::assets::orientImage(stored, 8);
    CHECK(counter.pixels == std::vector<std::uint8_t>{20, 10});
    CHECK(rv::assets::orientImage(stored, 3).pixels == std::vector<std::uint8_t>{20, 10});
    // Out of range or invalid: a copy.
    CHECK(rv::assets::orientImage(stored, 0).pixels == stored.pixels);
    CHECK(rv::assets::orientImage(stored, 9).pixels == stored.pixels);
    CHECK_FALSE(rv::assets::orientImage(ImageData{}, 6).isValid());
}

TEST_CASE("assets/image_orientation: a PNG's eXIf chunk stands each tag upright", "[assets][image_orientation]") {
    const ImageData stored = distinctPixels(6, 4);
    const auto png = rv::assets::encodePng(stored);
    REQUIRE(png.has_value());
    CHECK(rv::assets::exifOrientation(*png) == 1);
    const int orientation = GENERATE(1, 2, 3, 4, 5, 6, 7, 8);
    const bool little = GENERATE(true, false);
    CAPTURE(orientation, little);
    const auto tagged =
        withPngChunk(*png, "eXIf", tiffWithOrientation(static_cast<std::uint32_t>(orientation), little));
    CHECK(rv::assets::exifOrientation(tagged) == orientation);
    // decodeImage() keeps the stored rows; decodeUprightImage() turns them.
    const auto raw = rv::assets::decodeImage(tagged, 0, "tagged.png");
    REQUIRE(raw.has_value());
    CHECK(raw->pixels == stored.pixels);
    const auto upright = rv::assets::decodeUprightImage(tagged, 0, "tagged.png");
    REQUIRE(upright.has_value());
    checkUpright(stored, *upright, orientation, 0, 0);
}

TEST_CASE("assets/image_orientation: a JPEG's EXIF segment stands each tag upright", "[assets][image_orientation]") {
    const ImageData stored = quadrants(48, 32);
    const auto jpeg = rv::assets::encodeJpeg(stored, 95);
    REQUIRE(jpeg.has_value());
    CHECK(rv::assets::exifOrientation(*jpeg) == 1);
    const int orientation = GENERATE(1, 2, 3, 4, 5, 6, 7, 8);
    const bool little = GENERATE(true, false);
    CAPTURE(orientation, little);
    const auto tagged = withJpegExif(*jpeg, tiffWithOrientation(static_cast<std::uint32_t>(orientation), little));
    CHECK(rv::assets::exifOrientation(tagged) == orientation);
    const auto upright = rv::assets::decodeUprightImage(tagged, 0, "tagged.jpg");
    REQUIRE(upright.has_value());
    checkUpright(stored, *upright, orientation, 4, 40);
}

TEST_CASE("assets/image_orientation: a cover's front layer is fitted upright", "[assets][image_orientation]") {
    // A landscape picture stored sideways (tag 6): upright it is portrait, so it fits a portrait label cropped, not
    // banded, and its top (red) is at the layer's top.
    ImageData stored;
    stored.width = 64;
    stored.height = 48;
    stored.channels = 3;
    for (int y = 0; y < stored.height; ++y) {
        for (int x = 0; x < stored.width; ++x) {
            // Upright (turned clockwise) the stored column 0 is the top: red on the stored left half, blue on the
            // right.
            const Pixel colour = x < stored.width / 2 ? Pixel{230, 20, 20} : Pixel{20, 20, 230};
            stored.pixels.insert(stored.pixels.end(), colour.begin(), colour.end());
        }
    }
    const auto png = rv::assets::encodePng(stored);
    REQUIRE(png.has_value());
    const auto tagged = withPngChunk(*png, "eXIf", tiffWithOrientation(6, true));
    rv::assets::FitOptions options;
    options.label = rv::assets::LabelAspect{48, 64};
    const auto layer =
        rv::assets::buildCoverLayer(tagged, rv::assets::TextureFormat::Rgba8Srgb, "tagged cover", options);
    REQUIRE(layer.has_value());
    CHECK(layer->sourceWidth == 48);
    CHECK(layer->sourceHeight == 64);
    CHECK(layer->mode == rv::assets::FitMode::Cropped);
    const rv::assets::TextureLevel& top = layer->texture.levels.front();
    const int width = top.width;
    const int height = top.height;
    const auto texel = [&](int x, int y) {
        const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                static_cast<std::size_t>(x)) * 4;
        return Pixel{std::to_integer<std::uint8_t>(top.data[at]), std::to_integer<std::uint8_t>(top.data[at + 1]),
                     std::to_integer<std::uint8_t>(top.data[at + 2])};
    };
    CHECK(near(texel(width / 2, 4), Pixel{230, 20, 20}, 30));
    CHECK(near(texel(width / 2, height - 5), Pixel{20, 20, 230}, 30));
}

TEST_CASE("assets/image_orientation: missing, unknown or broken tags read as 1", "[assets][image_orientation]") {
    const auto png = rv::assets::encodePng(distinctPixels(4, 4));
    REQUIRE(png.has_value());
    const auto jpeg = rv::assets::encodeJpeg(quadrants(16, 16), 90);
    REQUIRE(jpeg.has_value());
    // A LONG is read leniently; a value outside 1..8 or another type is not an orientation.
    CHECK(rv::assets::exifOrientation(withJpegExif(*jpeg, tiffWithOrientation(6, true, 4))) == 6);
    CHECK(rv::assets::exifOrientation(withJpegExif(*jpeg, tiffWithOrientation(9, true))) == 1);
    CHECK(rv::assets::exifOrientation(withJpegExif(*jpeg, tiffWithOrientation(0, false))) == 1);
    CHECK(rv::assets::exifOrientation(withJpegExif(*jpeg, tiffWithOrientation(6, true, 5))) == 1);
    // A truncated IFD, a bad byte order or magic.
    std::vector<std::uint8_t> truncated = tiffWithOrientation(6, true);
    truncated.resize(20);
    CHECK(rv::assets::exifOrientation(withJpegExif(*jpeg, truncated)) == 1);
    std::vector<std::uint8_t> badOrder = tiffWithOrientation(6, true);
    badOrder[0] = 'X';
    CHECK(rv::assets::exifOrientation(withJpegExif(*jpeg, badOrder)) == 1);
    std::vector<std::uint8_t> badMagic = tiffWithOrientation(6, true);
    badMagic[2] = 41;
    CHECK(rv::assets::exifOrientation(withJpegExif(*jpeg, badMagic)) == 1);
    // An eXIf after the image data is not read (it must come before IDAT).
    CHECK(rv::assets::exifOrientation(withPngChunk(*png, "eXIf", tiffWithOrientation(6, true), true)) == 1);
    // Neither PNG nor JPEG, too short, empty.
    const std::vector<std::byte> junk(16, std::byte{0x42});
    CHECK(rv::assets::exifOrientation(junk) == 1);
    CHECK(rv::assets::exifOrientation(std::span<const std::byte>(png->data(), 6)) == 1);
    CHECK(rv::assets::exifOrientation({}) == 1);
    // A JPEG cut inside its EXIF segment.
    const auto tagged = withJpegExif(*jpeg, tiffWithOrientation(6, true));
    CHECK(rv::assets::exifOrientation(std::span<const std::byte>(tagged.data(), 20)) == 1);
}
