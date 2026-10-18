// I3 step 5: sample spines, generated labels and a fitted cover written as PNG files for a visual review (hidden):
//   build/msvc-x64-debug/bin/rv_tests.exe "[label-preview]"
// writes build/<preset>/label-preview/*.png: each layer's level 0 as drawn and after its encoding side by side,
// enlarged (nearest neighbour). Every title and picture is invented.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <tuple>

#include <rv/assets/collection_labels.hpp>
#include <rv/assets/cover_fit.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>

#include "cover_test_support.hpp"

using namespace rv::assets;

namespace {

/// Two RGBA images of one size side by side with a one-pixel gap, each pixel `scale` times, as RGB.
ImageData sideBySide(const ImageData& before, const ImageData& after, int scale) {
    ImageData out = rv::test::covers::blankImage((before.width * 2 + 1) * scale, before.height * scale, 3, 255);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const int column = x / scale;
            if (column == before.width) {
                continue;
            }
            const ImageData& source = column < before.width ? before : after;
            const int sx = column < before.width ? column : column - before.width - 1;
            const std::uint8_t* in =
                source.pixels.data() + (static_cast<std::size_t>(y / scale) * source.width + sx) * 4;
            std::uint8_t* pixel = out.pixels.data() + (static_cast<std::size_t>(y) * out.width + x) * 3;
            std::copy(in, in + 3, pixel);
        }
    }
    return out;
}

} // namespace

TEST_CASE("assets/label_preview: sample layers for a visual review", "[.label-preview]") {
    const auto& bundled = rv::test::covers::bundledFonts();
    const LabelFonts fonts{&bundled.title, &bundled.detail};
    const std::filesystem::path out = rv::core::executablePath().parent_path().parent_path() / "label-preview";
    std::filesystem::create_directories(out);
    const char* titles[] = {
        "Comet Courier",
        "Starlight Brigade - The Clockwork Armada",
        "Harbor Lights : Return of the Night Ferry",
        "The Extraordinarily Long Adventures of Captain Pemberton Across the Seven Floating Archipelagos",
        "Caf\xC3\xA9 Se\xC3\xB1or \xC3\x85ngstr\xC3\xB6m",
        "Robo Kid \xE3\x83\xAD\xE3\x83\x9C",
    };
    std::uint64_t index = 0;
    for (const char* title : titles) {
        for (const TextureFormat format : {TextureFormat::Bc1Srgb, TextureFormat::Rgba8Srgb}) {
            const std::string suffix = std::to_string(index) + "_" + std::string(toString(format)) + ".png";
            auto spine = buildSpineLayer(fonts, SpineRequest{title, "", std::nullopt, index}, format);
            auto spinePicture = drawSpine(fonts, SpineRequest{title, "", std::nullopt, index}, format);
            REQUIRE(spine.has_value());
            REQUIRE(spinePicture.has_value());
            REQUIRE(writePng(out / ("spine_" + suffix),
                             sideBySide(spinePicture->picture, decodeLevel(spine->texture, 0), 4)));
            const FrontLabelRequest request{title, "", "Fantasy Home System", "1994", index};
            auto front = buildFrontLabelLayer(fonts, request, format);
            auto frontPicture = drawFrontLabel(fonts, request, format);
            REQUIRE(front.has_value());
            REQUIRE(frontPicture.has_value());
            REQUIRE(writePng(out / ("front_" + suffix),
                             sideBySide(frontPicture->picture, decodeLevel(front->texture, 0), 3)));
        }
        ++index;
    }
    for (const auto& [name, width, height] : {std::tuple{"landscape", 680, 497}, std::tuple{"portrait", 476, 864}}) {
        const ImageData source = rv::test::covers::makeCover(width, height, 4);
        auto fitted = fitCover(source, 256, 188);
        auto layer = buildCoverLayer(encodePng(source).value(), TextureFormat::Bc1Srgb, name);
        REQUIRE(fitted.has_value());
        REQUIRE(layer.has_value());
        REQUIRE(writePng(out / (std::string("cover_") + name + ".png"),
                         sideBySide(fitted->picture, decodeLevel(layer->texture, 0), 2)));
    }
    WARN("written to " << rv::core::pathToUtf8(out));
}
