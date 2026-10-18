// I3 step 5: text rasterized with stb_truetype from the bundled font, loaded only when its SHA-256 matches the asset
// registry (rv/assets/text_raster.hpp, ADR-0011 §5).

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>

#include <rv/assets/text_raster.hpp>
#include <rv/core/file_io.hpp>

#include "cover_test_support.hpp"
#include "support/temp_dir.hpp"

using namespace rv::assets;
using rv::test::covers::blankImage;
using rv::test::covers::kAssetRoot;
namespace fs = std::filesystem;

namespace {

/// A scratch asset root: the registry and the regular font copied, then changed by the test.
fs::path scratchRoot(const rv::test::TempDir& temp) {
    const fs::path root = temp.path() / "assets";
    fs::create_directories(root / "registry");
    fs::create_directories(root / "ui" / "fonts");
    fs::copy_file(kAssetRoot / "registry" / "assets.json", root / "registry" / "assets.json");
    fs::copy_file(kAssetRoot / "ui" / "fonts" / "AtkinsonHyperlegible-Regular.ttf",
                  root / "ui" / "fonts" / "AtkinsonHyperlegible-Regular.ttf");
    return root;
}

} // namespace

TEST_CASE("assets/text_raster: UTF-8 decodes to code points, malformed bytes to U+FFFD", "[assets][text_raster]") {
    CHECK(decodeUtf8("Ab") == U"Ab");
    CHECK(decodeUtf8("Caf\xC3\xA9") == std::u32string{U'C', U'a', U'f', char32_t{0xE9}});
    CHECK(decodeUtf8("\xE3\x83\xAD\xE3\x83\x9C") == std::u32string{char32_t{0x30ED}, char32_t{0x30DC}});
    CHECK(decodeUtf8("\xF0\x9F\x8E\xAE") == std::u32string(1, char32_t{0x1F3AE}));
    const char32_t replacement = 0xFFFD;
    CHECK(decodeUtf8("a\xFFz") == std::u32string{U'a', replacement, U'z'});
    CHECK(decodeUtf8("\xC0\xAF") == std::u32string(2, replacement)); // an overlong form: C0 never leads
    CHECK(decodeUtf8("\xED\xA0\x80") == std::u32string(1, replacement)); // a surrogate
    CHECK(decodeUtf8("\xE3\x83") == std::u32string(1, replacement));     // truncated
    CHECK(encodeUtf8(std::u32string{U'C', char32_t{0xE9}, U' ', char32_t{0x30ED}}) == "C\xC3\xA9 \xE3\x83\xAD");
    CHECK(encodeUtf8(std::u32string(1, char32_t{0xD800})) == "\xEF\xBF\xBD");
}

TEST_CASE("assets/text_raster: the bundled fonts load through their registry entries", "[assets][text_raster]") {
    const auto& fonts = rv::test::covers::bundledFonts();
    CHECK(fonts.title.sha256() == "5a3b0c8cc8ca545155150b4512a1fa248298df121c50d6557e651e61fbdab92f");
    CHECK(fonts.detail.sha256() == "7fb917c89019896d0b52ee84b7cbb3304c18cb90b19a62f5e32712bd23e97669");
    CHECK(fonts.title.hasGlyph(U'A'));
    CHECK(fonts.title.hasGlyph(U'é'));
    CHECK_FALSE(fonts.title.hasGlyph(U'マ')); // no Japanese in the Latin font
    CHECK(fonts.title.ascent(20.0F) - fonts.title.descent(20.0F) > 19.9F);
    CHECK(fonts.title.capHeight(20.0F) > 8.0F);
    CHECK(fonts.title.capHeight(20.0F) < 16.0F);
    // The ellipsis exists; U+FFFD does not, so a missing character is drawn as the font's .notdef (glyph 0).
    CHECK(fonts.title.hasGlyph(char32_t{0x2026}));
    CHECK(fonts.title.replacementGlyph() == 0);
    CHECK(fonts.detail.hasGlyph(char32_t{0x2026}));
}

TEST_CASE("assets/text_raster: a font file with another SHA-256 is refused before stb_truetype reads it",
          "[assets][text_raster]") {
    const rv::test::TempDir temp("rv-font");
    const fs::path root = scratchRoot(temp);
    const fs::path font = root / "ui" / "fonts" / "AtkinsonHyperlegible-Regular.ttf";
    REQUIRE(FontFace::loadBundled(root, kLabelDetailFontPath).has_value());

    SECTION("one byte changed") {
        auto bytes = rv::core::readBinaryFile(font).value();
        bytes[bytes.size() / 2] ^= std::byte{0x40};
        REQUIRE(rv::core::writeFileAtomic(font, bytes).has_value());
        auto loaded = FontFace::loadBundled(root, kLabelDetailFontPath);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().code == rv::ErrorCode::ValidationFailed);
    }
    SECTION("another font under the bundled name") {
        fs::copy_file(kAssetRoot / "ui" / "fonts" / "AtkinsonHyperlegible-Bold.ttf", font,
                      fs::copy_options::overwrite_existing);
        auto loaded = FontFace::loadBundled(root, kLabelDetailFontPath);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().code == rv::ErrorCode::ValidationFailed);
    }
    SECTION("no registry entry") {
        auto loaded = FontFace::loadBundled(root, kLabelTitleFontPath); // the bold entry exists, its file does not
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().code == rv::ErrorCode::NotFound);
        auto unlisted = FontFace::loadBundled(root, "ui/fonts/Other.ttf");
        REQUIRE_FALSE(unlisted.has_value());
        CHECK(unlisted.error().code == rv::ErrorCode::NotFound);
        CHECK(FontFace::loadBundled(root, "../fonts/x.ttf").error().code == rv::ErrorCode::InvalidArgument);
    }
    SECTION("a matching hash of a file that is no font") {
        const fs::path text = temp.path() / "not_a_font.ttf";
        REQUIRE(rv::core::writeTextFileAtomic(text, "not a font").has_value());
        auto loaded = FontFace::loadVerified(text, rv::core::sha256Hex(std::string_view("not a font")));
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().code == rv::ErrorCode::ParseError);
    }
}

TEST_CASE("assets/text_raster: missing characters are laid out as the replacement glyph", "[assets][text_raster]") {
    const FontFace& font = rv::test::covers::bundledFonts().title;
    const GlyphRun latin = font.layout(U"Robo Kid", 20.0F);
    CHECK(latin.replacementGlyphs == 0);
    CHECK(latin.width > 50.0F);
    const GlyphRun mixed = font.layout(U"Robo Kid ロック", 20.0F);
    CHECK(mixed.replacementGlyphs == 3);
    CHECK(mixed.glyphs.back() == font.replacementGlyph());
    CHECK(mixed.width > latin.width);
    // Widths scale with the size (no hinting): the layout's sizes are computed from one measurement.
    CHECK(std::abs(font.layout(U"Robo Kid", 40.0F).width - 2.0F * latin.width) < 1e-3F);
}

TEST_CASE("assets/text_raster: text is drawn at twice the size and reduced in linear light", "[assets][text_raster]") {
    const FontFace& font = rv::test::covers::bundledFonts().title;
    ImageData image = blankImage(120, 40, 4, 255);
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        image.pixels[i + 0] = image.pixels[i + 1] = image.pixels[i + 2] = 0; // black
    }
    const PlacedRun run{&font, U"HILL", 24.0F, 10.0F, 30.0F};
    drawText(image, Rgb8{255, 255, 255}, std::span<const PlacedRun>(&run, 1));
    int full = 0;
    int partial = 0;
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        if (image.pixels[i] == 255) {
            ++full;
        } else if (image.pixels[i] > 0) {
            ++partial;
        }
        CHECK(image.pixels[i] == image.pixels[i + 1]);
        CHECK(image.pixels[i + 3] == 255);
    }
    CHECK(full > 100);   // solid stems
    CHECK(partial > 20); // antialiased edges
    // Nothing above the cap height nor left of the pen.
    for (int y = 0; y < 30 - static_cast<int>(font.capHeight(24.0F)) - 2; ++y) {
        for (int x = 0; x < 120; ++x) {
            REQUIRE(image.pixels[(static_cast<std::size_t>(y) * 120 + x) * 4] == 0);
        }
    }
    const ImageData before = image;
    drawText(image, Rgb8{255, 255, 255}, {});
    CHECK(image.pixels == before.pixels);
}
