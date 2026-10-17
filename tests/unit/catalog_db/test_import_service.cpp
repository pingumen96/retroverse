// The import (I2 step 11a, catalog::ImportService) on the SQLite store, with snapshots and media scripted by the test:
// identities given out on first sight (collisions included), matched on the exact path then the key, keys computed
// again; a second import that writes nothing; vanished identities and files that come back; media kept when presence()
// cannot know; failed gamelists (kept rows, metadata pending); folders and systems that cannot be listed; the
// mass-disappearance guard; systems no longer configured; the seam for reconciliation (I2 step 11b); cancellation;
// one import at a time; partial imports; disc groups and their references; `db rebuild-index` against a refresh; the
// search index of a bulk write against the triggers'; the library's id; a new ROM root.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/identity.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/catalog_db/search_index.hpp>

#include "import_test_support.hpp"
#include "support/temp_dir.hpp"

using namespace rv::catalog;
using rv::catalog_db::SearchIndexPolicy;
using rv::catalog_db::StoreOptions;
using rv::test::changesOf;
using rv::test::diskGame;
using rv::test::ImportCatalog;
using rv::test::importOk;
using rv::test::lookupKey;
using rv::test::missingGame;
using rv::test::ScriptedMedia;
using rv::test::ScriptedSource;
using rv::test::scriptedSystem;
using rv::test::systemName;
using rv::test::unlistedGame;

namespace {

/// The GameId a first import gives `relativePath` of `system` (counter 0 unless a collision).
GameId firstId(const LibraryProfile& profile, std::string_view system, std::string_view relativePath,
               std::uint32_t counter = 0) {
    const auto id = stableGameId(profile.id, systemName(system), lookupKey(relativePath).key(), counter);
    REQUIRE(id.has_value());
    return *id;
}

/// The identity of `relativePath` (its exact path), REQUIREd.
StoredIdentity identityAt(ImportCatalog& catalog, const LibraryProfile& profile, std::string_view system,
                          std::string_view relativePath) {
    const auto identities = catalog.store->identities(profile.id, systemName(system));
    REQUIRE(identities.has_value());
    const auto found = std::find_if(identities->begin(), identities->end(), [&](const StoredIdentity& identity) {
        return identity.key.relativePath() == relativePath;
    });
    INFO(relativePath);
    REQUIRE(found != identities->end());
    return *found;
}

/// An index row found, or not: -> and * REQUIRE it.
struct FoundRow {
    std::optional<IndexedEntry> row;
    const IndexedEntry* operator->() const {
        REQUIRE(row.has_value());
        return &*row;
    }
    [[nodiscard]] bool has_value() const noexcept { return row.has_value(); }
};

/// The index rows of a system (a vector of its own: a loop over a member of a temporary Expected would outlive it).
std::vector<IndexedEntry> rowsOf(ImportCatalog& catalog, const LibraryProfile& profile, std::string_view system) {
    auto rows = catalog.store->indexedEntries(profile.id, systemName(system));
    REQUIRE(rows.has_value());
    return std::move(*rows);
}

/// The index row of `relativePath`.
FoundRow rowAt(ImportCatalog& catalog, const LibraryProfile& profile, std::string_view system,
               std::string_view relativePath) {
    for (IndexedEntry& row : rowsOf(catalog, profile, system)) {
        if (row.entry.key.relativePath() == relativePath) {
            return FoundRow{std::move(row)};
        }
    }
    return FoundRow{};
}

std::size_t rowCount(ImportCatalog& catalog, const LibraryProfile& profile, std::string_view system) {
    const auto rows = catalog.store->indexedEntries(profile.id, systemName(system));
    REQUIRE(rows.has_value());
    return rows->size();
}

EntryMedia coverPng() {
    EntryMedia media;
    media[MediaKind::Cover] = MediaPresence{true, ".png"};
    return media;
}

bool hasIssue(const ChangeSet& changes, std::string_view code) {
    return std::any_of(changes.issues.begin(), changes.issues.end(),
                       [&](const Issue& issue) { return issue.code == code; });
}

/// A source with gb and snes and a few entries of each, sorted.
ScriptedSource twoSystems() {
    ScriptedSource source;
    source.snapshot.systems = {scriptedSystem("gb"), scriptedSystem("snes")};
    source.snapshot.entries = {diskGame("gb", "Alpha (USA).gb", 100), diskGame("gb", "Beta (USA).gb", 200),
                               diskGame("gb", "Sub/Gamma (USA).gb", 300), diskGame("snes", "Zed (Europe).sfc", 400)};
    rv::test::sortEntries(source.snapshot);
    return source;
}

} // namespace

TEST_CASE("catalog_db/import: a first import gives every entry with a key an identity and an index row",
          "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-first");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source;
    source.snapshot.systems = {scriptedSystem("gb"), scriptedSystem("snes")};
    CatalogEntry outside = missingGame("gb", "Wrong Way.gb", "Wrong Way");
    outside.state = EntryState::OutsideSystemFolder;
    outside.availability = Availability::Available;
    outside.key = LookupKey();
    outside.gamelistPath = "./../snes/Wrong Way.sfc";
    source.snapshot.entries = {diskGame("gb", "Alpha (USA).gb"), diskGame("gb", "CASE.gb"), diskGame("gb", "case.gb"),
                               missingGame("gb", "Gone (USA).gb", "Gone Fishing"), outside,
                               diskGame("snes", "Zed (Europe).sfc")};
    rv::test::sortEntries(source.snapshot);
    ScriptedMedia media;
    media.media["gb/Alpha (USA).gb"] = coverPng();

    const ImportResult first = importOk(catalog, source, media, profile);
    CHECK(first.changes.firstImport);
    const SystemChanges& gb = changesOf(first.changes, "gb");
    CHECK(gb.outcome == SystemOutcome::Applied);
    CHECK(gb.added == 4); // the entry outside the system folder has no key: never indexed
    CHECK(gb.newIdentities == 4);
    CHECK(changesOf(first.changes, "snes").added == 1);
    CHECK(first.statistics.identitiesWritten == 5);
    CHECK(first.statistics.entriesWritten == 5);
    CHECK(first.statistics.searchIndexRebuilt); // a first import suspends the search index
    CHECK(first.changes.added.size() == 5);

    // GameIds as their tuples derive them; the two names that fold to one key: the first in (key, path) order keeps
    // its tuple's value, the next one takes counter 1, and the import says so.
    CHECK(identityAt(catalog, profile, "gb", "Alpha (USA).gb").id == firstId(profile, "gb", "Alpha (USA).gb"));
    CHECK(identityAt(catalog, profile, "gb", "CASE.gb").id == firstId(profile, "gb", "CASE.gb"));
    CHECK(identityAt(catalog, profile, "gb", "case.gb").id == firstId(profile, "gb", "case.gb", 1));
    CHECK(hasIssue(first.changes, kImportIssueKeyCollision));
    const StoredIdentity gone = identityAt(catalog, profile, "gb", "Gone (USA).gb");
    CHECK_FALSE(gone.present);
    CHECK_FALSE(gone.file.has_value());
    CHECK(identityAt(catalog, profile, "gb", "Alpha (USA).gb").present);
    CHECK(identityAt(catalog, profile, "gb", "Alpha (USA).gb").keyVersion == currentLookupKeyVersion());
    const auto alpha = rowAt(catalog, profile, "gb", "Alpha (USA).gb");
    REQUIRE(alpha.has_value());
    CHECK(alpha->media == coverPng());
    CHECK(alpha->sortKey == "alpha (usa)");
    CHECK(rowAt(catalog, profile, "gb", "Gone (USA).gb")->entry.state == EntryState::MissingFile);
    // The store keeps the record of the import, and its issues.
    const auto record = catalog.store->lastImport(profile.id);
    REQUIRE(record.has_value());
    REQUIRE(record->has_value());
    CHECK((*record)->kind == ImportKind::Import);
    CHECK((*record)->systems == first.changes.systems);
    CHECK(rv::test::dumpIndex(catalog.database->writer())["idx_import_issues"].size() == first.changes.issues.size());

    // A second import over the same library changes no identity and writes no index row.
    const ImportResult second = importOk(catalog, source, media, profile);
    CHECK_FALSE(second.changes.firstImport);
    CHECK(second.statistics.identitiesWritten == 0);
    CHECK(second.statistics.entriesWritten == 0);
    CHECK(second.statistics.entriesRemoved == 0);
    CHECK(second.statistics.systemsWritten == 0);
    CHECK_FALSE(second.statistics.searchIndexRebuilt);
    CHECK(second.changes.empty());
    CHECK(changesOf(second.changes, "gb").unchanged == 4);
    CHECK(second.changes.total().newIdentities == 0);
    CHECK(rv::catalog_db::checkDatabase(*catalog.database).value().passed());
}

TEST_CASE("catalog_db/import: identities match the exact path first, then the key; stale keys are computed again",
          "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-match");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source = twoSystems();
    ScriptedMedia media;
    importOk(catalog, source, media, profile);
    const GameId alpha = identityAt(catalog, profile, "gb", "Alpha (USA).gb").id;

    SECTION("a file renamed in case only keeps its identity, which takes the new spelling") {
        source.snapshot.entries[0] = diskGame("gb", "ALPHA (usa).gb", 100);
        rv::test::sortEntries(source.snapshot);
        const ImportResult renamed = importOk(catalog, source, media, profile);
        CHECK(renamed.changes.total().newIdentities == 0);
        CHECK(identityAt(catalog, profile, "gb", "ALPHA (usa).gb").id == alpha);
        CHECK(renamed.statistics.identitiesWritten == 1);
        CHECK(changesOf(renamed.changes, "gb").changed == 1);
        CHECK(rowAt(catalog, profile, "gb", "ALPHA (usa).gb")->id == alpha);
    }
    SECTION("keys stored with another casing table are computed again, in place") {
        auto& writer = catalog.database->writer();
        REQUIRE(writer.execute("UPDATE rv_game_identities SET lookup_key = 'STALE ' || id, key_casing = 'old-table'")
                    .has_value());
        const ImportResult rekeyed = importOk(catalog, source, media, profile);
        CHECK(rekeyed.changes.total().rekeyed == 4);
        CHECK(rekeyed.changes.total().newIdentities == 0);
        CHECK(rekeyed.statistics.identitiesWritten == 4);
        CHECK(rekeyed.statistics.entriesWritten == 0);
        CHECK(identityAt(catalog, profile, "gb", "Alpha (USA).gb").id == alpha);
        CHECK(writer.queryInt("SELECT count(*) FROM rv_game_identities WHERE lookup_key LIKE 'STALE %'").value() == 0);
        CHECK(writer.queryText("SELECT value FROM rv_meta WHERE key = 'casing_table_version'").value() ==
              currentLookupKeyVersion().casingTable);
    }
}

TEST_CASE("catalog_db/import: files removed and added keep their identities; a file that comes back gets its id back",
          "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-sc05");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source = twoSystems();
    ScriptedMedia media;
    importOk(catalog, source, media, profile);
    const GameId beta = identityAt(catalog, profile, "gb", "Beta (USA).gb").id;

    // Beta removed, Delta added.
    std::erase_if(source.snapshot.entries,
                  [](const CatalogEntry& entry) { return entry.key.relativePath() == "Beta (USA).gb"; });
    source.snapshot.entries.push_back(diskGame("gb", "Delta (USA).gb", 500));
    rv::test::sortEntries(source.snapshot);
    const ImportResult changed = importOk(catalog, source, media, profile);
    const SystemChanges& gb = changesOf(changed.changes, "gb");
    CHECK(gb.added == 1);
    CHECK(gb.removed == 1);
    CHECK(gb.unchanged == 2);
    CHECK(gb.vanished == 1);
    CHECK(gb.newIdentities == 1);
    CHECK_FALSE(rowAt(catalog, profile, "gb", "Beta (USA).gb").has_value());
    const StoredIdentity vanished = identityAt(catalog, profile, "gb", "Beta (USA).gb");
    CHECK_FALSE(vanished.present);
    REQUIRE(vanished.file.has_value()); // its last facts kept
    CHECK(vanished.file->size == 200);
    CHECK(changed.changes.removed.size() == 1);
    CHECK(changed.changes.removed[0].id == beta);

    // Beta is back: the same GameId.
    source.snapshot.entries.push_back(diskGame("gb", "Beta (USA).gb", 200));
    rv::test::sortEntries(source.snapshot);
    const ImportResult back = importOk(catalog, source, media, profile);
    CHECK(changesOf(back.changes, "gb").returned == 1);
    CHECK(changesOf(back.changes, "gb").newIdentities == 0);
    CHECK(rowAt(catalog, profile, "gb", "Beta (USA).gb")->id == beta);
    CHECK(identityAt(catalog, profile, "gb", "Beta (USA).gb").present);
}

TEST_CASE("catalog_db/import: a file gone with its gamelist entry left stays as a missing file",
          "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-missing");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source = twoSystems();
    source.snapshot.entries[0].gamelistPath = "./Alpha (USA).gb";
    ScriptedMedia media;
    media.media["gb/Alpha (USA).gb"] = coverPng();
    importOk(catalog, source, media, profile);
    const GameId alpha = identityAt(catalog, profile, "gb", "Alpha (USA).gb").id;

    source.snapshot.entries[0] = missingGame("gb", "Alpha (USA).gb", "Alpha");
    const ImportResult gone = importOk(catalog, source, media, profile);
    CHECK(changesOf(gone.changes, "gb").vanished == 1);
    CHECK(changesOf(gone.changes, "gb").changed == 1);
    REQUIRE(gone.changes.availabilityChanged.size() == 1);
    CHECK(gone.changes.availabilityChanged[0].after == Availability::Missing);
    const auto row = rowAt(catalog, profile, "gb", "Alpha (USA).gb");
    REQUIRE(row.has_value());
    CHECK(row->id == alpha);
    CHECK(row->entry.state == EntryState::MissingFile);
    CHECK(row->media == coverPng()); // its media outlive the ROM
    const StoredIdentity identity = identityAt(catalog, profile, "gb", "Alpha (USA).gb");
    CHECK_FALSE(identity.present);
    REQUIRE(identity.file.has_value());
    CHECK(identity.file->size == 100);
}

TEST_CASE("catalog_db/import: media that cannot be known keep what was stored", "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-media");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source = twoSystems();
    ScriptedMedia media;
    media.media["gb/Alpha (USA).gb"] = coverPng();
    importOk(catalog, source, media, profile);

    media.unavailable = true; // the media root unplugged
    const ImportResult unknown = importOk(catalog, source, media, profile);
    CHECK(unknown.statistics.mediaUnknown == 4);
    CHECK(unknown.statistics.entriesWritten == 0);
    CHECK(rowAt(catalog, profile, "gb", "Alpha (USA).gb")->media == coverPng());

    // Known again, without the cover: "no media" is written.
    media.unavailable = false;
    media.media.clear();
    const ImportResult none = importOk(catalog, source, media, profile);
    CHECK(none.statistics.entriesWritten == 1);
    CHECK_FALSE(rowAt(catalog, profile, "gb", "Alpha (USA).gb")->media.any());
}

TEST_CASE("catalog_db/import: a gamelist that failed keeps the system's rows; on a first import, metadata pending",
          "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-gamelist");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source = twoSystems();
    ScriptedMedia media;
    SystemInfo& gb = source.snapshot.systems[0];

    // First import, gb's gamelist cut short: nothing of gb is imported.
    gb.gamelist = GamelistState::Failed;
    const ImportResult pending = importOk(catalog, source, media, profile);
    CHECK(changesOf(pending.changes, "gb").outcome == SystemOutcome::MetadataPending);
    CHECK(changesOf(pending.changes, "gb").rows() == 0);
    CHECK(changesOf(pending.changes, "snes").added == 1);
    CHECK(rowCount(catalog, profile, "gb") == 0);
    CHECK(catalog.store->identities(profile.id, systemName("gb")).value().empty());
    const auto systems = catalog.store->systems(profile.id).value();
    REQUIRE(systems.size() == 2);
    CHECK(systems[0].gamelist == GamelistState::Failed); // its system row says so
    CHECK(hasIssue(pending.changes, kImportIssueGamelistKept));

    // Read: imported.
    gb.gamelist = GamelistState::Read;
    gb.alternativeEmulator = "Fake Emulator (Hidden Console)";
    source.snapshot.entries[0].gamelistPath = "./Alpha (USA).gb";
    source.snapshot.entries[0].title = "Alpha the Scraped";
    const ImportResult read = importOk(catalog, source, media, profile);
    CHECK(changesOf(read.changes, "gb").added == 3);
    const auto before = rv::test::dumpIndex(catalog.database->writer());

    // Failed again, with a file gone and an unscraped title: the rows stay as they were.
    gb.gamelist = GamelistState::Failed;
    gb.alternativeEmulator.clear();
    source.snapshot.entries[0].title = "Alpha (USA)";
    source.snapshot.entries[0].gamelistPath.clear();
    std::erase_if(source.snapshot.entries,
                  [](const CatalogEntry& entry) { return entry.key.relativePath() == "Beta (USA).gb"; });
    const ImportResult kept = importOk(catalog, source, media, profile);
    const SystemChanges& keptGb = changesOf(kept.changes, "gb");
    CHECK(keptGb.outcome == SystemOutcome::GamelistFailed);
    CHECK(keptGb.kept == 3);
    CHECK(keptGb.removed == 0);
    CHECK(rowAt(catalog, profile, "gb", "Alpha (USA).gb")->entry.title == "Alpha the Scraped");
    CHECK(rowCount(catalog, profile, "gb") == 3);
    const auto after = catalog.store->systems(profile.id).value();
    CHECK(after[0].gamelist == GamelistState::Failed);
    CHECK(after[0].alternativeEmulator == "Fake Emulator (Hidden Console)"); // kept with the rows
    auto afterIndex = rv::test::dumpIndex(catalog.database->writer());
    CHECK(afterIndex["idx_games"] == before.at("idx_games"));
    CHECK(identityAt(catalog, profile, "gb", "Beta (USA).gb").present); // never taken as vanished
}

TEST_CASE("catalog_db/import: folders that cannot be listed keep their rows as source unavailable",
          "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-sc06");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source;
    source.snapshot.systems = {scriptedSystem("gb")};
    source.snapshot.entries = {diskGame("gb", "Sub/One (USA).gb", 10), diskGame("gb", "Sub/Two (USA).gb", 20),
                               diskGame("gb", "Top (USA).gb", 30)};
    source.snapshot.entries[0].gamelistPath = "./Sub/One (USA).gb";
    ScriptedMedia media;
    importOk(catalog, source, media, profile);
    const GameId one = identityAt(catalog, profile, "gb", "Sub/One (USA).gb").id;
    const GameId two = identityAt(catalog, profile, "gb", "Sub/Two (USA).gb").id;
    const LibrarySnapshot listed = source.snapshot;

    // Sub cannot be listed: the gamelist alone tells of One; Two has no gamelist entry.
    source.snapshot.unavailableFolders = {
        UnavailableFolder{systemName("gb"), "Sub", SourceUnavailableReason::NotListable, "access denied"}};
    source.snapshot.entries = {unlistedGame("gb", "Sub/One (USA).gb", SourceUnavailableReason::NotListable),
                               diskGame("gb", "Top (USA).gb", 30)};
    const ImportResult folder = importOk(catalog, source, media, profile);
    const SystemChanges& gb = changesOf(folder.changes, "gb");
    CHECK(gb.outcome == SystemOutcome::Applied);
    CHECK(gb.unavailable == 2);
    CHECK(gb.removed == 0);
    CHECK(gb.vanished == 0);
    CHECK(gb.presentBefore == 1); // only Top's folder was listed
    for (const GameId id : {one, two}) {
        const auto rows = catalog.store->indexedEntries(profile.id, systemName("gb")).value();
        const auto row = std::find_if(rows.begin(), rows.end(), [&](const IndexedEntry& r) { return r.id == id; });
        REQUIRE(row != rows.end());
        CHECK(row->entry.availability == Availability::SourceUnavailable);
        CHECK(row->entry.unavailableReason == SourceUnavailableReason::NotListable);
        CHECK(row->entry.file.has_value()); // kept as it was
    }
    CHECK(identityAt(catalog, profile, "gb", "Sub/Two (USA).gb").present); // out of any reconciliation

    // The whole system folder missing (the drive unplugged): every row kept, nothing removed.
    source.snapshot.unavailableFolders.clear();
    source.snapshot.systems[0].availability = Availability::SourceUnavailable;
    source.snapshot.systems[0].unavailableReason = SourceUnavailableReason::Unreachable;
    source.snapshot.entries = {unlistedGame("gb", "Sub/One (USA).gb", SourceUnavailableReason::Unreachable)};
    const ImportResult system = importOk(catalog, source, media, profile);
    CHECK(changesOf(system.changes, "gb").outcome == SystemOutcome::SourceUnavailable);
    CHECK(changesOf(system.changes, "gb").unavailable == 3);
    CHECK(changesOf(system.changes, "gb").removed == 0);
    CHECK(hasIssue(system.changes, kImportIssueRomRootUnavailable));
    for (const IndexedEntry& row : rowsOf(catalog, profile, "gb")) {
        CHECK(row.entry.availability == Availability::SourceUnavailable);
        CHECK(row.entry.unavailableReason == SourceUnavailableReason::Unreachable);
    }

    // Back: every row available again under its GameId.
    source.snapshot = listed;
    const ImportResult back = importOk(catalog, source, media, profile);
    CHECK(changesOf(back.changes, "gb").changed == 3);
    CHECK(back.changes.availabilityChanged.size() == 3);
    CHECK(back.changes.total().newIdentities == 0);
    CHECK(rowAt(catalog, profile, "gb", "Sub/One (USA).gb")->id == one);
    CHECK(rowAt(catalog, profile, "gb", "Sub/Two (USA).gb")->entry.availability == Availability::Available);
}

TEST_CASE("catalog_db/import: the mass-disappearance guard changes nothing until the removals are accepted",
          "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-guard");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source;
    source.snapshot.systems = {scriptedSystem("gb")};
    source.snapshot.entries = {diskGame("gb", "A.gb", 1), diskGame("gb", "B.gb", 2), diskGame("gb", "C.gb", 3),
                               diskGame("gb", "D.gb", 4)};
    ScriptedMedia media;
    importOk(catalog, source, media, profile);
    const auto before = rv::test::dumpIndex(catalog.database->writer());
    const LibrarySnapshot all = source.snapshot;

    SECTION("half gone is no mass disappearance") {
        source.snapshot.entries.resize(2);
        const ImportResult half = importOk(catalog, source, media, profile);
        CHECK(changesOf(half.changes, "gb").outcome == SystemOutcome::Applied);
        CHECK(changesOf(half.changes, "gb").removed == 2);
    }
    SECTION("more than half gone: nothing changes, then the removals are accepted") {
        source.snapshot.entries.resize(1);
        source.snapshot.entries.push_back(diskGame("gb", "E.gb", 5)); // a new file is held back too
        const ImportResult held = importOk(catalog, source, media, profile);
        const SystemChanges& gb = changesOf(held.changes, "gb");
        CHECK(gb.outcome == SystemOutcome::KeptByGuard);
        CHECK(gb.presentBefore == 4);
        CHECK(gb.gone == 3);
        CHECK(gb.kept == 4);
        CHECK(hasIssue(held.changes, kImportIssueMassDisappearance));
        CHECK(held.statistics.entriesWritten == 0);
        CHECK(held.statistics.identitiesWritten == 0);
        auto index = rv::test::dumpIndex(catalog.database->writer());
        CHECK(index["idx_games"] == before.at("idx_games"));
        CHECK(identityAt(catalog, profile, "gb", "B.gb").present);

        ImportOptions accept;
        accept.acceptRemovals = true;
        const ImportResult accepted = importOk(catalog, source, media, profile, accept);
        CHECK(changesOf(accepted.changes, "gb").outcome == SystemOutcome::Applied);
        CHECK(changesOf(accepted.changes, "gb").removed == 3);
        CHECK(changesOf(accepted.changes, "gb").added == 1);
        CHECK(rowCount(catalog, profile, "gb") == 2);
        CHECK_FALSE(identityAt(catalog, profile, "gb", "B.gb").present);
    }
}

TEST_CASE("catalog_db/import: a system the configuration no longer defines", "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-unconfigured");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source = twoSystems();
    ScriptedMedia media;
    importOk(catalog, source, media, profile);
    const GameId zed = identityAt(catalog, profile, "snes", "Zed (Europe).sfc").id;

    source.snapshot.systems.pop_back();
    std::erase_if(source.snapshot.entries, [](const CatalogEntry& entry) { return entry.system.str() == "snes"; });
    const ImportResult held = importOk(catalog, source, media, profile);
    CHECK(changesOf(held.changes, "snes").outcome == SystemOutcome::KeptByGuard);
    CHECK(rowCount(catalog, profile, "snes") == 1);

    ImportOptions accept;
    accept.acceptRemovals = true;
    const ImportResult removed = importOk(catalog, source, media, profile, accept);
    CHECK(changesOf(removed.changes, "snes").outcome == SystemOutcome::NotConfigured);
    CHECK(changesOf(removed.changes, "snes").removed == 1);
    CHECK(hasIssue(removed.changes, kImportIssueSystemNotConfigured));
    CHECK(rowCount(catalog, profile, "snes") == 0);
    CHECK(catalog.store->systems(profile.id).value().size() == 1);
    const StoredIdentity identity = identityAt(catalog, profile, "snes", "Zed (Europe).sfc");
    CHECK(identity.id == zed);
    CHECK_FALSE(identity.present);
    // Configured again: the same GameId.
    source = twoSystems();
    importOk(catalog, source, media, profile);
    CHECK(rowAt(catalog, profile, "snes", "Zed (Europe).sfc")->id == zed);
}

namespace {

/// A pairing pass as I2 step 11b may make one: a vanished identity pairs with the new file of the same file name and
/// size (MoveReason::SizeTimeName), as a test stand-in.
class NamePairing final : public IIdentityPairing {
public:
    std::vector<PairingRequest> requests; ///< (spans not kept beyond the call)
    std::vector<VanishedIdentity> vanishedSeen;
    std::vector<NewFile> newFilesSeen;
    bool bogus = false; ///< pair an identity it was not given

    rv::Expected<IdentityPairs> pair(const PairingRequest& request) override {
        vanishedSeen.assign(request.vanished.begin(), request.vanished.end());
        newFilesSeen.assign(request.newFiles.begin(), request.newFiles.end());
        IdentityPairs pairs;
        if (bogus) {
            pairs.pairs.push_back(IdentityPair{GameId::fromValue(1).value(), request.newFiles[0].key,
                                               MoveReason::SizeTimeName});
            return pairs;
        }
        const auto nameOf = [](const LookupKey& key) {
            const std::string& path = key.relativePath();
            return path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
        };
        for (const VanishedIdentity& vanished : request.vanished) {
            for (const NewFile& file : request.newFiles) {
                if (nameOf(file.key) == nameOf(vanished.identity.key) && vanished.identity.file &&
                    file.file.size == vanished.identity.file->size) {
                    pairs.pairs.push_back(IdentityPair{vanished.identity.id, file.key, MoveReason::SizeTimeName});
                }
            }
        }
        return pairs;
    }
};

} // namespace

TEST_CASE("catalog_db/import: the seam for reconciliation moves a paired identity and gives its old path a new id",
          "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-seam");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source;
    source.snapshot.systems = {scriptedSystem("gb")};
    source.snapshot.entries = {diskGame("gb", "Dir1/Game (USA).gb", 77), diskGame("gb", "Other (USA).gb", 88)};
    source.snapshot.entries[0].gamelistPath = "./Dir1/Game (USA).gb";
    ScriptedMedia media;
    NamePairing pairing;
    importOk(catalog, source, media, profile, {}, &pairing);
    CHECK(pairing.vanishedSeen.empty()); // nothing vanished: the pass was not asked
    const GameId game = identityAt(catalog, profile, "gb", "Dir1/Game (USA).gb").id;

    // ES-DE moved the file and left its gamelist entry behind: a MissingFile entry at the old path.
    source.snapshot.entries = {missingGame("gb", "Dir1/Game (USA).gb", "Game"), diskGame("gb", "Dir2/Game (USA).gb", 77),
                               diskGame("gb", "Other (USA).gb", 88)};
    rv::test::sortEntries(source.snapshot);
    const auto before = rv::test::dumpIndex(catalog.database->writer());

    SECTION("a pairing that names what it was not given ends the import, which writes nothing") {
        pairing.bogus = true;
        rv::catalog::ImportService service(source, media, *catalog.store, &pairing);
        const auto refused = service.run(profile);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == rv::ErrorCode::InvalidArgument);
        CHECK(rv::test::dumpIndex(catalog.database->writer()) == before);
    }
    SECTION("the move") {
        const ImportResult moved = importOk(catalog, source, media, profile, {}, &pairing);
        REQUIRE(pairing.vanishedSeen.size() == 1);
        CHECK(pairing.vanishedSeen[0].identity.id == game);
        CHECK(pairing.vanishedSeen[0].gamelistEntryKept);
        REQUIRE(pairing.newFilesSeen.size() == 1);
        CHECK(pairing.newFilesSeen[0].key.relativePath() == "Dir2/Game (USA).gb");
        REQUIRE(moved.changes.moved.size() == 1);
        CHECK(moved.changes.moved[0].id == game);
        CHECK(moved.changes.moved[0].from.relativePath() == "Dir1/Game (USA).gb");
        CHECK(moved.changes.moved[0].to.relativePath() == "Dir2/Game (USA).gb");
        CHECK(moved.changes.moved[0].reason == MoveReason::SizeTimeName);
        const SystemChanges& gb = changesOf(moved.changes, "gb");
        CHECK(gb.moved == 1);
        CHECK(gb.gone == 0);
        CHECK(gb.vanished == 0);
        CHECK(gb.newIdentities == 1); // the gamelist entry left at the old path
        // The identity followed its file; the old path's entry has a new GameId, its tuple's value being taken.
        CHECK(identityAt(catalog, profile, "gb", "Dir2/Game (USA).gb").id == game);
        CHECK(rowAt(catalog, profile, "gb", "Dir2/Game (USA).gb")->id == game);
        const StoredIdentity old = identityAt(catalog, profile, "gb", "Dir1/Game (USA).gb");
        CHECK(old.id == firstId(profile, "gb", "Dir1/Game (USA).gb", 1));
        CHECK_FALSE(old.present);
        CHECK(rowAt(catalog, profile, "gb", "Dir1/Game (USA).gb")->entry.state == EntryState::MissingFile);
    }
}

TEST_CASE("catalog_db/import: the guard counts what stays gone once the pairing pass has run", "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-seam-guard");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source;
    source.snapshot.systems = {scriptedSystem("nes")};
    source.snapshot.entries = {diskGame("nes", "Old/Solo (USA).nes", 55)};
    ScriptedMedia media;
    NamePairing pairing;
    importOk(catalog, source, media, profile);
    const GameId solo = identityAt(catalog, profile, "nes", "Old/Solo (USA).nes").id;
    source.snapshot.entries = {diskGame("nes", "New/Solo (USA).nes", 55)}; // the only file, moved

    SECTION("without a pairing pass the move is a disappearance: kept, no move reported") {
        const ImportResult held = importOk(catalog, source, media, profile);
        CHECK(changesOf(held.changes, "nes").outcome == SystemOutcome::KeptByGuard);
        CHECK(changesOf(held.changes, "nes").gone == 1);
        CHECK(held.changes.moved.empty());
    }
    SECTION("paired, nothing is gone: applied") {
        const ImportResult moved = importOk(catalog, source, media, profile, {}, &pairing);
        CHECK(changesOf(moved.changes, "nes").outcome == SystemOutcome::Applied);
        CHECK(changesOf(moved.changes, "nes").gone == 0);
        CHECK(changesOf(moved.changes, "nes").moved == 1);
        REQUIRE(moved.changes.moved.size() == 1);
        CHECK(identityAt(catalog, profile, "nes", "New/Solo (USA).nes").id == solo);
        CHECK(rowCount(catalog, profile, "nes") == 1);
    }
}

namespace {

/// A store that hands out a transaction which sets `cancel` after `writesBeforeCancel` entries written: a cancellation
/// in the middle of the writes.
class CancellingStore final : public ICatalogStore {
public:
    CancellingStore(ICatalogStore& inner, std::atomic<bool>& cancel, std::size_t writesBeforeCancel)
        : m_inner(inner), m_cancel(cancel), m_writesBeforeCancel(writesBeforeCancel) {}

    class Transaction final : public IImportTransaction {
    public:
        Transaction(std::unique_ptr<IImportTransaction> inner, CancellingStore& store)
            : m_inner(std::move(inner)), m_store(store) {}
        rv::Status putLibrary(const LibraryProfile& profile) override { return m_inner->putLibrary(profile); }
        rv::Status putIdentity(const StoredIdentity& identity) override { return m_inner->putIdentity(identity); }
        rv::Status putSystem(const LibraryId& library, const SystemInfo& system) override {
            return m_inner->putSystem(library, system);
        }
        rv::Status putEntry(const IndexedEntry& entry) override {
            if (++m_store.m_written >= m_store.m_writesBeforeCancel) {
                m_store.m_cancel = true;
            }
            return m_inner->putEntry(entry);
        }
        rv::Status removeEntry(GameId id) override { return m_inner->removeEntry(id); }
        rv::Expected<bool> prepareWrites(std::size_t rows) override { return m_inner->prepareWrites(rows); }
        rv::Status clearIndex(const LibraryId& library) override { return m_inner->clearIndex(library); }
        rv::Status removeSystem(const LibraryId& library, const SystemName& system) override {
            return m_inner->removeSystem(library, system);
        }
        rv::Status putDiscGroup(const IndexedDiscGroup& group) override { return m_inner->putDiscGroup(group); }
        rv::Status removeDiscGroup(GameId head) override { return m_inner->removeDiscGroup(head); }
        rv::Status putImportIssues(const LibraryId& library, std::span<const Issue> issues,
                                   std::span<const SystemName> systems) override {
            return m_inner->putImportIssues(library, issues, systems);
        }
        rv::Status recordImport(const ImportRecord& record) override { return m_inner->recordImport(record); }
        rv::Status commit() override { return m_inner->commit(); }

    private:
        std::unique_ptr<IImportTransaction> m_inner;
        CancellingStore& m_store;
    };

    rv::Expected<std::vector<LibraryProfile>> libraries() override { return m_inner.libraries(); }
    rv::Expected<std::vector<StoredIdentity>> identities(const LibraryId& library, const SystemName& system) override {
        return m_inner.identities(library, system);
    }
    rv::Expected<std::optional<StoredIdentity>> identity(GameId id) override { return m_inner.identity(id); }
    rv::Expected<std::vector<GameId>> takenGameIds() override { return m_inner.takenGameIds(); }
    rv::Expected<std::vector<IndexedEntry>> indexedEntries(const LibraryId& library,
                                                           const SystemName& system) override {
        return m_inner.indexedEntries(library, system);
    }
    rv::Expected<std::vector<SystemInfo>> systems(const LibraryId& library) override { return m_inner.systems(library); }
    rv::Expected<std::vector<SystemName>> identitySystems(const LibraryId& library) override {
        return m_inner.identitySystems(library);
    }
    rv::Expected<std::vector<IndexedDiscGroup>> discGroups(const LibraryId& library) override {
        return m_inner.discGroups(library);
    }
    rv::Expected<std::optional<ImportRecord>> lastImport(const LibraryId& library) override {
        return m_inner.lastImport(library);
    }
    rv::Expected<std::unique_ptr<IImportLock>> lockImport(const LibraryId& library) override {
        return m_inner.lockImport(library);
    }
    rv::Expected<std::unique_ptr<IImportTransaction>> beginImport(const LibraryId& library) override {
        auto inner = m_inner.beginImport(library);
        if (!inner) {
            return rv::propagate(inner.error());
        }
        return std::unique_ptr<IImportTransaction>(std::make_unique<Transaction>(std::move(*inner), *this));
    }

private:
    ICatalogStore& m_inner;
    std::atomic<bool>& m_cancel;
    std::size_t m_writesBeforeCancel;
    std::size_t m_written = 0;
};

} // namespace

TEST_CASE("catalog_db/import: a cancelled import leaves the database as it was", "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-cancel");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source = twoSystems();
    ScriptedMedia media;
    importOk(catalog, source, media, profile);
    // A larger library to write: 600 new files, so that the writes cross the cancellation checks.
    for (int i = 0; i < 600; ++i) {
        source.snapshot.entries.push_back(diskGame("gb", "Many/File " + std::to_string(1000 + i) + ".gb", 10 + i));
    }
    rv::test::sortEntries(source.snapshot);
    const auto index = rv::test::dumpIndex(catalog.database->writer());
    const auto state = rv::test::dumpState(catalog.database->writer());
    const auto unchanged = [&] {
        CHECK(rv::test::dumpIndex(catalog.database->writer()) == index);
        CHECK(rv::test::dumpState(catalog.database->writer()) == state);
        const auto record = catalog.store->lastImport(profile.id).value();
        REQUIRE(record.has_value());
        CHECK(record->id == 1); // no record of the cancelled import
    };
    std::atomic<bool> cancel{false};
    ImportOptions options;
    options.cancel = &cancel;

    SECTION("while the library is read") {
        source.onLoad = [&] { cancel = true; };
        ImportService service(source, media, *catalog.store);
        const auto cancelled = service.run(profile, options);
        REQUIRE_FALSE(cancelled.has_value());
        CHECK(cancelled.error().code == rv::ErrorCode::Cancelled);
        unchanged();
    }
    SECTION("while the media are asked") {
        media.onPresence = [&] {
            if (media.asked == 300) {
                cancel = true;
            }
        };
        ImportService service(source, media, *catalog.store);
        const auto cancelled = service.run(profile, options);
        REQUIRE_FALSE(cancelled.has_value());
        CHECK(cancelled.error().code == rv::ErrorCode::Cancelled);
        unchanged();
    }
    SECTION("in the middle of the writes") {
        CancellingStore store(*catalog.store, cancel, 300);
        ImportService service(source, media, store);
        const auto cancelled = service.run(profile, options);
        REQUIRE_FALSE(cancelled.has_value());
        CHECK(cancelled.error().code == rv::ErrorCode::Cancelled);
        unchanged();
        // The lock went with the import: the next one runs.
        cancel = false;
        CHECK(importOk(catalog, source, media, profile).changes.total().added == 600);
    }
}

TEST_CASE("catalog_db/import: one import at a time, also from another holder of the database",
          "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-lock");
    const auto file = dir.file("data/retroverse.sqlite");
    ImportCatalog catalog = ImportCatalog::open(file);
    ImportCatalog other = ImportCatalog::open(file); // another process's
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source = twoSystems();
    ScriptedMedia media;
    const auto refused = [&](ImportCatalog& on) {
        ImportService service(source, media, *on.store);
        const auto result = service.run(profile);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == rv::ErrorCode::Conflict);
    };
    {
        auto lock = other.store->lockImport(profile.id);
        REQUIRE(lock.has_value());
        refused(catalog);
        refused(other); // its own lock too
        CHECK(source.loads == 0); // refused before the library is read
    }
    {
        auto transaction = other.store->beginImport(profile.id);
        REQUIRE(transaction.has_value());
        refused(catalog);
    }
    CHECK(importOk(catalog, source, media, profile).changes.total().added == 4);
    // A lock that goes while its store's transaction is open: the transaction keeps it to its end.
    {
        auto lock = other.store->lockImport(profile.id).value();
        auto transaction = other.store->beginImport(profile.id).value();
        lock.reset();
        refused(catalog);
        transaction.reset();
    }
    CHECK(importOk(catalog, source, media, profile).statistics.entriesWritten == 0);
}

TEST_CASE("catalog_db/import: a partial import leaves the other systems as they are", "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-partial");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source = twoSystems();
    ScriptedMedia media;
    importOk(catalog, source, media, profile);
    const auto before = rv::test::dumpIndex(catalog.database->writer());

    // A source made for gb only: snes is not in its snapshot, and is not taken as gone.
    source.snapshot.systems.pop_back();
    std::erase_if(source.snapshot.entries, [](const CatalogEntry& entry) { return entry.system.str() == "snes"; });
    source.snapshot.entries.push_back(diskGame("gb", "Delta (USA).gb", 500));
    rv::test::sortEntries(source.snapshot);
    ImportOptions only;
    only.systems = {systemName("gb")};
    const ImportResult partial = importOk(catalog, source, media, profile, only);
    CHECK(partial.changes.systems.size() == 1);
    CHECK(changesOf(partial.changes, "gb").added == 1);
    CHECK(rowCount(catalog, profile, "snes") == 1);
    CHECK(catalog.store->systems(profile.id).value().size() == 2);

    ImportOptions unknown;
    unknown.systems = {systemName("nes")};
    ImportService service(source, media, *catalog.store);
    const auto refused = service.run(profile, unknown);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::InvalidArgument);
}

TEST_CASE("catalog_db/import: disc groups are stored with their references and given back as the next load's cache",
          "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-groups");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source;
    source.snapshot.systems = {scriptedSystem("psx")};
    CatalogEntry head = diskGame("psx", "Twin (Europe).cue", 172);
    CatalogEntry track1 = diskGame("psx", "Twin (Europe) (Track 1).bin", 70560);
    CatalogEntry track2 = diskGame("psx", "Twin (Europe) (Track 2).bin", 35280);
    track1.state = EntryState::DiscMember;
    track2.state = EntryState::DiscMember;
    track1.discHead = head.key;
    track2.discHead = head.key;
    source.snapshot.entries = {head, track1, track2};
    rv::test::sortEntries(source.snapshot);
    DiscGroup group{systemName("psx"), head.key, DiscGroupKind::Cue, {track1.key, track2.key},
                    {track1.key, track2.key, lookupKey("Twin (Europe) (Track 3).bin")}};
    source.snapshot.discGroups = {group};
    ScriptedMedia media;
    const ImportResult first = importOk(catalog, source, media, profile);
    CHECK(first.statistics.discGroupsWritten == 1);
    const auto stored = catalog.store->discGroups(profile.id).value();
    REQUIRE(stored.size() == 1);
    CHECK(stored[0].head == identityAt(catalog, profile, "psx", "Twin (Europe).cue").id);
    CHECK(stored[0].cached.group == group);
    CHECK(stored[0].cached.headSize == 172);
    CHECK(stored[0].cached.headLastWriteTime == head.file->lastWriteTime);
    CHECK(stored[0].members == std::vector<GameId>{identityAt(catalog, profile, "psx", track1.key.relativePath()).id,
                                                   identityAt(catalog, profile, "psx", track2.key.relativePath()).id});

    // The next load is given the group (its references: the third track, not there, among them).
    const ImportResult second = importOk(catalog, source, media, profile);
    REQUIRE(source.lastCache.size() == 1);
    CHECK(source.lastCache[0] == stored[0].cached);
    CHECK(second.statistics.discGroupsWritten == 0);

    // The second track gone: the group is written again, the track's row removed.
    const auto removeEntry = [&](const CatalogEntry& gone) {
        std::erase_if(source.snapshot.entries, [&](const CatalogEntry& entry) { return entry.key == gone.key; });
    };
    removeEntry(track2);
    source.snapshot.discGroups[0].members.pop_back();
    const ImportResult third = importOk(catalog, source, media, profile);
    CHECK(third.statistics.discGroupsWritten == 1);
    CHECK(changesOf(third.changes, "psx").removed == 1);
    CHECK(catalog.store->discGroups(profile.id).value()[0].members.size() == 1);
    // The head gone: its group goes with its row.
    removeEntry(head);
    source.snapshot.discGroups.clear();
    importOk(catalog, source, media, profile);
    CHECK(catalog.store->discGroups(profile.id).value().empty());
    CHECK(rv::test::dumpIndex(catalog.database->writer())["idx_disc_references"].empty());
}

TEST_CASE("catalog_db/import: db rebuild-index gives the index a refresh gives and keeps the state",
          "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-rebuild");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source;
    source.snapshot.systems = {scriptedSystem("gb"), scriptedSystem("nes"), scriptedSystem("psx"),
                               scriptedSystem("snes")};
    CatalogEntry head = diskGame("psx", "Twin (Europe).cue", 172);
    CatalogEntry track = diskGame("psx", "Twin (Europe) (Track 1).bin", 70560);
    track.state = EntryState::DiscMember;
    track.discHead = head.key;
    source.snapshot.entries = {diskGame("gb", "Sub/One (USA).gb", 10), diskGame("gb", "Top (USA).gb", 30),
                               missingGame("gb", "Gone (USA).gb", "Gone"), diskGame("nes", "Nes Game (USA).nes", 40),
                               head, track, diskGame("snes", "Zed (Europe).sfc", 400)};
    rv::test::sortEntries(source.snapshot);
    source.snapshot.discGroups = {
        DiscGroup{systemName("psx"), head.key, DiscGroupKind::Cue, {track.key}, {track.key}}};
    ScriptedMedia media;
    media.media["gb/Top (USA).gb"] = coverPng();
    media.media["snes/Zed (Europe).sfc"] = coverPng();
    importOk(catalog, source, media, profile);

    // A refresh in which the snapshot cannot tell everything: gb's Sub cannot be listed, nes's gamelist failed, snes's
    // media are out of reach; and something changed (a new psx file).
    source.snapshot.unavailableFolders = {
        UnavailableFolder{systemName("gb"), "Sub", SourceUnavailableReason::NotListable, "access denied"}};
    std::erase_if(source.snapshot.entries,
                  [](const CatalogEntry& entry) { return entry.key.relativePath() == "Sub/One (USA).gb"; });
    source.snapshot.systems[1].gamelist = GamelistState::Failed;
    source.snapshot.entries.push_back(diskGame("psx", "Solo (USA).chd", 999));
    rv::test::sortEntries(source.snapshot);
    media.unavailableSystems = {"snes"};
    importOk(catalog, source, media, profile);
    const auto refreshed = rv::test::dumpIndex(catalog.database->writer());
    const auto state = rv::test::dumpState(catalog.database->writer());
    CHECK(refreshed.at("idx_disc_references").size() == 1);

    SECTION("as it is") {
        ImportOptions rebuild;
        rebuild.rebuildIndex = true;
        const ImportResult rebuilt = importOk(catalog, source, media, profile, rebuild);
        CHECK(rebuilt.statistics.entriesWritten == 8); // every row, those only the index knew included
        CHECK(rebuilt.statistics.identitiesWritten == 0);
        CHECK(rebuilt.statistics.searchIndexRebuilt);
        CHECK(rv::test::dumpIndex(catalog.database->writer()) == refreshed);
        CHECK(rv::test::dumpState(catalog.database->writer()) == state);
        CHECK(catalog.store->lastImport(profile.id).value()->kind == ImportKind::RebuildIndex);
        CHECK(rv::catalog_db::checkDatabase(*catalog.database).value().passed());
    }
    SECTION("an index damaged by hand is repaired") {
        auto& writer = catalog.database->writer();
        REQUIRE(writer.execute("DELETE FROM idx_disc_references").has_value());
        REQUIRE(writer.execute("UPDATE idx_systems SET full_name = 'Damaged' WHERE name = 'psx'").has_value());
        ImportOptions rebuild;
        rebuild.rebuildIndex = true;
        importOk(catalog, source, media, profile, rebuild);
        CHECK(rv::test::dumpIndex(catalog.database->writer()) == refreshed);
        CHECK(rv::test::dumpState(catalog.database->writer()) == state);
    }
}

TEST_CASE("catalog_db/import: the search index of a bulk write equals the one the triggers build",
          "[catalog_db][import][search]") {
    const rv::test::TempDir dir("rv-import-search");
    const LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source = twoSystems();
    source.snapshot.entries[0].metadata.developer = "Studio Mezzanotte";
    source.snapshot.entries[1].title = "Éclair Nocturne";
    ScriptedMedia media;
    const auto importWith = [&](std::string_view name, SearchIndexPolicy policy) {
        StoreOptions options;
        options.searchIndex = policy;
        ImportCatalog catalog = ImportCatalog::open(dir.file(std::string(name) + "/retroverse.sqlite"), options);
        const ImportResult result = importOk(catalog, source, media, profile);
        CHECK(result.statistics.searchIndexRebuilt == (policy != SearchIndexPolicy::FollowWrites));
        CHECK(rv::catalog_db::searchIndexFollowsWrites(catalog.database->writer()).value());
        CHECK(rv::catalog_db::checkDatabase(*catalog.database).value().passed());
        return rv::test::dumpIndex(catalog.database->writer());
    };
    const auto bulk = importWith("automatic", SearchIndexPolicy::Automatic); // a first import: suspended
    const auto triggers = importWith("triggers", SearchIndexPolicy::FollowWrites);
    CHECK_FALSE(bulk.at("idx_game_search").empty());
    CHECK(bulk == triggers);
}

TEST_CASE("catalog_db/import: the library's id is the store's; a new ROM root keeps every id", "[catalog_db][import]") {
    const rv::test::TempDir dir("rv-import-library");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    LibraryProfile profile = rv::test::scriptedLibrary();
    ScriptedSource source = twoSystems();
    ScriptedMedia media;
    importOk(catalog, source, media, profile);
    const auto identities = rv::test::dumpState(catalog.database->writer()).at("rv_game_identities");

    // library.json naming another library is refused, and nothing is written.
    const LibraryProfile other = rv::test::scriptedLibrary("other");
    ImportService service(source, media, *catalog.store);
    const auto refused = service.run(other);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::Conflict);
    CHECK(refused.error().context == "library.json");
    CHECK(source.loads == 1);

    // A new ROM root (a new drive letter): the systems' folders change, no identity does.
    profile.romRoot = std::filesystem::path(L"Q:\\Moved\\roms");
    for (SystemInfo& system : source.snapshot.systems) {
        system.romFolder = *profile.romRoot / system.name.str();
    }
    const ImportResult moved = importOk(catalog, source, media, profile);
    CHECK(moved.statistics.systemsWritten == 2);
    CHECK(moved.statistics.identitiesWritten == 0);
    CHECK(moved.statistics.entriesWritten == 0);
    CHECK(rv::test::dumpState(catalog.database->writer()).at("rv_game_identities") == identities);
    CHECK(catalog.store->libraries().value()[0].romRoot == profile.romRoot);
}
