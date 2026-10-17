// The import (I2 step 11a) of ES-DE fixtures through the adapter (esde::EsdeLibrarySource, esde::EsdeMediaResolver)
// into the SQLite store: the standard fixture as its README counts it (every state, the disc group with its references,
// the media), imported again without a write, nothing of it copied and its scraper credentials nowhere in the database;
// ROMs added and removed on disk (SC-05); a system folder unplugged, the ROM root missing, a folder that cannot be
// listed (SC-06); a gamelist cut short while ES-DE writes it; a first import that reads no ROM byte; the search index
// of a bulk write against the triggers'; the long-path fixture.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/identity.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>

#include "../esde/snapshot_test_support.hpp"
#include "import_test_support.hpp"
#include "support/file_system.hpp"
#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using namespace rv::catalog;
using rv::test::changesOf;
using rv::test::ImportCatalog;
using rv::test::systemName;

namespace {

/// A fixture materialized, its ROM folders' times settled (settleFolderTimes()): imports compare exactly.
rv::test::Fixture materializeSettled(std::string_view name) {
    rv::test::Fixture fixture = rv::test::esde::materialize(name);
    rv::test::settleFolderTimes(fixture.path("roms"));
    return fixture;
}

/// An import of a fixture as retroverse-cli makes one: the ES-DE adapter (gamelists read again at once after a
/// failure: no delay in tests) and the media resolver over the fixture's media root.
ImportResult importFixture(ImportCatalog& catalog, const LibraryProfile& profile, const ImportOptions& options = {}) {
    rv::esde::EsdeLibrarySourceOptions sourceOptions;
    sourceOptions.gamelistRetryDelay = std::chrono::milliseconds(0);
    rv::esde::EsdeLibrarySource source(sourceOptions);
    const auto settings = rv::esde::readSettings(profile.esdeHome);
    REQUIRE(settings.has_value());
    rv::esde::EsdeMediaResolver media(rv::esde::resolveMediaRoot(*settings, profile));
    return rv::test::importOk(catalog, source, media, profile, options);
}

GameId idOf(std::string_view system, std::string_view relativePath, std::uint32_t counter = 0) {
    const auto id = stableGameId(LibraryId(), systemName(system), rv::test::lookupKey(relativePath).key(), counter);
    REQUIRE(id.has_value());
    return *id;
}

/// The index rows of a system (a vector of its own: a loop over a member of a temporary Expected would outlive it).
std::vector<IndexedEntry> rowsOf(ImportCatalog& catalog, const SystemName& system) {
    auto rows = catalog.store->indexedEntries(LibraryId(), system);
    REQUIRE(rows.has_value());
    return std::move(*rows);
}
std::vector<IndexedEntry> rowsOf(ImportCatalog& catalog, std::string_view system) {
    return rowsOf(catalog, systemName(system));
}

/// An index row found, or not: -> and * REQUIRE it.
struct FoundRow {
    std::optional<IndexedEntry> row;
    const IndexedEntry* operator->() const {
        REQUIRE(row.has_value());
        return &*row;
    }
    const IndexedEntry& operator*() const {
        REQUIRE(row.has_value());
        return *row;
    }
    [[nodiscard]] bool has_value() const noexcept { return row.has_value(); }
};

FoundRow rowOf(ImportCatalog& catalog, std::string_view system, std::string_view relativePath) {
    for (IndexedEntry& row : rowsOf(catalog, system)) {
        if (row.entry.key.relativePath() == relativePath) {
            return FoundRow{std::move(row)};
        }
    }
    return FoundRow{};
}

/// The index rows of a system, counted by state.
std::map<EntryState, std::size_t> statesOf(ImportCatalog& catalog, std::string_view system) {
    std::map<EntryState, std::size_t> states;
    for (const IndexedEntry& row : rowsOf(catalog, system)) {
        ++states[row.entry.state];
    }
    return states;
}

std::string mediaOf(const IndexedEntry& row) {
    std::string text;
    for (std::size_t i = 0; i < kMediaKindCount; ++i) {
        if (row.media.kinds[i].present) {
            text += std::string(text.empty() ? "" : " ") + std::string(toString(kMediaKinds[i])) +
                    row.media.kinds[i].extension;
        }
    }
    return text;
}

/// Every index row of the library, one line each: GameId, system, path, state, availability, the file's size (and, for a
/// file, its time), title, media: what two imports of one tree compare, readably.
std::vector<std::string> projectionOf(ImportCatalog& catalog) {
    std::vector<std::string> lines;
    const std::vector<SystemInfo> systems = catalog.store->systems(LibraryId()).value();
    for (const SystemInfo& system : systems) {
        for (const IndexedEntry& row : rowsOf(catalog, system.name)) {
            const CatalogEntry& entry = row.entry;
            std::string file = "-";
            if (entry.file) {
                file = std::to_string(entry.file->size) +
                       (entry.file->directory ? std::string(" dir") : " " + std::to_string(entry.file->lastWriteTime));
            }
            lines.push_back(row.id.toString() + " " + system.name.str() + "/" + entry.key.relativePath() + " " +
                            std::string(toString(entry.state)) + " " + std::string(toString(entry.availability)) +
                            " " + file + " " + entry.title + " [" + mediaOf(row) + "]");
        }
    }
    return lines;
}

/// The identities' GameIds with their paths.
std::vector<std::string> identitiesOf(ImportCatalog& catalog) {
    std::vector<std::string> lines;
    for (const std::string_view system : {"gb", "psx", "snes"}) {
        const std::vector<StoredIdentity> identities = catalog.store->identities(LibraryId(), systemName(system)).value();
        for (const StoredIdentity& identity : identities) {
            lines.push_back(identity.id.toString() + " " + std::string(system) + "/" + identity.key.relativePath());
        }
    }
    return lines;
}

/// The bytes of the database's files (the database and its WAL), read with full sharing.
std::string databaseBytes(const fs::path& file) {
    std::string bytes;
    for (const fs::path& part : {file, fs::path(file.native() + L"-wal")}) {
        if (fs::exists(part)) {
            auto read = rv::core::readFileShared(part, 512ull * 1024 * 1024);
            REQUIRE(read.has_value());
            bytes += *read;
        }
    }
    return bytes;
}

} // namespace

TEST_CASE("catalog_db/import: the standard fixture imported", "[catalog_db][import][fixture]") {
    const rv::test::Fixture fixture = materializeSettled("esde/standard");
    const rv::test::TempDir dir("rv-import-standard");
    const fs::path file = dir.file("data/database/retroverse.sqlite");
    ImportCatalog catalog = ImportCatalog::open(file);
    const LibraryProfile profile = rv::test::esde::profileOf(fixture);
    const auto tree = rv::test::esde::treeState(fixture.root());

    const ImportResult first = importFixture(catalog, profile);
    CHECK(first.changes.firstImport);
    // The README's counts: 22 indexed entries (the entry outside gb's folder never is).
    CHECK(changesOf(first.changes, "gb").added == 10);
    CHECK(changesOf(first.changes, "snes").added == 8);
    CHECK(changesOf(first.changes, "psx").added == 4);
    CHECK(first.changes.total().newIdentities == 22);
    CHECK(first.statistics.entriesWritten == 22);
    CHECK(first.statistics.discGroupsWritten == 1);
    CHECK(statesOf(catalog, "gb") == std::map<EntryState, std::size_t>{
                                         {EntryState::Game, 8}, {EntryState::MissingFile, 1}, {EntryState::Folder, 1}});
    CHECK(statesOf(catalog, "snes") == std::map<EntryState, std::size_t>{{EntryState::Game, 5},
                                                                           {EntryState::NotAGame, 1},
                                                                           {EntryState::Folder, 2}});
    CHECK(statesOf(catalog, "psx") ==
          std::map<EntryState, std::size_t>{{EntryState::Game, 2}, {EntryState::DiscMember, 2}});
    // GameIds as their tuples derive them on an empty database; the namesakes in two collections apart.
    CHECK(rowOf(catalog, "gb", "Tiny Tennis (Europe).gb")->id == idOf("gb", "Tiny Tennis (Europe).gb"));
    CHECK(rowOf(catalog, "gb", "Collection B/Pocket Puzzle (World).gb")->id ==
          idOf("gb", "Collection B/Pocket Puzzle (World).gb"));
    CHECK(rowOf(catalog, "snes", "Delta Drift (USA).sfc")->entry.gamelistPath == "./delta drift (usa).sfc");
    const auto missing = rowOf(catalog, "gb", "Gone Fishing (USA).gb");
    REQUIRE(missing.has_value());
    CHECK(missing->entry.availability == Availability::Missing);
    // The disc group, with what its sheet says.
    const auto groups = catalog.store->discGroups(LibraryId()).value();
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].group.head.relativePath() == "Twin Tracks (Europe).cue");
    CHECK(groups[0].group.references.size() == 2);
    CHECK(groups[0].members == std::vector<GameId>{idOf("psx", "Twin Tracks (Europe) (Track 1).bin"),
                                                   idOf("psx", "Twin Tracks (Europe) (Track 2).bin")});
    CHECK(groups[0].group.headSize == fs::file_size(fixture.path("roms/psx/Twin Tracks (Europe).cue")));
    // The media of the README's table, presence and extension per kind.
    CHECK(mediaOf(*rowOf(catalog, "gb", "Tiny Tennis (Europe).gb")) == "covers.png screenshots.jpg videos.mp4");
    CHECK(mediaOf(*rowOf(catalog, "gb", "Collection A/Pocket Puzzle (World).gb")) == "covers.png");
    CHECK(mediaOf(*rowOf(catalog, "gb", "Collection B/Pocket Puzzle (World).gb")).empty());
    CHECK(mediaOf(*missing) == "covers.png");
    CHECK(mediaOf(*rowOf(catalog, "gb", "Café Racer (Europe).gb")) == "videos.avi");
    CHECK(mediaOf(*rowOf(catalog, "gb", "LOUD CART (USA).GB")) == "videos.mkv");
    CHECK(mediaOf(*rowOf(catalog, "snes", "Alpha Quest (USA).sfc")) ==
          "covers.png backcovers.png 3dboxes.png physicalmedia.png screenshots.jpg titlescreens.jpg marquees.png "
          "miximages.png fanart.jpg manuals.pdf videos.mov custom.png");
    CHECK(mediaOf(*rowOf(catalog, "snes", "Beta Run (Europe).SMC")).empty());
    CHECK(mediaOf(*rowOf(catalog, "snes", "Hacks")) == "covers.png");
    CHECK(mediaOf(*rowOf(catalog, "snes", "Sports Vol. 2")) == "covers.png");
    CHECK(mediaOf(*rowOf(catalog, "psx", "Twin Tracks (Europe).cue")) == "covers.png videos.wmv");
    CHECK(mediaOf(*rowOf(catalog, "psx", "Twin Tracks (Europe) (Track 1).bin")).empty());
    // Systems: snes's alternative emulator, as the gamelist chooses it.
    const auto systems = catalog.store->systems(LibraryId()).value();
    REQUIRE(systems.size() == 3);
    CHECK(systems[2].name.str() == "snes");
    CHECK(systems[2].alternativeEmulator == "Fake Emulator (Hidden Console)");

    // Imported again: no identity changes, no entry row written, the sheet taken from the catalog.
    const ImportResult second = importFixture(catalog, profile);
    CHECK(second.statistics.identitiesWritten == 0);
    CHECK(second.statistics.entriesWritten == 0);
    CHECK(second.statistics.entriesRemoved == 0);
    CHECK(second.statistics.discGroupsWritten == 0);
    CHECK(second.changes.total().unchanged == 22);
    CHECK(rv::catalog_db::checkDatabase(*catalog.database).value().passed());

    // Nothing was written in the fixture; nothing of it was copied into the data directory, and its scraper
    // credentials are nowhere in the database.
    CHECK(rv::test::esde::treeState(fixture.root()) == tree);
    catalog.close();
    const std::string bytes = databaseBytes(file);
    CHECK(bytes.find("fixture-scraper-user-Q7x4") == std::string::npos);
    CHECK(bytes.find("fixture-scraper-password-K9w2") == std::string::npos);
    const auto listed = rv::core::listTree(dir.file("data"));
    REQUIRE(listed.has_value());
    std::uint64_t total = 0;
    for (const rv::core::TreeEntry& entry : *listed) {
        INFO(entry.relativePath);
        if (entry.kind != rv::core::TreeEntryKind::File) {
            continue;
        }
        total += entry.size;
        CHECK(entry.relativePath.starts_with("database/retroverse.sqlite"));
    }
    CHECK(total < 4ull * 1024 * 1024);
}

TEST_CASE("catalog_db/import: ROMs added and removed between imports (SC-05)", "[catalog_db][import][fixture]") {
    const rv::test::Fixture fixture = materializeSettled("esde/standard");
    const rv::test::TempDir dir("rv-import-sc05-disk");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::esde::profileOf(fixture);
    importFixture(catalog, profile);

    // Copied into a sub-folder of ES-DE's: two new ROMs; one removed.
    REQUIRE(rv::test::writeFileAnyLength(fixture.path("roms/gb/New Arrivals/Fresh Start (World).gb"),
                                         rv::test::payloadBytes(77, 32768))
                .has_value());
    REQUIRE(rv::test::writeFileAnyLength(fixture.path("roms/snes/Omega Run (USA).sfc"), rv::test::payloadBytes(78, 1024))
                .has_value());
    fs::remove(fixture.path("roms/gb/LOUD CART (USA).GB"));
    const ImportResult changed = importFixture(catalog, profile);
    CHECK(changesOf(changed.changes, "gb").added == 1);
    CHECK(changesOf(changed.changes, "gb").removed == 1);
    CHECK(changesOf(changed.changes, "gb").vanished == 1);
    CHECK(changesOf(changed.changes, "snes").added == 1);
    CHECK(changed.changes.total().newIdentities == 2);
    // The nine others are as they were: not written again.
    CHECK(changesOf(changed.changes, "gb").unchanged == 9);
    CHECK(changesOf(changed.changes, "gb").changed == 0);
    CHECK(changed.statistics.entriesWritten == 2);
    CHECK(rowOf(catalog, "gb", "New Arrivals/Fresh Start (World).gb")->id ==
          idOf("gb", "New Arrivals/Fresh Start (World).gb"));
    CHECK_FALSE(rowOf(catalog, "gb", "LOUD CART (USA).GB").has_value());

    // Put back: the same GameId.
    REQUIRE(rv::test::writeFileAnyLength(fixture.path("roms/gb/LOUD CART (USA).GB"), rv::test::payloadBytes(6, 65536))
                .has_value());
    const ImportResult back = importFixture(catalog, profile);
    CHECK(changesOf(back.changes, "gb").returned == 1);
    CHECK(rowOf(catalog, "gb", "LOUD CART (USA).GB")->id == idOf("gb", "LOUD CART (USA).GB"));
}

TEST_CASE("catalog_db/import: folders unplugged or that cannot be listed keep their rows (SC-06)",
          "[catalog_db][import][fixture]") {
    const rv::test::Fixture fixture = materializeSettled("esde/standard");
    const rv::test::TempDir dir("rv-import-sc06-disk");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::esde::profileOf(fixture);
    importFixture(catalog, profile);
    const std::vector<std::string> identities = identitiesOf(catalog);
    const auto available = [&](std::string_view system) {
        for (const IndexedEntry& row : rowsOf(catalog, system)) {
            INFO(row.entry.key.relativePath());
            CHECK(row.entry.availability != Availability::SourceUnavailable);
        }
    };

    SECTION("a system folder unplugged, then back") {
        fs::rename(fixture.path("roms/gb"), fixture.path("roms/gb-away"));
        const ImportResult away = importFixture(catalog, profile);
        const SystemChanges& gb = changesOf(away.changes, "gb");
        CHECK(gb.outcome == SystemOutcome::SourceUnavailable);
        CHECK(gb.removed == 0);
        CHECK(gb.unavailable == 10);
        CHECK(gb.vanished == 0);
        CHECK(changesOf(away.changes, "snes").unchanged == 8);
        for (const IndexedEntry& row : rowsOf(catalog, "gb")) {
            CHECK(row.entry.availability == Availability::SourceUnavailable);
            CHECK(row.entry.unavailableReason == SourceUnavailableReason::FolderMissing);
        }
        fs::rename(fixture.path("roms/gb-away"), fixture.path("roms/gb"));
        const ImportResult back = importFixture(catalog, profile);
        CHECK(back.changes.total().newIdentities == 0);
        CHECK(changesOf(back.changes, "gb").changed == 10);
        available("gb");
        CHECK(identitiesOf(catalog) == identities);
    }
    SECTION("the ROM root missing") {
        fs::rename(fixture.path("roms"), fixture.path("roms-away"));
        const ImportResult away = importFixture(catalog, profile);
        CHECK(away.changes.total().removed == 0);
        CHECK(away.changes.total().unavailable == 22);
        CHECK(std::any_of(away.changes.issues.begin(), away.changes.issues.end(),
                          [](const Issue& issue) { return issue.code == kImportIssueRomRootUnavailable; }));
        fs::rename(fixture.path("roms-away"), fixture.path("roms"));
        const ImportResult back = importFixture(catalog, profile);
        CHECK(back.changes.total().newIdentities == 0);
        CHECK(back.changes.total().removed == 0);
        available("gb");
        available("psx");
        CHECK(identitiesOf(catalog) == identities);
    }
    SECTION("a folder that cannot be listed") {
        std::optional<rv::test::ListingDenied> denied;
        {
            auto applied = rv::test::ListingDenied::apply(fixture.path("roms/gb/Collection A"));
            REQUIRE(applied.has_value());
            denied.emplace(std::move(*applied));
        }
        const ImportResult unlisted = importFixture(catalog, profile);
        const SystemChanges& gb = changesOf(unlisted.changes, "gb");
        CHECK(gb.outcome == SystemOutcome::Applied);
        CHECK(gb.unavailable == 2); // inside Collection A (listed itself): Pocket Puzzle and Deep/Bottom Dweller
        CHECK(gb.removed == 0);
        CHECK(gb.vanished == 0);
        const auto row = rowOf(catalog, "gb", "Collection A/Deep/Bottom Dweller (Japan).gbc");
        REQUIRE(row.has_value());
        CHECK(row->entry.availability == Availability::SourceUnavailable);
        CHECK(row->entry.unavailableReason == SourceUnavailableReason::NotListable);
        CHECK(row->entry.file.has_value());
        CHECK(rowOf(catalog, "gb", "Collection A")->entry.availability == Availability::Available); // listed above
        denied.reset();
        const ImportResult back = importFixture(catalog, profile);
        CHECK(back.changes.total().newIdentities == 0);
        available("gb");
        CHECK(identitiesOf(catalog) == identities);
    }
}

TEST_CASE("catalog_db/import: a gamelist cut short while ES-DE writes it", "[catalog_db][import][fixture]") {
    const rv::test::Fixture fixture = materializeSettled("esde/standard");
    const rv::test::TempDir dir("rv-import-cut");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::esde::profileOf(fixture);
    const fs::path gamelist = fixture.path("ES-DE/gamelists/gb/gamelist.xml");
    const auto whole = rv::core::readFileShared(gamelist, 1024 * 1024);
    REQUIRE(whole.has_value());
    const auto cut = [&] {
        REQUIRE(
            rv::test::writeFileAnyLength(gamelist, std::string_view(*whole).substr(0, whole->size() / 2)).has_value());
    };

    // On a first import: gb is metadata pending, not imported unscraped.
    cut();
    const ImportResult pending = importFixture(catalog, profile);
    CHECK(changesOf(pending.changes, "gb").outcome == SystemOutcome::MetadataPending);
    CHECK(changesOf(pending.changes, "snes").added == 8);
    CHECK(catalog.store->indexedEntries(LibraryId(), systemName("gb")).value().empty());

    // Whole again: imported with its metadata.
    REQUIRE(rv::test::writeFileAnyLength(gamelist, *whole).has_value());
    const ImportResult read = importFixture(catalog, profile);
    CHECK(changesOf(read.changes, "gb").added == 10);
    const std::string title = rowOf(catalog, "gb", "Tiny Tennis (Europe).gb")->entry.title;
    CHECK(title != "Tiny Tennis (Europe)"); // the gamelist's name, not the file's

    // Cut short again: the rows stay as they were (their metadata kept).
    cut();
    const ImportResult kept = importFixture(catalog, profile);
    CHECK(changesOf(kept.changes, "gb").outcome == SystemOutcome::GamelistFailed);
    CHECK(changesOf(kept.changes, "gb").kept == 10);
    CHECK(kept.statistics.entriesWritten == 0);
    CHECK(rowOf(catalog, "gb", "Tiny Tennis (Europe).gb")->entry.title == title);
}

TEST_CASE("catalog_db/import: a first import reads no ROM byte", "[catalog_db][import][fixture]") {
    const rv::test::Fixture fixture = materializeSettled("esde/standard");
    const rv::test::TempDir dir("rv-import-bytes");
    const LibraryProfile profile = rv::test::esde::profileOf(fixture);
    // Every file under roms/ and media/ denies reading its data (its attributes, size and times stay readable;
    // its folder stays listable), but the cue sheet, which the disc groups read (ADR-0008 §3).
    std::vector<rv::test::DataReadDenied> denials;
    for (const std::string_view root : {"roms", "media"}) {
        const auto listed = rv::core::listTree(fixture.path(root));
        REQUIRE(listed.has_value());
        for (const rv::core::TreeEntry& entry : *listed) {
            if (entry.kind != rv::core::TreeEntryKind::File || entry.relativePath.ends_with(".cue")) {
                continue;
            }
            auto denied = rv::test::DataReadDenied::apply(fixture.path(root) / rv::core::pathFromUtf8(entry.relativePath));
            REQUIRE(denied.has_value());
            denials.push_back(std::move(*denied));
        }
    }
    REQUIRE(denials.size() > 40);
    // The denial holds: the data of a ROM cannot be read.
    CHECK_FALSE(rv::core::readFileShared(fixture.path("roms/gb/Tiny Tennis (Europe).gb"), 1024 * 1024).has_value());

    ImportCatalog denied = ImportCatalog::open(dir.file("denied/retroverse.sqlite"));
    const ImportResult result = importFixture(denied, profile);
    CHECK(result.changes.total().added == 22);
    const std::vector<std::string> deniedRows = projectionOf(denied);
    auto deniedIndex = rv::test::dumpIndex(denied.database->writer());
    denials.clear();

    // The same import with every file readable gives the same entries, media, disc group and issues.
    ImportCatalog readable = ImportCatalog::open(dir.file("readable/retroverse.sqlite"));
    importFixture(readable, profile);
    CHECK(projectionOf(readable) == deniedRows);
    rv::test::checkSameDump(deniedIndex, rv::test::dumpIndex(readable.database->writer()));
}

TEST_CASE("catalog_db/import: the standard fixture's search index, bulk or by the triggers",
          "[catalog_db][import][fixture][search]") {
    const rv::test::Fixture fixture = materializeSettled("esde/standard");
    const rv::test::TempDir dir("rv-import-search-disk");
    const LibraryProfile profile = rv::test::esde::profileOf(fixture);
    rv::catalog_db::StoreOptions triggers;
    triggers.searchIndex = rv::catalog_db::SearchIndexPolicy::FollowWrites;
    ImportCatalog bulk = ImportCatalog::open(dir.file("bulk/retroverse.sqlite"));
    ImportCatalog rowByRow = ImportCatalog::open(dir.file("rows/retroverse.sqlite"), triggers);
    CHECK(importFixture(bulk, profile).statistics.searchIndexRebuilt);
    CHECK_FALSE(importFixture(rowByRow, profile).statistics.searchIndexRebuilt);
    const auto index = rv::test::dumpIndex(bulk.database->writer());
    CHECK(index.at("idx_game_search").size() > 50);
    rv::test::checkSameDump(index, rv::test::dumpIndex(rowByRow.database->writer()));
    CHECK(rv::catalog_db::checkDatabase(*bulk.database).value().passed());
    CHECK(rv::catalog_db::checkDatabase(*rowByRow.database).value().passed());
}

TEST_CASE("catalog_db/import: the user's custom systems are imported like any other (SC-11)",
          "[catalog_db][import][fixture]") {
    const rv::test::Fixture fixture = materializeSettled("esde/custom-systems");
    const rv::test::TempDir dir("rv-import-custom");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::esde::profileOf(fixture);
    const ImportResult first = importFixture(catalog, profile);
    CHECK(first.changes.total().added > 0);
    // homebrew, a system of the user's own, and gb as the custom file replaces it.
    const std::vector<SystemInfo> systems = catalog.store->systems(LibraryId()).value();
    const auto homebrew = std::find_if(systems.begin(), systems.end(),
                                       [](const SystemInfo& system) { return system.name.str() == "homebrew"; });
    REQUIRE(homebrew != systems.end());
    CHECK(changesOf(first.changes, "homebrew").added == rowsOf(catalog, "homebrew").size());
    CHECK_FALSE(rowsOf(catalog, "homebrew").empty());
    const auto gb = std::find_if(systems.begin(), systems.end(),
                                 [](const SystemInfo& system) { return system.name.str() == "gb"; });
    REQUIRE(gb != systems.end());
    CHECK(gb->fullName == "Game Boy (custom)");
    // Its media root is ES-DE's default, which is not there: unknown, never "no media".
    CHECK(first.statistics.mediaUnknown == first.changes.total().added);
    const ImportResult second = importFixture(catalog, profile);
    CHECK(second.statistics.entriesWritten == 0);
    CHECK(second.statistics.identitiesWritten == 0);
}

TEST_CASE("catalog_db/import: the long-path fixture", "[catalog_db][import][fixture]") {
    const rv::test::Fixture fixture = materializeSettled("esde/long-path");
    const std::string folders = "A Very Long Folder Name Made To Exceed The Classic Windows Path Limit Of Two Hundred "
                                "And Sixty (Part One)/Another Long Folder Name For The Same Purpose So That The Whole "
                                "Path Is Well Past It (Part Two)";
    const std::string cue = "Long Disc Of An Invented Game With Many Words In Its Title (Europe).cue";
    // The cue sheet, as the fixture's README has the test write it.
    REQUIRE(rv::test::writeFileAnyLength(
                fixture.path("roms/psx/" + folders + "/" + cue),
                "FILE \"Long Disc Of An Invented Game With Many Words In Its Title (Europe) (Track 1).bin\" BINARY\n")
                .has_value());
    const rv::test::TempDir dir("rv-import-long");
    ImportCatalog catalog = ImportCatalog::open(dir.file("data/retroverse.sqlite"));
    const LibraryProfile profile = rv::test::esde::profileOf(fixture);
    const ImportResult first = importFixture(catalog, profile);
    CHECK(changesOf(first.changes, "snes").added == 3);
    CHECK(changesOf(first.changes, "psx").added == 2);
    CHECK(first.statistics.discGroupsWritten == 1);
    std::size_t longest = 0;
    for (const SystemChanges& system : first.changes.systems) {
        for (const IndexedEntry& row : rowsOf(catalog, system.system)) {
            longest = std::max(longest, system.system.str().size() + 1 + row.entry.key.relativePath().size());
        }
    }
    CHECK(longest > 260); // below roms/, whatever the folder above
    CHECK(rowOf(catalog, "psx", folders + "/" + cue)->entry.state == EntryState::Game);
    CHECK(importFixture(catalog, profile).statistics.entriesWritten == 0);
}
