// Logos as pictures (I11 step 2, ADR-0019 §3): SVG documents rasterized by lunasvg into a box at their aspect, with
// straight alpha; what a hostile document is refused for; PNG and JPEG logos decoded and only ever made smaller.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

#include <rv/assets/image_data.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/svg_raster.hpp>
#include <rv/core/file_io.hpp>

#include "support/temp_dir.hpp"

namespace {

std::span<const std::byte> bytesOf(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

struct Rgba {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 0;
};

Rgba pixel(const rv::assets::ImageData& image, int x, int y) {
    const std::uint8_t* p =
        image.pixels.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                               static_cast<std::size_t>(x)) *
                                  4;
    return Rgba{p[0], p[1], p[2], p[3]};
}

bool near(int a, int b, int tolerance = 2) {
    return std::abs(a - b) <= tolerance;
}

rv::ErrorCode errorOf(std::string_view document, int box = 64) {
    auto image = rv::assets::rasterizeSvg(bytesOf(document), box, box, "test.svg");
    REQUIRE_FALSE(image.has_value());
    return image.error().code;
}

rv::assets::ImageData solid(int width, int height, std::uint8_t alpha) {
    rv::assets::ImageData image;
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        image.pixels[i] = 40;
        image.pixels[i + 1] = 120;
        image.pixels[i + 2] = 200;
        image.pixels[i + 3] = alpha;
    }
    return image;
}

constexpr std::string_view kTwoShapes = R"(<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 50">
  <rect x="0" y="0" width="50" height="50" fill="#FF0000"/>
  <circle cx="75" cy="25" r="20" fill="#0000FF"/>
</svg>)";

} // namespace

TEST_CASE("assets/svg: fitting a picture into a box at its aspect", "[assets][svg]") {
    CHECK(rv::assets::fitInto(100, 50, 200, 200) == std::make_pair(200, 100));
    CHECK(rv::assets::fitInto(50, 100, 200, 200) == std::make_pair(100, 200));
    CHECK(rv::assets::fitInto(264, 65, 512, 128) == std::make_pair(512, 126));
    CHECK(rv::assets::fitInto(1, 10000, 512, 128) == std::make_pair(1, 128));
    CHECK_FALSE(rv::assets::fitInto(0, 10, 64, 64));
    CHECK_FALSE(rv::assets::fitInto(10, -1, 64, 64));
    CHECK_FALSE(rv::assets::fitInto(10, 10, 0, 64));
}

TEST_CASE("assets/svg: a document rasterized at its aspect, transparent where it draws nothing", "[assets][svg]") {
    auto image = rv::assets::rasterizeSvg(bytesOf(kTwoShapes), 200, 200, "two-shapes.svg");
    INFO((image ? "rasterized" : image.error().toString()));
    REQUIRE(image.has_value());
    CHECK(image->width == 200);
    CHECK(image->height == 100);
    CHECK(image->channels == 4);
    REQUIRE(image->isValid());

    const Rgba red = pixel(*image, 50, 50);
    CHECK((red.r == 255 && red.g == 0 && red.b == 0 && red.a == 255));
    const Rgba blue = pixel(*image, 150, 50);
    CHECK((blue.r == 0 && blue.g == 0 && blue.b == 255 && blue.a == 255));
    CHECK(pixel(*image, 197, 3).a == 0);
    CHECK(pixel(*image, 150, 97).a == 0);
    // An edge of the circle is drawn smooth: some pixel along its radius is partly covered.
    bool partly = false;
    for (int x = 185; x < 195; ++x) {
        const int alpha = pixel(*image, x, 50).a;
        partly = partly || (alpha > 0 && alpha < 255);
    }
    CHECK(partly);
}

TEST_CASE("assets/svg: alpha is straight, not premultiplied", "[assets][svg]") {
    const std::string_view half = R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10">
  <rect width="10" height="10" fill="#FF8000" fill-opacity="0.5"/></svg>)";
    auto image = rv::assets::rasterizeSvg(bytesOf(half), 10, 10, "half.svg");
    REQUIRE(image.has_value());
    const Rgba p = pixel(*image, 5, 5);
    CHECK(near(p.a, 128));
    CHECK(near(p.r, 255));
    CHECK(near(p.g, 128));
    CHECK(near(p.b, 0));
}

TEST_CASE("assets/svg: what a document is refused for", "[assets][svg]") {
    CHECK(errorOf("") == rv::ErrorCode::InvalidArgument);
    CHECK(rv::assets::rasterizeSvg(bytesOf(kTwoShapes), 0, 10, "x").error().code == rv::ErrorCode::InvalidArgument);
    CHECK(rv::assets::rasterizeSvg(bytesOf(kTwoShapes), rv::assets::kMaxLogoSide + 1, 10, "x").error().code ==
          rv::ErrorCode::InvalidArgument);

    CHECK(errorOf(R"(<?xml version="1.0"?><!DOCTYPE svg [<!ENTITY a "aaaa">]><svg width="1" height="1">&a;</svg>)") ==
          rv::ErrorCode::Unsupported);
    CHECK(errorOf(R"(<svg width="1" height="1"><style>@import url(x.css);</style></svg>)") ==
          rv::ErrorCode::Unsupported);
    CHECK(errorOf(R"(<svg xmlns:xlink="http://www.w3.org/1999/xlink" width="1" height="1">
<image xlink:href="C:\secret.png" width="1" height="1"/></svg>)") == rv::ErrorCode::Unsupported);
    CHECK(errorOf(R"(<svg width="1" height="1"><use href = 'http://example.invalid/a.svg#b'/></svg>)") ==
          rv::ErrorCode::Unsupported);

    std::string large = R"(<svg width="1" height="1"><!--)";
    large.append(rv::assets::kMaxSvgBytes, ' ');
    large += "--></svg>";
    CHECK(errorOf(large) == rv::ErrorCode::OutOfRange);
    std::string many = R"(<svg width="1" height="1">)";
    for (std::size_t i = 0; i < rv::assets::kMaxSvgElements; ++i) {
        many += "<g/>";
    }
    many += "</svg>";
    CHECK(errorOf(many) == rv::ErrorCode::OutOfRange);

    CHECK(errorOf("this is no SVG") == rv::ErrorCode::ParseError);

    // A reference within the document and a data: URI are a logo's own.
    const std::string_view inside = R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
<defs><rect id="r" width="4" height="4" fill="#00FF00"/></defs><use href="#r"/></svg>)";
    auto used = rv::assets::rasterizeSvg(bytesOf(inside), 4, 4, "use.svg");
    REQUIRE(used.has_value());
    CHECK(pixel(*used, 2, 2).g == 255);
}

TEST_CASE("assets/svg: logo files: SVG rasterized, PNG kept or made smaller, other formats refused",
          "[assets][svg]") {
    const rv::test::TempDir dir("rv-assets-logo");
    const auto svg = dir.file("logo.svg");
    REQUIRE(rv::core::writeTextFileAtomic(svg, kTwoShapes).has_value());
    auto rasterized = rv::assets::loadLogo(svg, 64, 64);
    REQUIRE(rasterized.has_value());
    CHECK(rasterized->width == 64);
    CHECK(rasterized->height == 32);

    const auto large = dir.file("large.png");
    REQUIRE(rv::assets::writePng(large, solid(400, 100, 255)).has_value());
    auto smaller = rv::assets::loadLogo(large, 200, 200);
    REQUIRE(smaller.has_value());
    CHECK(smaller->width == 200);
    CHECK(smaller->height == 50);
    const Rgba inside = pixel(*smaller, 100, 25);
    CHECK((near(inside.r, 40) && near(inside.g, 120) && near(inside.b, 200) && inside.a == 255));

    const auto small = dir.file("small.png");
    REQUIRE(rv::assets::writePng(small, solid(40, 10, 128)).has_value());
    auto kept = rv::assets::loadLogo(small, 200, 200);
    REQUIRE(kept.has_value());
    CHECK(kept->width == 40); // never enlarged
    CHECK(kept->height == 10);
    CHECK(pixel(*kept, 5, 5).a == 128);

    const auto webp = dir.file("logo.webp");
    REQUIRE(rv::core::writeTextFileAtomic(webp, "RIFF").has_value());
    auto refused = rv::assets::loadLogo(webp, 64, 64);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::Unsupported);

    auto missing = rv::assets::loadLogo(dir.file("missing.svg"), 64, 64);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == rv::ErrorCode::NotFound);
}
