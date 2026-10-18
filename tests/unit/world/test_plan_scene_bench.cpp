// I3 step 10a: the time the plan-to-scene conversion takes, hidden ([.bench]) and meant for Release builds:
//   build/msvc-x64-release/bin/rv_tests.exe "[plan_scene][bench]"
// On the 2,000-title golden plan (the stress fixture's size): loadSceneData (the template, the platform, the families
// and every prefab built from its manifest: file reads), then toSceneFile, toSceneDescription and collectionLayout
// (pure), 21 runs each; the medians are printed and the pure part checked against 100 ms, a budget of this step's within
// step 10b's "room committed within 1 s of the snapshot" (generation takes about 70 ms of it, I3 step 3). Timings taken
// while other work runs are informative (docs/i3/step-10a.md records the figures).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

#include <fmt/format.h>

#include <rv/world/collection_layout.hpp>
#include <rv/world/scene_builder.hpp>

#include "plan_scene_support.hpp"

using Clock = std::chrono::steady_clock;

namespace {

template <class Work>
double medianMs(int runs, Work&& work) {
    std::vector<double> times;
    for (int run = 0; run < runs; ++run) {
        const auto start = Clock::now();
        work();
        times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

} // namespace

TEST_CASE("world/plan_scene: the conversion of 2,000 titles", "[.bench][plan_scene][bench]") {
    const rv::test::world::PlanCase& entry = rv::test::world::planCase("plan-2000-seed-102938");
    constexpr int kRuns = 21;
    const double load = medianMs(kRuns, [&] {
        auto data = rv::world::loadSceneData(rv::test::world::sourceAssetRoot(), entry.plan, entry.room);
        REQUIRE(data.has_value());
    });
    const double sceneFile = medianMs(kRuns, [&] {
        auto build = rv::world::toSceneFile(entry.plan, entry.room, entry.data);
        REQUIRE(build.has_value());
    });
    const double description = medianMs(kRuns, [&] {
        auto scene = rv::world::toSceneDescription(entry.plan, entry.room, entry.data);
        REQUIRE(scene.has_value());
    });
    const double layout = medianMs(kRuns, [&] {
        auto built = rv::world::collectionLayout(entry.plan, entry.room, entry.data, entry.titles);
        REQUIRE(built.has_value());
        REQUIRE(built->items.size() == 2000);
    });
    std::cout << fmt::format("plan-to-scene, 2,000 titles, medians of {} runs: loadSceneData {:.2f} ms, toSceneFile "
                             "{:.2f} ms, toSceneDescription {:.2f} ms, collectionLayout {:.2f} ms\n",
                             kRuns, load, sceneFile, description, layout);
#if defined(NDEBUG)
    CHECK(description + layout < 100.0);
#endif
}
