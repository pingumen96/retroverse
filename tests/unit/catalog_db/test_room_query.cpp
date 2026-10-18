// The room's bulk snapshot (I3 step 2): SqliteCatalogQuery::roomEntries on the catalog database, every field against
// what the store wrote (a generated catalog of every state, media and favourites, and play sessions of every outcome
// and launch tests); the systems refused or empty; one read while an import writes; the media left unknown until
// esde::checkRoomMedia() looks at the media root, and the media paths derived without listing a folder
// (esde::roomMediaPaths()); the standard ES-DE fixture imported, snapshotted and selected (world::selectRoom), its
// selection the golden tests/fixtures/world/selection/standard.json. The benchmark of 2,000 games is in
// test_room_query_bench.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/import_service.hpp>
#include <rv/catalog/play_session.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/core/time.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/world_config.hpp>

#include "../esde/snapshot_test_support.hpp"
#include "../world/selection_golden.hpp"
#include "catalog_db_test_support.hpp"
#include "import_test_support.hpp"
#include "support/catalog_generator.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using namespace rv::catalog;
using rv::catalog_db::SqliteCatalogQuery;

namespace {

/// A session of `game` that started at `started`, ended with `outcome` (Running: open).
PlaySession sessionOf(GameId game, std::string_view started, PlaySessionOutcome outcome, bool test = false) {
    PlaySession session;
    session.game = game;
    session.choice = rv::launcher::LaunchChoice{rv::launcher::LaunchChoiceKind::EsdeCommand, "Fake Emulator"};
    session.choiceSource = ChoiceSource::FirstCommand;
    session.test = test;
    session.started = rv::core::parseUtcIso8601(started).value();
    session.outcome = outcome;
    session.owner = SessionOwner{4242, 133'000'000'000'000'000ull};
    if (outcome != PlaySessionOutcome::Running) {
        session.ended = session.started + std::chrono::minutes(30);
    }
    if (outcome == PlaySessionOutcome::Blocked) {
        session.readiness = LaunchReadiness::EmulatorNotFound;
    }
    if (outcome == PlaySessionOutcome::FailedToStart) {
        session.failure = rv::launcher::FailureReason::StartFailed;
    }
    return session;
}

/// What roomEntries() must give for an entry the store wrote, with its sessions' count and last start.
RoomEntry expectedEntry(const IndexedEntry& indexed, std::uint64_t sessions = 0,
                        std::optional<std::chrono::system_clock::time_point> last = std::nullopt) {
    const CatalogEntry& entry = indexed.entry;
    RoomEntry room;
    room.id = indexed.id;
    room.key = entry.key;
    room.directory = entry.file ? entry.file->directory : entry.state == EntryState::Folder;
    room.title = entry.title;
    room.sortKey = indexed.sortKey;
    if (entry.metadata.releaseDate) {
        room.year = entry.metadata.releaseDate->year;
    }
    room.rating = entry.metadata.rating;
    room.favorite = entry.flags.favorite;
    room.hidden = entry.flags.hidden;
    room.esdePlayCount = entry.flags.playCount;
    room.sessionCount = sessions;
    room.lastSession = last;
    room.state = entry.state;
    room.availability = entry.availability;
    room.media = indexed.media;
    return room;
}

std::unique_ptr<SqliteCatalogQuery> openQuery(rv::catalog_db::Database& database) {
    auto reader = database.openReader();
    REQUIRE(reader.has_value());
    auto query = SqliteCatalogQuery::open(std::move(*reader));
    REQUIRE(query.has_value());
    return std::move(*query);
}

/// An import of a fixture as retroverse-cli makes one (test_import_fixtures.cpp's).
ImportResult importFixture(rv::test::ImportCatalog& catalog, const LibraryProfile& profile) {
    rv::esde::EsdeLibrarySourceOptions sourceOptions;
    sourceOptions.gamelistRetryDelay = std::chrono::milliseconds(0);
    rv::esde::EsdeLibrarySource source(sourceOptions);
    const auto settings = rv::esde::readSettings(profile.esdeHome);
    REQUIRE(settings.has_value());
    rv::esde::EsdeMediaResolver media(rv::esde::resolveMediaRoot(*settings, profile));
    return rv::test::importOk(catalog, source, media, profile);
}

} // namespace

TEST_CASE("catalog_db/room_query: every entry of a system with what the store wrote", "[catalog_db][room_query]") {
    rv::test::TempDir dir("rv-room-query");
    rv::test::CatalogGeneratorOptions options;
    options.entries = 3'000;
    auto generated = rv::test::generateCatalogDatabase(rv::test::databaseFileIn(dir), options);
    REQUIRE(generated.has_value());
    auto database = rv::catalog_db::openCatalogDatabase(rv::test::databaseFileIn(dir));
    REQUIRE(database.has_value());

    // Sessions of three games of one system: every outcome, a launch test, a blocked attempt and a failure to start.
    std::map<SystemName, std::vector<const IndexedEntry*>> bySystem;
    for (const IndexedEntry& indexed : generated->entries) {
        bySystem[indexed.entry.system].push_back(&indexed);
    }
    const SystemName snes = SystemName::parse("snes").value();
    REQUIRE(bySystem[snes].size() >= 3);
    const GameId played = bySystem[snes][0]->id;
    const GameId tested = bySystem[snes][1]->id;
    const GameId failed = bySystem[snes][2]->id;
    {
        auto sessions = rv::catalog_db::SessionStore::open(database->writer());
        REQUIRE(sessions.has_value());
        for (const PlaySession& session : {
                 sessionOf(played, "2026-10-01T20:00:00.000Z", PlaySessionOutcome::Returned),
                 sessionOf(played, "2026-10-03T20:00:00.000Z", PlaySessionOutcome::Crashed),
                 sessionOf(played, "2026-10-02T20:00:00.000Z", PlaySessionOutcome::LeftRunning),
                 sessionOf(played, "2026-10-04T08:00:00.000Z", PlaySessionOutcome::Running),
                 sessionOf(played, "2026-10-05T08:00:00.000Z", PlaySessionOutcome::Blocked),
                 sessionOf(played, "2026-10-06T08:00:00.000Z", PlaySessionOutcome::FailedToStart),
                 sessionOf(tested, "2026-10-01T10:00:00.000Z", PlaySessionOutcome::Returned, true),
                 sessionOf(failed, "2026-10-01T11:00:00.000Z", PlaySessionOutcome::FailedToStart),
                 sessionOf(failed, "2026-10-01T12:00:00.000Z", PlaySessionOutcome::Blocked),
             }) {
            PlaySession stored = session;
            if (stored.outcome == PlaySessionOutcome::Crashed) {
                stored.exitCode = 0xC0000005u;
                stored.exitKind = rv::launcher::ExitKind::Crash;
            }
            if (stored.outcome == PlaySessionOutcome::LeftRunning) {
                stored.leftRunning = LeftRunningReason::Quit;
            }
            REQUIRE((*sessions)->insertSession(stored).has_value());
        }
    }

    auto query = openQuery(*database);
    std::size_t systems = 0;
    for (const auto& [system, entries] : bySystem) {
        INFO(system.str());
        const auto snapshot = query->roomEntries(system);
        REQUIRE(snapshot.has_value());
        ++systems;
        CHECK(snapshot->system == system);
        CHECK(snapshot->sortKeyVersion == kSortKeyVersion);
        // The database cannot tell whether the media root can be reached: unknown until checkRoomMedia().
        CHECK_FALSE(snapshot->mediaKnown);
        CHECK_THAT(snapshot->mediaUnknownReason, ContainsSubstring("media root has not been checked"));
        std::vector<RoomEntry> expected;
        for (const IndexedEntry* indexed : entries) {
            if (indexed->id == played) {
                // Returned, crashed, left running and running: four that started it, the last on the 4th.
                expected.push_back(
                    expectedEntry(*indexed, 4, rv::core::parseUtcIso8601("2026-10-04T08:00:00.000Z").value()));
            } else {
                expected.push_back(expectedEntry(*indexed));
            }
        }
        std::sort(expected.begin(), expected.end(), [](const RoomEntry& a, const RoomEntry& b) { return a.id < b.id; });
        REQUIRE(snapshot->entries.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            INFO(expected[i].id.toString() << " " << expected[i].key.relativePath());
            CHECK(snapshot->entries[i] == expected[i]);
        }
    }
    CHECK(systems == bySystem.size());
    // Every state is in the catalog, and some entries have media of both kinds the generator writes.
    std::size_t covered = 0;
    std::set<EntryState> states;
    for (const IndexedEntry& indexed : generated->entries) {
        covered += indexed.media[MediaKind::Cover].present ? 1 : 0;
        states.insert(indexed.entry.state);
    }
    CHECK(covered > 1'000);
    CHECK(states.size() == 5);
}

TEST_CASE("catalog_db/room_query: systems unknown, empty, and one read while an import writes",
          "[catalog_db][room_query]") {
    rv::test::TempDir dir("rv-room-query-systems");
    rv::test::ImportCatalog catalog = rv::test::ImportCatalog::open(rv::test::databaseFileIn(dir));
    const LibraryProfile library = rv::test::testLibrary();
    const CatalogEntry first = rv::test::fullEntry("gb", "Tiny Tennis (USA).gb", "Tiny Tennis");
    const GameId firstId = rv::test::gameIdOf(library.id, first);
    {
        auto transaction = catalog.store->beginImport(library.id);
        REQUIRE(transaction.has_value());
        REQUIRE((*transaction)->putLibrary(library).has_value());
        REQUIRE((*transaction)->putSystem(library.id, rv::test::testSystem("gb", "C:/Giochi/gb")).has_value());
        REQUIRE((*transaction)->putSystem(library.id, rv::test::testSystem("snes", "C:/Giochi/snes")).has_value());
        REQUIRE((*transaction)->putIdentity(rv::test::identityOf(firstId, library.id, first)).has_value());
        REQUIRE((*transaction)->putEntry(rv::test::indexedEntry(firstId, first)).has_value());
        REQUIRE((*transaction)->commit().has_value());
    }
    auto query = openQuery(*catalog.database);

    const auto unknown = query->roomEntries(SystemName::parse("psx").value());
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == rv::ErrorCode::NotFound);
    CHECK(unknown.error().context == "psx");

    // A system the catalog knows, without entries.
    const auto empty = query->roomEntries(SystemName::parse("snes").value());
    REQUIRE(empty.has_value());
    CHECK(empty->entries.empty());

    // While an import's transaction is open, a reader sees the last commit, whole.
    const CatalogEntry second = rv::test::bareEntry("gb", "Pocket Puzzle (World).gb");
    const GameId secondId = rv::test::gameIdOf(library.id, second);
    auto transaction = catalog.store->beginImport(library.id);
    REQUIRE(transaction.has_value());
    REQUIRE((*transaction)->putIdentity(rv::test::identityOf(secondId, library.id, second)).has_value());
    REQUIRE((*transaction)->putEntry(rv::test::indexedEntry(secondId, second, false)).has_value());
    const auto during = query->roomEntries(SystemName::parse("gb").value());
    REQUIRE(during.has_value());
    REQUIRE(during->entries.size() == 1);
    CHECK(during->entries.front() == expectedEntry(rv::test::indexedEntry(firstId, first)));
    REQUIRE((*transaction)->commit().has_value());
    const auto after = query->roomEntries(SystemName::parse("gb").value());
    REQUIRE(after.has_value());
    CHECK(after->entries.size() == 2);
    // No read transaction outlives the call: the writer can checkpoint.
    CHECK_FALSE(query->connection().inTransaction());
}

TEST_CASE("catalog_db/room_query: the media known from the root, the paths derived without a listing",
          "[catalog_db][room_query]") {
    rv::test::Fixture fixture = rv::test::esde::materialize("esde/standard");
    const LibraryProfile profile = rv::test::esde::profileOf(fixture);
    rv::test::TempDir dir("rv-room-query-media");
    rv::test::ImportCatalog catalog = rv::test::ImportCatalog::open(rv::test::databaseFileIn(dir));
    (void)importFixture(catalog, profile);
    auto query = openQuery(*catalog.database);
    auto snapshot = query->roomEntries(SystemName::parse("snes").value());
    REQUIRE(snapshot.has_value());
    REQUIRE_FALSE(snapshot->mediaKnown);

    // The media root is there: known. The check lists the root's own folder, never a system's.
    const auto settings = rv::esde::readSettings(profile.esdeHome);
    REQUIRE(settings.has_value());
    rv::esde::EsdeMediaResolver media(rv::esde::resolveMediaRoot(*settings, profile));
    rv::esde::checkRoomMedia(*snapshot, media);
    CHECK(snapshot->mediaKnown);
    CHECK(snapshot->mediaUnknownReason.empty());
    CHECK(media.stats().systemsListed == 0);

    // The paths, from the stored presence: Alpha Quest's twelve kinds, the folders' covers (a folder's whole name).
    const std::vector<rv::esde::RoomMediaPaths> paths = rv::esde::roomMediaPaths(*snapshot);
    std::map<std::string, const rv::esde::RoomMediaPaths*> byCover;
    for (const rv::esde::RoomMediaPaths& entry : paths) {
        byCover[entry[MediaKind::Cover]] = &entry;
    }
    REQUIRE(byCover.contains("snes/covers/Alpha Quest (USA).png"));
    const rv::esde::RoomMediaPaths& alpha = *byCover["snes/covers/Alpha Quest (USA).png"];
    CHECK(alpha[MediaKind::Video] == "snes/videos/Alpha Quest (USA).mov");
    CHECK(alpha[MediaKind::Screenshot] == "snes/screenshots/Alpha Quest (USA).jpg");
    CHECK(alpha[MediaKind::Manual] == "snes/manuals/Alpha Quest (USA).pdf");
    CHECK(std::all_of(alpha.relativePaths.begin(), alpha.relativePaths.end(),
                      [](const std::string& path) { return !path.empty(); }));
    CHECK(byCover.contains("snes/covers/Sports Vol. 2.png"));
    CHECK(byCover.contains("snes/covers/Hacks.png"));
    // Each path names a file of the fixture's media folder: what the resolver would have found.
    for (const rv::esde::RoomMediaPaths& entry : paths) {
        for (const std::string& path : entry.relativePaths) {
            if (!path.empty()) {
                INFO(path);
                CHECK(fs::is_regular_file(fixture.path("media/" + path)));
            }
        }
    }
    // Only entries that have media are listed, in the snapshot's order.
    CHECK(paths.size() == 3);
    CHECK(std::is_sorted(
        paths.begin(), paths.end(),
        [](const rv::esde::RoomMediaPaths& a, const rv::esde::RoomMediaPaths& b) { return a.game < b.game; }));

    // The media root unplugged (renamed away): unknown, with the root's reason, and the paths still derived (the
    // texture cache serves what it holds under them).
    std::error_code renamed;
    fs::rename(fixture.path("media"), fixture.path("media-unplugged"), renamed);
    REQUIRE_FALSE(renamed);
    rv::esde::EsdeMediaResolver unplugged(rv::esde::resolveMediaRoot(*settings, profile));
    rv::esde::checkRoomMedia(*snapshot, unplugged);
    CHECK_FALSE(snapshot->mediaKnown);
    CHECK_THAT(snapshot->mediaUnknownReason, ContainsSubstring("does not exist"));
    CHECK(rv::esde::roomMediaPaths(*snapshot) == paths);
    const auto selection = rv::world::selectRoom(rv::world::RoomInput{profile.id, *snapshot}, rv::world::WorldConfig{});
    REQUIRE_FALSE(selection.has_value());
    CHECK(selection.error().code == rv::ErrorCode::Unavailable);
    CHECK_THAT(selection.error().message, ContainsSubstring("does not exist"));
}

TEST_CASE("catalog_db/room_query: a stored extension that is no extension gives no path", "[catalog_db][room_query]") {
    RoomSnapshot snapshot;
    snapshot.system = SystemName::parse("snes").value();
    RoomEntry entry;
    entry.id = GameId::fromValue(1).value();
    entry.key = makeLookupKey("Sub/Pixel Quest (USA).sfc").value();
    entry.media[MediaKind::Cover] = MediaPresence{true, ".png"};
    entry.media[MediaKind::Video] = MediaPresence{true, "mp4"};
    entry.media[MediaKind::Manual] = MediaPresence{true, ".p/df"};
    snapshot.entries.push_back(entry);
    RoomEntry folder;
    folder.id = GameId::fromValue(2).value();
    folder.key = makeLookupKey("Sports Vol. 2").value();
    folder.directory = true;
    folder.media[MediaKind::Cover] = MediaPresence{true, ".PNG"};
    snapshot.entries.push_back(folder);
    RoomEntry none;
    none.id = GameId::fromValue(3).value();
    none.key = makeLookupKey("Bare.sfc").value();
    none.media[MediaKind::Video] = MediaPresence{true, ""};
    snapshot.entries.push_back(none);

    const std::vector<rv::esde::RoomMediaPaths> paths = rv::esde::roomMediaPaths(snapshot);
    REQUIRE(paths.size() == 2);
    CHECK(paths[0].game == entry.id);
    CHECK(paths[0][MediaKind::Cover] == "snes/covers/Sub/Pixel Quest (USA).png");
    CHECK(paths[0][MediaKind::Video].empty());
    CHECK(paths[0][MediaKind::Manual].empty());
    CHECK(paths[1][MediaKind::Cover] == "snes/covers/Sports Vol. 2.PNG");
}

TEST_CASE("catalog_db/room_query: the standard fixture's SNES room, golden", "[catalog_db][room_query][golden]") {
    rv::test::Fixture fixture = rv::test::esde::materialize("esde/standard");
    const LibraryProfile profile = rv::test::esde::profileOf(fixture);
    rv::test::TempDir dir("rv-room-query-standard");
    rv::test::ImportCatalog catalog = rv::test::ImportCatalog::open(rv::test::databaseFileIn(dir));
    (void)importFixture(catalog, profile);
    auto query = openQuery(*catalog.database);
    auto snapshot = query->roomEntries(SystemName::parse("snes").value());
    REQUIRE(snapshot.has_value());
    const auto settings = rv::esde::readSettings(profile.esdeHome);
    REQUIRE(settings.has_value());
    rv::esde::EsdeMediaResolver media(rv::esde::resolveMediaRoot(*settings, profile));
    rv::esde::checkRoomMedia(*snapshot, media);
    REQUIRE(snapshot->mediaKnown);

    // Every entry the catalog holds for snes, as details() gives it.
    for (const RoomEntry& entry : snapshot->entries) {
        const auto details = query->details(entry.id);
        REQUIRE(details.has_value());
        REQUIRE(details->has_value());
        CHECK(entry == expectedEntry((*details)->indexed));
    }

    const auto selection = rv::world::selectRoom(rv::world::RoomInput{profile.id, *snapshot}, rv::world::WorldConfig{});
    REQUIRE(selection.has_value());
    // Alpha Quest is the system's only covered game: the one title; the others are unscraped or uncovered.
    CHECK(selection->eligibility == rv::world::Eligibility::Covered);
    REQUIRE(selection->titles.size() == 1);
    CHECK(selection->titles.front().titleKey == "alpha quest");
    CHECK(std::all_of(selection->unplaced.begin(), selection->unplaced.end(), [](const rv::world::UnplacedGame& game) {
        return game.reason == rv::world::UnplacedReason::NoCover;
    }));
    auto text = rv::world::writeRoomSelection(*selection);
    REQUIRE(text.has_value());
    rv::test::checkSelectionGolden("standard", *text);

    // Including every game: one title per title key, the versions of a title together.
    rv::world::WorldConfig all;
    all.include = rv::world::PlacementInclude::All;
    const auto every = rv::world::selectRoom(rv::world::RoomInput{profile.id, *snapshot}, all);
    REQUIRE(every.has_value());
    auto everyText = rv::world::writeRoomSelection(*every);
    REQUIRE(everyText.has_value());
    rv::test::checkSelectionGolden("standard_all", *everyText);
}
