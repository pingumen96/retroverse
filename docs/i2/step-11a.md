# I2 step 11a: the import

Roadmap row: I2 step 11a (wave 3, needs 4, 7, 8, 9). Decisions: [ADR-0008](../decisions/ADR-0008-esde-catalog.md) §4
(identity, fingerprints, availability, mass disappearance), §5 (storage, index and state tables, concurrency, the import
lock, the checkpoint, media stored as presence and extension), §7 (gamelists that fail, metadata pending). The import
turns the library snapshot of step 7 into the catalog database of step 4, with the media presence of step 8, the search
index of step 9 kept in step, and the identities that steps 11b (reconciliation) and 12 (launches, sessions) build on.
`retroverse-cli import`, `db rebuild-index` and `media --game` are filled; `show` prints the media paths. This page is
folded into the shared documentation at the wave merge.

## What was built

| File | What it does |
|---|---|
| `rv/catalog/import_service.hpp`, `src/import_service.cpp` (`rv_catalog`) | `ImportService` over the ports `ILibrarySource`, `IMediaResolver`, `ICatalogStore`: `run(profile, ImportOptions)` gives `ImportResult` (`ChangeSet` + `ImportStatistics`); `ImportOptions` (cancel flag, `acceptRemovals`, `systems` for a partial import, `rebuildIndex`); the seam for step 11b: `IIdentityPairing`, `PairingRequest`, `VanishedIdentity`, `NewFile` (with `heldBy`), `IdentityPair`, `IdentityPairs`; the import's issue codes `kImportIssue...` |
| `rv/catalog/change_set.hpp` (step 1's, extended) | `SystemOutcome`, `SystemChanges` (per system: added, changed, removed, unchanged, unavailable, kept, the identities' changes, the guard's figures, issues), `ChangeSet::systems`, `firstImport`, `total()`, `findSystem()`; `MovedGame::alias` |
| `rv/catalog/catalog_store.hpp` (step 1's, extended) | `IndexedDiscGroup`, `ImportKind`, `ImportRecord`, `IImportLock`, `IdentityAlias`; `ICatalogStore::systems`, `identitySystems`, `discGroups`, `lastImport`, `lockImport`; `IImportTransaction::prepareWrites`, `clearIndex`, `removeSystem`, `putDiscGroup`, `removeDiscGroup`, `putAlias`, `putImportIssues`, `recordImport` |
| `rv/catalog/disc_group.hpp`, `library_source.hpp`, `media_resolver.hpp` (steps 1, 7, 8, changed) | `DiscGroup::headSize`, `headLastWriteTime`: the facts of the head its references were read from (step 7's `CachedDiscGroup` folded into `DiscGroup`: the cache is `std::span<const DiscGroup>`); `LibraryLoadOptions::readEverySheet` (`db rebuild-index`); `IMediaResolver::refresh()` (a no-op by default) |
| `src/esde/src/library_source.cpp`, `rv/esde/media_resolver.hpp` (`rv_esde`) | each group carries the facts of the sheet its references came from (the listing's when read, the cached group's when kept: unchanged, unreadable, below a folder that cannot be listed); `readEverySheet`; `EsdeMediaResolver::refresh()` overrides the port's |
| `rv/catalog_db/catalog_store.hpp`, `src/catalog_store.cpp` (`rv_catalog_db`) | the store's side of all of the above (26 new statements in its `Sql` enumeration); `StoreOptions` with `SearchIndexPolicy` (`Automatic`, `Suspend`, `FollowWrites`) and `kBulkWriteShare`; `kKeptImportRecords`; `removeEntry()` also removes the references of the group it heads; `putAlias()`; each commit records `sort_key_version` |
| `src/catalog_db/src/schema_identity.cpp` | `rv_imports` (state: the import records) |
| `src/catalog_db/src/schema_index.cpp` | `idx_disc_references` (index: what a head says, by relative path) |
| `tools/cli/src/import_text.hpp` (new private header), `cmd_import.cpp` | `runCatalogImport()`: the import that `import` and `db rebuild-index` share, its summary as text and JSON |
| `tools/cli/src/cmd_db.cpp` | `db rebuild-index` |
| `tools/cli/src/cmd_media.cpp`, `cmd_show.cpp`, `library_snapshot.hpp` | `media --game <id>`; `show` with the media paths; `mediaRootOf()`, `derivedMediaPaths()` |
| `tests/unit/catalog_db/import_test_support.{hpp,cpp}` | catalogs opened for the tests, index and state dumps, scripted sources and resolvers, `settleFolderTimes()` |
| `tests/unit/catalog_db/test_import_service.cpp`, `test_import_fixtures.cpp`, `test_import_real.cpp` (hidden) | the tests (below) |
| `tests/support/file_system.hpp` | `DataReadDenied` (an alias of `ListingDenied`: on a file, the data cannot be read) |
| `tests/unit/catalog/test_domain.cpp`, `tests/unit/cli/test_cli_{import,db,media}.cpp` | the in-memory store of the port test grew the new methods; the CLI tests' stub expectations replaced |
| `tests/unit/esde/test_library_source.cpp`, `test_rom_scan_fixtures.cpp` | the cache as `DiscGroup`s with their facts; a changed sheet that cannot be read keeps its cached group with its cached facts; `readEverySheet` |

Typical use (the app's catalog worker, step 13r; retroverse-cli does the same):

```cpp
auto database = rv::catalog_db::openCatalogDatabase(rv::catalog_db::databaseFile(paths));
auto store = rv::catalog_db::SqliteCatalogStore::open(*database).value();
rv::esde::EsdeLibrarySource source;
rv::esde::EsdeMediaResolver media(rv::esde::resolveMediaRoot(settings, profile));
rv::catalog::ImportService service(source, media, *store);   // + an IIdentityPairing from step 11b
rv::catalog::ImportOptions options;
options.cancel = &stopRequested;                              // std::atomic<bool>
auto result = service.run(profile, options);                  // ChangeSet, statistics
```

The service, the source and the resolver may be kept for many runs: each run loads the library anew and refreshes the
resolver (`IMediaResolver::refresh()`), whose listings would otherwise answer from the first run.

## The import, step by step

1. **The import lock**, from the start: `ICatalogStore::lockImport()` takes it without waiting (byte 2 of the
   database's lock file, step 4), so a second import, of this process or another, is refused at once (`Conflict`) before
   it reads anything. It is held to the end, apart from the write transaction.
2. **The library** (ADR-0008 §4), under the lock: the store is the authority on its id. When the store holds libraries
   and none is the one library.json names, the import is refused (`Conflict`, context `library.json`); see "The
   library's id". Checked under the lock, two imports never both see an empty store (and write two libraries).
3. **The snapshot**: the disc groups the index holds are read first and handed to the load as its cache
   (`LibraryLoadOptions::discGroups`: each group with its references and the facts of the head they were read from),
   then `ILibrarySource::load()` (`db rebuild-index` sets `readEverySheet`). Then `IMediaResolver::refresh()` and
   `presence()` for every entry with a key.
4. **What the store holds, and what changes**, under the import lock alone: the GameIds taken, the systems of the index
   and of the identities, then per system its identities and index rows. Whatever writes identities, aliases or index
   rows holds the import lock (the import; step 11b's quick-hash pass and review resolution must take it too), so
   nothing can change them before the writes. Each system is planned (below), the pairing pass included; new keys get
   their GameIds in one batch; the writes are collected.
5. **The write transaction** (`beginImport()`, which uses the lock already held), for the writes alone: the library
   row, `prepareWrites()` (the search index suspended for a bulk write), for a rebuild `clearIndex()`, identities that
   become aliases (`putAlias()`), systems, identities (those that exist first, then the new ones, each in ascending
   GameId order), index rows (ascending GameId, the unchanged ones skipped), removals, removed systems, disc groups, the
   import's issues, its record; `commit()` rebuilds a suspended search index, records the key and sort key versions,
   commits and runs the WAL checkpoint (step 4).
6. **Cancellation** (`ImportOptions::cancel`) is looked at after the load, every 256 media questions, between systems and
   every 256 writes; the transaction, if begun, is then dropped (a rollback: the store as it was, DDL included) and the
   run ends with `Cancelled`.

## Identities

- Every entry with a key has an identity; entries outside their system folder have none and are never indexed.
- **Matching** (ADR-0008 §4): a system's stored identities are read whole (step 4's design: no index by key) and matched
  in memory: the exact stored relative path first, then the key, an entry taking the first unmatched identity of its
  key in (key, path) order. A file renamed in case only keeps its GameId and the identity takes the new spelling.
- **New keys** get `assignGameIds()` (step 1) over the whole library in (system, key, path) order, taken meaning stored
  (identities and aliases) or given earlier in the batch: on an empty database every GameId is its tuple's value; two
  names that fold to one key get their tuple's value and the next counter, and the import reports
  `import-key-collision` (info).
- **A matched identity follows its entry**: seen on disk (available, file facts), it takes the entry's relative path,
  file facts and `present`; a MissingFile entry (file gone, gamelist entry left) clears `present` and keeps the last path
  and facts; an identity no entry names, in a folder that was listed, clears `present` (vanished: gamelist entry or not);
  under a folder that could not be listed it stays as it was (out of any reconciliation, never taken as vanished).
- **Re-keying**: an identity whose stored key version (`StoredIdentity::keyVersion`: rule and casing table) is not
  `currentLookupKeyVersion()` is written again with the key computed now, in place (the store computes the key again
  from the path when it reads it, so matching never sees a stale key); `rv_meta` then says the version every key is of.
- **The import never removes an identity** (only a pairing turns one into an alias, see the seam). A file that comes back
  at its path or key gets its GameId back (SC-05, SC-06). A new ROM root or drive letter changes no relative path, so no
  GameId; a re-created library.json naming the same library changes nothing either.
- **Unchanged identities are not written.** `last_seen` (step 4) therefore says when an import last wrote the identity
  with its file present (first seen, or its path or facts changed), no longer "the last import that saw its file":
  writing every identity at every import would cost 100,000 writes for nothing. When the library was last imported is
  in `rv_imports`.

## What the index holds (`SystemOutcome`)

| Outcome | When | The system's rows | Its identities |
|---|---|---|---|
| `applied` | its folder was listed | every entry of the snapshot with a key, with its media and sort key; rows whose entry left the snapshot removed; rows below a folder that could not be listed kept as they were, marked `source-unavailable` with the folder's reason, and entries the gamelist alone names there added | follow their entries |
| `source-unavailable` | its folder is missing or cannot be listed (unplugged drive, ROM root renamed or emptied) | every row kept as it was, marked `source-unavailable` with the system's reason; entries its gamelist alone names added; nothing removed | as they were |
| `gamelist-failed` | its gamelist exists but failed (`GamelistState::Failed`) and the index holds rows of it | kept as they were; the system row written with gamelist "failed" and the alternative emulator it held | as they were |
| `metadata-pending` | the same on its first import (no row of it) | none (not imported unscraped); the system row says "failed" | none given |
| `kept-by-guard` | more than half of its present files gone (below) | kept as they were, the system row too | as they were |
| `not-configured` | the configuration no longer defines it (and the guard lets it go) | removed, its system row too | kept, not present |

Once its rows and system row are gone and no file of it is present, a system of identities alone is left out of the
ChangeSet (and of the records) and reported no more; its identities are still re-keyed when their key version is old.

Rows are compared whole (`IndexedEntry ==`: entry, media, sort key) with what the index holds: an unchanged row is not
written (step 9: an unchanged upsert still runs the search trigger and rewrites the order index). A vanished file
without a gamelist entry leaves the index; with one, its MissingFile entry stays (FR-033). The index is thus a function
of the snapshot, the identities and, where the snapshot cannot tell, the rows it held (folders that cannot be listed,
gamelists that failed, sheets that cannot be read, media out of reach, systems kept by the guard): ADR-0008 §5 says "a
function of the snapshot and the identity table", see "For the merge".

## The ChangeSet

`ChangeSet::systems` gives one `SystemChanges` per system looked at (the snapshot's, the index's and the identities'),
by name. The rows the index holds for a system after the import are partitioned: `added` (written, new to the index),
`changed` (written again: entry, media or sort key differ; availability changes included), `unchanged` (not written),
`unavailable` (below a folder that could not be listed: kept or added from the gamelist alone), `kept` (the system was
not applied); `removed` counts the rows that left. Then the identities' changes (`newIdentities`, `vanished`,
`returned`, `rekeyed`, `moved`), the guard's figures (`presentBefore`, `gone`) and the system's `issues`. The lists
(`added`, `removed`, `metadataChanged`, `availabilityChanged`, `moved`, `reviews`) name the games (GameId, system,
key; a move names the identity that became its alias, `MovedGame::alias`), and `issues` holds the snapshot's issues
followed by the import's. `firstImport` says the library had nothing.

The store's record of an import (`ImportRecord`, `rv_imports`) keeps the figures of the systems it changed or reported:
something added, changed, removed, new, vanished, back, re-keyed or moved, an issue, or an outcome other than `applied`
(and `source-unavailable` without rows). A refresh that changed nothing records no system: on the reference library a
record went from about 42.7 KB (195 systems) to a few hundred bytes (the systems with the 13 standing issues). The
newest 100 records of a library are kept (`kKeptImportRecords`; `recordImport()` deletes the older ones in the import's
transaction), so the history stays bounded although the app imports at every start.

Issue codes of the import (besides the snapshot's): `import-mass-disappearance` (warning), `import-rom-root-unavailable`
(warning: every system folder that held files is missing or unlistable at once), `import-key-collision` (info),
`import-gamelist-failed` (warning: rows kept, or metadata pending), `import-system-not-configured` (warning).

## The mass-disappearance guard

Per system, after the pairing pass (the seam): `presentBefore` counts the identities present at the last import outside
the folders that cannot be listed now; `gone` those of them whose file is gone (a folder listed, the file not there,
gamelist entry or not) and that were not paired with a new file. When `gone * 2 > presentBefore`, nothing changes for
the system (its rows, identities, groups and system row stay; new files wait too) and the import reports it, unless
`ImportOptions::acceptRemovals` (`--accept-removals`). Half gone is no mass disappearance. A system that is no longer
configured counts every present file as gone. An empty ROM root is no disappearance of files: every system folder is
then missing, so every system is `source-unavailable` (rows kept and marked, nothing removed) and the import reports
`import-rom-root-unavailable`; a ROM root whose system folders are there but emptied trips the guard per system.

## The import lock, the transaction, the search index

- One import at a time, also across processes: `lockImport()` (step 4's import lock byte, `Conflict` at once) before
  anything is read; `beginImport()` of the same store uses it. A lock that goes while its store's transaction is open
  is kept by the transaction to its end. `retroverse-cli import` refused this way exits 3 ("another import runs").
- The write transaction (BEGIN IMMEDIATE) holds the writes alone: the library, the media and the store are read before
  it, under the import lock. Other writers of the database (play sessions and launch choices, step 12) wait for it:
  0.05 s for a refresh that changes little, but on the reference library 5 to 9 s for a bulk write (a first import, `db
  rebuild-index`, the ROM root unplugged or back: every row written again, the search index rebuilt), as long as or
  longer than the writer's 5 s busy timeout (`DatabaseOptions::busyTimeout`). A session written from another process during such an
  import must retry (or queue) for as long as the import's writes last, or use a longer busy timeout; the app's own
  sessions share the catalog worker's writer and simply follow the import.
- The search index (step 9): `prepareWrites(rows)` before the writes. `SearchIndexPolicy::Automatic` (the product's)
  suspends it for a first import (the index holds no entry, of any library) and when the rows written are at least a
  fifth of the index (`kBulkWriteShare`: the triggers cost about 75 µs a row, the rebuild about 15 µs per row of the
  whole catalog); `commit()` rebuilds it. `Suspend` and `FollowWrites` are for tests: a first import's index equals
  the one the triggers build, term for term (tested on scripted entries and on the standard fixture).
- A WAL checkpoint after each commit (step 4's `commit()`).

## Disc groups

The groups of each applied system are written with their references (`idx_disc_references`: head, position, relative
path, files that are not there included) and their members by GameId; unchanged groups are skipped, groups whose head
left are removed (with the head's row, `removeEntry()`), groups whose head lies below a folder that cannot be listed are
kept as the index held them, as the head's row is (a member that became an alias is named by the identity it stands
for). The next load takes them back as its cache (`ICatalogStore::discGroups()`: by system, then head; the references
with `DiscGroup::headSize` and `headLastWriteTime`): a sheet whose key, size and time are those stored is not read
again. On the reference library the second import read no sheet.

The facts stored with a group are the source's: those of the sheet its references were read from. A sheet read at this
load gives the listing's; a group kept from the cache keeps the cached ones, whether the sheet was unchanged, could not
be read (a changed sheet denied or locked: `disc-sheet-unreadable`, "the group read before is kept") or lies below a
folder that cannot be listed. Storing the facts listed now with references kept from an older sheet would make those
references pass for the new sheet at every later load, and `db rebuild-index` too (the review's finding): the group
would never follow the sheet again. With the cached facts kept, the next load that can read the sheet reads it. `db
rebuild-index` reads every sheet that can be read (`LibraryLoadOptions::readEverySheet`: 2,520 sheets on the reference
library, about 0.04 s warm, step 7's 4 to 5 s estimate cold), so a group stored wrong is repaired; the cache still stands
for heads that cannot be read or listed.

## Media

Per entry and kind, presence and extension (`idx_media`, step 4); paths are derived again with
`IMediaResolver::path()` (`media --game`, `show`). When `presence()` is `Unavailable` (the media root out of reach, a
media folder that cannot be listed), the entry keeps the media its row held; a first import then stores none. Each run
calls `IMediaResolver::refresh()` first (step 8's note: one listing per import): `EsdeMediaResolver` answers from
per-system listings it keeps until then, so a service and resolver kept by the app's catalog worker would otherwise
store the first run's media forever (tested: a screenshot deleted and a cover added between two runs of one service).

## The library's id and the re-association

The store's `rv_libraries` is the authority (ADR-0008 §4): a library.json naming a library the catalog does not hold,
while it holds one, is refused by the import (`Conflict`, context `library.json`) and by `retroverse-cli import` and `db
rebuild-index` (exit 2), with the message naming both and the re-association to run:

```text
retroverse-cli library create --id <the catalog's library> --force   (with the folder options library.json was made with)
```

library.json only names the library, so naming the catalog's again is the whole re-association: no state changes, every
GameId stays. A catalog meant to be started anew is deleted. A library.json re-created with the same id (the default
`esde`), and a ROM root moved to another folder or drive letter (`library create --rom-root`), keep every GameId
(tested).

## `retroverse-cli`

```text
retroverse-cli [global options] import [--system <name>] [--accept-removals] [--json] [--output <file>]
retroverse-cli [global options] import --dry-run [--system <name>] [--json] [--output <file>]   (step 7, unchanged)
retroverse-cli [global options] db rebuild-index [--json] [--output <file>]
retroverse-cli [global options] media --game <id> [--json] [--output <file>]
retroverse-cli [global options] show <id> [--json] [--output <file>]                            (media paths added)
```

- `import` makes and migrates `<user data>/database/retroverse.sqlite` when needed and prints the library and the
  database, then one line per system that has rows, changes or issues, or an outcome other than applied or
  source-unavailable (a system whose folder is missing and that holds no rows is only in the JSON): added, changed,
  removed, unchanged, unavailable, kept, issues, the outcome (`kept-by-guard (5 of 9 present files gone)`); the totals,
  the identities' changes, a line per system kept by the guard naming `--accept-removals`, the issues, and the times
  (library read, media, comparison, writes) with the rows written and the sheets read or taken from the catalog. JSON:
  `library`, `esdeHome`, `database`, `kind`, `firstImport`, `acceptRemovals`, `system`, `systems` (each with its
  outcome and figures, `rows` among them), `totals`, `moved` (each with `alias`, null unless an identity became its
  alias), `reviews`, `issues`, `statistics`. `--system` imports one
  system (a partial import: the other systems stay as they are). Exit codes: 0; 1 an error issue (a folder that cannot
  be listed, a gamelist that failed) or a system kept by the guard; 2 invalid options, no library.json, a system the
  configuration does not define, a library.json naming another library; 3 the library or the database cannot be read or
  written, another import runs, the results cannot be written.
- `db rebuild-index` runs the same import with `ImportOptions::rebuildIndex` and prints the same summary (`Rebuilt in
  ...`; every sheet read again); its exit codes are `import`'s.
- The library check: `retroverse-cli` looks at the catalog's library first only to word the refusal (with the
  re-association to run); the import checks it again under its lock, and a refusal there (another import made the
  catalog meanwhile) is worded the same way.
- `media --game <id>`: the game's media as the last import stored them, with each file's path derived under the media
  root of the library library.json records (the game must be of that library: else exit 2); JSON `game`, `title`,
  `system`, `path`, `directory`, `mediaName`, `mediaRoot`, `media` (12 items: `kind`, `present`, `extension`, `path`).
  Exit 1 for a game the catalog does not have, 3 when there is no catalog. Nothing on disk is looked at.
- `show` adds under its `Media` line one line per kind present with the derived file (JSON: `media[].path`, null when
  the media root is not known), when library.json names the game's library.

## `db rebuild-index`

`ImportOptions::rebuildIndex`: the import as above, from a fresh snapshot (every cue sheet and playlist that can be read
is read again: `LibraryLoadOptions::readEverySheet`), but in its write transaction (after `prepareWrites()`, which
suspends the search index) the library's index tables are cleared (`clearIndex()`: systems, entries, media, disc groups
with members and references, import issues) and every row is written again, from the snapshot, the identities and,
where the snapshot cannot tell, the rows read before the clear (rows and disc groups below folders that cannot be
listed, groups whose sheet cannot be read, systems whose gamelist failed or that the guard keeps, media out of reach). The state rows stay (identities, libraries, records; the import adds its own
record, kind `rebuild-index`). Tested to give the index a refresh gives, table by table and term by term, and to keep
the state: on scripted libraries with all of the above, on the standard fixture through the CLI, and on the reference
library. An index damaged by hand (references deleted, a full name changed) is repaired, also a disc group whose head
kept its size and time (tested on the standard fixture: a refresh trusts it, the rebuild reads the sheet). A partial
rebuild is refused.

## The seam for step 11b (reconciliation)

`IIdentityPairing::pair(PairingRequest)` is called once per system whose gamelist was read that has both vanished
identities and new files, after the stored identities are matched and before any GameId is given out, under the import
lock and before the write transaction (nothing is written yet; whatever writes identities holds the same lock, so what
the pass reads of the store cannot change, and hashing new files keeps no other writer waiting):

- `PairingRequest::vanished`: `VanishedIdentity` (the stored identity with its last path and facts, `present` true;
  `gamelistEntryKept` when a MissingFile entry of the snapshot still names it at the old path), in the stored order;
- `PairingRequest::newFiles`: `NewFile` (key, file facts, state, `heldBy`) for every entry seen on disk whose path and
  key match no identity, and for every one matched to an identity that was not present at the last import (`heldBy`:
  that identity; a file that comes back, or one that lands where a MissingFile entry kept an identity since a move), in
  entry order; `system` (its ROM folder, where the new files can be read); `cancel`;
- the result, `IdentityPairs`: `pairs` (`IdentityPair`: the vanished GameId, the new file's key, the `MoveReason`), each
  identity and each file at most once (`InvalidArgument` otherwise, and nothing is written); `reviews`; `issues`.

The import applies a pair as a move: the new file's entry takes the vanished GameId, the identity takes the new path,
facts and `present`; a MissingFile entry left at the old path then has no identity and gets a new GameId (the tuple's
counter: its own value is the moved identity's), as ADR-0008 §4 asks; `ChangeSet::moved` and `SystemChanges::moved`
record it; the guard counts what stays gone. When the file had a holder (`NewFile::heldBy`), the holder becomes an alias
of the moved identity, ADR-0008 §4's moved-back rule ("a file moved back onto a key that has an identity keeps the
identity with history and records the other as an alias"): `MovedGame::alias` names it, and the write transaction's
first step, `IImportTransaction::putAlias(IdentityAlias)`, removes the index rows that name the holder (its entry and
media, the group it heads, its places among other groups' members), records its GameId in `rv_identity_aliases` with its
key and path (never given out again: `takenGameIds()`), and deletes its identity row, which frees its path before the
moved identity is written there. Left unpaired, a file with a holder keeps the holder's identity (it comes back). The
other identities that exist are written before new ones, so the old path of a moved identity is free when the new
identity of that path is written (step 4's unique (library, system, path)).

A test pairing (equal name and size) proves the seam: the move, the old path's new GameId, no UNIQUE conflict; the file
moved back where the gamelist entry stayed (the holder offered as `heldBy`, the moved identity at its old path again with
its GameId, the holder an alias, its row gone, its GameId still taken, nothing written at the next import); a file back
at its path that the pass does not pair keeping its holder; a pairing that names what it was not given refused with
nothing written.

How step 11b would use it: implement `IIdentityPairing` over the store, reading the vanished identities' stored quick
hashes (`StoredIdentity` or a store read of its own must then carry `quick_hash`, `quick_hash_size` and
`quick_hash_time`, which the store does not read yet); hash only the new files whose size equals a vanished identity's
hash size; pair by hash, then by a unique size, time and name, ties by name and the longest common path suffix, groups
of identical copies in key order (ADR-0008 §4); return review rows for the rest. 11b adds the writes of history
(`rv_identity_history`: the move with its reason, the alias; also the re-keyed identities of this step, which write none
yet) and of review rows (`ChangeSet::reviews` already reaches the CLI's JSON) to `IImportTransaction`, called from the
write phase as `putAlias()` is; `putAlias()` fails with a FOREIGN KEY error while a state row still names the holder
(its history, its review rows, step 12's sessions and choices, an alias that points at it), so 11b moves those to the
moved identity first (or in `putAlias()`). Without a pairing pass (11a), a moved file is a vanished identity and a new
one.

## Decisions taken within the step

1. **The import lock is held from before the load, the write transaction only for the writes** (`lockImport()`, a new
   port method): a second import is refused before it reads the library; the library, the media and the store are read
   and compared under the lock alone (whatever writes identities, aliases or index rows holds it), so the write
   transaction lasts as long as the writes: 0.05 s for a refresh that changes little, 5 to 9 s on the reference library
   for a bulk write. Play sessions of another process thus wait only for the writes, which for a bulk write can still
   outlast the writer's 5 s busy timeout: they must retry (see "The import lock").
2. **Unchanged rows and identities are not written** (step 9's note), so a second import writes nothing: `last_seen`
   moves only when an identity is written with its file present; `rv_imports` records each import.
3. **Rows below folders that cannot be listed are kept as they were** (state, file facts, metadata, media), only their
   availability marked with the reason; entries the gamelist alone names there and the index did not hold are added as
   the snapshot gives them. Their metadata catch up once the folder is listed again.
4. **A gamelist that failed keeps everything of the system** (rows, identities, groups), files added or removed
   meanwhile included: they wait for an import that reads the gamelist. The system row is still written, with gamelist
   "failed" (so `systems` shows it) and the alternative emulator it held. On its first import nothing is imported
   (metadata pending), the system row says "failed".
5. **A vanished file without a gamelist entry leaves the index** (its identity stays, not present): the index is a
   function of the snapshot and the identities, so `db rebuild-index` equals a refresh.
6. **The guard counts after the pairing pass** and holds back everything of the system, new files included; a system no
   longer configured is under it while it had present files; an empty ROM root makes every system
   `source-unavailable`, never a removal.
7. **The library's re-association is `library create --id <catalog's id> --force`**: library.json only names the
   library; no new command or state.
8. **`db rebuild-index` is an import that clears the index and writes every row**, taking from the index read before the
   clear only what the snapshot cannot tell, so that it equals a refresh in every case (folders unlisted, gamelists
   failed, media out of reach) and repairs a damaged index.
9. **Disc references in their own table** (`idx_disc_references`, by relative path, step 7's suggestion): references
   name files that are not there, without identity.
10. **Import records in a state table** (`rv_imports`): the per-system figures as JSON, kept by `db rebuild-index`; the
    import issues stay index rows (`idx_import_issues`, the last import's).
11. **The search index policy is the store's** (`prepareWrites(rows)`, `StoreOptions::searchIndex`): the import service
    only says how many rows it writes.
12. **The commit records `sort_key_version`**: rows of another sort key folding differ from what an import computes and
    are written again; the version is recorded with them (rows kept by the guard or a failed gamelist keep theirs until
    their system is applied).
13. **A disc group carries the facts of the sheet its references came from** (`DiscGroup::headSize`,
    `headLastWriteTime`, set by the source; step 7's `CachedDiscGroup` folded into `DiscGroup`), so that references kept
    from an older sheet (unreadable, or below a folder that cannot be listed) never pass for the sheet listed now.
    Groups whose head lies below a folder that cannot be listed are kept as the index held them.
14. **`db rebuild-index` reads every sheet** (`LibraryLoadOptions::readEverySheet`): the index from a fresh snapshot, the
    cache standing only for sheets that cannot be read or listed.
15. **The moved-back rule has its seam** (`NewFile::heldBy`, `MovedGame::alias`, `IImportTransaction::putAlias()`, the
    first write): the pairing pass decides, the import applies, the store records the alias.
16. **Import records are bounded**: only the systems with something to say, the newest 100 per library.
17. **The import refreshes the media resolver at every run** (`IMediaResolver::refresh()`, a new port method with a
    no-op default), so that a service kept by the app sees the media as they are.
18. **A system of identities alone** (no row, no system row, no file present: removed earlier as not configured) is left
    out of the ChangeSet and reported no more.

## Tests

`./scripts/test.ps1` passes every unit test in Debug and in Release (1,189), both built without a warning; the
import's (`rv_tests "[import]"`: 37 test cases, and the hidden real-library one):

- `tests/unit/catalog_db/test_import_service.cpp` (scripted snapshots and media on the SQLite store): a first import
  (GameIds as their tuples derive them, a key collision and its counter, the entry outside its folder never indexed, a
  MissingFile identity without facts, the record and the issues kept, the search index suspended) and a second one that
  writes nothing; the exact path then the key (a case rename keeps the id), stale keys computed again in place;
  removed and added files, a file that comes back with its id; a file gone with its gamelist entry left (MissingFile,
  media kept, identity not present with its last facts); media unknown keep what was stored, known again write "none";
  a failed gamelist (metadata pending on a first import, rows kept after, system row "failed" with its alternative
  emulator); folders that cannot be listed, a system folder unreachable, the folders back (same GameIds); the guard
  (half gone applied, more than half held, then accepted); a system no longer configured (held, then removed, then
  back with its id); the seam (a move, the old path's new GameId, a bogus pairing refused with nothing written; the
  guard counting after the pairing: a system's only file moved is kept by the guard without a pairing pass, with no
  move reported, and applied with one);
  cancellation while the library is read, while the media are asked, in the middle of the writes (index and state as
  they were, no record; the lock released); one import at a time (another holder's lock or transaction, a lock that goes
  while its transaction is open); a partial import; disc groups with references, given back as the cache, rewritten,
  removed with their head; `db rebuild-index` against a refresh with unlisted folders, a failed gamelist and media out of
  reach (equal index and state), and repairing a damaged index; the search index of a bulk write against the triggers';
  another library refused, a new ROM root keeping every id; the moved-back seam (the holder offered as `heldBy`, made an
  alias, its GameId still taken; unpaired, the holder keeps the file); a system of identities alone no longer reported;
  the order of the store's calls (the lock, then the library, the cache before the load, the comparison's reads before
  the write transaction, which holds the writes alone; the resolver refreshed at every run); the import records (only
  the systems with something to say, the newest `kKeptImportRecords` kept).
- `tests/unit/catalog_db/test_import_fixtures.cpp` (`esde::EsdeLibrarySource` and `EsdeMediaResolver` on fixtures): the
  standard fixture as its README counts it (22 rows, every state per system, GameIds, the disc group with its two
  references and members, the media table, snes's alternative emulator), imported again without a write (the sheet taken
  from the catalog), the fixture unchanged, nothing of it in the data directory (only the database's files, under 4 MiB)
  and its scraper credentials nowhere in the database's bytes; SC-05 on disk (two ROMs added in sub-folders, one
  removed, put back with its id); SC-06 on disk (gb's folder renamed away and back, the ROM root renamed away and back,
  `Collection A` unlistable: rows kept, marked, nothing removed, the same GameIds after); a gamelist cut short at test
  time (metadata pending first, imported once whole, kept when cut again); a first import that reads no ROM byte (every
  file under `roms/` and `media/` denied reading its data, but the cue sheet: the same index as with every file
  readable); the standard fixture's search index bulk against the triggers'; the user's custom systems (SC-11: a system
  of the user's own imported like any other; a media root that is not there: media unknown); the long-path fixture
  (paths past 260 characters below `roms/`, its cue sheet read); a changed sheet that cannot be read (the group read
  before kept with its facts, then read and followed once readable); a disc group below a system folder that cannot be
  listed, the gamelist naming its tracks (the disc tables unchanged, then nothing written once listed again); `db
  rebuild-index` reading a sheet a refresh trusts (a reference deleted by hand, repaired); a service and resolver kept
  between runs (a screenshot deleted, a cover added: both seen).
- `tests/unit/esde/test_library_source.cpp`, `test_rom_scan_fixtures.cpp`: groups carry their sheet's facts (read: the
  listing's; kept: the cache's, also for an unreadable or locked sheet); `readEverySheet` reads a sheet the cache would
  stand for.
- `tests/unit/cli/test_cli_import.cpp`: the import's text and JSON, a second import, `media --game` (text, JSON, a
  folder's media, an unknown game), `show` with the paths, `db rebuild-index`, `db check`, the data directory's files;
  another library refused (exit 2, the re-association named), re-associated with `library create --id esde --force`, a
  new ROM root (no write but the systems' rows); one import at a time in-process and from a second `retroverse-cli`
  process (exit 3); the guard's exit 1 and `--accept-removals`. `test_cli_db.cpp` and `test_cli_media.cpp`: the stubs'
  expectations replaced by the new commands' usage errors.

**Folder times as Windows lists them.** A folder's last write time comes from its parent folder's index (FindFirstFile,
`core::walkTree`), a copy of the folder's own that NTFS brings up to date later, on its own, after the folder's content
changed (seconds after a freshly made tree is written, or when the parent changes). Two imports a moment apart can then
see two times for one folder: a Folder entry's row (or a game directory's) is written again, which is right. The fixture
tests settle the times of the ROM folders first (`settleFolderTimes()`: a time set through a handle is written to both
at once), so that they compare imports exactly; the reference library's folders, last written weeks ago, are settled.

## Real library

`./scripts/real-library.ps1 -EsdeHome C:\Users\sinhu\ES-DE -EsdeInstall E:\Programmi\ES-DE -RomRoot I:\Giochi\Emulatori
-MediaRoot E:\Programmi\ES-DE_media\downloaded_media` (Release, ES-DE closed), with the default filter (9 test cases: 7
passed, the import's among them, the two launch tests skipped without `-RetroArchExe`): 0 differences among 542,082
entries. The step was first run so twice (also with `-Filter '[real-library][import]'`), then again after the review's
fixes, whose figures and timings are below. The database is made anew under `build/msvc-x64-release/real-library/import`.

- **The first import** gives step 7's figures: 105,130 identities and index rows (90,246 games, 12,821 disc members,
  1,038 missing files, 870 not games, 155 folders), 2,513 disc groups (12,821 member rows, 12,841 references: psx's cue
  sheets that name tracks not there among them), 388,098 media rows, 195 systems, 13 import issues (step 7's 12
  `disc-reference-missing` and pcfx's `extension-without-dot`), no row source unavailable; the search index suspended
  and rebuilt (966,896 (term, row) pairs). The database: 33,642 pages of 4 KiB (131 MiB). Its record: 10.6 KB (48
  systems).
- **The second import** changes no identity and writes no row (0 identity rows, 0 entry rows, 0 removed, 0 disc groups,
  0 systems): 105,130 rows unchanged; 2,513 sheets taken from the catalog, 7 read (heads that make no group are not
  cached and are read at every load, step 7). Its record: 441 bytes (the 2 systems with standing issues; 42.7 KB, 195
  systems, before the review).
- **`db rebuild-index`** reads the 2,520 sheets again, writes the 105,130 rows and the 2,513 groups again and gives the
  same index (SHA-256 of every index table and of the search index's terms equal) and the same state.
- **The ROM root unplugged** (library.json's ROM root an empty folder under `build/`): every row kept as source
  unavailable, nothing removed, no identity written, the disc tables unchanged (0 groups written: before the review, 8
  groups were written again with a head of no size, and the import after read 15 sheets instead of 7); **back**: the
  same index and state as after the second import, the sheets taken from the catalog, 0 groups written.

Timings (informative: the drive and the media tree were warm from the snapshots the script takes just before, and other
work may have run; the gate measures on a quiet machine). The comparison now runs before the write transaction, which
holds the writes alone (the last column):

| Run | Total | Library read (of it sheets) | Media | Comparison | Writes and commit |
|---|---|---|---|---|---|
| first import | 12.01 s | 1.59 s (0.03 s: 2,520 sheets read, warm) | 0.97 s | 0.62 s | 8.82 s (the search index rebuilt in it) |
| second import | 4.68 s | 1.77 s (7 sheets read) | 1.02 s | 1.82 s | 0.06 s |
| `db rebuild-index` | 13.06 s | 1.77 s (0.04 s: 2,520 sheets read) | 0.99 s | 1.96 s | 8.33 s |
| ROM root unplugged | 10.15 s | 0.93 s | 0.92 s | 1.94 s | 6.36 s |
| ROM root back | 10.16 s | 1.72 s (7 sheets read) | 1.06 s | 2.04 s | 5.34 s |

Before the review, the first import took 10.57 s and 10.74 s (writes 7.39 s, 7.53 s), the second 4.54 s and 4.57 s, the
rebuild 13.05 s and 13.65 s (writes 8.38 s, 8.73 s): the same work, the variance of a busy machine. The second import's
comparison is the reads of 105,130 identities and index rows with their 388,098 media rows (step 4 measured about 1 s
per 100,000 in Release) and the comparison in memory; the first import's writes are step 4's and 9's figures
(identities and rows in ascending GameId order, the search index rebuilt once). A cold first import adds the cold
listing (about 10 s on the external drive, ADR-0008) and the sheets (step 7's estimate: 4 to 5 s with 8 readers), which
later imports take from the catalog, and `db rebuild-index` reads again.

## The review's findings

Two reviews of the committed step; every finding was confirmed and fixed (none rejected), two with another remedy than
the one suggested:

- **Disc groups stored with the facts listed now** (a changed sheet that could not be read kept its old references
  with the new size and time, so they passed for the new sheet for good; a head below a folder that cannot be listed
  was written again with size and time 0): the source now says which facts its references came from
  (`DiscGroup::headSize`, `headLastWriteTime`), and groups whose head lies below a folder that cannot be listed are kept
  as the index held them (decision 13).
- **`db rebuild-index` trusted the cache**: it reads every sheet (`LibraryLoadOptions::readEverySheet`, decision 14).
  The suggested empty cache was not taken: a sheet that cannot be read would then turn its tracks into games, and the
  rebuild would no longer equal a refresh.
- **The seam could not apply the moved-back rule**: `NewFile::heldBy`, the alias applied in `match()`,
  `IImportTransaction::putAlias()` before any identity is written (decision 15); the pairing pass and every read of the
  comparison moved out of the write transaction (decision 1).
- **A system removed as not configured was reported at every import**: a system of identities alone is left out
  (decision 18).
- **The library was checked before the import lock**: checked under it (the CLI's earlier look only words the
  refusal, and words a refusal from under the lock the same way).
- **`rv_imports` grew without bound**: only the systems with something to say, the newest 100 kept (decision 16; 100
  rather than the 20 suggested: a record of a refresh is now a few hundred bytes).
- **A media resolver kept between runs answered from its first listings**: `IMediaResolver::refresh()`, called at every
  run (decision 17).
- **Comments and notes**: `last_seen`'s meaning in the schema and the upsert; decision 1 and the note for step 12 (the
  write transaction can outlast the 5 s busy timeout); the CLI's listed systems; the ADR amendments in "For the merge".

## Notes for the steps that build on it

- **Step 11b (fingerprints, reconciliation):** implement `IIdentityPairing` (reconciliation.hpp) as "How step 11b
  would use it" says and pass it to `ImportService`; read and write quick hashes through new store methods
  (`StoredIdentity` can carry them, or a read of its own); add to `IImportTransaction` the history and review writes and
  call them from the write phase; the moved-back rule's alias is already applied (`NewFile::heldBy`,
  `IImportTransaction::putAlias()`), 11b adds its history and moves the holder's state rows to the moved identity before
  `putAlias()` deletes it. Re-keyed identities (`SystemChanges::rekeyed`) write no history yet. `ChangeSet::reviews`
  already reaches the CLI's JSON. The pairing pass runs outside the write transaction, under the import lock; the
  quick-hash pass and review resolution must take the import lock too (`lockImport()`), or run on the import's worker,
  since the import reads the store before its write transaction trusting that nothing else writes identities.
- **Step 12 (launches, sessions):** GameIds come from the catalog now (`show`, `games`); the session store shares the
  process's writer and waits for an import's writes only (decision 1), but from another process a bulk import's write
  transaction (5 to 9 s on the reference library) can outlast the 5 s busy timeout: retry or queue a session write that
  gets `Conflict`/busy, or give the session store a longer busy timeout. `assign` choices are state rows: `db
  rebuild-index` keeps them (it clears only `idx_*`); they reference identities, so an alias (`putAlias()`) needs them
  moved to the moved identity first (11b). Session and choice writes never touch identities or index rows (the import
  reads those before its write transaction, trusting the import lock).
- **Step 13 (the app):** run `ImportService` on the catalog worker with a cancel flag (quit, 13r), then `run` the
  queries again (step 9: `data_version` changes at the commit) and restore the selection by GameId; `ChangeSet` says
  what changed per system; the media files are `IMediaResolver::path()` of the stored presence, opened through
  `core::extendedLengthPathBelow()` (step 8's note). The service, source and resolver can be kept for the app's life:
  each run refreshes the resolver. `rv_imports` keeps the newest 100 records per library, each holding only the systems
  with something to say.

## For the merge

- **CLAUDE.md**, the retroverse-cli section: `import` (its options and exit codes), `db rebuild-index` (every sheet
  read again), `media --game`, `show`'s media paths; that `import` writes `<user data>/database/retroverse.sqlite`; the
  re-association (`library create --id <catalog's id> --force`). "Catalog database": the import's writes,
  `lockImport()` (the store read under it, the write transaction for the writes alone; whatever writes identities,
  aliases or index rows takes it), `putAlias()`, `rv_imports` (bounded: `kKeptImportRecords`), `idx_disc_references`;
  `rv_game_identities.last_seen` now means "last written with its file present" (an import writes no unchanged
  identity; when the library was last imported is in `rv_imports`). Tests: `tests/unit/catalog_db/import_test_support.hpp`,
  `settleFolderTimes()`, `DataReadDenied`.
- **docs/architecture.md**: `rv_catalog`'s import service (the seam `IIdentityPairing` with `NewFile::heldBy` and the
  aliases), the store's import writes, `tools/cli`'s import commands (no longer stubs: only `identities` for 11b and step
  12's remain). The catalog row names `CachedDiscGroup`, which is gone: a disc group carries the facts of the head its
  references were read from (`DiscGroup::headSize`, `headLastWriteTime`), the load's cache is the stored groups
  (`LibraryLoadOptions::discGroups`, with `readEverySheet`); `IMediaResolver` gained `refresh()`.
- **docs/i2/step-4.md**'s table: `idx_disc_groups` holds the facts of the head its references were read from (no longer
  `CachedDiscGroup`); `last_seen` as above.
- **docs/roadmap.md**: step 11a done, with the real library's figures.
- **ADR-0008**, suggested amendments: §4 "a mass disappearance (more than half of a system's present files gone, after
  the pairing pass) changes nothing for that system and is reported, unless the user accepts the removals; a ROM root
  missing or emptied makes every system folder unavailable: rows kept as source unavailable"; §5 "the import holds the
  import lock from before it reads the library, and reads and compares the store under it (whatever writes identities,
  aliases or index rows holds it); its write transaction holds the writes alone; rows and identities equal to what the
  import computes are not written"; §5 "index tables are a function of the snapshot, the identity table and, for what
  the snapshot cannot tell (folders that cannot be listed, failed gamelists, sheets that cannot be read, systems kept by
  the guard, media unknown), the rows the index held" (instead of "a function of the ES-DE snapshot and the identity
  table"); §5 "`db rebuild-index` reads every sheet again"; §7 "a gamelist that fails keeps the system's rows,
  identities and disc groups; its system row says so".
- **BACKLOG** candidates: the history of re-keyed identities (with 11b's history); a Folder entry's time as listed
  lagging behind the folder (an import may rewrite such rows once: harmless, see "Folder times"); `--system` for `db
  rebuild-index` (refused today).
