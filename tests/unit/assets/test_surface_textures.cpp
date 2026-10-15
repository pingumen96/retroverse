#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <rv/assets/surface_textures.hpp>
#include <rv/core/math.hpp>

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
                                                 "ornate_rug", "brushed_metal", "aged_plastic"});
    CHECK(rv::assets::findProceduralSurface("teak") == nullptr);
    const auto unknown = rv::assets::makeProceduralSurface("teak");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == ErrorCode::NotFound);
    // The rug is a picture with a border; every other surface tiles.
    CHECK_FALSE(rv::assets::findProceduralSurface("ornate_rug")->tiles);
    CHECK_FALSE(rv::assets::findProceduralSurface("ornate_rug")->hasNormalMap);
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
        // Opaque base color; the metallic-roughness map leaves R and A at 255 (glTF ignores them).
        CHECK(meanChannel(surface.baseColor, 3) == 1.0);
        CHECK(meanChannel(surface.metallicRoughness, 0) == 1.0);
        CHECK(meanChannel(surface.metallicRoughness, 3) == 1.0);
    }
}

TEST_CASE("assets/surface_textures: the same name gives the same bytes", "[assets][surfaces]") {
    for (const char* name : {"aged_plastic", "painted_plaster"}) {
        INFO(name);
        const auto again = rv::assets::makeProceduralSurface(name);
        REQUIRE(again.has_value());
        const ProceduralSurface& first = surfaceOf(name);
        CHECK(again->baseColor.pixels == first.baseColor.pixels);
        CHECK(again->metallicRoughness.pixels == first.metallicRoughness.pixels);
        CHECK(again->normal.pixels == first.normal.pixels);
    }
}

TEST_CASE("assets/surface_textures: tiling surfaces repeat without a seam", "[assets][surfaces]") {
    // Across the wrap-around edge the image changes no more than between any two neighboring columns or rows inside it
    // (a seam would stand out as a line every repeat; where the layout puts a joint, between two planks or two boards
    // of a panel, the image changes as much at every other joint).
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
            INFO(info.name << " " << rv::assets::toString(map));
            const double columns = interiorDifference(image, false);
            const double rows = interiorDifference(image, true);
            CHECK(lineDifference(image, image.width - 1, 0, false) <= 1.1 * columns + 0.5);
            CHECK(lineDifference(image, image.height - 1, 0, true) <= 1.1 * rows + 0.5);
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
        {"oak_planks", "#76513A"}, {"walnut", "#493024"},          {"varnished_wood", "#5B3A26"},
        {"oak", "#76513A"},        {"painted_plaster", "#383735"}, {"brushed_metal", "#B9BBBE"},
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
    CHECK(roughness("brushed_metal") == Approx(0.32).margin(0.08));
    for (const char* name : {"oak_planks", "walnut", "varnished_wood", "oak", "painted_plaster", "ornate_rug",
                             "aged_plastic"}) {
        INFO(name);
        CHECK(metallic(name) == 0.0);
    }
    CHECK(metallic("brushed_metal") == 1.0);
    // The detail surface stays close to 1, so that it modulates a material's own color and roughness.
    const glm::vec3 plastic = meanLinearColor(surfaceOf("aged_plastic").baseColor);
    CHECK(plastic.r > 0.95f);
    CHECK(plastic.b > 0.9f);
    CHECK(roughness("aged_plastic") > 0.9);
}
