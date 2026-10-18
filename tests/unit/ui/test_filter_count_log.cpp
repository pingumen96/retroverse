// When the quick catalog logs a filter's count (after the M1 manual session, whose log read
// "Quick catalog: filter 'gba', <N> game(s)" with the count of the list shown before the filter's, still on the catalog
// worker): once the list that answers the filter is the one shown.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rv/ui/filter_count_log.hpp>
#include <rv/ui/in_memory_catalog_source.hpp>
#include <rv/ui/quick_catalog_model.hpp>

using rv::ui::CatalogEntry;
using rv::ui::FilterCountLog;
using rv::ui::InMemoryCatalogSource;
using rv::ui::QuickCatalogModel;

namespace {

/// `count` games, every third on "Arcade" and the others on "16 bit".
std::vector<CatalogEntry> catalog(std::size_t count) {
    std::vector<CatalogEntry> games;
    for (std::size_t index = 0; index < count; ++index) {
        CatalogEntry game;
        game.id = "g" + std::to_string(1000000 + index);
        game.title = "Gioco " + std::to_string(index);
        game.platform = index % 3 == 0 ? "Arcade" : "16 bit";
        games.push_back(std::move(game));
    }
    return games;
}

} // namespace

TEST_CASE("ui/filter_count_log: a list materialized at once is logged at once", "[ui][filter_count_log]") {
    InMemoryCatalogSource source(catalog(30));
    QuickCatalogModel model(source, 7);
    FilterCountLog log;
    CHECK_FALSE(log.take(model).has_value()); // no filter chosen
    REQUIRE(model.setFilter("Arcade"));
    log.chosen(model.activeFilter());
    CHECK(log.waiting());
    CHECK(log.take(model) == std::optional<std::string>("filter 'Arcade', 10 game(s)"));
    CHECK_FALSE(log.waiting());
    CHECK_FALSE(log.take(model).has_value()); // once
    REQUIRE(model.setFilter(""));
    log.chosen(model.activeFilter());
    CHECK(log.take(model) == std::optional<std::string>("filter '(every game)', 30 game(s)"));
}

TEST_CASE("ui/filter_count_log: a list from the worker is logged with its own count once it is shown",
          "[ui][filter_count_log]") {
    InMemoryCatalogSource source(catalog(30));
    QuickCatalogModel model(source, 7);
    source.setPendingPolls(2);
    FilterCountLog log;

    REQUIRE(model.setFilter("Arcade"));
    log.chosen(model.activeFilter());
    // The previous list (every game) is still the one shown: no count yet, never the previous list's.
    REQUIRE(model.pending());
    REQUIRE(model.count() == 30);
    CHECK_FALSE(log.take(model).has_value());
    CHECK_FALSE(model.poll());
    CHECK_FALSE(log.take(model).has_value());
    REQUIRE(model.poll());
    REQUIRE(model.count() == 10);
    CHECK(log.take(model) == std::optional<std::string>("filter 'Arcade', 10 game(s)"));

    // Another filter chosen while one is on the worker: only the latest one's count, with its list.
    REQUIRE(model.setFilter("16 bit"));
    log.chosen(model.activeFilter());
    REQUIRE(model.setFilter("Arcade"));
    log.chosen(model.activeFilter());
    CHECK_FALSE(log.take(model).has_value());
    CHECK_FALSE(model.poll());
    REQUIRE(model.poll());
    CHECK(log.take(model) == std::optional<std::string>("filter 'Arcade', 10 game(s)"));

    // A search typed while the filter's list is on the worker: the count of the list shown, the search applied.
    REQUIRE(model.setFilter("16 bit"));
    log.chosen(model.activeFilter());
    model.setSearch("Gioco 25");
    CHECK_FALSE(model.poll());
    CHECK_FALSE(log.take(model).has_value());
    REQUIRE(model.poll());
    CHECK(model.count() == 1);
    CHECK(log.take(model) == std::optional<std::string>("filter '16 bit', 1 game(s)"));
}
