// Schema area "search" (I2 step 9): the search index over the index tables and what the queries need (ADR-0008 §5).
//
// The search index, idx_game_search, is an FTS5 table with external content: it stores only its index, the text it
// indexes being read from idx_game_search_content, a view over idx_games joined to idx_systems that adds what idx_games
// does not hold (the year of the release date, the system's full name). Triggers keep the index in step with both
// tables: FTS5 removes a row's tokens only when it is given the values it indexed ('delete' with the old values), so
// the index must never miss a change of an indexed value, in idx_games or in idx_systems (a system's full name is
// indexed with each of its games). The store writes with upserts, never REPLACE (which deletes without firing delete
// triggers), so an existing row fires the UPDATE triggers, which skip the rows whose indexed values did not change. A
// full rebuild ('rebuild', from the view) is a repair; `db check` runs FTS5's integrity-check with rank 1, which
// compares the index with the view.
//
// Tokenizer: unicode61 with remove_diacritics 2 (case and diacritics folded, also on letters that carry several; NFD
// text indexed as NFC text), prefix indexes of 2 and 3 characters for the last word of a search, which is a prefix.
// detail=none: the index keeps which rows hold a token, not where, which is all the queries ask (each word of a search
// is one token, matched anywhere in the row: catalog_query.cpp); columnsize=0: no per-row token counts (nothing is
// ranked by relevance, lists are ordered by sort key). Descriptions are not searched in M1.
//
// idx_games_sort orders the queries' lists: (sort key, GameId) is the total order of ADR-0008 §5, the rowid being the
// index's last column; the filter columns and the release date make it covering, so a list is materialized from the
// index alone, in order, without visiting the rows.

#include "schema.hpp"

#include <array>

namespace rv::catalog_db::schema {
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

-- A new idx_games row: its values, with its system's full name as idx_systems holds it now.
CREATE TRIGGER idx_games_search_insert AFTER INSERT ON idx_games BEGIN
    INSERT INTO idx_game_search (rowid, title, sort_name, developer, publisher, genre, year, system_full_name)
    VALUES (new.id, new.title, new.sort_name, new.developer, new.publisher, new.genre, new.release_date / 10000,
            (SELECT full_name FROM idx_systems WHERE library = new.library AND name = new.system));
END;

-- A removed idx_games row: the values it was indexed with.
CREATE TRIGGER idx_games_search_delete AFTER DELETE ON idx_games BEGIN
    INSERT INTO idx_game_search (idx_game_search, rowid, title, sort_name, developer, publisher, genre, year,
                                 system_full_name)
    VALUES ('delete', old.id, old.title, old.sort_name, old.developer, old.publisher, old.genre,
            old.release_date / 10000,
            (SELECT full_name FROM idx_systems WHERE library = old.library AND name = old.system));
END;

-- An updated idx_games row whose indexed values changed (an upsert sets every column; most are unchanged): out with the
-- old values, in with the new ones.
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

-- A new system: its games already indexed (an import may write a system after its games) were indexed without a full
-- name; they get this one.
CREATE TRIGGER idx_systems_search_insert AFTER INSERT ON idx_systems BEGIN
    INSERT INTO idx_game_search (idx_game_search, rowid, title, sort_name, developer, publisher, genre, year,
                                 system_full_name)
    SELECT 'delete', id, title, sort_name, developer, publisher, genre, release_date / 10000, NULL
    FROM idx_games WHERE library = new.library AND system = new.name;
    INSERT INTO idx_game_search (rowid, title, sort_name, developer, publisher, genre, year, system_full_name)
    SELECT id, title, sort_name, developer, publisher, genre, release_date / 10000, new.full_name
    FROM idx_games WHERE library = new.library AND system = new.name;
END;

-- A removed system: its games lose its full name.
CREATE TRIGGER idx_systems_search_delete AFTER DELETE ON idx_systems BEGIN
    INSERT INTO idx_game_search (idx_game_search, rowid, title, sort_name, developer, publisher, genre, year,
                                 system_full_name)
    SELECT 'delete', id, title, sort_name, developer, publisher, genre, release_date / 10000, old.full_name
    FROM idx_games WHERE library = old.library AND system = old.name;
    INSERT INTO idx_game_search (rowid, title, sort_name, developer, publisher, genre, year, system_full_name)
    SELECT id, title, sort_name, developer, publisher, genre, release_date / 10000, NULL
    FROM idx_games WHERE library = old.library AND system = old.name;
END;

-- A system whose full name changed (an upsert sets it every time; the store's upsert never renames a system, but an
-- UPDATE may): the games of the system as it was take the full name they have now (its new one, or none when it was
-- renamed away); renamed, the games of its new name, which no system had, take its full name.
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

-- The order of the queries' lists, (sort key, GameId), covering their filters and the release date order.
CREATE INDEX idx_games_sort ON idx_games (sort_key, state, availability, library, system, release_date);
)sql";

constexpr std::array<AreaStep, 1> kSteps{{
    {1, kVersion1, nullptr, false},
}};

} // namespace

Area searchArea() {
    return Area{"search", kSteps};
}

} // namespace rv::catalog_db::schema
