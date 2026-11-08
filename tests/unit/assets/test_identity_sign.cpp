// I11 step 4: a room's sign drawn from its system's identity (assets::layoutIdentitySign, makeIdentitySign).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <rv/assets/identity_sign.hpp>
#include <rv/assets/test_patterns.hpp>
#include <rv/core/text.hpp>

using rv::assets::FontFace;
using rv::assets::IdentitySignStyle;
using rv::assets::ImageData;
using rv::assets::SignBox;

namespace {

const FontFace& boldFace() {
    static const FontFace font = [] {
        auto loaded =
            FontFace::loadBundled(rv::core::pathFromUtf8(RV_TEST_ASSET_DIR), rv::assets::kLabelTitleFontPath);
        REQUIRE(loaded.has_value());
        return std::move(*loaded);
    }();
    return font;
}

/// A logo of `width` x `height`: `color` where it draws, transparent in a frame of a sixth of its height all round.
ImageData logoOf(int width, int height, glm::u8vec4 color) {
    ImageData logo = rv::assets::makeSolid(width, height, glm::u8vec4(0, 0, 0, 0));
    const int frame = height / 6;
    for (int y = frame; y < height - frame; ++y) {
        for (int x = frame; x < width - frame; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                    static_cast<std::size_t>(x)) * 4;
            logo.pixels[at] = color.r;
            logo.pixels[at + 1] = color.g;
            logo.pixels[at + 2] = color.b;
            logo.pixels[at + 3] = color.a;
        }
    }
    return logo;
}

glm::u8vec4 pixel(const ImageData& image, int x, int y) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

/// The brightest red in a box of the image.
int brightestIn(const ImageData& image, const SignBox& box) {
    int brightest = 0;
    for (int y = static_cast<int>(box.y); y < static_cast<int>(box.y + box.height); ++y) {
        for (int x = static_cast<int>(box.x); x < static_cast<int>(box.x + box.width); ++x) {
            brightest = std::max(brightest, static_cast<int>(pixel(image, x, y).r));
        }
    }
    return brightest;
}

bool inside(const SignBox& inner, const SignBox& outer) {
    constexpr float kSlack = 0.01F;
    return inner.x >= outer.x - kSlack && inner.y >= outer.y - kSlack &&
           inner.x + inner.width <= outer.x + outer.width + kSlack &&
           inner.y + inner.height <= outer.y + outer.height + kSlack;
}

} // namespace

TEST_CASE("assets/identity_sign: the layout per case", "[assets][identity_sign]") {
    const FontFace& font = boldFace();
    const std::vector<std::pair<int, int>> wide{{400, 100}};

    SECTION("a blade's shape: the logo above, the name under it, both centred within the margin") {
        // A blade's picture: 512 x 130, a margin of 10.
        const auto layout = rv::assets::layoutIdentitySign(512, 130, wide, "Lumen 16", font);
        const SignBox inner{10.0F, 10.0F, 492.0F, 110.0F};
        CHECK_FALSE(layout.sideBySide);
        REQUIRE(layout.logos.size() == 1);
        const SignBox& logo = layout.logos.front();
        CHECK(logo.height == Catch::Approx(110.0F * 0.62F));
        CHECK(logo.width == Catch::Approx(logo.height * 4.0F));
        CHECK(logo.x + logo.width / 2.0F == Catch::Approx(256.0F));
        CHECK(inside(logo, inner));
        REQUIRE(layout.nameLines.size() == 1);
        CHECK(rv::assets::encodeUtf8(layout.nameLines.front()) == "Lumen 16");
        const SignBox& name = layout.nameBoxes.front();
        CHECK(name.y >= logo.y + logo.height);
        CHECK(layout.namePixelHeight == Catch::Approx(110.0F * 0.30F));
        CHECK(name.x + name.width / 2.0F == Catch::Approx(256.0F));
        CHECK(inside(name, inner));
    }
    SECTION("the room's neon (512 x 116, an inner box over 4.5 times as wide as tall): the name beside the logo") {
        const auto layout = rv::assets::layoutIdentitySign(512, 116, wide, "Lumen 16", font);
        CHECK(layout.sideBySide);
        REQUIRE(layout.logos.size() == 1);
        // The logos' row at most half the inner width.
        CHECK(layout.logos.front().width == Catch::Approx(247.0F));
        CHECK(inside(layout.nameBoxes.front(), SignBox{9.0F, 9.0F, 494.0F, 98.0F}));
    }
    SECTION("the hub's fascia, much wider than tall: the name beside the logo, the group centred") {
        const auto layout = rv::assets::layoutIdentitySign(512, 61, wide, "Lumen 16", font);
        CHECK(layout.sideBySide);
        REQUIRE(layout.logos.size() == 1);
        REQUIRE(layout.nameBoxes.size() == 1);
        const SignBox& logo = layout.logos.front();
        const SignBox& name = layout.nameBoxes.front();
        CHECK(logo.height == Catch::Approx(51.0F));
        CHECK(name.x > logo.x + logo.width);
        const float left = logo.x - 5.0F;
        const float right = 507.0F - (name.x + name.width);
        CHECK(left == Catch::Approx(right).margin(0.5F));
    }
    SECTION("a name alone takes the whole inner box, on one line when that is as large") {
        const auto layout = rv::assets::layoutIdentitySign(512, 116, {}, "Lumen 16", font);
        CHECK(layout.logos.empty());
        REQUIRE(layout.nameLines.size() == 1);
        CHECK(layout.namePixelHeight <= 98.0F);
        CHECK(inside(layout.nameBoxes.front(), SignBox{9.0F, 9.0F, 494.0F, 98.0F}));
    }
    SECTION("a long name on a tall sign breaks at the space nearest its middle, never inside a word") {
        const auto layout =
            rv::assets::layoutIdentitySign(512, 300, {}, "Kestrel Entertainment Home Video System", font);
        REQUIRE(layout.nameLines.size() == 2);
        CHECK(rv::assets::encodeUtf8(layout.nameLines[0]) == "Kestrel Entertainment");
        CHECK(rv::assets::encodeUtf8(layout.nameLines[1]) == "Home Video System");
        CHECK(layout.nameBoxes[1].y == Catch::Approx(layout.nameBoxes[0].y + layout.namePixelHeight));
    }
    SECTION("a name of one word never breaks") {
        const auto layout = rv::assets::layoutIdentitySign(512, 300, {}, "Odyssey²", font);
        REQUIRE(layout.nameLines.size() == 1);
        CHECK(rv::assets::encodeUtf8(layout.nameLines.front()) == "Odyssey²");
    }
    SECTION("a shared room's logos in one row at one height, at most four, the others left out") {
        const std::vector<std::pair<int, int>> five{{200, 100}, {100, 100}, {300, 100}, {200, 100}, {200, 100}};
        const auto layout = rv::assets::layoutIdentitySign(512, 116, five, "", font);
        REQUIRE(layout.logos.size() == rv::assets::kMaxSignLogos);
        CHECK(layout.nameLines.empty());
        for (std::size_t i = 1; i < layout.logos.size(); ++i) {
            CHECK(layout.logos[i].height == Catch::Approx(layout.logos[0].height));
            CHECK(layout.logos[i].x ==
                  Catch::Approx(layout.logos[i - 1].x + layout.logos[i - 1].width + 0.1F * layout.logos[0].height));
        }
        const SignBox& last = layout.logos.back();
        CHECK(layout.logos.front().x - 9.0F == Catch::Approx(503.0F - (last.x + last.width)).margin(0.01F));
        CHECK(inside(last, SignBox{9.0F, 9.0F, 494.0F, 98.0F}));
    }
    SECTION("nothing to show, or no size: nothing laid out") {
        CHECK(rv::assets::layoutIdentitySign(512, 116, {}, "   ", font).logos.empty());
        CHECK(rv::assets::layoutIdentitySign(512, 116, {}, "", font).nameLines.empty());
        CHECK(rv::assets::layoutIdentitySign(0, 116, wide, "Lumen 16", font).logos.empty());
        const std::vector<std::pair<int, int>> bad{{0, 10}, {10, -1}};
        CHECK(rv::assets::layoutIdentitySign(512, 116, bad, "", font).logos.empty());
    }
}

TEST_CASE("assets/identity_sign: the lit plate and the neon", "[assets][identity_sign]") {
    const FontFace& font = boldFace();
    const ImageData orange = logoOf(400, 100, glm::u8vec4(240, 120, 20, 255));
    const auto layout = rv::assets::layoutIdentitySign(512, 116, std::vector<std::pair<int, int>>{{400, 100}},
                                                       "Lumen 16", font);
    REQUIRE(layout.logos.size() == 1);
    const SignBox& logo = layout.logos.front();
    const int cx = static_cast<int>(logo.x + logo.width / 2.0F);
    const int cy = static_cast<int>(logo.y + logo.height / 2.0F);

    SECTION("lit: the logo in its colours and the name in white on black") {
        const ImageData sign =
            rv::assets::makeIdentitySign(IdentitySignStyle::Lit, 512, 116, std::vector{orange}, "Lumen 16", font);
        REQUIRE(sign.isValid());
        CHECK(sign.channels == 4);
        CHECK(pixel(sign, cx, cy) == glm::u8vec4(240, 120, 20, 255));
        CHECK(pixel(sign, 2, 2) == glm::u8vec4(0, 0, 0, 255));
        CHECK(brightestIn(sign, layout.nameBoxes.front()) == 255);
        // The same arguments, the same pixels.
        CHECK(rv::assets::makeIdentitySign(IdentitySignStyle::Lit, 512, 116, std::vector{orange}, "Lumen 16", font)
                  .pixels == sign.pixels);
    }
    SECTION("lit: a dark logo shows as a white silhouette") {
        const ImageData dark = logoOf(400, 100, glm::u8vec4(10, 10, 12, 255));
        CHECK(rv::assets::isDarkLogo(dark));
        CHECK_FALSE(rv::assets::isDarkLogo(orange));
        const ImageData sign =
            rv::assets::makeIdentitySign(IdentitySignStyle::Lit, 512, 116, std::vector{dark}, "", font);
        const auto alone = rv::assets::layoutIdentitySign(512, 116, std::vector<std::pair<int, int>>{{400, 100}},
                                                          "", font);
        const SignBox& box = alone.logos.front();
        CHECK(pixel(sign, static_cast<int>(box.x + box.width / 2.0F), static_cast<int>(box.y + box.height / 2.0F)) ==
              glm::u8vec4(255, 255, 255, 255));
    }
    SECTION("neon: the logo's outline glows, its inside is the backing, the name's letters glow") {
        const ImageData sign =
            rv::assets::makeIdentitySign(IdentitySignStyle::Neon, 512, 116, std::vector{orange}, "Lumen 16", font);
        REQUIRE(sign.isValid());
        // Grey levels only (the neon's material colours them).
        CHECK(pixel(sign, cx, cy).r == pixel(sign, cx, cy).g);
        // The filled middle of the logo is no tube: the backing (with some halo at most).
        CHECK(pixel(sign, cx, cy).r <= static_cast<int>(rv::assets::kNeonHalo * 255.0F) + 1);
        // Its outline, a sixth of the logo's height in from its box, glows white at the core.
        const SignBox outline{logo.x + logo.height / 6.0F - 1.0F, cy - 2.0F, 6.0F, 4.0F};
        CHECK(brightestIn(sign, outline) >= 230);
        CHECK(brightestIn(sign, layout.nameBoxes.front()) >= 230);
    }
    SECTION("no size: an empty image") {
        CHECK_FALSE(rv::assets::makeIdentitySign(IdentitySignStyle::Lit, 0, 10, {}, "x", font).isValid());
    }
}
