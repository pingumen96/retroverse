// The black bars of a captured game's window (I10 step 11): found in pairs, steady over the samples, and the CRT's
// rectangle that shows the crop at its own aspect.

#include <rv/app/picture_bounds.hpp>
#include <rv/app/screen_aspect.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

using Catch::Approx;
using rv::app::findPictureBounds;
using rv::app::PictureBounds;

namespace {

constexpr glm::ivec2 kSize{256, 144};

/// A black picture of kSize with the rectangle [x0, x1) x [y0, y1) lit grey.
std::vector<std::uint8_t> picture(int x0, int y0, int x1, int y1) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kSize.x) * kSize.y * 4, 0);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * kSize.x + x) * 4;
            rgba[at] = rgba[at + 1] = rgba[at + 2] = 128;
            rgba[at + 3] = 255;
        }
    }
    return rgba;
}

void light(std::vector<std::uint8_t>& rgba, int x0, int y0, int x1, int y1) {
    const std::vector<std::uint8_t> lit = picture(x0, y0, x1, y1);
    for (std::size_t i = 0; i < rgba.size(); ++i) {
        rgba[i] = std::max(rgba[i], lit[i]);
    }
}

} // namespace

TEST_CASE("app/picture_bounds: a 4:3 game pillarboxed in a 16:9 window", "[app][picture_bounds]") {
    // 192 of 256 columns lit in the middle: 32 black columns each side.
    const std::optional<glm::vec4> crop = findPictureBounds(picture(32, 0, 224, 144), kSize);
    REQUIRE(crop);
    CHECK(crop->x == Approx(31.0 / 256.0));
    CHECK(crop->y == 0.0f);
    CHECK(crop->z == Approx(194.0 / 256.0));
    CHECK(crop->w == 1.0f);
    // A full picture has no bar; a hairline is no bar.
    CHECK(*findPictureBounds(picture(0, 0, 256, 144), kSize) == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
    CHECK(*findPictureBounds(picture(2, 0, 254, 144), kSize) == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
}

TEST_CASE("app/picture_bounds: bars in pairs, a notice in a bar, dark frames", "[app][picture_bounds]") {
    // A letterbox, its bars unequal: the thinner taken on both sides.
    const std::optional<glm::vec4> letterbox = findPictureBounds(picture(0, 20, 256, 130), kSize);
    REQUIRE(letterbox);
    CHECK(letterbox->y == Approx(13.0 / 144.0));
    CHECK(letterbox->w == Approx(118.0 / 144.0));
    // An emulator's notice in the left bar's bottom corner leaves the bar a bar.
    std::vector<std::uint8_t> notice = picture(32, 0, 224, 144);
    light(notice, 2, 136, 60, 142);
    const std::optional<glm::vec4> withNotice = findPictureBounds(notice, kSize);
    REQUIRE(withNotice);
    CHECK(withNotice->x == Approx(31.0 / 256.0));
    // All black, or a small logo on black: nothing to decide on.
    CHECK_FALSE(findPictureBounds(picture(0, 0, 0, 0), kSize));
    CHECK_FALSE(findPictureBounds(picture(100, 50, 156, 94), kSize));
    CHECK_FALSE(findPictureBounds(std::vector<std::uint8_t>(16, 0), kSize));
}

TEST_CASE("app/picture_bounds: the crop narrows once it held, widens at once", "[app][picture_bounds]") {
    PictureBounds bounds;
    const glm::vec4 pillarbox(0.125f, 0.0f, 0.75f, 1.0f);
    for (std::size_t i = 1; i < PictureBounds::kNarrowSamples; ++i) {
        CHECK_FALSE(bounds.update(pillarbox));
        CHECK(bounds.crop() == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
    }
    CHECK(bounds.update(pillarbox));
    CHECK(bounds.crop() == pillarbox);
    // Dark frames keep it; a dark scene's narrower crop for a moment does not take it.
    CHECK_FALSE(bounds.update(std::nullopt));
    CHECK_FALSE(bounds.update(glm::vec4(0.25f, 0.1f, 0.5f, 0.8f)));
    CHECK_FALSE(bounds.update(pillarbox));
    CHECK(bounds.crop() == pillarbox);
    // The emulator's menu over the whole window: widened at once.
    CHECK(bounds.update(glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)));
    CHECK(bounds.crop() == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
    bounds.reset();
    CHECK(bounds.crop() == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
}

TEST_CASE("app/screen_aspect: a cropped window fills the tube at the crop's aspect", "[app][screen_aspect]") {
    constexpr glm::vec2 kCrt{0.48f, 0.36f};
    constexpr glm::vec2 kWindow{2560.0f, 1440.0f};
    // The whole 16:9 window: letterboxed.
    const glm::vec4 whole = rv::app::croppedPictureRect(kWindow, glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), kCrt);
    CHECK(whole == rv::app::pictureRect(kWindow, kCrt));
    CHECK(whole.w == Approx(0.75f));
    // Its 4:3 middle fills the tube: the whole window's rectangle reaches beyond the screen at the sides.
    const glm::vec4 middle = rv::app::croppedPictureRect(kWindow, glm::vec4(0.125f, 0.0f, 0.75f, 1.0f), kCrt);
    CHECK(middle.x == Approx(-1.0f / 6.0f));
    CHECK(middle.y == 0.0f);
    CHECK(middle.z == Approx(4.0f / 3.0f));
    CHECK(middle.w == 1.0f);
    // The screen's left edge samples the crop's left edge, its right edge the crop's right edge.
    CHECK((0.0f - middle.x) / middle.z == Approx(0.125f));
    CHECK((1.0f - middle.x) / middle.z == Approx(0.875f));
}
