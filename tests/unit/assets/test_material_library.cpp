#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/log_capture.hpp"
#include <rv/assets/material_library.hpp>
#include <rv/assets/surface_textures.hpp>
#include <rv/core/json.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>

using Catch::Approx;
using rv::ErrorCode;
using rv::assets::AlphaMode;
using rv::assets::MaterialDesc;
using rv::assets::MaterialLibrary;
using rv::assets::ShadingModel;

namespace {

const std::filesystem::path kLibraryPath = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR) / "materials/base/materials.json";

/// The shared material contract: scene files and procedural prefabs reference these names.
const std::vector<std::string> kSharedNames = {
    "MAT_Wood_Walnut",     "MAT_Wood_Oak",      "MAT_Wood_Varnished", "MAT_Floor_OakPlanks",
    "MAT_Concrete_Warm",   "MAT_Wall_Painted",  "MAT_Plaster_Used",   "MAT_Metal_Graphite",
    "MAT_Metal_Brushed",   "MAT_Plastic_Black", "MAT_Plastic_Gray",   "MAT_Plastic_AgedIvory",
    "MAT_Plastic_Accent",  "MAT_Glass_Display", "MAT_Glass_CRT",      "MAT_Fabric_Sofa",
    "MAT_Carpet_Ornate",   "MAT_Cardboard_Box", "MAT_Paper_Print",    "MAT_Neon_Diffuser",
    "MAT_Screen_Emissive", "MAT_Screen_CRT",    "MAT_Lamp_Shade",     "MAT_Label_Print",
    "MAT_Fallback_Magenta"};

/// The procedural surface a "full" textured preset takes its albedo and roughness from
/// (rv/assets/surface_textures.hpp): its factors are neutral, the surface carries the style section 3 color.
const std::vector<std::pair<std::string, std::string>> kFullSurfaces = {
    {"MAT_Wood_Walnut", "walnut"},
    {"MAT_Wood_Oak", "oak"},
    {"MAT_Wood_Varnished", "varnished_wood"},
    {"MAT_Floor_OakPlanks", "oak_planks"},
    {"MAT_Wall_Painted", "painted_plaster"},
    {"MAT_Metal_Brushed", "brushed_metal"},
    {"MAT_Carpet_Ornate", "ornate_rug"},
};

nlohmann::json parse(std::string_view text) {
    auto json = rv::core::parseJson(text, "test");
    REQUIRE(json.has_value());
    return *json;
}

glm::vec3 linear(std::string_view hex) {
    return rv::core::hexToLinearRgb(hex).value();
}

void checkColor(const glm::vec3& actual, const glm::vec3& expected) {
    CHECK(actual.r == Approx(expected.r).margin(1e-5));
    CHECK(actual.g == Approx(expected.g).margin(1e-5));
    CHECK(actual.b == Approx(expected.b).margin(1e-5));
}

} // namespace

TEST_CASE("assets/materials: the shared library defines every contract material", "[assets][materials]") {
    const auto library = MaterialLibrary::loadFromFile(kLibraryPath);
    REQUIRE(library.has_value());
    for (const std::string& name : kSharedNames) {
        CAPTURE(name);
        const MaterialDesc* material = library->find(name);
        REQUIRE(material != nullptr);
        CHECK(material->name == name);
        if (material->shading != ShadingModel::Pbr) {
            continue;
        }
        // Textured presets name procedural textures of the catalog; the "full" surfaces carry the albedo and
        // roughness, so their factors are neutral (checked below) rather than plausible values of their own.
        for (const rv::assets::TextureRef* texture : {&material->baseColorTexture, &material->metallicRoughnessTexture,
                                                       &material->normalTexture}) {
            if (!texture->procedural.empty()) {
                CHECK(rv::assets::parseProceduralTextureName(texture->procedural).has_value());
            }
        }
        if (!material->metallicRoughnessTexture.empty() && material->roughnessFactor == 1.0f) {
            continue;
        }
        // Physically plausible presets: dielectrics vs metals. A metal's base color is its reflectance at normal
        // incidence, at least about 50% for real metals (linear luminance).
        if (material->metallicFactor == 0.0f) {
            CHECK(material->roughnessFactor >= 0.3f);
            CHECK(material->roughnessFactor <= 0.95f);
        } else {
            CHECK(material->metallicFactor >= 0.9f);
            CHECK(material->metallicFactor <= 1.0f);
            const glm::vec4& color = material->baseColorFactor;
            CHECK(0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b >= 0.4f);
        }
    }
    CHECK(library->size() == kSharedNames.size());
    const auto names = library->names();
    CHECK(std::is_sorted(names.begin(), names.end()));

    // Art-direction colors of style.md section 3, converted to linear; the textured woods, walls, metal and rug carry
    // theirs in their procedural surfaces (test_surface_textures.cpp checks the surfaces' averages), with neutral
    // factors and all of their surface's maps.
    checkColor(glm::vec3(library->find("MAT_Metal_Graphite")->baseColorFactor), linear("#252A2E"));
    checkColor(glm::vec3(library->find("MAT_Fabric_Sofa")->baseColorFactor), linear("#683D42"));
    for (const auto& [name, surface] : kFullSurfaces) {
        CAPTURE(name);
        const MaterialDesc& material = *library->find(name);
        checkColor(glm::vec3(material.baseColorFactor), glm::vec3(1.0f));
        CHECK(material.roughnessFactor == 1.0f);
        CHECK(material.baseColorTexture.procedural == surface + "/baseColor");
        CHECK(material.metallicRoughnessTexture.procedural == surface + "/metallicRoughness");
        const rv::assets::ProceduralSurfaceInfo* info = rv::assets::findProceduralSurface(surface);
        REQUIRE(info != nullptr);
        CHECK(material.normalTexture.procedural == (info->hasNormalMap ? surface + "/normal" : std::string()));
    }
    // The plastics keep their style colors and roughness; the aged plastic detail modulates them.
    for (const char* name : {"MAT_Plastic_Black", "MAT_Plastic_Gray", "MAT_Plastic_AgedIvory"}) {
        CAPTURE(name);
        const MaterialDesc& plastic = *library->find(name);
        CHECK(plastic.baseColorTexture.procedural == "aged_plastic/baseColor");
        CHECK(plastic.roughnessFactor < 0.6f);
    }
    checkColor(glm::vec3(library->find("MAT_Plastic_AgedIvory")->baseColorFactor), linear("#D6CBAE"));

    const MaterialDesc& glass = *library->find("MAT_Glass_Display");
    CHECK(glass.alphaMode == AlphaMode::Blend);
    CHECK(glass.baseColorFactor.a < 1.0f);

    const MaterialDesc& lamp = *library->find("MAT_Lamp_Shade");
    CHECK(lamp.shading == ShadingModel::Pbr);
    checkColor(lamp.emissiveFactor, linear("#FFC88A"));
    CHECK(lamp.doubleSided);

    const MaterialDesc& label = *library->find("MAT_Label_Print");
    checkColor(glm::vec3(label.baseColorFactor), glm::vec3(1.0f));
    CHECK(label.roughnessFactor == Approx(0.6f));
}

TEST_CASE("assets/materials: unlit presets", "[assets][materials]") {
    const auto library = MaterialLibrary::loadFromFile(kLibraryPath);
    REQUIRE(library.has_value());

    const MaterialDesc& screen = *library->find("MAT_Screen_Emissive");
    CHECK(screen.shading == ShadingModel::Unlit);
    checkColor(glm::vec3(screen.baseColorFactor), glm::vec3(1.0f));
    CHECK(screen.emissiveStrength == Approx(2.0f));
    CHECK_FALSE(screen.crt.has_value());

    // The picture tube: the same brightness, a subtle CRT effect (style 15.2: never hurting legibility).
    const MaterialDesc& crt = *library->find("MAT_Screen_CRT");
    CHECK(crt.shading == ShadingModel::Unlit);
    CHECK(crt.emissiveStrength == screen.emissiveStrength);
    REQUIRE(crt.crt.has_value());
    CHECK(crt.crt->lineCount == 240.0f);
    CHECK(crt.crt->scanlineStrength <= 0.5f);
    CHECK(crt.crt->maskStrength <= 0.3f);
    CHECK(crt.crt->curvature <= 0.1f);
    CHECK(crt.crt->curvature > 0.0f); // a bulging tube: rounded picture corners
    CHECK(crt.crt->vignette <= 0.4f);
    // A faint sheen on the glass: a lamp's reflection must not wash out the picture (style 15.2).
    CHECK(crt.crt->glassReflection > 0.0f);
    CHECK(crt.crt->glassReflection <= 0.35f);
    CHECK(rv::assets::CrtEffect{}.glassReflection <= 0.35f);

    const MaterialDesc& neon = *library->find("MAT_Neon_Diffuser");
    CHECK(neon.shading == ShadingModel::Unlit);
    checkColor(glm::vec3(neon.baseColorFactor), linear("#B34DCE"));

    const MaterialDesc& magenta = *library->find("MAT_Fallback_Magenta");
    CHECK(magenta.shading == ShadingModel::Unlit);
    checkColor(glm::vec3(magenta.baseColorFactor), glm::vec3(1.0f, 0.0f, 1.0f));
}

TEST_CASE("assets/materials: sRGB hex colors are converted to linear", "[assets][materials]") {
    const auto grey = rv::assets::parseMaterialDesc(parse(R"({"baseColor": "#808080"})"), "MAT_Grey", "inline");
    REQUIRE(grey.has_value());
    CHECK(grey->name == "MAT_Grey");
    CHECK(grey->baseColorFactor.r == Approx(0.2158f).margin(1e-4));
    CHECK(grey->baseColorFactor.g == Approx(0.2158f).margin(1e-4));
    CHECK(grey->baseColorFactor.b == Approx(0.2158f).margin(1e-4));
    CHECK(grey->baseColorFactor.a == Approx(1.0f));

    const auto noHash = rv::assets::parseMaterialDesc(parse(R"({"emissive": "FF0000"})"), "MAT_Red", "inline");
    REQUIRE(noHash.has_value());
    checkColor(noHash->emissiveFactor, glm::vec3(1.0f, 0.0f, 0.0f));

    const auto linearColor =
        rv::assets::parseMaterialDesc(parse(R"({"baseColorLinear": [0.5, 0.25, 0.125], "alpha": 0.5})"), "MAT_L", "x");
    REQUIRE(linearColor.has_value());
    CHECK(linearColor->baseColorFactor.r == Approx(0.5f));
    CHECK(linearColor->baseColorFactor.g == Approx(0.25f));
    CHECK(linearColor->baseColorFactor.a == Approx(0.5f));
}

TEST_CASE("assets/materials: every key is parsed", "[assets][materials]") {
    const auto material = rv::assets::parseMaterialDesc(parse(R"({
        "shading": "pbr", "baseColor": "#FFFFFF", "alpha": 0.75, "metallic": 0.25, "roughness": 0.4,
        "emissive": "#000000", "emissiveStrength": 3.5, "normalScale": -1.0, "occlusionStrength": 0.6,
        "alphaMode": "mask", "alphaCutoff": 0.3, "doubleSided": true,
        "baseColorTexture": "textures/wood/walnut_basecolor.png",
        "metallicRoughnessTexture": "textures/wood/walnut_mr.png",
        "normalTexture": "textures/wood/walnut_normal.png",
        "occlusionTexture": "textures/wood/walnut_ao.png",
        "emissiveTexture": "textures/wood/walnut_emissive.png"
    })"),
                                                        "MAT_Full", "inline");
    REQUIRE(material.has_value());
    CHECK(material->shading == ShadingModel::Pbr);
    CHECK(material->baseColorFactor.a == Approx(0.75f));
    CHECK(material->metallicFactor == Approx(0.25f));
    CHECK(material->roughnessFactor == Approx(0.4f));
    CHECK(material->emissiveStrength == Approx(3.5f));
    CHECK(material->normalScale == Approx(-1.0f));
    CHECK(material->occlusionStrength == Approx(0.6f));
    CHECK(material->alphaMode == AlphaMode::Mask);
    CHECK(material->alphaCutoff == Approx(0.3f));
    CHECK(material->doubleSided);
    CHECK(material->baseColorTexture.path == "textures/wood/walnut_basecolor.png");
    CHECK_FALSE(material->baseColorTexture.imageIndex.has_value());
    CHECK(material->metallicRoughnessTexture.path == "textures/wood/walnut_mr.png");
    CHECK(material->normalTexture.path == "textures/wood/walnut_normal.png");
    CHECK(material->occlusionTexture.path == "textures/wood/walnut_ao.png");
    CHECK(material->emissiveTexture.path == "textures/wood/walnut_emissive.png");

    const auto defaults = rv::assets::parseMaterialDesc(nlohmann::json::object(), "MAT_Default", "inline");
    REQUIRE(defaults.has_value());
    const MaterialDesc reference;
    CHECK(defaults->shading == reference.shading);
    CHECK(defaults->baseColorFactor == reference.baseColorFactor);
    CHECK(defaults->roughnessFactor == reference.roughnessFactor);
    CHECK(defaults->alphaMode == AlphaMode::Opaque);
    CHECK(defaults->baseColorTexture.empty());
}

TEST_CASE("assets/materials: texture slots name procedural textures", "[assets][materials][procedural]") {
    const auto material = rv::assets::parseMaterialDesc(parse(R"({
        "baseColorTexture": "procedural:walnut/baseColor",
        "metallicRoughnessTexture": "procedural:walnut/metallicRoughness",
        "normalTexture": "procedural:walnut/normal",
        "emissiveTexture": "textures/glow.png"
    })"),
                                                        "MAT_Walnut", "inline");
    REQUIRE(material.has_value());
    CHECK(material->baseColorTexture.procedural == "walnut/baseColor");
    CHECK(material->baseColorTexture.path.empty());
    CHECK_FALSE(material->baseColorTexture.empty());
    CHECK(material->metallicRoughnessTexture.procedural == "walnut/metallicRoughness");
    CHECK(material->normalTexture.procedural == "walnut/normal");
    CHECK(material->emissiveTexture.path == "textures/glow.png");
    CHECK(material->emissiveTexture.procedural.empty());
    CHECK(material->occlusionTexture.empty());

    // Unknown surfaces and maps, a surface without a normal map, and malformed names are parse errors at the slot.
    for (const char* reference : {"procedural:teak/baseColor", "procedural:walnut/albedo",
                                  "procedural:ornate_rug/normal", "procedural:walnut", "procedural:"}) {
        CAPTURE(reference);
        const auto bad = rv::assets::parseMaterialDesc(nlohmann::json{{"baseColorTexture", reference}}, "MAT_X",
                                                       "lib.json:/materials/MAT_X");
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().code == ErrorCode::ParseError);
        CHECK(bad.error().context == "lib.json:/materials/MAT_X/baseColorTexture");
    }
}

TEST_CASE("assets/materials: the CRT effect of unlit screens", "[assets][materials][crt]") {
    const auto screen = rv::assets::parseMaterialDesc(parse(R"({
        "shading": "unlit", "crt": {"lineCount": 480, "scanlineStrength": 0.5, "maskTriadCount": 640,
                                    "maskStrength": 0.1, "curvature": 0.2, "vignette": 0.4, "glassReflection": 0.5}
    })"),
                                                      "MAT_Tube", "inline");
    REQUIRE(screen.has_value());
    REQUIRE(screen->crt.has_value());
    CHECK(screen->crt->lineCount == 480.0f);
    CHECK(screen->crt->scanlineStrength == 0.5f);
    CHECK(screen->crt->maskTriadCount == 640.0f);
    CHECK(screen->crt->maskStrength == Approx(0.1f));
    CHECK(screen->crt->curvature == Approx(0.2f));
    CHECK(screen->crt->vignette == Approx(0.4f));
    CHECK(screen->crt->glassReflection == 0.5f);

    // Missing keys keep the defaults; materials without the block have none.
    const auto defaults =
        rv::assets::parseMaterialDesc(parse(R"({"shading": "unlit", "crt": {}})"), "MAT_Tube", "inline");
    REQUIRE(defaults.has_value());
    CHECK(defaults->crt == rv::assets::CrtEffect{});
    const auto plain = rv::assets::parseMaterialDesc(parse(R"({"shading": "unlit"})"), "MAT_Plain", "inline");
    REQUIRE(plain.has_value());
    CHECK_FALSE(plain->crt.has_value());

    struct Case {
        const char* json;
        ErrorCode code;
        const char* context;
    };
    const Case cases[] = {
        {R"({"crt": {}})", ErrorCode::ParseError, "m:/crt"}, // a lit material
        {R"({"shading": "unlit", "crt": []})", ErrorCode::ParseError, "m:/crt"},
        {R"({"shading": "unlit", "crt": {"scanlines": 0.3}})", ErrorCode::ParseError, "m:/crt/scanlines"},
        {R"({"shading": "unlit", "crt": {"lineCount": 8}})", ErrorCode::OutOfRange, "m:/crt/lineCount"},
        {R"({"shading": "unlit", "crt": {"maskTriadCount": 5000}})", ErrorCode::OutOfRange, "m:/crt/maskTriadCount"},
        {R"({"shading": "unlit", "crt": {"scanlineStrength": 1.5}})", ErrorCode::OutOfRange,
         "m:/crt/scanlineStrength"},
        {R"({"shading": "unlit", "crt": {"curvature": 0.6}})", ErrorCode::OutOfRange, "m:/crt/curvature"},
        {R"({"shading": "unlit", "crt": {"glassReflection": -0.1}})", ErrorCode::OutOfRange,
         "m:/crt/glassReflection"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.json);
        const auto material = rv::assets::parseMaterialDesc(parse(c.json), "MAT_X", "m:");
        REQUIRE_FALSE(material.has_value());
        CHECK(material.error().code == c.code);
        CHECK(material.error().context == c.context);
    }
}

TEST_CASE("assets/materials: invalid material definitions are errors with a JSON path", "[assets][materials]") {
    struct Case {
        const char* json;
        ErrorCode code;
        const char* context;
    };
    const Case cases[] = {
        {R"({"roughnes": 0.5})", ErrorCode::ParseError, "lib.json:/materials/MAT_X/roughnes"},
        {R"({"roughness": 1.5})", ErrorCode::OutOfRange, "lib.json:/materials/MAT_X/roughness"},
        {R"({"metallic": -0.1})", ErrorCode::OutOfRange, "lib.json:/materials/MAT_X/metallic"},
        {R"({"roughness": "rough"})", ErrorCode::ParseError, "lib.json:/materials/MAT_X/roughness"},
        {R"({"baseColor": "#12345"})", ErrorCode::ParseError, "lib.json:/materials/MAT_X/baseColor"},
        {R"({"baseColor": "#11223344"})", ErrorCode::ParseError, "lib.json:/materials/MAT_X/baseColor"},
        {R"({"baseColor": "#GG0000"})", ErrorCode::ParseError, "lib.json:/materials/MAT_X/baseColor"},
        {R"({"baseColor": "#FFFFFF", "baseColorLinear": [1, 1, 1]})", ErrorCode::ParseError,
         "lib.json:/materials/MAT_X/baseColorLinear"},
        {R"({"baseColorLinear": [1, 2, 1]})", ErrorCode::OutOfRange, "lib.json:/materials/MAT_X/baseColorLinear"},
        {R"({"shading": "toon"})", ErrorCode::ParseError, "lib.json:/materials/MAT_X/shading"},
        {R"({"alphaMode": "additive"})", ErrorCode::ParseError, "lib.json:/materials/MAT_X/alphaMode"},
        {R"({"doubleSided": 1})", ErrorCode::ParseError, "lib.json:/materials/MAT_X/doubleSided"},
        {R"({"emissiveStrength": -1})", ErrorCode::OutOfRange, "lib.json:/materials/MAT_X/emissiveStrength"},
        {R"({"normalTexture": "../outside.png"})", ErrorCode::InvalidArgument,
         "lib.json:/materials/MAT_X/normalTexture"},
        {R"({"baseColorTexture": "C:/textures/a.png"})", ErrorCode::InvalidArgument,
         "lib.json:/materials/MAT_X/baseColorTexture"},
        {R"({"baseColorTexture": 3})", ErrorCode::ParseError, "lib.json:/materials/MAT_X/baseColorTexture"},
        {R"([1, 2])", ErrorCode::ParseError, "lib.json:/materials/MAT_X"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.json);
        nlohmann::json document = {{"schemaVersion", 1}, {"materials", {{"MAT_X", parse(c.json)}}}};
        const auto library = MaterialLibrary::fromJson(document, "lib.json");
        REQUIRE_FALSE(library.has_value());
        CHECK(library.error().code == c.code);
        CHECK(library.error().context == c.context);
    }
}

TEST_CASE("assets/materials: library document structure", "[assets][materials]") {
    const auto version = MaterialLibrary::fromJson(parse(R"({"schemaVersion": 2, "materials": {}})"), "lib.json");
    REQUIRE_FALSE(version.has_value());
    CHECK(version.error().code == ErrorCode::Unsupported);
    CHECK(version.error().context == "lib.json:/schemaVersion");

    CHECK(MaterialLibrary::fromJson(parse(R"({"materials": {}})"), "lib.json").error().context ==
          "lib.json:/schemaVersion");
    CHECK(MaterialLibrary::fromJson(parse(R"({"schemaVersion": 1})"), "lib.json").error().context ==
          "lib.json:/materials");
    CHECK(MaterialLibrary::fromJson(parse(R"({"schemaVersion": 1, "materials": {}, "extra": 1})"), "lib.json")
              .error()
              .context == "lib.json:/extra");
    CHECK(MaterialLibrary::fromJson(parse(R"({"schemaVersion": 1, "materials": []})"), "lib.json").error().code ==
          ErrorCode::ParseError);
    CHECK(MaterialLibrary::fromJson(parse(R"({"schemaVersion": 1, "materials": {"": {}}})"), "lib.json").error().code ==
          ErrorCode::ParseError);

    const auto missingFile = MaterialLibrary::loadFromFile("does/not/exist.json");
    REQUIRE_FALSE(missingFile.has_value());
    CHECK(missingFile.error().code == ErrorCode::NotFound);
}

TEST_CASE("assets/materials: unknown names fall back to magenta and warn once per name", "[assets][materials]") {
    const auto library = MaterialLibrary::fromJson(
        parse(R"({"schemaVersion": 1, "materials": {"MAT_Wood_Oak": {"baseColor": "#76513A"}}})"), "lib.json");
    REQUIRE(library.has_value());
    CHECK(library->find("MAT_Unknown") == nullptr);
    CHECK(library->find("mat_wood_oak") == nullptr); // case-sensitive

    rv::test::LogCapture capture;
    const MaterialDesc& fallback = library->getOrFallback("MAT_Unknown");
    CHECK(fallback.name == "MAT_Fallback_Magenta");
    CHECK(fallback.shading == ShadingModel::Unlit);
    checkColor(glm::vec3(fallback.baseColorFactor), glm::vec3(1.0f, 0.0f, 1.0f));
    CHECK(&library->getOrFallback("MAT_Unknown") == &fallback);
    (void)library->getOrFallback("MAT_Other");
    CHECK(&library->getOrFallback("MAT_Wood_Oak") == library->find("MAT_Wood_Oak"));
    CHECK(capture.count("MAT_Unknown") == 1);
    CHECK(capture.count("MAT_Other") == 1);
    CHECK(capture.count("warning") == 2);
}

TEST_CASE("assets/materials: the file's fallback material wins over the built-in one", "[assets][materials]") {
    const auto library = MaterialLibrary::fromJson(
        parse(
            R"({"schemaVersion": 1, "materials": {"MAT_Fallback_Magenta": {"baseColor": "#FF00FF", "roughness": 0.9}}})"),
        "lib.json");
    REQUIRE(library.has_value());
    CHECK(library->fallback().shading == ShadingModel::Pbr);
    CHECK(library->fallback().roughnessFactor == Approx(0.9f));
    CHECK(&library->getOrFallback("MAT_Missing") == library->find("MAT_Fallback_Magenta"));

    const MaterialLibrary empty;
    CHECK(empty.size() == 0);
    CHECK(empty.names().empty());
    CHECK(empty.fallback().name == "MAT_Fallback_Magenta");
    CHECK(empty.fallback().shading == ShadingModel::Unlit);
    checkColor(glm::vec3(empty.getOrFallback("anything").baseColorFactor), glm::vec3(1.0f, 0.0f, 1.0f));
}
