// The real library imported (I2 step 11a, ADR-0008 §10). Hidden ([.real-library]): ctest never runs it;
// scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the library before and after, and the
// folders in environment variables:
//   RV_REAL_ESDE_HOME     the ES-DE home (settings, gamelists)
//   RV_REAL_ESDE_INSTALL  the ES-DE install folder (es_systems.xml, the MAME lists)
//   RV_REAL_ROM_ROOT      ES-DE's ROMDirectory, given as the library's ROM root (the same folder)
//   RV_REAL_MEDIA_ROOT    ES-DE's MediaDirectory, given as the library's media root (the same folder)
// The database goes under the build tree (build/<preset>/real-library/import, made anew each run). The expected figures
// are step 7's (the census of 2026-10-17): 105,130 entries (90,246 games, 12,821 disc members, 1,038 missing files, 870
// not games, 155 folders) and 2,513 disc groups. The first import, the second (0 identity changes, 0 entry rows
// written), `db rebuild-index` (every sheet read again, the same index) and the ROM root unplugged and back (rows kept
// as source unavailable, the disc groups untouched, then the same index) are timed: informative, the gate times the
// import on a quiet machine.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>

#include <windows.h>

#include <fmt/format.h>

#include <rv/catalog/import_service.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>

#include "import_test_support.hpp"
#include "support/fixture.hpp"

namespace fs = std::filesystem;
using namespace rv::catalog;

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

double seconds(std::chrono::nanoseconds time) {
    return std::chrono::duration<double>(time).count();
}

/// An import as retroverse-cli makes one (its source and resolver fresh: nothing kept from an earlier run).
ImportResult importLibrary(rv::test::ImportCatalog& catalog, const LibraryProfile& profile,
                           const ImportOptions& options = {}) {
    rv::esde::EsdeLibrarySource source;
    const auto settings = rv::esde::readSettings(profile.esdeHome);
    REQUIRE(settings.has_value());
    rv::esde::EsdeMediaResolver media(rv::esde::resolveMediaRoot(*settings, profile));
    ImportResult result = rv::test::importOk(catalog, source, media, profile, options);
    const rv::esde::EsdeLoadStatistics& load = source.lastStatistics();
    const ImportStatistics& statistics = result.statistics;
    std::cout << fmt::format("{}: {:.2f} s (library read {:.2f} s, of it sheets {:.2f} s, {} read, {} from the "
                             "catalog; media {:.2f} s; comparison {:.2f} s; writes and commit {:.2f} s): {} identity "
                             "rows, {} entry rows written, {} removed, {} disc groups written; search index rebuilt: "
                             "{}\n",
                             options.rebuildIndex ? "db rebuild-index" : "import", seconds(statistics.totalTime),
                             seconds(statistics.loadTime), seconds(load.sheetTime), load.sheetsRead,
                             load.sheetsFromCache, seconds(statistics.mediaTime), seconds(statistics.compareTime),
                             seconds(statistics.writeTime), statistics.identitiesWritten, statistics.entriesWritten,
                             statistics.entriesRemoved, statistics.discGroupsWritten, statistics.searchIndexRebuilt);
    return result;
}

std::int64_t count(rv::catalog_db::Connection& connection, const std::string& sql) {
    const auto value = connection.queryInt(sql);
    REQUIRE(value.has_value());
    return value->value_or(0);
}

} // namespace

TEST_CASE("catalog_db/import: the real library imported, imported again and its index rebuilt",
          "[.real-library][real-library][import]") {
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

    // A new database under the build tree, never in the library.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "import";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    rv::test::ImportCatalog catalog = rv::test::ImportCatalog::open(folder / "retroverse.sqlite");
    rv::catalog_db::Connection& writer = catalog.database->writer();

    // The first import: step 7's figures, every entry indexed under its identity.
    const ImportResult first = importLibrary(catalog, profile);
    CHECK(first.changes.firstImport);
    CHECK(first.statistics.searchIndexRebuilt);
    CHECK(first.statistics.entries == 105'130);
    CHECK(first.changes.total().newIdentities == 105'130);
    CHECK(first.changes.total().added == 105'130);
    CHECK(count(writer, "SELECT count(*) FROM rv_game_identities") == 105'130);
    CHECK(count(writer, "SELECT count(*) FROM idx_games") == 105'130);
    const std::map<std::string, std::int64_t> expected{{"game", 90'246},      {"disc-member", 12'821},
                                                       {"missing-file", 1'038}, {"not-a-game", 870},
                                                       {"folder", 155}};
    for (const auto& [state, number] : expected) {
        INFO(state);
        CHECK(count(writer, fmt::format("SELECT count(*) FROM idx_games WHERE state = '{}'", state)) == number);
    }
    CHECK(count(writer, "SELECT count(*) FROM idx_disc_groups") == 2'513);
    CHECK(count(writer, "SELECT count(*) FROM idx_games WHERE availability = 'source-unavailable'") == 0);
    std::cout << fmt::format("Media rows: {}; disc members {}, references {}; database {} pages of {} bytes\n",
                             count(writer, "SELECT count(*) FROM idx_media"),
                             count(writer, "SELECT count(*) FROM idx_disc_members"),
                             count(writer, "SELECT count(*) FROM idx_disc_references"),
                             count(writer, "PRAGMA page_count"), count(writer, "PRAGMA page_size"));

    // The second import: no identity changes, no entry row written, the sheets taken from the catalog.
    const ImportResult second = importLibrary(catalog, profile);
    CHECK_FALSE(second.changes.firstImport);
    CHECK(second.statistics.identitiesWritten == 0);
    CHECK(second.statistics.entriesWritten == 0);
    CHECK(second.statistics.entriesRemoved == 0);
    CHECK(second.statistics.discGroupsWritten == 0);
    CHECK(second.changes.total().newIdentities == 0);
    CHECK(second.changes.total().unchanged == 105'130);
    CHECK(second.changes.empty());

    // db rebuild-index: the index the refresh left, the state untouched.
    const auto index = rv::test::digestOf(rv::test::dumpIndex(writer));
    const auto state = rv::test::digestOf(rv::test::dumpState(writer));
    ImportOptions rebuild;
    rebuild.rebuildIndex = true;
    const ImportResult rebuilt = importLibrary(catalog, profile, rebuild);
    CHECK(rebuilt.statistics.entriesWritten == 105'130);
    CHECK(rebuilt.statistics.identitiesWritten == 0);
    CHECK(rv::test::digestOf(rv::test::dumpIndex(writer)) == index);
    CHECK(rv::test::digestOf(rv::test::dumpState(writer)) == state);
    for (const auto& [table, digest] : index) {
        std::cout << fmt::format("  {}: {}\n", table, digest);
    }
    CHECK(rebuilt.statistics.discGroupsWritten == 2'513); // every sheet read again, the same groups

    // The ROM root unplugged (an empty folder named in its place): every row kept as source unavailable, nothing
    // removed, the disc groups as they were (their heads cannot be listed); back, the index as it was.
    LibraryProfile unplugged = profile;
    unplugged.romRoot = folder / "empty-rom-root";
    fs::create_directories(*unplugged.romRoot);
    const ImportResult away = importLibrary(catalog, unplugged);
    CHECK(away.changes.total().removed == 0);
    CHECK(away.changes.total().newIdentities == 0);
    CHECK(away.changes.total().unavailable + away.changes.total().unchanged == 105'130); // (folders outside the root)
    CHECK(away.statistics.identitiesWritten == 0);
    CHECK(away.statistics.discGroupsWritten == 0);
    CHECK(away.statistics.discGroupsRemoved == 0);
    const auto awayIndex = rv::test::digestOf(rv::test::dumpIndex(writer));
    for (const char* table : {"idx_disc_groups", "idx_disc_members", "idx_disc_references"}) {
        INFO(table);
        CHECK(awayIndex.at(table) == index.at(table));
    }
    const ImportResult back = importLibrary(catalog, profile);
    CHECK(back.changes.total().newIdentities == 0);
    CHECK(back.changes.total().removed == 0);
    CHECK(back.statistics.discGroupsWritten == 0);
    CHECK(rv::test::digestOf(rv::test::dumpIndex(writer)) == index);
    CHECK(rv::test::digestOf(rv::test::dumpState(writer)) == state);
    CHECK(count(writer, "SELECT count(*) FROM rv_imports") == 5);
    CHECK(rv::catalog_db::checkDatabase(*catalog.database).value().passed());
}
