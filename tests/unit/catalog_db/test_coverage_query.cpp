// The media coverage of the catalog's own rows (I7 step 8, ADR-0018 §14): SqliteCatalogQuery::mediaCoverage() after an
// import of a fixture equals catalog::measureCoverage()'s Games and ScrapedGames populations over the snapshot that
// import read, with the same media resolver, system by system and in all; the import issues and the quick hashes'
// counts read back.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <rv/catalog/coverage.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>

#include "../esde/snapshot_test_support.hpp"
#include "import_test_support.hpp"
#include "support/temp_dir.hpp"

using rv::catalog::CoveragePopulation;

namespace {

/// `fixture` imported into a fresh catalog under `dir`; the coverage of the snapshot the import read, measured with the
/// same resolver, and the catalog's.
struct Measured {
    rv::catalog::MediaCoverage snapshot;
    rv::catalog::MediaCoverage catalog;
    std::vector<rv::catalog::Issue> issues;
    std::vector<rv::catalog::Issue> stored;
    rv::catalog_db::QuickHashCounts hashes;
    rv::catalog::QuickHashPassResult pass;
    rv::catalog_db::QuickHashCounts hashed;
};

Measured measure(std::string_view fixtureName) {
    const rv::test::Fixture fixture = rv::test::esde::materialize(fixtureName);
    const rv::catalog::LibraryProfile profile = rv::test::esde::profileOf(fixture);
    const rv::test::TempDir dir("rv-coverage-query");
    rv::test::ImportCatalog catalog = rv::test::ImportCatalog::open(dir.file("catalog.sqlite"));

    rv::esde::EsdeLibrarySourceOptions sourceOptions;
    sourceOptions.gamelistRetryDelay = std::chrono::milliseconds(0);
    rv::esde::EsdeLibrarySource source(sourceOptions);
    const auto settings = rv::esde::readSettings(profile.esdeHome);
    REQUIRE(settings.has_value());
    rv::esde::EsdeMediaResolver media(rv::esde::resolveMediaRoot(*settings, profile));
    const rv::catalog::ImportResult imported = rv::test::importOk(catalog, source, media, profile);

    // The snapshot as the import read it, measured as `retroverse-cli media --coverage` does.
    const rv::catalog::LibrarySnapshot snapshot = rv::test::esde::loadSnapshot(source, profile);
    media.refresh();
    auto measured = rv::catalog::measureCoverage(snapshot, media);
    REQUIRE(measured.has_value());

    auto reader = catalog.database->openReader();
    REQUIRE(reader.has_value());
    auto query = rv::catalog_db::SqliteCatalogQuery::open(std::move(*reader));
    REQUIRE(query.has_value());
    auto coverage = (*query)->mediaCoverage(profile.id);
    REQUIRE(coverage.has_value());
    auto issues = (*query)->importIssues(profile.id);
    REQUIRE(issues.has_value());
    auto hashes = (*query)->quickHashCounts(profile.id);
    REQUIRE(hashes.has_value());
    // The quick-hash pass, then the counts again: every present file hashed.
    auto pass = rv::catalog::runQuickHashPass(*catalog.store, profile.id);
    REQUIRE(pass.has_value());
    auto hashed = (*query)->quickHashCounts(profile.id);
    REQUIRE(hashed.has_value());
    Measured result{std::move(*measured), std::move(*coverage), imported.changes.issues, std::move(*issues),
                    *hashes, std::move(*pass), *hashed};
    query->reset();
    catalog.close();
    return result;
}

/// The catalog's coverage holds the snapshot's two populations, system by system and in all.
void checkSame(const Measured& measured) {
    for (const CoveragePopulation population : {CoveragePopulation::Games, CoveragePopulation::ScrapedGames}) {
        INFO(rv::catalog::toString(population));
        CHECK(measured.catalog.total[population] == measured.snapshot.total[population]);
        for (const rv::catalog::SystemCoverage& system : measured.snapshot.systems) {
            if (system[population].entries == 0) {
                continue;
            }
            INFO(system.system.str());
            const rv::catalog::SystemCoverage* stored = measured.catalog.find(system.system);
            REQUIRE(stored != nullptr);
            CHECK((*stored)[population] == system[population]);
        }
        for (const rv::catalog::SystemCoverage& system : measured.catalog.systems) {
            if (system[population].entries > 0) {
                INFO(system.system.str());
                CHECK(measured.snapshot.find(system.system) != nullptr);
            }
        }
    }
    // The rows hold no other population: the index has no gamelist entry outside its system folder.
    CHECK(measured.catalog.total[CoveragePopulation::GamelistEntries].entries == 0);
    CHECK(measured.catalog.total[CoveragePopulation::GamelistEntriesWithFile].entries == 0);
    CHECK(measured.catalog.issues.empty());
}

} // namespace

TEST_CASE("catalog_db/coverage: mediaCoverage() equals measureCoverage()'s two populations on the fixtures",
          "[catalog_db][coverage]") {
    for (const std::string_view name : {"esde/standard", "esde/snes-room", "esde/building", "esde/video-verdicts"}) {
        INFO(name);
        const Measured measured = measure(name);
        REQUIRE(measured.snapshot.total[CoveragePopulation::Games].entries > 0);
        checkSame(measured);
        CHECK(measured.stored == measured.issues);
        CHECK(measured.hashes.current == 0); // a first import reads no ROM byte
        CHECK(measured.hashes.files > 0);
        CHECK(measured.pass.hashed == measured.hashes.files);
        CHECK(measured.hashed.current == measured.hashes.files);
        CHECK(measured.hashed.missing() == 0);
    }
}

TEST_CASE("catalog_db/coverage: the standard fixture's figures as its README counts them", "[catalog_db][coverage]") {
    const Measured measured = measure("esde/standard");
    const rv::catalog::CoverageCounts& games = measured.catalog.total[CoveragePopulation::Games];
    const rv::catalog::CoverageCounts& scraped = measured.catalog.total[CoveragePopulation::ScrapedGames];
    // Every Game entry of the README's 23 entries, and those a gamelist names.
    CHECK(games.entries == 15);
    CHECK(scraped.entries == 8);
    CHECK(games.unknown == 0);
    CHECK(games.withAny <= games.entries);
    CHECK(measured.catalog.find(rv::test::systemName("psx")) != nullptr);
}
