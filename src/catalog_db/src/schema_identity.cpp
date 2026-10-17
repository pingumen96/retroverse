// Schema area "identity" (I2 step 4): the state tables, what only Retroverse knows and no import removes (ADR-0008 §4,
// §5). Reconciliation (step 11b) fills history, aliases, reviews and quick hashes; the store (catalog_store.cpp) writes
// libraries, identities and, from step 11a on, the import records.

#include "schema.hpp"

#include <array>
#include <string>

#include <rv/catalog/lookup_key.hpp>
#include <rv/core/case_fold.hpp>
#include <rv/core/time.hpp>
#include <rv/core/version.hpp>

namespace rv::catalog_db::schema {
namespace {

constexpr std::string_view kVersion1 = R"sql(
-- Facts about the database: when and by which program it was made, and the versions of the rules its stored values
-- follow (key, value; catalog_store.hpp lists the keys).
CREATE TABLE rv_meta (
    key   TEXT NOT NULL PRIMARY KEY,
    value TEXT NOT NULL
) WITHOUT ROWID, STRICT;

-- The libraries (LibraryProfile), by their fixed id: the database is the authority on the id (ADR-0008 §4), which every
-- GameId of the library hashes; library.json only names it. Paths in UTF-8; NULL where the profile has none.
CREATE TABLE rv_libraries (
    id             TEXT NOT NULL PRIMARY KEY
                   CHECK (length(id) BETWEEN 1 AND 32 AND id NOT GLOB '*[^a-z0-9-]*'),
    adapter        TEXT NOT NULL,
    esde_home      TEXT NOT NULL,
    install_folder TEXT,
    rom_root       TEXT,
    media_root     TEXT
) WITHOUT ROWID, STRICT;

-- One row per game identity (StoredIdentity), never removed by an import, whatever ES-DE says: the GameId given the
-- first time its key was seen; the key and the exact relative path its file had when last seen, with the version of
-- the key's rule and casing table (rv_meta keeps the version the store last wrote keys with); the file's facts at the
-- last import that saw it (all NULL when none did); the quick hash with the size and time it belongs to (I2 step 11);
-- when the identity was made (first_seen) and when an import last wrote it with its file present (last_seen: first
-- seen, its path or file facts changed, re-keyed; an import writes no identity that did not change, and when the
-- library was last imported is in rv_imports). Two names that fold to one key keep two identities (ADR-0008 §4), so
-- the key is not unique: the exact path is.
CREATE TABLE rv_game_identities (
    id              INTEGER PRIMARY KEY CHECK (id > 0),
    library         TEXT    NOT NULL REFERENCES rv_libraries (id),
    system          TEXT    NOT NULL CHECK (system <> ''),
    lookup_key      TEXT    NOT NULL CHECK (lookup_key <> ''),
    relative_path   TEXT    NOT NULL CHECK (relative_path <> ''),
    key_rule        INTEGER NOT NULL,
    key_casing      TEXT    NOT NULL,
    present         INTEGER NOT NULL CHECK (present IN (0, 1)),
    is_directory    INTEGER CHECK (is_directory IN (0, 1)),
    file_size       INTEGER CHECK (file_size >= 0),
    file_time       INTEGER,
    file_hidden     INTEGER CHECK (file_hidden IN (0, 1)),
    quick_hash      BLOB    CHECK (length(quick_hash) = 32),
    quick_hash_size INTEGER CHECK (quick_hash_size >= 0),
    quick_hash_time INTEGER,
    first_seen      TEXT    NOT NULL,
    last_seen       TEXT,
    CHECK ((is_directory IS NULL) = (file_size IS NULL) AND (file_size IS NULL) = (file_time IS NULL)
           AND (file_time IS NULL) = (file_hidden IS NULL)),
    CHECK ((quick_hash IS NULL) = (quick_hash_size IS NULL) AND (quick_hash_size IS NULL) = (quick_hash_time IS NULL))
) STRICT;
-- A file has one identity: its exact relative path is unique in its system, whatever casing table its stored key was
-- computed with (keys of several versions while a re-keying is not finished). The index also serves the reads of a
-- system's identities (ICatalogStore::identities), which the import matches by path and by key in memory: no index by
-- key, a second index written in random order (GameIds are random) that made writing 100,000 rows half again as slow.
CREATE UNIQUE INDEX rv_game_identities_path ON rv_game_identities (library, system, relative_path);
-- Reconciliation pairs a vanished identity with a new file of the same quick hash (I2 step 11).
CREATE INDEX rv_game_identities_quick_hash ON rv_game_identities (quick_hash) WHERE quick_hash IS NOT NULL;

-- What happened to identities (I2 step 11): moves with their reason (MoveReason), keys computed again, aliases
-- recorded, reviews resolved; the key and path before and after.
CREATE TABLE rv_identity_history (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    game      INTEGER NOT NULL REFERENCES rv_game_identities (id),
    at        TEXT    NOT NULL,
    event     TEXT    NOT NULL,
    reason    TEXT    NOT NULL DEFAULT '',
    from_key  TEXT,
    from_path TEXT,
    to_key    TEXT,
    to_path   TEXT,
    detail    TEXT    NOT NULL DEFAULT ''
) STRICT;
CREATE INDEX rv_identity_history_game ON rv_identity_history (game, id);

-- GameIds that stand for another identity (I2 step 11): a file moved back onto a key that had another identity
-- meanwhile, a review resolved. An alias is never given out again (ICatalogStore::takenGameIds); what it was is kept.
CREATE TABLE rv_identity_aliases (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    alias         INTEGER NOT NULL UNIQUE CHECK (alias > 0),
    game          INTEGER NOT NULL REFERENCES rv_game_identities (id),
    lookup_key    TEXT    NOT NULL,
    relative_path TEXT    NOT NULL,
    at            TEXT    NOT NULL,
    reason        TEXT    NOT NULL,
    CHECK (alias <> game)
) STRICT;
CREATE INDEX rv_identity_aliases_game ON rv_identity_aliases (game);

-- Vanished identities no pairing was safe for (ReviewRow, I2 step 11): the user decides; the ids are shown to the user.
-- The candidates are the new files' relative paths in key order (a JSON array).
CREATE TABLE rv_identity_reviews (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    game          INTEGER NOT NULL REFERENCES rv_game_identities (id),
    vanished_key  TEXT    NOT NULL,
    vanished_path TEXT    NOT NULL,
    candidates    TEXT    NOT NULL CHECK (json_valid(candidates)),
    reason        TEXT    NOT NULL,
    created_at    TEXT    NOT NULL,
    resolved_at   TEXT,
    resolution    TEXT
) STRICT;
CREATE INDEX rv_identity_reviews_game ON rv_identity_reviews (game);
CREATE INDEX rv_identity_reviews_open ON rv_identity_reviews (id) WHERE resolved_at IS NULL;

-- The imports of each library (ImportRecord, I2 step 11a), kept for the user and by `db rebuild-index`: when each began
-- and committed, how it ran (ImportKind), whether it accepted the removals the mass-disappearance guard holds back, the
-- systems it was asked for (a JSON array of names, empty for every system) and what it did per system (a JSON array of
-- objects, ChangeSet::systems' figures with the outcome's name, for the systems it changed or reported). The newest
-- 100 of each library are kept (kKeptImportRecords).
CREATE TABLE rv_imports (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    library         TEXT    NOT NULL REFERENCES rv_libraries (id),
    kind            TEXT    NOT NULL,
    started_at      TEXT    NOT NULL,
    finished_at     TEXT    NOT NULL,
    accept_removals INTEGER NOT NULL CHECK (accept_removals IN (0, 1)),
    systems_asked   TEXT    NOT NULL CHECK (json_valid(systems_asked)),
    summary         TEXT    NOT NULL CHECK (json_valid(summary))
) STRICT;
CREATE INDEX rv_imports_library ON rv_imports (library, id);
)sql";

/// rv_meta's first rows: when the database was made and by which program, and the lookup key version of the system that
/// made it (the version the store has written keys with until an identity is written with another).
Status seedMeta(Connection& connection) {
    auto insert = connection.prepare("INSERT INTO rv_meta (key, value) VALUES (?1, ?2)");
    if (!insert) {
        return propagate(insert.error());
    }
    const catalog::LookupKeyVersion keys = catalog::currentLookupKeyVersion();
    const std::array<std::pair<std::string_view, std::string>, 4> rows{{
        {"schema_created_at", core::utcNow()},
        {"schema_created_by", core::versionBanner()},
        {"lookup_key_rule", std::to_string(keys.rule)},
        {"casing_table_version", keys.casingTable},
    }};
    for (const auto& [key, value] : rows) {
        insert->bindText(1, key).bindText(2, value);
        if (auto inserted = insert->run(); !inserted) {
            return inserted;
        }
    }
    return ok();
}

constexpr std::array<AreaStep, 1> kSteps{{
    {1, kVersion1, &seedMeta, false},
}};

} // namespace

Area identityArea() {
    return Area{"identity", kSteps};
}

} // namespace rv::catalog_db::schema
