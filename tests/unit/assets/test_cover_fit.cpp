// I3 step 5: the fit rules of a cover on its label (rv/assets/cover_fit.hpp, ADR-0011 §5, style §12.2): never
// stretched, cropped by at most 8% of the cropped side, else banded with the edge's colour; and a fallback for every
// failure (rv/assets/collection_labels.hpp's buildFrontLayer()). Every image is generated here.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <rv/assets/collection_labels.hpp>
#include <rv/assets/cover_fit.hpp>
#include <rv/assets/image_io.hpp>

#include "cover_test_support.hpp"

using namespace rv::assets;
using rv::test::covers::blankImage;
using rv::test::covers::makeCover;

namespace {

constexpr double kLabelAspect = 256.0 / 188.0;

/// The picture is the label's size, opaque, and the cover inside it keeps the source's aspect.
void checkNotStretched(const FitResult& fit, int sourceWidth, int sourceHeight) {
    CHECK(fit.picture.width == 256);
    CHECK(fit.picture.height == 188);
    CHECK(fit.picture.channels == 4);
    for (std::size_t i = 3; i < fit.picture.pixels.size(); i += 4) {
        REQUIRE(fit.picture.pixels[i] == 255);
    }
    const double sourceAspect = static_cast<double>(sourceWidth) / sourceHeight;
    if (fit.mode == FitMode::Banded) {
        // Rounded to whole pixels: within half a pixel on the scaled side.
        const double expectedWidth = fit.coverHeight * sourceAspect;
        const double expectedHeight = fit.coverWidth / sourceAspect;
        CHECK((std::abs(fit.coverWidth - expectedWidth) <= 0.5 + 1e-9 ||
               std::abs(fit.coverHeight - expectedHeight) <= 0.5 + 1e-9));
        CHECK(fit.cropShare == 0.0);
    } else {
        // Cropped: what is kept of the source has the label's aspect.
        const bool cropsWidth = sourceAspect > kLabelAspect;
        const double keptAspect = cropsWidth ? sourceAspect * (1.0 - fit.cropShare)
                                             : sourceAspect / (1.0 - fit.cropShare);
        CHECK(std::abs(keptAspect - kLabelAspect) < 1e-9);
        CHECK(fit.cropShare <= kMaxCoverCropShare);
    }
}

std::vector<std::byte> png(const ImageData& image) {
    auto bytes = encodePng(image);
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

const std::uint8_t* pixelAt(const ImageData& image, int x, int y) {
    return image.pixels.data() + (static_cast<std::size_t>(y) * image.width + x) * image.channels;
}

} // namespace

TEST_CASE("assets/cover_fit: covers of every shape are fitted without stretching", "[assets][cover_fit]") {
    struct Case {
        const char* name;
        int width;
        int height;
        FitMode mode;
    };
    const Case cases[] = {
        {"the survey's landscape cover", 680, 497, FitMode::Cropped},
        {"a Japanese portrait cover", 476, 864, FitMode::Banded},
        {"a square", 300, 300, FitMode::Banded},
        {"16 px", 16, 12, FitMode::Cropped},
        {"4,096 px", 4096, 3000, FitMode::Cropped},
        {"a wide banner", 1000, 300, FitMode::Banded},
        {"exactly the label", 256, 188, FitMode::Cropped},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        auto fit = fitCover(makeCover(c.width, c.height, 5), 256, 188);
        REQUIRE(fit.has_value());
        CHECK(fit->mode == c.mode);
        checkNotStretched(*fit, c.width, c.height);
    }
    // The survey's covers lose under half a percent; the portrait ones get side bands of 76 px.
    auto landscape = fitCover(makeCover(680, 497, 5), 256, 188);
    CHECK(landscape->cropShare < 0.005);
    auto portrait = fitCover(makeCover(476, 864, 5), 256, 188);
    CHECK(portrait->coverWidth == 104);
    CHECK(portrait->coverX == 76);
    CHECK(portrait->coverHeight == 188);
    // The banner gets bands above and below.
    auto banner = fitCover(makeCover(1000, 300, 5), 256, 188);
    CHECK(banner->coverWidth == 256);
    CHECK(banner->coverY > 0);
    auto exact = fitCover(makeCover(256, 188, 5), 256, 188);
    CHECK(exact->cropShare == 0.0);
}

TEST_CASE("assets/cover_fit: the crop never exceeds 8% of the cropped side", "[assets][cover_fit]") {
    struct Case {
        int width;
        int height;
        FitMode mode;
    };
    // Wider than the label (the width cropped) and taller (the height cropped), just under and just over 8%.
    const Case cases[] = {
        {1478, 1000, FitMode::Cropped}, // 7.87% of the width
        {1482, 1000, FitMode::Banded},  // 8.12%
        {1254, 1000, FitMode::Cropped}, // 7.91% of the height
        {1251, 1000, FitMode::Banded},  // 8.13%
    };
    for (const Case& c : cases) {
        INFO(c.width << "x" << c.height);
        auto fit = fitCover(makeCover(c.width, c.height, 9), 256, 188);
        REQUIRE(fit.has_value());
        CHECK(fit->mode == c.mode);
        checkNotStretched(*fit, c.width, c.height);
        if (c.mode == FitMode::Cropped) {
            CHECK(fit->cropShare > 0.078);
        }
    }
}

TEST_CASE("assets/cover_fit: bands take the mean colour of the edges they face", "[assets][cover_fit]") {
    // A portrait cover whose left and right strips are two colours: the side bands take their mean in linear light.
    ImageData source = blankImage(100, 200, 3, 0);
    for (int y = 0; y < 200; ++y) {
        for (int x = 0; x < 100; ++x) {
            std::uint8_t* pixel = source.pixels.data() + (static_cast<std::size_t>(y) * 100 + x) * 3;
            const std::array<std::uint8_t, 3> colour =
                x < 50 ? std::array<std::uint8_t, 3>{200, 40, 40} : std::array<std::uint8_t, 3>{40, 40, 200};
            std::copy(colour.begin(), colour.end(), pixel);
        }
    }
    auto fit = fitCover(source, 256, 188);
    REQUIRE(fit.has_value());
    REQUIRE(fit->mode == FitMode::Banded);
    const auto mean = [](std::uint8_t a, std::uint8_t b) {
        return linearToSrgbByte((srgbByteToLinear(a) + srgbByteToLinear(b)) / 2.0F);
    };
    const Rgb8 expected{mean(200, 40), mean(40, 40), mean(40, 200)};
    CHECK(fit->bandColour == expected);
    const std::uint8_t* band = pixelAt(fit->picture, 2, 90);
    CHECK(Rgb8{band[0], band[1], band[2]} == expected);
    const std::uint8_t* rightBand = pixelAt(fit->picture, 253, 10);
    CHECK(Rgb8{rightBand[0], rightBand[1], rightBand[2]} == expected);
    // Inside the cover the source's colours stay (away from the resampled middle).
    const std::uint8_t* inside = pixelAt(fit->picture, fit->coverX + 5, 90);
    CHECK(std::abs(inside[0] - 200) <= 1);
}

TEST_CASE("assets/cover_fit: palette, alpha, grey and JPEG covers decode and fit", "[assets][cover_fit]") {
    SECTION("an indexed PNG with a transparent entry") {
        const std::array<std::array<std::uint8_t, 3>, 4> palette = {{{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {9, 9, 9}}};
        const std::array<std::uint8_t, 4> alpha = {255, 255, 255, 0};
        std::vector<std::uint8_t> indices(68 * 50);
        for (std::size_t i = 0; i < indices.size(); ++i) {
            indices[i] = static_cast<std::uint8_t>((i / 68) < 25 ? 0 : 3); // top red, bottom transparent
        }
        const std::vector<std::byte> bytes = rv::test::covers::makePalettePng(68, 50, palette, alpha, indices);
        FitOptions options;
        options.matte = {0x10, 0x80, 0x10};
        auto layer = buildCoverLayer(bytes, TextureFormat::Bc1Srgb, "palette", options);
        REQUIRE(layer.has_value());
        CHECK(layer->sourceWidth == 68);
        const ImageData top = decodeLevel(layer->texture, 0);
        CHECK(pixelAt(top, 128, 10)[0] >= 250);   // red
        CHECK(pixelAt(top, 128, 180)[1] >= 0x7C); // the matte shows through the transparent entry
        CHECK(pixelAt(top, 128, 180)[0] <= 0x14);
    }
    SECTION("grey with alpha, composited over the matte in linear light") {
        ImageData grey = blankImage(136, 100, 2, 255);
        for (std::size_t i = 1; i < grey.pixels.size(); i += 2) {
            grey.pixels[i] = 128; // half covered
        }
        FitOptions options;
        options.matte = {0, 0, 0};
        auto fit = fitCover(grey, 256, 188, options);
        REQUIRE(fit.has_value());
        const std::uint8_t expected = linearToSrgbByte(128.0F / 255.0F);
        CHECK(std::abs(pixelAt(fit->picture, 100, 100)[0] - expected) <= 1);
        CHECK(pixelAt(fit->picture, 100, 100)[3] == 255);
    }
    SECTION("JPEG") {
        auto jpeg = encodeJpeg(makeCover(680, 497, 21), 90);
        REQUIRE(jpeg.has_value());
        auto layer = buildCoverLayer(*jpeg, TextureFormat::Bc1Srgb, "jpeg");
        REQUIRE(layer.has_value());
        CHECK(layer->mode == FitMode::Cropped);
        CHECK(layer->texture.levels.size() == 9);
    }
    SECTION("the RGBA8 fallback fits to 128 x 94") {
        auto layer = buildCoverLayer(png(makeCover(476, 864, 3)), TextureFormat::Rgba8Srgb, "portrait");
        REQUIRE(layer.has_value());
        CHECK(layer->texture.width == 128);
        CHECK(layer->texture.height == 94);
        CHECK(layer->mode == FitMode::Banded);
    }
}

TEST_CASE("assets/cover_fit: the dominant colour is the most common one", "[assets][cover_fit]") {
    ImageData image = makeCover(256, 188, 2, 4);
    // Two thirds of the picture one colour (with a little noise inside its bin).
    for (int y = 0; y < 188; ++y) {
        for (int x = 0; x < 170; ++x) {
            std::uint8_t* pixel = image.pixels.data() + (static_cast<std::size_t>(y) * 256 + x) * 4;
            pixel[0] = static_cast<std::uint8_t>(0x60 + (x % 3));
            pixel[1] = 0x30;
            pixel[2] = 0x90;
        }
    }
    const Rgb8 dominant = dominantColour(image);
    CHECK(std::abs(dominant.r - 0x61) <= 1);
    CHECK(dominant.g == 0x30);
    CHECK(dominant.b == 0x90);
    // A region: only the right third.
    const Rgb8 right = dominantColour(image, 170, 0, 86, 188);
    CHECK_FALSE(right == dominant);
    CHECK(dominantColour(ImageData{}) == Rgb8{});
}

TEST_CASE("assets/cover_fit: a cover's black frame gives way to its colours (I3 step 16)", "[assets][cover_fit]") {
    // 60% black frame, 25% red picture, 15% grey: the red is the spine's colour, not the frame.
    ImageData image;
    image.width = 100;
    image.height = 100;
    image.channels = 4;
    image.pixels.assign(100 * 100 * 4, 0xFF);
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 100; ++x) {
            std::uint8_t* pixel = image.pixels.data() + (static_cast<std::size_t>(y) * 100 + x) * 4;
            const bool red = y >= 60 && y < 85;
            const bool grey = y >= 85;
            pixel[0] = red ? 0xC0 : grey ? 0x80 : 0x08;
            pixel[1] = red ? 0x20 : grey ? 0x80 : 0x08;
            pixel[2] = red ? 0x28 : grey ? 0x80 : 0x08;
        }
    }
    const Rgb8 dominant = dominantColour(image);
    CHECK(dominant.r == 0xC0);
    CHECK(dominant.g == 0x20);
    CHECK(dominant.b == 0x28);
    // Under 15% of colour the frame stays: a black cover with a small red title gives a black spine.
    for (int y = 60; y < 80; ++y) {
        for (int x = 0; x < 100; ++x) {
            std::uint8_t* pixel = image.pixels.data() + (static_cast<std::size_t>(y) * 100 + x) * 4;
            pixel[0] = pixel[1] = pixel[2] = 0x08;
        }
    }
    const Rgb8 frame = dominantColour(image);
    CHECK(frame.r == 0x08);
    CHECK(frame.b == 0x08);
}

TEST_CASE("assets/cover_fit: every failure falls back to the generated label", "[assets][cover_fit]") {
    const auto& fonts = rv::test::covers::bundledFonts();
    const LabelFonts labelFonts{&fonts.title, &fonts.detail};
    FrontLabelRequest label;
    label.title = "Comet Courier";
    label.systemName = "Fantasy Home System";
    label.year = "1993";
    label.seed = 4;

    const std::vector<std::byte> good = png(makeCover(680, 497, 8));
    struct Case {
        const char* name;
        std::vector<std::byte> bytes;
        rv::ErrorCode code;
    };
    std::vector<std::byte> notAnImage;
    for (const char c : std::string("this is not an image, only text")) {
        notAnImage.push_back(static_cast<std::byte>(c));
    }
    auto jpeg = encodeJpeg(makeCover(680, 497, 8), 90).value();
    const std::array<std::array<std::uint8_t, 3>, 1> grey = {{{90, 90, 90}}};
    const std::vector<std::uint8_t> wideRow(20000, 0);
    const std::vector<std::byte> tooWide = rv::test::covers::makePalettePng(20000, 1, grey, {}, wideRow);
    const Case failures[] = {
        {"a truncated PNG", std::vector<std::byte>(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2)),
         rv::ErrorCode::ParseError},
        {"a truncated JPEG", std::vector<std::byte>(jpeg.begin(), jpeg.begin() + static_cast<std::ptrdiff_t>(jpeg.size() / 2)),
         rv::ErrorCode::ParseError},
        {"not an image", notAnImage, rv::ErrorCode::Unsupported},
        {"wider than 16,384 px", tooWide, rv::ErrorCode::OutOfRange},
        {"an empty file", {}, rv::ErrorCode::InvalidArgument},
    };
    for (const Case& c : failures) {
        INFO(c.name);
        auto cover = buildCoverLayer(c.bytes, TextureFormat::Bc1Srgb, "cover");
        REQUIRE_FALSE(cover.has_value());
        CHECK(cover.error().code == c.code);
        auto front = buildFrontLayer(std::span<const std::byte>(c.bytes), label, labelFonts, TextureFormat::Bc1Srgb,
                                     "cover");
        REQUIRE(front.has_value());
        CHECK(front->origin == FrontOrigin::CoverFailed);
        REQUIRE(front->coverError.has_value());
        CHECK(front->coverError->code == c.code);
        CHECK(front->texture.width == 256);
        CHECK(front->texture.levels.size() == 9);
    }
    auto none = buildFrontLayer(std::nullopt, label, labelFonts, TextureFormat::Bc1Srgb, "cover");
    REQUIRE(none.has_value());
    CHECK(none->origin == FrontOrigin::NoCover);
    auto cover = buildFrontLayer(std::span<const std::byte>(good), label, labelFonts, TextureFormat::Bc1Srgb, "cover");
    REQUIRE(cover.has_value());
    CHECK(cover->origin == FrontOrigin::Cover);
    CHECK_FALSE(cover->coverError.has_value());
    CHECK(toString(FrontOrigin::CoverFailed) == "cover-failed");
}

TEST_CASE("assets/cover_fit: a label's shape, its key and its canvas (I4 step 2)", "[assets][cover_fit]") {
    // ADR-0013 §4: a family's label quad; 0 x 0 is the layer's own shape (I3's fit).
    CHECK(LabelAspect{}.isLayer());
    CHECK(LabelAspect{}.key().empty());
    CHECK(LabelAspect{120, 0}.isLayer());
    CHECK(LabelAspect{120, 120}.reduced() == LabelAspect{1, 1});
    CHECK(LabelAspect{120, 120}.key() == "1x1");
    CHECK(LabelAspect{180, 230}.key() == "18x23");
    CHECK(LabelAspect{123, 178}.key() == "123x178");
    // The canvas: the label's aspect, at least the layer on both sides.
    CHECK(labelCanvas({}, 256, 188) == CanvasSize{256, 188});
    CHECK(labelCanvas({1, 1}, 256, 188) == CanvasSize{256, 256});
    CHECK(labelCanvas({123, 178}, 256, 188) == CanvasSize{256, 370});
    CHECK(labelCanvas({96, 60}, 256, 188) == CanvasSize{301, 188});
    CHECK(labelCanvas({1, 1}, 128, 94) == CanvasSize{128, 128});
}

TEST_CASE("assets/cover_fit: a cover fitted to its label's shape, resampled once into the layer (I4 step 2)",
          "[assets][cover_fit]") {
    // The rules of I3's fit (never stretched, at most 8% cropped, else bands) against the label quad's shape: seen on
    // the quad, which shows the 256 x 188 layer stretched to its own shape, the cover keeps its aspect.
    struct Case {
        const char* name;
        int width;
        int height;
        LabelAspect label;
        FitMode mode;
    };
    const Case cases[] = {
        {"a square insert on a jewel case", 600, 600, {120, 120}, FitMode::Cropped},
        {"a near-square insert on a jewel case", 640, 620, {120, 120}, FitMode::Cropped},
        {"a portrait box on a jewel case", 476, 864, {120, 120}, FitMode::Banded},
        {"a keep case's cover on a keep case", 700, 1000, {123, 178}, FitMode::Cropped},
        {"a landscape cover on a keep case", 680, 497, {123, 178}, FitMode::Banded},
        {"a big box's cover", 760, 1000, {180, 230}, FitMode::Cropped},
        {"a landscape box on a 64-bit cartridge", 680, 497, {96, 60}, FitMode::Banded},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        FitOptions options;
        options.label = c.label;
        auto fit = fitCover(makeCover(c.width, c.height, 5), 256, 188, options);
        REQUIRE(fit.has_value());
        CHECK(fit->mode == c.mode);
        CHECK(fit->picture.width == 256);
        CHECK(fit->picture.height == 188);
        const double labelAspect = static_cast<double>(c.label.width) / c.label.height;
        const double sourceAspect = static_cast<double>(c.width) / c.height;
        if (fit->mode == FitMode::Cropped) {
            // What is kept of the source has the label's aspect.
            const bool cropsWidth = sourceAspect > labelAspect;
            const double kept =
                cropsWidth ? sourceAspect * (1.0 - fit->cropShare) : sourceAspect / (1.0 - fit->cropShare);
            CHECK(std::abs(kept - labelAspect) < 1e-9);
            CHECK(fit->cropShare <= kMaxCoverCropShare);
            CHECK(fit->coverWidth == 256);
            CHECK(fit->coverHeight == 188);
        } else {
            // The cover's place on the quad (in the label's units) keeps its aspect within a pixel's rounding.
            const double onQuad = (fit->coverWidth / 256.0 * c.label.width) / (fit->coverHeight / 188.0 * c.label.height);
            const double pixel = std::max(1.0 / fit->coverWidth, 1.0 / fit->coverHeight);
            CHECK(std::abs(onQuad / sourceAspect - 1.0) <= pixel + 1e-9);
            CHECK(fit->cropShare == 0.0);
        }
    }
    // The layer's shape is I3's fit to the pixel.
    FitOptions layer;
    layer.label = {};
    auto i3 = fitCover(makeCover(476, 864, 5), 256, 188);
    auto same = fitCover(makeCover(476, 864, 5), 256, 188, layer);
    REQUIRE(i3.has_value());
    REQUIRE(same.has_value());
    CHECK(i3->picture.pixels == same->picture.pixels);
    // A square cover on a square label fills the layer, squeezed vertically: the quad undoes it.
    FitOptions square;
    square.label = {120, 120};
    auto squeezed = fitCover(makeCover(600, 600, 5), 256, 188, square);
    REQUIRE(squeezed.has_value());
    CHECK(squeezed->cropShare == 0.0);
    CHECK(squeezed->picture.pixels != fitCover(makeCover(600, 600, 5), 256, 188)->picture.pixels);
    // buildCoverLayer() fits to the options' shape.
    auto layered = buildCoverLayer(png(makeCover(476, 864, 3)), TextureFormat::Bc1Srgb, "portrait", square);
    REQUIRE(layered.has_value());
    CHECK(layered->mode == FitMode::Banded);
    CHECK(layered->texture.width == 256);
    CHECK(layered->texture.height == 188);
}
