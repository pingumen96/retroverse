// I3 step 4: benchmarks of the validation (docs/roadmap.md, I3 row 4 and "Budgets"), hidden ([.bench]) and meant for
// Release builds, run directly: `build/msvc-x64-release/bin/rv_tests.exe "[.bench]"`. The grid of the 16 m room (9 x
// 16 m, its 26 side-wall modules and the hero wall) built and searched within 20 ms, the whole validation of that
// plan (2,000 placements, two grids, the line of sight) reported, the median of 21 runs each.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

#include <fmt/format.h>

#include <rv/world/nav_grid.hpp>
#include <rv/world/validation.hpp>

#include "world_test_plans.hpp"

namespace {

constexpr int kRuns = 21;

template <class Work>
double medianMs(Work&& work) {
    std::vector<double> times;
    for (int run = 0; run < kRuns; ++run) {
        const auto start = std::chrono::steady_clock::now();
        work();
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

/// The floor of a fixture turned by a quarter turn or not (the hero corner's turned parts as their bounding square).
rv::world::Footprint floorOf(const rv::world::Fixture& fixture) {
    std::int64_t x = fixture.widthMm;
    std::int64_t z = fixture.depthMm;
    if (fixture.yawCdeg == 9000 || fixture.yawCdeg == 27000) {
        std::swap(x, z);
    } else if (fixture.yawCdeg % 9000 != 0) {
        x = z = std::max(fixture.widthMm, fixture.depthMm) * 3 / 2;
    }
    return {fixture.xMm - x / 2 - 1, fixture.zMm - z / 2 - 1, fixture.xMm + x / 2 + 1, fixture.zMm + z / 2 + 1};
}

} // namespace

TEST_CASE("world/bench: the 0.1 m grid of the 9 x 16 m room built and searched within 20 ms", "[.bench]") {
    const rv::world::WorldPlan plan = rv::test::grownRoomPlan(3);
    const rv::world::Room& room = plan.rooms.front();
    REQUIRE(room.maxZMm - room.minZMm == 16'000);
    std::vector<rv::world::Footprint> obstacles;
    for (const rv::world::Fixture& fixture : plan.fixtures) {
        if (fixture.heightMm > rv::world::kFloorCoveringMaxHeightMm) {
            obstacles.push_back(floorOf(fixture));
        }
    }
    const rv::world::Footprint area{room.minXMm, room.minZMm, room.maxXMm, room.maxZMm};
    std::size_t reached = 0;
    const double gridMs = medianMs([&] {
        const rv::world::NavGrid grid(area, obstacles, 600);
        const rv::world::NavReach reach(grid, *grid.cellAt(0, room.maxZMm - 750));
        reached = reach.reachedCount();
    });
    WARN(fmt::format("grid of {} x {} cells, {} obstacles, {} cells reached: median {:.3f} ms", 90, 160,
                     obstacles.size(), reached, gridMs));
    CHECK(reached > 4'000);
    CHECK(gridMs <= 20.0);

    const rv::world::ValidationContext context = rv::test::validContextFor(plan);
    rv::world::ValidationReport report;
    const double validationMs = medianMs([&] { report = rv::world::validatePlan(plan, context); });
    WARN(fmt::format("validatePlan of {} fixtures and {} placements: median {:.3f} ms", plan.fixtures.size(),
                     plan.placements.size(), validationMs));
    CHECK(report.passed());
    // Within the budget of a fresh plan of 2,000 titles with its validation (200 ms), with room for the generator.
    CHECK(validationMs <= 100.0);
}
