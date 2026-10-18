// I3 step 7: the benchmark of picking (world::PickIndex, ADR-0012 §2: "answers in microseconds"), hidden ([.bench])
// and meant for Release builds:
//   build/msvc-x64-release/bin/rv_tests.exe "[pick_index][bench]"
// A hand-built room of the 16 m template's size: two hero-wall modules on the north wall and 13 side-wall modules on
// each side wall (28 modules, 6,468 spines in 196 clusters, 336 occluders), and the 9 x 10 m room's 16 modules (3,696
// spines). Two sets of 20,000 seeded rays: free ones (eyes anywhere in the room at standing and crouching heights,
// looking any way, level to steeply down) and aimed ones (from 0.6 to 2 m in front of a random item, at its face), each
// sampled as the hover samples it every frame (PickIndex::sample with a current hover); the median and the 99th
// percentile of batches of 1,000 are printed and checked against 20 microseconds a sample, a budget of
// this step's (the roadmap's I3 budgets list none for picking; one sample a frame is far below a frame's main-thread
// budget either way). Timings taken while other work runs are informative (docs/i3/step-7.md records the figures).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

#include <fmt/format.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <rv/core/math.hpp>
#include <rv/core/random.hpp>
#include <rv/world/pick_index.hpp>

#include "synthetic_shelves.hpp"

using Clock = std::chrono::steady_clock;
using rv::test::ShelfModuleSpec;
using rv::test::SyntheticShelves;

namespace {

/// One room: two modules on the north wall (facing south) and `sideModules` on each side wall (facing into the room),
/// its north wall at z = -5.
SyntheticShelves room(int sideModules) {
    const float north = -5.0f + 0.5f * rv::test::shelf::kDepth + 0.03f;
    const float west = -4.5f + 0.5f * rv::test::shelf::kDepth + 0.03f;
    const std::vector<ShelfModuleSpec> hero(2);
    const std::vector<ShelfModuleSpec> side(static_cast<std::size_t>(sideModules));
    SyntheticShelves all = rv::test::buildShelves(hero, glm::translate(glm::mat4(1.0f), glm::vec3(-2.5f, 0.0f, north)));
    const auto append = [&](const SyntheticShelves& more) {
        all.targets.insert(all.targets.end(), more.targets.begin(), more.targets.end());
        all.occluders.insert(all.occluders.end(), more.occluders.begin(), more.occluders.end());
        all.items.insert(all.items.end(), more.items.begin(), more.items.end());
    };
    // The west wall's modules face +X, a run going north from its southern module; the east wall's face -X, going
    // south from its northern one; both between z = -4 and the room's south end.
    const float southmost = -4.0f + static_cast<float>(sideModules - 1);
    const glm::mat4 westFrame = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(west, 0.0f, southmost)),
                                            rv::core::degreesToRadians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 eastFrame = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(-west, 0.0f, -4.0f)),
                                            rv::core::degreesToRadians(-90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    // Distinct ids and clusters per wall.
    SyntheticShelves westShelves = rv::test::buildShelves(side, westFrame, 100'000);
    SyntheticShelves eastShelves = rv::test::buildShelves(side, eastFrame, 200'000);
    for (auto& target : westShelves.targets) {
        target.cluster += 10'000;
    }
    for (auto& target : eastShelves.targets) {
        target.cluster += 20'000;
    }
    append(westShelves);
    append(eastShelves);
    return all;
}

/// A sample's budget: see the file comment.
constexpr double kBudgetMicroseconds = 20.0;

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1))];
}

} // namespace

TEST_CASE("world/pick_index: a sample within microseconds on thousands of targets", "[.bench][bench][pick_index]") {
    for (const int sideModules : {7, 13}) {
        const SyntheticShelves shelves = room(sideModules);
        const auto built = rv::world::PickIndex::build(shelves.targets, shelves.occluders);
        REQUIRE(built.has_value());
        const rv::world::PickIndex& index = *built;

        rv::core::Random random(0x9E3779B97F4A7C15ull + static_cast<std::uint64_t>(sideModules));
        const auto uniform = [&](double low, double high) {
            return static_cast<float>(low + (high - low) * random.unit());
        };
        constexpr int kBatches = 20;
        constexpr int kPerBatch = 1000;
        // Free rays: anywhere in the room, any way; most reach nothing within 2.5 m.
        std::vector<rv::core::Ray> roaming;
        const float roomSouth = -5.0f + 2.0f + static_cast<float>(sideModules) * 1.0f;
        for (int i = 0; i < kBatches * kPerBatch; ++i) {
            const glm::vec3 eye(uniform(-3.8, 3.8), uniform(0.9, 1.75), uniform(-4.4, roomSouth));
            const float yaw = uniform(-rv::core::kPi, rv::core::kPi);
            const float pitch = uniform(-0.9, 0.2);
            const glm::vec3 direction(std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                                      -std::cos(yaw) * std::cos(pitch));
            roaming.push_back(rv::core::Ray{eye, direction});
        }
        // Aimed rays: from 0.6 to 2 m in front of a random item, at a point of its face (the hover's usual case).
        std::vector<rv::core::Ray> aimed;
        for (int i = 0; i < kBatches * kPerBatch; ++i) {
            const auto& item = shelves.items[static_cast<std::size_t>(random.below(shelves.items.size()))];
            const glm::vec3 side = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), item.out));
            glm::vec3 eye = item.faceCenter + uniform(0.6, 2.0) * item.out + uniform(-0.5, 0.5) * side;
            eye.y = uniform(0.9, 1.75);
            const glm::vec3 point = item.faceCenter + uniform(-0.009, 0.009) * item.right + uniform(-0.06, 0.06) * item.up;
            aimed.push_back(rv::core::Ray{eye, glm::normalize(point - eye)});
        }

        for (const auto* rays : {&roaming, &aimed}) {
            rv::world::PickId current = rv::world::kNoPick;
            std::size_t hits = 0;
            std::vector<double> batches;
            for (int batch = 0; batch < kBatches; ++batch) {
                const Clock::time_point start = Clock::now();
                for (int i = 0; i < kPerBatch; ++i) {
                    const rv::world::HoverSample sample =
                        index.sample((*rays)[static_cast<std::size_t>(batch * kPerBatch + i)], current);
                    if (sample.hit != rv::world::kNoPick) {
                        ++hits;
                        current = sample.hit;
                    }
                }
                batches.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count() /
                                  kPerBatch);
            }
            const double median = percentile(batches, 0.5);
            const double p99 = percentile(batches, 0.99);
            std::cout << fmt::format("Pick index of {} targets in {} clusters, {} occluders, {} rays: a sample median "
                                     "{:.2f} us, p99 {:.2f} us over {} batches of {}; {} of {} rays on an item\n",
                                     index.targetCount(), index.clusterCount(), index.occluderCount(),
                                     rays == &roaming ? "free" : "aimed", median, p99, kBatches, kPerBatch, hits,
                                     rays->size());
            CHECK(hits > (rays == &roaming ? rays->size() / 25 : rays->size() / 2));
            CHECK(median <= kBudgetMicroseconds);
        }
        CHECK(index.targetCount() >= 3000);
    }
}
