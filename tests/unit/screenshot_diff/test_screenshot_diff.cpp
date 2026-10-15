#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>

#include <rv/assets/image_io.hpp>
#include <rv/screenshot_diff/screenshot_diff.hpp>

#include "support/temp_dir.hpp"

using rv::ErrorCode;
using rv::assets::ImageData;
using rv::screenshot_diff::Tolerance;

namespace {

/// Index of the first channel of pixel (x, y).
std::size_t offsetOf(const ImageData& image, int x, int y) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) *
           static_cast<std::size_t>(image.channels);
}

/// A width x height image of `channels` channels filled with a gradient, so that every pixel differs from its
/// neighbors.
ImageData gradient(int width, int height, int channels = 3) {
    ImageData image;
    image.width = width;
    image.height = height;
    image.channels = channels;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                        static_cast<std::size_t>(channels));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint8_t* pixel = &image.pixels[offsetOf(image, x, y)];
            for (int c = 0; c < channels; ++c) {
                pixel[c] = static_cast<std::uint8_t>(c == 3 ? 255 : (x * 7 + y * 13 + c * 50) % 200 + 20);
            }
        }
    }
    return image;
}

/// Adds `delta` to channel `channel` of pixel (x, y).
void nudge(ImageData& image, int x, int y, int channel, int delta) {
    std::uint8_t& value = image.pixels[offsetOf(image, x, y) + static_cast<std::size_t>(channel)];
    value = static_cast<std::uint8_t>(static_cast<int>(value) + delta);
}

const std::uint8_t* pixelAt(const ImageData& image, int x, int y) {
    return &image.pixels[offsetOf(image, x, y)];
}

} // namespace

TEST_CASE("screenshot_diff: identical images match without a changed pixel", "[screenshot_diff]") {
    const ImageData image = gradient(40, 30);
    const auto comparison = rv::screenshot_diff::compareImages(image, image, Tolerance{});
    REQUIRE(comparison.has_value());
    CHECK(comparison->matches);
    CHECK(comparison->sameSize());
    CHECK(comparison->pixels == 1200);
    CHECK(comparison->differingPixels == 0);
    CHECK(comparison->changedPixels == 0);
    CHECK(comparison->maxChannelDifference == 0);
    CHECK(comparison->differingMin == comparison->differingMax);
    CHECK(comparison->describe(Tolerance{}).find("0 of 1200 pixels") == 0);
}

TEST_CASE("screenshot_diff: a pixel differs when one channel moves beyond the threshold", "[screenshot_diff]") {
    const ImageData expected = gradient(10, 10);
    ImageData actual = expected;
    nudge(actual, 2, 3, 1, 6);  // at the threshold: changed, not differing
    nudge(actual, 7, 5, 2, -7); // beyond it
    nudge(actual, 4, 8, 0, 30);
    const auto comparison = rv::screenshot_diff::compareImages(expected, actual, Tolerance{6, 100.0});
    REQUIRE(comparison.has_value());
    CHECK(comparison->changedPixels == 3);
    CHECK(comparison->differingPixels == 2);
    CHECK(comparison->maxChannelDifference == 30);
    CHECK(comparison->differingMin == glm::ivec2(4, 5));
    CHECK(comparison->differingMax == glm::ivec2(8, 9));
    CHECK(comparison->differingPercent() == 2.0);
    CHECK(comparison->matches);
}

TEST_CASE("screenshot_diff: the images match up to the allowed share of differing pixels", "[screenshot_diff]") {
    // 1000 pixels: 0.5% is 5 of them.
    const ImageData expected = gradient(100, 10);
    ImageData actual = expected;
    for (int x = 0; x < 5; ++x) {
        nudge(actual, x * 10, 4, 0, 20);
    }
    const auto five = rv::screenshot_diff::compareImages(expected, actual, Tolerance{});
    REQUIRE(five.has_value());
    CHECK(five->differingPixels == 5);
    CHECK(five->matches);

    nudge(actual, 99, 9, 2, -20);
    const auto six = rv::screenshot_diff::compareImages(expected, actual, Tolerance{});
    REQUIRE(six.has_value());
    CHECK(six->differingPixels == 6);
    CHECK_FALSE(six->matches);
    CHECK(six->describe(Tolerance{}).find("MISMATCH") != std::string::npos);

    // A zero tolerance: every change differs.
    nudge(actual, 50, 5, 1, 1);
    const auto exact = rv::screenshot_diff::compareImages(expected, actual, Tolerance{0, 0.0});
    REQUIRE(exact.has_value());
    CHECK(exact->differingPixels == 7);
    CHECK_FALSE(exact->matches);
}

TEST_CASE("screenshot_diff: alpha is ignored and grey compares as RGB", "[screenshot_diff]") {
    const ImageData rgb = gradient(8, 8, 3);
    ImageData rgba = gradient(8, 8, 4);
    for (std::size_t i = 3; i < rgba.pixels.size(); i += 4) {
        rgba.pixels[i] = static_cast<std::uint8_t>(i % 256); // any alpha
    }
    const auto mixed = rv::screenshot_diff::compareImages(rgb, rgba, Tolerance{0, 0.0});
    REQUIRE(mixed.has_value());
    CHECK(mixed->matches);

    const ImageData grey = rv::assets::convertChannels(rgb, 1);
    const ImageData expanded = rv::assets::convertChannels(grey, 3);
    const auto greyVsRgb = rv::screenshot_diff::compareImages(grey, expanded, Tolerance{0, 0.0});
    REQUIRE(greyVsRgb.has_value());
    CHECK(greyVsRgb->matches);
}

TEST_CASE("screenshot_diff: images of different sizes never match", "[screenshot_diff]") {
    const auto comparison =
        rv::screenshot_diff::compareImages(gradient(16, 9), gradient(9, 16), Tolerance{255, 100.0});
    REQUIRE(comparison.has_value());
    CHECK_FALSE(comparison->sameSize());
    CHECK_FALSE(comparison->matches);
    CHECK(comparison->pixels == 0);
    CHECK(comparison->describe(Tolerance{}) == "the sizes differ: expected 16x9, actual 9x16");
}

TEST_CASE("screenshot_diff: invalid images and tolerances are errors", "[screenshot_diff]") {
    const ImageData image = gradient(4, 4);
    const auto invalid = rv::screenshot_diff::compareImages(ImageData{}, image, Tolerance{});
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(rv::screenshot_diff::compareImages(image, image, Tolerance{-1, 0.5}).has_value());
    CHECK_FALSE(rv::screenshot_diff::compareImages(image, image, Tolerance{256, 0.5}).has_value());
    CHECK_FALSE(rv::screenshot_diff::compareImages(image, image, Tolerance{6, 100.5}).has_value());
    CHECK_FALSE(rv::screenshot_diff::makeHeatmap(image, gradient(5, 4), 6).has_value());
}

TEST_CASE("screenshot_diff: the heatmap shows unchanged, small and large differences apart", "[screenshot_diff]") {
    const ImageData expected = gradient(6, 2);
    ImageData actual = expected;
    nudge(actual, 1, 0, 0, 3);  // within the threshold
    nudge(actual, 2, 0, 1, 7);  // just beyond it
    nudge(actual, 3, 1, 2, 90); // far beyond it
    const auto heatmap = rv::screenshot_diff::makeHeatmap(expected, actual, 6);
    REQUIRE(heatmap.has_value());
    CHECK(heatmap->width == 6);
    CHECK(heatmap->height == 2);
    CHECK(heatmap->channels == 3);

    const std::uint8_t* unchanged = pixelAt(*heatmap, 0, 0);
    CHECK(unchanged[0] == unchanged[1]);
    CHECK(unchanged[1] == unchanged[2]);
    CHECK(unchanged[0] < 64); // a dark grey
    const std::uint8_t* small = pixelAt(*heatmap, 1, 0);
    CHECK((small[0] == 0 && small[1] == 0 && small[2] > 0));
    const std::uint8_t* justBeyond = pixelAt(*heatmap, 2, 0);
    CHECK((justBeyond[0] == 255 && justBeyond[1] < 16 && justBeyond[2] == 0));
    const std::uint8_t* farBeyond = pixelAt(*heatmap, 3, 1);
    CHECK((farBeyond[0] == 255 && farBeyond[1] == 255 && farBeyond[2] == 0));
}

TEST_CASE("screenshot_diff: files are compared and a mismatch leaves a heatmap", "[screenshot_diff]") {
    const rv::test::TempDir dir("rv-screenshot-diff");
    const ImageData expected = gradient(20, 20);
    ImageData actual = expected;
    for (int x = 0; x < 20; ++x) {
        nudge(actual, x, 10, 0, 40);
    }
    REQUIRE(rv::assets::writePng(dir.file("golden.png"), expected).has_value());
    REQUIRE(rv::assets::writePng(dir.file("same.png"), expected).has_value());
    REQUIRE(rv::assets::writePng(dir.file("changed.png"), actual).has_value());

    const auto same = rv::screenshot_diff::compareFiles(dir.file("golden.png"), dir.file("same.png"), Tolerance{},
                                                        dir.file("same_heatmap.png"));
    REQUIRE(same.has_value());
    CHECK(same->matches);
    CHECK_FALSE(std::filesystem::exists(dir.file("same_heatmap.png")));

    const auto changed = rv::screenshot_diff::compareFiles(dir.file("golden.png"), dir.file("changed.png"),
                                                           Tolerance{}, dir.file("heatmaps/changed.png"));
    REQUIRE(changed.has_value());
    CHECK_FALSE(changed->matches);
    CHECK(changed->differingPixels == 20);
    const auto heatmap = rv::assets::loadImage(dir.file("heatmaps/changed.png"));
    REQUIRE(heatmap.has_value());
    CHECK(heatmap->width == 20);

    const auto missing = rv::screenshot_diff::compareFiles(dir.file("golden.png"), dir.file("absent.png"),
                                                           Tolerance{}, std::nullopt);
    REQUIRE_FALSE(missing.has_value());
}
