// I3 step 2: the benchmark of the room's bulk snapshot (SqliteCatalogQuery::roomEntries, ADR-0010 §12), hidden
// ([.bench]) and meant for Release builds:
//   build/msvc-x64-release/bin/rv_tests.exe "[room_query][bench]"
// The budget (docs/roadmap.md, I3 "Budgets": the catalog worker): the snapshot of a system of 2,000 games within 50 ms.
// A generated catalog of 20,000 entries (tests/support/catalog_generator.hpp: every state, media, favourites) holds
// systems of about that size among others; the largest is read on a reader as the catalog worker reads it, once to warm
// the page cache, then 20 times, and its median and slowest times are printed and checked, together with the
// selection's own time (world::selectRoom, informative). Timings taken while other work runs are informative; the gate
// measures on a quiet machine (docs/i3/step-2.md records the figures).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/world_config.hpp>

#include "catalog_db_test_support.hpp"
#include "support/catalog_generator.hpp"
#include "support/temp_dir.hpp"

using namespace rv::catalog;
using Clock = std::chrono::steady_clock;

namespace {

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

} // namespace

TEST_CASE("catalog_db/room_query: the snapshot of 2,000 games within 50 ms", "[.bench][bench][room_query]") {
    rv::test::TempDir dir("rv-room-query-bench");
    rv::test::CatalogGeneratorOptions options;
    options.entries = 20'000;
    auto generated = rv::test::generateCatalogDatabase(rv::test::databaseFileIn(dir), options, false);
    REQUIRE(generated.has_value());
    auto database = rv::catalog_db::openCatalogDatabase(rv::test::databaseFileIn(dir));
    REQUIRE(database.has_value());
    auto query = rv::catalog_db::SqliteCatalogQuery::open(database->openReader().value());
    REQUIRE(query.has_value());

    // The largest system.
    const auto systems = (*query)->systems();
    REQUIRE(systems.has_value());
    const auto largest = std::max_element(systems->begin(), systems->end(),
                                          [](const auto& a, const auto& b) { return a.entries < b.entries; });
    REQUIRE(largest != systems->end());
    const SystemName system = largest->system.name;
    REQUIRE(largest->entries >= 2'000);

    REQUIRE((*query)->roomEntries(system).has_value()); // warms the page cache, as the app's reader is
    std::vector<double> reads;
    std::vector<double> selections;
    std::size_t entries = 0;
    std::size_t titles = 0;
    for (int run = 0; run < 20; ++run) {
        const Clock::time_point start = Clock::now();
        auto snapshot = (*query)->roomEntries(system);
        reads.push_back(millisecondsSince(start));
        REQUIRE(snapshot.has_value());
        entries = snapshot->entries.size();
        snapshot->mediaKnown = true; // as esde::checkRoomMedia() says of a media root that is there
        const Clock::time_point selected = Clock::now();
        const auto selection =
            rv::world::selectRoom(rv::world::RoomInput{generated->library.id, *snapshot}, rv::world::WorldConfig{});
        selections.push_back(millisecondsSince(selected));
        REQUIRE(selection.has_value());
        titles = selection->titles.size();
    }
    const double medianRead = median(reads);
    const double slowestRead = *std::max_element(reads.begin(), reads.end());
    std::cout << fmt::format(
        "Room snapshot of {} ({} entries, {} titles selected): median {:.2f} ms, slowest {:.2f} ms; "
        "selection median {:.2f} ms\n",
        system.str(), entries, titles, medianRead, slowestRead, median(selections));
    CHECK(entries == largest->entries);
    CHECK(medianRead <= 50.0);
}
