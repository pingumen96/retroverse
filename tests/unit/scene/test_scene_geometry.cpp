#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <rv/assets/material_library.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

using nlohmann::json;
using rv::scene::SceneGeometry;
using rv::scene::SceneObjectGeometry;
using rv::scene::ScenePart;

namespace {

std::filesystem::path assetDir() {
    return rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
}

rv::assets::MaterialLibrary realMaterials() {
    auto library = rv::assets::MaterialLibrary::loadFromFile(assetDir() / "materials/base/materials.json");
    REQUIRE(library.has_value());
    return std::move(*library);
}

rv::scene::SceneDescription referenceScene(std::string_view name) {
    auto scene = rv::scene::loadSceneDescription(assetDir() / "style_tests/reference_scenes" /
                                                 rv::core::pathFromUtf8(std::string(name) + ".json"));
    REQUIRE(scene.has_value());
    return std::move(*scene);
}

const SceneObjectGeometry& object(const SceneGeometry& geometry, std::string_view id) {
    INFO("object " << id);
    const auto found = std::find_if(geometry.objects.begin(), geometry.objects.end(),
                                    [id](const SceneObjectGeometry& candidate) { return candidate.id == id; });
    REQUIRE(found != geometry.objects.end());
    return *found;
}

const ScenePart& part(const SceneObjectGeometry& owner, std::string_view role) {
    INFO("object " << owner.id << ", part " << role);
    const auto found = std::find_if(owner.parts.begin(), owner.parts.end(),
                                    [role](const ScenePart& candidate) { return candidate.role == role; });
    REQUIRE(found != owner.parts.end());
    return *found;
}

glm::vec4 linear(std::string_view hex) {
    const auto color = rv::core::hexToLinearRgb(hex);
    REQUIRE(color.has_value());
    return glm::vec4(*color, 1.0f);
}

} // namespace

TEST_CASE("scene/scene_geometry: the M0 test room resolves every part to a library material", "[scene][geometry]") {
    const auto materials = realMaterials();
    const auto scene = referenceScene("m0_testroom");
    const SceneGeometry geometry = rv::scene::buildSceneGeometry(scene, materials);

    REQUIRE(geometry.objects.size() == scene.objects.size());
    for (std::size_t i = 0; i < scene.objects.size(); ++i) {
        CHECK(geometry.objects[i].id == scene.objects[i].id); // file order
        CHECK_FALSE(geometry.objects[i].placeholder);
        CHECK(geometry.objects[i].instances.size() == 1);
    }
    CHECK(geometry.placeholderCount() == 0);
    CHECK(geometry.fallbackMaterialCount() == 0);
    CHECK(geometry.instanceCount() == scene.objects.size());

    const SceneObjectGeometry& room = object(geometry, "room");
    CHECK(part(room, "floor").material.name == "MAT_Wood_Oak");
    CHECK(part(room, "wall").material.name == "MAT_Wall_Painted");
    CHECK(rv::core::nearlyEqual(part(room, "wall").material.baseColorFactor, linear("#383735")));

    const SceneObjectGeometry& cartridge = object(geometry, "cartridge_2_3");
    REQUIRE(cartridge.parts.size() == 2);
    CHECK(part(cartridge, "plastic_shell").material.name == "MAT_Plastic_Gray");
    CHECK(part(cartridge, "label").material.name == "MAT_Label_Print");
    CHECK(part(cartridge, "label").mesh.name == "cartridge/label");
    CHECK(rv::core::nearlyEqual(cartridge.instances.front(), scene.findObject("cartridge_2_3")->transform));

    const SceneObjectGeometry& crt = object(geometry, "crt_tv");
    CHECK(part(crt, "screen").material.name == "MAT_Screen_Emissive");
    CHECK(part(crt, "plastic_body").material.name == "MAT_Plastic_AgedIvory");

    std::size_t triangles = 0;
    for (const SceneObjectGeometry& built : geometry.objects) {
        for (const ScenePart& builtPart : built.parts) {
            CHECK_FALSE(builtPart.mesh.empty());
            triangles += builtPart.mesh.triangleCount();
        }
    }
    CHECK(geometry.drawnTriangleCount() == triangles); // one instance per object
}

TEST_CASE("scene/scene_geometry: primitives use their kind as role and grids expand", "[scene][geometry]") {
    const auto materials = realMaterials();
    const SceneGeometry geometry = rv::scene::buildSceneGeometry(referenceScene("instancing_stress"), materials);

    REQUIRE(geometry.objects.size() == 4);
    CHECK(geometry.instanceCount() == 1001);
    CHECK(geometry.meshCount() == 4);
    const SceneObjectGeometry& walnut = object(geometry, "boxes_walnut");
    REQUIRE(walnut.parts.size() == 1);
    CHECK(walnut.parts.front().role == "box");
    CHECK(walnut.parts.front().material.name == "MAT_Wood_Walnut");
    CHECK(walnut.instances.size() == 400);
    CHECK(object(geometry, "floor").parts.front().role == "plane");
    CHECK(geometry.drawnTriangleCount() == 1000 * 12 + 2);
}

TEST_CASE("scene/scene_geometry: inline materials are parsed with the library rules", "[scene][geometry]") {
    const auto materials = realMaterials();
    const SceneGeometry geometry = rv::scene::buildSceneGeometry(referenceScene("material_test"), materials);
    CHECK(geometry.fallbackMaterialCount() == 0);

    const ScenePart& sphere = object(geometry, "sphere_m2_r3").parts.front();
    CHECK(sphere.role == "sphere");
    CHECK_FALSE(sphere.fallbackMaterial);
    CHECK(sphere.material.name == "sphere_m2_r3.inlineMaterial");
    CHECK(rv::core::nearlyEqual(sphere.material.baseColorFactor, linear("#C24A3A")));
    CHECK(sphere.material.metallicFactor == 0.5f);
    CHECK(sphere.material.roughnessFactor == 0.7625f);
}

TEST_CASE("scene/scene_geometry: missing or bad content falls back to visible magenta", "[scene][geometry]") {
    const auto materials = realMaterials();
    const json document = json::parse(R"({
        "schemaVersion": 1,
        "name": "fallbacks",
        "objects": [
            {"id": "unknown_material", "primitive": "box", "material": "MAT_Does_Not_Exist"},
            {"id": "no_material", "primitive": "quad"},
            {"id": "bad_inline", "primitive": "sphere", "inlineMaterial": {"roughness": 0.5, "shininess": 3}},
            {"id": "bad_params", "prefab": "shelf", "position": [1, 0, 2], "params": {"width": 99.0},
             "material": "MAT_Wood_Walnut"},
            {"id": "model", "model": "models/crt.glb", "grid": {"counts": [2, 1, 1], "spacing": [1, 1, 1]}},
            {"id": "per_role", "prefab": "cartridge", "material": "MAT_Plastic_Black",
             "materials": {"label": "MAT_Paper_Print"}}
        ]
    })");
    const auto scene = rv::scene::parseSceneDescription(document, "fallbacks.json");
    REQUIRE(scene.has_value());
    const SceneGeometry geometry = rv::scene::buildSceneGeometry(*scene, materials);
    REQUIRE(geometry.objects.size() == 6);
    const glm::vec4 magenta(1.0f, 0.0f, 1.0f, 1.0f);

    for (const char* id : {"unknown_material", "no_material", "bad_inline"}) {
        INFO(id);
        const SceneObjectGeometry& built = object(geometry, id);
        CHECK_FALSE(built.placeholder); // the geometry itself is fine
        REQUIRE(built.parts.size() == 1);
        CHECK(built.parts.front().fallbackMaterial);
        CHECK(built.parts.front().material.name == rv::assets::kFallbackMaterialName);
        CHECK(rv::core::nearlyEqual(built.parts.front().material.baseColorFactor, magenta));
    }

    for (const char* id : {"bad_params", "model"}) {
        INFO(id);
        const SceneObjectGeometry& built = object(geometry, id);
        CHECK(built.placeholder);
        REQUIRE(built.parts.size() == 1);
        const ScenePart& cube = built.parts.front();
        CHECK(cube.role == rv::scene::kPlaceholderRole);
        CHECK(cube.fallbackMaterial);
        CHECK(rv::core::nearlyEqual(cube.material.baseColorFactor, magenta));
        // A kPlaceholderSize cube standing on the object origin.
        CHECK(rv::core::nearlyEqual(cube.mesh.bounds.min, glm::vec3(-0.25f, 0.0f, -0.25f)));
        CHECK(rv::core::nearlyEqual(cube.mesh.bounds.max, glm::vec3(0.25f, 0.5f, 0.25f)));
    }
    CHECK(object(geometry, "model").instances.size() == 2); // the grid still applies to the placeholder
    CHECK(rv::core::nearlyEqual(glm::vec3(object(geometry, "bad_params").instances.front()[3]), glm::vec3(1, 0, 2)));

    const SceneObjectGeometry& perRole = object(geometry, "per_role");
    CHECK(part(perRole, "label").material.name == "MAT_Paper_Print");
    CHECK(part(perRole, "plastic_shell").material.name == "MAT_Plastic_Black");
    CHECK_FALSE(part(perRole, "label").fallbackMaterial);

    CHECK(geometry.placeholderCount() == 2);
    CHECK(geometry.fallbackMaterialCount() == 5);
}

TEST_CASE("scene/scene_geometry: a library without the file still has the built-in fallback", "[scene][geometry]") {
    const rv::assets::MaterialLibrary empty;
    const SceneGeometry geometry = rv::scene::buildSceneGeometry(referenceScene("instancing_stress"), empty);
    CHECK(geometry.fallbackMaterialCount() == geometry.meshCount());
    for (const SceneObjectGeometry& built : geometry.objects) {
        for (const ScenePart& builtPart : built.parts) {
            CHECK(builtPart.material.name == rv::assets::kFallbackMaterialName);
        }
    }
}
