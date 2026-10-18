// The GPU-free half of the static instance groups (I3 step 8, ADR-0011 §3): the pre-sorted batches and cluster ranges
// of scene::InstanceClusters, the per-view culling of the clusters and the merged draws, against scene::BatchPlanner
// (every item a planner keeps is drawn, nothing is drawn twice), the attributes changed in place and the highlighted
// records; and the hover outline's geometry (ADR-0012 §3): its width, its reach and the pixels a box covers.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <rv/core/random.hpp>
#include <rv/scene/batch_planner.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/instance_clusters.hpp>

using namespace rv;
using Catch::Matchers::WithinAbs;

namespace {

// Three parts per cartridge (the probe's and the room's): the shell casts a shadow, its front and spine do not.
constexpr scene::MeshHandle kShellMesh{0};
constexpr scene::MeshHandle kFrontMesh{1};
constexpr scene::MeshHandle kSpineMesh{2};
constexpr scene::MaterialHandle kShellMaterial{0};
constexpr scene::MaterialHandle kFrontMaterial{1};
constexpr scene::MaterialHandle kSpineMaterial{2};
constexpr scene::MaterialHandle kMaskedMaterial{3};
constexpr scene::MaterialHandle kBlendMaterial{4};

core::Aabb meshBounds(scene::MeshHandle mesh) {
    switch (mesh.value) {
    case 0:
        return core::Aabb::fromCenterHalfExtents(glm::vec3(0.0f), glm::vec3(0.01f, 0.0625f, 0.0425f));
    case 1:
    case 2:
        return core::Aabb::fromCenterHalfExtents(glm::vec3(0.0f), glm::vec3(0.009f, 0.05f, 0.0f));
    default:
        return {}; // unknown
    }
}

scene::DrawPass passOf(scene::MaterialHandle material) {
    return material == kMaskedMaterial ? scene::DrawPass::Mask
           : material == kBlendMaterial ? scene::DrawPass::Blend
                                        : scene::DrawPass::Opaque;
}

scene::StaticGroupOptions options() {
    return scene::StaticGroupOptions{meshBounds, passOf};
}

/// A wall of `modules` shelf modules side by side, `levels` levels of `slots` spines each; a cluster per level of a
/// module, numbered module by module, bottom to top. Item order: module, level, slot; three instances per item.
struct Wall {
    int modules = 4;
    int levels = 7;
    int slots = 33;

    [[nodiscard]] std::uint32_t clusterCount() const { return static_cast<std::uint32_t>(modules * levels); }
    [[nodiscard]] std::size_t itemCount() const { return static_cast<std::size_t>(modules * levels * slots); }
};

glm::vec3 slotCenter(int module, int level, int slot) {
    return {static_cast<float>(module) * 0.9f + static_cast<float>(slot) * 0.024f, 0.2f + 0.3f * static_cast<float>(level),
            -4.0f};
}

std::vector<scene::StaticInstance> wallInstances(const Wall& wall) {
    std::vector<scene::StaticInstance> instances;
    std::uint32_t pick = 1;
    for (int module = 0; module < wall.modules; ++module) {
        for (int level = 0; level < wall.levels; ++level) {
            for (int slot = 0; slot < wall.slots; ++slot) {
                const glm::vec3 center = slotCenter(module, level, slot);
                const auto cluster = static_cast<std::uint32_t>(module * wall.levels + level);
                scene::InstanceAttributes attributes{pick, pick, 0, pick};
                ++pick;
                instances.push_back({kShellMesh, kShellMaterial, scene::ShaderKind::Pbr,
                                     glm::translate(glm::mat4(1.0f), center), true, attributes, cluster});
                instances.push_back({kFrontMesh, kFrontMaterial, scene::ShaderKind::Pbr,
                                     glm::translate(glm::mat4(1.0f), center + glm::vec3(0.0f, 0.0f, 0.043f)), false,
                                     attributes, cluster});
                instances.push_back({kSpineMesh, kSpineMaterial, scene::ShaderKind::Pbr,
                                     glm::translate(glm::mat4(1.0f), center + glm::vec3(0.0f, 0.0f, 0.0431f)), false,
                                     attributes, cluster});
            }
        }
    }
    return instances;
}

scene::InstanceClusters buildWall(const Wall& wall) {
    const std::vector<scene::StaticInstance> instances = wallInstances(wall);
    auto layout = scene::InstanceClusters::build(instances, wall.clusterCount(), options());
    REQUIRE(layout.has_value());
    return std::move(*layout);
}

/// The records a list of draws covers, each once (a record drawn twice fails the test).
std::set<std::uint32_t> drawnRecords(const scene::StaticDrawList& list) {
    std::set<std::uint32_t> records;
    for (const scene::StaticDraw& draw : list.draws) {
        for (std::uint32_t record = draw.firstInstance; record < draw.firstInstance + draw.instanceCount; ++record) {
            CHECK(records.insert(record).second);
        }
    }
    return records;
}

scene::Camera cameraAt(const glm::vec3& eye, float yawDeg, float pitchDeg, float fovDeg = 75.0f) {
    scene::Camera camera;
    scene::CameraLens lens;
    lens.horizontalFovDeg = fovDeg;
    camera.setLens(lens);
    camera.setPose(eye, glm::radians(yawDeg), glm::radians(pitchDeg));
    return camera;
}

} // namespace

TEST_CASE("scene/instance clusters: batches by key, instances by cluster, ranges per cluster", "[scene][instance_clusters]") {
    const Wall wall{3, 4, 5};
    const std::vector<scene::StaticInstance> instances = wallInstances(wall);
    const scene::InstanceClusters layout = buildWall(wall);
    REQUIRE(layout.instances().size() == instances.size());
    CHECK(layout.clusterCount() == 12);
    CHECK(layout.occupiedClusters() == 12);

    // One batch per part, in key order (pass, shader, material, mesh): shells, fronts, spines.
    REQUIRE(layout.batches().size() == 3);
    const std::size_t perBatch = wall.itemCount();
    for (std::uint32_t b = 0; b < 3; ++b) {
        const scene::StaticBatch& batch = layout.batches()[b];
        INFO("batch " << b);
        CHECK(batch.material.value == b);
        CHECK(batch.mesh.value == b);
        CHECK(batch.castsShadow == (b == 0));
        CHECK(batch.firstInstance == b * perBatch);
        CHECK(batch.instanceCount == perBatch);
        // A range per cluster, ascending, back to back, each the cluster's 5 items in source order.
        REQUIRE(batch.rangeCount == 12);
        std::uint32_t next = batch.firstInstance;
        for (std::uint32_t r = 0; r < batch.rangeCount; ++r) {
            const scene::ClusterRange& range = layout.ranges()[batch.firstRange + r];
            CHECK(range.cluster == r);
            CHECK(range.firstInstance == next);
            CHECK(range.instanceCount == 5);
            next += range.instanceCount;
        }
    }
    // Every source instance's record holds its matrix and attributes, and lies in its batch and cluster.
    std::set<std::uint32_t> positions;
    for (std::size_t source = 0; source < instances.size(); ++source) {
        const std::uint32_t position = layout.positionOf(source);
        CHECK(positions.insert(position).second);
        CHECK(layout.instances()[position].model == instances[source].model);
        CHECK(layout.instances()[position].attributes == instances[source].attributes);
        const std::uint32_t batch = layout.batchOf(position);
        CHECK(batch == source % 3);
    }
    // Source order within a cluster: item order.
    CHECK(layout.positionOf(0) == 0);
    CHECK(layout.positionOf(3) == 1);
    CHECK(layout.positionOf(1) == perBatch);
    CHECK_THROWS(layout.positionOf(instances.size()));
    CHECK_THROWS(layout.batchOf(static_cast<std::uint32_t>(instances.size())));
}

TEST_CASE("scene/instance clusters: opaque before alpha-masked, casters and non-casters apart", "[scene][instance_clusters]") {
    const glm::mat4 at = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, -3.0f));
    const std::vector<scene::StaticInstance> instances{
        {kShellMesh, kMaskedMaterial, scene::ShaderKind::Pbr, at, true, {}, 1},
        {kShellMesh, kShellMaterial, scene::ShaderKind::Pbr, at, false, {}, 0},
        {kShellMesh, kShellMaterial, scene::ShaderKind::Pbr, at, true, {}, 1},
        {kShellMesh, kShellMaterial, scene::ShaderKind::Unlit, at, true, {}, 0},
        {kShellMesh, kShellMaterial, scene::ShaderKind::Pbr, at, true, {}, 0},
    };
    auto layout = scene::InstanceClusters::build(instances, 2, options());
    REQUIRE(layout.has_value());
    REQUIRE(layout->batches().size() == 4);
    const auto batch = [&](std::size_t index) { return layout->batches()[index]; };
    // Opaque PBR casters (sources 4, 2: cluster 0 before 1), opaque PBR non-casters, opaque unlit, masked.
    CHECK(batch(0).pass == scene::DrawPass::Opaque);
    CHECK(batch(0).shader == scene::ShaderKind::Pbr);
    CHECK(batch(0).castsShadow);
    CHECK(batch(0).instanceCount == 2);
    CHECK(layout->positionOf(4) == 0);
    CHECK(layout->positionOf(2) == 1);
    CHECK_FALSE(batch(1).castsShadow);
    CHECK(batch(1).shader == scene::ShaderKind::Pbr);
    CHECK(batch(2).shader == scene::ShaderKind::Unlit);
    CHECK(batch(3).pass == scene::DrawPass::Mask);
    CHECK(layout->positionOf(0) == 4);

    // Shadow plans draw the casters only: batch 1 is left out.
    scene::StaticDrawList shadow;
    layout->plan(nullptr, true, shadow);
    std::set<std::uint32_t> batches;
    for (const scene::StaticDraw& draw : shadow.draws) {
        batches.insert(draw.batch);
    }
    CHECK(batches == std::set<std::uint32_t>{0, 2, 3});
    CHECK(shadow.instances == 4);
}

TEST_CASE("scene/instance clusters: malformed instances are refused", "[scene][instance_clusters]") {
    scene::StaticInstance good{kShellMesh, kShellMaterial, scene::ShaderKind::Pbr, glm::mat4(1.0f), true, {}, 0};
    SECTION("a non-finite matrix") {
        scene::StaticInstance bad = good;
        bad.model[3][1] = std::numeric_limits<float>::quiet_NaN();
        const std::vector<scene::StaticInstance> instances{good, bad};
        const auto layout = scene::InstanceClusters::build(instances, 1, options());
        REQUIRE_FALSE(layout.has_value());
        CHECK(layout.error().code == ErrorCode::InvalidArgument);
    }
    SECTION("an alpha-blended material: blended items are drawn back to front, which no pre-sorted group does") {
        scene::StaticInstance bad = good;
        bad.material = kBlendMaterial;
        const std::vector<scene::StaticInstance> instances{bad};
        const auto layout = scene::InstanceClusters::build(instances, 1, options());
        REQUIRE_FALSE(layout.has_value());
        CHECK(layout.error().code == ErrorCode::InvalidArgument);
    }
    SECTION("a cluster beyond the count") {
        scene::StaticInstance bad = good;
        bad.cluster = 3;
        const std::vector<scene::StaticInstance> instances{good, bad};
        const auto layout = scene::InstanceClusters::build(instances, 3, options());
        REQUIRE_FALSE(layout.has_value());
        CHECK(layout.error().code == ErrorCode::OutOfRange);
    }
    SECTION("nothing: an empty layout draws nothing") {
        const auto layout = scene::InstanceClusters::build({}, 4, options());
        REQUIRE(layout.has_value());
        CHECK(layout->instances().empty());
        CHECK(layout->occupiedClusters() == 0);
        scene::StaticDrawList draws;
        layout->plan(nullptr, false, draws);
        CHECK(draws.draws.empty());
        CHECK(draws.clusters == 0);
    }
}

TEST_CASE("scene/instance clusters: cluster and caster bounds", "[scene][instance_clusters]") {
    const Wall wall{2, 3, 4};
    const scene::InstanceClusters layout = buildWall(wall);
    for (int module = 0; module < wall.modules; ++module) {
        for (int level = 0; level < wall.levels; ++level) {
            const auto cluster = static_cast<std::uint32_t>(module * wall.levels + level);
            INFO("cluster " << cluster);
            const core::Aabb& bounds = layout.clusterBounds(cluster);
            CHECK(layout.clusterCullable(cluster));
            // From the first shell's left side to the last's right, the shells' back to the labels' face.
            const glm::vec3 first = slotCenter(module, level, 0);
            const glm::vec3 last = slotCenter(module, level, wall.slots - 1);
            CHECK_THAT(bounds.min.x, WithinAbs(first.x - 0.01f, 1e-5));
            CHECK_THAT(bounds.max.x, WithinAbs(last.x + 0.01f, 1e-5));
            CHECK_THAT(bounds.min.y, WithinAbs(first.y - 0.0625f, 1e-5));
            CHECK_THAT(bounds.max.y, WithinAbs(first.y + 0.0625f, 1e-5));
            CHECK_THAT(bounds.min.z, WithinAbs(first.z - 0.0425f, 1e-5));
            CHECK_THAT(bounds.max.z, WithinAbs(first.z + 0.0431f, 1e-5));
        }
    }
    // The casters are the shells: their depth, not the labels'.
    CHECK_THAT(layout.casterBounds().max.z, WithinAbs(-4.0f + 0.0425f, 1e-5));
    CHECK_THAT(layout.casterBounds().min.x, WithinAbs(-0.01f, 1e-5));
    // A record's bounds: its mesh's under its matrix.
    CHECK(layout.instanceBounds(layout.positionOf(0)) ==
          meshBounds(kShellMesh).transformed(glm::translate(glm::mat4(1.0f), slotCenter(0, 0, 0))));

    // A cluster holding an instance of unknown bounds is drawn in every view.
    std::vector<scene::StaticInstance> instances = wallInstances(wall);
    instances[0].mesh = scene::MeshHandle{9};
    auto unknown = scene::InstanceClusters::build(instances, wall.clusterCount(), options());
    REQUIRE(unknown.has_value());
    CHECK_FALSE(unknown->clusterCullable(0));
    CHECK(unknown->clusterCullable(1));
    CHECK(unknown->instanceBounds(unknown->positionOf(0)).isEmpty());
    const scene::Frustum away = cameraAt(glm::vec3(0.0f, 1.0f, 0.0f), 180.0f, 0.0f).frustum();
    scene::StaticDrawList draws;
    unknown->plan(&away, false, draws);
    CHECK(draws.culledClusters == wall.clusterCount() - 1);
    CHECK_FALSE(draws.draws.empty());
}

TEST_CASE("scene/instance clusters: every view draws the visible clusters' ranges, merged, never an item twice",
          "[scene][instance_clusters]") {
    const Wall wall;
    const std::vector<scene::StaticInstance> instances = wallInstances(wall);
    const scene::InstanceClusters layout = buildWall(wall);
    scene::StaticDrawList draws;

    SECTION("without a frustum: one draw per batch") {
        layout.plan(nullptr, false, draws);
        REQUIRE(draws.draws.size() == 3);
        for (std::uint32_t b = 0; b < 3; ++b) {
            CHECK(draws.draws[b] == scene::StaticDraw{b, layout.batches()[b].firstInstance,
                                                      layout.batches()[b].instanceCount});
        }
        CHECK(draws.instances == instances.size());
        CHECK(draws.culledClusters == 0);
        CHECK(draws.clusters == wall.clusterCount());
    }
    SECTION("a view of the whole wall: one draw per batch") {
        const scene::Frustum frustum = cameraAt(glm::vec3(1.8f, 1.2f, 2.0f), 0.0f, 0.0f).frustum();
        layout.plan(&frustum, false, draws);
        CHECK(draws.draws.size() == 3);
        CHECK(draws.culledClusters == 0);
        CHECK(draws.instances == instances.size());
    }
    SECTION("a view of the left module's lower levels") {
        // Close to the wall, looking down at the first module: its lower levels are in view, the others are not.
        const scene::Frustum frustum = cameraAt(glm::vec3(0.2f, 0.6f, -3.2f), -10.0f, -30.0f, 50.0f).frustum();
        layout.plan(&frustum, false, draws);
        CHECK(draws.culledClusters > 0);
        CHECK(draws.culledClusters < wall.clusterCount());
        // The draws cover exactly the records of the clusters whose bounds the frustum meets, in as many draws per
        // batch as runs of consecutive visible clusters.
        std::set<std::uint32_t> expected;
        std::size_t runs = 0;
        for (const scene::StaticBatch& batch : layout.batches()) {
            bool previous = false;
            for (std::uint32_t r = 0; r < batch.rangeCount; ++r) {
                const scene::ClusterRange& range = layout.ranges()[batch.firstRange + r];
                const bool visible = frustum.intersects(layout.clusterBounds(range.cluster));
                if (visible) {
                    for (std::uint32_t i = 0; i < range.instanceCount; ++i) {
                        expected.insert(range.firstInstance + i);
                    }
                    runs += previous ? 0 : 1;
                }
                previous = visible;
            }
        }
        CHECK(drawnRecords(draws) == expected);
        CHECK(draws.draws.size() == runs);
        CHECK(draws.instances == expected.size());
    }
    SECTION("a view away from the wall: nothing") {
        const scene::Frustum frustum = cameraAt(glm::vec3(1.8f, 1.2f, 2.0f), 180.0f, 0.0f).frustum();
        layout.plan(&frustum, false, draws);
        CHECK(draws.draws.empty());
        CHECK(draws.culledClusters == wall.clusterCount());
        CHECK(draws.instances == 0);
    }
}

TEST_CASE("scene/instance clusters: the static draws hold every item the batch planner keeps, from any view",
          "[scene][instance_clusters]") {
    // 1,000 seeded views around the wall: the clusters are culled as a whole, so a view draws every record whose item
    // the per-item planner keeps (a superset: the rest of a visible cluster), and of a shadow view only the casters.
    const Wall wall;
    const std::vector<scene::StaticInstance> instances = wallInstances(wall);
    const scene::InstanceClusters layout = buildWall(wall);
    std::vector<scene::RenderItem> items;
    for (const scene::StaticInstance& instance : instances) {
        items.push_back({instance.mesh, instance.material, instance.shader, instance.model, instance.castsShadow,
                         instance.attributes});
    }
    scene::BatchPlanner planner;
    scene::BatchPlan plan;
    scene::StaticDrawList draws;
    core::Random random(0x5747A71C);
    std::size_t culledSomewhere = 0;
    for (int view = 0; view < 1000; ++view) {
        const glm::vec3 eye(static_cast<float>(random.unit()) * 4.0f - 0.5f, static_cast<float>(random.unit()) * 2.5f,
                            static_cast<float>(random.unit()) * 4.0f - 3.9f);
        const scene::Camera camera = cameraAt(eye, static_cast<float>(random.unit()) * 360.0f - 180.0f,
                                              static_cast<float>(random.unit()) * 120.0f - 60.0f,
                                              30.0f + static_cast<float>(random.unit()) * 60.0f);
        const bool shadow = view % 4 == 3;
        scene::PlanOptions planOptions;
        planOptions.frustum = camera.frustum();
        planOptions.localBounds = meshBounds;
        planOptions.passOf = passOf;
        planOptions.shadowCastersOnly = shadow;
        planner.plan(items, planOptions, plan);
        const scene::Frustum frustum = camera.frustum();
        layout.plan(&frustum, shadow, draws);
        const std::set<std::uint32_t> drawn = drawnRecords(draws);
        // The planner's records by source: the item from the pick id, the part from the batch's mesh.
        std::set<std::uint32_t> kept;
        for (const scene::Batch& batch : plan.batches) {
            for (std::uint32_t i = batch.firstInstance; i < batch.firstInstance + batch.instanceCount; ++i) {
                const std::size_t source = 3 * (plan.instances[i].attributes.pickId - 1) + batch.mesh.value;
                REQUIRE(source < instances.size());
                CHECK(instances[source].model == plan.instances[i].model);
                kept.insert(layout.positionOf(source));
            }
        }
        INFO("view " << view);
        CHECK(std::includes(drawn.begin(), drawn.end(), kept.begin(), kept.end()));
        for (const std::uint32_t record : drawn) {
            const scene::StaticBatch& batch = layout.batches()[layout.batchOf(record)];
            CHECK((!shadow || batch.castsShadow));
        }
        culledSomewhere += draws.culledClusters > 0 ? 1 : 0;
    }
    CHECK(culledSomewhere > 100);
}

TEST_CASE("scene/instance clusters: attributes change in place, the highlighted records listed", "[scene][instance_clusters]") {
    const Wall wall{2, 2, 3};
    std::vector<scene::StaticInstance> instances = wallInstances(wall);
    instances[4].attributes.flags = scene::instance_flags::kHighlighted;
    auto built = scene::InstanceClusters::build(instances, wall.clusterCount(), options());
    REQUIRE(built.has_value());
    scene::InstanceClusters& layout = *built;
    REQUIRE(layout.highlighted().size() == 1);
    CHECK(layout.highlighted()[0] == layout.positionOf(4));

    // Equal attributes write nothing; a change does, and the record holds it.
    CHECK_FALSE(layout.setAttributes(0, instances[0].attributes));
    scene::InstanceAttributes lifted = instances[0].attributes;
    lifted.flags = scene::instance_flags::kHidden;
    CHECK(layout.setAttributes(0, lifted));
    CHECK(layout.instances()[layout.positionOf(0)].attributes == lifted);
    CHECK(layout.highlighted().size() == 1);

    // Highlights keep their records in ascending order, also when added out of order.
    scene::InstanceAttributes hovered = instances[9].attributes;
    hovered.flags = scene::instance_flags::kHighlighted;
    CHECK(layout.setAttributes(9, hovered));
    hovered = instances[1].attributes;
    hovered.flags = scene::instance_flags::kHighlighted | scene::instance_flags::kUnavailable;
    CHECK(layout.setAttributes(1, hovered));
    std::vector<std::uint32_t> expected{layout.positionOf(4), layout.positionOf(9), layout.positionOf(1)};
    std::sort(expected.begin(), expected.end());
    CHECK(std::vector<std::uint32_t>(layout.highlighted().begin(), layout.highlighted().end()) == expected);
    // A layer change keeps the highlight; clearing the flag removes it.
    hovered.frontLayer = 77;
    CHECK(layout.setAttributes(1, hovered));
    CHECK(layout.highlighted().size() == 3);
    CHECK(layout.setAttributes(4, instances[4].attributes) == false);
    scene::InstanceAttributes plain = instances[4].attributes;
    plain.flags = 0;
    CHECK(layout.setAttributes(4, plain));
    CHECK(layout.highlighted().size() == 2);
    CHECK(std::find(layout.highlighted().begin(), layout.highlighted().end(), layout.positionOf(4)) ==
          layout.highlighted().end());
    // Attributes never move a record or change a batch.
    CHECK(layout.batches().size() == 3);
    CHECK_THROWS(layout.setAttributes(instances.size(), plain));
}

TEST_CASE("scene/outline: the width scales with the output's height, the reach and the disc", "[scene][outline]") {
    CHECK(scene::outlineWidthPixels(1080) == 2.0f);
    CHECK(scene::outlineWidthPixels(2160) == 4.0f);
    CHECK_THAT(scene::outlineWidthPixels(720), WithinAbs(4.0f / 3.0f, 1e-6));
    CHECK(scene::outlineWidthPixels(0) == 0.0f);
    CHECK(scene::outlineWidthPixels(1080, 3.0f) == 3.0f);
    CHECK(scene::outlineWidthPixels(1080, -1.0f) == 0.0f);
    CHECK(scene::outlineWidthPixels(1080, std::numeric_limits<float>::infinity()) == 0.0f);

    CHECK(scene::outlineReach(2.0f) == 2);
    CHECK(scene::outlineReach(4.0f / 3.0f) == 1);
    CHECK(scene::outlineReach(0.5f) == 0);
    CHECK(scene::outlineReach(std::numeric_limits<float>::quiet_NaN()) == 0);
    CHECK(scene::outlineReach(50.0f) == scene::kOutlineMaxReach);

    // At 1080p: 2 px straight out, the diagonal neighbor (1.41) but not the knight's move (2.24).
    CHECK(scene::outlineCovers(2, 0, 2.0f));
    CHECK(scene::outlineCovers(0, -2, 2.0f));
    CHECK(scene::outlineCovers(1, 1, 2.0f));
    CHECK_FALSE(scene::outlineCovers(2, 1, 2.0f));
    CHECK_FALSE(scene::outlineCovers(3, 0, 2.0f));
    // At 720p (1.33 px): the four neighbors only, so an edge gets one pixel.
    CHECK(scene::outlineCovers(1, 0, 4.0f / 3.0f));
    CHECK_FALSE(scene::outlineCovers(1, 1, 4.0f / 3.0f));
    // The colour is the theme's warm white.
    CHECK(scene::kOutlineColorSrgb == std::array<std::uint8_t, 3>{0xF4, 0xEF, 0xE6});
}

TEST_CASE("scene/outline: the pixels a box covers on screen", "[scene][outline]") {
    scene::Camera camera = cameraAt(glm::vec3(0.0f, 0.0f, 2.0f), 0.0f, 0.0f, 90.0f);
    camera.setAspect(1.0f);
    const glm::mat4 viewProjection = camera.viewProjection();
    const glm::ivec2 size(1000, 1000);
    // A 0.2 m box 2 m ahead at the center: tan(45 degrees) = 1, so it spans 0.1 / 2 of the half-width each way.
    const core::Aabb centered = core::Aabb::fromCenterHalfExtents(glm::vec3(0.0f), glm::vec3(0.1f, 0.1f, 0.0f));
    const auto rect = scene::screenRect(centered, viewProjection, size, 0);
    REQUIRE(rect.has_value());
    // Pixels 475 to 525 each way, the bounds rounded outwards (by one when the projection lands a hair inside).
    CHECK(rect->x >= 474);
    CHECK(rect->x <= 475);
    CHECK(rect->x + rect->z >= 525);
    CHECK(rect->x + rect->z <= 526);
    CHECK(rect->y == rect->x);
    CHECK(rect->w == rect->z);
    const auto grown = scene::screenRect(centered, viewProjection, size, 3);
    REQUIRE(grown.has_value());
    CHECK(*grown == glm::ivec4(rect->x - 3, rect->y - 3, rect->z + 6, rect->w + 6));
    // Clipped to the output.
    const core::Aabb edge = core::Aabb::fromCenterHalfExtents(glm::vec3(1.95f, 0.0f, 0.0f), glm::vec3(0.1f));
    const auto clipped = scene::screenRect(edge, viewProjection, size, 4);
    REQUIRE(clipped.has_value());
    CHECK(clipped->x + clipped->z == 1000);
    // Outside the output: nothing; behind the eye or reaching behind it: everything.
    const core::Aabb outside = core::Aabb::fromCenterHalfExtents(glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.1f));
    CHECK_FALSE(scene::screenRect(outside, viewProjection, size, 2).has_value());
    const core::Aabb around = core::Aabb::fromCenterHalfExtents(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.5f));
    CHECK(scene::screenRect(around, viewProjection, size, 0) == glm::ivec4(0, 0, 1000, 1000));
    CHECK_FALSE(scene::screenRect(core::Aabb{}, viewProjection, size, 0).has_value());

    CHECK(scene::unionRect(std::nullopt, std::nullopt) == std::nullopt);
    CHECK(scene::unionRect(glm::ivec4(1, 2, 3, 4), std::nullopt) == glm::ivec4(1, 2, 3, 4));
    CHECK(scene::unionRect(glm::ivec4(1, 2, 3, 4), glm::ivec4(10, 0, 2, 2)) == glm::ivec4(1, 0, 11, 6));
}

TEST_CASE("scene/instance clusters: 2016 cartridges planned for a view and its shadow map",
          "[.bench][scene][instance_clusters]") {
    // Release: build/msvc-x64-release/bin/rv_tests.exe "[instance_clusters][bench]" (the roadmap's 0.3 ms of
    // collection planning per frame, CPU section collectionPlanning, measured in the room by the GPU smoke test).
    const Wall wall{9, 7, 32}; // 2,016 cartridges, 63 clusters, 6,048 instances
    const scene::InstanceClusters layout = buildWall(wall);
    scene::StaticDrawList draws;
    scene::StaticDrawList shadowDraws;
    const scene::Frustum camera = cameraAt(glm::vec3(2.0f, 1.2f, 1.0f), 10.0f, -5.0f).frustum();
    const scene::Frustum light = cameraAt(glm::vec3(4.0f, 3.0f, 2.0f), -20.0f, -30.0f, 100.0f).frustum();
    std::vector<double> microseconds;
    for (int run = 0; run < 2000; ++run) {
        const auto start = std::chrono::steady_clock::now();
        layout.plan(&camera, false, draws);
        layout.plan(&light, true, shadowDraws);
        microseconds.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                                   .count());
    }
    std::sort(microseconds.begin(), microseconds.end());
    const double median = microseconds[microseconds.size() / 2];
    WARN("2,016 cartridges, 63 clusters, a view and its shadow map: median " << median << " us, p99 "
                                                                              << microseconds[microseconds.size() * 99 / 100]
                                                                              << " us");
    CHECK(median < 300.0);
}
