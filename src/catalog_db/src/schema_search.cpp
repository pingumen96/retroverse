// Schema area "search" (I2 step 9): the search index over the index tables and what the queries need (ADR-0008 §5).
//
// The search index, idx_game_search, is an FTS5 table with external content: it stores only its index, the text it
// indexes being read from idx_game_search_content, a view over idx_games joined to idx_systems that adds what idx_games
// does not hold (the year of the release date, the system's full name). Triggers keep the index in step with both
// tables: FTS5 removes a row's tokens only when it is given the values it indexed ('delete' with the old values), so
// the index must never miss a change of an indexed value, in idx_games or in idx_systems (a system's full name is
// indexed with each of its games). The store writes with upserts, never REPLACE (which deletes without firing delete
// triggers), so an existing row fires the UPDATE triggers, whose WHEN clauses leave the index alone for the rows whose
// indexed values did not change. A full rebuild ('rebuild', from the view) is a repair, and what ends a bulk write
// (search_index.hpp: the triggers dropped for it and created again, from the same text); `db check` runs FTS5's
// integrity-check with rank 1, which compares the index with the view.
//
// Tokenizer: unicode61 with remove_diacritics 2 (case and diacritics folded, also on letters that carry several; NFD
// text indexed as NFC text), prefix indexes of 2 and 3 characters for the last word of a search, which is a prefix.
// detail=none: the index keeps which rows hold a token, not where, which is all the queries ask (each word of a search
// is one token, matched anywhere in the row: catalog_query.cpp); columnsize=0: no per-row token counts (nothing is
// ranked by relevance, lists are ordered by sort key). Descriptions are not searched in M1.
//
// idx_games_sort orders the queries' lists: its first two columns, (sort key, GameId), are the total order of ADR-0008
// §5, so that a list, or the sort key range of a first character, is read in order from the index alone, without a
// sort (SQLite keeps the rowid at the end of every index entry, which orders ties only after every column before it:
// the GameId must be named second). The filter columns and the release date after them make it covering: a list is
// materialized, and the systems' entries counted, without visiting the rows.

#include "schema.hpp"

#include <array>

#include <fmt/format.h>

#include <rv/catalog_db/search_index.hpp>

namespace rv::catalog_db {
namespace {

constexpr std::string_view kVersion1 = R"sql(
-- What the search index indexes, one row per idx_games row (its GameId as id): the title, the sort title, developer,
-- publisher and genre as written, the year of the release date (NULL when undated) and the full name of the game's
-- system (NULL while idx_systems has no row for it).
CREATE VIEW idx_game_search_content AS
SELECT g.id AS id, g.title AS title, g.sort_name AS sort_name, g.developer AS developer, g.publisher AS publisher,
       g.genre AS genre, g.release_date / 10000 AS year, s.full_name AS system_full_name
FROM idx_games AS g LEFT JOIN idx_systems AS s ON s.library = g.library AND s.name = g.system;

CREATE VIRTUAL TABLE idx_game_search USING fts5(
    title, sort_name, developer, publisher, genre, year, system_full_name,
    content = 'idx_game_search_content', content_rowid = 'id',
    tokenize = 'unicode61 remove_diacritics 2', prefix = '2 3', detail = none, columnsize = 0
);

-- The order of the queries' lists, (sort key, GameId), covering their filters and the release date order.
CREATE INDEX idx_games_sort ON idx_games (sort_key, id, state, availability, library, system, release_date);
)sql";

/// A trigger of the search index: its name and the statement that creates it, run by the migration (createTriggers())
/// and by resumeSearchIndex(), so that the schema holds the same text either way (`db check` compares it).
struct SearchTrigger {
    std::string_view name;
    std::string_view sql;
};

constexpr std::array<SearchTrigger, 6> kTriggers{{
    // A new idx_games row: its values, with its system's full name as idx_systems holds it now.
    {"idx_games_search_insert", R"sql(
CREATE TRIGGER idx_games_search_insert AFTER INSERT ON idx_games BEGIN
    INSERT INTO idx_game_search (rowid, title, sort_name, developer, publisher, genre, year, system_full_name)
    VALUES (new.id, new.title, new.sort_name, new.developer, new.publisher, new.genre, new.release_date / 10000,
            (SELECT full_name FROM idx_systems WHERE library = new.library AND name = new.system));
END;
)sql"},
    // A removed idx_games row: the values it was indexed with.
    {"idx_games_search_delete", R"sql(
CREATE TRIGGER idx_games_search_delete AFTER DELETE ON idx_games BEGIN
    INSERT INTO idx_game_search (idx_game_search, rowid, title, sort_name, developer, publisher, genre, year,
                                 system_full_name)
    VALUES ('delete', old.id, old.title, old.sort_name, old.developer, old.publisher, old.genre,
            old.release_date / 10000,
            (SELECT full_name FROM idx_systems WHERE library = old.library AND name = old.system));
END;
)sql"},
    // An updated idx_games row whose indexed values changed (an upsert sets every column; most are unchanged): out with
    // the old values, in with the new ones.
    {"idx_games_search_update", R"sql(
CREATE TRIGGER idx_games_search_update
AFTER UPDATE OF id, library, system, title, sort_name, developer, publisher, genre, release_date ON idx_games
WHEN old.id IS NOT new.id OR old.library IS NOT new.library OR old.system IS NOT new.system
     OR old.title IS NOT new.title OR old.sort_name IS NOT new.sort_name OR old.developer IS NOT new.developer
     OR old.publisher IS NOT new.publisher OR old.genre IS NOT new.genre OR old.release_date IS NOT new.release_date
BEGIN
    INSERT INTO idx_game_search (idx_game_search, rowid, title, sort_name, developer, publisher, genre, year,
                                 system_full_name)
    VALUES ('delete', old.id, old.title, old.sort_name, old.developer, old.publisher, old.genre,
            old.release_date / 10000,
            (SELECT full_name FROM idx_systems WHERE library = old.library AND name = old.system));
    INSERT INTO idx_game_search (rowid, title, sort_name, developer, publisher, genre, year, system_full_name)
    VALUES (new.id, new.title, new.sort_name, new.developer, new.publisher, new.genre, new.release_date / 10000,
            (SELECT full_name FROM idx_systems WHERE library = new.library AND name = new.system));
END;
)sql"},
    // A new system: its games already indexed (an import may write a system after its games) were indexed without a
    // full name; they get this one.
    {"idx_systems_search_insert", R"sql(
CREATE TRIGGER idx_systems_search_insert AFTER INSERT ON idx_systems BEGIN
    INSERT INTO idx_game_search (idx_game_search, rowid, title, sort_name, developer, publisher, genre, year,
                                 system_full_name)
    SELECT 'delete', id, title, sort_name, developer, publisher, genre, release_date / 10000, NULL
    FROM idx_games WHERE library = new.library AND system = new.name;
    INSERT INTO idx_game_search (rowid, title, sort_name, developer, publisher, genre, year, system_full_name)
    SELECT id, title, sort_name, developer, publisher, genre, release_date / 10000, new.full_name
    FROM idx_games WHERE library = new.library AND system = new.name;
END;
)sql"},
    // A removed system: its games lose its full name.
    {"idx_systems_search_delete", R"sql(
CREATE TRIGGER idx_systems_search_delete AFTER DELETE ON idx_systems BEGIN
    INSERT INTO idx_game_search (idx_game_search, rowid, title, sort_name, developer, publisher, genre, year,
                                 system_full_name)
    SELECT 'delete', id, title, sort_name, developer, publisher, genre, release_date / 10000, old.full_name
    FROM idx_games WHERE library = old.library AND system = old.name;
    INSERT INTO idx_game_search (rowid, title, sort_name, developer, publisher, genre, year, system_full_name)
    SELECT id, title, sort_name, developer, publisher, genre, release_date / 10000, NULL
    FROM idx_games WHERE library = old.library AND system = old.name;
END;
)sql"},
    // A system whose full name changed (an upsert sets it every time; the store's upsert never renames a system, but
    // an UPDATE may): the games of the system as it was take the full name they have now (its new one, or none when it
    // was renamed away); renamed, the games of its new name, which no system had, take its full name.
    {"idx_systems_search_update", R"sql(
CREATE TRIGGER idx_systems_search_update AFTER UPDATE OF library, name, full_name ON idx_systems
WHEN old.full_name IS NOT new.full_name OR old.library IS NOT new.library OR old.name IS NOT new.name
BEGIN
    INSERT INTO idx_game_search (idx_game_search, rowid, title, sort_name, developer, publisher, genre, year,
                                 system_full_name)
    SELECT 'delete', id, title, sort_name, developer, publisher, genre, release_date / 10000, old.full_name
    FROM idx_games WHERE library = old.library AND system = old.name;
    INSERT INTO idx_game_search (rowid, title, sort_name, developer, publisher, genre, year, system_full_name)
    SELECT id, title, sort_name, developer, publisher, genre, release_date / 10000,
           (SELECT full_name FROM idx_systems WHERE library = old.library AND name = old.name)
    FROM idx_games WHERE library = old.library AND system = old.name;
    INSERT INTO idx_game_search (idx_game_search, rowid, title, sort_name, developer, publisher, genre, year,
                                 system_full_name)
    SELECT 'delete', id, title, sort_name, developer, publisher, genre, release_date / 10000, NULL
    FROM idx_games
    WHERE library = new.library AND system = new.name AND (old.library IS NOT new.library OR old.name IS NOT new.name);
    INSERT INTO idx_game_search (rowid, title, sort_name, developer, publisher, genre, year, system_full_name)
    SELECT id, title, sort_name, developer, publisher, genre, release_date / 10000, new.full_name
    FROM idx_games
    WHERE library = new.library AND system = new.name AND (old.library IS NOT new.library OR old.name IS NOT new.name);
END;
)sql"},
}};

/// Creates every search trigger (the migration's step, after kVersion1).
Status createTriggers(Connection& connection) {
    for (const SearchTrigger& trigger : kTriggers) {
        if (auto created = connection.execute(trigger.sql); !created) {
            return created;
        }
    }
    return ok();
}

/// Drops the search triggers that exist.
Status dropTriggers(Connection& connection) {
    for (const SearchTrigger& trigger : kTriggers) {
        if (auto dropped = connection.execute(fmt::format("DROP TRIGGER IF EXISTS {}", trigger.name)); !dropped) {
            return dropped;
        }
    }
    return ok();
}

/// Suspending and resuming change the schema of an open write transaction only: outside one, the triggers would be
/// gone, or the index rebuilt, for good in a statement of its own.
Status requireTransaction(Connection& connection, std::string_view what) {
    if (!connection.isOpen() || !connection.inTransaction()) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("{} the search index needs a write transaction open on the connection", what));
    }
    return ok();
}

constexpr std::array<schema::AreaStep, 1> kSteps{{
    {1, kVersion1, &createTriggers, false},
}};

} // namespace

namespace schema {

Area searchArea() {
    return Area{"search", kSteps};
}

} // namespace schema

Status suspendSearchIndex(Connection& connection) {
    if (auto required = requireTransaction(connection, "suspending"); !required) {
        return required;
    }
    return dropTriggers(connection);
}

Status resumeSearchIndex(Connection& connection) {
    if (auto required = requireTransaction(connection, "resuming"); !required) {
        return required;
    }
    // Dropped first, the triggers that are still there (a repair without a suspension) are created again like the
    // others; the index is rebuilt in between, which fires none of them.
    if (auto dropped = dropTriggers(connection); !dropped) {
        return dropped;
    }
    if (auto rebuilt = connection.execute("INSERT INTO idx_game_search (idx_game_search) VALUES ('rebuild')");
        !rebuilt) {
        return rebuilt;
    }
    return createTriggers(connection);
}

Expected<bool> searchIndexFollowsWrites(Connection& connection) {
    auto count = connection.prepare("SELECT count(*) FROM sqlite_schema WHERE type = 'trigger' AND name IN "
                                    "(SELECT value FROM json_each(?1))");
    if (!count) {
        return propagate(count.error());
    }
    std::string names = "[";
    for (std::size_t i = 0; i < kTriggers.size(); ++i) {
        names += fmt::format("{}\"{}\"", i == 0 ? "" : ",", kTriggers[i].name);
    }
    names += ']';
    count->bindText(1, names);
    const auto row = count->step();
    if (!row) {
        return propagate(row.error());
    }
    return *row && count->columnInt(0) == static_cast<std::int64_t>(kTriggers.size());
}

} // namespace rv::catalog_db
