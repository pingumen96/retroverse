#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/scene_content_plan.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/assets/test_patterns.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/scene/scene_description.hpp>

using rv::app::PlannedItem;
using rv::app::PlannedMaterial;
using rv::app::SceneContentPlan;
using rv::assets::AlphaMode;
using rv::assets::MaterialDesc;
using rv::scene::SceneGeometry;
using rv::scene::SceneObjectGeometry;
using rv::scene::ScenePart;
using rv::scene::ShaderKind;
using rv::scene::TextureSource;

namespace {

/// A width x height quad with uv 0..1 over it (a label, a poster picture, a screen), keyed by its size.
ScenePart quadPart(std::string role, MaterialDesc desc, float width = 0.4f, float height = 0.3f) {
    auto mesh = rv::geometry::buildPrimitive("quad", nlohmann::json{{"size", {width, height}}});
    REQUIRE(mesh.has_value());
    ScenePart result;
    result.role = std::move(role);
    result.material = std::move(desc);
    result.mesh = std::make_shared<const rv::geometry::MeshData>(std::move(*mesh));
    result.meshKey = "primitive:quad:" + std::to_string(width) + "x" + std::to_string(height) + "#" + result.role;
    return result;
}

MaterialDesc material(std::string name, AlphaMode alphaMode = AlphaMode::Opaque,
                      rv::assets::ShadingModel shading = rv::assets::ShadingModel::Pbr) {
    MaterialDesc desc;
    desc.name = std::move(name);
    desc.alphaMode = alphaMode;
    desc.shading = shading;
    return desc;
}

TextureSource cover(std::string title) {
    TextureSource source;
    source.kind = TextureSource::Kind::Pattern;
    source.pattern = "cover";
    source.title = std::move(title);
    source.seed = 7;
    return source;
}

SceneObjectGeometry object(std::string id, std::vector<ScenePart> parts,
                           std::vector<glm::mat4> instances = {glm::mat4(1.0f)}) {
    SceneObjectGeometry result;
    result.id = std::move(id);
    result.parts = std::move(parts);
    result.instances = std::move(instances);
    return result;
}

glm::mat4 at(float x) {
    return glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 0.0f));
}

/// Material of the `instance`-th item of a part (items of one part keep their instance order).
const PlannedMaterial& materialOf(const SceneContentPlan& plan, std::size_t object, std::size_t part,
                                  std::size_t instance = 0) {
    const PlannedMaterial* found = nullptr;
    std::size_t seen = 0;
    for (const PlannedItem& item : plan.items) {
        if (item.object == object && item.part == part && seen++ == instance) {
            found = &plan.materials.at(item.material);
            break;
        }
    }
    INFO("object " << object << ", part " << part << ", instance " << instance);
    REQUIRE(found != nullptr);
    return *found;
}

SceneGeometry referenceGeometry(std::string_view name) {
    const std::filesystem::path assets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    auto library = rv::assets::MaterialLibrary::loadFromFile(assets / "materials/base/materials.json");
    REQUIRE(library.has_value());
    auto scene = rv::scene::loadSceneDescription(assets / "style_tests/reference_scenes" /
                                                 rv::core::pathFromUtf8(std::string(name) + ".json"));
    REQUIRE(scene.has_value());
    return rv::scene::buildSceneGeometry(*scene, *library);
}

} // namespace

TEST_CASE("app/scene_content_plan: one material per name, one instance per object and role with a picture",
          "[app][scene_content_plan]") {
    ScenePart label = quadPart("label", material("MAT_Label_Print"));
    label.texture = cover("RETRO RALLY");
    ScenePart otherLabel = quadPart("label", material("MAT_Label_Print"));
    otherLabel.texture = cover("STAR CORSAIR");
    ScenePart poster = quadPart("picture", material("MAT_Paper_Print"));
    poster.texture = TextureSource{TextureSource::Kind::File, "textures/poster.png", {}, {}, 0};

    SceneGeometry geometry;
    geometry.objects.push_back(object("cart_a", {quadPart("plastic_shell", material("MAT_Plastic_Gray")), label}));
    geometry.objects.push_back(object("cart_b", {quadPart("plastic_shell", material("MAT_Plastic_Gray")), otherLabel}));
    // A grid: its instances share their rotation and scale, hence one material instance for the label.
    geometry.objects.push_back(object("grid", {label}, {at(0.0f), at(1.0f), at(2.0f)}));
    geometry.objects.push_back(object("poster", {quadPart("frame", material("MAT_Wood_Oak")), poster}));

    const SceneContentPlan plan = rv::app::planSceneContent(geometry);
    CHECK(plan.items.size() == 2 + 2 + 3 + 2);
    std::vector<std::string> keys;
    for (const PlannedMaterial& planned : plan.materials) {
        keys.push_back(planned.key);
    }
    const glm::ivec2 labelSize = rv::assets::patternSizeForAspect(0.4f / 0.3f);
    const std::string size = std::to_string(labelSize.x) + "x" + std::to_string(labelSize.y);
    // Materials in order of first use: the shared shell material once, an instance per label, the frame, the picture.
    CHECK(keys == std::vector<std::string>{"MAT_Plastic_Gray", "cart_a/label@" + size, "cart_b/label@" + size,
                                           "grid/label@" + size, "MAT_Wood_Oak", "poster/picture"});
    CHECK(plan.pictureMaterialCount() == 4);

    const PlannedMaterial& shell = materialOf(plan, 0, 0);
    CHECK(shell.label == "MAT_Plastic_Gray");
    CHECK_FALSE(shell.picture.has_value());
    CHECK(shell.patternSize == glm::ivec2(0));
    CHECK(&materialOf(plan, 1, 0) == &shell);

    const PlannedMaterial& labelA = materialOf(plan, 0, 1);
    CHECK(labelA.label == "cart_a/label@" + size + " (MAT_Label_Print)");
    REQUIRE(labelA.picture.has_value());
    CHECK(labelA.picture->title == "RETRO RALLY");
    CHECK(labelA.patternSize == labelSize);
    CHECK(materialOf(plan, 1, 1).picture->title == "STAR CORSAIR");
    CHECK(&materialOf(plan, 2, 0, 0) == &materialOf(plan, 2, 0, 2));

    // A picture file keeps its own size: no pattern size in the key.
    const PlannedMaterial& picture = materialOf(plan, 3, 1);
    REQUIRE(picture.picture.has_value());
    CHECK(picture.picture->kind == TextureSource::Kind::File);
    CHECK(picture.picture->path == "textures/poster.png");
    CHECK(picture.patternSize == glm::ivec2(0));
}

TEST_CASE("app/scene_content_plan: pattern pictures take the aspect ratio of their surface as drawn",
          "[app][scene_content_plan]") {
    // Two 0.5 x 0.7 m posters with the same cover; one is scaled twice as wide, one is turned and moved.
    ScenePart poster = quadPart("picture", material("MAT_Paper_Print"), 0.5f, 0.7f);
    poster.texture = cover("PIXEL PILOTS");
    const glm::mat4 wide = glm::scale(at(3.0f), glm::vec3(2.0f, 1.0f, 1.0f));
    const glm::mat4 turned = glm::rotate(at(-2.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    SceneGeometry geometry;
    geometry.objects.push_back(object("poster_plain", {poster}));
    geometry.objects.push_back(object("poster_wide", {poster}, {wide}));
    geometry.objects.push_back(object("poster_turned", {poster}, {turned}));
    // Instances of one object with different scales (not produced by scene files today) get one material each.
    geometry.objects.push_back(object("poster_mixed", {poster}, {glm::mat4(1.0f), wide}));

    const SceneContentPlan plan = rv::app::planSceneContent(geometry);
    const glm::ivec2 tall = rv::assets::patternSizeForAspect(0.5f / 0.7f);
    const glm::ivec2 broad = rv::assets::patternSizeForAspect(1.0f / 0.7f);
    REQUIRE(tall.x < tall.y);
    REQUIRE(broad.x > broad.y);
    CHECK(materialOf(plan, 0, 0).patternSize == tall);
    CHECK(materialOf(plan, 1, 0).patternSize == broad);
    CHECK(materialOf(plan, 2, 0).patternSize == tall);
    CHECK(materialOf(plan, 3, 0, 0).patternSize == tall);
    CHECK(materialOf(plan, 3, 0, 1).patternSize == broad);
    CHECK(&materialOf(plan, 3, 0, 0) != &materialOf(plan, 3, 0, 1));
    CHECK(plan.materials.size() == 5);
    // The four posters share one mesh.
    CHECK(plan.meshes.size() == 1);
}

TEST_CASE("app/scene_content_plan: materials keep their shader kind; items keep the shadow flag",
          "[app][scene_content_plan]") {
    using rv::assets::ShadingModel;
    SceneGeometry geometry;
    geometry.objects.push_back(
        object("screen", {quadPart("screen", material("MAT_Screen", AlphaMode::Opaque, ShadingModel::Unlit))}));
    geometry.objects.push_back(object("glass", {quadPart("glass", material("MAT_Glass", AlphaMode::Blend))}));
    geometry.objects.back().castsShadows = false;

    const SceneContentPlan plan = rv::app::planSceneContent(geometry);
    REQUIRE(plan.items.size() == 2);
    CHECK(plan.materials[plan.items[0].material].shader == ShaderKind::Unlit);
    CHECK(plan.materials[plan.items[1].material].shader == ShaderKind::Pbr);
    CHECK(plan.materials[plan.items[1].material].material.alphaMode == AlphaMode::Blend);
    CHECK(plan.items[0].castsShadow);
    CHECK_FALSE(plan.items[1].castsShadow);
}

TEST_CASE("app/scene_content_plan: the test room uploads 23 meshes for its 71 parts", "[app][scene_content_plan]") {
    const SceneGeometry geometry = referenceGeometry("m0_testroom");
    const SceneContentPlan plan = rv::app::planSceneContent(geometry);
    CHECK(plan.items.size() == 71);
    CHECK(plan.meshes.size() == 23);
    CHECK(plan.modelInstances.empty());

    // Every mesh key is distinct, and the 24 cartridges point at the same two meshes.
    std::set<std::string> keys;
    for (const rv::app::PlannedMesh& mesh : plan.meshes) {
        keys.insert(mesh.key);
        CHECK(mesh.mesh != nullptr);
    }
    CHECK(keys.size() == plan.meshes.size());
    std::set<std::size_t> cartridgeMeshes;
    std::set<std::size_t> labelMaterials;
    for (const PlannedItem& item : plan.items) {
        if (geometry.objects[item.object].id.starts_with("cartridge_")) {
            cartridgeMeshes.insert(item.mesh);
            if (geometry.objects[item.object].parts[item.part].role == "label") {
                labelMaterials.insert(item.material);
            }
        }
    }
    CHECK(cartridgeMeshes.size() == 2);
    // Each label shows its own cover: 24 material instances (a texture array is a backlog item).
    CHECK(labelMaterials.size() == 24);
    // 39 materials: 12 by name, 24 labels, 2 posters and the CRT screen.
    CHECK(plan.materials.size() == 39);
    CHECK(plan.pictureMaterialCount() == 27);
}

TEST_CASE("app/scene_content_plan: instancing_stress shares one box mesh over three grids",
          "[app][scene_content_plan]") {
    const SceneContentPlan plan = rv::app::planSceneContent(referenceGeometry("instancing_stress"));
    CHECK(plan.items.size() == 1001);
    CHECK(plan.meshes.size() == 2); // the floor plane and the box
    CHECK(plan.materials.size() == 4);
}

TEST_CASE("app/scene_content_plan: model objects are planned as model instances", "[app][scene_content_plan]") {
    SceneGeometry geometry;
    SceneObjectGeometry model = object("model", {}, {at(0.0f), at(2.0f)});
    model.model = std::make_shared<const rv::assets::ModelData>();
    model.modelPath = "models/crt.glb";
    model.castsShadows = false;
    geometry.objects.push_back(object("floor", {quadPart("floor", material("MAT_Wood_Oak"))}));
    geometry.objects.push_back(model);

    const SceneContentPlan plan = rv::app::planSceneContent(geometry);
    CHECK(plan.items.size() == 1);
    REQUIRE(plan.modelInstances.size() == 2);
    CHECK(plan.modelInstances[0].object == 1);
    CHECK(plan.modelInstances[1].model == at(2.0f));
    CHECK_FALSE(plan.modelInstances[0].castsShadow);
}
