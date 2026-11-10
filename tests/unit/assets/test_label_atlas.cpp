// I12 step 5: the shelves' labels' atlases (assets::labelAtlasLayout, makeTabAtlas, makeCaptionAtlas).

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <rv/assets/label_atlas.hpp>
#include <rv/core/text.hpp>

using rv::assets::AtlasCell;
using rv::assets::CaptionText;
using rv::assets::FontFace;
using rv::assets::ImageData;

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

/// The pixels of a rectangle darker than half: ink.
std::size_t inkIn(const ImageData& image, int x0, int y0, int x1, int y1) {
    std::size_t count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            count += pixel(image, x, y).r < 128 ? 1 : 0;
        }
    }
    return count;
}

std::size_t inkIn(const ImageData& image, const AtlasCell& cell) {
    return inkIn(image, cell.x, cell.y, cell.x + cell.width, cell.y + cell.height);
}

} // namespace

TEST_CASE("assets/label_atlas: the cells' layout", "[assets][label_atlas]") {
    const auto five = rv::assets::labelAtlasLayout(5, rv::assets::kTabCell, rv::assets::kTabColumns);
    REQUIRE(five.cells.size() == 5);
    CHECK(five.width == 4 * (192 + 8));
    CHECK(five.height == 2 * (64 + 8));
    CHECK(five.cells[0] == AtlasCell{4, 4, 192, 64});
    CHECK(five.cells[3] == AtlasCell{3 * 200 + 4, 4, 192, 64});
    CHECK(five.cells[4] == AtlasCell{4, 72 + 4, 192, 64});
    const glm::vec4 uv = five.uv(4);
    CHECK(uv.x == 4.0f / 800.0f);
    CHECK(uv.y == 76.0f / 144.0f);
    CHECK(uv.z == 196.0f / 800.0f);
    CHECK(uv.w == 140.0f / 144.0f);
    // Fewer cells than columns: an atlas as wide as they are; the sides multiples of 4.
    const auto one = rv::assets::labelAtlasLayout(1, glm::ivec2(30, 10), 4);
    CHECK(one.width == 40);
    CHECK(one.height == 20);
    CHECK(one.width % 4 == 0);
    // None.
    CHECK(rv::assets::labelAtlasLayout(0, rv::assets::kTabCell, 4).cells.empty());
    CHECK(rv::assets::labelAtlasLayout(rv::assets::kMaxAtlasCells + 1, rv::assets::kTabCell, 4).cells.empty());
    CHECK(rv::assets::labelAtlasLayout(3, rv::assets::kTabCell, 0).cells.empty());
}

TEST_CASE("assets/label_atlas: the letter tabs", "[assets][label_atlas]") {
    const std::vector<std::string> texts{"A–C", "D", "0-9", "#", "M–Z"};
    const ImageData atlas = rv::assets::makeTabAtlas(texts, bold());
    REQUIRE(atlas.isValid());
    REQUIRE(atlas.channels == 4);
    const auto layout = rv::assets::labelAtlasLayout(texts.size(), rv::assets::kTabCell, rv::assets::kTabColumns);
    CHECK(atlas.width == layout.width);
    CHECK(atlas.height == layout.height);
    for (std::size_t i = 0; i < texts.size(); ++i) {
        INFO(texts[i]);
        CHECK(inkIn(atlas, layout.cells[i]) > 100);
    }
    // A wider text has more ink across; the gutter and the cell's margins stay paper.
    CHECK(inkIn(atlas, layout.cells[0]) > inkIn(atlas, layout.cells[1]));
    CHECK(inkIn(atlas, 0, 0, atlas.width, 4) == 0);
    CHECK(inkIn(atlas, layout.cells[0].x, layout.cells[0].y, layout.cells[0].x + 8, layout.cells[0].y + 64) == 0);
    CHECK(pixel(atlas, 1, 1) ==
          glm::u8vec4(rv::assets::kTabPaper.r, rv::assets::kTabPaper.g, rv::assets::kTabPaper.b, 255));
    // Deterministic.
    CHECK(rv::assets::makeTabAtlas(texts, bold()).pixels == atlas.pixels);
    // None.
    CHECK_FALSE(rv::assets::makeTabAtlas({}, bold()).isValid());
}

TEST_CASE("assets/label_atlas: the caption cards", "[assets][label_atlas]") {
    const std::vector<CaptionText> cards{
        {"Lumen Quest", "1993 · Fixture Works"},
        {"A Title Long Enough To Be Cut With An Ellipsis On The Card", "1991 · A Developer Whose Name Is Long Too"},
        {"Solo", ""},
    };
    const ImageData atlas = rv::assets::makeCaptionAtlas(cards, bold(), regular());
    REQUIRE(atlas.isValid());
    const auto layout =
        rv::assets::labelAtlasLayout(cards.size(), rv::assets::kCaptionCell, rv::assets::kCaptionColumns);
    REQUIRE(atlas.width == layout.width);
    REQUIRE(atlas.height == layout.height);
    // The title in the upper half, the detail in the lower one; a card without a detail has its title in the middle.
    for (std::size_t i = 0; i < 2; ++i) {
        const AtlasCell& cell = layout.cells[i];
        CHECK(inkIn(atlas, cell.x, cell.y, cell.x + cell.width, cell.y + cell.height / 2 - 2) > 200);
        CHECK(inkIn(atlas, cell.x, cell.y + cell.height / 2 + 2, cell.x + cell.width, cell.y + cell.height) > 100);
    }
    const AtlasCell& solo = layout.cells[2];
    CHECK(inkIn(atlas, solo.x, solo.y + solo.height / 3, solo.x + solo.width, solo.y + 2 * solo.height / 3) > 100);
    CHECK(inkIn(atlas, solo.x, solo.y, solo.x + solo.width, solo.y + solo.height / 5) == 0);
    // The long lines stay inside their margins.
    const AtlasCell& cut = layout.cells[1];
    CHECK(inkIn(atlas, cut.x, cut.y, cut.x + 10, cut.y + cut.height) == 0);
    CHECK(inkIn(atlas, cut.x + cut.width - 10, cut.y, cut.x + cut.width, cut.y + cut.height) == 0);
    CHECK(rv::assets::makeCaptionAtlas(cards, bold(), regular()).pixels == atlas.pixels);
    CHECK_FALSE(rv::assets::makeCaptionAtlas({}, bold(), regular()).isValid());
}
