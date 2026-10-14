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
    const BatchPlan plan = BatchPlanner{}.plan(items, frustum, localBounds);
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
    CHECK(BatchPlanner{}.plan(scaled, frustum, localBounds).batches.size() == 1);

    // Without a bounds function nothing is culled.
    CHECK(BatchPlanner{}.plan(items, frustum, std::function<Aabb(MeshHandle)>{}).instanceTransforms.size() ==
          items.size());
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
