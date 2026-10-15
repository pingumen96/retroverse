#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/ext/matrix_transform.hpp>

#include <rv/core/aabb.hpp>
#include <rv/scene/batch_planner.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/render_item.hpp>

using rv::core::Aabb;
using rv::scene::Batch;
using rv::scene::BatchPlan;
using rv::scene::BatchPlanner;
using rv::scene::MaterialHandle;
using rv::scene::MeshHandle;
using rv::scene::RenderItem;
using rv::scene::ShaderKind;

namespace {

/// Item whose model matrix encodes `tag` in its x translation, so tests can follow it through the plan.
RenderItem item(std::uint32_t mesh, std::uint32_t material, ShaderKind shader, float tag, float z = -5.0f) {
    RenderItem result;
    result.mesh = MeshHandle{mesh};
    result.material = MaterialHandle{material};
    result.shader = shader;
    result.model = glm::translate(glm::mat4(1.0f), glm::vec3(tag, 0.0f, z));
    return result;
}

float tagOf(const glm::mat4& model) {
    return model[3][0];
}

/// Options that cull against `frustum` with `localBounds`.
rv::scene::PlanOptions culling(const rv::scene::Frustum& frustum, const std::function<Aabb(MeshHandle)>& localBounds) {
    rv::scene::PlanOptions options;
    options.frustum = frustum;
    options.localBounds = localBounds;
    return options;
}

/// Every batch is non-empty and the batches tile instanceTransforms contiguously and in order.
void checkContiguous(const BatchPlan& plan) {
    std::uint32_t next = 0;
    for (const Batch& batch : plan.batches) {
        CHECK(batch.instanceCount > 0);
        CHECK(batch.firstInstance == next);
        next += batch.instanceCount;
    }
    CHECK(next == plan.instanceTransforms.size());
}

} // namespace

TEST_CASE("scene/batch_planner: groups by shader, material and mesh in key order", "[scene][batch_planner]") {
    const std::vector<RenderItem> items{
        item(2, 7, ShaderKind::Unlit, 0.0f), item(1, 3, ShaderKind::Pbr, 1.0f), item(2, 3, ShaderKind::Pbr, 2.0f),
        item(1, 3, ShaderKind::Pbr, 3.0f),   item(1, 1, ShaderKind::Pbr, 4.0f), item(2, 7, ShaderKind::Unlit, 5.0f),
        item(1, 1, ShaderKind::Unlit, 6.0f),
    };
    const BatchPlan plan = BatchPlanner{}.plan(items);
    REQUIRE(plan.batches.size() == 5);
    checkContiguous(plan);

    // Shader first (all Pbr before Unlit), then material, then mesh.
    const auto expect = [&plan](std::size_t index, ShaderKind shader, std::uint32_t material, std::uint32_t mesh,
                                std::uint32_t count) {
        CHECK(plan.batches[index].shader == shader);
        CHECK(plan.batches[index].material == MaterialHandle{material});
        CHECK(plan.batches[index].mesh == MeshHandle{mesh});
        CHECK(plan.batches[index].instanceCount == count);
    };
    expect(0, ShaderKind::Pbr, 1, 1, 1);
    expect(1, ShaderKind::Pbr, 3, 1, 2);
    expect(2, ShaderKind::Pbr, 3, 2, 1);
    expect(3, ShaderKind::Unlit, 1, 1, 1);
    expect(4, ShaderKind::Unlit, 7, 2, 2);

    // Instances follow the batches; same-key items keep their submission order (stable).
    std::vector<float> tags;
    for (const glm::mat4& model : plan.instanceTransforms) {
        tags.push_back(tagOf(model));
    }
    CHECK(tags == std::vector<float>{4.0f, 1.0f, 3.0f, 2.0f, 6.0f, 0.0f, 5.0f});
}

TEST_CASE("scene/batch_planner: 1000 items with 3 materials and 1 mesh make 3 batches", "[scene][batch_planner]") {
    std::vector<RenderItem> items;
    for (std::uint32_t i = 0; i < 1000; ++i) {
        items.push_back(item(42, 10 + (i % 3), ShaderKind::Pbr, static_cast<float>(i)));
    }
    const BatchPlan plan = BatchPlanner{}.plan(items);
    REQUIRE(plan.batches.size() == 3);
    CHECK(plan.instanceTransforms.size() == 1000);
    checkContiguous(plan);
    CHECK(plan.batches[0].material == MaterialHandle{10});
    CHECK(plan.batches[0].instanceCount == 334);
    CHECK(plan.batches[1].material == MaterialHandle{11});
    CHECK(plan.batches[1].instanceCount == 333);
    CHECK(plan.batches[2].material == MaterialHandle{12});
    CHECK(plan.batches[2].instanceCount == 333);
    for (const Batch& batch : plan.batches) {
        CHECK(batch.mesh == MeshHandle{42});
        // Inside a batch the original order is kept: tags grow by 3.
        for (std::uint32_t k = 1; k < batch.instanceCount; ++k) {
            CHECK(tagOf(plan.instanceTransforms[batch.firstInstance + k]) ==
                  tagOf(plan.instanceTransforms[batch.firstInstance + k - 1]) + 3.0f);
        }
    }

    // The plan is a pure function of its input.
    const BatchPlan again = BatchPlanner{}.plan(items);
    CHECK(again.instanceTransforms == plan.instanceTransforms);
    REQUIRE(again.batches.size() == plan.batches.size());
    for (std::size_t i = 0; i < plan.batches.size(); ++i) {
        CHECK(again.batches[i].firstInstance == plan.batches[i].firstInstance);
        CHECK(again.batches[i].instanceCount == plan.batches[i].instanceCount);
    }
}

TEST_CASE("scene/batch_planner: frustum culling uses the world bounds of each instance", "[scene][batch_planner]") {
    rv::scene::Camera camera;
    camera.setPose(glm::vec3(0.0f), 0.0f, 0.0f); // looking down -Z
    const rv::scene::Frustum frustum = camera.frustum();

    int boundsCalls = 0;
    const std::function<Aabb(MeshHandle)> localBounds = [&boundsCalls](MeshHandle mesh) {
        ++boundsCalls;
        // Mesh 1: unit cube. Mesh 2: unknown bounds (empty box), never culled.
        return mesh == MeshHandle{1} ? Aabb::fromCenterHalfExtents(glm::vec3(0.0f), glm::vec3(0.5f)) : Aabb{};
    };

    const std::vector<RenderItem> items{
        item(1, 1, ShaderKind::Pbr, 0.0f, -5.0f),  // in front: kept
        item(1, 1, ShaderKind::Pbr, 0.0f, 5.0f),   // behind: culled
        item(1, 1, ShaderKind::Pbr, 1.0f, -10.0f), // in front: kept
        item(1, 2, ShaderKind::Pbr, 0.0f, 5.0f),   // only instance of material 2, behind: no batch at all
        item(2, 1, ShaderKind::Pbr, 0.0f, 5.0f),   // behind but bounds unknown: kept
        item(1, 1, ShaderKind::Pbr, 30.0f, -5.0f), // far to the right of the 75-degree view: culled
    };
    const BatchPlan plan = BatchPlanner{}.plan(items, culling(frustum, localBounds));
    CHECK(plan.culledItems == 3);
    REQUIRE(plan.batches.size() == 2);
    checkContiguous(plan);
    CHECK(plan.batches[0].mesh == MeshHandle{1});
    CHECK(plan.batches[0].material == MaterialHandle{1});
    CHECK(plan.batches[0].instanceCount == 2);
    CHECK(plan.instanceTransforms[0][3][2] == -5.0f);
    CHECK(plan.instanceTransforms[1][3][2] == -10.0f);
    CHECK(plan.batches[1].mesh == MeshHandle{2});
    CHECK(plan.batches[1].instanceCount == 1);
    // Sorted runs: (mesh 1, material 1), (mesh 2, material 1), (mesh 1, material 2) -> 3 lookups, not 6.
    CHECK(boundsCalls == 3);

    // A scaled model moves the world bounds: a unit cube scaled 20x around z = -5 reaches the view from behind.
    std::vector<RenderItem> scaled{item(1, 1, ShaderKind::Pbr, 0.0f, 5.0f)};
    scaled[0].model = glm::scale(scaled[0].model, glm::vec3(20.0f));
    CHECK(BatchPlanner{}.plan(scaled, culling(frustum, localBounds)).batches.size() == 1);

    // Without a bounds function nothing is culled.
    const BatchPlan unknown = BatchPlanner{}.plan(items, culling(frustum, std::function<Aabb(MeshHandle)>{}));
    CHECK(unknown.instanceTransforms.size() == items.size());
    CHECK(unknown.culledItems == 0);
}

TEST_CASE("scene/batch_planner: empty input and non-finite transforms", "[scene][batch_planner]") {
    const BatchPlan empty = BatchPlanner{}.plan(std::vector<RenderItem>{});
    CHECK(empty.batches.empty());
    CHECK(empty.instanceTransforms.empty());

    std::vector<RenderItem> items{item(1, 1, ShaderKind::Pbr, 0.0f), item(1, 1, ShaderKind::Pbr, 1.0f)};
    items[0].model[3][1] = std::numeric_limits<float>::quiet_NaN();
    const BatchPlan plan = BatchPlanner{}.plan(items);
    REQUIRE(plan.batches.size() == 1);
    CHECK(plan.batches[0].instanceCount == 1);
    CHECK(tagOf(plan.instanceTransforms[0]) == 1.0f);
}

TEST_CASE("scene/batch_planner: handles compare and hash by value", "[scene][batch_planner]") {
    CHECK(MeshHandle{3} == MeshHandle{3});
    CHECK(MeshHandle{3} != MeshHandle{4});
    CHECK(MeshHandle{3} < MeshHandle{4});
    CHECK(MaterialHandle{9} > MaterialHandle{2});

    std::unordered_set<MeshHandle> meshes{MeshHandle{1}, MeshHandle{2}, MeshHandle{1}};
    CHECK(meshes.size() == 2);
    CHECK(meshes.contains(MeshHandle{2}));
    std::unordered_map<MaterialHandle, int> materials{{MaterialHandle{5}, 1}};
    CHECK(materials.at(MaterialHandle{5}) == 1);
    CHECK(std::hash<MeshHandle>{}(MeshHandle{7}) == std::hash<MeshHandle>{}(MeshHandle{7}));
}

namespace {

using rv::scene::DrawPass;
using rv::scene::PlanOptions;

/// Tags (x translations) of the instances in plan order.
std::vector<float> tagsOf(const BatchPlan& plan) {
    std::vector<float> tags;
    for (const glm::mat4& model : plan.instanceTransforms) {
        tags.push_back(tagOf(model));
    }
    return tags;
}

Aabb unitCube(MeshHandle /*mesh*/) {
    return Aabb::fromCenterHalfExtents(glm::vec3(0.0f), glm::vec3(0.5f));
}

} // namespace

TEST_CASE("scene/batch_planner: opaque, then masked, then blended items back to front", "[scene][batch_planner]") {
    // Materials 1 and 2 are opaque, 3 is masked, 4 and 5 are blended.
    int passCalls = 0;
    PlanOptions options;
    options.localBounds = unitCube;
    options.passOf = [&passCalls](MaterialHandle material) {
        ++passCalls;
        return material.value >= 4 ? DrawPass::Blend : (material.value == 3 ? DrawPass::Mask : DrawPass::Opaque);
    };
    // Item i sits at (tag, 0, z); the eye is at the origin.
    const std::vector<RenderItem> items{
        item(1, 4, ShaderKind::Pbr, 0.0f, -3.0f),   // blended, distance^2 9
        item(1, 1, ShaderKind::Pbr, 1.0f, -5.0f),   // opaque
        item(1, 4, ShaderKind::Pbr, 2.0f, -9.0f),   // blended, distance^2 85
        item(2, 3, ShaderKind::Pbr, 3.0f, -5.0f),   // masked
        item(1, 5, ShaderKind::Unlit, 4.0f, -6.0f), // blended, distance^2 52
        item(1, 2, ShaderKind::Pbr, 5.0f, -5.0f),   // opaque
        item(1, 4, ShaderKind::Pbr, 6.0f, -8.0f),   // blended, distance^2 100: the farthest
    };
    const BatchPlan plan = BatchPlanner{}.plan(items, options);
    checkContiguous(plan);
    CHECK(plan.culledItems == 0);
    // One passOf call per run of a material in submission order (no two neighbors share one here).
    CHECK(passCalls == 7);

    REQUIRE(plan.batches.size() == 6);
    const auto expect = [&plan](std::size_t index, DrawPass pass, ShaderKind shader, std::uint32_t material,
                                std::uint32_t count) {
        INFO("batch " << index);
        CHECK(plan.batches[index].pass == pass);
        CHECK(plan.batches[index].shader == shader);
        CHECK(plan.batches[index].material == MaterialHandle{material});
        CHECK(plan.batches[index].instanceCount == count);
    };
    expect(0, DrawPass::Opaque, ShaderKind::Pbr, 1, 1);
    expect(1, DrawPass::Opaque, ShaderKind::Pbr, 2, 1);
    expect(2, DrawPass::Mask, ShaderKind::Pbr, 3, 1);
    // Back to front: the two farthest blended items share material and mesh, so they form one batch drawn in order.
    expect(3, DrawPass::Blend, ShaderKind::Pbr, 4, 2);
    expect(4, DrawPass::Blend, ShaderKind::Unlit, 5, 1);
    expect(5, DrawPass::Blend, ShaderKind::Pbr, 4, 1);
    CHECK(tagsOf(plan) == std::vector<float>{1.0f, 5.0f, 3.0f, 6.0f, 2.0f, 4.0f, 0.0f});
}

TEST_CASE("scene/batch_planner: blended items sort by the center of their world bounds, ties in submission order",
          "[scene][batch_planner]") {
    PlanOptions options;
    options.eye = glm::vec3(0.0f, 0.0f, 0.0f);
    options.passOf = [](MaterialHandle) {
        return DrawPass::Blend;
    };
    // Mesh 1: a unit cube around its origin. Mesh 2: bounds centered 20 m behind its origin. Mesh 3: unknown bounds.
    options.localBounds = [](MeshHandle mesh) {
        if (mesh == MeshHandle{1}) {
            return unitCube(mesh);
        }
        if (mesh == MeshHandle{2}) {
            return Aabb::fromCenterHalfExtents(glm::vec3(0.0f, 0.0f, -20.0f), glm::vec3(0.5f));
        }
        return Aabb{};
    };

    // Mesh 2 at z = -2 is centered at z = -22: the farthest although its origin is the nearest.
    std::vector<RenderItem> items{item(1, 1, ShaderKind::Pbr, 0.0f, -10.0f), item(2, 2, ShaderKind::Pbr, 1.0f, -2.0f),
                                  item(3, 3, ShaderKind::Pbr, 2.0f, -15.0f)};
    CHECK(tagsOf(BatchPlanner{}.plan(items, options)) == std::vector<float>{1.0f, 2.0f, 0.0f});

    // Equal distances keep the submission order, whatever the keys.
    std::vector<RenderItem> ring{item(1, 7, ShaderKind::Pbr, 3.0f, 0.0f), item(1, 2, ShaderKind::Pbr, -3.0f, 0.0f),
                                 item(1, 7, ShaderKind::Pbr, 0.0f, -3.0f)};
    const BatchPlan tied = BatchPlanner{}.plan(ring, options);
    CHECK(tagsOf(tied) == std::vector<float>{3.0f, -3.0f, 0.0f});
    CHECK(tied.batches.size() == 3);

    // A non-finite eye counts as the origin instead of breaking the sort.
    options.eye = glm::vec3(std::numeric_limits<float>::quiet_NaN());
    CHECK(tagsOf(BatchPlanner{}.plan(ring, options)) == std::vector<float>{3.0f, -3.0f, 0.0f});
}

TEST_CASE("scene/batch_planner: culling applies to every pass", "[scene][batch_planner]") {
    rv::scene::Camera camera;
    camera.setPose(glm::vec3(0.0f), 0.0f, 0.0f); // looking down -Z
    PlanOptions options = culling(camera.frustum(), unitCube);
    options.passOf = [](MaterialHandle material) {
        return material.value == 2 ? DrawPass::Blend : DrawPass::Opaque;
    };
    const std::vector<RenderItem> items{
        item(1, 1, ShaderKind::Pbr, 0.0f, -5.0f), // opaque, visible
        item(1, 1, ShaderKind::Pbr, 0.0f, 5.0f),  // opaque, behind
        item(1, 2, ShaderKind::Pbr, 1.0f, -5.0f), // blended, visible
        item(1, 2, ShaderKind::Pbr, 1.0f, 5.0f),  // blended, behind
    };
    const BatchPlan plan = BatchPlanner{}.plan(items, options);
    CHECK(plan.culledItems == 2);
    REQUIRE(plan.batches.size() == 2);
    CHECK(plan.batches[0].pass == DrawPass::Opaque);
    CHECK(plan.batches[1].pass == DrawPass::Blend);
    CHECK(tagsOf(plan) == std::vector<float>{0.0f, 1.0f});
}

TEST_CASE("scene/batch_planner: planning into a kept plan reuses its storage", "[scene][batch_planner]") {
    rv::scene::Camera camera;
    camera.setPose(glm::vec3(0.0f), 0.0f, 0.0f); // looking down -Z
    PlanOptions options = culling(camera.frustum(), unitCube);
    options.passOf = [](MaterialHandle material) {
        return material.value == 9 ? DrawPass::Blend : DrawPass::Opaque;
    };
    // 1000 items in front of the camera over 3 materials and 2 meshes, one in 10 blended, one in 7 behind (culled).
    std::vector<RenderItem> items;
    for (std::uint32_t i = 0; i < 1000; ++i) {
        const std::uint32_t material = i % 10 == 0 ? 9u : 1u + i % 3;
        items.push_back(item(1 + i % 2, material, ShaderKind::Pbr, static_cast<float>(i % 40) - 20.0f,
                             i % 7 == 0 ? 5.0f : -2.0f - static_cast<float>(i % 23)));
    }

    BatchPlanner planner;
    BatchPlan kept;
    planner.plan(items, options, kept);
    const BatchPlan fresh = BatchPlanner{}.plan(items, options);
    REQUIRE(kept.instanceTransforms.size() == fresh.instanceTransforms.size());
    CHECK(kept.culledItems == fresh.culledItems);
    CHECK(kept.culledItems > 0);
    CHECK(kept.instanceTransforms == fresh.instanceTransforms);
    checkContiguous(kept);

    // Planning the same view again: same result, same storage (nothing reallocated).
    const glm::mat4* transforms = kept.instanceTransforms.data();
    const Batch* batches = kept.batches.data();
    planner.plan(items, options, kept);
    CHECK(kept.instanceTransforms.data() == transforms);
    CHECK(kept.batches.data() == batches);
    CHECK(kept.instanceTransforms == fresh.instanceTransforms);
    CHECK(kept.culledItems == fresh.culledItems);
    REQUIRE(kept.batches.size() == fresh.batches.size());
    for (std::size_t i = 0; i < kept.batches.size(); ++i) {
        CHECK(kept.batches[i].material == fresh.batches[i].material);
        CHECK(kept.batches[i].mesh == fresh.batches[i].mesh);
        CHECK(kept.batches[i].firstInstance == fresh.batches[i].firstInstance);
        CHECK(kept.batches[i].instanceCount == fresh.batches[i].instanceCount);
    }

    // A smaller view replaces the whole content (counters included) within the same storage.
    const std::vector<RenderItem> few{item(1, 1, ShaderKind::Pbr, 1.0f), item(1, 1, ShaderKind::Pbr, 2.0f)};
    planner.plan(few, options, kept);
    CHECK(kept.instanceTransforms.data() == transforms);
    CHECK(kept.culledItems == 0);
    REQUIRE(kept.batches.size() == 1);
    CHECK(tagsOf(kept) == std::vector<float>{1.0f, 2.0f});
}

TEST_CASE("scene/batch_planner: draw passes follow the alpha modes", "[scene][batch_planner]") {
    CHECK(rv::scene::drawPassOf(rv::assets::AlphaMode::Opaque) == DrawPass::Opaque);
    CHECK(rv::scene::drawPassOf(rv::assets::AlphaMode::Mask) == DrawPass::Mask);
    CHECK(rv::scene::drawPassOf(rv::assets::AlphaMode::Blend) == DrawPass::Blend);
    // Without passOf every item is opaque.
    const BatchPlan plan = BatchPlanner{}.plan(std::vector<RenderItem>{item(1, 1, ShaderKind::Pbr, 0.0f)});
    REQUIRE(plan.batches.size() == 1);
    CHECK(plan.batches[0].pass == DrawPass::Opaque);
}

TEST_CASE("scene/batch_planner: a shadow map plans the items that cast shadows, blended ones left out",
          "[scene][batch_planner]") {
    // Materials 1 and 2 are opaque, 3 is masked, 4 is blended (glass casts no shadow).
    PlanOptions options;
    options.localBounds = unitCube;
    options.passOf = [](MaterialHandle material) {
        return material.value == 4 ? DrawPass::Blend : (material.value == 3 ? DrawPass::Mask : DrawPass::Opaque);
    };
    std::vector<RenderItem> items{
        item(1, 1, ShaderKind::Pbr, 0.0f), // opaque caster
        item(1, 2, ShaderKind::Pbr, 1.0f), // opaque, casts no shadow (a rug)
        item(2, 3, ShaderKind::Pbr, 2.0f), // masked caster
        item(1, 4, ShaderKind::Pbr, 3.0f), // blended caster: left out all the same
        item(1, 1, ShaderKind::Unlit, 4.0f), // unlit surfaces cast like the others
    };
    items[1].castsShadow = false;

    // The view draws everything.
    const BatchPlan view = BatchPlanner{}.plan(items, options);
    CHECK(view.instanceTransforms.size() == 5);

    options.shadowCastersOnly = true;
    const BatchPlan shadow = BatchPlanner{}.plan(items, options);
    checkContiguous(shadow);
    // Left out, not culled: the culled count belongs to the frustum.
    CHECK(shadow.culledItems == 0);
    CHECK(tagsOf(shadow) == std::vector<float>{0.0f, 4.0f, 2.0f});
    REQUIRE(shadow.batches.size() == 3);
    CHECK(shadow.batches[0].pass == DrawPass::Opaque);
    CHECK(shadow.batches[1].pass == DrawPass::Opaque);
    CHECK(shadow.batches[1].shader == ShaderKind::Unlit);
    CHECK(shadow.batches[2].pass == DrawPass::Mask);

    // The frustum of the light culls as usual.
    rv::scene::Camera light;
    light.setPose(glm::vec3(100.0f, 0.0f, 0.0f), 0.0f, 0.0f); // looking away from every item
    options.frustum = light.frustum();
    const BatchPlan away = BatchPlanner{}.plan(items, options);
    CHECK(away.instanceTransforms.empty());
    CHECK(away.culledItems == 3);
}
