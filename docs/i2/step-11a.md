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
| `rv/catalog/import_service.hpp`, `src/import_service.cpp` (`rv_catalog`) | `ImportService` over the ports `ILibrarySource`, `IMediaResolver`, `ICatalogStore`: `run(profile, ImportOptions)` gives `ImportResult` (`ChangeSet` + `ImportStatistics`); `ImportOptions` (cancel flag, `acceptRemovals`, `systems` for a partial import, `rebuildIndex`); the seam for step 11b: `IIdentityPairing`, `PairingRequest`, `VanishedIdentity`, `NewFile`, `IdentityPair`, `IdentityPairs`; the import's issue codes `kImportIssue...` |
| `rv/catalog/change_set.hpp` (step 1's, extended) | `SystemOutcome`, `SystemChanges` (per system: added, changed, removed, unchanged, unavailable, kept, the identities' changes, the guard's figures, issues), `ChangeSet::systems`, `firstImport`, `total()`, `findSystem()` |
| `rv/catalog/catalog_store.hpp` (step 1's, extended) | `IndexedDiscGroup`, `ImportKind`, `ImportRecord`, `IImportLock`; `ICatalogStore::systems`, `identitySystems`, `discGroups`, `lastImport`, `lockImport`; `IImportTransaction::prepareWrites`, `clearIndex`, `removeSystem`, `putDiscGroup`, `removeDiscGroup`, `putImportIssues`, `recordImport` |
| `rv/catalog_db/catalog_store.hpp`, `src/catalog_store.cpp` (`rv_catalog_db`) | the store's side of all of the above (21 new statements in its `Sql` enumeration); `StoreOptions` with `SearchIndexPolicy` (`Automatic`, `Suspend`, `FollowWrites`) and `kBulkWriteShare`; `removeEntry()` also removes the references of the group it heads; each commit records `sort_key_version` |
| `src/catalog_db/src/schema_identity.cpp` | `rv_imports` (state: the import records) |
| `src/catalog_db/src/schema_index.cpp` | `idx_disc_references` (index: what a head says, by relative path) |
| `tools/cli/src/import_text.hpp` (new private header), `cmd_import.cpp` | `runCatalogImport()`: the import that `import` and `db rebuild-index` share, its summary as text and JSON |
| `tools/cli/src/cmd_db.cpp` | `db rebuild-index` |
| `tools/cli/src/cmd_media.cpp`, `cmd_show.cpp`, `library_snapshot.hpp` | `media --game <id>`; `show` with the media paths; `mediaRootOf()`, `derivedMediaPaths()` |
| `tests/unit/catalog_db/import_test_support.{hpp,cpp}` | catalogs opened for the tests, index and state dumps, scripted sources and resolvers, `settleFolderTimes()` |
| `tests/unit/catalog_db/test_import_service.cpp`, `test_import_fixtures.cpp`, `test_import_real.cpp` (hidden) | the tests (below) |
| `tests/support/file_system.hpp` | `DataReadDenied` (an alias of `ListingDenied`: on a file, the data cannot be read) |
| `tests/unit/catalog/test_domain.cpp`, `tests/unit/cli/test_cli_{import,db,media}.cpp` | the in-memory store of the port test grew the new methods; the CLI tests' stub expectations replaced |

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

## The import, step by step

1. **The library** (ADR-0008 §4): the store is the authority on its id. When the store holds libraries and none is the
   one library.json names, the import is refused (`Conflict`, context `library.json`); see "The library's id".
2. **The import lock**, from the start: `ICatalogStore::lockImport()` takes it without waiting (byte 2 of the database's
   lock file, step 4), so a second import, of this process or another, is refused at once (`Conflict`) before it reads
   anything. It is held to the end, apart from the write transaction.
3. **The snapshot**: the disc groups the index holds are read first and handed to the load as its cache
   (`LibraryLoadOptions::discGroups`: each group with its head's size and time and its references), then
   `ILibrarySource::load()`. Then `IMediaResolver::presence()` for every entry with a key. Neither runs inside the write
   transaction (seconds on a warm drive, more on a cold one), so other writers (play sessions, step 12) wait only for
   the comparison and the writes.
4. **The write transaction** (`beginImport()`, which uses the lock already held): the GameIds taken, the systems of the
   index and of the identities, then per system its identities and index rows. Each system is planned (below); new keys
   get their GameIds in one batch; the writes are collected, then made: the library row, `prepareWrites()` (the search
   index suspended for a bulk write), for a rebuild `clearIndex()`, systems, identities (those that exist first, then the
   new ones, each in ascending GameId order), index rows (ascending GameId, the unchanged ones skipped), removals, removed
   systems, disc groups, the import's issues, its record; `commit()` rebuilds a suspended search index, records the key
   and sort key versions, commits and runs the WAL checkpoint (step 4).
5. **Cancellation** (`ImportOptions::cancel`) is looked at after the load, every 256 media questions, between systems and
   every 256 writes; the transaction is then dropped (a rollback: the store as it was, DDL included) and the run ends
   with `Cancelled`.

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
- **Identities are never removed.** A file that comes back at its path or key gets its GameId back (SC-05, SC-06). A new
  ROM root or drive letter changes no relative path, so no GameId; a re-created library.json naming the same library
  changes nothing either.
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

Rows are compared whole (`IndexedEntry ==`: entry, media, sort key) with what the index holds: an unchanged row is not
written (step 9: an unchanged upsert still runs the search trigger and rewrites the order index). A vanished file
without a gamelist entry leaves the index; with one, its MissingFile entry stays (FR-033). The index is thus a function
of the snapshot, the identities and, where the snapshot cannot tell, the rows it held (folders that cannot be listed,
gamelists that failed, media out of reach, systems kept by the guard).

## The ChangeSet

`ChangeSet::systems` gives one `SystemChanges` per system looked at (the snapshot's, the index's and the identities'),
by name. The rows the index holds for a system after the import are partitioned: `added` (written, new to the index),
`changed` (written again: entry, media or sort key differ; availability changes included), `unchanged` (not written),
`unavailable` (below a folder that could not be listed: kept or added from the gamelist alone), `kept` (the system was
not applied); `removed` counts the rows that left. Then the identities' changes (`newIdentities`, `vanished`,
`returned`, `rekeyed`, `moved`), the guard's figures (`presentBefore`, `gone`) and the system's `issues`. The lists
(`added`, `removed`, `metadataChanged`, `availabilityChanged`, `moved`, `reviews`) name the games (GameId, system,
key), and `issues` holds the snapshot's issues followed by the import's. `firstImport` says the library had nothing.

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
kept. The next load takes them back as its cache (`ICatalogStore::discGroups()`: by system, then head; the references
of `CachedDiscGroup::group`, the head's size and time): a sheet whose key, size and time are the same is not read
again. On the reference library the second import read no sheet.

## Media

Per entry and kind, presence and extension (`idx_media`, step 4); paths are derived again with
`IMediaResolver::path()` (`media --game`, `show`). When `presence()` is `Unavailable` (the media root out of reach, a
media folder that cannot be listed), the entry keeps the media its row held; a first import then stores none.

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
  database, then one line per system that has rows, changes or issues, or another outcome than applied: added, changed,
  removed, unchanged, unavailable, kept, issues, the outcome (`kept-by-guard (5 of 9 present files gone)`); the totals,
  the identities' changes, a line per system kept by the guard naming `--accept-removals`, the issues, and the times
  (library read, media, comparison, writes) with the rows written and the sheets read or taken from the catalog. JSON:
  `library`, `esdeHome`, `database`, `kind`, `firstImport`, `acceptRemovals`, `system`, `systems` (each with its
  outcome and figures, `rows` among them), `totals`, `moved`, `reviews`, `issues`, `statistics`. `--system` imports one
  system (a partial import: the other systems stay as they are). Exit codes: 0; 1 an error issue (a folder that cannot
  be listed, a gamelist that failed) or a system kept by the guard; 2 invalid options, no library.json, a system the
  configuration does not define, a library.json naming another library; 3 the library or the database cannot be read or
  written, another import runs, the results cannot be written.
- `db rebuild-index` runs the same import with `ImportOptions::rebuildIndex` and prints the same summary (`Rebuilt in
  ...`); its exit codes are `import`'s.
- `media --game <id>`: the game's media as the last import stored them, with each file's path derived under the media
  root of the library library.json records (the game must be of that library: else exit 2); JSON `game`, `title`,
  `system`, `path`, `directory`, `mediaName`, `mediaRoot`, `media` (12 items: `kind`, `present`, `extension`, `path`).
  Exit 1 for a game the catalog does not have, 3 when there is no catalog. Nothing on disk is looked at.
- `show` adds under its `Media` line one line per kind present with the derived file (JSON: `media[].path`, null when
  the media root is not known), when library.json names the game's library.

## `db rebuild-index`

`ImportOptions::rebuildIndex`: the import as above, but after its reads (and `prepareWrites()`, which suspends the search
index) the library's index tables are cleared (`clearIndex()`: systems, entries, media, disc groups with members and
references, import issues) and every row is written again, from the snapshot, the identities and, where the snapshot
cannot tell, the rows read before the clear (rows below folders that cannot be listed, systems whose gamelist failed or
that the guard keeps, media out of reach). The state rows stay (identities, libraries, records; the import adds its own
record, kind `rebuild-index`). Tested to give the index a refresh gives, table by table and term by term, and to keep
the state: on scripted libraries with all of the above, on the standard fixture through the CLI, and on the reference
library. An index damaged by hand (references deleted, a full name changed) is repaired. A partial rebuild is refused.

## The seam for step 11b (reconciliation)

`IIdentityPairing::pair(PairingRequest)` is called once per applied system that has both vanished identities and new
files, inside the import's write transaction, after the stored identities are matched and before any GameId is given
out:

- `PairingRequest::vanished`: `VanishedIdentity` (the stored identity with its last path and facts, `present` true;
  `gamelistEntryKept` when a MissingFile entry of the snapshot still names it at the old path), in the stored order;
- `PairingRequest::newFiles`: `NewFile` (key, file facts, state) for every entry seen on disk whose path and key match no
  identity, in entry order; `system` (its ROM folder, where the new files can be read); `cancel`;
- the result, `IdentityPairs`: `pairs` (`IdentityPair`: the vanished GameId, the new file's key, the `MoveReason`), each
  identity and each file at most once (`InvalidArgument` otherwise, and nothing is written); `reviews`; `issues`.

The import applies a pair as a move: the new file's entry takes the vanished GameId, the identity takes the new path,
facts and `present`; a MissingFile entry left at the old path then has no identity and gets a new GameId (the tuple's
counter: its own value is the moved identity's), as ADR-0008 §4 asks; `ChangeSet::moved` and `SystemChanges::moved`
record it; the guard counts what stays gone. Identities that exist are written before new ones, so the old path of a
moved identity is free when the new identity of that path is written (step 4's unique (library, system, path)). A test
pairing (equal name and size) proves the seam: the move, the old path's new GameId, no UNIQUE conflict, a pairing that
names what it was not given refused with nothing written.

What 11b adds there: the quick hashes (the pairing pass reads them, and hashes new candidates), the writes of history
(`rv_identity_history`: the move with its reason; also the re-keyed identities of this step, which write none yet),
aliases (a file moved back onto a path that has an identity: that one's row removed first, step 4's note) and review
rows (`ChangeSet::reviews` already reaches the CLI's JSON): `IImportTransaction` gains those writes, `ImportRun` calls
them where it applies the pairs. Without a pairing pass (11a), a moved file is a vanished identity and a new one.

## Decisions taken within the step

1. **The import lock is held from before the load, the write transaction only for the comparison and the writes**
   (`lockImport()`, a new port method): a second import is refused before it reads the library, and play sessions of
   another process wait only for the writes (a load takes seconds warm, more cold) instead of the busy timeout.
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

## Tests

`./scripts/test.ps1` (Debug): every unit test passes; the import's (`rv_tests "[import]"`, 30 test cases):

- `tests/unit/catalog_db/test_import_service.cpp` (scripted snapshots and media on the SQLite store): a first import
  (GameIds as their tuples derive them, a key collision and its counter, the entry outside its folder never indexed, a
  MissingFile identity without facts, the record and the issues kept, the search index suspended) and a second one that
  writes nothing; the exact path then the key (a case rename keeps the id), stale keys computed again in place;
  removed and added files, a file that comes back with its id; a file gone with its gamelist entry left (MissingFile,
  media kept, identity not present with its last facts); media unknown keep what was stored, known again write "none";
  a failed gamelist (metadata pending on a first import, rows kept after, system row "failed" with its alternative
  emulator); folders that cannot be listed, a system folder unreachable, the folders back (same GameIds); the guard
  (half gone applied, more than half held, then accepted); a system no longer configured (held, then removed, then
  back with its id); the seam (a move, the old path's new GameId, a bogus pairing refused with nothing written);
  cancellation while the library is read, while the media are asked, in the middle of the writes (index and state as
  they were, no record; the lock released); one import at a time (another holder's lock or transaction, a lock that goes
  while its transaction is open); a partial import; disc groups with references, given back as the cache, rewritten,
  removed with their head; `db rebuild-index` against a refresh with unlisted folders, a failed gamelist and media out of
  reach (equal index and state), and repairing a damaged index; the search index of a bulk write against the triggers';
  another library refused, a new ROM root keeping every id.
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
  (paths past 260 characters below `roms/`, its cue sheet read).
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

(Filled after the run of `./scripts/real-library.ps1`.)

## Notes for the steps that build on it

- **Step 11b (fingerprints, reconciliation):** implement `IIdentityPairing` (reconciliation.hpp) and pass it to
  `ImportService`; read and write quick hashes through new store methods (`StoredIdentity` can carry them, or a read of
  its own); add to `IImportTransaction` the history, alias and review writes and call them where `ImportRun` applies the
  pairs (`match()`, `buildIdentityWrites()`); a file moved back onto a path held by another identity needs that
  identity's row removed (and recorded as an alias) before the moved one is written. Re-keyed identities
  (`SystemChanges::rekeyed`) write no history yet. `ChangeSet::reviews` already reaches the CLI's JSON. The quick-hash
  pass takes the import lock too (`lockImport()`), or runs on the same worker.
- **Step 12 (launches, sessions):** GameIds come from the catalog now (`show`, `games`); the session store shares the
  process's writer and waits for an import's writes only (decision 1). `assign` choices are state rows: `db
  rebuild-index` keeps them (it clears only `idx_*`).
- **Step 13 (the app):** run `ImportService` on the catalog worker with a cancel flag (quit, 13r), then `run` the
  queries again (step 9: `data_version` changes at the commit) and restore the selection by GameId; `ChangeSet` says
  what changed per system; the media files are `IMediaResolver::path()` of the stored presence, opened through
  `core::extendedLengthPathBelow()` (step 8's note).

## For the merge

- **CLAUDE.md**, the retroverse-cli section: `import` (its options and exit codes), `db rebuild-index`, `media --game`,
  `show`'s media paths; that `import` writes `<user data>/database/retroverse.sqlite`; the re-association (`library
  create --id <catalog's id> --force`). "Catalog database": the import's writes, `lockImport()`, `rv_imports`,
  `idx_disc_references`. Tests: `tests/unit/catalog_db/import_test_support.hpp`, `settleFolderTimes()`,
  `DataReadDenied`.
- **docs/architecture.md**: `rv_catalog`'s import service (the seam `IIdentityPairing`), the store's import writes,
  `tools/cli`'s import commands (no longer stubs: only `identities` for 11b and step 12's remain).
- **docs/roadmap.md**: step 11a done, with the real library's figures.
- **ADR-0008**, suggested amendments: §4 "a mass disappearance (more than half of a system's present files gone, after
  the pairing pass) changes nothing for that system and is reported, unless the user accepts the removals; a ROM root
  missing or emptied makes every system folder unavailable: rows kept as source unavailable"; §5 "the import holds the
  import lock from before it reads the library and its write transaction only for the comparison and the writes; rows
  and identities equal to what the import computes are not written"; §7 "a gamelist that fails keeps the system's rows,
  identities and disc groups; its system row says so".
- **BACKLOG** candidates: the history of re-keyed identities (with 11b's history); a Folder entry's time as listed
  lagging behind the folder (an import may rewrite such rows once: harmless, see "Folder times"); `--system` for `db
  rebuild-index` (refused today).
