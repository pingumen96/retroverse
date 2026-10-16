// Schema area "index" (I2 step 4): the index tables, a function of the ES-DE snapshot and the identities that an import
// (or `db rebuild-index`, I2 step 11) can rebuild at any time (ADR-0008 §5). They refer to the state tables, never the
// other way round, and nothing cascades: rebuilding them never touches what only Retroverse knows.

#include "schema.hpp"

#include <array>
#include <string>

#include <rv/catalog/metadata.hpp>

namespace rv::catalog_db::schema {
namespace {

constexpr std::string_view kVersion1 = R"sql(
-- The systems of each library as its last import read them (SystemInfo): what lists and filters show, where the ROM
-- folder is and whether it could be listed, whether the gamelist was read, the command the gamelist chooses. Platforms
-- as a JSON array of strings, the ROM folder in UTF-8, enumerations as their names.
CREATE TABLE idx_systems (
    library               TEXT NOT NULL REFERENCES rv_libraries (id),
    name                  TEXT NOT NULL CHECK (name <> ''),
    full_name             TEXT NOT NULL,
    platforms             TEXT NOT NULL CHECK (json_valid(platforms)),
    theme                 TEXT NOT NULL,
    rom_folder_configured TEXT NOT NULL,
    rom_folder            TEXT NOT NULL,
    availability          TEXT NOT NULL,
    unavailable_reason    TEXT NOT NULL,
    gamelist              TEXT NOT NULL,
    alternative_emulator  TEXT NOT NULL,
    PRIMARY KEY (library, name)
) WITHOUT ROWID, STRICT;

-- One row per indexed entry (IndexedEntry) under its identity's GameId: every entry of the snapshot that has a key
-- (OutsideSystemFolder entries are never indexed), exactly as the snapshot gave it, with its sort key (sortKeyOf(),
-- rv_meta's sort_key_version) and its media presence mask (bit n: MediaKind n, idx_media holds the extensions). The
-- key of the relative path is its identity's. Text fields as written, '' when empty; NULL where a value is absent:
-- file facts (all four together), release date (YYYYMMDD), rating (tenths), players (players_max NULL with
-- players_min: an open range, "8+"), the disc head's relative path (DiscMember entries), the last played time (ES-DE's
-- local "YYYYMMDDTHHMMSS"), the raw ES-DE values (a JSON object of [name, text] pairs: "unknown" elements, "original"
-- forms). Enumerations as their names.
CREATE TABLE idx_games (
    id                 INTEGER PRIMARY KEY REFERENCES rv_game_identities (id),
    library            TEXT    NOT NULL REFERENCES rv_libraries (id),
    system             TEXT    NOT NULL,
    relative_path      TEXT    NOT NULL CHECK (relative_path <> ''),
    state              TEXT    NOT NULL,
    not_a_game_reason  TEXT    NOT NULL,
    excluding_setting  TEXT    NOT NULL,
    availability       TEXT    NOT NULL,
    unavailable_reason TEXT    NOT NULL,
    is_directory       INTEGER CHECK (is_directory IN (0, 1)),
    file_size          INTEGER CHECK (file_size >= 0),
    file_time          INTEGER,
    file_hidden        INTEGER CHECK (file_hidden IN (0, 1)),
    gamelist_path      TEXT    NOT NULL,
    title              TEXT    NOT NULL,
    sort_key           TEXT    NOT NULL,
    name               TEXT    NOT NULL,
    sort_name          TEXT    NOT NULL,
    description        TEXT    NOT NULL,
    developer          TEXT    NOT NULL,
    publisher          TEXT    NOT NULL,
    genre              TEXT    NOT NULL,
    release_date       INTEGER CHECK (release_date BETWEEN 10101 AND 99991231),
    rating             INTEGER CHECK (rating BETWEEN 0 AND 10),
    players_min        INTEGER CHECK (players_min >= 1),
    players_max        INTEGER CHECK (players_max >= players_min),
    disc_head_path     TEXT,
    alt_emulator       TEXT    NOT NULL,
    favorite           INTEGER NOT NULL CHECK (favorite IN (0, 1)),
    hidden             INTEGER NOT NULL CHECK (hidden IN (0, 1)),
    kid_game           INTEGER NOT NULL CHECK (kid_game IN (0, 1)),
    completed          INTEGER NOT NULL CHECK (completed IN (0, 1)),
    broken             INTEGER NOT NULL CHECK (broken IN (0, 1)),
    play_count         INTEGER NOT NULL CHECK (play_count >= 0),
    play_time          INTEGER NOT NULL CHECK (play_time >= 0),
    last_played        TEXT,
    media_mask         INTEGER NOT NULL CHECK (media_mask >= 0),
    raw                TEXT    CHECK (json_valid(raw)),
    CHECK ((is_directory IS NULL) = (file_size IS NULL) AND (file_size IS NULL) = (file_time IS NULL)
           AND (file_time IS NULL) = (file_hidden IS NULL)),
    CHECK (players_max IS NULL OR players_min IS NOT NULL)
) STRICT;
-- A system's entries (ICatalogStore::indexedEntries; the queries' system filter, I2 step 9).
CREATE INDEX idx_games_system ON idx_games (library, system);

-- The media an entry has (ADR-0008 §5): a row per kind present, with the file's extension as found ("" for none);
-- paths are derived (IMediaResolver). kind is MediaKind's value, stable by media.hpp's rule.
CREATE TABLE idx_media (
    game      INTEGER NOT NULL REFERENCES rv_game_identities (id),
    kind      INTEGER NOT NULL CHECK (kind BETWEEN 0 AND 31),
    extension TEXT    NOT NULL,
    PRIMARY KEY (game, kind)
) WITHOUT ROWID, STRICT;

-- Disc groups (ADR-0008 §3; I2 step 11 writes them): a head (a .cue or .m3u that is a game of its system) with the
-- size and last write time it had when it was read, which the next import reuses instead of reading it again
-- (CachedDiscGroup) ...
CREATE TABLE idx_disc_groups (
    head      INTEGER PRIMARY KEY REFERENCES rv_game_identities (id),
    kind      TEXT    NOT NULL,
    head_size INTEGER NOT NULL CHECK (head_size >= 0),
    head_time INTEGER NOT NULL
) STRICT;
-- ... and its members, in the order the head names them, each once.
CREATE TABLE idx_disc_members (
    head     INTEGER NOT NULL REFERENCES rv_game_identities (id),
    position INTEGER NOT NULL CHECK (position >= 0),
    member   INTEGER NOT NULL REFERENCES rv_game_identities (id),
    PRIMARY KEY (head, position),
    UNIQUE (head, member)
) WITHOUT ROWID, STRICT;
CREATE INDEX idx_disc_members_member ON idx_disc_members (member);

-- The issues of each library's last import (Issue; I2 step 11), in the order they were met.
CREATE TABLE idx_import_issues (
    id       INTEGER PRIMARY KEY,
    library  TEXT    NOT NULL REFERENCES rv_libraries (id),
    severity TEXT    NOT NULL,
    code     TEXT    NOT NULL,
    system   TEXT    NOT NULL,
    file     TEXT    NOT NULL,
    reason   TEXT    NOT NULL
) STRICT;
CREATE INDEX idx_import_issues_library ON idx_import_issues (library, id);
)sql";

/// The version of the sort keys idx_games holds: an index made with another folding is rebuilt (I2 step 11).
Status seedMeta(Connection& connection) {
    auto insert = connection.prepare("INSERT INTO rv_meta (key, value) VALUES ('sort_key_version', ?1)");
    if (!insert) {
        return propagate(insert.error());
    }
    insert->bindText(1, std::to_string(catalog::kSortKeyVersion));
    return insert->run();
}

constexpr std::array<AreaStep, 1> kSteps{{
    {1, kVersion1, &seedMeta, false},
}};

} // namespace

Area indexArea() {
    return Area{"index", kSteps};
}

} // namespace rv::catalog_db::schema
