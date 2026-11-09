#include <rv/assets/image_io.hpp>
#include <rv/assets/surface_textures.hpp>
#include <rv/core/math.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using Catch::Approx;
using rv::ErrorCode;
using rv::assets::ImageData;
using rv::assets::ProceduralSurface;
using rv::assets::ProceduralSurfaceInfo;
using rv::assets::SurfaceMap;

namespace {

/// Every surface of the catalog, generated once for all the checks of this file (a few seconds in Debug builds).
const ProceduralSurface& surfaceOf(std::string_view name) {
    static std::map<std::string, ProceduralSurface, std::less<>> generated;
    if (const auto found = generated.find(name); found != generated.end()) {
        return found->second;
    }
    auto surface = rv::assets::makeProceduralSurface(name);
    REQUIRE(surface.has_value());
    return generated.emplace(std::string(name), std::move(*surface)).first->second;
}

const std::uint8_t* texel(const ImageData& image, int x, int y) {
    return image.pixels.data() +
           (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4u;
}

/// Mean absolute RGB difference between columns x0 and x1 (or rows, with `rows`).
double lineDifference(const ImageData& image, int a, int b, bool rows) {
    const int count = rows ? image.width : image.height;
    double sum = 0.0;
    for (int i = 0; i < count; ++i) {
        const std::uint8_t* p = rows ? texel(image, i, a) : texel(image, a, i);
        const std::uint8_t* q = rows ? texel(image, i, b) : texel(image, b, i);
        for (int c = 0; c < 3; ++c) {
            sum += std::abs(static_cast<int>(p[c]) - static_cast<int>(q[c]));
        }
    }
    return sum / (3.0 * count);
}

/// Largest difference between neighboring columns (or rows) inside the image.
double interiorDifference(const ImageData& image, bool rows) {
    const int count = rows ? image.height : image.width;
    double largest = 0.0;
    for (int i = 0; i + 1 < count; ++i) {
        largest = std::max(largest, lineDifference(image, i, i + 1, rows));
    }
    return largest;
}

/// Largest difference between neighboring columns (or rows) among the `pairs` pairs on each side of the wrap-around
/// edge, inside the image.
double differenceBesideEdge(const ImageData& image, bool rows, int pairs) {
    const int count = rows ? image.height : image.width;
    double largest = 0.0;
    for (int i = 0; i < pairs; ++i) {
        largest = std::max({largest, lineDifference(image, i, i + 1, rows),
                            lineDifference(image, count - 2 - i, count - 1 - i, rows)});
    }
    return largest;
}

/// Mean linear albedo of a base color map.
glm::vec3 meanLinearColor(const ImageData& image) {
    glm::dvec3 sum(0.0);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::uint8_t* p = texel(image, x, y);
            for (int c = 0; c < 3; ++c) {
                sum[c] += rv::core::srgbToLinear(static_cast<float>(p[c]) / 255.0f);
            }
        }
    }
    return glm::vec3(sum / static_cast<double>(image.width * image.height));
}

/// Mean of one channel, in [0, 1].
double meanChannel(const ImageData& image, int channel) {
    double sum = 0.0;
    for (std::size_t i = static_cast<std::size_t>(channel); i < image.pixels.size(); i += 4) {
        sum += image.pixels[i];
    }
    return sum / (255.0 * static_cast<double>(image.width * image.height));
}

} // namespace

TEST_CASE("assets/surface_textures: the catalog", "[assets][surfaces]") {
    std::vector<std::string_view> names;
    for (const ProceduralSurfaceInfo& info : rv::assets::proceduralSurfaces()) {
        INFO(info.name);
        names.push_back(info.name);
        CHECK(rv::assets::findProceduralSurface(info.name) == &info);
        CHECK(info.size.x > 0);
        CHECK(info.size.y > 0);
        CHECK(info.meters.x > 0.0f);
        CHECK(info.meters.y > 0.0f);
        CHECK_FALSE(info.description.empty());
        // Square texels: the normal map's slopes are the same along u and v.
        CHECK(info.meters.x / static_cast<float>(info.size.x) ==
              Approx(info.meters.y / static_cast<float>(info.size.y)));
    }
    CHECK(names == std::vector<std::string_view>{"oak_planks", "walnut", "varnished_wood", "oak", "painted_plaster",
                                                 "ornate_rug", "ornate_rug_blue", "ornate_rug_sage", "ornate_rug_green",
                                                 "geometric_rug", "runner_rug", "brushed_metal", "aged_plastic",
                                                 "wallwash", "shelfglow", "picturewash", "herringbone_parquet",
                                                 "fabric_weave", "leather", "wallpaper_damask"});
    CHECK(rv::assets::findProceduralSurface("teak") == nullptr);
    const auto unknown = rv::assets::makeProceduralSurface("teak");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == ErrorCode::NotFound);
    // The rug, the wall wash and the shelf glow are pictures; every other surface tiles.
    for (const char* rug : {"ornate_rug", "ornate_rug_blue", "ornate_rug_sage", "ornate_rug_green", "geometric_rug",
                            "runner_rug"}) {
        INFO(rug);
        CHECK_FALSE(rv::assets::findProceduralSurface(rug)->tiles);
        CHECK_FALSE(rv::assets::findProceduralSurface(rug)->hasNormalMap);
    }
    CHECK_FALSE(rv::assets::findProceduralSurface("wallwash")->tiles);
    CHECK_FALSE(rv::assets::findProceduralSurface("wallwash")->hasNormalMap);
    CHECK_FALSE(rv::assets::findProceduralSurface("shelfglow")->tiles);
    CHECK_FALSE(rv::assets::findProceduralSurface("shelfglow")->hasNormalMap);
    CHECK_FALSE(rv::assets::findProceduralSurface("picturewash")->tiles);
    CHECK_FALSE(rv::assets::findProceduralSurface("picturewash")->hasNormalMap);
}

TEST_CASE("assets/surface_textures: names of single maps", "[assets][surfaces]") {
    for (const SurfaceMap map : {SurfaceMap::BaseColor, SurfaceMap::MetallicRoughness, SurfaceMap::Normal}) {
        CHECK(rv::assets::surfaceMapFromString(rv::assets::toString(map)) == map);
    }
    CHECK_FALSE(rv::assets::surfaceMapFromString("albedo").has_value());

    const auto planks = rv::assets::parseProceduralTextureName("oak_planks/normal");
    REQUIRE(planks.has_value());
    CHECK(planks->surface == "oak_planks");
    CHECK(planks->map == SurfaceMap::Normal);
    CHECK(planks->toString() == "oak_planks/normal");

    struct Case {
        const char* text;
        ErrorCode code;
    };
    for (const Case& c : {Case{"oak_planks", ErrorCode::InvalidArgument},
                          Case{"/baseColor", ErrorCode::InvalidArgument}, Case{"walnut/", ErrorCode::InvalidArgument},
                          Case{"a/b/c", ErrorCode::InvalidArgument}, Case{"teak/baseColor", ErrorCode::NotFound},
                          Case{"walnut/albedo", ErrorCode::NotFound}, Case{"ornate_rug/normal", ErrorCode::NotFound}}) {
        INFO(c.text);
        const auto parsed = rv::assets::parseProceduralTextureName(c.text);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == c.code);
    }
}

TEST_CASE("assets/surface_textures: every surface generates its maps", "[assets][surfaces]") {
    for (const ProceduralSurfaceInfo& info : rv::assets::proceduralSurfaces()) {
        INFO(info.name);
        const ProceduralSurface& surface = surfaceOf(info.name);
        for (const SurfaceMap map : {SurfaceMap::BaseColor, SurfaceMap::MetallicRoughness, SurfaceMap::Normal}) {
            INFO(rv::assets::toString(map));
            const ImageData& image = surface.map(map);
            if (map == SurfaceMap::Normal && !info.hasNormalMap) {
                CHECK_FALSE(image.isValid());
                continue;
            }
            REQUIRE(image.isValid());
            CHECK(image.width == info.size.x);
            CHECK(image.height == info.size.y);
            CHECK(image.channels == 4);
        }
        // Opaque base color (but the wall wash's, the shelf glow's and the picture wash's, whose alpha is their
        // picture); the metallic-roughness map leaves R and A at 255 (glTF ignores them).
        if (info.name != "wallwash" && info.name != "shelfglow" && info.name != "picturewash") {
            CHECK(meanChannel(surface.baseColor, 3) == 1.0);
        }
        CHECK(meanChannel(surface.metallicRoughness, 0) == 1.0);
        CHECK(meanChannel(surface.metallicRoughness, 3) == 1.0);
    }
}

TEST_CASE("assets/surface_textures: the same name gives the same bytes", "[assets][surfaces]") {
    // Every surface, its rows rendered in parallel, is a pure function of its name.
    for (const ProceduralSurfaceInfo& info : rv::assets::proceduralSurfaces()) {
        INFO(info.name);
        const auto again = rv::assets::makeProceduralSurface(info.name);
        REQUIRE(again.has_value());
        const ProceduralSurface& first = surfaceOf(info.name);
        CHECK(again->baseColor.pixels == first.baseColor.pixels);
        CHECK(again->metallicRoughness.pixels == first.metallicRoughness.pixels);
        CHECK(again->normal.pixels == first.normal.pixels);
    }
}

TEST_CASE("assets/surface_textures: tiling surfaces repeat without a seam", "[assets][surfaces]") {
    // A seam would stand out as a line, every repeat, where the image changes more than around it. Across the
    // wrap-around edge the image changes no more than twice as much as between the neighboring columns (or rows) on
    // either side of it: where the layout puts a joint on the edge, between two planks or two boards of a panel, the
    // lines beside it change sharply too (the joint's gap and bevels), while along the grain they barely change at
    // all. Nor does it change more than anywhere inside the image. Half an 8-bit step of slack absorbs the
    // quantization of normals that flip between two codes where a noise lattice puts the slope through zero, at the
    // edge as at every lattice line.
    for (const ProceduralSurfaceInfo& info : rv::assets::proceduralSurfaces()) {
        if (!info.tiles) {
            continue;
        }
        const ProceduralSurface& surface = surfaceOf(info.name);
        for (const SurfaceMap map : {SurfaceMap::BaseColor, SurfaceMap::MetallicRoughness, SurfaceMap::Normal}) {
            const ImageData& image = surface.map(map);
            if (!image.isValid()) {
                continue;
            }
            for (const bool rows : {false, true}) {
                INFO(info.name << " " << rv::assets::toString(map) << (rows ? " rows" : " columns"));
                const int count = rows ? image.height : image.width;
                const double edge = lineDifference(image, count - 1, 0, rows);
                CHECK(edge <= 2.0 * differenceBesideEdge(image, rows, 3) + 0.5);
                CHECK(edge <= 1.1 * interiorDifference(image, rows) + 0.5);
            }
        }
    }
    // The rug's outer guard band is uniform, so its picture joins seamlessly too where a material repeats it.
    const ImageData& rug = surfaceOf("ornate_rug").baseColor;
    CHECK(lineDifference(rug, rug.width - 1, 0, false) <= 1.1 * interiorDifference(rug, false) + 0.5);
}

TEST_CASE("assets/surface_textures: normal maps hold unit normals facing out", "[assets][surfaces]") {
    for (const ProceduralSurfaceInfo& info : rv::assets::proceduralSurfaces()) {
        if (!info.hasNormalMap) {
            continue;
        }
        INFO(info.name);
        const ImageData& normals = surfaceOf(info.name).normal;
        int bad = 0;
        double tilt = 0.0;
        for (int y = 0; y < normals.height; ++y) {
            for (int x = 0; x < normals.width; ++x) {
                const std::uint8_t* p = texel(normals, x, y);
                const glm::vec3 n = glm::vec3(p[0], p[1], p[2]) / 255.0f * 2.0f - 1.0f;
                bad += (n.z <= 0.0f || std::abs(glm::length(n) - 1.0f) > 0.02f) ? 1 : 0;
                tilt += 1.0 - n.z;
            }
        }
        CHECK(bad == 0);
        // Relief, but gentle on average: the surfaces stay readable as flat boards, walls and plastics.
        CHECK(tilt / (normals.width * normals.height) < 0.05);
    }
    // The planks' joints and bevels tilt the normals well beyond the grain's relief: along every joint the normals lean
    // by more than 15 degrees (n.z below 0.966), nowhere inside a plank.
    const ImageData& planks = surfaceOf("oak_planks").normal;
    int steep = 0;
    for (int y = 0; y < planks.height; ++y) {
        for (int x = 0; x < planks.width; ++x) {
            steep += texel(planks, x, y)[2] < 250 ? 1 : 0;
        }
    }
    // 14 rows of planks, each with two long edges over the 1024 rows of the image (about one steep texel beside each),
    // and their joints across.
    CHECK(steep > 10 * planks.height);
    CHECK(steep < planks.width * planks.height / 10);
}

TEST_CASE("assets/surface_textures: full surfaces average the style palette", "[assets][surfaces]") {
    // style.md section 3: walnut #493024, oak #76513A, varnished wood #5B3A26, wall #383735, brushed steel #B9BBBE.
    // The surfaces vary around them; their mean linear albedo stays within 12% of the color.
    const std::pair<const char*, const char*> palette[] = {
        {"oak_planks", "#76513A"},          {"walnut", "#493024"},
        {"varnished_wood", "#5B3A26"},      {"oak", "#76513A"},
        {"painted_plaster", "#383735"},     {"brushed_metal", "#B9BBBE"},
        {"herringbone_parquet", "#76513A"},
    };
    for (const auto& [name, hex] : palette) {
        INFO(name << " " << hex);
        const glm::vec3 mean = meanLinearColor(surfaceOf(name).baseColor);
        const glm::vec3 expected = rv::core::hexToLinearRgb(hex).value();
        for (int c = 0; c < 3; ++c) {
            CHECK(mean[c] == Approx(expected[c]).epsilon(0.12));
        }
    }
    // The rug is mostly its bordeaux field and navy borders: a dark, warm red.
    const glm::vec3 rug = meanLinearColor(surfaceOf("ornate_rug").baseColor);
    CHECK(rug.r > rug.g);
    CHECK(rug.r > rug.b);
    CHECK(rug.r < 0.25f);
    // I6 step 3b's rugs: the optical era's blue, the archive's sage, the study's green, the arcade's red runner, the
    // 90s collector's charcoal with its confetti.
    const glm::vec3 blue = meanLinearColor(surfaceOf("ornate_rug_blue").baseColor);
    CHECK(blue.b > blue.g);
    CHECK(blue.b > blue.r);
    const glm::vec3 sage = meanLinearColor(surfaceOf("ornate_rug_sage").baseColor);
    CHECK(sage.g > sage.b);
    CHECK(sage.g > 0.08f);
    const glm::vec3 green = meanLinearColor(surfaceOf("ornate_rug_green").baseColor);
    CHECK(green.g > green.r);
    CHECK(green.g > green.b);
    const glm::vec3 runner = meanLinearColor(surfaceOf("runner_rug").baseColor);
    CHECK(runner.r > 1.5f * runner.g);
    CHECK(runner.r < 0.2f);
    const glm::vec3 geometric = meanLinearColor(surfaceOf("geometric_rug").baseColor);
    CHECK(geometric.r < 0.12f);
    CHECK(geometric.g > 0.02f);
}

TEST_CASE("assets/surface_textures: roughness and metal of the finishes", "[assets][surfaces]") {
    const auto roughness = [](const char* name) {
        return meanChannel(surfaceOf(name).metallicRoughness, 1);
    };
    const auto metallic = [](const char* name) {
        return meanChannel(surfaceOf(name).metallicRoughness, 2);
    };
    // Satin wood, glossier varnish, matte paint and wool, satin steel.
    CHECK(roughness("oak_planks") == Approx(0.45).margin(0.08));
    CHECK(roughness("walnut") == Approx(0.5).margin(0.08));
    CHECK(roughness("varnished_wood") < roughness("walnut"));
    CHECK(roughness("painted_plaster") > 0.8);
    CHECK(roughness("ornate_rug") > 0.85);
    CHECK(roughness("geometric_rug") > 0.85);
    CHECK(roughness("runner_rug") > 0.85);
    CHECK(roughness("brushed_metal") == Approx(0.32).margin(0.08));
    // Every surface is all metal or none (the GPU keeps its roughness alone: gl::metallicRoughnessFormat, R8).
    for (const rv::assets::ProceduralSurfaceInfo& info : rv::assets::proceduralSurfaces()) {
        INFO(info.name);
        CHECK(metallic(std::string(info.name).c_str()) == (info.name == "brushed_metal" ? 1.0 : 0.0));
    }
    // The detail surface stays close to 1, so that it modulates a material's own color and roughness.
    const glm::vec3 plastic = meanLinearColor(surfaceOf("aged_plastic").baseColor);
    CHECK(plastic.r > 0.95f);
    CHECK(plastic.b > 0.9f);
    CHECK(roughness("aged_plastic") > 0.9);
    // I12 step 1: the herringbone's satin varnish as the planks'; the weave, the leather and the wallpaper are grey
    // detail surfaces a little under 1 (they darken their material by about a tenth), the weave and the paper matte,
    // the leather a little polished.
    CHECK(roughness("herringbone_parquet") == Approx(0.45).margin(0.08));
    for (const char* name : {"fabric_weave", "leather", "wallpaper_damask"}) {
        INFO(name);
        const glm::vec3 mean = meanLinearColor(surfaceOf(name).baseColor);
        CHECK(mean.r == Approx(mean.g).margin(0.001));
        CHECK(mean.g == Approx(mean.b).margin(0.001));
        CHECK(mean.r > 0.8f);
        CHECK(mean.r < 0.97f);
    }
    CHECK(roughness("fabric_weave") > 0.93);
    CHECK(roughness("wallpaper_damask") > 0.9);
    CHECK(roughness("leather") > 0.8);
    CHECK(roughness("leather") < roughness("fabric_weave"));
}

TEST_CASE("assets/surface_textures: the herringbone's joints and the damask's ink", "[assets][surfaces]") {
    // The herringbone's joints run at 45 degrees: the normals lean steeply along them (as the planks'), nowhere inside
    // a block; its blocks alternate, so its rows and columns hold as many steep texels as each other.
    const ImageData& normals = surfaceOf("herringbone_parquet").normal;
    int steep = 0;
    for (int y = 0; y < normals.height; ++y) {
        for (int x = 0; x < normals.width; ++x) {
            steep += texel(normals, x, y)[2] < 250 ? 1 : 0;
        }
    }
    CHECK(steep > 10 * normals.height);
    CHECK(steep < normals.width * normals.height / 6);
    // The damask: lighter ink on about a fifth of the paper (between a tenth and a third), symmetric about the
    // motifs' vertical axes (u = 0 and u = 0.5 are axes of the first set).
    const ImageData& paper = surfaceOf("wallpaper_damask").baseColor;
    int inked = 0;
    for (int y = 0; y < paper.height; ++y) {
        for (int x = 0; x < paper.width; ++x) {
            inked += texel(paper, x, y)[0] > 245 ? 1 : 0;
        }
    }
    const double share = static_cast<double>(inked) / static_cast<double>(paper.width * paper.height);
    CHECK(share > 0.1);
    CHECK(share < 0.34);
    const int axis = paper.width / 2;
    double mirrored = 0.0;
    for (int y = 0; y < paper.height; y += 7) {
        for (int dx = 1; dx < 100; dx += 3) {
            mirrored += std::abs(static_cast<int>(texel(paper, axis + dx, y)[0]) -
                                 static_cast<int>(texel(paper, axis - dx, y)[0]));
        }
    }
    // Mirror images up to the fibre's noise and the texel centres' half-texel offset.
    CHECK(mirrored / (static_cast<double>(paper.height / 7 + 1) * 33.0) < 12.0);
}

TEST_CASE("assets/surface_textures: dump every map as PNG for a look by eye", "[.surface-dump]") {
    // A developer aid, never run by ctest: RV_SURFACE_DUMP names a folder (under build/) that receives
    // <surface>_<map>.png for every surface of the catalog.
    const char* folder = std::getenv("RV_SURFACE_DUMP"); // NOLINT(concurrency-mt-unsafe): read before any thread
    if (folder == nullptr || *folder == '\0') {
        SKIP("set RV_SURFACE_DUMP to a folder under build/ to write every surface's maps there");
    }
    const std::filesystem::path out(folder);
    std::filesystem::create_directories(out);
    for (const ProceduralSurfaceInfo& info : rv::assets::proceduralSurfaces()) {
        const ProceduralSurface& surface = surfaceOf(info.name);
        for (const SurfaceMap map : {SurfaceMap::BaseColor, SurfaceMap::MetallicRoughness, SurfaceMap::Normal}) {
            const ImageData& image = surface.map(map);
            if (image.isValid()) {
                const auto path =
                    out / (std::string(info.name) + "_" + std::string(rv::assets::toString(map)) + ".png");
                CHECK(rv::assets::writePng(path, image).has_value());
            }
        }
    }
}

TEST_CASE("assets/surface_textures: the shelf glow: a white picture, strongest under the strip, faint at the sides",
          "[assets][surfaces]") {
    const ProceduralSurface& glow = surfaceOf("shelfglow");
    const ImageData& image = glow.baseColor;
    REQUIRE(image.isValid());
    const auto alpha = [&](int x, int y) { return static_cast<int>(texel(image, x, y)[3]); };
    const int centre = image.width / 2;
    CHECK(texel(image, centre, image.height / 2)[0] == 255); // white: the material gives the colour
    // Falling from the top (under the strip) to the board below, never quite to nothing.
    int previous = 256;
    for (int y = 0; y < image.height; ++y) {
        CHECK(alpha(centre, y) <= previous);
        previous = alpha(centre, y);
    }
    CHECK(alpha(centre, 0) >= 220);
    CHECK(alpha(centre, image.height - 1) >= 20);
    CHECK(alpha(centre, image.height - 1) <= 60);
    // Fading at the sides, symmetric.
    CHECK(alpha(0, 0) <= 10);
    CHECK(alpha(image.width - 1, 0) <= 10);
    CHECK(alpha(10, 4) == alpha(image.width - 11, 4));
}

TEST_CASE("assets/surface_textures: the picture wash: a white cone from the hood, falling downwards, none at the sides",
          "[assets][surfaces]") {
    // I12 step 2: the gallery's picture lights.
    const ImageData& image = surfaceOf("picturewash").baseColor;
    REQUIRE(image.isValid());
    const auto alpha = [&](int x, int y) { return static_cast<int>(texel(image, x, y)[3]); };
    const int centre = image.width / 2;
    CHECK(texel(image, centre, image.height / 2)[0] == 255);
    // Nothing at the very top (the hood's edge), strongest just under it, then falling, never quite to nothing.
    CHECK(alpha(centre, 0) <= 20);
    int strongest = 0;
    int at = 0;
    for (int y = 0; y < image.height; ++y) {
        if (alpha(centre, y) > strongest) {
            strongest = alpha(centre, y);
            at = y;
        }
    }
    CHECK(strongest >= 200);
    CHECK(at < image.height / 8);
    for (int y = at + 1; y < image.height; ++y) {
        CHECK(alpha(centre, y) <= alpha(centre, y - 1));
    }
    CHECK(alpha(centre, image.height - 1) >= 25);
    // A cone: none at the sides, wider below than above, symmetric.
    for (int y = 0; y < image.height; y += 8) {
        CHECK(alpha(0, y) <= 2);
        CHECK(alpha(image.width - 1, y) <= 2);
    }
    const auto width = [&](int y) {
        int lit = 0;
        for (int x = 0; x < image.width; ++x) {
            lit += alpha(x, y) > alpha(centre, y) / 2 ? 1 : 0;
        }
        return lit;
    };
    CHECK(width(image.height - 1) > width(image.height / 8));
    CHECK(alpha(20, 60) == alpha(image.width - 21, 60));
}

TEST_CASE("assets/surface_textures: the wall wash: a white picture, its light rising from the shade, none at its edges",
          "[assets][surfaces]") {
    const ProceduralSurface& wash = surfaceOf("wallwash");
    const ImageData& image = wash.baseColor;
    REQUIRE(image.isValid());
    const auto alpha = [&](int x, int y) { return static_cast<int>(texel(image, x, y)[3]); };
    int edge = 0;
    for (int x = 0; x < image.width; ++x) {
        edge = std::max({edge, alpha(x, 0), alpha(x, image.height - 1)});
        CHECK(texel(image, x, image.height / 2)[0] == 255); // white: the material gives the colour
    }
    for (int y = 0; y < image.height; ++y) {
        edge = std::max({edge, alpha(0, y), alpha(image.width - 1, y)});
    }
    CHECK(edge == 0);
    // The shade sits a little below the middle (the quad centred 5 cm above it): opaque around it, the scallop above
    // stronger and taller than the one below, symmetric across.
    const int centreX = image.width / 2;
    const int shade = image.height / 2 + image.height / 30;
    CHECK(alpha(centreX, shade) >= 200);
    const int above = image.height / 2 - image.height / 4; // 0.375 m above the middle
    const int below = image.height / 2 + image.height / 4;
    CHECK(alpha(centreX, above) > alpha(centreX, below));
    CHECK(alpha(centreX, above) >= 80);
    CHECK(alpha(centreX - 20, shade) == alpha(image.width - 1 - (centreX - 20), shade));
    // Soft across: a quarter metre to the side much fainter than at the centre.
    CHECK(alpha(centreX + image.width / 4, shade) < alpha(centreX, shade) / 2);
}

