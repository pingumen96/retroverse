#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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

TEST_CASE("assets/test_patterns: named patterns", "[assets][patterns]") {
    const auto names = rv::assets::patternNames();
    CHECK(std::vector<std::string_view>(names.begin(), names.end()) ==
          std::vector<std::string_view>{"color_bars", "checker", "cover"});

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

    const auto unknown = rv::assets::makeNamedPattern("plasma", 16, 16);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == ErrorCode::NotFound);
    CHECK(rv::assets::makeNamedPattern("checker", 0, 16).error().code == ErrorCode::InvalidArgument);
}
