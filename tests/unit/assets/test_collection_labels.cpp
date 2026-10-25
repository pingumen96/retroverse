// I3 step 5: generated spines and front labels (rv/assets/collection_labels.hpp, ADR-0011 §5): line splits and the
// ellipsis as a table, long, Unicode and Japanese titles, the spine's fit, the text's contrast after BC1, pixels
// hashed. Every title here is invented.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <rv/assets/bc1.hpp>
#include <rv/assets/collection_labels.hpp>
#include <rv/assets/derived_texture_cache.hpp>
#include <rv/assets/image_io.hpp>

#include "cover_test_support.hpp"

using namespace rv::assets;
using rv::test::covers::pixelHash;

namespace {

LabelFonts fonts() {
    const auto& bundled = rv::test::covers::bundledFonts();
    return {&bundled.title, &bundled.detail};
}

const FontFace& titleFont() {
    return rv::test::covers::bundledFonts().title;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += (out.empty() ? "" : " | ") + line;
    }
    return out;
}

/// The contrast between the background and the strongest text pixel of a decoded layer.
double textContrast(const ImageData& image, Rgb8 background) {
    double best = 1.0;
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        best = std::max(best, contrastRatio(background, {image.pixels[i], image.pixels[i + 1], image.pixels[i + 2]}));
    }
    return best;
}

/// The most common colour of a decoded layer (its background).
Rgb8 mostCommon(const ImageData& image) {
    std::map<std::uint32_t, int> counts;
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        ++counts[(static_cast<std::uint32_t>(image.pixels[i]) << 16) | (image.pixels[i + 1] << 8) | image.pixels[i + 2]];
    }
    const auto best = std::max_element(counts.begin(), counts.end(),
                                       [](const auto& a, const auto& b) { return a.second < b.second; });
    return {static_cast<std::uint8_t>(best->first >> 16), static_cast<std::uint8_t>(best->first >> 8),
            static_cast<std::uint8_t>(best->first)};
}

} // namespace

TEST_CASE("assets/collection_labels: contrast and the label colours", "[assets][collection_labels]") {
    CHECK(contrastRatio({0, 0, 0}, {255, 255, 255}) > 20.9);
    CHECK(contrastRatio({255, 255, 255}, {0, 0, 0}) == contrastRatio({0, 0, 0}, {255, 255, 255}));
    CHECK(contrastRatio({0x77, 0x77, 0x77}, {0x77, 0x77, 0x77}) == 1.0);
    // Every palette colour and a grey ramp reach the target with one of the two text colours.
    std::vector<Rgb8> bases(kDefaultLabelPalette.begin(), kDefaultLabelPalette.end());
    for (int v = 0; v <= 255; v += 15) {
        bases.push_back({static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v)});
    }
    bases.push_back({0xFF, 0x00, 0x00});
    bases.push_back({0x00, 0x80, 0xFF});
    for (const Rgb8 base : bases) {
        const LabelColours colours = labelColours(base);
        INFO(toHexCode(base) << " -> " << toHexCode(colours.background) << " on " << toHexCode(colours.text));
        CHECK(colours.contrast >= kLabelContrastTarget);
        CHECK((colours.text == kLabelTextLight || colours.text == kLabelTextDark));
    }
    // A colour that already contrasts is kept as it is.
    CHECK(labelColours({0x25, 0x2A, 0x2E}).background == Rgb8{0x25, 0x2A, 0x2E});
    // The palette's colour depends on the seed only.
    CHECK(paletteColour(7) == paletteColour(7));
    std::vector<Rgb8> seen;
    for (std::uint64_t seed = 0; seed < 64; ++seed) {
        const Rgb8 colour = paletteColour(seed);
        if (std::find(seen.begin(), seen.end(), colour) == seen.end()) {
            seen.push_back(colour);
        }
    }
    CHECK(seen.size() == kDefaultLabelPalette.size());
}

TEST_CASE("assets/collection_labels: spine lines, splits and ellipses", "[assets][collection_labels]") {
    struct Case {
        const char* title;
        LineSplit split;
        const char* lines;
        bool ellipsized;
    };
    const Case cases[] = {
        {"Comet Courier", LineSplit::OneLine, "Comet Courier", false},
        {"Lanterns of the Quiet Valley", LineSplit::OneLine, "Lanterns of the Quiet Valley", false},
        {"Starlight Brigade - The Clockwork Armada", LineSplit::Separator, "Starlight Brigade | The Clockwork Armada",
         false},
        {"Harbor Lights : Return of the Night Ferry", LineSplit::Separator, "Harbor Lights: | Return of the Night Ferry",
         false},
        {"Moonbase Mechanic: Gravity Hammer Edition", LineSplit::Separator, "Moonbase Mechanic: | Gravity Hammer Edition",
         false},
        {"The Extraordinarily Long Adventures of Captain Pemberton Across the Seven Floating Archipelagos",
         LineSplit::Words, "", true},
        {"Supercalifragilisticexpialidociousnessesquadrupled", LineSplit::OneLine, "", true},
        {"   Spaced    Out\tTitle  ", LineSplit::OneLine, "Spaced Out Title", false},
    };
    for (const Case& c : cases) {
        const TitleLayout layout = layoutSpineTitle(titleFont(), c.title, "fallback title", TextureFormat::Bc1Srgb);
        INFO(c.title << " -> [" << joinLines(layout.lines) << "] at " << layout.pixelHeight << " px, "
                     << toString(layout.split));
        CHECK(layout.split == c.split);
        CHECK(layout.ellipsized == c.ellipsized);
        CHECK_FALSE(layout.usedFallbackTitle);
        if (*c.lines != '\0') {
            CHECK(joinLines(layout.lines) == c.lines);
        }
        if (c.ellipsized) {
            CHECK(layout.lines.back().ends_with("\xE2\x80\xA6")); // U+2026
        }
        // The spine's fit: every line within the length less its end margins, at a size within the rules.
        for (const std::string& line : layout.lines) {
            CHECK(titleFont().layout(decodeUtf8(line), layout.pixelHeight).width <= 256.0F - 2.0F * 13.0F + 1e-3F);
        }
        CHECK(layout.pixelHeight >= 0.34F * 40.0F - 1e-4F);
        CHECK(layout.pixelHeight <= 0.62F * 40.0F + 1e-4F);
        if (layout.lines.size() == 2) {
            CHECK(layout.pixelHeight <= 0.45F * 40.0F + 1e-4F);
        } else {
            CHECK(layout.pixelHeight >= 0.45F * 40.0F - 1e-4F);
        }
    }
}

TEST_CASE("assets/collection_labels: Unicode and Japanese titles", "[assets][collection_labels]") {
    SECTION("accented Latin is drawn as it is") {
        const TitleLayout layout = layoutSpineTitle(titleFont(), "Caf\xC3\xA9 Se\xC3\xB1or \xC3\x85ngstr\xC3\xB6m",
                                                    "Cafe Senor Angstrom", TextureFormat::Bc1Srgb);
        CHECK(layout.replacementGlyphs == 0);
        CHECK_FALSE(layout.usedFallbackTitle);
        CHECK(layout.lines.front() == "Caf\xC3\xA9 Se\xC3\xB1or \xC3\x85ngstr\xC3\xB6m");
    }
    SECTION("a title in Japanese only uses the cleaned file-name title") {
        // "Tsuki no Yuubinya" in katakana and kanji, invented.
        const TitleLayout layout =
            layoutSpineTitle(titleFont(), "\xE6\x9C\x88\xE3\x81\xAE\xE3\x83\x9D\xE3\x82\xB9\xE3\x83\x88",
                             "Tsuki no Posuto (Japan)", TextureFormat::Bc1Srgb);
        CHECK(layout.usedFallbackTitle);
        CHECK(layout.replacementGlyphs == 0);
        CHECK(joinLines(layout.lines) == "Tsuki no Posuto (Japan)");
    }
    SECTION("a mixed title keeps its Latin part and draws replacement glyphs for the rest") {
        const TitleLayout layout = layoutSpineTitle(titleFont(), "Robo Kid \xE3\x83\xAD\xE3\x83\x9C",
                                                    "Robo Kid (Japan)", TextureFormat::Bc1Srgb);
        CHECK_FALSE(layout.usedFallbackTitle);
        CHECK(layout.replacementGlyphs == 2);
    }
    SECTION("nothing drawable anywhere: the title with replacement glyphs") {
        const TitleLayout layout = layoutSpineTitle(titleFont(), "\xE3\x83\xAD\xE3\x83\x9C", "\xE3\x83\xAD",
                                                    TextureFormat::Bc1Srgb);
        CHECK_FALSE(layout.usedFallbackTitle);
        CHECK(layout.replacementGlyphs == 2);
    }
    SECTION("an empty title uses the fallback, and nothing at all draws nothing") {
        CHECK(layoutSpineTitle(titleFont(), "", "Rescue Title", TextureFormat::Bc1Srgb).lines.front() == "Rescue Title");
        CHECK(layoutSpineTitle(titleFont(), "  ", "", TextureFormat::Bc1Srgb).lines.empty());
    }
}

TEST_CASE("assets/collection_labels: front label lines", "[assets][collection_labels]") {
    struct Case {
        const char* title;
        const char* lines;
        LineSplit split;
        bool ellipsized;
    };
    const Case cases[] = {
        {"Comet Courier", "Comet Courier", LineSplit::OneLine, false},
        {"Starlight Brigade - The Clockwork Armada", "Starlight Brigade | The Clockwork Armada", LineSplit::Separator,
         false},
        {"Harbor Lights : Return of the Night Ferry", "Harbor Lights: | Return of the Night Ferry",
         LineSplit::Separator, false},
        {"Lanterns of the Quiet Valley and the Hollow Mountain",
         "Lanterns of the | Quiet Valley and the | Hollow Mountain", LineSplit::Words, false},
        {"The Extraordinarily Long Adventures of Captain Pemberton Across the Seven Floating Archipelagos", "",
         LineSplit::Words, false},
        {"The Extraordinarily Long and Winding Adventures of Captain Pemberton Across the Seven Floating Archipelagos of "
         "the Eastern Dawn Sea and Beyond",
         "", LineSplit::Words, true},
        {"Supercalifragilisticexpialidociousnessesquadrupledandthensome", "", LineSplit::Words, true},
    };
    for (const Case& c : cases) {
        const TitleLayout layout = layoutFrontTitle(titleFont(), c.title, "", TextureFormat::Bc1Srgb);
        INFO(c.title << " -> [" << joinLines(layout.lines) << "] at " << layout.pixelHeight << " px");
        if (!std::string_view(c.lines).empty()) {
            CHECK(joinLines(layout.lines) == c.lines);
        }
        CHECK(layout.lines.size() <= 3);
        CHECK(layout.split == c.split);
        CHECK(layout.ellipsized == c.ellipsized);
        for (const std::string& line : layout.lines) {
            CHECK(titleFont().layout(decodeUtf8(line), layout.pixelHeight).width <= 216.0F + 1e-3F);
        }
    }
}

TEST_CASE("assets/collection_labels: spine text keeps 4.5:1 after BC1", "[assets][collection_labels]") {
    std::vector<SpineRequest> requests;
    for (const Rgb8 colour : kDefaultLabelPalette) {
        requests.push_back({"Starlight Brigade - The Clockwork Armada", "", colour, 0});
    }
    for (const Rgb8 colour : {Rgb8{0xFF, 0x00, 0x00}, Rgb8{0x80, 0x80, 0x80}, Rgb8{0x00, 0xFF, 0x40},
                              Rgb8{0xFF, 0xFF, 0x00}, Rgb8{0x77, 0x77, 0x77}}) {
        requests.push_back({"Comet Courier", "", colour, 0});
    }
    for (std::uint64_t seed = 0; seed < 8; ++seed) {
        requests.push_back({"Harbor Lights", "", std::nullopt, seed});
    }
    for (const SpineRequest& request : requests) {
        for (const TextureFormat format : {TextureFormat::Bc1Srgb, TextureFormat::Rgba8Srgb}) {
            auto layer = buildSpineLayer(fonts(), request, format);
            REQUIRE(layer.has_value());
            const ImageData decoded = decodeLevel(layer->texture, 0);
            const Rgb8 background = mostCommon(decoded);
            const double contrast = textContrast(decoded, background);
            INFO(toHexCode(spineBaseColour(request)) << " in " << toString(format) << ": "
                                                     << toHexCode(background) << ", " << contrast << ":1");
            CHECK(contrast >= kMinTextContrast);
            CHECK(layer->texture.dominant == layer->colours.background);
            CHECK_FALSE(layer->texture.source.has_value());
        }
    }
}

TEST_CASE("assets/collection_labels: the spine reads top to bottom", "[assets][collection_labels]") {
    // Heavy capitals first, light dots after: the capitals land in the upper half of the spine. The dots sit on the
    // baseline, which is on the spine's left side when the glyphs' tops face +x (the text turned clockwise).
    const SpineRequest request{"MMMM ....................", "", Rgb8{0x25, 0x2A, 0x2E}, 0};
    auto picture = drawSpine(fonts(), request, TextureFormat::Bc1Srgb);
    REQUIRE(picture.has_value());
    const ImageData& image = picture->picture;
    REQUIRE(image.width == 40);
    REQUIRE(image.height == 256);
    const Rgb8 background = picture->colours.background;
    long upper = 0;
    long lower = 0;
    long dotsLeft = 0;
    long dotsRight = 0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::uint8_t* pixel = image.pixels.data() + (static_cast<std::size_t>(y) * 40 + x) * 4;
            const long ink = std::abs(pixel[0] - background.r);
            (y < 128 ? upper : lower) += ink;
            if (y >= 160) {
                (x < 20 ? dotsLeft : dotsRight) += ink;
            }
        }
    }
    CHECK(upper > lower * 2);
    CHECK(dotsLeft > 0);
    CHECK(dotsRight * 4 < dotsLeft);
}

TEST_CASE("assets/collection_labels: generated pixels are stable (hashed)", "[assets][collection_labels]") {
    // Any change of the fit, the layout, the drawing, the filters, the encoder or the font changes these: raise
    // kCoverPipelineVersion with it (the cache keys then name new entries) and update the hashes. Debug and Release
    // give the same bytes.
    const auto spine = [](const char* title, std::optional<Rgb8> colour, TextureFormat format) {
        auto picture = drawSpine(fonts(), SpineRequest{title, "", colour, 3}, format);
        REQUIRE(picture.has_value());
        return pixelHash(picture->picture);
    };
    const auto front = [](const char* title, TextureFormat format) {
        auto picture = drawFrontLabel(fonts(), FrontLabelRequest{title, "", "Fantasy Home System", "1994", 5}, format);
        REQUIRE(picture.has_value());
        return pixelHash(picture->picture);
    };
    const auto payload = [](const RvTexture& texture) {
        rv::core::Sha256 hash;
        for (const TextureLevel& level : texture.levels) {
            hash.update(level.data);
        }
        const auto digest = hash.finish();
        return rv::core::toHex(digest);
    };
    const auto encodedSpine = [&] {
        auto layer = buildSpineLayer(fonts(), SpineRequest{"Comet Courier", "", Rgb8{0x4C, 0x8B, 0xC9}, 0},
                                     TextureFormat::Bc1Srgb);
        REQUIRE(layer.has_value());
        return payload(layer->texture);
    };
    const auto encodedCover = [&](TextureFormat format) {
        auto bytes = rv::assets::encodePng(rv::test::covers::makeCover(680, 497, 77));
        REQUIRE(bytes.has_value());
        auto layer = buildCoverLayer(*bytes, format, "hash");
        REQUIRE(layer.has_value());
        return payload(layer->texture);
    };
    struct Case {
        const char* name;
        std::string actual;
        const char* expected;
    };
    const Case cases[] = {
        {"spine, one line", spine("Comet Courier", Rgb8{0x68, 0x3D, 0x42}, TextureFormat::Bc1Srgb),
         "077d1ec7e1ea5e2ea473ed11b822a30e2448077c9ec0cf72ab07da6c554ed98a"},
        {"spine, separator", spine("Starlight Brigade - The Clockwork Armada", std::nullopt, TextureFormat::Bc1Srgb),
         "954013c35e13d1fa3948a4f163f84525f61b44f8df4a1adadaff01d247ae01f4"},
        {"spine, RGBA8", spine("Comet Courier", Rgb8{0xFF, 0xC8, 0x8A}, TextureFormat::Rgba8Srgb),
         "2753fce32b1206927733a900b4f71ac854b8a3c0b83e5169665bd61fa7429508"},
        {"front, one line", front("Comet Courier", TextureFormat::Bc1Srgb),
         "235d591ac773422ede11e583a14876e9b9f32e649e0f042360fcf74cbe2b8772"},
        {"front, three lines",
         front("The Extraordinarily Long Adventures of Captain Pemberton Across the Seven Floating Archipelagos",
               TextureFormat::Bc1Srgb),
         "666daafedca4af65eba88c66b22e5cb38c75750f45b0073cca6ceefc9fd22e58"},
        {"front, RGBA8", front("Harbor Lights : Return of the Night Ferry", TextureFormat::Rgba8Srgb),
         "9cbe2ddd74c94ce5e04f3d6de2f0625aaa90acf65e74e79dc4f03a587f3975f8"},
        {"spine layer, BC1 with mips", encodedSpine(), "114a11f8be68330d2b6c1cbb1c34f06093f48140e45c2f9ddec24b2b0ffa9bf6"},
        {"cover layer, BC1 with mips", encodedCover(TextureFormat::Bc1Srgb),
         "edc1f6a6e075219b98f17f9729490f890476bb08f68b84ce336349fa01155069"},
        {"cover layer, RGBA8 with mips", encodedCover(TextureFormat::Rgba8Srgb),
         "46fc29f422ad6fbe80ff60407525a506379b61707f475c033309b8ff6f65489c"},
    };
    for (const Case& c : cases) {
        INFO(c.name << ": " << c.actual);
        CHECK(c.actual == c.expected);
    }
}

TEST_CASE("assets/collection_labels: a front label laid out at its label's shape (I4 step 2)",
          "[assets][collection_labels]") {
    // ADR-0013 §4: laid out on a canvas of the label quad's shape, then resampled once into the 256 x 188 layer, which
    // the quad stretches back. The layer's shape is I3's label to the pixel.
    const char* title = "Harbor Lights : Return of the Night Ferry";
    FrontLabelRequest i3{title, "", "Fantasy Home System", "1994", 5};
    FrontLabelRequest layer = i3;
    layer.label = {};
    auto drawnI3 = drawFrontLabel(fonts(), i3, TextureFormat::Bc1Srgb);
    auto drawnLayer = drawFrontLabel(fonts(), layer, TextureFormat::Bc1Srgb);
    REQUIRE(drawnI3.has_value());
    REQUIRE(drawnLayer.has_value());
    CHECK(drawnI3->picture.pixels == drawnLayer->picture.pixels);
    for (const LabelAspect shape : {LabelAspect{120, 120}, LabelAspect{123, 178}, LabelAspect{96, 60}}) {
        INFO(shape.key());
        for (const TextureFormat format : {TextureFormat::Bc1Srgb, TextureFormat::Rgba8Srgb}) {
            FrontLabelRequest request = i3;
            request.label = shape;
            auto drawn = drawFrontLabel(fonts(), request, format);
            REQUIRE(drawn.has_value());
            const LayerSize size = layerSize(TextureClass::Front, format);
            CHECK(drawn->picture.width == size.width);
            CHECK(drawn->picture.height == size.height);
            CHECK(drawn->picture.pixels != drawFrontLabel(fonts(), i3, format)->picture.pixels);
            // The lines fit the canvas's width.
            const CanvasSize canvas = labelCanvas(shape, size.width, size.height);
            const float k = static_cast<float>(size.width) / 256.0F;
            for (const std::string& line : drawn->layout.lines) {
                CHECK(titleFont().layout(decodeUtf8(line), drawn->layout.pixelHeight).width <=
                      static_cast<float>(canvas.width) - 40.0F * k + 1e-3F);
            }
            // Readable after BC1.
            if (format == TextureFormat::Bc1Srgb) {
                auto layerTexture = buildFrontLabelLayer(fonts(), request, format);
                REQUIRE(layerTexture.has_value());
                const ImageData decoded = decodeLevel(layerTexture->texture, 0);
                CHECK(textContrast(decoded, mostCommon(decoded)) >= kMinTextContrast);
            }
        }
    }
    // A landscape canvas lays a long title out on wider lines: never more of them.
    const TitleLayout onLayer = layoutFrontTitle(titleFont(), title, "", TextureFormat::Bc1Srgb);
    const TitleLayout onWide = layoutFrontTitle(titleFont(), title, "", TextureFormat::Bc1Srgb, LabelAspect{2, 1});
    CHECK(onWide.lines.size() <= onLayer.lines.size());
    const TitleLayout onSquare = layoutFrontTitle(titleFont(), title, "", TextureFormat::Bc1Srgb, LabelAspect{1, 1});
    CHECK(onSquare.lines == onLayer.lines); // as wide as the layer
}

TEST_CASE("assets/collection_labels: a printed spine drawn at its quad's shape (I6 step 4's review)",
          "[assets][collection_labels]") {
    // A case or box standing on its side shows a spine label as tall as its face: drawn on a canvas of that shape (the
    // title along its length, a darker band of its colour across its top 12%), then resampled once into the 40 x 256
    // layer, which the quad stretches back. The layer's shape is I3's spine to the pixel, and keeps its cache key.
    const SpineRequest i3{"Harbor Lights : Return of the Night Ferry", "", Rgb8{0x4C, 0x8B, 0xC9}, 0};
    SpineRequest layer = i3;
    layer.spine = {};
    for (const TextureFormat format : {TextureFormat::Bc1Srgb, TextureFormat::Rgba8Srgb}) {
        auto drawnI3 = drawSpine(fonts(), i3, format);
        auto drawnLayer = drawSpine(fonts(), layer, format);
        REQUIRE(drawnI3.has_value());
        REQUIRE(drawnLayer.has_value());
        CHECK(drawnI3->picture.pixels == drawnLayer->picture.pixels);
        CHECK(spineCacheKey(i3, format, fonts()).hex() == spineCacheKey(layer, format, fonts()).hex());
    }
    // The jewel case's, the keep case's, the handheld case's, the generic case's and the big box's spines (tenths of a
    // millimetre, media_families.json).
    const LabelAspect shapes[] = {{71, 1234}, {97, 1870}, {97, 1370}, {107, 1770}, {407, 2370}};
    const std::vector<SpineRequest> requests{
        i3,
        {"Comet Courier", "", Rgb8{0xFF, 0xC8, 0x8A}, 0},
        {"Starlight Brigade - The Clockwork Armada", "", std::nullopt, 7},
        {"MMMM", "", Rgb8{0x25, 0x2A, 0x2E}, 0},
    };
    for (const LabelAspect& shape : shapes) {
        INFO(shape.key());
        for (const TextureFormat format : {TextureFormat::Bc1Srgb, TextureFormat::Rgba8Srgb}) {
            INFO(toString(format));
            const LayerSize size = layerSize(TextureClass::Spine, format);
            const CanvasSize canvas = labelCanvas(shape, size.width, size.height);
            for (SpineRequest request : requests) {
                INFO(request.title);
                request.spine = shape;
                auto drawn = drawSpine(fonts(), request, format);
                REQUIRE(drawn.has_value());
                const ImageData& image = drawn->picture;
                CHECK(image.width == size.width);
                CHECK(image.height == size.height);
                // Another picture and another cache entry than the layer's spine.
                SpineRequest plain = request;
                plain.spine = {};
                CHECK(image.pixels != drawSpine(fonts(), plain, format)->picture.pixels);
                CHECK(spineCacheKey(request, format, fonts()).hex() != spineCacheKey(plain, format, fonts()).hex());
                // The line fits the canvas's length less the band and the end margins.
                const float band = std::round(static_cast<float>(canvas.height) * 0.12F);
                for (const std::string& line : drawn->layout.lines) {
                    CHECK(titleFont().layout(decodeUtf8(line), drawn->layout.pixelHeight).width <=
                          static_cast<float>(canvas.height) - band + 1e-3F);
                }
                // The band: the top rows darker than the background, no text in them.
                const Rgb8 background = drawn->colours.background;
                const auto luminance = [](const std::uint8_t* pixel) {
                    return 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2];
                };
                const int bandRows = static_cast<int>(0.10F * static_cast<float>(size.height));
                double bandMean = 0.0;
                for (int y = 1; y < bandRows; ++y) {
                    for (int x = 0; x < size.width; ++x) {
                        bandMean += luminance(image.pixels.data() + (static_cast<std::size_t>(y) * size.width + x) * 4);
                    }
                }
                bandMean /= static_cast<double>((bandRows - 1) * size.width);
                const std::uint8_t backgroundPixel[] = {background.r, background.g, background.b};
                CHECK(bandMean < luminance(backgroundPixel));
                // Readable after BC1: the text keeps its contrast with the background.
                if (format == TextureFormat::Bc1Srgb) {
                    auto encoded = buildSpineLayer(fonts(), request, format);
                    REQUIRE(encoded.has_value());
                    const ImageData decoded = decodeLevel(encoded->texture, 0);
                    CHECK(textContrast(decoded, mostCommon(decoded)) >= kMinTextContrast);
                    CHECK(encoded->texture.dominant == encoded->colours.background);
                }
            }
        }
    }
    // On a slender printed spine a long title gets a longer line than on the layer: one line, never smaller.
    const TitleLayout onLayer = layoutSpineTitle(titleFont(), i3.title, "", TextureFormat::Bc1Srgb);
    const TitleLayout onJewel =
        layoutSpineTitle(titleFont(), i3.title, "", TextureFormat::Bc1Srgb, LabelAspect{71, 1234});
    CHECK(onJewel.lines.size() <= onLayer.lines.size());
    CHECK(onJewel.pixelHeight >= onLayer.pixelHeight);
    CHECK_FALSE(onJewel.ellipsized);
}
