// NFR-02 on the real library's catalog (I2 step 14, Gate M1). Hidden ([.real-library]): ctest never runs it;
// scripts/real-library.ps1 does (Release by default), with ES-DE closed, read-only snapshots of the library before and
// after, and the folders in the RV_REAL_* variables (test_import_real.cpp lists them). The library is imported into a
// new database under the build tree (build/<preset>/real-library/query), then the searches of the benchmark's kind
// (words, prefixes of one to five characters, two and three words, years, companies, system names, words found nowhere)
// run with every order and a system filter, on a reader as the app's catalog worker runs them, three passes after one
// that warms the page cache: the p95 of the indexed queries must stay within 100 ms (NFR-02, specification §18.1, which
// asks it of 10,000 games; the reference library has 90,246). Pages of 30 rows fetched by ids at spread positions of
// the default list must stay within 5 ms at p95 (ADR-0008 §5). The figures are printed for scripts/gate.ps1 -Gate M1
// -RealLibrary, which reads the line starting "NFR-02 on the real catalog:".

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <windows.h>

#include <fmt/format.h>

#include <rv/catalog/import_service.hpp>
#include <rv/catalog/query.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>

#include "import_test_support.hpp"
#include "support/fixture.hpp"

namespace fs = std::filesystem;
using namespace rv::catalog;
using Clock = std::chrono::steady_clock;

namespace {

std::optional<fs::path> pathVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return fs::path(value);
}

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// The value below which `share` of `values` lie (nearest rank), as the benchmark of step 9 computes it.
double percentile(std::vector<double> values, double share) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(share * static_cast<double>(values.size()) + 0.999999);
    return values[std::min(values.size(), std::max<std::size_t>(rank, 1)) - 1];
}

/// Searches of the kinds the NFR-02 benchmark runs on its generated catalog, in words a game collection holds: whole
/// words, prefixes, several words, years, companies, system names, one character, words that match nothing.
std::vector<CatalogQuery> realQueries() {
    const std::vector<std::string> texts{
        "super",   "mario",     "zelda",          "sonic",         "dragon",       "world",      "soccer",
        "racing",  "puzzle",    "the",            "of",            "and",          "s",          "m",
        "p",       "su",        "ma",             "dr",            "wo",           "sup",        "mar",
        "dra",     "worl",      "socce",          "final fantasy", "street fight", "super mario world",
        "metal g", "1990",      "1995",           "199",           "nintendo",     "sega",       "capcom",
        "konami",  "namco",     "sony play",      "game boy",      "arcade",       "europe",     "japan",
        "usa",     "rev",       "beta",           "zz",            "xyzzy",        "super zz",
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

TEST_CASE("catalog_db/query: NFR-02 on the real library's catalog: indexed queries and pages",
          "[.real-library][real-library][query]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!home || !install || !romRoot || !mediaRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT "
             "(scripts/real-library.ps1 does)");
    }
    LibraryProfile profile;
    profile.esdeHome = *home;
    profile.installFolder = *install;
    profile.romRoot = *romRoot;
    profile.mediaRoot = *mediaRoot;

    // A new database under the build tree, never in the library, with the real library imported as retroverse-cli
    // imports it.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "query";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    rv::test::ImportCatalog catalog = rv::test::ImportCatalog::open(folder / "retroverse.sqlite");
    {
        rv::esde::EsdeLibrarySource source;
        const auto settings = rv::esde::readSettings(profile.esdeHome);
        REQUIRE(settings.has_value());
        rv::esde::EsdeMediaResolver media(rv::esde::resolveMediaRoot(*settings, profile));
        const ImportResult imported = rv::test::importOk(catalog, source, media, profile);
        REQUIRE(imported.statistics.entries > 10'000);
    }

    // The queries on a reader of their own, as the catalog worker runs them.
    auto opened = rv::catalog_db::SqliteCatalogQuery::open(catalog.database->openReader().value());
    REQUIRE(opened.has_value());
    rv::catalog_db::SqliteCatalogQuery& query = **opened;
    const std::vector<CatalogQuery> queries = realQueries();
    for (const CatalogQuery& warm : queries) { // warm the page cache, as the app's reader is
        REQUIRE(query.run(warm).has_value());
    }
    std::vector<double> times;
    std::vector<double> slowest(queries.size(), 0.0); // per query, its slowest pass
    std::vector<std::size_t> found(queries.size(), 0);
    std::size_t rows = 0;
    for (int pass = 0; pass < 3; ++pass) {
        for (std::size_t i = 0; i < queries.size(); ++i) {
            const auto started = Clock::now();
            const auto ids = query.run(queries[i]);
            const double ms = millisecondsSince(started);
            times.push_back(ms);
            slowest[i] = std::max(slowest[i], ms);
            REQUIRE(ids.has_value());
            rows += ids->size();
            found[i] = ids->size();
        }
    }
    const double p95 = percentile(times, 0.95);
    // The five slowest queries, for the record: what costs most on a real catalog.
    std::vector<std::size_t> order(queries.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return slowest[a] > slowest[b]; });
    for (std::size_t k = 0; k < std::min<std::size_t>(5, order.size()); ++k) {
        const CatalogQuery& slow = queries[order[k]];
        std::cout << fmt::format("  slow: \"{}\" ({}{}{}): {} games, {:.1f} ms at most\n", slow.text,
                                 toString(slow.sort), slow.systems.empty() ? "" : ", psx and snes",
                                 slow.includeUnavailable ? ", with the unavailable" : "", found[order[k]],
                                 slowest[order[k]]);
    }

    // The default list (every game, title order), then pages of 30 rows at 200 spread positions of it.
    const auto listed = Clock::now();
    const auto all = query.run(CatalogQuery{});
    const double listMs = millisecondsSince(listed);
    REQUIRE(all.has_value());
    REQUIRE(all->size() > 30);
    std::vector<double> pages;
    constexpr std::size_t kPage = 30;
    constexpr std::size_t kPages = 200;
    for (std::size_t i = 0; i < kPages; ++i) {
        const std::size_t at = (all->size() - kPage) * i / (kPages - 1);
        const auto started = Clock::now();
        const auto fetched = query.fetch(std::span<const GameId>(all->data() + at, kPage));
        pages.push_back(millisecondsSince(started));
        REQUIRE(fetched.has_value());
        CHECK(fetched->size() == kPage);
    }
    const double pageP95 = percentile(pages, 0.95);

    std::cout << fmt::format("NFR-02 on the real catalog: {} games listed, {} indexed queries ({} rows in all): p50 "
                             "{:.2f} ms, p95 {:.2f} ms, max {:.2f} ms; the default list {:.1f} ms; {} pages of {} "
                             "rows: p50 {:.3f} ms, p95 {:.3f} ms, max {:.3f} ms\n",
                             all->size(), times.size(), rows, percentile(times, 0.5), p95, percentile(times, 1.0),
                             listMs, pages.size(), kPage, percentile(pages, 0.5), pageP95, percentile(pages, 1.0));
    CHECK(p95 <= 100.0);   // NFR-02
    CHECK(pageP95 <= 5.0); // ADR-0008 §5's page budget
}
