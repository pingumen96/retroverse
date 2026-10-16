# I2 step 4: the SQLite store

The catalog database of ADR-0008 §5 in `rv_catalog_db`: connections (one writer, read-only readers), the process-level
lock, migrations with backups, schema v1 with the identity and index areas, the store of `rv_catalog`'s `ICatalogStore`
port, and `retroverse-cli db`. SQLite (3.53.4, FTS5 and JSON compiled in) stays private to the module: its types appear
in no public header, and only `src/catalog_db/src/database.cpp` includes `sqlite3.h`; the rest of the module, and the
later steps, use the `Connection`/`Statement` API.

## What was built

| File | What |
|---|---|
| `include/rv/catalog_db/database.hpp`, `src/database.cpp` | `Connection`, `Statement`, `ScopedReset`, `Transaction`, `DatabaseLock`, `Database`; `databaseFile()`, `lockFileOf()`, `sqliteHasFts5()`, `sqliteVersion()` |
| `include/rv/catalog_db/migrator.hpp`, `src/migrator.cpp` | `Migration`, `programMigrations()`, `programSchemaVersion()`, `migrate()`, `openCatalogDatabase()`, backups (`backupFolderOf()`, `listBackups()`), `checkDatabase()`, `describeDatabase()` |
| `src/schema.hpp` | the schema registry: areas, their steps per version, the rules every area keeps |
| `src/schema_identity.cpp`, `src/schema_index.cpp` | schema v1: the state tables (`rv_*`) and the index tables (`idx_*`) |
| `src/schema_search.cpp`, `src/schema_launch.cpp` | empty areas for steps 9 and 12 |
| `include/rv/catalog_db/catalog_store.hpp`, `src/catalog_store.cpp` | `SqliteCatalogStore`: `ICatalogStore` and `IImportTransaction` |
| `tools/cli/src/cmd_db.cpp` | `retroverse-cli db migrate | check | info` (`rebuild-index` stays a stub for step 11) |
| `tests/unit/catalog_db/*`, `tests/unit/cli/test_cli_db.cpp` | the tests (below) |

Typical use:

```cpp
auto database = rv::catalog_db::openCatalogDatabase(rv::catalog_db::databaseFile(paths)); // open + migrate
auto store = rv::catalog_db::SqliteCatalogStore::open(*database);   // ICatalogStore on the writer
auto reader = database->openReader();                               // a read-only connection for another thread
```

`Database::open()` alone opens without migrating (what `db check` and `db info` do); `migrate()` brings a database to the
program's version (or to a list of migrations a test gives).

## Connections and locking

- **One database file**, `<user data>/database/retroverse.sqlite` (`databaseFile(paths)`), with SQLite's `-wal` and `-shm`
  beside it, the lock file `retroverse.sqlite.lock`, and the folder `backups/`.
- **One writer per process** (`Database::writer()`), used from one thread (the catalog worker in the app, the main thread
  of the CLI): read-write, `journal_mode=WAL`, `foreign_keys=ON`, `synchronous=FULL` (no committed identity or session is
  lost on a power cut), `journal_size_limit` 64 MiB, `cache_size` 32 MiB, busy timeout 5 s (`DatabaseOptions`).
- **Readers** (`Database::openReader()`): `SQLITE_OPEN_READONLY`, no busy timeout. In WAL a reader sees the last committed
  snapshot while a write transaction is open; what would have to wait fails at once with `Conflict` instead. A reader
  keeps the process-level lock alive while it lives. Statements kept for reuse are reset after each use (`ScopedReset`),
  so that none keeps a read transaction (and an old WAL snapshot) open.
- **Every connection**: SQLite's defensive mode (no SQL can corrupt the file on purpose: `writable_schema`,
  `journal_mode=OFF`, writes to shadow tables), multi-thread mode (`SQLITE_OPEN_NOMUTEX`: a connection is used by one
  thread at a time), extended result codes, `sqlite3_close_v2` (a statement may outlive its `Connection` object).
- **Long paths**: every connection opens through SQLite's `win32-longpath` VFS (it differs from `win32` only in the path
  length it accepts), and the database is named by its `\\?\` form when the files SQLite keeps beside it would pass
  MAX_PATH; folders, the lock file and backups go through `\\?\` paths too. Paths are UTF-8 (a folder named outside ASCII
  is tested).
- **FTS5** is checked at open (`sqlite3_compileoption_used("ENABLE_FTS5")`): `Unsupported` without it.
- **A file that is not a catalog database** is never touched: the first migration sets `PRAGMA application_id` to
  `0x52564344` ("RVCD"); `Database::open()` refuses (`Conflict`) a file with another id, or with tables and no id, before
  it switches anything to WAL; a file that is no SQLite database at all is `ParseError`; an empty file is a new database.
- **Errors** are `Expected` values with SQLite's message and the statement's SQL as context: `SQLITE_BUSY`/`LOCKED`,
  UNIQUE and PRIMARY KEY constraints → `Conflict`; FOREIGN KEY constraints and SQL errors → `InvalidArgument`; CHECK,
  NOT NULL and STRICT type constraints → `ValidationFailed`; `SQLITE_INTERRUPT` → `Cancelled`; a damaged file or no
  database → `ParseError`; the file system → `IoError`.
- **Shutdown**: `Database::interrupt()` (`sqlite3_interrupt` on the writer) stops its running statement with `Cancelled`,
  from any thread, provided the database stays open until it returns.
- **The process-level lock** (`DatabaseLock`, `LockFileEx` on the lock file, released by the system however the process
  ends): byte 1 is held shared by every process that has the database open and exclusive by a migration; byte 0 is its
  gate, taken for an instant by a holder joining, and by a holder that wants to migrate from the moment it waits for the
  others to leave to the end of its migration, so that nobody joins meanwhile and moving between shared and exclusive
  never loses the lock; byte 2 is the import lock. Opening waits up to `DatabaseOptions::lockWait` (10 s) while another
  process migrates. A migration waits up to `MigrationOptions::lockWait` (2 s) for the other holders to close the
  database, then fails with `Conflict`: a migration never runs while another Retroverse process has the database open.
  Two processes that want to migrate at the same time (the app and `retroverse-cli` on a new database) would wait on each
  other: the one that finds the gate taken leaves and joins again (`DatabaseLock::rejoin()`), waiting up to
  `MigrationOptions::otherMigrationWait` (10 s), and then finds the database migrated. Two `Database` objects in one
  process are two holders (the tests use that to stand for two processes).
- **The import transaction** (`SqliteCatalogStore::beginImport()`) takes the import lock without waiting (`Conflict` when
  another import, of this store, another store or another process, holds it), then `BEGIN IMMEDIATE` on the writer. The
  store's own reads use the same connection and see its writes; readers and other processes see them once it commits;
  a transaction dropped without `commit()`, or whose store goes, rolls back. A refused write leaves it open; when SQLite
  rolls the whole transaction back on its own (a full disk, an I/O error, an interrupt), the import transaction ends
  there and every later call fails, so that nothing is ever written outside it. `commit()` records the keys' version in
  `rv_meta`, then runs a WAL checkpoint (`TRUNCATE`, ADR-0008 §5) that waits at most 200 ms for readers of older
  snapshots and is otherwise left to the next one.

## Migrations

- `PRAGMA user_version` counts the migrations a database ran. They run forward only, each in one `BEGIN IMMEDIATE`
  transaction that also sets the version and the application id and runs `PRAGMA foreign_key_check` before `COMMIT`; a
  migration that fails rolls back and leaves the version it had. A database newer than the program is refused
  (`Conflict`, "written by a newer Retroverse"); `Database::open()` still opens it, so that `db check` and `db info` show it.
- Under the exclusive lock, the version is read again (another process may have migrated meanwhile); then, when the
  database holds anything, a backup: `VACUUM INTO` a new file `backups/retroverse-<UTC YYYYMMDDTHHMMSS.mmmZ>-v<version it
  holds>[-<n>].sqlite` (a counter from 2 when the time repeats), written as `<name>.partial`, flushed to its disk
  (`FlushFileBuffers`) and renamed once complete; the free space is checked first (twice the database's size and 32 MiB
  more, room for the backup and for the migration's WAL); the newest 3 backups are kept, the one just written always
  (a clock set back would date it the oldest), and copies cut short (`.partial`) are removed. Nothing else in the folder
  is touched.
- Later migrations that change a table follow SQLite's table rebuild procedure ("Making Other Kinds Of Table Schema
  Changes", lang_altertable.html#otheralter) with `Migration::rebuildsTables` set: the migrator turns foreign keys off
  around the transaction (the pragma does nothing inside one), the migration creates `new_X`, copies, drops `X`, renames,
  and creates `X`'s indexes, triggers and views again; `foreign_key_check` before `COMMIT` rolls back a migration that
  breaks a reference. Never `writable_schema` (defensive mode).
- **Schema areas** (`src/schema.hpp`): the migration to version v runs every area's steps of version v in the order
  identity, index, search, launch, in one transaction. Steps 9 and 12 fill `searchArea()` and `launchArea()` in their own
  files; the registry already calls them. An area step is SQL (run statement by statement: an error names the statement
  that failed) and an optional function for rows of runtime values. **Schema v1 stays open until the M1 gate**: steps add
  their tables to v1 (`AreaStep{1, ...}`); a development database made by an earlier build is deleted, never migrated, and
  `db check` tells it from the program's schema. From the gate on, a change is a new version.

## Schema v1

Rules every area keeps: STRICT tables (types enforced); foreign keys point from `idx_*` to `rv_*` (and among `rv_*`),
never into `idx_*`, and none cascades (`NO ACTION`), so a rebuild of the index never touches the state and a state row
cannot go while rows refer to it; game rows take the GameId as `INTEGER PRIMARY KEY`; ids shown to users are
`AUTOINCREMENT` (never reused); text is UTF-8 compared byte for byte (BINARY), as the catalog orders keys; times are UTC
ISO-8601 text (`core::formatUtcIso8601`); enumerations are stored as their `toString()` names (never numbers, which a
reordered enum would change), except `MediaKind`, whose values media.hpp declares stable.

State tables (area identity, what only Retroverse knows; no import removes a row):

| Table | Holds |
|---|---|
| `rv_meta` | key/value: `schema_created_at`, `schema_created_by`; `lookup_key_rule` and `casing_table_version`, the lookup key version of every identity's key, recorded by each import's commit (`mixed` while identities hold keys of several versions, a re-keying not finished; the running system's when the schema was made, until an identity exists): when they differ from `catalog::currentLookupKeyVersion()`, the keys must be computed again (ADR-0008 §4); `sort_key_version` (of `idx_games.sort_key`) |
| `rv_libraries` | the libraries by their fixed id (the authority on it, ADR-0008 §4; a CHECK keeps the slug's form): adapter, ES-DE home and install folder, ROM and media root overrides |
| `rv_game_identities` | one row per identity (`StoredIdentity`): GameId, library, system, lookup key and exact relative path of its file when last seen, the key's rule and casing table versions, present flag and file facts (directory, size, last-write time, hidden; all NULL when no import saw the file), the quick hash with the size and time it belongs to (step 11), first and last seen. Unique on (library, system, key, relative path): two names that fold to one key keep two identities (ADR-0008 §4). Indexed for the import's matches (that unique index, in (key, path) order) and the quick hash pairing (a partial index) |
| `rv_identity_history` | what happened to an identity (moves with their reason, keys recomputed, aliases, reviews resolved), the key and path before and after (step 11) |
| `rv_identity_aliases` | GameIds that stand for another identity, never given out again (`takenGameIds()`), with what they were (step 11) |
| `rv_identity_reviews` | vanished identities no pairing was safe for, their candidates as a JSON array, resolution (step 11) |

Index tables (area index, a function of the ES-DE snapshot and the identities):

| Table | Holds |
|---|---|
| `idx_systems` | `SystemInfo` per library and system: full name, platforms (JSON array), theme, ROM folder as configured and resolved, availability with its reason, gamelist state, the gamelist's alternative emulator |
| `idx_games` | one row per indexed entry (`IndexedEntry`) under its identity's GameId: library, system, exact relative path (its key is computed again from it), state with its reason and excluding setting, availability with its reason, file facts, gamelist path, title, sort key, the metadata (text fields as written, release date as YYYYMMDD, rating in tenths, players min and max, NULL max for "N+"), the disc head's relative path, the alternative emulator, ES-DE's flags (favorite, hidden, kid game, completed, broken, play count and time, last played as ES-DE's local text), the media presence mask, the raw ES-DE values as JSON (`{"unknown":[["name","text"],...],"original":[...]}`, NULL when none). Indexed by (library, system) |
| `idx_media` | a row per entry and media kind present, with the extension (paths are derived) |
| `idx_disc_groups`, `idx_disc_members` | a disc group's head (kind, size and time when read: `CachedDiscGroup`) and its members in order (step 11 writes them) |
| `idx_import_issues` | the issues of each library's last import, in order (step 11) |

## The store (`SqliteCatalogStore`)

`ICatalogStore` as rv_catalog's header defines it today, on the database's writer; `open()` refuses a database whose
schema is not the program's (`Conflict`). Values come back unchanged; a `LookupKey` is computed again from its stored
relative path (`makeLookupKey()` is its only maker), and reads are put in the port's order in C++ (identities by key
then path, index rows by `entryOrder()`), whatever the casing table of the stored keys. Writes are upserts (`INSERT ...
ON CONFLICT DO UPDATE`), never `REPLACE`, so that triggers on the index tables (step 9's search index) see updates as
updates; an identity upsert keeps `first_seen` and the quick hash, and moves `last_seen` only when the file is present.
Besides the port's rules the transaction refuses: an identity without a GameId, system or key, of an unknown library
(`InvalidArgument`) or with another identity's key and path (`Conflict`); an entry of another system than its
identity's, an absent media kind with an extension, a count or file size beyond 2^63 - 1, a release date or last played
time that is none, raw values that are not UTF-8 (`InvalidArgument`), a rating beyond 10 or players below 1
(`ValidationFailed`, the schema's checks); a system of an unknown library. A refused write leaves the transaction open
and working; when SQLite rolls the whole transaction back on its own (a full disk, an I/O error, an interrupt), the
import transaction ends there and every later call fails (`InvalidArgument`). `removeEntry()` removes the entry's
`idx_games` and `idx_media` rows and the disc group it heads; a group it is a member of is its head's and stays as it
was read (`CachedDiscGroup`); its identity stays.

The raw ES-DE values cross JSON at every import, both ways: the store writes and reads their one shape with a small
dedicated writer and reader (RFC 8259 text, which SQLite's `json_valid` CHECK accepts) instead of nlohmann::json, which
cost about 130 µs to write and 240 µs to read such an object in Debug builds.

## `retroverse-cli db`

```
retroverse-cli [--data-dir <dir> | --portable] db migrate [--json] [--output <file>]
retroverse-cli [--data-dir <dir> | --portable] db check   [--json] [--output <file>]
retroverse-cli [--data-dir <dir> | --portable] db info    [--json] [--output <file>]
```

- `migrate` creates the database (and its folders) or brings its schema to the program's version, backing it up first;
  prints the database, `Schema version: 0 -> 1` or `Schema version: 1 (up to date)`, the backup written and the backups
  removed. Exit 0; 3 when it fails (another process has the database open, a newer database, no free space...).
- `check` opens an existing database without changing it and runs `integrity_check`, `foreign_key_check`, FTS5 (compiled
  in, and FTS5's `integrity-check` of every FTS5 table of the schema), the schema version against the program's, and, at
  the program's version, the schema itself against the one the program's migrations make in a private in-memory database
  (tables, indexes, triggers and views by their SQL: a database made by an earlier build of the open v1 shows here).
  One line per check (`integrity: ok`, `foreign-keys: PROBLEMS` and the problems below it, the first 100 of each and
  `... and N more`), then `No problems found.` or `N problem(s) found.`. A file SQLite cannot read (damaged pages, a
  header that is not SQLite's) is an integrity problem. Exit 0, 1 when a check finds problems, there is no database or
  it is damaged, 3 when it cannot run (the file locked, of another application, unreadable for the file system). The
  JSON has `passed`, `problemCount`, `problems` (`"<check>: <problem>"`) and `checks` (name, passed, problems,
  details); `schemaVersion` is null when the file could not be read.
- `info`: the path, schema version (and the program's), SQLite's version and journal mode, file sizes (database, WAL,
  shared memory) and pages, `rv_meta`, row counts per table (FTS5 shadow tables left out), backups newest first. A missing
  database is shown (`Exists: no`), not made. Exit 0, 3 when it cannot be read.
- `--json` writes one JSON object (keys sorted); `--output <file>` writes the results there instead of stdout, as the
  other commands. `db --help` lists the subcommands. `db rebuild-index` answers "not implemented yet (I2 step 11)", exit 2.

## Decisions taken within the step

1. **One backup per migration run**, before its first migration, not one per migration: the states after the first
   migration are the program's own work, which the first backup reproduces; a backup per migration would multiply the
   time (a `VACUUM INTO` of the whole database each) and, with the last 3 kept, could delete the user's original state
   when a run applies more than three migrations. **No backup of a database that holds nothing** (a new one).
2. **A database that is not ours is never touched** (`application_id`), checked before the switch to WAL.
3. **The lock file has a gate byte** besides the lock byte, so that moving between shared and exclusive never lets a
   holder in or loses the lock, and an **import lock byte**, so that `beginImport()` across processes gets `Conflict` at
   once instead of waiting on SQLite's write lock for the busy timeout.
4. **Identities keep their own key version** (`key_rule`, `key_casing` columns), which `StoredIdentity::keyVersion`
   reads back exactly; each commit records in `rv_meta` the version every identity's key is of, or `mixed` while a
   re-keying is not finished (a summary for `db info` and a quick check that never claims more than the rows hold).
5. **The unique key of an identity is (library, system, key, relative path)**: ADR-0008 §4 keeps two identities for two
   names that fold to one key, so the key alone cannot be unique.
6. **`idx_games` stores the relative path, not the key** (the key is computed again when read), and a disc member's head
   as its relative path: the key would be a third copy of the path in the index.
7. **`idx_disc_groups`** joins `idx_disc_members`: a group's head size and time (`CachedDiscGroup`) and groups whose
   members are all gone need a row of their own.
8. **STRICT tables with CHECK constraints** (flags 0 or 1, positive ids, a valid library slug, JSON columns valid) and
   SQLite's defensive mode on every connection.
9. **The writer's page cache is 32 MiB**: an import writes its rows in GameId order, which is random, all over its tables.
10. **`db check` on a missing or damaged database exits 1** (nothing to check, or a file SQLite cannot read, is a
    problem found, not a check that could not run): a check that SQLite stops on a damaged page (`ParseError`) records
    it and the others still run; a file whose header is not SQLite's is reported as an integrity problem. `db info` shows
    a missing database and exits 0; `migrate` and `info` fail on a damaged one (exit 3) and write nothing into it.
11. **Two processes that migrate a new database at once** (the app and `retroverse-cli`) never wait on each other: the
    one that finds the gate taken leaves and joins again once the other is done, then finds nothing left to do.

## Tests

`tests/unit/catalog_db` (label `unit`; databases only under each test's temporary folder):

- `test_database.cpp`: the file and its lock file; the writer's and the readers' pragmas; FTS5 with `unicode61
  remove_diacritics 2` ("POKÉMON" finds "Pokémon"); statements, bindings, columns, blobs, SQLite's errors by kind;
  transactions; **a reader during a write** (the previous snapshot, no wait, another process's connection alike and
  unable to write meanwhile); `interrupt()`; files that are not a catalog database (left untouched); **a database past
  MAX_PATH in folders named outside ASCII** (open, reader, migration with a backup); **two lock holders** (the second
  blocks the exclusive migration lock, joiners wait for a migration and get in when it ends; of two holders that want
  to migrate, one waits with the gate and the other lets it go first); the import lock.
- `test_migrator.cpp`: **a fresh database migrates to v1** (the 12 tables, `application_id`, `rv_meta`; no backup);
  **reopening is idempotent**; **a newer database is refused**; **backups with unique names, only the last 3 kept**, each
  the database as it was; **a failing migration rolls back** (also one that breaks a foreign key with foreign keys off);
  a copy cut short removed, a backup dated earlier by a clock set back kept, nothing else in the folder touched; no
  migration while another holder has the database open, and one that waits for a holder to leave; **two processes that
  migrate a new database at once** (one migrates, the other finds it migrated); the free space check; migration
  numbering; **the schema's rules** (foreign keys only into `rv_*`, `NO ACTION`; AUTOINCREMENT; STRICT; INTEGER PRIMARY
  KEY GameIds); **foreign keys enforced** (no cascade); `checkDatabase()` (broken references, a schema not the
  program's, an FTS5 table checked, an older version, damaged pages); `describeDatabase()`.
- `test_catalog_store.cpp`: a store needs the program's version; **every value read back unchanged** (every entry
  state, file facts, metadata, raw values, flags, media, disc head, an open players range, paths outside ASCII);
  **the ICatalogStore semantics** (own reads see the open transaction's writes; readers and another process do not, until
  commit; a dropped transaction leaves nothing; a second `beginImport()` from the same store, another store or another
  process gets `Conflict`; ended transactions fail with `InvalidArgument`); a store that goes rolls its transaction back;
  a transaction SQLite rolls back on its own (an interrupt) ends and writes nothing; the store's refusals; the orders
  (two names of one key, two identities); first and last seen and the quick hash across imports; `removeEntry()` and the
  disc groups; taken GameIds with aliases; key versions and `rv_meta` (`mixed`); raw values with any text and damaged
  JSON; **100,000 identities and index rows in one transaction** (timed, informative).
- `tests/unit/cli/test_cli_db.cpp`: `db migrate`, `db check`, `db info` as text and JSON, exit codes, `--output`, a newer
  database, a damaged one (pages, header: exit 1, nothing written), usage errors that touch no database.

The timed test writes 100,000 rows in builds with optimizations (the gate runs the Release unit tests) and 10,000 in
Debug, whose SQLite build (`SQLITE_DEBUG`) checks every page it touches and would take minutes over 100,000.

## Informative timings

Taken on the reference machine while other worktrees built and ran their tests, so only indicative (the gate measures
on a quiet machine). 100,000 identities and index rows (two thirds scraped entries with metadata, raw values and media),
four systems, one transaction:

| Build | Written in GameId order | Written in key order (random GameIds) | Read back (identities and index rows) |
|---|---|---|---|
| Release, writer cache 32 MiB | 3.5 to 4.5 s (identities 0.6 s, entries 1.9 s, commit and checkpoint 1.0 s) | 7.5 to 10 s | 0.8 to 1.3 s |
| Release, SQLite's default cache (2 MiB) | 7.5 to 8.5 s | 19 to 26 s | |
| Debug (10,000 rows) | 3.1 s | | 1.8 s |

The last runs of the step (the processor at 100% with the other worktrees' builds) took 6.2 to 13.6 s to write and 1.8
to 3.4 s to read back in Release, 2.8 to 7.6 s and 1.7 to 3.4 s in Debug: the gate's quiet measurement decides.
The database then holds 20,121 pages of 4 KiB (79 MiB). GameIds are random, so rows written in another order land on
random pages of the tables' B-trees; written in ascending GameId order they append. Reading the raw values with
nlohmann::json and sorting whole entries had cost 540 µs per row in Debug; the dedicated reader and the sort through a
permutation brought it to about 120 µs.

## Notes for the steps that build on it

- **Step 9** (search): fill `searchArea()` in `schema_search.cpp` with `AreaStep{1, "<SQL>"}` (schema v1 is open). The
  FTS5 table can take `idx_games` as external content (`content_rowid` = `id`, the GameId); keep it in step with
  triggers on `idx_games`: the store's `putEntry()` is an upsert, which fires the INSERT trigger for a new row and the
  UPDATE trigger for an existing one, and `removeEntry()` deletes. The year is `release_date / 10000`; the system's full
  name is in `idx_systems`. `db check` already runs FTS5's integrity-check on every FTS5 table. Queries run on reader
  connections (`Database::openReader()`), their statements kept and reset after each page (`ScopedReset`).
- **Step 11** (import): write identities and entries in ascending GameId order (see the timings). The state tables for
  history, aliases (already in `takenGameIds()`), reviews and quick hashes, and the index tables for disc groups and
  import issues exist; `rv_meta`'s `sort_key_version` says which folding `idx_games.sort_key` holds. Extend the store in
  `catalog_store.cpp` (its statements are listed in its `Sql` enumeration). `StoredIdentity::keyVersion` comes back as
  stored, the key computed again with the running system's casing table.
- **Step 12** (sessions, launch choices): fill `launchArea()` in `schema_launch.cpp`; the session store writes through
  the same `Database::writer()` (one writer per process, one thread).
