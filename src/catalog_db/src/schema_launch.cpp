// Schema area "launch" (I2 step 12): the state tables of launches (ADR-0008 §5, §6, §8, §9): play sessions, per-game
// and per-system launch choices (retroverse-cli assign), launch verifications. Every table that names a game refers to
// rv_game_identities (id) by a foreign key, never to idx_* and never cascading: `db rebuild-index` clears the index and
// leaves these rows alone, and reconciliation (I2 step 11b), when it turns an identity into an alias of the identity a
// moved file kept, re-points every rv_* row that refers to it (UPDATE OR IGNORE, then deletes what is left: a table
// unique per game keeps the moved identity's own row). The session store (session_store.cpp) reads and writes them
// with upserts, never REPLACE.

#include "schema.hpp"

#include <array>

namespace rv::catalog_db::schema {
namespace {

constexpr std::string_view kVersion1 = R"sql(
-- Every launch attempt (PlaySession), its id shown to users: the game (the identity a GameId stands for, an alias
-- resolved when written); the choice it launched with, or would have (an ES-DE command label of the game's system, ''
-- for a single unlabelled command, or a profile id: launcher::LaunchChoice, its kind by toString()), and where the
-- choice came from (catalog::ChoiceSource); whether it was a launch test (retroverse-cli launch --test). UTC wall-clock
-- times: started_at when the launch began; ended_at when it ended (for left-running, when its owner stopped following
-- it), NULL while running and for interrupted sessions, whose end is not known; closed_at when a later start closed an
-- interrupted session. run_ms: how long the game ran (a monotonic clock: from the moment its process ran to its
-- authoritative exit, or to the moment its owner stopped following it), NULL when nothing ran. The outcome
-- (PlaySessionOutcome), with the readiness that blocked a launch that never started, why a game was left running
-- (LeftRunningReason), and the launcher's details (launcher::FailureReason by toString(), the exit code and its
-- launcher::ExitKind by toString(), quick exit, the user's confirmation, processes left in the tree, a refusal by the
-- automation guard, its sentence). The exit code and its kind are both NULL when nothing exited (I2 step 11b-12i): so
-- always for an attempt that never started (blocked before the launch; a request refused or a process that could not
-- be created, failure 'invalid request' or 'start failed', a refusal by the guard among them), never for a crash. A
-- running or interrupted row may hold one (an exit seen while the return awaits the user's confirmation).
-- The started process's id, and the owner: the process that recorded the session (its id and creation time in
-- FILETIME ticks), so that a later start closes as interrupted only the open sessions whose owner is gone.
CREATE TABLE rv_play_sessions (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    game               INTEGER NOT NULL REFERENCES rv_game_identities (id),
    choice_kind        TEXT    NOT NULL CHECK (choice_kind IN ('command', 'profile')),
    choice             TEXT    NOT NULL,
    choice_source      TEXT    NOT NULL CHECK (choice_source IN ('retroverse-game', 'esde-game', 'retroverse-system',
                                                                 'esde-system', 'first-command')),
    test               INTEGER NOT NULL CHECK (test IN (0, 1)),
    started_at         TEXT    NOT NULL,
    ended_at           TEXT,
    closed_at          TEXT,
    run_ms             INTEGER CHECK (run_ms >= 0),
    outcome            TEXT    NOT NULL CHECK (outcome IN ('running', 'returned', 'failed-to-start', 'crashed',
                                                           'interrupted', 'blocked', 'left-running')),
    readiness          TEXT    CHECK (readiness IN ('source-unavailable', 'rom-missing', 'requires-configuration',
                                                    'emulator-not-found', 'core-not-found')),
    left_running       TEXT    NOT NULL CHECK (left_running IN ('none', 'confirmed', 'not-waited', 'wait-limit',
                                                                'quit')),
    failure            TEXT    NOT NULL,
    exit_code          INTEGER CHECK (exit_code BETWEEN 0 AND 4294967295),
    exit_kind          TEXT,
    quick_exit         INTEGER NOT NULL CHECK (quick_exit IN (0, 1)),
    confirmed_by_user  INTEGER NOT NULL CHECK (confirmed_by_user IN (0, 1)),
    processes_remained INTEGER NOT NULL CHECK (processes_remained IN (0, 1)),
    refused_by_guard   INTEGER NOT NULL CHECK (refused_by_guard IN (0, 1)),
    message            TEXT    NOT NULL,
    process_id         INTEGER NOT NULL CHECK (process_id BETWEEN 0 AND 4294967295),
    owner_pid          INTEGER NOT NULL CHECK (owner_pid BETWEEN 0 AND 4294967295),
    owner_created      INTEGER NOT NULL CHECK (owner_created >= 0),
    CHECK ((outcome IN ('running', 'interrupted')) = (ended_at IS NULL)),
    CHECK ((outcome = 'interrupted') = (closed_at IS NOT NULL)),
    CHECK ((outcome = 'blocked') = (readiness IS NOT NULL)),
    CHECK ((outcome = 'left-running') = (left_running <> 'none')),
    CHECK ((exit_code IS NULL) = (exit_kind IS NULL)),
    CHECK (exit_code IS NULL OR (outcome <> 'blocked' AND refused_by_guard = 0 AND
                                 failure NOT IN ('invalid request', 'start failed'))),
    CHECK (outcome <> 'crashed' OR exit_code IS NOT NULL)
) STRICT;
-- A game's sessions, newest first (retroverse-cli sessions --game), and the re-pointing of an alias's rows (I2 step 11b).
CREATE INDEX rv_play_sessions_game ON rv_play_sessions (game, id);
-- The open sessions, which every start looks at (SessionStore::closeInterrupted()).
CREATE INDEX rv_play_sessions_open ON rv_play_sessions (id) WHERE outcome = 'running';

-- Retroverse's choice for a game (retroverse-cli assign --game), the first of ADR-0008 §8's order: an ES-DE command
-- label of the game's system or a profile id, never a copied command (translated again at every launch); when it was
-- made. One per game: its GameId is the INTEGER PRIMARY KEY.
CREATE TABLE rv_launch_game_choices (
    game      INTEGER PRIMARY KEY REFERENCES rv_game_identities (id),
    kind      TEXT    NOT NULL CHECK (kind IN ('command', 'profile')),
    value     TEXT    NOT NULL CHECK (value <> ''),
    chosen_at TEXT    NOT NULL
) STRICT;

-- Retroverse's choice for a system (retroverse-cli assign <system>), by library: before emulators.json's
-- "systemChoices" for the same system (the user's latest explicit choice, docs/i2/step-12.md), after the game's own
-- choices (ADR-0008 §8). Never written into the user's files (ADR-0008 §6).
CREATE TABLE rv_launch_system_choices (
    library   TEXT NOT NULL REFERENCES rv_libraries (id),
    system    TEXT NOT NULL CHECK (system <> ''),
    kind      TEXT NOT NULL CHECK (kind IN ('command', 'profile')),
    value     TEXT NOT NULL CHECK (value <> ''),
    chosen_at TEXT NOT NULL,
    PRIMARY KEY (library, system)
) WITHOUT ROWID, STRICT;

-- The launch verifications (ADR-0008 §8: "verified" is shown, never required): per library, system and choice, the
-- latest session that returned normally with it (catalog::returnedNormally: a clean exit that was no quick exit,
-- nothing left running), whether that was a launch test (retroverse-cli launch --test), the game and the program that
-- ran, and when.
CREATE TABLE rv_launch_verifications (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    library     TEXT    NOT NULL REFERENCES rv_libraries (id),
    system      TEXT    NOT NULL CHECK (system <> ''),
    kind        TEXT    NOT NULL CHECK (kind IN ('command', 'profile')),
    value       TEXT    NOT NULL,
    game        INTEGER NOT NULL REFERENCES rv_game_identities (id),
    session     INTEGER NOT NULL REFERENCES rv_play_sessions (id),
    test        INTEGER NOT NULL CHECK (test IN (0, 1)),
    program     TEXT    NOT NULL,
    verified_at TEXT    NOT NULL,
    UNIQUE (library, system, kind, value)
) STRICT;
CREATE INDEX rv_launch_verifications_game ON rv_launch_verifications (game);
)sql";

// I7 step 4 (ADR-0018 §5): the program Retroverse's user chose for an emulator of ES-DE's commands that finds none, per
// library and %EMULATOR_X% name (as written in the commands, compared exactly): esde::findEmulator() looks here before
// the find rules. The path is refused by launcher::isRefusedProgram() at every translation, like a program a find rule
// finds. A state table: `db rebuild-index` never touches it, and nothing is written into ES-DE's files.
constexpr std::string_view kVersion2 = R"sql(
CREATE TABLE rv_launch_program_choices (
    library   TEXT NOT NULL REFERENCES rv_libraries (id),
    emulator  TEXT NOT NULL CHECK (emulator <> ''),
    program   TEXT NOT NULL CHECK (program <> ''),
    chosen_at TEXT NOT NULL,
    PRIMARY KEY (library, emulator)
) WITHOUT ROWID, STRICT;
)sql";

constexpr std::array<AreaStep, 2> kSteps{{
    {1, kVersion1, nullptr, false},
    {2, kVersion2, nullptr, false},
}};

} // namespace

Area launchArea() {
    return Area{"launch", kSteps};
}

} // namespace rv::catalog_db::schema
