// I4 step 6b: the building job on a large generated catalog, hidden ([.bench]) and meant for Release builds:
//   build/msvc-x64-release/bin/rv_tests.exe "[world_build][bench]"
// A catalog of 90,000 generated entries (tests/support/catalog_generator.hpp: eleven systems of every state, media,
// favourites; about the size of the reference library's 91,000 games) under an empty media root that can be read (the
// media known), planned whole by the building job as the app runs it (planBuilding(): every system's snapshot in one
// read transaction, the selections, the generation or the update, the plan's write, the rooms' items files), then
// again with nothing changed (the whole job, as before I4 step 6b), then with the building stamp compared instead (the
// app's unchanged start since I4 step 6b), the start floor built from the plan and the items files. Each phase's time
// is printed (informative: the acceptable row is the app's start, docs/roadmap.md).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>

#include <fmt/format.h>

#include <rv/app/room_snapshot.hpp>
#include <rv/app/world_build.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/catalog_stamp.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/worker_pool.hpp>

#include "support/catalog_generator.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string phases(const rv::app::BuildingResult& result, double snapshotMs, double totalMs) {
    const rv::app::WorldBuild& build = result.build;
    return fmt::format("{} in {:.0f} ms: snapshot {:.0f}, lock {:.0f}, selections {:.0f}, rooms' data {:.0f}, plan "
                       "{:.0f}, save {:.0f}, items {:.0f} ({} placements, {} rooms)",
                       rv::app::toString(build.outcome), totalMs, snapshotMs, build.lockMs, build.selectMs,
                       build.dataMs, build.planMs, build.saveMs, build.itemsMs,
                       build.plan ? build.plan->placements.size() : 0, build.plan ? build.plan->rooms.size() : 0);
}

} // namespace

TEST_CASE("app/world_build: the building job on a catalog of 90,000 generated entries, then unchanged",
          "[.bench][bench][world_build]") {
    const rv::test::TempDir dir("rv-world-build-bench");
    rv::test::CatalogGeneratorOptions options;
    options.entries = 90'000;
    const Clock::time_point generatedAt = Clock::now();
    auto generated = rv::test::generateCatalogDatabase(dir.file("retroverse.sqlite"), options, false);
    REQUIRE(generated.has_value());
    std::cout << fmt::format("Generated {} entries in {:.0f} ms\n", generated->entryCount,
                             millisecondsSince(generatedAt));
    auto database = rv::catalog_db::openCatalogDatabase(dir.file("retroverse.sqlite"));
    REQUIRE(database.has_value());
    rv::app::WorldCatalogReader reader;
    reader.query = rv::catalog_db::SqliteCatalogQuery::open(database->openReader().value()).value();
    reader.profile = generated->library;
    reader.profile.esdeHome = dir.file("ES-DE");
    reader.profile.mediaRoot = dir.file("media");
    fs::create_directories(*reader.profile.mediaRoot);

    rv::app::WorldBuildInput input;
    input.paths = rv::app::WorldPaths{rv::core::pathFromUtf8(RV_TEST_ASSET_DIR), dir.file("data"),
                                      dir.file("data/config"), dir.file("data/cache")};
    input.library = reader.profile.id;
    input.writer = "retroverse (bench)";
    const rv::app::SystemFilter wanted = rv::app::roomSystemsFilter(input.paths.assetRoot);

    const auto run = [&](std::string_view label) {
        std::vector<rv::catalog::GameId> placed;
        if (auto stored = rv::app::readWorldPlan(input.paths, input.library).stored) {
            for (const rv::world::Placement& placement : stored->plan.placements) {
                placed.push_back(placement.game);
            }
        }
        const Clock::time_point start = Clock::now();
        input.snapshot = rv::app::takeBuildingSnapshot(reader, placed, wanted);
        const double snapshotMs = millisecondsSince(start);
        REQUIRE_FALSE(input.snapshot.error.has_value());
        REQUIRE(input.snapshot.mediaKnown());
        rv::app::BuildingResult result = rv::app::planBuilding(input, rv::core::JobContext{});
        const double totalMs = millisecondsSince(start);
        std::cout << fmt::format("{}: {}\n", label, phases(result, snapshotMs, totalMs));
        REQUIRE(result.build.plan.has_value());
        return result;
    };
    const rv::app::BuildingResult first = run("First building");
    CHECK(first.build.outcome == rv::app::WorldOutcome::Generated);
    const rv::app::BuildingResult again = run("Unchanged start, the whole job");
    CHECK(again.build.outcome == rv::app::WorldOutcome::Unchanged);
    CHECK(again.build.planDigest == first.build.planDigest);
    CHECK_FALSE(again.build.itemsWritten);

    // The unchanged start as the app's job makes it (I4 step 6b): the building stamp compared before any snapshot,
    // then the start floor (the lobby) built from the plan and the items files.
    const auto stored = rv::app::readWorldPlan(input.paths, input.library).stored;
    REQUIRE(stored.has_value());
    const Clock::time_point start = Clock::now();
    const auto catalogStamp = rv::catalog_db::catalogStamp(reader.query->connection(), input.library);
    REQUIRE(catalogStamp.has_value());
    std::optional<rv::app::BuildingResult> unchanged =
        rv::app::unchangedBuilding(input.paths, reader.profile, *stored, *catalogStamp, std::nullopt);
    const double shortcutMs = millisecondsSince(start);
    REQUIRE(unchanged.has_value());
    rv::app::buildFloor(input.paths, unchanged->build, unchanged->sources);
    std::cout << fmt::format("Unchanged start, the building stamp: {:.0f} ms, the lobby's floor {:.0f} ms more\n",
                             shortcutMs, unchanged->build.floorMs);
    CHECK(unchanged->build.skipped);
    CHECK(unchanged->build.scene.has_value());
}
