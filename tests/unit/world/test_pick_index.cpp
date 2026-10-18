// I3 step 7: picking against the collection's pick targets (world::PickIndex) and the hover that follows the ray
// (world::HoverTracker), ADR-0012 §2, on hand-built shelves of step 1b's figures (synthetic_shelves.hpp).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/math.hpp>
#include <rv/core/random.hpp>
#include <rv/world/pick_index.hpp>

#include "synthetic_shelves.hpp"

using Catch::Approx;
using rv::core::Aabb;
using rv::core::Ray;
using rv::test::ShelfModuleSpec;
using rv::test::SyntheticItem;
using rv::test::SyntheticShelves;
using rv::world::HoverSample;
using rv::world::HoverTracker;
using rv::world::kNoPick;
using rv::world::PickId;
using rv::world::PickIndex;
using rv::world::PickTarget;

namespace {

PickIndex indexOf(const SyntheticShelves& shelves) {
    auto index = PickIndex::build(shelves.targets, shelves.occluders);
    REQUIRE(index.has_value());
    return std::move(*index);
}

SyntheticShelves oneModule() {
    const std::vector<ShelfModuleSpec> modules(1);
    return rv::test::buildShelves(modules);
}

/// The eye of a standing walker (1.65 m) `distance` in front of the module's front (z = 0.12), at x.
glm::vec3 eyeInFront(float distance, float x = 0.0f) {
    return {x, 1.65f, 0.5f * rv::test::shelf::kDepth + distance};
}

/// A point on `item`'s visible face, `dx` to its right and `dy` up from the face's middle, meters.
glm::vec3 onFace(const SyntheticItem& item, float dx, float dy) {
    return item.faceCenter + dx * item.right + dy * item.up;
}

/// Where `ray` first enters one of `boxes` before `distance`, if it does.
std::optional<float> firstBlocker(const std::vector<Aabb>& boxes, const Ray& ray, float distance) {
    std::optional<float> nearest;
    for (const Aabb& box : boxes) {
        if (const auto t = box.intersectRay(ray, distance); t && *t < distance && (!nearest || *t < *nearest)) {
            nearest = t;
        }
    }
    return nearest;
}

/// Feeds `tracker` the sample of `ray` `frames` times; the hovered item after the last.
PickId hold(HoverTracker& tracker, const PickIndex& index, const Ray& ray, int frames) {
    for (int frame = 0; frame < frames; ++frame) {
        tracker.update(index.sample(ray, tracker.hovered()));
    }
    return tracker.hovered();
}

} // namespace

TEST_CASE("world/pick_index: a target's visible face is inflated by 1 mm, its depth kept", "[world][pick_index]") {
    const Aabb item = Aabb::fromCenterHalfExtents({0.0f, 0.0f, 0.0f}, {0.01f, 0.0625f, 0.0425f});
    const Aabb target = rv::world::faceTargetBox(item);
    CHECK(target.size().x == Approx(0.022f));
    CHECK(target.size().y == Approx(0.127f));
    CHECK(target.size().z == Approx(0.085f));
    CHECK(rv::world::faceTargetBox(Aabb{}).isEmpty());
    CHECK(rv::world::kMaxPickDistance == 2.5f);
    CHECK(rv::world::kHysteresisMargin == 0.002f);
    CHECK(rv::world::kHoverChangeFrames == 3);
}

TEST_CASE("world/pick_index: malformed targets and occluders are refused", "[world][pick_index]") {
    const Aabb box = Aabb::fromCenterHalfExtents({0.0f, 1.0f, 0.0f}, {0.01f, 0.06f, 0.04f});
    const auto refused = [](std::vector<PickTarget> targets, std::vector<Aabb> occluders = {}) {
        const auto index = PickIndex::build(targets, occluders);
        REQUIRE_FALSE(index.has_value());
        CHECK(index.error().code == rv::ErrorCode::InvalidArgument);
        return index.error();
    };
    CHECK(refused({PickTarget{kNoPick, 0, glm::mat4(1.0f), box}}).message.find("no id") != std::string::npos);
    CHECK(refused({PickTarget{7, 0, glm::mat4(1.0f), box}, PickTarget{7, 1, glm::mat4(1.0f), box}}).context ==
          "target 7");
    refused({PickTarget{1, 0, glm::mat4(1.0f), Aabb{}}});
    refused({PickTarget{1, 0, glm::mat4(1.0f), Aabb::fromMinMax({0.0f, 0.0f, 0.0f},
                                                               {std::numeric_limits<float>::infinity(), 1.0f, 1.0f})}});
    refused({PickTarget{1, 0, glm::mat4(0.0f), box}});
    glm::mat4 projective(1.0f);
    projective[2][3] = -1.0f;
    refused({PickTarget{1, 0, projective, box}});
    glm::mat4 notFinite(1.0f);
    notFinite[3][0] = std::numeric_limits<float>::quiet_NaN();
    refused({PickTarget{1, 0, notFinite, box}});
    CHECK(refused({PickTarget{1, 0, glm::mat4(1.0f), box}}, {box, Aabb{}}).context == "occluder 1");

    const auto empty = PickIndex::build({}, {});
    REQUIRE(empty.has_value());
    CHECK(empty->targetCount() == 0);
    CHECK(empty->pick(Ray{{0.0f, 1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}}).id == kNoPick);
}

TEST_CASE("world/pick_index: the index keeps its targets by id and in clusters", "[world][pick_index]") {
    const std::vector<ShelfModuleSpec> modules(2);
    const SyntheticShelves shelves = rv::test::buildShelves(modules);
    const PickIndex index = indexOf(shelves);
    CHECK(index.targetCount() == 2 * 7 * 33);
    CHECK(index.clusterCount() == 2 * 7);
    CHECK(index.occluderCount() == shelves.occluders.size());
    for (const PickTarget& target : shelves.targets) {
        REQUIRE(index.contains(target.id));
        REQUIRE(index.target(target.id) != nullptr);
        CHECK(index.target(target.id)->box == target.box);
        CHECK(index.target(target.id)->cluster == target.cluster);
    }
    CHECK_FALSE(index.contains(kNoPick));
    CHECK_FALSE(index.contains(100000));
    CHECK(index.target(100000) == nullptr);
}

TEST_CASE("world/pick_index: at 1.5 m the ray picks the spine it crosses at a 2.4 cm pitch", "[world][pick_index]") {
    const SyntheticShelves shelves = oneModule();
    const PickIndex index = indexOf(shelves);
    const glm::vec3 eye = eyeInFront(1.5f);
    std::size_t rays = 0;
    // Every spine of every level, at points spread over its own face (2.0 x 12.5 cm, kept 0.5 mm inside its edges).
    for (const SyntheticItem& item : shelves.items) {
        CAPTURE(item.id, item.level, item.slot);
        for (const float dx : {-0.0095f, -0.005f, 0.0f, 0.005f, 0.0095f}) {
            for (const float dy : {-0.06f, -0.03f, 0.0f, 0.03f, 0.06f}) {
                const rv::world::PickHit hit = index.pick(rv::test::rayTowards(eye, onFace(item, dx, dy)));
                CHECK(hit.id == item.id);
                CHECK(hit.distance == Approx(glm::length(onFace(item, dx, dy) - eye)).margin(1e-4));
                ++rays;
            }
        }
    }
    CHECK(rays == 7 * 33 * 25);

    // Across a level in 1 mm steps: over a face its spine, in a 4 mm gap one of its two neighbours, never another.
    const int level = 4;
    const SyntheticItem& first = *shelves.at(0, level, 0);
    const SyntheticItem& last = *shelves.at(0, level, 32);
    for (float x = first.faceCenter.x - 0.012f; x <= last.faceCenter.x + 0.012f; x += 0.001f) {
        const glm::vec3 point(x, first.faceCenter.y, first.faceCenter.z);
        const PickId id = index.pick(rv::test::rayTowards(eye, point)).id;
        const float slotPosition = (x - first.faceCenter.x) / rv::test::shelf::kSpinePitch;
        const int nearestSlot = static_cast<int>(std::lround(slotPosition));
        const float fromMiddle = std::fabs(x - (first.faceCenter.x + rv::test::shelf::kSpinePitch *
                                                                          static_cast<float>(nearestSlot)));
        CAPTURE(x, id, nearestSlot);
        if (fromMiddle < 0.0095f && nearestSlot >= 0 && nearestSlot <= 32) {
            CHECK(id == shelves.at(0, level, nearestSlot)->id);
        } else if (id != kNoPick) {
            const SyntheticItem& picked = shelves.item(id);
            CHECK(picked.level == level);
            CHECK(std::abs(static_cast<float>(picked.slot) - slotPosition) < 1.0f);
        }
    }
}

TEST_CASE("world/pick_index: an item behind a board is never picked", "[world][pick_index]") {
    const SyntheticShelves shelves = oneModule();
    const PickIndex index = indexOf(shelves);
    std::size_t blocked = 0;
    std::size_t seen = 0;
    // Eyes above, level with and below the levels, near and far, aiming at points of every item's face.
    for (const float height : {0.3f, 0.9f, 1.2f, 1.65f, 2.1f}) {
        for (const float distance : {0.15f, 0.4f, 0.9f, 1.6f}) {
            for (const float x : {-0.5f, 0.0f, 0.35f}) {
                const glm::vec3 eye(x, height, 0.12f + distance);
                for (const SyntheticItem& item : shelves.items) {
                    for (const float dy : {-0.06f, 0.0f, 0.06f}) {
                        const glm::vec3 point = onFace(item, 0.0f, dy);
                        const Ray ray = rv::test::rayTowards(eye, point);
                        const float reach = glm::length(point - eye);
                        const PickId id = index.pick(ray).id;
                        if (firstBlocker(shelves.occluders, ray, reach - 1e-4f)) {
                            ++blocked;
                            CAPTURE(height, distance, x, item.id, dy);
                            CHECK(id != item.id);
                        } else if (reach <= rv::world::kMaxPickDistance) {
                            ++seen;
                        }
                    }
                }
            }
        }
    }
    // Both kinds of rays are plentiful: the test proves something either way.
    CHECK(blocked > 3000);
    CHECK(seen > 3000);

    // Straight through a board: from under level 3's board up to an item of level 3, and down through the top board.
    const SyntheticItem& item = *shelves.at(0, 3, 16);
    const glm::vec3 below(item.faceCenter.x, item.faceCenter.y - 0.3f, item.faceCenter.z - 0.01f);
    CHECK(index.pick(Ray{below, {0.0f, 1.0f, 0.0f}}).id == kNoPick);
    const glm::vec3 above(item.faceCenter.x, 2.0f, item.faceCenter.z - 0.01f);
    CHECK(index.pick(Ray{above, {0.0f, -1.0f, 0.0f}}).id == kNoPick);
    // From inside the compartment the same column of items is there to pick.
    const glm::vec3 inside(item.faceCenter.x, item.faceCenter.y + 0.07f, item.faceCenter.z - 0.01f);
    CHECK(index.pick(Ray{inside, {0.0f, -1.0f, 0.0f}}).id == item.id);
}

TEST_CASE("world/pick_index: the ray reaches 2.5 m and no farther", "[world][pick_index]") {
    const SyntheticShelves shelves = oneModule();
    const PickIndex index = indexOf(shelves);
    const SyntheticItem& item = *shelves.at(0, 4, 16);
    const glm::vec3 face = item.faceCenter;
    CHECK(index.pick(Ray{face + glm::vec3(0.0f, 0.0f, 2.49f), {0.0f, 0.0f, -1.0f}}).id == item.id);
    CHECK(index.pick(Ray{face + glm::vec3(0.0f, 0.0f, 2.51f), {0.0f, 0.0f, -1.0f}}).id == kNoPick);
    // A range of the caller's, and the direction's length does not matter.
    CHECK(index.pick(Ray{face + glm::vec3(0.0f, 0.0f, 2.51f), {0.0f, 0.0f, -4.0f}}, 3.0f).id == item.id);
    CHECK(index.pick(Ray{face + glm::vec3(0.0f, 0.0f, 1.0f), {0.0f, 0.0f, -1.0f}}, 0.9f).id == kNoPick);
    // Rays that cannot hit anything.
    CHECK(index.pick(Ray{face + glm::vec3(0.0f, 0.0f, 1.0f), {0.0f, 0.0f, 0.0f}}).id == kNoPick);
    CHECK(index.pick(Ray{face + glm::vec3(0.0f, 0.0f, 1.0f), {0.0f, std::nanf(""), -1.0f}}).id == kNoPick);
    CHECK(index.pick(Ray{face + glm::vec3(0.0f, 0.0f, 1.0f), {0.0f, 0.0f, -1.0f}}, 0.0f).id == kNoPick);
    CHECK(index.pick(Ray{face + glm::vec3(0.0f, 0.0f, 1.0f), {0.0f, 0.0f, -1.0f}},
                     std::numeric_limits<float>::infinity())
              .id == kNoPick);
    // Looking away.
    CHECK(index.pick(Ray{face + glm::vec3(0.0f, 0.0f, 1.0f), {0.0f, 0.0f, 1.0f}}).id == kNoPick);
}

TEST_CASE("world/pick_index: ties go to the smaller id, a flush occluder to the target", "[world][pick_index]") {
    const Aabb box = Aabb::fromCenterHalfExtents({0.0f, 0.0f, 0.0f}, {0.05f, 0.05f, 0.05f});
    const glm::mat4 frame = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    // Two targets in one place, given in either order and in either cluster order.
    for (const bool reversed : {false, true}) {
        std::vector<PickTarget> targets{PickTarget{9, reversed ? 1u : 0u, frame, box},
                                        PickTarget{4, reversed ? 0u : 1u, frame, box}};
        if (reversed) {
            std::swap(targets[0], targets[1]);
        }
        const auto index = PickIndex::build(targets, {});
        REQUIRE(index.has_value());
        CHECK(index->pick(Ray{{0.0f, 1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}}).id == 4);
    }
    // An occluder whose face is flush with the target's front does not hide it; one a hair in front does.
    const Aabb flush = Aabb::fromMinMax({-0.2f, 0.9f, -0.2f}, {0.2f, 1.1f, 0.05f});
    const Aabb inFront = Aabb::fromMinMax({-0.2f, 0.9f, 0.0501f}, {0.2f, 1.1f, 0.06f});
    const std::vector<PickTarget> one{PickTarget{3, 0, frame, box}};
    const auto flushIndex = PickIndex::build(one, std::vector<Aabb>{flush});
    REQUIRE(flushIndex.has_value());
    CHECK(flushIndex->pick(Ray{{0.0f, 1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}}).id == 3);
    const auto hiddenIndex = PickIndex::build(one, std::vector<Aabb>{inFront});
    REQUIRE(hiddenIndex.has_value());
    CHECK(hiddenIndex->pick(Ray{{0.0f, 1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}}).id == kNoPick);
    // The nearer of two targets in a line wins whatever their ids.
    const std::vector<PickTarget> line{PickTarget{1, 0, frame, box},
                                       PickTarget{2, 0, glm::translate(frame, glm::vec3(0.0f, 0.0f, 0.3f)), box}};
    const auto lineIndex = PickIndex::build(line, {});
    REQUIRE(lineIndex.has_value());
    const rv::world::PickHit hit = lineIndex->pick(Ray{{0.0f, 1.0f, 1.0f}, {0.0f, 0.0f, -1.0f}});
    CHECK(hit.id == 2);
    CHECK(hit.distance == Approx(0.65f));
}

TEST_CASE("world/pick_index: tilted and turned targets are tested in their own frames", "[world][pick_index]") {
    // A face-out cartridge leaning back 30 degrees, as in a display case.
    const Aabb item = Aabb::fromCenterHalfExtents({0.0f, 0.0f, 0.0f}, {0.0625f, 0.0425f, 0.01f});
    const glm::mat4 frame = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                                        rv::core::degreesToRadians(-30.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    const std::vector<PickTarget> targets{PickTarget{5, 0, frame, rv::world::faceTargetBox(item)}};
    const auto index = PickIndex::build(targets, {});
    REQUIRE(index.has_value());
    // Through the world box's top front edge, sideways: the leaning item never reaches that corner of its box.
    const Aabb worldBox = item.transformed(frame);
    const glm::vec3 corner(1.0f, worldBox.max.y - 0.002f, worldBox.max.z - 0.002f);
    CHECK(worldBox.intersectRay(Ray{corner, {-1.0f, 0.0f, 0.0f}}).has_value());
    CHECK(index->pick(Ray{corner, {-1.0f, 0.0f, 0.0f}}).id == kNoPick);
    // Its face's middle, straight along its leaning normal.
    const glm::vec3 normal = glm::normalize(glm::vec3(frame * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));
    const glm::vec3 middle = glm::vec3(frame * glm::vec4(0.0f, 0.0f, 0.01f, 1.0f));
    const rv::world::PickHit hit = index->pick(Ray{middle + normal, -normal});
    CHECK(hit.id == 5);
    CHECK(hit.distance == Approx(1.0f).margin(1e-4));

    // A run of modules on a west wall (turned 90 degrees, facing +X): every item picked from 1 m along its normal.
    const std::vector<ShelfModuleSpec> modules(2);
    const glm::mat4 west = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(-4.38f, 0.0f, 2.0f)),
                                       rv::core::degreesToRadians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const SyntheticShelves shelves = rv::test::buildShelves(modules, west);
    const PickIndex turned = indexOf(shelves);
    for (const SyntheticItem& candidate : shelves.items) {
        CAPTURE(candidate.id);
        CHECK(candidate.out.x == Approx(1.0f));
        CHECK(turned.pick(Ray{candidate.faceCenter + candidate.out, -candidate.out}).id == candidate.id);
    }
}

TEST_CASE("world/pick_index: the sample says whether the ray is on a core and on the current hover",
          "[world][pick_index]") {
    const SyntheticShelves shelves = oneModule();
    const PickIndex index = indexOf(shelves);
    const SyntheticItem& a = *shelves.at(0, 4, 10);
    const SyntheticItem& b = *shelves.at(0, 4, 11);
    const glm::vec3 eye = a.faceCenter + glm::vec3(0.012f, 0.0f, 1.5f);
    const auto at = [&](float dx, PickId current) {
        return index.sample(rv::test::rayTowards(eye, onFace(a, dx, 0.0f)), current);
    };
    // A's middle: its core.
    CHECK(at(0.0f, kNoPick) == HoverSample{a.id, true, false});
    CHECK(at(0.0f, a.id) == HoverSample{a.id, true, true});
    // 1.05 cm right of A's middle: A's inflated rim, not its core.
    CHECK(at(0.0105f, a.id) == HoverSample{a.id, false, true});
    // 1.2 cm: between the targets (2 mm apart), within A's margin.
    CHECK(at(0.012f, a.id) == HoverSample{kNoPick, false, true});
    CHECK(at(0.012f, b.id) == HoverSample{kNoPick, false, true}); // the two margins overlap in the gap
    // 1.35 cm: B's rim (B's target starts 1.3 cm right of A's middle), past A's margin.
    CHECK(at(0.0135f, a.id) == HoverSample{b.id, false, false});
    // 1.6 cm: B's core.
    CHECK(at(0.016f, a.id) == HoverSample{b.id, true, false});
    // An unknown current hover is never on.
    CHECK(at(0.0f, 99999).onCurrent == false);
    // A zero ray samples nothing.
    CHECK(index.sample(Ray{eye, glm::vec3(0.0f)}, a.id) == HoverSample{});
}

TEST_CASE("world/pick_index: the hover changes after 3 frames on another core", "[world][pick_index][hover]") {
    const SyntheticShelves shelves = oneModule();
    const PickIndex index = indexOf(shelves);
    const SyntheticItem& a = *shelves.at(0, 4, 10);
    const SyntheticItem& b = *shelves.at(0, 4, 11);
    const glm::vec3 eye = a.faceCenter + glm::vec3(0.0f, 0.0f, 1.5f);
    const Ray onA = rv::test::rayTowards(eye, a.faceCenter);
    const Ray onB = rv::test::rayTowards(eye, b.faceCenter);
    const Ray onBRim = rv::test::rayTowards(eye, onFace(b, -0.0105f, 0.0f));
    const Ray gap = rv::test::rayTowards(eye, onFace(a, 0.012f, 0.0f));
    const Ray away = Ray{eye, {0.0f, 0.0f, 1.0f}};

    HoverTracker tracker;
    CHECK(tracker.hovered() == kNoPick);
    // Acquired on the third consecutive frame.
    CHECK(hold(tracker, index, onA, 1) == kNoPick);
    CHECK(tracker.candidate() == a.id);
    CHECK(tracker.candidateFrames() == 1);
    CHECK(hold(tracker, index, onA, 1) == kNoPick);
    CHECK(hold(tracker, index, onA, 1) == a.id);
    CHECK(tracker.candidateFrames() == 0);
    // Two frames on B, one back on A: the count starts again.
    CHECK(hold(tracker, index, onB, 2) == a.id);
    CHECK(hold(tracker, index, onA, 1) == a.id);
    CHECK(hold(tracker, index, onB, 2) == a.id);
    CHECK(hold(tracker, index, onB, 1) == b.id);
    // B's rim never takes the hover back to A, nor gives it to anyone: from A, B's rim keeps A.
    tracker.force(a.id);
    CHECK(hold(tracker, index, onBRim, 30) == a.id);
    // Between the targets, within A's margin: A stays.
    CHECK(hold(tracker, index, gap, 30) == a.id);
    // Nothing for 3 frames: the hover leaves.
    CHECK(hold(tracker, index, away, 2) == a.id);
    CHECK(hold(tracker, index, away, 1) == kNoPick);
    // Nothing hovered, the gap and a rim acquire nothing.
    CHECK(hold(tracker, index, gap, 30) == kNoPick);
    CHECK(hold(tracker, index, onBRim, 30) == kNoPick);
    // A still ray never changes it once it is held.
    CHECK(hold(tracker, index, onB, 3) == b.id);
    for (int frame = 0; frame < 600; ++frame) {
        REQUIRE(tracker.update(index.sample(onB, tracker.hovered())) == b.id);
    }
    tracker.reset();
    CHECK(tracker.hovered() == kNoPick);
    CHECK(tracker.candidateFrames() == 0);
}

TEST_CASE("world/pick_index: a ray jittered by 1 mm never changes the hover", "[world][pick_index][hover]") {
    const SyntheticShelves shelves = oneModule();
    const PickIndex index = indexOf(shelves);
    rv::core::Random random(0x5EED7);
    const auto jitter = [&] { return static_cast<float>(random.unit() * 0.002 - 0.001); };
    std::size_t frames = 0;
    for (const float distance : {0.9f, 1.5f}) {
        const glm::vec3 eye = eyeInFront(distance);
        for (const int level : {0, 3, 6}) {
            for (int slot = 0; slot < 33; slot += 4) {
                const SyntheticItem& item = *shelves.at(0, level, slot);
                // Points across the face and on the inflated target's edges, the hover acquired there first.
                for (const float dx : {-0.011f, -0.006f, 0.0f, 0.006f, 0.011f}) {
                    for (const float dy : {-0.05f, 0.0f, 0.05f}) {
                        HoverTracker tracker;
                        tracker.force(item.id);
                        const glm::vec3 base = onFace(item, dx, dy);
                        if (index.pick(rv::test::rayTowards(eye, base)).id != item.id) {
                            continue; // the inflated edge is hidden by a board from this eye
                        }
                        for (int frame = 0; frame < 60; ++frame) {
                            const glm::vec3 point = base + jitter() * item.right + jitter() * item.up;
                            const PickId hovered =
                                tracker.update(index.sample(rv::test::rayTowards(eye, point), tracker.hovered()));
                            ++frames;
                            if (hovered != item.id) {
                                CAPTURE(distance, level, slot, dx, dy, frame);
                                FAIL("the hover changed");
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(frames > 10000);
}
