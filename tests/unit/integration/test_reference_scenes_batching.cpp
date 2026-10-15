// Cross-module integration checks (GPU-free): the reference scenes go through the same steps as in the application,
// scene::buildSceneGeometry, app::planSceneContent, then scene::BatchPlanner as gl::SceneRenderer calls it (handles in
// plan order, as the registries hand them out), so the batching the renderer achieves is pinned without a GPU.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <rv/app/scene_content_plan.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/scene/batch_planner.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/render_item.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

namespace {

struct PlannedView {
    rv::scene::SceneDescription scene;
    rv::app::SceneContentPlan content;
    rv::scene::BatchPlan batches;
};

/// Plans the default view of a reference scene the way the application does, turned by `turnDeg` about +Y in place.
PlannedView planReferenceView(std::string_view name, float turnDeg = 0.0f) {
    const std::filesystem::path assets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    auto library = rv::assets::MaterialLibrary::loadFromFile(assets / "materials/base/materials.json");
    REQUIRE(library.has_value());
    auto scene = rv::scene::loadSceneDescription(assets / "style_tests/reference_scenes" /
                                                 rv::core::pathFromUtf8(std::string(name) + ".json"));
    REQUIRE(scene.has_value());
    const rv::scene::SceneGeometry geometry = rv::scene::buildSceneGeometry(*scene, *library);

    PlannedView view;
    view.content = rv::app::planSceneContent(geometry);
    std::vector<rv::scene::RenderItem> items;
    for (const rv::app::PlannedItem& item : view.content.items) {
        items.push_back(rv::scene::RenderItem{rv::scene::MeshHandle{static_cast<std::uint32_t>(item.mesh)},
                                              rv::scene::MaterialHandle{static_cast<std::uint32_t>(item.material)},
                                              view.content.materials[item.material].shader, item.model,
                                              item.castsShadow});
    }
    rv::scene::Camera camera;
    camera.setLens(scene->camera.lens);
    camera.setPose(scene->camera.position, rv::core::degreesToRadians(scene->camera.yawDeg + turnDeg),
                   rv::core::degreesToRadians(scene->camera.pitchDeg));
    rv::scene::PlanOptions options;
    options.frustum = camera.frustum();
    options.eye = camera.position();
    options.localBounds = [&view](rv::scene::MeshHandle mesh) {
        return view.content.meshes[mesh.value].mesh->bounds;
    };
    options.passOf = [&view](rv::scene::MaterialHandle material) {
        return rv::scene::drawPassOf(view.content.materials[material.value].material.alphaMode);
    };
    view.batches = rv::scene::BatchPlanner{}.plan(items, options);
    view.scene = std::move(*scene);
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

TEST_CASE("integration/batching: the test room draws fewer batches than parts", "[integration][batching]") {
    const PlannedView view = planReferenceView("m0_testroom");
    CHECK(view.content.items.size() == 71);
    CHECK(view.batches.culledItems == 0);
    // The 24 cartridge shells share one batch; each label has a cover material of its own, so it is a batch of one
    // (texture arrays for covers are in the backlog); so are the two poster frames.
    CHECK(view.batches.batches.size() == 47);
    std::size_t largest = 0;
    for (const rv::scene::Batch& batch : view.batches.batches) {
        largest = std::max<std::size_t>(largest, batch.instanceCount);
    }
    CHECK(largest == 24);
}

TEST_CASE("integration/batching: instancing_growth outgrows the initial instance regions when turned around",
          "[integration][batching]") {
    // gl::SceneRenderer::kInitialInstanceCapacity: matrices per region of the instance buffer at creation.
    constexpr std::size_t kInitialInstanceCapacity = 1024;
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
