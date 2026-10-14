#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "support/temp_dir.hpp"
#include <rv/assets/image_io.hpp>
#include <rv/core/file_io.hpp>

using rv::ErrorCode;
using rv::assets::ImageData;

namespace {

/// Every byte different, so any reordering of rows, columns or channels is detected.
ImageData makeGradient(int width, int height, int channels) {
    ImageData image;
    image.width = width;
    image.height = height;
    image.channels = channels;
    image.pixels.resize(static_cast<std::size_t>(width * height * channels));
    for (std::size_t i = 0; i < image.pixels.size(); ++i) {
        image.pixels[i] = static_cast<std::uint8_t>((i * 37 + 11) % 251);
    }
    return image;
}

/// Top half white, bottom half black (RGB).
ImageData makeTopWhiteBottomBlack(int width, int height) {
    ImageData image;
    image.width = width;
    image.height = height;
    image.channels = 3;
    image.pixels.assign(static_cast<std::size_t>(width * height * 3), std::uint8_t{0});
    for (int y = 0; y < height / 2; ++y) {
        for (int x = 0; x < width * 3; ++x) {
            image.pixels[static_cast<std::size_t>(y * width * 3 + x)] = 255;
        }
    }
    return image;
}

std::uint8_t at(const ImageData& image, int x, int y, int channel) {
    return image
        .pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) *
                    static_cast<std::size_t>(image.channels) +
                static_cast<std::size_t>(channel)];
}

void appendBigEndian32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

/// PNG signature, IHDR chunk and an empty IDAT chunk header (stb does not check CRCs): enough for the header probe,
/// which stops at the first IDAT.
std::vector<std::byte> pngHeaderOnly(std::uint32_t width, std::uint32_t height) {
    std::vector<std::byte> bytes;
    for (const int b : {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}) { // \x89 "PNG" \r \n \x1A \n
        bytes.push_back(static_cast<std::byte>(b));
    }
    appendBigEndian32(bytes, 13);
    for (const char c : {'I', 'H', 'D', 'R'}) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    appendBigEndian32(bytes, width);
    appendBigEndian32(bytes, height);
    for (const int b : {8, 6, 0, 0, 0}) { // 8 bits, RGBA, deflate, adaptive filter, no interlace
        bytes.push_back(static_cast<std::byte>(b));
    }
    appendBigEndian32(bytes, 0); // CRC (unchecked)
    appendBigEndian32(bytes, 0); // IDAT length
    for (const char c : {'I', 'D', 'A', 'T'}) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    appendBigEndian32(bytes, 0); // CRC
    return bytes;
}

} // namespace

TEST_CASE("assets/image_io: PNG round trip keeps size, channels, pixels and row order", "[assets][image_io]") {
    for (int channels = 1; channels <= 4; ++channels) {
        CAPTURE(channels);
        const ImageData original = makeGradient(7, 5, channels);
        const auto png = rv::assets::encodePng(original);
        REQUIRE(png.has_value());
        REQUIRE(png->size() > 8);
        CHECK((*png)[1] == std::byte{'P'});

        const auto decoded = rv::assets::decodeImage(*png, 0, "round-trip");
        REQUIRE(decoded.has_value());
        CHECK(decoded->width == 7);
        CHECK(decoded->height == 5);
        CHECK(decoded->channels == channels);
        CHECK(decoded->isValid());
        CHECK(decoded->pixels == original.pixels);
    }
}

TEST_CASE("assets/image_io: encoding is deterministic", "[assets][image_io]") {
    const ImageData image = makeGradient(16, 9, 4);
    CHECK(rv::assets::encodePng(image).value() == rv::assets::encodePng(image).value());
}

TEST_CASE("assets/image_io: writePng and loadImage round trip through a Unicode path", "[assets][image_io]") {
    rv::test::TempDir dir;
    // The sources are compiled with /utf-8, so this literal is UTF-8 (TempDir::file expects UTF-8).
    const auto path = dir.file("textures/café_テスト/cover.png");
    const ImageData original = makeGradient(4, 3, 3);
    REQUIRE(rv::assets::writePng(path, original).has_value());
    CHECK(std::filesystem::exists(path));

    const auto loaded = rv::assets::loadImage(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->pixels == original.pixels);

    const auto rgba = rv::assets::loadImage(path, 4);
    REQUIRE(rgba.has_value());
    CHECK(rgba->channels == 4);
    CHECK(at(*rgba, 0, 0, 3) == 255);
    CHECK(at(*rgba, 2, 1, 1) == at(original, 2, 1, 1));

    const auto missing = rv::assets::loadImage(dir.file("missing.png"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("assets/image_io: JPEG decode keeps the top row at the top", "[assets][image_io]") {
    // Encoded with stb_image_write (rv::assets::encodeJpeg), decoded with stb_image.
    const ImageData original = makeTopWhiteBottomBlack(32, 16);
    const auto jpeg = rv::assets::encodeJpeg(original, 95);
    REQUIRE(jpeg.has_value());
    REQUIRE(jpeg->size() > 4);
    CHECK((*jpeg)[0] == std::byte{0xFF});
    CHECK((*jpeg)[1] == std::byte{0xD8});

    const auto decoded = rv::assets::decodeImage(*jpeg, 0, "photo.jpg");
    REQUIRE(decoded.has_value());
    CHECK(decoded->width == 32);
    CHECK(decoded->height == 16);
    CHECK(decoded->channels == 3);
    CHECK(at(*decoded, 3, 1, 0) > 230); // top: white (lossy, so a tolerance)
    CHECK(at(*decoded, 3, 14, 0) < 25); // bottom: black
    CHECK(at(*decoded, 28, 2, 2) > 230);

    const auto grey = rv::assets::decodeImage(*jpeg, 1, "photo.jpg");
    REQUIRE(grey.has_value());
    CHECK(grey->channels == 1);
    CHECK(grey->pixels.size() == 32u * 16u);

    CHECK(rv::assets::encodeJpeg(original, 0).error().code == ErrorCode::InvalidArgument);
    CHECK(rv::assets::encodeJpeg(original, 101).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("assets/image_io: requested channel counts are honored", "[assets][image_io]") {
    ImageData rgb;
    rgb.width = 2;
    rgb.height = 1;
    rgb.channels = 3;
    rgb.pixels = {255, 0, 0, 0, 0, 255};
    const auto png = rv::assets::encodePng(rgb).value();

    const auto rgba = rv::assets::decodeImage(png, 4, "rgb.png").value();
    CHECK(rgba.channels == 4);
    CHECK(rgba.pixels == std::vector<std::uint8_t>{255, 0, 0, 255, 0, 0, 255, 255});

    const auto grey = rv::assets::decodeImage(png, 1, "rgb.png").value();
    CHECK(grey.channels == 1);
    REQUIRE(grey.pixels.size() == 2);
    CHECK(grey.pixels[0] > grey.pixels[1]); // red is brighter than blue

    const auto badChannels = rv::assets::decodeImage(png, 5, "rgb.png");
    REQUIRE_FALSE(badChannels.has_value());
    CHECK(badChannels.error().code == ErrorCode::InvalidArgument);
    CHECK(rv::assets::decodeImage(png, -1, "rgb.png").error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("assets/image_io: convertChannels", "[assets][image_io]") {
    ImageData rgba;
    rgba.width = 2;
    rgba.height = 1;
    rgba.channels = 4;
    rgba.pixels = {255, 255, 255, 10, 255, 0, 0, 200};

    const ImageData rgb = rv::assets::convertChannels(rgba, 3);
    CHECK(rgb.channels == 3);
    CHECK(rgb.pixels == std::vector<std::uint8_t>{255, 255, 255, 255, 0, 0});

    const ImageData greyAlpha = rv::assets::convertChannels(rgba, 2);
    CHECK(greyAlpha.pixels == std::vector<std::uint8_t>{255, 10, 76, 200}); // (255 * 77) >> 8 = 76

    const ImageData grey = rv::assets::convertChannels(rgba, 1);
    CHECK(grey.pixels == std::vector<std::uint8_t>{255, 76});

    const ImageData expanded = rv::assets::convertChannels(grey, 4);
    CHECK(expanded.pixels == std::vector<std::uint8_t>{255, 255, 255, 255, 76, 76, 76, 255});

    const ImageData expandedAlpha = rv::assets::convertChannels(greyAlpha, 4);
    CHECK(expandedAlpha.pixels == std::vector<std::uint8_t>{255, 255, 255, 10, 76, 76, 76, 200});

    const ImageData greyToRgb = rv::assets::convertChannels(grey, 3);
    CHECK(greyToRgb.pixels == std::vector<std::uint8_t>{255, 255, 255, 76, 76, 76});

    const ImageData copy = rv::assets::convertChannels(rgba, 4);
    CHECK(copy.pixels == rgba.pixels);

    CHECK_FALSE(rv::assets::convertChannels(rgba, 0).isValid());
    CHECK_FALSE(rv::assets::convertChannels(rgba, 5).isValid());
    CHECK_FALSE(rv::assets::convertChannels(ImageData{}, 3).isValid());
}

TEST_CASE("assets/image_io: flipRowsInPlace reverses the row order", "[assets][image_io]") {
    for (const int height : {1, 2, 3, 4}) {
        CAPTURE(height);
        const ImageData original = makeGradient(3, height, 2);
        ImageData flipped = original;
        rv::assets::flipRowsInPlace(flipped);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < 3; ++x) {
                CHECK(at(flipped, x, y, 0) == at(original, x, height - 1 - y, 0));
                CHECK(at(flipped, x, y, 1) == at(original, x, height - 1 - y, 1));
            }
        }
        rv::assets::flipRowsInPlace(flipped);
        CHECK(flipped.pixels == original.pixels);
    }
    ImageData invalid;
    invalid.width = 4;
    invalid.height = 4;
    invalid.channels = 4;
    invalid.pixels = {1, 2, 3};
    rv::assets::flipRowsInPlace(invalid);
    CHECK(invalid.pixels == std::vector<std::uint8_t>{1, 2, 3});
}

TEST_CASE("assets/image_io: corrupt and truncated data are clear errors", "[assets][image_io]") {
    const auto empty = rv::assets::decodeImage({}, 0, "empty.png");
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == ErrorCode::InvalidArgument);
    CHECK(empty.error().context == "empty.png");

    std::vector<std::byte> garbage(64);
    for (std::size_t i = 0; i < garbage.size(); ++i) {
        garbage[i] = static_cast<std::byte>((i * 73 + 5) & 0xFF);
    }
    const auto unknown = rv::assets::decodeImage(garbage, 0, "garbage.bin");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == ErrorCode::Unsupported);

    const auto png = rv::assets::encodePng(makeGradient(32, 32, 4)).value();
    std::vector<std::byte> corrupt = png;
    for (std::size_t i = 40; i < corrupt.size(); ++i) {
        corrupt[i] = std::byte{0x5A};
    }
    const auto corrupted = rv::assets::decodeImage(corrupt, 0, "corrupt.png");
    REQUIRE_FALSE(corrupted.has_value());
    CHECK(corrupted.error().code == ErrorCode::ParseError);
    CHECK(corrupted.error().message.find("PNG") != std::string::npos);

    const std::vector<std::byte> truncatedPng(png.begin(), png.begin() + static_cast<std::ptrdiff_t>(png.size() / 2));
    const auto truncated = rv::assets::decodeImage(truncatedPng, 0, "truncated.png");
    REQUIRE_FALSE(truncated.has_value());
    CHECK(truncated.error().code == ErrorCode::ParseError);

    const std::vector<std::byte> signatureOnly(png.begin(), png.begin() + 8);
    CHECK(rv::assets::decodeImage(signatureOnly, 0, "header.png").error().code == ErrorCode::ParseError);

    const auto jpeg = rv::assets::encodeJpeg(makeTopWhiteBottomBlack(64, 64), 90).value();
    const std::vector<std::byte> truncatedJpeg(jpeg.begin(),
                                               jpeg.begin() + static_cast<std::ptrdiff_t>(jpeg.size() * 2 / 3));
    const auto truncatedJpegResult = rv::assets::decodeImage(truncatedJpeg, 0, "truncated.jpg");
    REQUIRE_FALSE(truncatedJpegResult.has_value());
    CHECK(truncatedJpegResult.error().code == ErrorCode::ParseError);
    CHECK(truncatedJpegResult.error().message.find("truncated") != std::string::npos);
}

TEST_CASE("assets/image_io: images larger than 16384 pixels per side are rejected", "[assets][image_io]") {
    const auto wide = rv::assets::decodeImage(pngHeaderOnly(20000, 10), 0, "wide.png");
    REQUIRE_FALSE(wide.has_value());
    CHECK(wide.error().code == ErrorCode::OutOfRange);
    CHECK(wide.error().message.find("16384") != std::string::npos);
    CHECK(rv::assets::decodeImage(pngHeaderOnly(10, 16385), 0, "tall.png").error().code == ErrorCode::OutOfRange);

    ImageData tooWide;
    tooWide.width = rv::assets::kMaxImageDimension + 1;
    tooWide.height = 1;
    tooWide.channels = 1;
    tooWide.pixels.assign(static_cast<std::size_t>(tooWide.width), std::uint8_t{128});
    CHECK(rv::assets::encodePng(tooWide).error().code == ErrorCode::OutOfRange);

    ImageData invalid;
    invalid.width = 2;
    invalid.height = 2;
    invalid.channels = 3;
    invalid.pixels = {1, 2, 3};
    CHECK(rv::assets::encodePng(invalid).error().code == ErrorCode::InvalidArgument);
    rv::test::TempDir dir;
    const auto written = rv::assets::writePng(dir.file("invalid.png"), invalid);
    REQUIRE_FALSE(written.has_value());
    CHECK_FALSE(std::filesystem::exists(dir.file("invalid.png")));
}
