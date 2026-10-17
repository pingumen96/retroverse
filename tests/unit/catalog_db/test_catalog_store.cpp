// I2 step 4: SqliteCatalogStore, rv_catalog's ICatalogStore on the catalog database (ADR-0008 §4, §5): values written
// and read back unchanged, the orders the port documents, the import transaction (its own reads see its writes, others
// see them once committed, a dropped one leaves nothing, one import at a time across stores and processes), the store's
// refusals, and 100,000 identities and index rows in one transaction (a tenth in Debug; timed, informative).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/identity.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>

#include "catalog_db_test_support.hpp"
#include "support/temp_dir.hpp"

using namespace rv::catalog;
using namespace std::chrono_literals;
using namespace std::string_view_literals;
using rv::ErrorCode;
using rv::catalog_db::Connection;
using rv::catalog_db::Database;
using rv::catalog_db::SqliteCatalogStore;
using rv::test::fullEntry;
using rv::test::gameIdOf;
using rv::test::identityOf;
using rv::test::indexedEntry;

namespace {

template <class T>
ErrorCode codeOf(const rv::Expected<T>& result) {
    return result.has_value() ? ErrorCode::Unknown : result.error().code;
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

SystemName systemNamed(std::string_view name) {
    return SystemName::parse(name).value();
}

/// A migrated database in a test's folder and a store on it.
struct StoreFixture {
    explicit StoreFixture(std::string_view prefix)
        : dir(prefix), database(rv::catalog_db::openCatalogDatabase(rv::test::databaseFileIn(dir)).value()),
          store(SqliteCatalogStore::open(database).value()) {}

    rv::test::TempDir dir;
    Database database;
    std::unique_ptr<SqliteCatalogStore> store;
};

/// Writes `library`, `systems`, and each entry with its identity in one import.
void importAll(ICatalogStore& store, const LibraryProfile& library, const std::vector<SystemInfo>& systems,
               const std::vector<IndexedEntry>& entries) {
    auto transaction = store.beginImport(library.id);
    REQUIRE(transaction.has_value());
    REQUIRE((*transaction)->putLibrary(library).has_value());
    for (const SystemInfo& system : systems) {
        REQUIRE((*transaction)->putSystem(library.id, system).has_value());
    }
    for (const IndexedEntry& entry : entries) {
        REQUIRE((*transaction)->putIdentity(identityOf(entry.id, library.id, entry.entry)).has_value());
        REQUIRE((*transaction)->putEntry(entry).has_value());
    }
    REQUIRE((*transaction)->commit().has_value());
}

} // namespace

TEST_CASE("catalog_db/store: a store needs the program's schema version", "[catalog_db]") {
    const rv::test::TempDir dir("rv-store-version");
    auto database = Database::open(rv::test::databaseFileIn(dir));
    REQUIRE(database.has_value());
    const auto unmigrated = SqliteCatalogStore::open(*database);
    REQUIRE_FALSE(unmigrated.has_value());
    CHECK(unmigrated.error().code == ErrorCode::Conflict);
    CHECK(contains(unmigrated.error().message, "migrate it first"));
    REQUIRE(rv::catalog_db::migrate(*database).has_value());
    CHECK(SqliteCatalogStore::open(*database).has_value());
}

TEST_CASE("catalog_db/store: libraries, systems, identities and index rows are read back unchanged", "[catalog_db]") {
    StoreFixture fixture("rv-store-roundtrip");
    ICatalogStore& store = *fixture.store;
    const LibraryProfile library = rv::test::testLibrary();
    LibraryProfile second = rv::test::testLibrary("second-library");
    second.installFolder.reset();
    second.romRoot = std::filesystem::path(L"R:\\ROMs \u00E0 jouer");

    // Every kind of entry the snapshot holds, every field set somewhere.
    std::vector<IndexedEntry> entries;
    const auto add = [&](CatalogEntry entry, bool withMedia = true) {
        const GameId id = gameIdOf(library.id, entry); // before the entry moves (arguments have no order)
        entries.push_back(indexedEntry(id, std::move(entry), withMedia));
    };
    add(fullEntry("gb", "Collection À/Pokémon-like (Japan).gb", "Pocket Monsters-alike"));
    {
        CatalogEntry missing = fullEntry("gb", "Gone Game (Europe).gb", "Gone Game");
        missing.state = EntryState::MissingFile;
        missing.availability = Availability::Missing;
        missing.file.reset();
        missing.raw = {};
        missing.metadata.players = Players{8, std::nullopt}; // an open range
        missing.metadata.releaseDate.reset();
        missing.metadata.rating = Rating{0};
        missing.flags = {};
        add(std::move(missing), false);
    }
    {
        CatalogEntry hidden = fullEntry("gb", "Hidden Game (USA).gb", "Hidden Game");
        hidden.state = EntryState::NotAGame;
        hidden.notAGameReason = NotAGameReason::ExcludedBySetting;
        hidden.excludingSetting = "ShowHiddenGames";
        hidden.flags.hidden = true;
        add(std::move(hidden));
    }
    {
        CatalogEntry folder = rv::test::bareEntry("gb", "Sports Vol. 2");
        folder.state = EntryState::Folder;
        folder.file = FileFacts{true, 0, 132'500'000'000'000'000, true};
        add(std::move(folder));
    }
    {
        CatalogEntry unavailable = rv::test::bareEntry("gb", "Locked/Behind Glass (World).gb");
        unavailable.availability = Availability::SourceUnavailable;
        unavailable.unavailableReason = SourceUnavailableReason::NotListable;
        unavailable.file.reset();
        add(std::move(unavailable), false);
    }
    add(rv::test::bareEntry("psx", "Twin Tracks (Europe).cue"));
    {
        CatalogEntry member = rv::test::bareEntry("psx", "Twin Tracks (Europe) (Track 1).bin");
        member.state = EntryState::DiscMember;
        member.discHead = makeLookupKey("Twin Tracks (Europe).cue").value();
        add(std::move(member), false);
    }
    const std::vector<SystemInfo> systems{rv::test::testSystem("gb", L"I:\\Giochi\\gb"),
                                          rv::test::testSystem("psx", L"I:\\Giochi\\psx")};
    importAll(store, library, systems, entries);
    {
        // A second library, in a second import: libraries come back by id.
        auto transaction = store.beginImport(second.id);
        REQUIRE(transaction.has_value());
        REQUIRE((*transaction)->putLibrary(second).has_value());
        REQUIRE((*transaction)->commit().has_value());
    }
    CHECK(store.libraries().value() == std::vector<LibraryProfile>{library, second});

    for (const std::string_view systemName : {"gb", "psx"}) {
        INFO(systemName);
        const SystemName system = systemNamed(systemName);
        std::vector<IndexedEntry> expected;
        std::vector<StoredIdentity> identities;
        for (const IndexedEntry& entry : entries) {
            if (entry.entry.system == system) {
                expected.push_back(entry);
                identities.push_back(identityOf(entry.id, library.id, entry.entry));
            }
        }
        std::sort(expected.begin(), expected.end(),
                  [](const IndexedEntry& a, const IndexedEntry& b) { return entryOrder(a.entry, b.entry); });
        std::sort(identities.begin(), identities.end(),
                  [](const StoredIdentity& a, const StoredIdentity& b) { return a.key < b.key; });
        const auto storedEntries = store.indexedEntries(library.id, system);
        REQUIRE(storedEntries.has_value());
        REQUIRE(storedEntries->size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            INFO(expected[i].entry.key.relativePath());
            CHECK((*storedEntries)[i] == expected[i]);
        }
        CHECK(store.identities(library.id, system).value() == identities);
        CHECK(store.indexedEntries(second.id, system).value().empty());
    }
    const IndexedEntry& first = entries.front();
    CHECK(store.identity(first.id).value() == identityOf(first.id, library.id, first.entry));
    CHECK_FALSE(store.identity(GameId::fromValue(7).value()).value().has_value());

    std::vector<GameId> ids;
    for (const IndexedEntry& entry : entries) {
        ids.push_back(entry.id);
    }
    std::sort(ids.begin(), ids.end());
    CHECK(store.takenGameIds().value() == ids);

    // The systems' rows (read by the queries of I2 step 9).
    Connection& writer = fixture.database.writer();
    CHECK(writer
              .queryText("SELECT full_name || '|' || platforms || '|' || rom_folder_configured || '|' || rom_folder "
                         "|| '|' || availability || '|' || gamelist || '|' || alternative_emulator FROM idx_systems "
                         "WHERE library = 'esde' AND name = 'gb'")
              .value() == "Test System gb|[\"gb\",\"test-platform\"]|%ROMPATH%\\gb|I:\\Giochi\\gb|available|read|"
                          "Fake Emulator (Standalone)");
}

TEST_CASE("catalog_db/store: the import transaction: its own reads see its writes, others see them once committed, "
          "a dropped one leaves nothing, and imports never overlap",
          "[catalog_db]") {
    StoreFixture fixture("rv-store-transaction");
    SqliteCatalogStore& store = *fixture.store;
    const LibraryProfile library = rv::test::testLibrary();
    const SystemName gb = systemNamed("gb");
    const IndexedEntry tiny = indexedEntry(GameId(), fullEntry("gb", "Tiny Tennis (Europe).gb", "Tiny Tennis"));
    const GameId tinyId = gameIdOf(library.id, tiny.entry);
    IndexedEntry indexed = tiny;
    indexed.id = tinyId;

    // Another process: another holder of the database and its own store; and a reader of this one.
    auto other = Database::open(fixture.database.path());
    REQUIRE(other.has_value());
    auto otherStore = SqliteCatalogStore::open(*other);
    REQUIRE(otherStore.has_value());
    auto reader = fixture.database.openReader();
    REQUIRE(reader.has_value());
    const auto readerCount = [&] {
        return reader->queryInt("SELECT count(*) FROM rv_game_identities").value();
    };

    {
        auto transaction = store.beginImport(library.id);
        REQUIRE(transaction.has_value());
        // One import at a time: this store, another store, another process.
        const auto again = store.beginImport(library.id);
        CHECK(codeOf(again) == ErrorCode::Conflict);
        CHECK(codeOf((*otherStore)->beginImport(library.id)) == ErrorCode::Conflict);

        IImportTransaction& writes = **transaction;
        REQUIRE(writes.putLibrary(library).has_value());
        REQUIRE(writes.putSystem(library.id, rv::test::testSystem("gb", L"I:\\gb")).has_value());
        const auto orphan = writes.putEntry(indexed);
        REQUIRE_FALSE(orphan.has_value()); // its identity comes first
        CHECK(orphan.error().code == ErrorCode::InvalidArgument);
        REQUIRE(writes.putIdentity(identityOf(tinyId, library.id, indexed.entry)).has_value());
        REQUIRE(writes.putEntry(indexed).has_value());

        // The store's own reads see the open transaction's writes ...
        CHECK(store.libraries().value().size() == 1);
        CHECK(store.identities(library.id, gb).value().size() == 1);
        CHECK(store.identity(tinyId).value().has_value());
        CHECK(store.takenGameIds().value() == std::vector<GameId>{tinyId});
        CHECK(store.indexedEntries(library.id, gb).value() == std::vector<IndexedEntry>{indexed});
        // ... other connections and processes do not, and do not wait for it.
        CHECK(readerCount() == 0);
        CHECK((*otherStore)->libraries().value().empty());
        CHECK((*otherStore)->identities(library.id, gb).value().empty());
        CHECK((*otherStore)->takenGameIds().value().empty());
    }
    // Dropped without commit(): rolled back, the import lock released.
    CHECK(store.libraries().value().empty());
    CHECK(store.identities(library.id, gb).value().empty());
    CHECK(store.takenGameIds().value().empty());
    CHECK(fixture.database.writer().queryInt("SELECT count(*) FROM idx_games").value() == 0);
    CHECK(fixture.database.writer().queryInt("SELECT count(*) FROM idx_systems").value() == 0);
    {
        auto transaction = (*otherStore)->beginImport(library.id); // the other process imports now
        REQUIRE(transaction.has_value());
        CHECK(codeOf(store.beginImport(library.id)) == ErrorCode::Conflict);
        IImportTransaction& writes = **transaction;
        REQUIRE(writes.putLibrary(library).has_value());
        // The same tuple gives the same GameId again.
        const std::vector<GameId> taken = (*otherStore)->takenGameIds().value();
        const auto assigned =
            assignGameIds(library.id, std::vector<GameIdRequest>{{gb, indexed.entry.key}},
                          [&](GameId id) { return std::binary_search(taken.begin(), taken.end(), id); });
        REQUIRE(assigned.has_value());
        CHECK(assigned->at(0).id == tinyId);
        REQUIRE(writes.putIdentity(identityOf(tinyId, library.id, indexed.entry)).has_value());
        REQUIRE(writes.putEntry(indexed).has_value());
        CHECK(readerCount() == 0);
        REQUIRE(writes.commit().has_value());
        // The transaction has ended: later calls fail.
        CHECK(codeOf<void>(writes.putLibrary(library)) == ErrorCode::InvalidArgument);
        CHECK(codeOf<void>(writes.removeEntry(tinyId)) == ErrorCode::InvalidArgument);
        CHECK(codeOf<void>(writes.commit()) == ErrorCode::InvalidArgument);
        // Committed: everyone sees the writes, and the import lock is free while the ended transaction lives.
        CHECK(readerCount() == 1);
        CHECK(store.identities(library.id, gb).value().size() == 1);
        CHECK(store.indexedEntries(library.id, gb).value() == std::vector<IndexedEntry>{indexed});
        auto next = store.beginImport(library.id);
        CHECK(next.has_value());
    }
}

TEST_CASE("catalog_db/store: a store that goes rolls its open import back, and the transaction fails from then on",
          "[catalog_db]") {
    const rv::test::TempDir dir("rv-store-gone");
    auto database = rv::catalog_db::openCatalogDatabase(rv::test::databaseFileIn(dir));
    REQUIRE(database.has_value());
    auto store = SqliteCatalogStore::open(*database);
    REQUIRE(store.has_value());
    auto transaction = (*store)->beginImport(LibraryId());
    REQUIRE(transaction.has_value());
    REQUIRE((*transaction)->putLibrary(rv::test::testLibrary()).has_value());
    store->reset();
    CHECK(codeOf<void>((*transaction)->putLibrary(rv::test::testLibrary())) == ErrorCode::InvalidArgument);
    CHECK(codeOf<void>((*transaction)->commit()) == ErrorCode::InvalidArgument);
    CHECK(database->writer().queryInt("SELECT count(*) FROM rv_libraries").value() == 0);
    CHECK_FALSE(database->lock().holdsImport());
}

TEST_CASE("catalog_db/store: when SQLite rolls the import back on its own, the transaction ends and nothing is written "
          "outside it",
          "[catalog_db]") {
    StoreFixture fixture("rv-store-rolled-back");
    Connection& writer = fixture.database.writer();
    const LibraryProfile library = rv::test::testLibrary();
    auto transaction = fixture.store->beginImport(library.id);
    REQUIRE(transaction.has_value());
    REQUIRE((*transaction)->putLibrary(library).has_value());

    // A write interrupted inside the transaction makes SQLite roll all of it back, as a full disk or an I/O error does.
    std::atomic<bool> done{false};
    std::optional<rv::Status> interrupted;
    std::thread worker([&] {
        interrupted.emplace(writer.execute("WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM n LIMIT "
                                           "200000000) INSERT INTO rv_meta (key, value) SELECT 'n' || x, '' FROM n"));
        done = true;
    });
    while (!done) {
        fixture.database.interrupt();
        std::this_thread::sleep_for(5ms);
    }
    worker.join();
    REQUIRE(interrupted.has_value());
    CHECK(codeOf<void>(*interrupted) == ErrorCode::Cancelled);
    CHECK_FALSE(writer.inTransaction());

    // The import transaction has ended with it: its writes never go to the database one by one.
    const auto after = (*transaction)->putLibrary(library);
    CHECK(codeOf<void>(after) == ErrorCode::InvalidArgument);
    CHECK(contains(after.error().message, "rolled it back"));
    CHECK(codeOf<void>((*transaction)->commit()) == ErrorCode::InvalidArgument);
    CHECK(writer.queryInt("SELECT count(*) FROM rv_libraries").value() == 0);
    CHECK(writer.queryInt("SELECT count(*) FROM rv_meta WHERE key GLOB 'n*'").value() == 0);
    CHECK(fixture.store->beginImport(library.id).has_value()); // the import lock was released
}

TEST_CASE("catalog_db/store: what the import transaction refuses, and that it goes on after a refusal",
          "[catalog_db]") {
    StoreFixture fixture("rv-store-refusals");
    const LibraryProfile library = rv::test::testLibrary();
    auto transaction = fixture.store->beginImport(library.id);
    REQUIRE(transaction.has_value());
    IImportTransaction& writes = **transaction;
    REQUIRE(writes.putLibrary(library).has_value());

    const CatalogEntry entry = fullEntry("gb", "Alpha Quest (USA).gb", "Alpha Quest");
    const GameId id = gameIdOf(library.id, entry);
    const StoredIdentity identity = identityOf(id, library.id, entry);

    // Identities: an unknown library; no key; the key and path of another identity.
    StoredIdentity elsewhere = identity;
    elsewhere.library = LibraryId::parse("unknown").value();
    const auto unknownLibrary = writes.putIdentity(elsewhere);
    CHECK(codeOf<void>(unknownLibrary) == ErrorCode::InvalidArgument);
    CHECK(contains(unknownLibrary.error().message, "is not in the store"));
    StoredIdentity keyless = identity;
    keyless.key = LookupKey();
    CHECK(codeOf<void>(writes.putIdentity(keyless)) == ErrorCode::InvalidArgument);
    REQUIRE(writes.putIdentity(identity).has_value());
    StoredIdentity twin = identity;
    twin.id = gameIdOf(library.id, entry, 1);
    CHECK(codeOf<void>(writes.putIdentity(twin)) == ErrorCode::Conflict);
    // A file has one identity, also when the other's key was stored with another casing table (a re-keying not
    // finished): the path decides, not the key.
    REQUIRE(fixture.database.writer()
                .execute(fmt::format("UPDATE rv_game_identities SET lookup_key = 'KEY OF AN OLDER TABLE' WHERE id = {}",
                                     id.value()))
                .has_value());
    const auto samePath = writes.putIdentity(twin);
    REQUIRE_FALSE(samePath.has_value());
    CHECK(samePath.error().code == ErrorCode::Conflict);
    CHECK(contains(samePath.error().message, "another identity of its system has its relative path"));

    // Systems of an unknown library.
    CHECK(codeOf<void>(writes.putSystem(elsewhere.library, rv::test::testSystem("gb", L"I:\\gb"))) ==
          ErrorCode::InvalidArgument);

    // Entries: outside the system folder (no key); another system than the identity's; values the schema refuses.
    IndexedEntry indexed = indexedEntry(id, entry);
    IndexedEntry outside = indexed;
    outside.entry.state = EntryState::OutsideSystemFolder;
    outside.entry.key = LookupKey();
    outside.entry.gamelistPath = "./../snes/Alpha Quest (USA).sfc";
    CHECK(codeOf<void>(writes.putEntry(outside)) == ErrorCode::InvalidArgument);
    IndexedEntry otherSystem = indexed;
    otherSystem.entry.system = systemNamed("gbc");
    CHECK(codeOf<void>(writes.putEntry(otherSystem)) == ErrorCode::InvalidArgument);
    IndexedEntry extension = indexed;
    extension.media[MediaKind::Marquee] = MediaPresence{false, ".png"};
    CHECK(codeOf<void>(writes.putEntry(extension)) == ErrorCode::InvalidArgument);
    IndexedEntry count = indexed;
    count.entry.flags.playCount = std::numeric_limits<std::uint64_t>::max();
    CHECK(codeOf<void>(writes.putEntry(count)) == ErrorCode::InvalidArgument);
    IndexedEntry date = indexed;
    date.entry.metadata.releaseDate = ReleaseDate{1997, 13, 2};
    CHECK(codeOf<void>(writes.putEntry(date)) == ErrorCode::InvalidArgument);
    IndexedEntry rating = indexed;
    rating.entry.metadata.rating = Rating{11};
    CHECK(codeOf<void>(writes.putEntry(rating)) == ErrorCode::ValidationFailed);
    IndexedEntry players = indexed;
    players.entry.metadata.players = Players{0, 2};
    CHECK(codeOf<void>(writes.putEntry(players)) == ErrorCode::ValidationFailed);
    IndexedEntry played = indexed;
    played.entry.flags.lastPlayed = LocalDateTime{2026, 2, 30, 21, 0, 0};
    CHECK(codeOf<void>(writes.putEntry(played)) == ErrorCode::InvalidArgument);
    IndexedEntry notText = indexed;
    notText.entry.raw.unknownElements.push_back(RawElement{"region", std::string("\xFF\xFE", 2)});
    CHECK(codeOf<void>(writes.putEntry(notText)) == ErrorCode::InvalidArgument);
    CHECK(codeOf<void>(writes.removeEntry(GameId())) == ErrorCode::InvalidArgument);

    // A refused write leaves the transaction open and working.
    REQUIRE(writes.putEntry(indexed).has_value());
    REQUIRE(writes.commit().has_value());
    CHECK(fixture.store->indexedEntries(library.id, entry.system).value() == std::vector<IndexedEntry>{indexed});
}

TEST_CASE(
    "catalog_db/store: an entry SQLite refuses halfway through its write or removal leaves nothing of the write, and "
    "the transaction goes on",
    "[catalog_db]") {
    StoreFixture fixture("rv-store-halfway");
    Connection& writer = fixture.database.writer();
    const LibraryProfile library = rv::test::testLibrary();
    const CatalogEntry entry = fullEntry("gb", "Halfway (World).gb", "Halfway");
    const IndexedEntry indexed = indexedEntry(gameIdOf(library.id, entry), entry); // a cover and a video
    importAll(*fixture.store, library, {}, {indexed});
    // Statements SQLite refuses after the write's first ones ran, undoing only themselves as after a full disk: a media
    // row, an index row's removal (temporary triggers of the writer connection, the database's schema untouched).
    REQUIRE(writer
                .execute("CREATE TEMP TRIGGER refuse_media BEFORE INSERT ON idx_media WHEN NEW.extension = '.bad' "
                         "BEGIN SELECT RAISE(ABORT, 'media refused'); END;"
                         "CREATE TEMP TRIGGER refuse_removal BEFORE DELETE ON idx_games "
                         "BEGIN SELECT RAISE(ABORT, 'removal refused'); END")
                .has_value());

    auto transaction = fixture.store->beginImport(library.id);
    REQUIRE(transaction.has_value());
    IndexedEntry changed = indexed;
    changed.entry.title = "Changed";
    changed.media[MediaKind::Video] = MediaPresence{};
    changed.media[MediaKind::Marquee] = MediaPresence{true, ".bad"};
    const auto refused = (*transaction)->putEntry(changed); // after its row's upsert and its old media's removal
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::ValidationFailed);
    CHECK(contains(refused.error().message, "media refused"));
    const auto kept = (*transaction)->removeEntry(indexed.id); // after its media's removal
    REQUIRE_FALSE(kept.has_value());
    CHECK(kept.error().code == ErrorCode::ValidationFailed);
    CHECK(contains(kept.error().message, "removal refused"));

    // Nothing of either write is left: the entry and its media as they were, within the transaction and after it.
    CHECK(writer.inTransaction());
    CHECK(fixture.store->indexedEntries(library.id, entry.system).value() == std::vector<IndexedEntry>{indexed});
    REQUIRE(writer.execute("DROP TRIGGER refuse_media; DROP TRIGGER refuse_removal").has_value());
    REQUIRE((*transaction)->putEntry(indexed).has_value()); // and it goes on
    REQUIRE((*transaction)->commit().has_value());
    CHECK(fixture.store->indexedEntries(library.id, entry.system).value() == std::vector<IndexedEntry>{indexed});
    CHECK(writer.queryInt("SELECT count(*) FROM idx_media").value() == 2);
}

TEST_CASE("catalog_db/store: identities in (key, relative path) order, two names of one key two identities; index rows "
          "in entry order",
          "[catalog_db]") {
    StoreFixture fixture("rv-store-order");
    const LibraryProfile library = rv::test::testLibrary();
    std::vector<IndexedEntry> entries;
    std::vector<GameId> given;
    for (const std::string_view path : {"b.gb", "A.gb", "a.gb", "Sub/c.gb", "Z/a.gb", "\u00C9clair.gb"}) {
        CatalogEntry entry = rv::test::bareEntry("gb", path);
        // Two names that fold to one key: the second in (key, path) order takes counter 1, as assignGameIds() does.
        const std::uint32_t counter = path == "a.gb" ? 1 : 0;
        const GameId id = gameIdOf(library.id, entry, counter);
        entries.push_back(indexedEntry(id, std::move(entry), false));
        given.push_back(entries.back().id);
    }
    importAll(*fixture.store, library, {}, entries);

    const auto identities = fixture.store->identities(library.id, systemNamed("gb"));
    REQUIRE(identities.has_value());
    std::vector<std::string> paths;
    for (const StoredIdentity& identity : *identities) {
        paths.push_back(identity.key.relativePath());
    }
    // Ordinal by key, then path: "A.GB" holds "A.gb" then "a.gb"; "É" (C3 89 in UTF-8) after every ASCII key.
    CHECK(paths == std::vector<std::string>{"A.gb", "a.gb", "b.gb", "Sub/c.gb", "Z/a.gb", "\u00C9clair.gb"});
    CHECK((*identities)[0].key.key() == (*identities)[1].key.key());
    CHECK((*identities)[0].id != (*identities)[1].id);

    const auto indexed = fixture.store->indexedEntries(library.id, systemNamed("gb"));
    REQUIRE(indexed.has_value());
    REQUIRE(indexed->size() == entries.size());
    CHECK(std::is_sorted(indexed->begin(), indexed->end(),
                         [](const IndexedEntry& a, const IndexedEntry& b) { return entryOrder(a.entry, b.entry); }));
}

TEST_CASE("catalog_db/store: an identity keeps when it was first seen and its quick hash across imports; last seen "
          "follows its file",
          "[catalog_db]") {
    StoreFixture fixture("rv-store-seen");
    Connection& writer = fixture.database.writer();
    const LibraryProfile library = rv::test::testLibrary();
    const IndexedEntry entry = [&] {
        CatalogEntry e = fullEntry("gb", "Seen Game (Europe).gb", "Seen Game");
        const GameId id = gameIdOf(library.id, e);
        return indexedEntry(id, std::move(e));
    }();
    importAll(*fixture.store, library, {}, {entry});
    const auto seen = [&](std::string_view column) {
        return writer
            .queryText(fmt::format("SELECT {} FROM rv_game_identities WHERE id = {}", column, entry.id.value()))
            .value();
    };
    const auto firstSeen = seen("first_seen");
    REQUIRE(firstSeen.has_value());
    CHECK(seen("last_seen") == firstSeen);
    // The quick hash of I2 step 11b, written alone (the quick-hash pass), read back with the identity.
    QuickHash hash;
    hash.digest.fill(0xA5);
    hash.size = entry.entry.file->size;
    hash.lastWriteTime = entry.entry.file->lastWriteTime;
    {
        auto transaction = fixture.store->beginImport(library.id);
        REQUIRE(transaction.has_value());
        REQUIRE((*transaction)->putQuickHash(entry.id, hash).has_value());
        const auto nobody = (*transaction)->putQuickHash(GameId::fromValue(12345).value(), hash);
        REQUIRE_FALSE(nobody.has_value());
        CHECK(nobody.error().code == rv::ErrorCode::InvalidArgument); // no identity has that GameId
        REQUIRE((*transaction)->commit().has_value());
    }
    CHECK(fixture.store->identity(entry.id).value()->quickHash == hash);
    CHECK(seen("last_seen") == firstSeen); // the hash alone moves nothing else

    std::this_thread::sleep_for(5ms); // the times have milliseconds
    StoredIdentity gone = fixture.store->identity(entry.id).value().value();
    {
        // Its file gone at the next import: present no more, last seen kept; the identity written as read keeps its
        // quick hash.
        gone.present = false;
        auto transaction = fixture.store->beginImport(library.id);
        REQUIRE(transaction.has_value());
        REQUIRE((*transaction)->putIdentity(gone).has_value());
        REQUIRE((*transaction)->commit().has_value());
        CHECK(fixture.store->identity(entry.id).value() == gone);
        CHECK(gone.quickHash == hash);
    }
    CHECK(seen("first_seen") == firstSeen);
    CHECK(seen("last_seen") == firstSeen);
    std::this_thread::sleep_for(5ms);
    {
        StoredIdentity back = gone; // back
        back.present = true;
        auto transaction = fixture.store->beginImport(library.id);
        REQUIRE(transaction.has_value());
        REQUIRE((*transaction)->putIdentity(back).has_value());
        REQUIRE((*transaction)->commit().has_value());
    }
    CHECK(seen("first_seen") == firstSeen);
    CHECK(seen("last_seen").value() > firstSeen.value()); // ISO-8601 UTC text sorts like the times
    CHECK(
        writer
            .queryInt(fmt::format("SELECT length(quick_hash) FROM rv_game_identities WHERE id = {}", entry.id.value()))
            .value() == 32);
    // An identity written without a hash has none: the identity is its row.
    importAll(*fixture.store, library, {}, {entry});
    CHECK_FALSE(fixture.store->identity(entry.id).value()->quickHash.has_value());
}

TEST_CASE("catalog_db/store: removeEntry removes the index rows and the group the entry heads, keeps its identity and "
          "the groups of other heads",
          "[catalog_db]") {
    StoreFixture fixture("rv-store-remove");
    Connection& writer = fixture.database.writer();
    const LibraryProfile library = rv::test::testLibrary();
    const CatalogEntry playlist = rv::test::bareEntry("psx", "Twin Tracks (Europe).m3u");
    const CatalogEntry cue = rv::test::bareEntry("psx", "Twin Tracks (Europe).cue");
    const CatalogEntry track = rv::test::bareEntry("psx", "Twin Tracks (Europe) (Track 1).bin");
    const IndexedEntry head = indexedEntry(gameIdOf(library.id, playlist), playlist);
    const IndexedEntry middle = indexedEntry(gameIdOf(library.id, cue), cue);
    const IndexedEntry member = indexedEntry(gameIdOf(library.id, track), track, false);
    importAll(*fixture.store, library, {}, {head, middle, member});
    // The disc groups of I2 step 11: the playlist names the cue sheet, which names its track.
    REQUIRE(writer
                .execute(fmt::format("INSERT INTO idx_disc_groups (head, kind, head_size, head_time) VALUES ({0}, "
                                     "'m3u', 1, 2), ({1}, 'cue', 3, 4);"
                                     "INSERT INTO idx_disc_members (head, position, member) VALUES ({0}, 0, {1}), "
                                     "({1}, 0, {2})",
                                     head.id.value(), middle.id.value(), member.id.value()))
                .has_value());
    CHECK(writer.queryInt("SELECT count(*) FROM idx_media").value() == 4);

    auto transaction = fixture.store->beginImport(library.id);
    REQUIRE(transaction.has_value());
    REQUIRE((*transaction)->removeEntry(middle.id).has_value());
    REQUIRE((*transaction)->removeEntry(GameId::fromValue(12345).value()).has_value()); // nothing to remove: fine
    REQUIRE((*transaction)->commit().has_value());

    // The cue sheet's rows and its own group are gone; the playlist's group, which names it, is its head's and stays.
    const auto remaining = fixture.store->indexedEntries(library.id, playlist.system);
    REQUIRE(remaining.has_value());
    std::vector<GameId> left;
    for (const IndexedEntry& entry : *remaining) {
        left.push_back(entry.id);
    }
    std::sort(left.begin(), left.end());
    std::vector<GameId> expected{head.id, member.id};
    std::sort(expected.begin(), expected.end());
    CHECK(left == expected);
    CHECK(writer.queryInt(fmt::format("SELECT count(*) FROM idx_media WHERE game = {}", middle.id.value())).value() ==
          0);
    CHECK(writer.queryInt(fmt::format("SELECT count(*) FROM idx_disc_groups WHERE head = {}", middle.id.value()))
              .value() == 0);
    CHECK(writer.queryInt(fmt::format("SELECT count(*) FROM idx_disc_members WHERE head = {}", middle.id.value()))
              .value() == 0);
    CHECK(writer
              .queryInt(fmt::format("SELECT count(*) FROM idx_disc_members WHERE head = {} AND member = {}",
                                    head.id.value(), middle.id.value()))
              .value() == 1);
    CHECK(fixture.store->identity(middle.id).value().has_value());
    CHECK(fixture.store->takenGameIds().value().size() == 3);
}

TEST_CASE("catalog_db/store: taken GameIds are the identities' and the aliases', ascending", "[catalog_db]") {
    StoreFixture fixture("rv-store-taken");
    const LibraryProfile library = rv::test::testLibrary();
    const IndexedEntry entry = indexedEntry(GameId(), rv::test::bareEntry("gb", "Kept (World).gb"));
    IndexedEntry kept = entry;
    kept.id = gameIdOf(library.id, entry.entry);
    importAll(*fixture.store, library, {}, {kept});
    // An alias recorded by reconciliation (I2 step 11): its GameId is never given out again.
    REQUIRE(fixture.database.writer()
                .execute(fmt::format("INSERT INTO rv_identity_aliases (alias, game, lookup_key, relative_path, at, "
                                     "reason) VALUES (5, {0}, 'OLD.GB', 'old.gb', 'now', 'moved-back'), (9, {0}, "
                                     "'OLDER.GB', 'older.gb', 'now', 'review-resolved')",
                                     kept.id.value()))
                .has_value());
    std::vector<GameId> expected{GameId::fromValue(5).value(), GameId::fromValue(9).value(), kept.id};
    std::sort(expected.begin(), expected.end());
    CHECK(fixture.store->takenGameIds().value() == expected);
}

TEST_CASE("catalog_db/store: identities keep their key version; rv_meta records the version of every key at each "
          "commit",
          "[catalog_db]") {
    StoreFixture fixture("rv-store-key-version");
    Connection& writer = fixture.database.writer();
    const LibraryProfile library = rv::test::testLibrary();
    const CatalogEntry entry = rv::test::bareEntry("gb", "Versioned (World).gb");
    const CatalogEntry other = rv::test::bareEntry("gb", "Other (World).gb");
    StoredIdentity identity = identityOf(gameIdOf(library.id, entry), library.id, entry);
    StoredIdentity second = identityOf(gameIdOf(library.id, other), library.id, other);
    identity.keyVersion = LookupKeyVersion{kLookupKeyRule, "older.table"};
    second.keyVersion = identity.keyVersion;
    const auto meta = [&](std::string_view key) {
        return writer.queryText(fmt::format("SELECT value FROM rv_meta WHERE key = '{}'", key)).value().value();
    };
    const auto importIdentities = [&](const std::vector<StoredIdentity>& identities) {
        auto transaction = fixture.store->beginImport(library.id);
        REQUIRE(transaction.has_value());
        REQUIRE((*transaction)->putLibrary(library).has_value());
        for (const StoredIdentity& stored : identities) {
            REQUIRE((*transaction)->putIdentity(stored).has_value());
        }
        REQUIRE((*transaction)->commit().has_value());
    };
    // Until an identity exists: the version of the system that made the schema.
    CHECK(meta("casing_table_version") == currentLookupKeyVersion().casingTable);
    importIdentities({identity, second});
    CHECK(fixture.store->identity(identity.id).value() == identity);
    CHECK(meta("casing_table_version") == "older.table");
    CHECK(meta("lookup_key_rule") == std::to_string(kLookupKeyRule));

    // Computed again with the running system's table, a key is stored in place with its version; while the other key
    // is of the older table, the keys are of two versions.
    identity.keyVersion = currentLookupKeyVersion();
    importIdentities({identity});
    CHECK(fixture.store->identity(identity.id).value() == identity);
    CHECK(meta("casing_table_version") == "mixed");
    CHECK(meta("lookup_key_rule") == "mixed");
    // Every key of the running system's version.
    second.keyVersion = currentLookupKeyVersion();
    importIdentities({second});
    CHECK(meta("casing_table_version") == currentLookupKeyVersion().casingTable);
    CHECK(meta("lookup_key_rule") == std::to_string(kLookupKeyRule));
}

TEST_CASE("catalog_db/store: the raw ES-DE values keep any text; JSON escapes are read; a damaged row is a ParseError",
          "[catalog_db]") {
    StoreFixture fixture("rv-store-raw");
    Connection& writer = fixture.database.writer();
    const LibraryProfile library = rv::test::testLibrary();
    CatalogEntry entry = rv::test::bareEntry("gb", "Raw (World).gb");
    entry.raw.unknownElements = {
        {"quotes", "a \"quoted\" back\\slash and a /slash"},
        {"controls", std::string("tab\tline\ncarriage\rnul\0bell\x07unit\x1f"
                                 "del\x7F"sv)}, // the NUL kept: the literal's whole length
        {"text", "Pokémon 日本 \U0001F3AE"},
        {"empty", ""},
        {"", "a nameless one"},
    };
    entry.raw.originalForms = {{"releasedate", " 19970702T000000\n"}};
    const GameId id = gameIdOf(library.id, entry);
    const IndexedEntry indexed = indexedEntry(id, entry, false);
    importAll(*fixture.store, library, {}, {indexed});
    CHECK(fixture.store->indexedEntries(library.id, entry.system).value() == std::vector<IndexedEntry>{indexed});
    // What the store wrote is JSON SQLite reads too.
    CHECK(writer
              .queryText(
                  fmt::format("SELECT json_extract(raw, '$.unknown[2][1]') FROM idx_games WHERE id = {}", id.value()))
              .value() == "Pokémon 日本 \U0001F3AE");

    // JSON another writer might have used: every escape of RFC 8259, white space between the tokens.
    const auto setRaw = [&](std::string_view json) {
        auto update = writer.prepare("UPDATE idx_games SET raw = ?1 WHERE id = ?2");
        REQUIRE(update.has_value());
        update->bindText(1, json).bindInt(2, static_cast<std::int64_t>(id.value()));
        return update->run();
    };
    REQUIRE(setRaw(R"( { "original" : [ [ "a\/b" , "é🎮\b\f\"" ] ] , "unknown" : [ ] } )").has_value());
    const auto escaped = fixture.store->indexedEntries(library.id, entry.system);
    REQUIRE(escaped.has_value());
    REQUIRE(escaped->size() == 1);
    CHECK(escaped->front().entry.raw.unknownElements.empty());
    CHECK(escaped->front().entry.raw.originalForms == std::vector<RawElement>{{"a/b", "é\U0001F3AE\b\f\""}});

    // Valid JSON of another shape, or a lone surrogate: the row cannot be read back.
    for (const std::string_view damaged :
         {R"({"unknown":[["only a name"]]})", R"({"other":[]})", R"([])", R"({"unknown":[["a","\ud83c"]]})",
          R"({"unknown":[["a","b"]],"unknown":[]})"}) {
        INFO(damaged);
        REQUIRE(setRaw(damaged).has_value());
        CHECK(codeOf(fixture.store->indexedEntries(library.id, entry.system)) == ErrorCode::ParseError);
    }
}

TEST_CASE("catalog_db/store: 100000 identities and index rows in one transaction (10000 in Debug; timed, informative)",
          "[catalog_db]") {
    StoreFixture fixture("rv-store-100k");
    const LibraryProfile library = rv::test::testLibrary();
    constexpr std::size_t kSystems = 4;
    // 100,000 rows where the build is optimized (the gate runs the Release unit tests); a tenth in Debug, whose SQLite
    // (SQLITE_DEBUG) checks every page it touches and would take minutes over the same rows.
#if defined(NDEBUG)
    constexpr std::size_t kPerSystem = 25'000;
#else
    constexpr std::size_t kPerSystem = 2'500;
#endif
    const std::array<std::string_view, kSystems> systemNames{"gb", "nes", "snes", "psx"};

    std::vector<IndexedEntry> entries;
    entries.reserve(kSystems * kPerSystem);
    for (std::size_t s = 0; s < kSystems; ++s) {
        for (std::size_t i = 0; i < kPerSystem; ++i) {
            const std::string path = fmt::format("Collection {:02}/Game {:06} (Europe) [b{}].zip", i % 50, i, s);
            CatalogEntry entry = (i % 3 == 0) ? rv::test::bareEntry(systemNames[s], path)
                                              : fullEntry(systemNames[s], path, fmt::format("Game {:06}", i));
            const GameId id = gameIdOf(library.id, entry);
            entries.push_back(indexedEntry(id, std::move(entry), i % 2 == 0));
        }
    }
    std::vector<SystemInfo> systems;
    for (const std::string_view name : systemNames) {
        systems.push_back(rv::test::testSystem(name, std::filesystem::path(L"I:\\Giochi") / std::string(name)));
    }
    // Written in GameId order, as an import should: the rows then append to the tables' B-trees instead of landing on
    // random pages (about three times faster in Release here, where the cache does not hold the tables).
    std::sort(entries.begin(), entries.end(), [](const IndexedEntry& a, const IndexedEntry& b) { return a.id < b.id; });

    const auto started = std::chrono::steady_clock::now();
    importAll(*fixture.store, library, systems, entries);
    const auto written = std::chrono::steady_clock::now();
    std::size_t identities = 0;
    std::vector<std::vector<IndexedEntry>> indexed;
    for (const SystemInfo& system : systems) {
        identities += fixture.store->identities(library.id, system.name).value().size();
        indexed.push_back(fixture.store->indexedEntries(library.id, system.name).value());
    }
    const auto read = std::chrono::steady_clock::now();
    CHECK(identities == entries.size());
    std::size_t rows = 0;
    for (const std::vector<IndexedEntry>& system : indexed) {
        rows += system.size();
    }
    CHECK(rows == entries.size());
    CHECK(fixture.store->takenGameIds().value().size() == entries.size());

    const auto ms = [](auto duration) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    };
    const auto info = rv::catalog_db::describeDatabase(fixture.database).value();
    WARN(fmt::format("Informative timing (I2 step 4): {} identities and index rows written in one transaction in {} ms "
                     "(commit and checkpoint included), read back per system in {} ms; database {} pages of {} bytes",
                     entries.size(), ms(written - started), ms(read - written), info.pageCount, info.pageSize));

    // A sample of rows comes back unchanged.
    for (const std::size_t i : {std::size_t{0}, std::size_t{1}, entries.size() / 3, entries.size() - 1}) {
        const IndexedEntry& expected = entries[i];
        INFO(expected.entry.key.relativePath());
        const std::size_t system = static_cast<std::size_t>(
            std::find(systemNames.begin(), systemNames.end(), expected.entry.system.str()) - systemNames.begin());
        REQUIRE(system < indexed.size());
        const auto found = std::find_if(indexed[system].begin(), indexed[system].end(),
                                        [&](const IndexedEntry& e) { return e.id == expected.id; });
        REQUIRE(found != indexed[system].end());
        CHECK(*found == expected);
    }
}
