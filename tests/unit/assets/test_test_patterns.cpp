#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include <rv/assets/test_patterns.hpp>

using rv::ErrorCode;
using rv::assets::ImageData;
using Rgba = glm::u8vec4;

namespace {

constexpr Rgba kIvory(0xF4, 0xEF, 0xE6, 255);    // style §3 "UI testo", the title band
constexpr Rgba kCharcoal(0x17, 0x1B, 0x22, 255); // style §3 "UI neutra", border and title text

Rgba pixel(const ImageData& image, int x, int y) {
    const std::size_t offset =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4u;
    return Rgba(image.pixels[offset], image.pixels[offset + 1], image.pixels[offset + 2], image.pixels[offset + 3]);
}

bool isRgba(const ImageData& image, int width, int height) {
    return image.isValid() && image.width == width && image.height == height && image.channels == 4;
}

/// Rows whose central 3/4 are entirely ivory: the title band outside the glyphs.
std::vector<int> ivoryRows(const ImageData& image) {
    std::vector<int> rows;
    for (int y = 0; y < image.height; ++y) {
        bool ivory = true;
        for (int x = image.width / 8; x < image.width * 7 / 8 && ivory; ++x) {
            ivory = pixel(image, x, y) == kIvory;
        }
        if (ivory) {
            rows.push_back(y);
        }
    }
    return rows;
}

int countColor(const ImageData& image, int y0, int y1, Rgba color) {
    int count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = image.width / 8; x < image.width * 7 / 8; ++x) {
            count += pixel(image, x, y) == color ? 1 : 0;
        }
    }
    return count;
}

/// One line of title text: the glyphs it holds and the font scale (pixels per font pixel).
struct TextLine {
    int glyphs = 0;
    int scale = 0;
    bool operator==(const TextLine&) const = default;
};

/// Lines of charcoal glyphs in rows [y0, y1) of a cover, inside its border (min(width, height) / 24): each run of rows
/// holding glyph pixels is a line 7 font pixels tall, its width (the 6-pixel advance per glyph, less the last gap,
/// give or take the empty edge columns of some glyphs) gives its glyph count.
std::vector<TextLine> textLines(const ImageData& cover, int y0, int y1) {
    const int border = std::max(1, std::min(cover.width, cover.height) / 24);
    std::vector<TextLine> lines;
    int runStart = -1;
    int minX = cover.width;
    int maxX = -1;
    for (int y = y0; y <= y1; ++y) {
        bool glyphRow = false;
        for (int x = border; y < y1 && x < cover.width - border; ++x) {
            if (pixel(cover, x, y) == kCharcoal) {
                glyphRow = true;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
            }
        }
        if (glyphRow && runStart < 0) {
            runStart = y;
        } else if (!glyphRow && runStart >= 0) {
            const int scale = (y - runStart) / 7;
            if (scale > 0) {
                lines.push_back({(maxX - minX + 1 + scale + 3 * scale) / (6 * scale), scale});
            }
            runStart = -1;
            minX = cover.width;
            maxX = -1;
        }
    }
    return lines;
}

/// The title lines of a cover: those inside its ivory band.
std::vector<TextLine> titleLines(const ImageData& cover) {
    const std::vector<int> rows = ivoryRows(cover);
    if (rows.empty()) {
        return {};
    }
    return textLines(cover, rows.front(), rows.back() + 1);
}

} // namespace

TEST_CASE("assets/test_patterns: sizes and invalid dimensions", "[assets][patterns]") {
    CHECK(isRgba(rv::assets::makeColorBars(320, 240), 320, 240));
    CHECK(isRgba(rv::assets::makeCheckerboard(64, 32, 4, kIvory, kCharcoal), 64, 32));
    CHECK(isRgba(rv::assets::makeCoverPlaceholder(256, 360, "Placeholder", 7), 256, 360));
    CHECK(isRgba(rv::assets::makeSolid(3, 5, kIvory), 3, 5));
    for (const auto& [width, height] : {std::pair{1, 1}, std::pair{2, 3}, std::pair{7, 7}, std::pair{1, 40}}) {
        CAPTURE(width, height);
        CHECK(isRgba(rv::assets::makeColorBars(width, height), width, height));
        CHECK(isRgba(rv::assets::makeCoverPlaceholder(width, height, "Tiny", 1), width, height));
        CHECK(isRgba(rv::assets::makeCheckerboard(width, height, 8, kIvory, kCharcoal), width, height));
    }

    CHECK_FALSE(rv::assets::makeColorBars(0, 10).isValid());
    CHECK_FALSE(rv::assets::makeSolid(-1, 5, kIvory).isValid());
    CHECK_FALSE(rv::assets::makeCheckerboard(10, 0, 2, kIvory, kCharcoal).isValid());
    CHECK_FALSE(rv::assets::makeCoverPlaceholder(rv::assets::kMaxImageDimension + 1, 1, "X", 0).isValid());
}

TEST_CASE("assets/test_patterns: color bars layout", "[assets][patterns]") {
    const ImageData bars = rv::assets::makeColorBars(70, 60);
    const std::vector<Rgba> top = {{191, 191, 191, 255}, {191, 191, 0, 255}, {0, 191, 191, 255}, {0, 191, 0, 255},
                                   {191, 0, 191, 255},   {191, 0, 0, 255},   {0, 0, 191, 255}};
    const std::vector<Rgba> reverse = {{0, 0, 191, 255},   {0, 0, 0, 255}, {191, 0, 191, 255},  {0, 0, 0, 255},
                                       {0, 191, 191, 255}, {0, 0, 0, 255}, {191, 191, 191, 255}};
    for (int bar = 0; bar < 7; ++bar) {
        CAPTURE(bar);
        const int x = bar * 10 + 5;
        CHECK(pixel(bars, x, 0) == top[static_cast<std::size_t>(bar)]);
        CHECK(pixel(bars, x, 39) == top[static_cast<std::size_t>(bar)]);     // top two thirds: rows 0..39
        CHECK(pixel(bars, x, 40) == reverse[static_cast<std::size_t>(bar)]); // reverse strip: rows 40..44
        CHECK(pixel(bars, x, 44) == reverse[static_cast<std::size_t>(bar)]);
    }
    // Bottom strip: grey ramp from black to white.
    CHECK(pixel(bars, 0, 45) == Rgba(0, 0, 0, 255));
    CHECK(pixel(bars, 69, 59) == Rgba(255, 255, 255, 255));
    for (int x = 1; x < 70; ++x) {
        const Rgba left = pixel(bars, x - 1, 50);
        const Rgba right = pixel(bars, x, 50);
        CHECK(right.r >= left.r);
        CHECK(right.r == right.g);
        CHECK(right.g == right.b);
    }
    CHECK(rv::assets::makeColorBars(70, 60).pixels == bars.pixels);
}

TEST_CASE("assets/test_patterns: checkerboard and solid", "[assets][patterns]") {
    const Rgba a(255, 0, 0, 255);
    const Rgba b(0, 0, 255, 128);
    const ImageData checker = rv::assets::makeCheckerboard(64, 32, 4, a, b);
    CHECK(pixel(checker, 0, 0) == a);
    CHECK(pixel(checker, 15, 7) == a);
    CHECK(pixel(checker, 16, 0) == b);
    CHECK(pixel(checker, 0, 8) == b);
    CHECK(pixel(checker, 16, 8) == a);
    CHECK(pixel(checker, 63, 31) == a);

    const ImageData single = rv::assets::makeCheckerboard(8, 8, 0, a, b); // clamped to one cell
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            CHECK(pixel(single, x, y) == a);
        }
    }
    const ImageData finest = rv::assets::makeCheckerboard(4, 4, 100, a, b); // clamped to 4 x 4 cells
    CHECK(pixel(finest, 0, 0) == a);
    CHECK(pixel(finest, 1, 0) == b);
    CHECK(pixel(finest, 1, 1) == a);

    const ImageData solid = rv::assets::makeSolid(5, 4, b);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 5; ++x) {
            CHECK(pixel(solid, x, y) == b);
        }
    }
}

TEST_CASE("assets/test_patterns: cover placeholders are deterministic and seed dependent", "[assets][patterns]") {
    const ImageData first = rv::assets::makeCoverPlaceholder(256, 160, "SUPER PLACEHOLDER WORLD", 3);
    const ImageData again = rv::assets::makeCoverPlaceholder(256, 160, "SUPER PLACEHOLDER WORLD", 3);
    CHECK(first.pixels == again.pixels);

    for (std::uint32_t seed = 0; seed < 8; ++seed) {
        CAPTURE(seed);
        CHECK(rv::assets::makeCoverPlaceholder(128, 128, "Same Title", seed).pixels !=
              rv::assets::makeCoverPlaceholder(128, 128, "Same Title", seed + 1).pixels);
    }

    // A dark border frames the art.
    CHECK(pixel(first, 0, 0) == kCharcoal);
    CHECK(pixel(first, 255, 159) == kCharcoal);
    CHECK(pixel(first, 128, 0) == kCharcoal);
}

TEST_CASE("assets/test_patterns: cover placeholders carry a light title band with text", "[assets][patterns]") {
    for (const auto& [width, height] : {std::pair{256, 160}, std::pair{256, 360}, std::pair{512, 512}}) {
        for (std::uint32_t seed = 0; seed < 6; ++seed) {
            CAPTURE(width, height, seed);
            const ImageData cover = rv::assets::makeCoverPlaceholder(width, height, "Super Placeholder World", seed);
            const std::vector<int> rows = ivoryRows(cover);
            REQUIRE(rows.size() >= 3);
            const int bandTop = rows.front();
            const int bandBottom = rows.back();
            CHECK(bandBottom - bandTop >= height / 8);
            CHECK(bandTop > height / 3); // the band sits in the lower part, under the artwork
            CHECK(countColor(cover, bandTop, bandBottom, kCharcoal) > 50); // glyph pixels inside the band

            // Without a title the band is empty.
            const ImageData untitled = rv::assets::makeCoverPlaceholder(width, height, "", seed);
            CHECK(countColor(untitled, bandTop, bandBottom + 1, kCharcoal) == 0);
            CHECK(countColor(untitled, bandTop, bandBottom + 1, kIvory) ==
                  (bandBottom + 1 - bandTop) * (width * 7 / 8 - width / 8));
        }
    }
}

TEST_CASE("assets/test_patterns: cover titles change only the band", "[assets][patterns]") {
    const ImageData a = rv::assets::makeCoverPlaceholder(200, 300, "ALPHA", 11);
    const ImageData b = rv::assets::makeCoverPlaceholder(200, 300, "OMEGA", 11);
    REQUIRE(a.pixels != b.pixels);
    const std::vector<int> rows = ivoryRows(a);
    REQUIRE_FALSE(rows.empty());
    for (int y = 0; y < rows.front(); ++y) {
        for (int x = 0; x < 200; ++x) {
            CHECK(pixel(a, x, y) == pixel(b, x, y));
        }
    }

    // Lowercase is uppercased and Latin-1 accents fold to their base letter.
    CHECK(rv::assets::makeCoverPlaceholder(200, 300, "café Été", 4).pixels ==
          rv::assets::makeCoverPlaceholder(200, 300, "CAFE ETE", 4).pixels);
    // Unsupported characters render as '?', extra whitespace collapses; ROM-name brackets have glyphs.
    CHECK(rv::assets::makeCoverPlaceholder(200, 300, "A  @  B", 4).pixels ==
          rv::assets::makeCoverPlaceholder(200, 300, "A ? B", 4).pixels);
    CHECK(rv::assets::makeCoverPlaceholder(200, 300, "ROGUE [!]", 4).pixels !=
          rv::assets::makeCoverPlaceholder(200, 300, "ROGUE ?!?", 4).pixels);

    // Very long titles are wrapped, truncated and never overflow the image.
    const std::string longTitle(300, 'W');
    const ImageData crowded = rv::assets::makeCoverPlaceholder(64, 64, longTitle + " " + longTitle, 2);
    CHECK(isRgba(crowded, 64, 64));
    CHECK(rv::assets::makeCoverPlaceholder(64, 64, std::string("\xFF\xFE broken utf8 \xC3", 17), 2).isValid());
}

TEST_CASE("assets/test_patterns: titles wrap between words at the largest scale that keeps them whole",
          "[assets][patterns]") {
    // The test room's east poster (a 352 x 512 picture): at scale 6 a line holds 8 glyphs, so "MOONLIGHT" would break
    // into "MOONLIGH" / "T"; scale 5 keeps both words whole.
    CHECK(titleLines(rv::assets::makeCoverPlaceholder(352, 512, "MOONLIGHT DERBY", 1511)) ==
          std::vector<TextLine>{{9, 5}, {5, 5}});
    // A word longer than a line at every scale is split as a last resort, at the largest scale that fits: never
    // dropped.
    CHECK(titleLines(rv::assets::makeCoverPlaceholder(128, 64, "SUPERCALIFRAGILISTIC", 3)) ==
          std::vector<TextLine>{{19, 1}, {1, 1}});
    // Room signs follow the same rule (their white letters recolored as glyphs for textLines): at scale 3 a line holds
    // 8 glyphs, scale 2 keeps both words whole.
    ImageData sign = rv::assets::makeSignLettering(160, 64, "MOONLIGHT DERBY");
    REQUIRE(sign.isValid());
    for (std::size_t i = 0; i < sign.pixels.size(); i += 4) {
        if (sign.pixels[i] == 255) {
            sign.pixels[i] = kCharcoal.r;
            sign.pixels[i + 1] = kCharcoal.g;
            sign.pixels[i + 2] = kCharcoal.b;
        }
    }
    CHECK(textLines(sign, 0, sign.height) == std::vector<TextLine>{{9, 2}, {5, 2}});
}

TEST_CASE("assets/test_patterns: cartridge labels keep long titles legible", "[assets][patterns]") {
    // A cartridge label of the test room: 10.1 x 6.1 cm at 2560 pixels per meter. Seen from 1.3 m at 1920 x 1080 it
    // spans about 60 pixels in height, so its glyphs need 3 texels per font pixel (8 screen pixels tall) to stay
    // legible: two lines of up to a dozen glyphs get them, short single-line titles more.
    CHECK(rv::assets::patternLongSideFor(0.101f) == 259);
    CHECK(titleLines(rv::assets::makeCoverPlaceholder(259, 156, "PIXEL KNIGHT CHRONICLES", 138)) ==
          std::vector<TextLine>{{12, 3}, {10, 3}});
    CHECK(titleLines(rv::assets::makeCoverPlaceholder(259, 156, "SPELLBOUND SPROUT", 1766)) ==
          std::vector<TextLine>{{10, 3}, {6, 3}});
    CHECK(titleLines(rv::assets::makeCoverPlaceholder(259, 156, "EMERALD", 1729)) == std::vector<TextLine>{{7, 5}});
    // The band grows on wide labels only (above half the label's inner height: its border is 6 pixels, 156 / 24, and
    // its inner height 144); portrait covers keep it from 58% to 88% of their inner height (the 352 x 512 poster: a
    // 14-pixel border, an inner height of 484).
    const ImageData label = rv::assets::makeCoverPlaceholder(259, 156, "TITLE", 1);
    const std::vector<int> labelBand = ivoryRows(label);
    REQUIRE_FALSE(labelBand.empty());
    CHECK(labelBand.front() < 6 + 144 * 50 / 100);
    const ImageData poster = rv::assets::makeCoverPlaceholder(352, 512, "TITLE", 1);
    const std::vector<int> posterBand = ivoryRows(poster);
    REQUIRE_FALSE(posterBand.empty());
    CHECK(posterBand.front() == 14 + 484 * 58 / 100);
    CHECK(posterBand.back() == 14 + 484 * 88 / 100 - 1);
}

TEST_CASE("assets/test_patterns: named patterns", "[assets][patterns]") {
    const auto names = rv::assets::patternNames();
    CHECK(std::vector<std::string_view>(names.begin(), names.end()) ==
          std::vector<std::string_view>{"color_bars", "checker", "cover", "sign", "print"});

    const auto bars = rv::assets::makeNamedPattern("color_bars", 70, 60);
    REQUIRE(bars.has_value());
    CHECK(bars->pixels == rv::assets::makeColorBars(70, 60).pixels);

    const auto checker = rv::assets::makeNamedPattern("checker", 64, 64);
    REQUIRE(checker.has_value());
    CHECK(pixel(*checker, 0, 0) == kIvory);
    CHECK(pixel(*checker, 8, 0) == Rgba(0x38, 0x37, 0x35, 255));

    const auto cover = rv::assets::makeNamedPattern("cover", 128, 96, "Title", 9);
    REQUIRE(cover.has_value());
    CHECK(cover->pixels == rv::assets::makeCoverPlaceholder(128, 96, "Title", 9).pixels);

    const auto sign = rv::assets::makeNamedPattern("sign", 256, 48, "16-BIT CORNER", 9);
    REQUIRE(sign.has_value());
    CHECK(sign->pixels == rv::assets::makeSignLettering(256, 48, "16-BIT CORNER").pixels);

    const auto print = rv::assets::makeNamedPattern("print", 96, 128, "SAVE POINT", 9);
    REQUIRE(print.has_value());
    CHECK(print->pixels == rv::assets::makePrintPlaceholder(96, 128, "SAVE POINT", 9).pixels);

    const auto unknown = rv::assets::makeNamedPattern("plasma", 16, 16);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == ErrorCode::NotFound);
    CHECK(rv::assets::makeNamedPattern("checker", 0, 16).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("assets/test_patterns: a hall's print is a cover in a muted palette", "[assets][patterns]") {
    // The same design and layout (the title band where the cover has it), its colours less saturated: the mean
    // chroma (max - min channel) of the picture lower than the cover's over a run of seeds, and never a pure accent.
    double coverChroma = 0.0;
    double printChroma = 0.0;
    std::size_t translucent = 0;
    for (std::uint32_t seed = 0; seed < 24; ++seed) {
        const ImageData cover = rv::assets::makeCoverPlaceholder(96, 128, "SAVE POINT", seed);
        const ImageData print = rv::assets::makePrintPlaceholder(96, 128, "SAVE POINT", seed);
        REQUIRE(isRgba(print, 96, 128));
        CHECK(print.pixels != cover.pixels);
        CHECK(rv::assets::makePrintPlaceholder(96, 128, "SAVE POINT", seed).pixels == print.pixels);
        for (std::size_t i = 0; i < print.pixels.size(); i += 4) {
            const auto chroma = [&](const ImageData& image) {
                const std::uint8_t* p = image.pixels.data() + i;
                return static_cast<double>(std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]}));
            };
            coverChroma += chroma(cover);
            printChroma += chroma(print);
            translucent += print.pixels[i + 3] != 255 ? 1 : 0;
        }
    }
    CHECK(translucent == 0);
    CHECK(printChroma < 0.8 * coverChroma);
}

TEST_CASE("assets/test_patterns: sign lettering is white text on black inside a margin", "[assets][patterns]") {
    constexpr int kWidth = 512;
    constexpr int kHeight = 100;
    const ImageData sign = rv::assets::makeSignLettering(kWidth, kHeight, "16-bit corner");
    REQUIRE(isRgba(sign, kWidth, kHeight));
    const Rgba black(0, 0, 0, 255);
    const Rgba white(255, 255, 255, 255);
    const int margin = kHeight / 12;
    int lit = 0;
    int minX = kWidth;
    int maxX = -1;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const Rgba color = pixel(sign, x, y);
            REQUIRE((color == black || color == white)); // two colors only: the material's color glows on black
            if (color == white) {
                ++lit;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                CHECK(x >= margin);
                CHECK(x < kWidth - margin);
                CHECK(y >= margin);
                CHECK(y < kHeight - margin);
            }
        }
    }
    CHECK(lit > 1000);
    // One line, centered, filling most of the width at the largest integer scale (lowercase is uppercased).
    CHECK(std::abs((minX + maxX) / 2 - kWidth / 2) <= 6);
    CHECK(maxX - minX > kWidth / 2);
    CHECK(sign.pixels == rv::assets::makeSignLettering(kWidth, kHeight, "16-BIT CORNER").pixels);
    // Text too long for one line wraps onto at most two; an empty text leaves the panel dark.
    CHECK(rv::assets::makeSignLettering(128, 64, "A LONGER ROOM NAME").isValid());
    const ImageData dark = rv::assets::makeSignLettering(64, 32, "");
    CHECK(std::all_of(dark.pixels.begin(), dark.pixels.end(), [](std::uint8_t value) {
        return value == 0 || value == 255;
    }));
    CHECK(countColor(dark, 0, 32, Rgba(255, 255, 255, 255)) == 0);
    CHECK_FALSE(rv::assets::makeSignLettering(0, 32, "X").isValid());
}

TEST_CASE("assets/test_patterns: pattern resolution follows the size of the surface", "[assets][patterns]") {
    using rv::assets::patternLongSideFor;
    CHECK(rv::assets::kPatternPixelsPerMeter == 2560.0f);
    CHECK(patternLongSideFor(0.101f) == 259); // a cartridge label: about half a poster's pixels
    CHECK(patternLongSideFor(0.44f) == rv::assets::kPatternLongSide);  // a poster picture
    CHECK(patternLongSideFor(0.53f) == rv::assets::kPatternLongSide);  // a CRT screen
    CHECK(patternLongSideFor(0.001f) == rv::assets::kMinPatternLongSide);
    for (const float bad :
         {0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        INFO(bad);
        CHECK(patternLongSideFor(bad) == rv::assets::kPatternLongSide);
    }
}

TEST_CASE("assets/test_patterns: pattern images follow the aspect of their surface", "[assets][patterns]") {
    using rv::assets::patternSizeForAspect;
    CHECK(rv::assets::kPatternLongSide == 512);
    CHECK(patternSizeForAspect(4.0f / 3.0f) == glm::ivec2(512, 384));   // CRT screen
    CHECK(patternSizeForAspect(0.101f / 0.061f) == glm::ivec2(512, 309)); // cartridge label
    CHECK(patternSizeForAspect(0.44f / 0.64f) == glm::ivec2(352, 512));   // poster picture
    CHECK(patternSizeForAspect(1.0f, 64) == glm::ivec2(64, 64));
    CHECK(patternSizeForAspect(2.0f, 100) == glm::ivec2(100, 50));

    // Bad aspects give a square; extreme ones keep at least one pixel; the long side stays a valid image side.
    for (const float bad :
         {0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        INFO(bad);
        CHECK(patternSizeForAspect(bad) == glm::ivec2(512, 512));
    }
    CHECK(patternSizeForAspect(1e30f) == glm::ivec2(512, 1));
    CHECK(patternSizeForAspect(1e-30f) == glm::ivec2(1, 512));
    CHECK(patternSizeForAspect(1.0f, 0) == glm::ivec2(1, 1));
    CHECK(patternSizeForAspect(1.0f, 1 << 20) == glm::ivec2(rv::assets::kMaxImageDimension));
}

TEST_CASE("assets/test_patterns: pattern keys name the pattern, its title and its seed", "[assets][patterns]") {
    CHECK(rv::assets::patternKey("cover", "RETRO RALLY 16", 101) == "pattern:cover:RETRO RALLY 16:101");
    CHECK(rv::assets::patternKey("color_bars", "", 0) == "pattern:color_bars::0");
    CHECK(rv::assets::patternKey("cover", "A", 1) != rv::assets::patternKey("cover", "A", 2));
}
