#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/scene_draw_plan.hpp>
#include <rv/assets/test_patterns.hpp>
#include <rv/geometry/prefabs.hpp>

using rv::app::DrawPass;
using rv::app::PlannedDraw;
using rv::app::PlannedMaterial;
using rv::app::SceneDrawPlan;
using rv::assets::AlphaMode;
using rv::assets::MaterialDesc;
using rv::scene::SceneGeometry;
using rv::scene::SceneObjectGeometry;
using rv::scene::ScenePart;
using rv::scene::ShaderKind;
using rv::scene::TextureSource;

namespace {

/// A width x height quad with uv 0..1 over it (a label, a poster picture, a screen).
rv::geometry::MeshData quad(float width, float height) {
    auto mesh = rv::geometry::buildPrimitive("quad", nlohmann::json{{"size", {width, height}}});
    REQUIRE(mesh.has_value());
    return std::move(*mesh);
}

MaterialDesc material(std::string name, AlphaMode alphaMode = AlphaMode::Opaque,
                      rv::assets::ShadingModel shading = rv::assets::ShadingModel::Pbr) {
    MaterialDesc desc;
    desc.name = std::move(name);
    desc.alphaMode = alphaMode;
    desc.shading = shading;
    return desc;
}

ScenePart part(std::string role, MaterialDesc desc, rv::geometry::MeshData mesh = quad(0.4f, 0.3f)) {
    ScenePart result;
    result.role = std::move(role);
    result.material = std::move(desc);
    result.mesh = std::move(mesh);
    return result;
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

/// Material of the `instance`-th draw of a part (draws of one part keep their instance order).
const PlannedMaterial& materialOf(const SceneDrawPlan& plan, std::size_t object, std::size_t part,
                                  std::size_t instance = 0) {
    const PlannedMaterial* found = nullptr;
    std::size_t seen = 0;
    for (const PlannedDraw& draw : plan.draws) {
        if (draw.object == object && draw.part == part && seen++ == instance) {
            found = &plan.materials.at(draw.material);
            break;
        }
    }
    INFO("object " << object << ", part " << part << ", instance " << instance);
    REQUIRE(found != nullptr);
    return *found;
}

} // namespace

TEST_CASE("app/scene_draw_plan: one material per name, one instance per object and role with a picture",
          "[app][scene_draw_plan]") {
    ScenePart label = part("label", material("MAT_Label_Print"));
    label.texture = cover("RETRO RALLY");
    ScenePart otherLabel = part("label", material("MAT_Label_Print"));
    otherLabel.texture = cover("STAR CORSAIR");
    ScenePart poster = part("picture", material("MAT_Paper_Print"));
    poster.texture = TextureSource{TextureSource::Kind::File, "textures/poster.png", {}, {}, 0};

    SceneGeometry geometry;
    geometry.objects.push_back(object("cart_a", {part("plastic_shell", material("MAT_Plastic_Gray")), label}));
    geometry.objects.push_back(object("cart_b", {part("plastic_shell", material("MAT_Plastic_Gray")), otherLabel}));
    // A grid: its instances share their rotation and scale, hence one material instance for the label.
    geometry.objects.push_back(object("grid", {label}, {at(0.0f), at(1.0f), at(2.0f)}));
    geometry.objects.push_back(object("poster", {part("frame", material("MAT_Wood_Oak")), poster}));

    const SceneDrawPlan plan = rv::app::planSceneDraws(geometry);
    CHECK(plan.draws.size() == 2 + 2 + 3 + 2);
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

TEST_CASE("app/scene_draw_plan: pattern pictures take the aspect ratio of their surface as drawn",
          "[app][scene_draw_plan]") {
    // Two 0.5 x 0.7 m posters with the same cover; one is scaled twice as wide, one is turned and moved.
    ScenePart poster = part("picture", material("MAT_Paper_Print"), quad(0.5f, 0.7f));
    poster.texture = cover("PIXEL PILOTS");
    const glm::mat4 wide = glm::scale(at(3.0f), glm::vec3(2.0f, 1.0f, 1.0f));
    const glm::mat4 turned = glm::rotate(at(-2.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    SceneGeometry geometry;
    geometry.objects.push_back(object("poster_plain", {poster}));
    geometry.objects.push_back(object("poster_wide", {poster}, {wide}));
    geometry.objects.push_back(object("poster_turned", {poster}, {turned}));
    // Instances of one object with different scales (not produced by scene files today) get one material each.
    geometry.objects.push_back(object("poster_mixed", {poster}, {glm::mat4(1.0f), wide}));

    const SceneDrawPlan plan = rv::app::planSceneDraws(geometry);
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
}

TEST_CASE("app/scene_draw_plan: opaque, then masked draws by shader and material, then blended ones in scene order",
          "[app][scene_draw_plan]") {
    using rv::assets::ShadingModel;
    SceneGeometry geometry;
    geometry.objects.push_back(object("glass_a", {part("glass", material("MAT_Glass", AlphaMode::Blend))}));
    geometry.objects.push_back(
        object("screen", {part("screen", material("MAT_Screen", AlphaMode::Opaque, ShadingModel::Unlit))}));
    geometry.objects.push_back(object("leaves", {part("leaf", material("MAT_Leaf", AlphaMode::Mask))}));
    geometry.objects.push_back(
        object("shelf", {part("wood", material("MAT_Wood")), part("glass", material("MAT_Glass", AlphaMode::Blend))},
               {at(0.0f), at(1.0f)}));
    geometry.objects.push_back(object("tint", {part("glass", material("MAT_Tint", AlphaMode::Blend))}));
    geometry.objects.push_back(object("box", {part("wood", material("MAT_Wood"))}));

    const SceneDrawPlan plan = rv::app::planSceneDraws(geometry);
    std::vector<std::string> order;
    for (const PlannedDraw& draw : plan.draws) {
        order.push_back(geometry.objects[draw.object].id + "/" + geometry.objects[draw.object].parts[draw.part].role +
                        "@" + std::to_string(static_cast<int>(draw.model[3].x)));
    }
    // Opaque PBR (MAT_Wood: both shelf instances in order, then the box), opaque unlit, masked, then every blended
    // draw in scene order whatever its material.
    CHECK(order == std::vector<std::string>{"shelf/wood@0", "shelf/wood@1", "box/wood@0", "screen/screen@0",
                                            "leaves/leaf@0", "glass_a/glass@0", "shelf/glass@0", "shelf/glass@1",
                                            "tint/glass@0"});

    std::vector<DrawPass> passes;
    for (const PlannedDraw& draw : plan.draws) {
        passes.push_back(plan.materials[draw.material].pass);
    }
    CHECK(std::is_sorted(passes.begin(), passes.end()));
    CHECK(plan.materials[plan.draws[3].material].shader == ShaderKind::Unlit);
    CHECK(rv::app::drawPassOf(AlphaMode::Mask) == DrawPass::Mask);
}
