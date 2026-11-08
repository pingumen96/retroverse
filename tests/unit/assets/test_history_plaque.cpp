// I11 step 5: a console room's history plaque (assets::makeHistoryPlaque, wrapText, plaqueDetails).

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <rv/assets/history_plaque.hpp>
#include <rv/assets/test_patterns.hpp>
#include <rv/core/text.hpp>

using rv::assets::FontFace;
using rv::assets::ImageData;
using rv::assets::PlaqueText;

namespace {

const FontFace& face(std::string_view path) {
    static std::map<std::string, FontFace> faces;
    auto found = faces.find(std::string(path));
    if (found == faces.end()) {
        auto loaded = FontFace::loadBundled(rv::core::pathFromUtf8(RV_TEST_ASSET_DIR), path);
        REQUIRE(loaded.has_value());
        found = faces.emplace(std::string(path), std::move(*loaded)).first;
    }
    return found->second;
}

const FontFace& bold() {
    return face(rv::assets::kLabelTitleFontPath);
}

const FontFace& regular() {
    return face(rv::assets::kLabelDetailFontPath);
}

glm::u8vec4 pixel(const ImageData& image, int x, int y) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

/// The pixels of a row band darker than the brass by far: the engraving.
std::size_t engravedIn(const ImageData& image, int y0, int y1) {
    std::size_t count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < image.width; ++x) {
            count += pixel(image, x, y).r < 100 ? 1 : 0;
        }
    }
    return count;
}

const std::string kDescription =
    "La console a sedici bit di Fixture Works, uscita nel 1991, con un processore grafico capace di ruotare e scalare "
    "lo sfondo e un chip sonoro a otto canali. Questa descrizione e inventata per la fixture e prosegue a lungo, "
    "piu di quanto una targa possa tenere, perche si veda dove viene tagliata.";

} // namespace

TEST_CASE("assets/history_plaque: the details' line and the wrapping", "[assets][history_plaque]") {
    CHECK(rv::assets::plaqueDetails(PlaqueText{"Lumen 16", "Fixture Works", "1991", "Console", ""}) ==
          "Fixture Works · 1991 · Console");
    CHECK(rv::assets::plaqueDetails(PlaqueText{"x", "", "1991", "", ""}) == "1991");
    CHECK(rv::assets::plaqueDetails(PlaqueText{}).empty());

    const FontFace& font = regular();
    SECTION("lines no wider than the width, broken at spaces, every word kept in order") {
        const auto lines = rv::assets::wrapText(kDescription, 300.0F, 20.0F, font, 100);
        REQUIRE(lines.size() > 3);
        std::string joined;
        for (const std::u32string& line : lines) {
            CHECK(font.layout(line, 20.0F).width <= 300.0F);
            joined += (joined.empty() ? "" : " ") + rv::assets::encodeUtf8(line);
        }
        CHECK(joined == kDescription);
    }
    SECTION("cut at the lines the plate holds, the last ending with an ellipsis") {
        const auto lines = rv::assets::wrapText(kDescription, 300.0F, 20.0F, font, 3);
        REQUIRE(lines.size() == 3);
        CHECK(lines.back().back() == U'…');
        CHECK(font.layout(lines.back(), 20.0F).width <= 300.0F);
    }
    SECTION("spaces and line breaks run together; a word wider than a line stands alone, cut") {
        const auto lines = rv::assets::wrapText("a  b\n\nc", 300.0F, 20.0F, font, 10);
        REQUIRE(lines.size() == 1);
        CHECK(rv::assets::encodeUtf8(lines.front()) == "a b c");
        const auto cut = rv::assets::wrapText("Supercalifragilistichespiralidoso x", 80.0F, 20.0F, font, 10);
        REQUIRE(cut.size() == 2);
        CHECK(cut.front().back() == U'…');
        CHECK(font.layout(cut.front(), 20.0F).width <= 80.0F);
        CHECK(rv::assets::wrapText("", 300.0F, 20.0F, font, 3).empty());
        CHECK(rv::assets::wrapText(kDescription, 300.0F, 20.0F, font, 0).empty());
    }
}

TEST_CASE("assets/history_plaque: the plate", "[assets][history_plaque]") {
    const PlaqueText text{"Lumen 16", "Fixture Works", "1991", "Console", kDescription};
    const ImageData plate = rv::assets::makeHistoryPlaque(768, 548, text, nullptr, bold(), regular());
    REQUIRE(plate.isValid());
    CHECK(plate.channels == 4);
    // Brass where nothing is engraved: a corner just inside the border's line.
    const glm::u8vec4 brass = pixel(plate, 30, 274);
    CHECK(brass.r > brass.g);
    CHECK(brass.g > brass.b);
    CHECK(brass.r > 140);
    // The name (no logo) engraved at the top, the details under it, the description below the rule.
    CHECK(engravedIn(plate, 40, 120) > 500);
    CHECK(engravedIn(plate, 140, 175) > 200);
    CHECK(engravedIn(plate, 220, 500) > 3000);
    // The same arguments, the same pixels.
    CHECK(rv::assets::makeHistoryPlaque(768, 548, text, nullptr, bold(), regular()).pixels == plate.pixels);

    SECTION("a logo takes the name's place, engraved as a silhouette") {
        ImageData logo = rv::assets::makeSolid(400, 100, glm::u8vec4(240, 120, 20, 255));
        const ImageData withLogo = rv::assets::makeHistoryPlaque(768, 548, text, &logo, bold(), regular());
        // The logo's box: the plate's inner width at the title's height (0.16 h), fitted at 4:1 and centred.
        const glm::u8vec4 middle = pixel(withLogo, 384, 38 + 44);
        CHECK(middle.r < 70);
        CHECK(middle.g < 70); // engraved, never the logo's orange
        CHECK_FALSE(withLogo.pixels == plate.pixels);
    }
    SECTION("no size: an empty image") {
        CHECK_FALSE(rv::assets::makeHistoryPlaque(0, 10, text, nullptr, bold(), regular()).isValid());
    }
}
