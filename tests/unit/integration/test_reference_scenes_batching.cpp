// Cross-module integration checks (GPU-free): the reference scenes go through the same steps as in the application,
// scene::buildSceneGeometry, app::planSceneContent, then scene::BatchPlanner as gl::SceneRenderer calls it (handles in
// plan order, as the registries hand them out), so the batching the renderer achieves is pinned without a GPU; the key
// light's shadow map is planned the same way (gl::packLights, gl::shadowProjectionFor, the shadow casters).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rv/app/scene_content_plan.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/core/aabb.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/render/light_packing.hpp>
#include <rv/render/shadow_settings.hpp>
#include <rv/scene/batch_planner.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/render_item.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

namespace {

/// gl::SceneRenderer::kInitialInstanceCapacity: matrices per region of the instance buffer at creation (each view
/// writes its plan's instances and its shadow map's into one region).
constexpr std::size_t kInitialInstanceCapacity = 1024;

/// A reference scene's content as the application registers it: render items whose handles are indices into the
/// plan's meshes and materials (the registries hand them out in plan order).
struct ReferenceContent {
    rv::scene::SceneDescription scene;
    rv::app::SceneContentPlan content;
    std::vector<rv::scene::RenderItem> items;

    /// Options of BatchPlanner::plan with the plan's mesh bounds and material passes, like gl::SceneRenderer's.
    [[nodiscard]] rv::scene::PlanOptions planOptions() const {
        rv::scene::PlanOptions options;
        options.localBounds = [this](rv::scene::MeshHandle mesh) {
            return content.meshes[mesh.value].mesh->bounds;
        };
        options.passOf = [this](rv::scene::MaterialHandle material) {
            return rv::scene::drawPassOf(content.materials[material.value].material.alphaMode);
        };
        return options;
    }
};

ReferenceContent loadReferenceContent(std::string_view name) {
    const std::filesystem::path assets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    auto library = rv::assets::MaterialLibrary::loadFromFile(assets / "materials/base/materials.json");
    REQUIRE(library.has_value());
    auto scene = rv::scene::loadSceneDescription(assets / "style_tests/reference_scenes" /
                                                 rv::core::pathFromUtf8(std::string(name) + ".json"));
    REQUIRE(scene.has_value());
    const rv::scene::SceneGeometry geometry = rv::scene::buildSceneGeometry(*scene, *library);

    ReferenceContent reference;
    reference.content = rv::app::planSceneContent(geometry);
    for (const rv::app::PlannedItem& item : reference.content.items) {
        reference.items.push_back(
            rv::scene::RenderItem{rv::scene::MeshHandle{static_cast<std::uint32_t>(item.mesh)},
                                  rv::scene::MaterialHandle{static_cast<std::uint32_t>(item.material)},
                                  reference.content.materials[item.material].shader, item.model, item.castsShadow});
    }
    reference.scene = std::move(*scene);
    return reference;
}

struct PlannedView {
    rv::scene::SceneDescription scene;
    rv::app::SceneContentPlan content;
    rv::scene::BatchPlan batches;
};

/// Plans the default view of a reference scene the way the application does, turned by `turnDeg` about +Y in place.
PlannedView planReferenceView(std::string_view name, float turnDeg = 0.0f) {
    const ReferenceContent reference = loadReferenceContent(name);
    rv::scene::Camera camera;
    camera.setLens(reference.scene.camera.lens);
    camera.setPose(reference.scene.camera.position,
                   rv::core::degreesToRadians(reference.scene.camera.yawDeg + turnDeg),
                   rv::core::degreesToRadians(reference.scene.camera.pitchDeg));
    rv::scene::PlanOptions options = reference.planOptions();
    options.frustum = camera.frustum();
    options.eye = camera.position();
    PlannedView view;
    view.batches = rv::scene::BatchPlanner{}.plan(reference.items, options);
    view.scene = reference.scene;
    view.content = reference.content;
    return view;
}

} // namespace

TEST_CASE("integration/batching: instancing_stress draws 1000 boxes in three batches plus the floor",
          "[integration][batching]") {
    const PlannedView view = planReferenceView("instancing_stress");
    CHECK(view.content.items.size() == 1001);
    CHECK(view.batches.culledItems == 0); // the whole block is in view
    // One batch per box material, one for the floor.
    REQUIRE(view.batches.batches.size() == 4);
    std::size_t boxes = 0;
    for (const rv::scene::Batch& batch : view.batches.batches) {
        if (batch.instanceCount > 1) {
            boxes += batch.instanceCount;
        }
    }
    CHECK(boxes == 1000);
}

TEST_CASE("integration/batching: instancing_stress's sun shadows the block through a map fitted to it",
          "[integration][batching][shadow]") {
    // The directional shadow map as gl::SceneRenderer plans it: fitted to what the shadow pass draws.
    const ReferenceContent reference = loadReferenceContent("instancing_stress");
    rv::gl::PackedLights lights = rv::gl::packLights(reference.scene.lights, reference.scene.environment);
    const rv::core::Aabb casterBounds = rv::scene::shadowCasterBounds(reference.items, reference.planOptions());
    rv::gl::requireShadowCasters(lights, reference.scene.lights, casterBounds);
    REQUIRE(lights.shadowLight.casts());
    CHECK(lights.shadowLight.type == rv::scene::LightType::Directional);
    // The floor casts no shadow: the casters are the 10 x 10 x 10 boxes, 0.3 m wide and 0.6 m apart, on the floor.
    CHECK(rv::core::nearlyEqual(casterBounds.min, glm::vec3(-2.85f, 0.0f, -2.85f), 1e-4f));
    CHECK(rv::core::nearlyEqual(casterBounds.max, glm::vec3(2.85f, 5.7f, 2.85f), 1e-4f));
    const std::optional<rv::gl::ShadowProjection> shadow =
        rv::gl::shadowProjectionFor(lights.shadowLight, casterBounds, rv::gl::kShadowMapResolution);
    REQUIRE(shadow.has_value());
    CHECK_FALSE(shadow->perspective);

    rv::scene::PlanOptions options = reference.planOptions();
    options.frustum = rv::scene::Frustum::fromViewProjection(shadow->viewProjection);
    options.eye = shadow->eye;
    options.shadowCastersOnly = true;
    const rv::scene::BatchPlan plan = rv::scene::BatchPlanner{}.plan(reference.items, options);
    CHECK(plan.culledItems == 0);
    CHECK(plan.instanceTransforms.size() == 1000);
    CHECK(plan.batches.size() == 3);
    // With the view's 1001 instances a frame writes about 2000 into one region of the instance buffer: more than the
    // renderer starts with, so it grows on the first frame (unlike instancing_growth, which grows mid-run).
    const PlannedView view = planReferenceView("instancing_stress");
    CHECK(view.batches.instanceTransforms.size() + plan.instanceTransforms.size() > kInitialInstanceCapacity);
}

TEST_CASE("integration/batching: the test room draws fewer batches than parts", "[integration][batching]") {
    const PlannedView view = planReferenceView("m0_testroom");
    CHECK(view.content.items.size() == 231);
    // The scene camera leaves out what lies behind it or beside the view: the south wall, the side posters.
    CHECK(view.batches.culledItems == 6);
    // The 96 cartridge shells share one batch; each label has a cover material of its own, so it is a batch of one
    // (texture arrays for covers are in the backlog); so are the poster pictures and the sign's lettering.
    CHECK(view.batches.batches.size() == 129);
    CHECK(view.batches.batches.size() < view.content.items.size() - view.batches.culledItems);
    std::size_t largest = 0;
    for (const rv::scene::Batch& batch : view.batches.batches) {
        largest = std::max<std::size_t>(largest, batch.instanceCount);
    }
    CHECK(largest == 96);
}

TEST_CASE("integration/batching: instancing_growth outgrows the initial instance regions when turned around",
          "[integration][batching]") {
    // No light casts shadows: a view writes the instances of its own plan only.
    const rv::gl::PackedLights lights =
        rv::gl::packLights(loadReferenceContent("instancing_growth").scene.lights, rv::scene::EnvironmentSetup{});
    CHECK_FALSE(lights.shadowLight.casts());
    // The start view writes few instances, so the meshes are attached to the first buffer at frame 0...
    const PlannedView start = planReferenceView("instancing_growth");
    CHECK(start.content.items.size() == 1 + 36 + 3 * 768);
    CHECK(start.batches.instanceTransforms.size() < 100);
    // ...and turning around (tests/input_scripts/turn_around.json) brings more than a region holds into view: the
    // buffer grows mid-run and the meshes are attached to the new one.
    const PlannedView turned = planReferenceView("instancing_growth", 180.0f);
    CHECK(turned.batches.instanceTransforms.size() > kInitialInstanceCapacity);
    // The field's three bands and the floor (the start boxes are behind the camera now).
    CHECK(turned.batches.batches.size() == 4);
}

TEST_CASE("integration/batching: the test room's key light shadows the furniture of the hero wall",
          "[integration][batching][shadow]") {
    // The shadow map as gl::SceneRenderer plans it: the scene's key light (gl::packLights), its projection
    // (gl::shadowProjectionFor), then the items that cast shadows in its frustum (PlanOptions::shadowCastersOnly).
    const ReferenceContent reference = loadReferenceContent("m0_testroom");
    const rv::gl::PackedLights lights = rv::gl::packLights(reference.scene.lights, reference.scene.environment);
    REQUIRE(lights.shadowLight.casts());
    CHECK(lights.shadowNote.empty());
    CHECK(lights.shadowLight.type == rv::scene::LightType::Spot);

    const rv::core::Aabb casterBounds = rv::scene::shadowCasterBounds(reference.items, reference.planOptions());
    const std::optional<rv::gl::ShadowProjection> shadow =
        rv::gl::shadowProjectionFor(lights.shadowLight, casterBounds, rv::gl::kShadowMapResolution);
    REQUIRE(shadow.has_value());
    CHECK(shadow->perspective);
    rv::scene::PlanOptions options = reference.planOptions();
    options.frustum = rv::scene::Frustum::fromViewProjection(shadow->viewProjection);
    options.eye = shadow->eye;
    options.shadowCastersOnly = true;
    const rv::scene::BatchPlan plan = rv::scene::BatchPlanner{}.plan(reference.items, options);

    // Every part of an object instance shares its transform: an object casts when its transform is in the plan.
    const auto casts = [&plan](const rv::scene::SceneObject& object) {
        return std::find(plan.instanceTransforms.begin(), plan.instanceTransforms.end(), object.transform) !=
               plan.instanceTransforms.end();
    };
    // The hero corner and the main shelf with its cartridges are in the key's map; the furniture farther along the
    // north wall may fall outside it (lit by lights of its own).
    const auto inHeroCorner = [](const rv::scene::SceneObject& object) {
        return object.id == "shelf_main" || object.id == "tv_cabinet" || object.id == "crt_tv" ||
               object.id == "console" || (object.kind == "cartridge" && !object.id.starts_with("cartridge_left_"));
    };
    for (const rv::scene::SceneObject& object : reference.scene.objects) {
        INFO("object " << object.id);
        if (object.kind == "rug" || object.kind == "poster_frame") {
            // castsShadows false: a flat rug would only shadow the floor under it, the frames are thin geometry
            CHECK_FALSE(casts(object));
        } else if (inHeroCorner(object)) {
            CHECK(casts(object));
        }
    }
    // The ceiling above the light is outside its map; everything else of the room that casts is in it or culled.
    CHECK(plan.culledItems > 0);
    const auto nonCasting =
        static_cast<std::size_t>(std::count_if(reference.items.begin(), reference.items.end(),
                                               [](const rv::scene::RenderItem& item) { return !item.castsShadow; }));
    CHECK(nonCasting >= 11); // the rug and the two parts of each of the five frames
    CHECK(plan.instanceTransforms.size() + plan.culledItems + nonCasting == reference.items.size());
    // Batched like the view: the cartridge shells in the map (79 of the 96) share one draw.
    std::size_t largest = 0;
    for (const rv::scene::Batch& batch : plan.batches) {
        largest = std::max<std::size_t>(largest, batch.instanceCount);
    }
    CHECK(largest == 79);
}
