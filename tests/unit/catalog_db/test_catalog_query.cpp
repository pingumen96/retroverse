// I2 step 9: SqliteCatalogQuery, rv_catalog's ICatalogQuery on the catalog database (ADR-0008 §5), and the search
// area of the schema: the FTS5 index kept in step with idx_games and idx_systems by its triggers; search text split by
// the index's own tokenizer (diacritics and case folded, words in any order, the last one a prefix, one character
// without FTS5, FTS5 syntax in user text inert); the filters; the total orders and their ties; positions and pages;
// rows fetched by ids; a reader that never waits on a writer's open transaction; a generated catalog checked against
// an oracle; the systems with their counts; a game's details.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog/identity.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/query.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/catalog_db/search_index.hpp>

#include "catalog_db_test_support.hpp"
#include "support/catalog_generator.hpp"
#include "support/temp_dir.hpp"

using namespace rv::catalog;
using namespace std::chrono_literals;
using rv::ErrorCode;
using rv::catalog_db::Connection;
using rv::catalog_db::Database;
using rv::catalog_db::GameDetails;
using rv::catalog_db::SearchPlan;
using rv::catalog_db::SqliteCatalogQuery;
using rv::catalog_db::SqliteCatalogStore;
using rv::catalog_db::SystemSummary;

/// GameIds in failure messages by their text form.
template <>
struct Catch::StringMaker<rv::catalog::GameId> {
    static std::string convert(rv::catalog::GameId id) { return id.toString(); }
};

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

/// A game of `system`: a listed file of `relativePath` titled `title` by its gamelist entry, nothing else set.
IndexedEntry game(const LibraryId& library, std::string_view system, std::string_view relativePath,
                  std::string_view title) {
    CatalogEntry entry = rv::test::bareEntry(system, relativePath);
    entry.gamelistPath = "./" + std::string(relativePath);
    entry.title = std::string(title);
    entry.metadata.name = std::string(title);
    const GameId id = rv::test::gameIdOf(library, entry);
    return rv::test::indexedEntry(id, std::move(entry), false);
}

/// `entry` changed by `edit`, its sort key computed again.
IndexedEntry edited(IndexedEntry entry, const std::function<void(CatalogEntry&)>& edit) {
    edit(entry.entry);
    entry.sortKey = sortKeyOf(entry.entry).value();
    return entry;
}

SystemInfo system(std::string_view name, std::string_view fullName) {
    SystemInfo info = rv::test::testSystem(name, std::filesystem::path(L"E:\\roms") / std::string(name));
    info.fullName = std::string(fullName);
    return info;
}

/// A migrated database in a test's folder, its store on the writer and its queries on a reader.
struct QueryFixture {
    explicit QueryFixture(std::string_view prefix)
        : dir(prefix), database(rv::catalog_db::openCatalogDatabase(rv::test::databaseFileIn(dir)).value()),
          store(SqliteCatalogStore::open(database).value()),
          query(SqliteCatalogQuery::open(database.openReader().value()).value()) {}

    /// Writes `systems` and `entries` (with their identities) in one import, after the library.
    void import(const std::vector<SystemInfo>& systems, const std::vector<IndexedEntry>& entries) {
        auto transaction = store->beginImport(library.id);
        REQUIRE(transaction.has_value());
        REQUIRE((*transaction)->putLibrary(library).has_value());
        for (const SystemInfo& info : systems) {
            REQUIRE((*transaction)->putSystem(library.id, info).has_value());
        }
        for (const IndexedEntry& entry : entries) {
            REQUIRE((*transaction)->putIdentity(rv::test::identityOf(entry.id, library.id, entry.entry)).has_value());
            REQUIRE((*transaction)->putEntry(entry).has_value());
        }
        REQUIRE((*transaction)->commit().has_value());
    }

    /// The ids `text` finds among the games (Title order).
    std::vector<GameId> search(std::string_view text) {
        CatalogQuery request;
        request.text = std::string(text);
        auto ids = query->run(request);
        REQUIRE(ids.has_value());
        return *ids;
    }

    std::vector<GameId> run(const CatalogQuery& request) {
        auto ids = query->run(request);
        REQUIRE(ids.has_value());
        return *ids;
    }

    /// The titles `text` finds, in Title order.
    std::vector<std::string> titles(std::string_view text) {
        const std::vector<GameId> ids = search(text);
        auto rows = query->fetch(ids);
        REQUIRE(rows.has_value());
        std::vector<std::string> result;
        for (const CatalogRow& row : *rows) {
            result.push_back(row.title);
        }
        return result;
    }

    /// db check, whose FTS5 integrity-check with rank 1 compares the search index with its content.
    bool checked() {
        auto report = rv::catalog_db::checkDatabase(database);
        REQUIRE(report.has_value());
        for (const auto& check : report->checks) {
            for (const std::string& detail : check.details) {
                UNSCOPED_INFO(check.name << ": " << detail);
            }
        }
        return report->passed();
    }

    rv::test::TempDir dir;
    Database database;
    std::unique_ptr<SqliteCatalogStore> store;
    std::unique_ptr<SqliteCatalogQuery> query;
    LibraryProfile library = rv::test::testLibrary();
};

std::vector<GameId> idsOf(const std::vector<IndexedEntry>& entries) {
    std::vector<GameId> ids;
    for (const IndexedEntry& entry : entries) {
        ids.push_back(entry.id);
    }
    return ids;
}

/// `entries` in Title's order: (sort key, GameId).
std::vector<GameId> titleOrder(std::vector<IndexedEntry> entries) {
    std::sort(entries.begin(), entries.end(), [](const IndexedEntry& a, const IndexedEntry& b) {
        return std::tie(a.sortKey, a.id) < std::tie(b.sortKey, b.id);
    });
    return idsOf(entries);
}

} // namespace

TEST_CASE("catalog_db/search: the search area: an FTS5 index with ADR-0008's tokenizer and prefixes over a view of "
          "idx_games and idx_systems, its triggers and the order index",
          "[catalog_db][search]") {
    QueryFixture fixture("rv-search-schema");
    Connection& writer = fixture.database.writer();
    const std::string search =
        writer.queryText("SELECT sql FROM sqlite_schema WHERE name = 'idx_game_search'").value().value();
    CHECK(search.starts_with("CREATE VIRTUAL TABLE idx_game_search USING fts5("));
    CHECK(contains(search, "title, sort_name, developer, publisher, genre, year, system_full_name"));
    CHECK(contains(search, "content = 'idx_game_search_content', content_rowid = 'id'"));
    // The tokenizer the queries split text with (catalog_query.cpp) has these very options.
    CHECK(contains(search, "tokenize = 'unicode61 remove_diacritics 2'"));
    CHECK(contains(search, "prefix = '2 3'"));
    CHECK_FALSE(contains(search, "description"));
    CHECK(writer.queryText("SELECT type FROM sqlite_schema WHERE name = 'idx_game_search_content'").value() == "view");
    for (const std::string_view trigger :
         {"idx_games_search_insert", "idx_games_search_delete", "idx_games_search_update", "idx_systems_search_insert",
          "idx_systems_search_delete", "idx_systems_search_update"}) {
        INFO(trigger);
        CHECK(writer.queryText(fmt::format("SELECT type FROM sqlite_schema WHERE name = '{}'", trigger)).value() ==
              "trigger");
    }
    CHECK(writer.queryText("SELECT sql FROM sqlite_schema WHERE name = 'idx_games_sort'").value() ==
          "CREATE INDEX idx_games_sort ON idx_games (sort_key, id, state, availability, library, system, "
          "release_date)");
    CHECK(fixture.checked());
}

TEST_CASE("catalog_db/search: the order index gives the lists' total order: a list, a first character's range and the "
          "systems' counts are read from it without a sort",
          "[catalog_db][search]") {
    QueryFixture fixture("rv-search-plans");
    Connection& writer = fixture.database.writer();
    // The plan SQLite makes for a statement: its steps' details, one per line.
    const auto plan = [&](std::string_view sql) {
        auto explain = writer.prepare(fmt::format("EXPLAIN QUERY PLAN {}", sql));
        REQUIRE(explain.has_value());
        std::string steps;
        while (explain->step().value()) {
            steps += fmt::format("{}\n", explain->columnText(3));
        }
        return steps;
    };
    // The statements of catalog_query.cpp's readList() and systems(), for each order's columns, with the systems filter
    // and with a first character's range.
    const std::string filters = "g.state IN ('game', 'missing-file') AND g.availability = 'available'";
    for (const std::string_view columns : {"g.id", "g.id, g.library, g.system", "g.id, g.release_date"}) {
        for (const std::string_view extra :
             {"", " AND g.system IN (SELECT value FROM json_each(?3))", " AND g.sort_key >= ?5 AND g.sort_key < ?6"}) {
            const std::string steps = plan(fmt::format("SELECT {} FROM idx_games AS g INDEXED BY idx_games_sort WHERE "
                                                       "{}{} ORDER BY g.sort_key, g.id",
                                                       columns, filters, extra));
            INFO(steps);
            CHECK(contains(steps, "USING COVERING INDEX idx_games_sort"));
            CHECK_FALSE(contains(steps, "TEMP B-TREE"));
        }
    }
    const std::string counts = plan("SELECT library, system, state, availability FROM idx_games INDEXED BY "
                                    "idx_games_sort");
    INFO(counts);
    CHECK(contains(counts, "USING COVERING INDEX idx_games_sort"));
    CHECK_FALSE(contains(counts, "TEMP B-TREE"));
}

TEST_CASE("catalog_db/search: the triggers keep the index in step: upserts that change or keep a row, removals, a "
          "system's full name changed, a system written after its games, removed or renamed",
          "[catalog_db][search]") {
    QueryFixture fixture("rv-search-triggers");
    const LibraryId& library = fixture.library.id;
    IndexedEntry puzzle = edited(game(library, "gb", "Pocket Puzzle (World).gb", "Pocket Puzzle"),
                                 [](CatalogEntry& e) { e.metadata.developer = "Mirage Soft"; });
    const IndexedEntry racer = game(library, "gb", "Harbor Racer (Europe).gb", "Harbor Racer");
    fixture.import({system("gb", "Portable Console")}, {puzzle, racer});
    CHECK(fixture.search("pocket") == std::vector{puzzle.id});
    CHECK(fixture.search("mirage") == std::vector{puzzle.id});
    CHECK(fixture.search("portable console") == titleOrder({puzzle, racer}));
    CHECK(fixture.checked());

    // Another import: one row changed, the other written again unchanged.
    const IndexedEntry renamed = edited(puzzle, [](CatalogEntry& e) {
        e.title = "Tower Puzzle";
        e.metadata.developer = "Kumo Works";
        e.metadata.releaseDate = ReleaseDate{1994, 3, 1};
    });
    fixture.import({system("gb", "Portable Console")}, {renamed, racer});
    CHECK(fixture.search("pocket").empty());
    CHECK(fixture.search("mirage").empty());
    CHECK(fixture.search("tower") == std::vector{puzzle.id});
    CHECK(fixture.search("kumo 1994") == std::vector{puzzle.id});
    CHECK(fixture.search("harbor racer") == std::vector{racer.id});
    CHECK(fixture.checked());

    // The system's full name changes: every one of its games is found by the new one only.
    fixture.import({system("gb", "Pocket Handheld")}, {});
    CHECK(fixture.search("portable console").empty());
    CHECK(fixture.search("handheld") == titleOrder({renamed, racer}));
    CHECK(fixture.checked());

    // A removed entry leaves the index.
    {
        auto transaction = fixture.store->beginImport(library);
        REQUIRE(transaction.has_value());
        REQUIRE((*transaction)->removeEntry(racer.id).has_value());
        REQUIRE((*transaction)->commit().has_value());
    }
    CHECK(fixture.search("harbor").empty());
    CHECK(fixture.search("handheld") == std::vector{puzzle.id});
    CHECK(fixture.checked());

    // A system written after its games, in the same import: its full name reaches them.
    const IndexedEntry disc = game(library, "psx", "Neon Circus (Europe).chd", "Neon Circus");
    {
        auto transaction = fixture.store->beginImport(library);
        REQUIRE(transaction.has_value());
        REQUIRE((*transaction)->putIdentity(rv::test::identityOf(disc.id, library, disc.entry)).has_value());
        REQUIRE((*transaction)->putEntry(disc).has_value());
        REQUIRE((*transaction)->putSystem(library, system("psx", "Disc Station")).has_value());
        REQUIRE((*transaction)->commit().has_value());
    }
    CHECK(fixture.search("disc station") == std::vector{disc.id});
    CHECK(fixture.checked());

    // Removed or renamed by hand (the store never does either): the games follow.
    Connection& writer = fixture.database.writer();
    REQUIRE(writer.execute("DELETE FROM idx_systems WHERE name = 'psx'").has_value());
    CHECK(fixture.search("disc station").empty());
    CHECK(fixture.search("neon") == std::vector{disc.id});
    CHECK(fixture.checked());
    REQUIRE(writer.execute("UPDATE idx_systems SET name = 'psx', full_name = 'Renamed Station' WHERE name = 'gb'")
                .has_value());
    CHECK(fixture.search("handheld").empty());
    CHECK(fixture.search("renamed station") == std::vector{disc.id});
    CHECK(fixture.checked());
    // A full rebuild (the repair) gives the same index.
    REQUIRE(writer.execute("INSERT INTO idx_game_search (idx_game_search) VALUES ('rebuild')").has_value());
    CHECK(fixture.search("renamed station") == std::vector{disc.id});
    CHECK(fixture.checked());
}

TEST_CASE("catalog_db/search: a bulk write suspends the search index and rebuilds it before its commit; a rollback "
          "keeps it; a commit in between is caught by db check and repaired",
          "[catalog_db][search]") {
    using rv::catalog_db::resumeSearchIndex;
    using rv::catalog_db::searchIndexFollowsWrites;
    using rv::catalog_db::suspendSearchIndex;
    QueryFixture fixture("rv-search-bulk");
    const LibraryId& library = fixture.library.id;
    Connection& writer = fixture.database.writer();
    const IndexedEntry quest = game(library, "gb", "Pixel Quest.gb", "Pixel Quest");
    const IndexedEntry racer = game(library, "gb", "Harbor Racer.gb", "Harbor Racer");
    fixture.import({system("gb", "Portable Console")}, {quest, racer});
    CHECK(searchIndexFollowsWrites(writer).value());

    // Outside a transaction both are refused: the triggers would be gone, or the index rebuilt, for good.
    CHECK(codeOf(suspendSearchIndex(writer)) == ErrorCode::InvalidArgument);
    CHECK(codeOf(resumeSearchIndex(writer)) == ErrorCode::InvalidArgument);
    CHECK(searchIndexFollowsWrites(writer).value());

    // A bulk import: a row changed, one added, one removed, a system's full name changed and a system added, all
    // written without the triggers; a reader meanwhile reads the last commit.
    const IndexedEntry tower = edited(quest, [](CatalogEntry& e) {
        e.title = "Tower Quest";
        e.metadata.developer = "Kumo Works";
    });
    const IndexedEntry disc = game(library, "psx", "Neon Circus.chd", "Neon Circus");
    {
        auto transaction = fixture.store->beginImport(library);
        REQUIRE(transaction.has_value());
        REQUIRE(suspendSearchIndex(writer).has_value());
        CHECK_FALSE(searchIndexFollowsWrites(writer).value());
        REQUIRE(suspendSearchIndex(writer).has_value()); // nothing more to drop
        REQUIRE((*transaction)->putEntry(tower).has_value());
        REQUIRE((*transaction)->putIdentity(rv::test::identityOf(disc.id, library, disc.entry)).has_value());
        REQUIRE((*transaction)->putEntry(disc).has_value());
        REQUIRE((*transaction)->removeEntry(racer.id).has_value());
        REQUIRE((*transaction)->putSystem(library, system("gb", "Pocket Handheld")).has_value());
        REQUIRE((*transaction)->putSystem(library, system("psx", "Disc Station")).has_value());
        CHECK(fixture.search("pixel") == std::vector{quest.id});
        CHECK(fixture.search("harbor") == std::vector{racer.id});
        REQUIRE(resumeSearchIndex(writer).has_value());
        CHECK(searchIndexFollowsWrites(writer).value());
        REQUIRE((*transaction)->commit().has_value());
    }
    CHECK(fixture.search("pixel").empty());
    CHECK(fixture.search("tower kumo") == std::vector{quest.id});
    CHECK(fixture.search("harbor").empty());
    CHECK(fixture.search("pocket handheld") == std::vector{quest.id});
    CHECK(fixture.search("portable").empty());
    CHECK(fixture.search("disc station neon") == std::vector{disc.id});
    CHECK(fixture.checked()); // the triggers' text as the program's schema has it, the index as its content
    // The triggers follow the writes again.
    const IndexedEntry finalQuest = edited(quest, [](CatalogEntry& e) { e.title = "Final Quest"; });
    fixture.import({}, {finalQuest});
    CHECK(fixture.search("final") == std::vector{quest.id});
    CHECK(fixture.search("tower").empty());

    // Rolled back: the triggers and the index as they were.
    {
        auto transaction = fixture.store->beginImport(library);
        REQUIRE(transaction.has_value());
        REQUIRE(suspendSearchIndex(writer).has_value());
        REQUIRE((*transaction)->putEntry(edited(quest, [](CatalogEntry& e) { e.title = "Lost Title"; })).has_value());
    } // never committed
    CHECK(searchIndexFollowsWrites(writer).value());
    CHECK(fixture.search("lost").empty());
    CHECK(fixture.search("final") == std::vector{quest.id});
    CHECK(fixture.checked());

    // Committed while suspended, a mistake: the index is behind its tables, db check says so, and a resume in a later
    // transaction repairs it.
    REQUIRE(writer.execute("BEGIN IMMEDIATE").has_value());
    REQUIRE(suspendSearchIndex(writer).has_value());
    REQUIRE(writer.execute(fmt::format("UPDATE idx_games SET title = 'Stale Title' WHERE id = {}", quest.id.value()))
                .has_value());
    REQUIRE(writer.execute("COMMIT").has_value());
    CHECK_FALSE(searchIndexFollowsWrites(writer).value());
    CHECK(fixture.search("stale").empty());
    CHECK_FALSE(fixture.checked());
    REQUIRE(writer.execute("BEGIN IMMEDIATE").has_value());
    REQUIRE(resumeSearchIndex(writer).has_value());
    REQUIRE(writer.execute("COMMIT").has_value());
    CHECK(fixture.search("stale") == std::vector{quest.id});
    CHECK(fixture.search("final").empty());
    CHECK(fixture.checked());
}

TEST_CASE("catalog_db/search: a generated catalog written with the search index suspended holds the index its "
          "triggers make row by row",
          "[catalog_db][search][generated]") {
    const rv::test::TempDir dir("rv-search-bulk-generated");
    // Every term of the index with the rows that hold it (the whole index: detail=none keeps nothing else), and the
    // database's checks.
    const auto indexOf = [](const std::filesystem::path& file) {
        auto database = rv::catalog_db::openCatalogDatabase(file);
        REQUIRE(database.has_value());
        Connection& writer = database->writer();
        CHECK(rv::catalog_db::searchIndexFollowsWrites(writer).value());
        REQUIRE(
            writer.execute("CREATE VIRTUAL TABLE temp.search_terms USING fts5vocab(main, idx_game_search, instance)")
                .has_value());
        auto terms = writer.prepare("SELECT term, doc FROM temp.search_terms ORDER BY term, doc");
        REQUIRE(terms.has_value());
        std::vector<std::pair<std::string, std::int64_t>> index;
        while (terms->step().value()) {
            index.emplace_back(terms->columnText(0), terms->columnInt(1));
        }
        terms = rv::catalog_db::Statement{};
        auto report = rv::catalog_db::checkDatabase(*database);
        REQUIRE(report.has_value());
        CHECK(report->passed());
        return index;
    };
    rv::test::CatalogGeneratorOptions options{800, 5};
    options.suspendSearchIndex = false;
    REQUIRE(rv::test::generateCatalogDatabase(dir.file("rows/retroverse.sqlite"), options, false).has_value());
    options.suspendSearchIndex = true;
    REQUIRE(rv::test::generateCatalogDatabase(dir.file("bulk/retroverse.sqlite"), options, false).has_value());
    const auto rows = indexOf(dir.file("rows/retroverse.sqlite"));
    CHECK(rows.size() > 800);
    CHECK(rows == indexOf(dir.file("bulk/retroverse.sqlite")));
}

TEST_CASE("catalog_db/search: case and diacritics are ignored, decomposed text found as composed text",
          "[catalog_db][search]") {
    QueryFixture fixture("rv-search-diacritics");
    const LibraryId& library = fixture.library.id;
    const IndexedEntry composed = game(library, "gb", "Eclair 1.gb",
                                       "\xC3\x89"
                                       "clair"); // Éclair
    const IndexedEntry decomposed = game(library, "gb", "Eclair 2.gb",
                                         "E\xCC\x81"
                                         "clair Nocturne"); // NFD
    const IndexedEntry plain = game(library, "gb", "Eclair 3.gb", "ECLAIR DE LUNE");
    const IndexedEntry castle = game(library, "gb", "Chateau.gb", "Ch\xC3\xA2teau Fort"); // Château
    const IndexedEntry other = game(library, "gb", "Pixel.gb", "Pixel Quest");
    fixture.import({system("gb", "Portable Console")}, {composed, decomposed, plain, castle, other});

    const std::vector<GameId> eclairs = titleOrder({composed, decomposed, plain});
    CHECK(fixture.search("eclair") == eclairs);
    CHECK(fixture.search("ECLAIR") == eclairs);
    CHECK(fixture.search("\xC3\xA9"
                         "clair") == eclairs); // éclair
    CHECK(fixture.search("e\xCC\x81"
                         "clair") == eclairs); // éclair, decomposed
    CHECK(fixture.search("\xC3\x89"
                         "CL") == eclairs); // ÉCL, a prefix
    CHECK(fixture.search("chateau") == std::vector{castle.id});
    CHECK(fixture.search("CH\xC3\x82") == std::vector{castle.id}); // CHÂ
    CHECK(fixture.search("fort ch\xC3\xA2teau") == std::vector{castle.id});
}

TEST_CASE("catalog_db/search: every word must match, in any order and any searched column, the last one as a prefix; "
          "descriptions are not searched",
          "[catalog_db][search]") {
    QueryFixture fixture("rv-search-words");
    const LibraryId& library = fixture.library.id;
    const IndexedEntry superQuest =
        edited(game(library, "gb", "Super Pixel Quest.gb", "Super Pixel Quest"), [](CatalogEntry& e) {
            e.metadata.developer = "Mirage Soft";
            e.metadata.publisher = "Harbor Lights";
            e.metadata.genre = "Role Playing Game";
            e.metadata.releaseDate = ReleaseDate{1997, 7, 2};
            e.metadata.description = "A quixotry of pixels.";
        });
    const IndexedEntry racer = edited(game(library, "snes", "Pixel Racer.sfc", "Pixel Racer"), [](CatalogEntry& e) {
        e.metadata.releaseDate = ReleaseDate{1993, 1, 1};
        e.metadata.sortName = "Velocity Pixel Racer";
    });
    const IndexedEntry pixels = edited(game(library, "gb", "Quest of Pixels.gb", "The Quest of Pixels"),
                                       [](CatalogEntry& e) { e.metadata.sortName = "Quest of Pixels, The"; });
    fixture.import({system("gb", "Portable Console"), system("snes", "Home Console")}, {superQuest, racer, pixels});

    CHECK(fixture.search("pixel quest") == std::vector{superQuest.id});       // "pixels" is not the word "pixel"
    CHECK(fixture.search("quest pixel") == titleOrder({superQuest, pixels})); // but starts with the prefix "pixel"
    CHECK(fixture.search("pix") == titleOrder({superQuest, racer, pixels}));
    CHECK(fixture.search("pi") == titleOrder({superQuest, racer, pixels})); // the prefix index of 2
    CHECK(fixture.search("pixel qu") == titleOrder({superQuest}));          // "pixel": a whole word
    CHECK(fixture.search("pix quest").empty());                             // only the last word is a prefix
    CHECK(fixture.search("mirage") == std::vector{superQuest.id});          // developer
    CHECK(fixture.search("harbor lights") == std::vector{superQuest.id});   // publisher
    CHECK(fixture.search("role playing") == std::vector{superQuest.id});    // genre
    CHECK(fixture.search("1997") == std::vector{superQuest.id});            // year
    CHECK(fixture.search("199") == titleOrder({superQuest, racer}));        // the prefix index of 3
    CHECK(fixture.search("home console pixel") == std::vector{racer.id});   // the system's full name
    CHECK(fixture.search("quest of pixels the") == std::vector{pixels.id});
    CHECK(fixture.search("velocity") == std::vector{racer.id}); // the sort title
    CHECK(fixture.search("quixotry").empty());                  // descriptions are not searched
    CHECK(fixture.search("pixel zzz").empty());
}

TEST_CASE("catalog_db/search: one character filters by the sort key's first character, without FTS5",
          "[catalog_db][search]") {
    QueryFixture fixture("rv-search-one-character");
    const LibraryId& library = fixture.library.id;
    const IndexedEntry pixel = game(library, "gb", "Pixel.gb", "Pixel Quest");
    const IndexedEntry puzzle = game(library, "gb", "Puzzle.gb", "puzzle tower");
    const IndexedEntry superPixel = game(library, "gb", "Super.gb", "Super Pixel");
    const IndexedEntry eclair = game(library, "gb", "Eclair.gb",
                                     "\xC3\x89"
                                     "clair"); // Éclair
    const IndexedEntry plain = game(library, "gb", "Echo.gb", "Echo");
    const IndexedEntry quoted = game(library, "gb", "Quoted.gb", "\"Quoted\" Title");
    // Written decomposed (NFD, as gamelists written on some systems hold it): "e" + U+0301, the sort key's first
    // character "é" once composed.
    const IndexedEntry nocturne = game(library, "gb", "Nocturne.gb",
                                       "E\xCC\x81"
                                       "clair Nocturne");
    const IndexedEntry decomposedEcho = game(library, "gb", "Echo 2.gb",
                                             "E\xCC\x81"
                                             "\xCC\x81"
                                             "cho"); // two acute accents: "é" and one that composes with nothing
    // "e" + U+0332 (a low line, which composes with no letter): the first character stays "e".
    const IndexedEntry underlined = game(library, "gb", "Underlined.gb",
                                         "e\xCC\xB2"
                                         "cho Line");
    fixture.import({system("gb", "Portable Console")},
                   {pixel, puzzle, superPixel, eclair, plain, quoted, nocturne, decomposedEcho, underlined});

    for (const std::string_view text : {"p", "P", " p ", "\tP\n"}) {
        INFO(text);
        const auto plan = fixture.query->searchPlan(text);
        REQUIRE(plan.has_value());
        CHECK(plan->kind == SearchPlan::Kind::FirstCharacter);
        CHECK(plan->prefix == "p");
        CHECK(plan->forms == std::vector<std::string>{"p"});
        CHECK(fixture.search(text) == titleOrder({pixel, puzzle})); // not "Super Pixel"
    }
    // "é" typed composed or decomposed, in either case: one character, the titles that start with it whichever form
    // they were written in; "e" only those that start with "e" once composed. Diacritics are not folded here.
    const std::vector<GameId> acute = titleOrder({eclair, nocturne, decomposedEcho});
    for (const std::string_view text : {"\xC3\xA9", "\xC3\x89", "e\xCC\x81", " E\xCC\x81 "}) {
        INFO(text);
        const auto plan = fixture.query->searchPlan(text);
        REQUIRE(plan.has_value());
        CHECK(plan->kind == SearchPlan::Kind::FirstCharacter);
        CHECK(plan->prefix == "\xC3\xA9");
        CHECK(plan->forms == std::vector<std::string>{"e\xCC\x81", "\xC3\xA9"}); // ascending
        CHECK(fixture.search(text) == acute);
    }
    CHECK(fixture.search("e") == titleOrder({plain, underlined}));
    CHECK(fixture.search("E") == titleOrder({plain, underlined}));
    CHECK(fixture.search("\"") == std::vector{quoted.id}); // any character, never FTS5 syntax
    CHECK(fixture.search("z").empty());
    // Every order and a filter keep the rule (the System order is made from the list of the forms read in turn).
    CatalogQuery bySystem{"\xC3\xA9"};
    bySystem.sort = SortOrder::System;
    CHECK(fixture.run(bySystem) == acute);
    bySystem.systems = {systemNamed("gb")};
    bySystem.sort = SortOrder::ReleaseDate;
    CHECK(fixture.run(bySystem) == acute);
    // Two characters are two words or one: FTS5 again.
    CHECK(fixture.query->searchPlan("pi").value().kind == SearchPlan::Kind::Words);
    CHECK(fixture.query->searchPlan("p q").value().kind == SearchPlan::Kind::Words);
}

TEST_CASE("catalog_db/search: FTS5 syntax in user text is inert: quotes, '*', '-', ':', parentheses, NEAR, OR, NOT, "
          "AND, '^', '+' are text the tokenizer splits",
          "[catalog_db][search]") {
    QueryFixture fixture("rv-search-syntax");
    const LibraryId& library = fixture.library.id;
    const IndexedEntry quest = edited(game(library, "gb", "Pixel Quest.gb", "Pixel Quest"),
                                      [](CatalogEntry& e) { e.metadata.developer = "Mirage Soft"; });
    const IndexedEntry racer = game(library, "gb", "Pixel Racer.gb", "Pixel Racer");
    const IndexedEntry nearby = game(library, "gb", "Near Or Not.gb", "Near Or Not And Title");
    fixture.import({system("gb", "Portable Console")}, {quest, racer, nearby});

    const std::vector<std::string> inputs{
        "\"",
        "\"\"",
        "\"pixel",
        "pixel\"",
        "pixel\" OR \"racer",
        "pixel*",
        "*pixel",
        "* *",
        "pixel -racer",
        "-pixel",
        "pixel NOT racer",
        "pixel OR racer",
        "pixel AND",
        "AND",
        "OR",
        "NOT",
        "NEAR",
        "NEAR(pixel racer)",
        "NEAR(pixel racer, 2)",
        "title:pixel",
        "developer:mirage",
        "{title}:pixel",
        "- title : pixel",
        "(pixel)",
        "(pixel OR",
        "pixel)",
        "^pixel",
        "pixel^",
        "+pixel",
        "pixel + quest",
        "col1 : x",
        "pixel quest*",
        "*",
        "'pixel'",
        "pixel\\",
        "\\\"",
        "\x01pixel",
        "pixel  \t quest",
        "near or not and",
        "NeAr(",
        "\"\"\"\"",
    };
    for (const std::string& input : inputs) {
        INFO(input);
        const auto plan = fixture.query->searchPlan(input);
        REQUIRE(plan.has_value());
        const auto ids = fixture.query->run(CatalogQuery{input});
        REQUIRE(ids.has_value()); // never an FTS5 syntax error
        if (plan->kind == SearchPlan::Kind::Words) {
            // The same as the words alone, separated by spaces.
            std::string words;
            for (const std::string& word : plan->words) {
                words += (words.empty() ? "" : " ") + word;
            }
            CHECK(fixture.search(words) == *ids);
            for (const std::string& word : plan->words) {
                CHECK(word.find_first_of("\"*:()^+-{}\\") == std::string::npos);
            }
        }
    }
    // What a few of them find: their words, never their syntax.
    CHECK(fixture.search("pixel OR racer").empty());                // "pixel", "or", "racer": no title holds "or"
    CHECK(fixture.search("pixel -racer") == std::vector{racer.id}); // '-' separates
    CHECK(fixture.search("pixel NOT racer").empty());
    CHECK(fixture.search("NEAR(pixel racer)").empty());
    CHECK(fixture.search("near or not and") == std::vector{nearby.id});
    CHECK(fixture.search("title:pixel").empty()); // no column filter: "title" is a word nothing else holds
    CHECK(fixture.search("developer:mirage").empty());
    CHECK(fixture.search("(pixel)") == titleOrder({quest, racer}));
    CHECK(fixture.search("pixel*") == titleOrder({quest, racer}));
    CHECK(fixture.search("^pixel") == titleOrder({quest, racer}));
    CHECK(fixture.search("\"pixel quest\"") == std::vector{quest.id});
    // Text without a word matches every entry, as blank text does.
    CHECK(fixture.query->searchPlan("\"\"").value().kind == SearchPlan::Kind::Everything);
    CHECK(fixture.search("* *") == titleOrder({quest, racer, nearby}));
    CHECK(fixture.search("") == titleOrder({quest, racer, nearby}));
    CHECK(fixture.query->searchPlan("pixel\" OR \"racer").value().match == R"("pixel" AND "or" AND "racer"*)");
    // Text that is not UTF-8 is refused.
    CHECK(codeOf(fixture.query->run(CatalogQuery{"pixel \xFF"})) == ErrorCode::InvalidArgument);
    CHECK(codeOf(fixture.query->searchWords("\xC3")) == ErrorCode::InvalidArgument);
    // So is text of more than 64 words (no title has as many; an FTS5 query would have as many terms).
    std::string many;
    for (int i = 0; i < 64; ++i) {
        many += "pixel ";
    }
    CHECK(fixture.query->run(CatalogQuery{many}).value() == titleOrder({quest, racer}));
    CHECK(codeOf(fixture.query->run(CatalogQuery{many + "pixel"})) == ErrorCode::InvalidArgument);
}

TEST_CASE("catalog_db/search: the words are FTS5's own tokens of the text, as the index holds them",
          "[catalog_db][search]") {
    QueryFixture fixture("rv-search-tokenizer");
    const LibraryId& library = fixture.library.id;
    // Titles of every kind: generated ones (precomposed and decomposed accents), punctuation, symbols, digits, scripts
    // without spaces, and the FTS5 syntax of the test above.
    std::vector<std::string> texts{
        "\xC3\x89"
        "clair d'\xC3\x89t\xC3\xA9: Super-Pixel's 2nd \"Quest\" (NEAR) OR -x title:y",
        "E\xCC\x81"
        "clair na\xC3\xAF"
        "ve \xC5\x92UVRE Stra\xC3\x9F"
        "e \xC4\xB0stanbul \xC3\xB8 \xC3\xA6",
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E abc \xEF\xBC\x91\xEF\xBC\x92\xEF\xBC\x93", // 日本語 abc １２３
        "C++ & C# | $100 @home #1 100% 3.14 1,000 a_b a.b.c",
        "pixel\" OR \"racer NEAR(a b, 2) {title}: ^x +y *z",
        "\xF0\x9F\x8E\xAE game \xF0\x9F\x91\xBE", // emoji
    };
    const rv::test::CatalogGeneratorOptions options{40, 3};
    for (std::size_t i = 0; i < options.entries; ++i) {
        texts.push_back(rv::test::generatedEntry(options, i).entry.title);
    }
    std::vector<IndexedEntry> entries;
    for (std::size_t i = 0; i < texts.size(); ++i) {
        entries.push_back(game(library, "gb", fmt::format("Title {}.gb", i), texts[i]));
    }
    // A system without a full name: only the titles are indexed.
    fixture.import({system("gb", "")}, entries);

    Connection& writer = fixture.database.writer();
    REQUIRE(writer.execute("CREATE VIRTUAL TABLE temp.search_terms USING fts5vocab(main, idx_game_search, instance)")
                .has_value());
    auto terms = writer.prepare("SELECT DISTINCT term FROM temp.search_terms WHERE doc = ?1 ORDER BY term");
    REQUIRE(terms.has_value());
    for (std::size_t i = 0; i < texts.size(); ++i) {
        INFO(texts[i]);
        const auto words = fixture.query->searchWords(texts[i]);
        REQUIRE(words.has_value());
        const std::set<std::string> fromWords(words->begin(), words->end());
        std::set<std::string> indexed;
        terms->bindInt(1, static_cast<std::int64_t>(entries[i].id.value()));
        while (terms->step().value()) {
            indexed.emplace(terms->columnText(0));
        }
        terms->reset();
        CHECK(fromWords == indexed);
        // Each word is one token: searched alone, as a whole word, it finds the title.
        for (const std::string& word : *words) {
            INFO(word);
            CHECK(fixture.query->searchWords(word).value() == std::vector{word});
        }
        if (i >= texts.size() - options.entries) {
            CHECK(*words == rv::test::generatedWords(texts[i])); // the oracle of the generated catalog agrees
        }
    }
    CHECK(fixture.query->searchWords("Super  Pixel's quest").value() ==
          std::vector<std::string>{"super", "pixel", "s", "quest"});
    CHECK(fixture.query->searchWords("quest quest").value() == std::vector<std::string>{"quest", "quest"});
    CHECK(fixture.query->searchWords(" -- ").value().empty());
}

TEST_CASE("catalog_db/query: the filters: states, availabilities (unavailable entries only when asked for) and "
          "systems",
          "[catalog_db][query]") {
    QueryFixture fixture("rv-query-filters");
    const LibraryId& library = fixture.library.id;
    const IndexedEntry gbGame = game(library, "gb", "Pixel Quest.gb", "Pixel Quest");
    const IndexedEntry snesGame = game(library, "snes", "Pixel Racer.sfc", "Pixel Racer");
    const IndexedEntry missing = edited(game(library, "gb", "Gone.gb", "Gone Pixel"), [](CatalogEntry& e) {
        e.state = EntryState::MissingFile;
        e.availability = Availability::Missing;
        e.file.reset();
    });
    const IndexedEntry unlisted = edited(game(library, "snes", "Away.sfc", "Away Pixel"), [](CatalogEntry& e) {
        e.availability = Availability::SourceUnavailable;
        e.unavailableReason = SourceUnavailableReason::Unreachable;
        e.file.reset();
    });
    const IndexedEntry notAGame = edited(game(library, "gb", "Readme.txt", "Readme Pixel"), [](CatalogEntry& e) {
        e.state = EntryState::NotAGame;
        e.notAGameReason = NotAGameReason::ExtensionNotConfigured;
    });
    const IndexedEntry member = edited(game(library, "gb", "Set/Track 2.bin", "Track Pixel"), [](CatalogEntry& e) {
        e.state = EntryState::DiscMember;
        e.discHead = makeLookupKey("Set/Set.cue").value();
    });
    const IndexedEntry folder = edited(game(library, "gb", "Collection", "Collection Pixel"), [](CatalogEntry& e) {
        e.state = EntryState::Folder;
        e.file = FileFacts{true, 0, 0, false};
    });
    const std::vector<IndexedEntry> all{gbGame, snesGame, missing, unlisted, notAGame, member, folder};
    fixture.import({system("gb", "Portable Console"), system("snes", "Home Console")}, all);

    // By default, the games as the catalog lists them: available Game entries.
    CHECK(fixture.run(CatalogQuery{}) == titleOrder({gbGame, snesGame}));
    CatalogQuery request;
    request.includeUnavailable = true;
    CHECK(fixture.run(request) == titleOrder({gbGame, snesGame, missing, unlisted}));
    request.availabilities = {Availability::Missing};
    CHECK(fixture.run(request) == std::vector{missing.id});
    request.availabilities = {Availability::SourceUnavailable, Availability::Missing};
    CHECK(fixture.run(request) == titleOrder({missing, unlisted}));
    request.includeUnavailable = false; // an unavailable availability is only let through when asked for
    CHECK(fixture.run(request).empty());
    request.availabilities = {Availability::Available, Availability::Missing};
    CHECK(fixture.run(request) == titleOrder({gbGame, snesGame}));

    // States.
    const auto withStates = [&](std::vector<EntryState> states, bool includeUnavailable = false) {
        CatalogQuery byState;
        byState.states = std::move(states);
        byState.includeUnavailable = includeUnavailable;
        return fixture.run(byState);
    };
    CHECK(withStates({EntryState::NotAGame}) == std::vector{notAGame.id});
    CHECK(withStates({EntryState::DiscMember}) == std::vector{member.id});
    CHECK(withStates({EntryState::Folder}) == std::vector{folder.id});
    CHECK(withStates({EntryState::MissingFile}).empty());
    CHECK(withStates({EntryState::MissingFile}, true) == std::vector{missing.id});
    CHECK(withStates({EntryState::OutsideSystemFolder}, true).empty());
    CHECK(withStates({EntryState::Game, EntryState::NotAGame, EntryState::DiscMember, EntryState::Folder,
                      EntryState::MissingFile},
                     true) == titleOrder(all));

    // Systems, with a search and with every state.
    CatalogQuery bySystem;
    bySystem.systems = {systemNamed("snes")};
    CHECK(fixture.run(bySystem) == std::vector{snesGame.id});
    bySystem.includeUnavailable = true;
    CHECK(fixture.run(bySystem) == titleOrder({snesGame, unlisted}));
    bySystem.text = "away";
    CHECK(fixture.run(bySystem) == std::vector{unlisted.id});
    bySystem.text = "quest";
    CHECK(fixture.run(bySystem).empty());
    bySystem.systems = {systemNamed("gb"), systemNamed("snes")};
    bySystem.text = "pixel";
    CHECK(fixture.run(bySystem) == titleOrder({gbGame, snesGame, missing, unlisted}));
    bySystem.systems = {systemNamed("n64")};
    CHECK(fixture.run(bySystem).empty());
    bySystem.text = "p"; // one character, with a filter
    bySystem.systems = {systemNamed("gb")};
    CHECK(fixture.run(bySystem) == std::vector{gbGame.id});
}

TEST_CASE("catalog_db/query: the orders are total: ties of the sort key by GameId; System by the folded full name, "
          "then the name; ReleaseDate oldest first, undated last",
          "[catalog_db][query]") {
    QueryFixture fixture("rv-query-orders");
    const LibraryId& library = fixture.library.id;
    // Namesakes: three "Harbor Racer" (two files of one system and one of another), and a sort title that ties them.
    const IndexedEntry racerA = game(library, "gb", "Harbor Racer (Europe).gb", "Harbor Racer");
    const IndexedEntry racerB = game(library, "gb", "Harbor Racer (USA).gb", "Harbor Racer");
    const IndexedEntry racerC = edited(game(library, "snes", "Harbor Racer.sfc", "Harbor Racer"),
                                       [](CatalogEntry& e) { e.metadata.releaseDate = ReleaseDate{1994, 5, 1}; });
    const IndexedEntry sorted = edited(game(library, "zx", "The Racer.tap", "The Harbor Racer"),
                                       [](CatalogEntry& e) { e.metadata.sortName = "harbor  RACER "; });
    const IndexedEntry alpha = edited(game(library, "snes", "Alpha.sfc", "Alpha Tower"),
                                      [](CatalogEntry& e) { e.metadata.releaseDate = ReleaseDate{1994, 5, 1}; });
    const IndexedEntry omega = edited(game(library, "zx", "Omega.tap", "Omega Tower"),
                                      [](CatalogEntry& e) { e.metadata.releaseDate = ReleaseDate{1982, 1, 1}; });
    const IndexedEntry orphan = game(library, "nosystem", "Orphan.bin", "Orphan Tower"); // no idx_systems row
    const std::vector<IndexedEntry> all{racerA, racerB, racerC, sorted, alpha, omega, orphan};
    // Full names: "Älter" folds to a sort key after "zeta" (accents are not folded), "home" and "Home" alike.
    fixture.import({system("gb", "home Console"), system("snes", "Home Console"), system("zx", "\xC3\x84lter Micro")},
                   all);
    REQUIRE(sorted.sortKey == racerA.sortKey);

    // Title: (sort key, GameId).
    const std::vector<GameId> title = titleOrder(all);
    CHECK(fixture.run(CatalogQuery{}) == title);
    std::vector<GameId> racers = idsOf({racerA, racerB, racerC, sorted});
    std::sort(racers.begin(), racers.end());
    CHECK(fixture.search("harbor racer") == racers);

    // System: gb and snes fold to one full name and order by name ("gb" < "snes"); the orphan's empty full name first;
    // "älter micro" last; Title's order within each.
    CatalogQuery bySystem;
    bySystem.sort = SortOrder::System;
    std::vector<IndexedEntry> expected = all;
    std::stable_sort(expected.begin(), expected.end(), [](const IndexedEntry& a, const IndexedEntry& b) {
        const auto rank = [](const IndexedEntry& e) {
            const std::string& name = e.entry.system.str();
            return name == "nosystem" ? 0 : name == "gb" ? 1 : name == "snes" ? 2 : 3;
        };
        return std::forward_as_tuple(rank(a), a.sortKey, a.id) < std::forward_as_tuple(rank(b), b.sortKey, b.id);
    });
    CHECK(fixture.run(bySystem) == idsOf(expected));

    // ReleaseDate: 1982, then the two of 1994-05-01 in Title's order, then the undated in Title's order.
    CatalogQuery byDate;
    byDate.sort = SortOrder::ReleaseDate;
    std::vector<GameId> dated{omega.id};
    const std::vector<GameId> sameDay = titleOrder({racerC, alpha});
    dated.insert(dated.end(), sameDay.begin(), sameDay.end());
    const std::vector<GameId> undated = titleOrder({racerA, racerB, sorted, orphan});
    dated.insert(dated.end(), undated.begin(), undated.end());
    CHECK(fixture.run(byDate) == dated);
    byDate.text = "tower";
    CHECK(fixture.run(byDate) == std::vector{omega.id, alpha.id, orphan.id});
}

TEST_CASE("catalog_db/query: positions and pages of a list; rows fetched by ids, in their order, with every field",
          "[catalog_db][query]") {
    QueryFixture fixture("rv-query-pages");
    const LibraryId& library = fixture.library.id;
    std::vector<IndexedEntry> entries;
    for (int i = 0; i < 40; ++i) {
        entries.push_back(game(library, "gb", fmt::format("Game {:02}.gb", i), fmt::format("Game {:02}", i)));
    }
    entries[7] = edited(entries[7], [](CatalogEntry& e) {
        e.metadata.developer = "Mirage Soft \n";
        e.metadata.publisher = " \xC3\x89"
                               "ditions\tPixel";
        e.metadata.genre = "Corsa, Guida / Moto";
        e.metadata.releaseDate = ReleaseDate{1997, 7, 2};
        e.metadata.players = Players{2, std::nullopt};
        e.metadata.rating = Rating{7};
        e.metadata.description = "A line.\nAnother line.";
        e.flags.favorite = true;
    });
    fixture.import({system("gb", "Portable Console")}, entries);

    const std::vector<GameId> list = fixture.run(CatalogQuery{});
    REQUIRE(list == titleOrder(entries));
    for (std::size_t i = 0; i < list.size(); ++i) {
        CHECK(positionOf(list, list[i]) == i);
    }
    CHECK_FALSE(positionOf(list, GameId::fromValue(12345).value()).has_value());

    // A page past the middle, fetched by ids: rows in the page's order.
    const auto page = pageOf(list, 30, 30);
    REQUIRE(page.size() == 10);
    const auto rows = fixture.query->fetch(page);
    REQUIRE(rows.has_value());
    REQUIRE(rows->size() == 10);
    for (std::size_t i = 0; i < rows->size(); ++i) {
        CHECK((*rows)[i].id == page[i]);
        CHECK((*rows)[i].title == fmt::format("Game {:02}", 30 + i));
    }

    // Every field of a row.
    const auto one = fixture.query->fetch(std::vector{entries[7].id});
    REQUIRE(one.has_value());
    REQUIRE(one->size() == 1);
    const CatalogRow& row = one->front();
    CHECK(row.id == entries[7].id);
    CHECK(row.system == systemNamed("gb"));
    CHECK(row.systemFullName == "Portable Console");
    CHECK(row.title == "Game 07");
    CHECK(row.state == EntryState::Game);
    CHECK(row.availability == Availability::Available);
    CHECK(row.releaseDate == ReleaseDate{1997, 7, 2});
    CHECK(row.developer == "Mirage Soft");
    CHECK(row.publisher == "\xC3\x89"
                           "ditions Pixel");
    CHECK(row.genre == "Corsa, Guida / Moto");
    CHECK(row.players == Players{2, std::nullopt});
    CHECK(row.rating == Rating{7});
    CHECK(row.description == "A line.\nAnother line.");
    CHECK(row.favorite);
    const auto plain = fixture.query->fetch(std::vector{entries[8].id}).value();
    REQUIRE(plain.size() == 1);
    CHECK_FALSE(plain[0].releaseDate.has_value());
    CHECK_FALSE(plain[0].players.has_value());
    CHECK_FALSE(plain[0].rating.has_value());
    CHECK_FALSE(plain[0].favorite);

    // Ids without a row have none (callers match rows by id); an id listed twice gives two rows; none, none.
    const GameId gone = GameId::fromValue(42).value();
    const auto mixed = fixture.query->fetch(std::vector{entries[3].id, gone, entries[1].id, entries[3].id});
    REQUIRE(mixed.has_value());
    REQUIRE(mixed->size() == 3);
    CHECK((*mixed)[0].id == entries[3].id);
    CHECK((*mixed)[1].id == entries[1].id);
    CHECK((*mixed)[2].id == entries[3].id);
    CHECK(fixture.query->fetch({}).value().empty());
    // GameIds up to 2^63 - 1 (JSON integers of SQLite's 64 bits).
    CHECK(fixture.query->fetch(std::vector{GameId::fromValue(GameId::kMaxValue).value()}).value().empty());
}

TEST_CASE("catalog_db/query: a reader never waits on the writer: during an import's open transaction it reads the "
          "last committed snapshot at once, in one read",
          "[catalog_db][query]") {
    QueryFixture fixture("rv-query-reader");
    const LibraryId& library = fixture.library.id;
    const IndexedEntry quest = game(library, "gb", "Pixel Quest.gb", "Pixel Quest");
    fixture.import({system("gb", "Portable Console")}, {quest});
    CHECK(fixture.query->connection().role() == rv::catalog_db::ConnectionRole::Reader);
    CHECK(fixture.query->connection().busyTimeout() == 0ms);

    // Another query that narrows the list it keeps for every search (lookup limit 0), kept while nothing is committed.
    auto narrowing =
        SqliteCatalogQuery::open(fixture.database.openReader().value(), rv::catalog_db::QueryOptions{0}).value();
    CHECK(narrowing->run(CatalogQuery{"pixel"}).value() == std::vector{quest.id});

    auto transaction = fixture.store->beginImport(library);
    REQUIRE(transaction.has_value());
    const IndexedEntry racer = game(library, "gb", "Pixel Racer.gb", "Pixel Racer");
    REQUIRE((*transaction)->putIdentity(rv::test::identityOf(racer.id, library, racer.entry)).has_value());
    REQUIRE((*transaction)->putEntry(racer).has_value());
    REQUIRE((*transaction)->putEntry(edited(quest, [](CatalogEntry& e) { e.title = "Tower Quest"; })).has_value());
    REQUIRE((*transaction)->putSystem(library, system("gb", "Renamed Console")).has_value());

    const auto started = std::chrono::steady_clock::now();
    CHECK(fixture.search("pixel") == std::vector{quest.id});
    CHECK(fixture.search("portable") == std::vector{quest.id});
    CHECK(fixture.search("renamed").empty());
    CHECK(fixture.titles("p") == std::vector<std::string>{"Pixel Quest"});
    CatalogQuery bySystem;
    bySystem.sort = SortOrder::System;
    CHECK(fixture.run(bySystem) == std::vector{quest.id});
    CHECK(fixture.query->systems().value().front().system.fullName == "Portable Console");
    CHECK(fixture.query->details(racer.id).value() == std::nullopt);
    CHECK(fixture.run(CatalogQuery{}) == std::vector{quest.id});
    CHECK(narrowing->run(CatalogQuery{"pixel"}).value() == std::vector{quest.id});
    CHECK(narrowing->run(CatalogQuery{"pixel qu"}).value() == std::vector{quest.id});
    CHECK(std::chrono::steady_clock::now() - started < 2s);   // the writer's busy timeout is 5 s
    CHECK_FALSE(fixture.query->connection().inTransaction()); // no read outlives a call

    REQUIRE((*transaction)->commit().has_value());
    const IndexedEntry tower = edited(quest, [](CatalogEntry& e) { e.title = "Tower Quest"; });
    CHECK(fixture.titles("pixel") == std::vector<std::string>{"Pixel Racer"});
    CHECK(fixture.titles("tower") == std::vector<std::string>{"Tower Quest"});
    CHECK(fixture.search("renamed") == titleOrder({racer, tower}));
    // The lists kept before the commit are read again.
    CHECK(fixture.run(CatalogQuery{}) == titleOrder({racer, tower}));
    CHECK(narrowing->run(CatalogQuery{"pixel"}).value() == std::vector{racer.id});
    CHECK(narrowing->run(CatalogQuery{"quest"}).value() == std::vector{quest.id});
}

TEST_CASE("catalog_db/query: lists are kept on a reader only: on a connection that writes, every query reads its own "
          "writes, committed or not",
          "[catalog_db][query]") {
    QueryFixture fixture("rv-query-writer");
    const LibraryId& library = fixture.library.id;
    const IndexedEntry alpha = game(library, "gb", "Alpha Quest.gb", "Alpha Quest");
    fixture.import({system("gb", "Portable Console")}, {alpha});

    // The queries on a writer: a second Database of the file (both hold its lock shared), its writer moved into them.
    // With a lookup limit of 0, every search narrows the list without the search, which a reader would keep.
    auto second = Database::open(rv::test::databaseFileIn(fixture.dir));
    REQUIRE(second.has_value());
    auto query = SqliteCatalogQuery::open(std::move(second->writer()), rv::catalog_db::QueryOptions{0});
    REQUIRE(query.has_value());
    Connection& connection = (*query)->connection();
    REQUIRE(connection.role() == rv::catalog_db::ConnectionRole::Writer);
    const auto titles = [&](std::string_view text) {
        const std::vector<GameId> ids = (*query)->run(CatalogQuery{std::string(text)}).value();
        const std::vector<CatalogRow> rows = (*query)->fetch(ids).value();
        std::vector<std::string> result;
        for (const CatalogRow& row : rows) {
            result.push_back(row.title);
        }
        return result;
    };
    // A game written by hand on the connection: its identity, then its index row (the triggers index it).
    const auto write = [&](std::int64_t id, std::string_view title) {
        REQUIRE(connection
                    .execute(fmt::format(
                        "INSERT INTO rv_game_identities (id, library, system, lookup_key, relative_path, key_rule, "
                        "key_casing, present, first_seen) SELECT {0}, library, system, '{1}', '{1}', key_rule, "
                        "key_casing, present, first_seen FROM rv_game_identities WHERE id = {2}; "
                        "INSERT INTO idx_games (id, library, system, relative_path, state, not_a_game_reason, "
                        "excluding_setting, availability, unavailable_reason, gamelist_path, title, sort_key, name, "
                        "sort_name, description, developer, publisher, genre, alt_emulator, favorite, hidden, "
                        "kid_game, completed, broken, play_count, play_time) SELECT {0}, library, system, '{1}', "
                        "state, not_a_game_reason, excluding_setting, availability, unavailable_reason, '', '{1}', "
                        "lower('{1}'), '', '', '', '', '', '', '', 0, 0, 0, 0, 0, 0, 0 FROM idx_games WHERE id = {2}",
                        id, title, alpha.id.value()))
                    .has_value());
    };
    CHECK(titles("") == std::vector<std::string>{"Alpha Quest"});
    CHECK(titles("quest") == std::vector<std::string>{"Alpha Quest"});

    write(4242, "Beta Quest"); // committed at once
    CHECK(titles("") == std::vector<std::string>{"Alpha Quest", "Beta Quest"});
    CHECK(titles("quest") == std::vector<std::string>{"Alpha Quest", "Beta Quest"});

    REQUIRE(connection.execute("BEGIN").has_value());
    write(4343, "Gamma Quest"); // not committed: the connection's reads see it
    CHECK(titles("") == std::vector<std::string>{"Alpha Quest", "Beta Quest", "Gamma Quest"});
    CHECK(titles("quest") == std::vector<std::string>{"Alpha Quest", "Beta Quest", "Gamma Quest"});
    REQUIRE(connection.execute("ROLLBACK").has_value());
    CHECK(titles("quest") == std::vector<std::string>{"Alpha Quest", "Beta Quest"});
    CHECK(titles("") == std::vector<std::string>{"Alpha Quest", "Beta Quest"});
}

TEST_CASE("catalog_db/query: the queries need the program's schema and an open connection; the writer's own open "
          "transaction is read as it stands",
          "[catalog_db][query]") {
    const rv::test::TempDir dir("rv-query-open");
    auto database = Database::open(rv::test::databaseFileIn(dir));
    REQUIRE(database.has_value());
    const auto unmigrated = SqliteCatalogQuery::open(database->openReader().value());
    REQUIRE_FALSE(unmigrated.has_value());
    CHECK(unmigrated.error().code == ErrorCode::Conflict);
    CHECK(contains(unmigrated.error().message, "migrate it first"));
    CHECK(codeOf(SqliteCatalogQuery::open(Connection{})) == ErrorCode::InvalidArgument);
    CHECK(codeOf(SqliteCatalogQuery::open(Connection::openMemory().value())) == ErrorCode::Conflict); // no catalog
    REQUIRE(rv::catalog_db::migrate(*database).has_value());

    auto reader = SqliteCatalogQuery::open(database->openReader().value());
    REQUIRE(reader.has_value());
    CHECK((*reader)->run(CatalogQuery{}).value().empty());
    CHECK((*reader)->systems().value().empty());
    // On a connection inside a transaction of its caller's, the queries read without one of their own.
    Connection& connection = (*reader)->connection();
    REQUIRE(connection.execute("BEGIN").has_value());
    CatalogQuery bySystem;
    bySystem.sort = SortOrder::System;
    bySystem.text = "pixel quest";
    CHECK((*reader)->run(bySystem).value().empty());
    CHECK((*reader)->details(GameId::fromValue(42).value()).value() == std::nullopt);
    CHECK(connection.inTransaction());
    REQUIRE(connection.execute("COMMIT").has_value());
}

TEST_CASE("catalog_db/query: a game's details are what the store wrote, with its system; the systems with their "
          "counts",
          "[catalog_db][query]") {
    QueryFixture fixture("rv-query-details");
    const LibraryId& library = fixture.library.id;
    CatalogEntry full = rv::test::fullEntry("gb", "Collection \xC3\x80/Pixel Quest (World).gb", "Pixel Quest");
    full.discHead = makeLookupKey("Collection \xC3\x80/Set.m3u").value();
    full.state = EntryState::DiscMember;
    const IndexedEntry detailed = rv::test::indexedEntry(rv::test::gameIdOf(library, full), full, true);
    const IndexedEntry bare = edited(game(library, "snes", "Bare.sfc", "Bare"), [](CatalogEntry&) {});
    const IndexedEntry missing = edited(game(library, "gb", "Gone.gb", "Gone"), [](CatalogEntry& e) {
        e.state = EntryState::MissingFile;
        e.availability = Availability::Missing;
        e.file.reset();
    });
    const IndexedEntry orphan = game(library, "nosystem", "Orphan.bin", "Orphan");
    const SystemInfo gb = system("gb", "Portable Console");
    const SystemInfo snes = system("snes", "Home Console");
    fixture.import({gb, snes}, {detailed, bare, missing, orphan});

    const auto details = fixture.query->details(detailed.id);
    REQUIRE(details.has_value());
    REQUIRE(details->has_value());
    CHECK((*details)->library == library);
    CHECK((*details)->system == gb);
    CHECK((*details)->indexed == detailed);
    const auto orphanDetails = fixture.query->details(orphan.id).value();
    REQUIRE(orphanDetails.has_value());
    CHECK_FALSE(orphanDetails->system.has_value());
    CHECK(fixture.query->details(GameId::fromValue(42).value()).value() == std::nullopt);
    CHECK(fixture.query->details(GameId{}).value() == std::nullopt);

    const auto systems = fixture.query->systems();
    REQUIRE(systems.has_value());
    REQUIRE(systems->size() == 3);
    // SortOrder::System's order: "" (the orphan's), "home console", "portable console".
    CHECK((*systems)[0].system.name == systemNamed("nosystem"));
    CHECK((*systems)[0].system.fullName.empty());
    CHECK((*systems)[0].games == 1);
    CHECK((*systems)[1].system == snes);
    CHECK((*systems)[1].library == library);
    CHECK((*systems)[1].games == 1);
    CHECK((*systems)[2].system == gb);
    CHECK((*systems)[2].games == 0);
    CHECK((*systems)[2].unavailable == 1);
    CHECK((*systems)[2].entries == 2);

    // The systems it knows: those with a row, those known from their entries only; no other.
    CHECK(fixture.query->hasSystem(systemNamed("gb")).value());
    CHECK(fixture.query->hasSystem(systemNamed("snes")).value());
    CHECK(fixture.query->hasSystem(systemNamed("nosystem")).value());
    CHECK_FALSE(fixture.query->hasSystem(systemNamed("n64")).value());
    REQUIRE(fixture.database.writer()
                .execute("INSERT INTO idx_systems SELECT library, 'n64', full_name, platforms, "
                         "theme, rom_folder_configured, rom_folder, availability, "
                         "unavailable_reason, gamelist, alternative_emulator FROM idx_systems "
                         "WHERE name = 'gb'")
                .has_value());
    CHECK(fixture.query->hasSystem(systemNamed("n64")).value()); // a system without entries
}

TEST_CASE("catalog_db/query: a generated catalog: every query as an oracle over its entries answers it",
          "[catalog_db][query][generated]") {
    const rv::test::TempDir dir("rv-query-generated");
    const rv::test::CatalogGeneratorOptions options{3'000, 21};
    const auto generated = rv::test::generateCatalogDatabase(rv::test::databaseFileIn(dir), options);
    REQUIRE(generated.has_value());
    REQUIRE(generated->entries.size() == options.entries);
    auto database = rv::catalog_db::openCatalogDatabase(rv::test::databaseFileIn(dir));
    REQUIRE(database.has_value());
    auto query = SqliteCatalogQuery::open(database->openReader().value());
    REQUIRE(query.has_value());

    // The catalog holds what the generator promises.
    std::map<EntryState, std::size_t> states;
    std::map<Availability, std::size_t> availabilities;
    std::set<std::string> titles;
    std::size_t namesakes = 0;
    std::size_t decomposed = 0;
    std::size_t decomposedFirst = 0; // sort keys whose first character is decomposed ("e" + U+0301)
    for (const IndexedEntry& e : generated->entries) {
        ++states[e.entry.state];
        ++availabilities[e.entry.availability];
        namesakes += titles.insert(e.sortKey).second ? 0 : 1;
        decomposed += contains(e.entry.title, "\xCC") ? 1 : 0;
        decomposedFirst += e.sortKey.size() > 1 && e.sortKey[1] == '\xCC' ? 1 : 0;
    }
    CHECK(states.size() == 5);
    CHECK(availabilities.size() == 3);
    CHECK(namesakes > options.entries / 10);
    CHECK(decomposed > 0);
    CHECK(decomposedFirst > 0);

    // The oracle: the entry's words per searched field, and the order keys.
    std::map<std::pair<std::string, std::string>, std::string> fullNames;
    for (const SystemInfo& info : generated->systems) {
        fullNames[{generated->library.id.str(), info.name.str()}] = info.fullName;
    }
    struct Facts {
        const IndexedEntry* entry;
        std::vector<std::string> words;
        std::pair<std::string, std::string> systemKey;
    };
    std::vector<Facts> facts;
    for (const IndexedEntry& e : generated->entries) {
        Facts f{&e, {}, {}};
        const GameMetadata& m = e.entry.metadata;
        const std::string year = m.releaseDate ? std::to_string(m.releaseDate->year) : "";
        const std::string& fullName = fullNames[{generated->library.id.str(), e.entry.system.str()}];
        for (const std::string& field :
             {e.entry.title, m.sortName, m.developer, m.publisher, m.genre, year, fullName}) {
            const auto words = rv::test::generatedWords(field);
            f.words.insert(f.words.end(), words.begin(), words.end());
        }
        f.systemKey = {sortKeyOf(fullName).value(), e.entry.system.str()};
        facts.push_back(std::move(f));
    }
    const auto oracle = [&](const CatalogQuery& request) {
        const std::vector<EntryState> wantedStates =
            request.states.empty() ? std::vector{EntryState::Game, EntryState::MissingFile} : request.states;
        const SearchPlan plan = query->get()->searchPlan(request.text).value();
        std::vector<const Facts*> matches;
        for (const Facts& f : facts) {
            const CatalogEntry& e = f.entry->entry;
            if (std::find(wantedStates.begin(), wantedStates.end(), e.state) == wantedStates.end()) {
                continue;
            }
            if (!request.includeUnavailable && e.availability != Availability::Available) {
                continue;
            }
            if (!request.availabilities.empty() &&
                std::find(request.availabilities.begin(), request.availabilities.end(), e.availability) ==
                    request.availabilities.end()) {
                continue;
            }
            if (!request.systems.empty() &&
                std::find(request.systems.begin(), request.systems.end(), e.system) == request.systems.end()) {
                continue;
            }
            // The sort key's first character once composed (the generator decomposes the titles it writes in NFD
            // with its own table, which composes them back).
            if (plan.kind == SearchPlan::Kind::FirstCharacter &&
                !rv::test::generatedComposed(f.entry->sortKey).starts_with(plan.prefix)) {
                continue;
            }
            if (plan.kind == SearchPlan::Kind::Words) {
                bool all = true;
                for (std::size_t w = 0; w < plan.words.size() && all; ++w) {
                    const bool last = w + 1 == plan.words.size();
                    all = std::any_of(f.words.begin(), f.words.end(), [&](const std::string& token) {
                        return last ? token.starts_with(plan.words[w]) : token == plan.words[w];
                    });
                }
                if (!all) {
                    continue;
                }
            }
            matches.push_back(&f);
        }
        std::sort(matches.begin(), matches.end(), [&](const Facts* a, const Facts* b) {
            const auto& ea = *a->entry;
            const auto& eb = *b->entry;
            if (request.sort == SortOrder::System && a->systemKey != b->systemKey) {
                return a->systemKey < b->systemKey;
            }
            if (request.sort == SortOrder::ReleaseDate) {
                const auto& da = ea.entry.metadata.releaseDate;
                const auto& db = eb.entry.metadata.releaseDate;
                if (da.has_value() != db.has_value()) {
                    return da.has_value();
                }
                if (da && *da != *db) {
                    return *da < *db;
                }
            }
            return std::tie(ea.sortKey, ea.id) < std::tie(eb.sortKey, eb.id);
        });
        std::vector<GameId> ids;
        for (const Facts* f : matches) {
            ids.push_back(f->entry->id);
        }
        return ids;
    };

    // Searches from the generator's own words: whole words, prefixes of 1 to 5 characters, two and three words in
    // either order, typed without their accents or in capitals, years, companies, system names.
    std::vector<std::string> texts{"",
                                   "pixel",
                                   "eclair",
                                   "ECLAIR",
                                   "\xC3\x89"
                                   "cl",
                                   "chateau ch",
                                   "quest of",
                                   "the",
                                   "super pixel",
                                   "pixel super",
                                   "zurich",
                                   "ZÜR",
                                   "kobo",
                                   "angstrom",
                                   "1985",
                                   "19",
                                   "199",
                                   "mirage",
                                   "editions pixel",
                                   "pixel editions",
                                   "systeme",
                                   "nintendo game",
                                   "sony",
                                   "corsa guida",
                                   "shoot em",
                                   "role playing game",
                                   "europe",
                                   "espana",
                                   "track",
                                   "collection",
                                   "archivio",
                                   "quixotry",
                                   "p",
                                   "S",
                                   "é",
                                   "e\xCC\x81", // é typed decomposed
                                   "e",
                                   "a",
                                   "o",
                                   "Ö",
                                   "2",
                                   "a b",
                                   "super m",
                                   "pi",
                                   "py",
                                   "zz",
                                   "galaxy robot wizard"};
    std::size_t nonEmpty = 0;
    for (const std::string& text : texts) {
        for (const SortOrder sort : {SortOrder::Title, SortOrder::System, SortOrder::ReleaseDate}) {
            for (const bool includeUnavailable : {false, true}) {
                CatalogQuery request;
                request.text = text;
                request.sort = sort;
                request.includeUnavailable = includeUnavailable;
                INFO(text << " / " << toString(sort) << " / " << includeUnavailable);
                const std::vector<GameId> expected = oracle(request);
                const auto ids = query->get()->run(request);
                REQUIRE(ids.has_value());
                CHECK(*ids == expected);
                nonEmpty += expected.empty() ? 0 : 1;
            }
        }
    }
    CHECK(nonEmpty > texts.size() * 3);
    // Filters of every kind, alone and with a search.
    for (const std::string text : {"", "pixel", "s", "galaxy"}) {
        for (const std::vector<EntryState>& wanted :
             {std::vector<EntryState>{}, std::vector{EntryState::NotAGame}, std::vector{EntryState::DiscMember},
              std::vector{EntryState::Folder, EntryState::MissingFile}}) {
            for (const std::vector<SystemName>& systems :
                 {std::vector<SystemName>{}, std::vector{systemNamed("psx")},
                  std::vector{systemNamed("fictif"), systemNamed("archive")}}) {
                CatalogQuery request;
                request.text = text;
                request.states = wanted;
                request.systems = systems;
                request.includeUnavailable = true;
                request.availabilities =
                    text == "s" ? std::vector{Availability::SourceUnavailable} : std::vector<Availability>{};
                INFO(text << " / " << wanted.size() << " states / " << systems.size() << " systems");
                const auto ids = query->get()->run(request);
                REQUIRE(ids.has_value());
                CHECK(*ids == oracle(request));
            }
        }
    }
    // Both ways of reading a search's list give the same lists: with a lookup limit of 0, every search reads the order
    // index whole; with no limit, every search looks its rows up.
    for (const std::size_t limit : {std::size_t{0}, std::numeric_limits<std::size_t>::max()}) {
        INFO("lookup limit " << limit);
        auto other = SqliteCatalogQuery::open(database->openReader().value(), rv::catalog_db::QueryOptions{limit});
        REQUIRE(other.has_value());
        for (const std::string text : {"pixel", "19", "an", "super m", "galaxy"}) {
            for (const SortOrder sort : {SortOrder::Title, SortOrder::System, SortOrder::ReleaseDate}) {
                CatalogQuery request;
                request.text = text;
                request.sort = sort;
                request.includeUnavailable = true;
                INFO(text << " / " << toString(sort));
                CHECK((*other)->run(request).value() == oracle(request));
            }
        }
    }

    // The systems' counts.
    const auto summaries = query->get()->systems();
    REQUIRE(summaries.has_value());
    CHECK(summaries->size() == generated->systems.size());
    for (const SystemSummary& summary : *summaries) {
        INFO(summary.system.name.str());
        std::size_t games = 0;
        std::size_t unavailable = 0;
        std::size_t entries = 0;
        for (const IndexedEntry& e : generated->entries) {
            if (e.entry.system != summary.system.name) {
                continue;
            }
            ++entries;
            const bool listed = e.entry.state == EntryState::Game || e.entry.state == EntryState::MissingFile;
            if (e.entry.state == EntryState::Game && e.entry.availability == Availability::Available) {
                ++games;
            } else if (listed && e.entry.availability != Availability::Available) {
                ++unavailable;
            }
        }
        CHECK(summary.games == games);
        CHECK(summary.unavailable == unavailable);
        CHECK(summary.entries == entries);
    }
    // db check: the index agrees with its content.
    auto report = rv::catalog_db::checkDatabase(*database);
    REQUIRE(report.has_value());
    CHECK(report->passed());
}
