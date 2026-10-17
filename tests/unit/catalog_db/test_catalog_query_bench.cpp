// I2 step 9: benchmarks of the catalog's queries (ADR-0008 §5), hidden ([.bench]) and meant for Release builds:
//   build/msvc-x64-release/bin/rv_tests.exe "[bench]"
// NFR-02: the p95 of indexed queries at most 100 ms on 10,000 games; a UI page (30 rows fetched by ids) within 5 ms on
// 100,000 games; and, informative, the materialization of lists on 100,000 games: every game in each order, a common
// one-letter and two-letter prefix (both ways of reading a search's list), longer searches, the position of an id deep
// in a list; what the search area (its triggers and the order index) costs the writes of 100,000 entries. The catalogs
// are generated (tests/support/catalog_generator.hpp). Timings taken while other work runs are
// informative; the gate measures on a quiet machine (docs/i2/step-9.md records the figures).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/query.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>

#include "catalog_db_test_support.hpp"
#include "support/catalog_generator.hpp"
#include "support/temp_dir.hpp"

using namespace rv::catalog;
using rv::catalog_db::QueryOptions;
using rv::catalog_db::SqliteCatalogQuery;
using Clock = std::chrono::steady_clock;

namespace {

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// The value below which `share` of `values` lie (nearest rank).
double percentile(std::vector<double> values, double share) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(share * static_cast<double>(values.size()) + 0.999999);
    return values[std::min(values.size(), std::max<std::size_t>(rank, 1)) - 1];
}

/// A generated catalog database in a temporary folder, opened, with queries on a reader.
struct BenchCatalog {
    BenchCatalog(std::string_view prefix, std::size_t entries) : dir(prefix) {
        const auto started = Clock::now();
        const auto generated = rv::test::generateCatalogDatabase(rv::test::databaseFileIn(dir),
                                                                 rv::test::CatalogGeneratorOptions{entries, 9}, false);
        REQUIRE(generated.has_value());
        generation = millisecondsSince(started);
        auto opened = rv::catalog_db::openCatalogDatabase(rv::test::databaseFileIn(dir));
        REQUIRE(opened.has_value());
        database = std::make_unique<rv::catalog_db::Database>(std::move(*opened));
        query = open();
        pages = database->writer().queryInt("PRAGMA page_count").value().value_or(0);
    }

    std::unique_ptr<SqliteCatalogQuery> open(const QueryOptions& options = {}) {
        auto opened = SqliteCatalogQuery::open(database->openReader().value(), options);
        REQUIRE(opened.has_value());
        return std::move(*opened);
    }

    rv::test::TempDir dir;
    std::unique_ptr<rv::catalog_db::Database> database;
    std::unique_ptr<SqliteCatalogQuery> query;
    double generation = 0.0;
    std::int64_t pages = 0;
};

/// The searches of the NFR-02 benchmark: words, prefixes of two to five characters, two and three words, years,
/// companies, system names, one character; with every order and some filters.
std::vector<CatalogQuery> nfr02Queries() {
    const std::vector<std::string> texts{
        "pixel",
        "quest",
        "eclair",
        "chateau",
        "zurich",
        "dragon",
        "galaxy",
        "robot",
        "super",
        "mega",
        "phantom",
        "neon",
        "lunar",
        "the",
        "of",
        "and",
        "pi",
        "su",
        "ch",
        "ga",
        "ro",
        "dr",
        "ne",
        "19",
        "pix",
        "sup",
        "cha",
        "gal",
        "rob",
        "dra",
        "198",
        "200",
        "pixe",
        "supe",
        "chat",
        "gala",
        "drago",
        "quest p",
        "super pi",
        "pixel q",
        "dragon ca",
        "the of",
        "neon samurai",
        "golden tower",
        "royal circus garden",
        "1985",
        "1999",
        "mirage",
        "editions",
        "kumo works",
        "studio brule",
        "nintendo",
        "sony play",
        "systeme fictif",
        "sega mega",
        "arcade",
        "europe",
        "espana",
        "japan",
        "role playing",
        "corsa guida",
        "shoot",
        "s",
        "p",
        "c",
        "e",
        "zz",
        "pixel zz",
        "super m",
    };
    std::vector<CatalogQuery> queries;
    for (const std::string& text : texts) {
        CatalogQuery query;
        query.text = text;
        queries.push_back(query);
        query.sort = SortOrder::System;
        queries.push_back(query);
        query.sort = SortOrder::ReleaseDate;
        query.includeUnavailable = true;
        queries.push_back(query);
        query.sort = SortOrder::Title;
        query.includeUnavailable = false;
        query.systems = {SystemName::parse("psx").value(), SystemName::parse("snes").value()};
        queries.push_back(query);
    }
    return queries;
}

} // namespace

TEST_CASE("catalog_db/bench: NFR-02, the p95 of indexed queries is at most 100 ms on 10,000 games", "[.bench]") {
    BenchCatalog catalog("rv-bench-nfr02", 10'000);
    const std::vector<CatalogQuery> queries = nfr02Queries();
    for (const CatalogQuery& query : queries) { // warm the page cache, as the app's reader is
        REQUIRE(catalog.query->run(query).has_value());
    }
    std::vector<double> times;
    std::size_t rows = 0;
    for (int pass = 0; pass < 3; ++pass) {
        for (const CatalogQuery& query : queries) {
            const auto started = Clock::now();
            const auto ids = catalog.query->run(query);
            times.push_back(millisecondsSince(started));
            REQUIRE(ids.has_value());
            rows += ids->size();
        }
    }
    const double p95 = percentile(times, 0.95);
    WARN(fmt::format("Informative timing (I2 step 9, NFR-02): 10,000 games generated and written in {:.0f} ms "
                     "({} pages of 4 KiB); {} queries materialized ({} rows in all): p50 {:.2f} ms, p95 {:.2f} ms, "
                     "max {:.2f} ms",
                     catalog.generation, catalog.pages, times.size(), rows, percentile(times, 0.5), p95,
                     percentile(times, 1.0)));
    CHECK(p95 <= 100.0);
}

TEST_CASE("catalog_db/bench: a UI page of 30 rows fetched by ids within 5 ms on 100,000 games; lists materialized",
          "[.bench]") {
    BenchCatalog catalog("rv-bench-page", 100'000);
    std::string report = fmt::format("Informative timings (I2 step 9) on 100,000 entries, generated and written in "
                                     "{:.0f} ms ({} pages of 4 KiB):\n",
                                     catalog.generation, catalog.pages);

    // Materializations, each run 5 times after a first that warms the page cache, the median and the slowest
    // reported. "read": a commit of the writer before each run makes the queries read the list again (as after an
    // import); "kept": the list without the search is kept from the run before, and narrowed.
    rv::catalog_db::Connection& writer = catalog.database->writer();
    int commits = 0;
    const auto commit = [&] {
        REQUIRE(writer
                    .execute(fmt::format("INSERT INTO rv_meta (key, value) VALUES ('bench', '{0}') "
                                         "ON CONFLICT (key) DO UPDATE SET value = '{0}'",
                                         ++commits))
                    .has_value());
    };
    const auto timeRun = [&](std::string_view label, SqliteCatalogQuery& query, const CatalogQuery& request,
                             bool read) {
        std::vector<double> times;
        std::size_t count = 0;
        for (int run = 0; run < 6; ++run) {
            if (read) {
                commit();
            }
            const auto started = Clock::now();
            const auto ids = query.run(request);
            const double elapsed = millisecondsSince(started);
            REQUIRE(ids.has_value());
            count = ids->size();
            if (run > 0) {
                times.push_back(elapsed);
            }
        }
        report += fmt::format("  {:<60} {:>6} ids  median {:7.2f} ms  max {:7.2f} ms\n", label, count,
                              percentile(times, 0.5), percentile(times, 1.0));
    };
    const auto request = [](std::string text, SortOrder sort = SortOrder::Title, bool includeUnavailable = false) {
        CatalogQuery query;
        query.text = std::move(text);
        query.sort = sort;
        query.includeUnavailable = includeUnavailable;
        return query;
    };
    SqliteCatalogQuery& query = *catalog.query;
    CatalogQuery onePlatform = request("");
    onePlatform.systems = {SystemName::parse("psx").value()};
    for (const bool read : {true, false}) {
        const std::string_view how = read ? "read" : "kept";
        timeRun(fmt::format("every game, Title, {}", how), query, request(""), read);
        timeRun(fmt::format("every game, System, {}", how), query, request("", SortOrder::System), read);
        timeRun(fmt::format("every game, ReleaseDate, {}", how), query, request("", SortOrder::ReleaseDate), read);
        timeRun(fmt::format("every entry (unavailable included), Title, {}", how), query,
                request("", SortOrder::Title, true), read);
        timeRun(fmt::format("one system (psx), Title, {}", how), query, onePlatform, read);
    }
    timeRun("one letter 's' (no FTS5)", query, request("s"), true);
    timeRun("one letter 'p' (no FTS5), System", query, request("p", SortOrder::System), true);
    const auto lookUpAll = catalog.open(QueryOptions{std::numeric_limits<std::size_t>::max()});
    const auto readAll = catalog.open(QueryOptions{0});
    for (const std::string_view text :
         {"su", "pi", "ch", "19", "an", "sup", "pixel", "drago", "super pi", "neon samurai", "super m", "pixel zz"}) {
        const CatalogQuery search = request(std::string(text));
        timeRun(fmt::format("'{}' after a commit (default)", text), query, search, true);
        timeRun(fmt::format("'{}' after a commit, rows looked up", text), *lookUpAll, search, true);
        timeRun(fmt::format("'{}' after a commit, list read and narrowed", text), *readAll, search, true);
        REQUIRE(readAll->run(CatalogQuery{}).has_value()); // the list without the search, kept
        timeRun(fmt::format("'{}', kept list narrowed", text), *readAll, search, false);
    }
    {
        std::vector<double> times;
        for (int run = 0; run < 5; ++run) {
            const auto started = Clock::now();
            REQUIRE(query.systems().has_value());
            times.push_back(millisecondsSince(started));
        }
        report += fmt::format("  {:<52} median {:7.2f} ms\n", "systems() with their counts", percentile(times, 0.5));
    }

    // The position of an id deep in the list (the last), as a view finds its selection again.
    const std::vector<GameId> list = query.run(CatalogQuery{}).value();
    REQUIRE(list.size() > 50'000);
    {
        std::vector<double> times;
        for (int run = 0; run < 20; ++run) {
            const auto started = Clock::now();
            const auto position = positionOf(list, list.back());
            times.push_back(millisecondsSince(started));
            REQUIRE(position == list.size() - 1);
        }
        report += fmt::format("  {:<52} median {:7.3f} ms\n",
                              fmt::format("position of the last of {} ids", list.size()), percentile(times, 0.5));
    }

    // UI pages: 30 rows fetched by ids, at the start, at the end, and at 500 positions spread over the list.
    std::vector<double> pages;
    std::vector<std::size_t> offsets{0, list.size() - 30, list.size() / 2};
    for (std::size_t i = 0; i < 500; ++i) {
        offsets.push_back((i * 7'919 * 13) % (list.size() - 30));
    }
    for (const std::size_t offset : offsets) { // warm
        REQUIRE(query.fetch(pageOf(list, offset, 30)).has_value());
    }
    for (const std::size_t offset : offsets) {
        const auto started = Clock::now();
        const auto rows = query.fetch(pageOf(list, offset, 30));
        pages.push_back(millisecondsSince(started));
        REQUIRE(rows.has_value());
        REQUIRE(rows->size() == 30);
    }
    // A cold page: another reader, its cache empty.
    const auto cold = catalog.open();
    const auto coldStarted = Clock::now();
    REQUIRE(cold->fetch(pageOf(list, list.size() - 30, 30)).has_value());
    const double coldPage = millisecondsSince(coldStarted);
    const double p95 = percentile(pages, 0.95);
    report += fmt::format("  UI page, 30 rows by ids ({} pages): p50 {:.3f} ms, p95 {:.3f} ms, max {:.3f} ms; "
                          "first page of a new reader {:.3f} ms\n",
                          pages.size(), percentile(pages, 0.5), p95, percentile(pages, 1.0), coldPage);

    // Sizes: what a copy of the database loses without the order index, then without the search index.
    const std::filesystem::path copy = catalog.dir.file("sizes/copy.sqlite");
    REQUIRE(catalog.database->backupTo(copy).has_value());
    {
        auto copied = rv::catalog_db::Database::open(copy);
        REQUIRE(copied.has_value());
        rv::catalog_db::Connection& copyWriter = copied->writer();
        const std::int64_t whole = copyWriter.queryInt("PRAGMA page_count").value().value_or(0);
        REQUIRE(copyWriter.execute("DROP INDEX idx_games_sort; VACUUM").has_value());
        const std::int64_t withoutOrder = copyWriter.queryInt("PRAGMA page_count").value().value_or(0);
        REQUIRE(copyWriter.execute("DROP TABLE idx_game_search; VACUUM").has_value());
        const std::int64_t withoutSearch = copyWriter.queryInt("PRAGMA page_count").value().value_or(0);
        report += fmt::format("  sizes: database {} pages of 4 KiB (vacuumed), order index {} pages, search index {} "
                              "pages\n",
                              whole, whole - withoutOrder, withoutOrder - withoutSearch);
    }
    WARN(report);
    CHECK(p95 <= 5.0);
}

TEST_CASE("catalog_db/bench: what the search area costs the writes of 100,000 entries (informative)", "[.bench]") {
    const rv::test::TempDir dir("rv-bench-writes");
    const rv::test::CatalogGeneratorOptions options{100'000, 9};

    // The entries alone, generated twice as generateCatalogDatabase() does (GameId order, then the writes).
    auto started = Clock::now();
    std::size_t made = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < options.entries; ++i) {
            made += rv::test::generatedEntry(options, i).sortKey.empty() ? 0 : 1;
        }
    }
    const double generation = millisecondsSince(started);
    REQUIRE(made == 2 * options.entries);

    // The same writes into a database with the search area, and into ones without parts of it.
    constexpr std::string_view kDropTriggers =
        "DROP TRIGGER idx_games_search_insert; DROP TRIGGER idx_games_search_delete; "
        "DROP TRIGGER idx_games_search_update; DROP TRIGGER idx_systems_search_insert; "
        "DROP TRIGGER idx_systems_search_delete; DROP TRIGGER idx_systems_search_update;";
    struct Variant {
        std::string_view label;
        std::string setup;
    };
    const std::vector<Variant> variants{
        {"with the search area", ""},
        {"without the search triggers", std::string(kDropTriggers)},
        {"without the order index", "DROP INDEX idx_games_sort"},
        {"without either", std::string(kDropTriggers) + " DROP INDEX idx_games_sort"},
    };
    std::string report = fmt::format("Informative timings (I2 step 9): 100,000 entries generated twice in {:.0f} ms; "
                                     "generated and written in one transaction:\n",
                                     generation);
    for (std::size_t i = 0; i < variants.size(); ++i) {
        const std::filesystem::path file = dir.file(fmt::format("v{}/retroverse.sqlite", i));
        if (!variants[i].setup.empty()) {
            auto database = rv::catalog_db::openCatalogDatabase(file);
            REQUIRE(database.has_value());
            REQUIRE(database->writer().execute(variants[i].setup).has_value());
        }
        started = Clock::now();
        REQUIRE(rv::test::generateCatalogDatabase(file, options, false).has_value());
        const double elapsed = millisecondsSince(started);
        auto database = rv::catalog_db::Database::open(file);
        REQUIRE(database.has_value());
        report += fmt::format("  {:<44} {:7.0f} ms, {} pages\n", variants[i].label, elapsed,
                              database->writer().queryInt("PRAGMA page_count").value().value_or(0));
    }
    WARN(report);
}
