# I2 step 11b: fingerprints and reconciliation

Roadmap row: I2 step 11b (wave 3, after 11a, parallel with 12). Decisions: [ADR-0008](../decisions/ADR-0008-esde-catalog.md)
§4 (fingerprints, reconciliation, the moved-back rule, review rows), §5 (state tables: history, aliases, fingerprints,
review rows). The import of step 11a hands its pairing seam (`IIdentityPairing`) the identities whose file vanished and
the new files; this step fills it: files moved or renamed keep their GameId, their history and their state rows, by their
quick hash or by a unique size, time and name, and what no rule pairs safely becomes a review row for the user. The quick
hash is computed by a progressive pass of its own. `retroverse-cli identities` shows identities, their history and the
review rows, resolves reviews and runs the pass. This page is folded into the shared documentation at the wave merge.

## What was built

| File | What it does |
|---|---|
| `rv/catalog/fingerprint.hpp`, `src/fingerprint.cpp` (`rv_catalog`, step 1's stubs) | `QuickHash` (digest, size, time), `isCurrent()`, `quickHashDigest()` (the definition, for golden vectors), `quickHashBytes()`, `readQuickHash()`, `readQuickHashBelow()`, `QuickHashReader`, `kQuickHashRule` |
| `rv/catalog/reconciliation.hpp`, `src/reconciliation.cpp` (`rv_catalog`, step 1's stubs) | `IdentityPairing` (the import's `IIdentityPairing`, with `Statistics`), `currentQuickHash()`, `runQuickHashPass()` (`QuickHashPassOptions`, `QuickHashProgress`, `QuickHashPassResult`), `resolveReview()` (`ReviewDecision`, `ReviewResolutionResult`); issue codes `quick-hash-unreadable`, `quick-hash-unavailable` |
| `rv/catalog/catalog_store.hpp` (extended) | `StoredIdentity::quickHash`; `IdentityEventKind`, `IdentityEvent`, `StoredAlias`, `ReviewResolution`, `StoredReview`; `ICatalogStore::history`, `aliasesOf`, `alias`, `reviews`, `review`; `IImportTransaction::putQuickHash`, `putHistory`, `putReview`, `resolveReview`, `resolveReviewsOf`; `putAlias()`'s contract (state rows moved) |
| `rv/catalog/import_service.hpp`, `src/import_service.cpp` (11a's, extended) | `NewFileHash`, `IdentityPairs::hashes`; `ImportStatistics::historyWritten`, `reviewsWritten`; the import writes the history (moves, aliases, re-keyed identities), the review rows (their ids into `ChangeSet::reviews`), resolves the reviews of identities whose file came back, stores the quick hashes the pairing read; the holder of a file moved back becomes an alias with `MoveReason::MovedBack` |
| `rv/catalog_db/catalog_store.hpp`, `src/catalog_store.cpp` | the store's side: identities read and written with their quick hash, 12 new statements, the history, review and alias reads, `putAlias()` resolving the holder's reviews and moving every state row that names it (found in the schema) |
| `src/catalog_db/src/schema_identity.cpp` | `rv_identity_history` gains `alias`, `review` and a CHECK on `event`; `rv_identity_reviews` gains `resolved_with` and a CHECK pairing `resolved_at` with `resolution` (schema v1 is open) |
| `rv/core/file_io.hpp`, `src/file_io.cpp` | `core::SharedFileReader`: a file opened for reading only with full sharing, read at offsets, its size and time from the handle |
| `tools/cli/src/cmd_identities.cpp` (step 0a's stub), `cmd_import.cpp`, `command.cpp` | `identities list | show | reviews | resolve | hash`; `import` passes `IdentityPairing` and prints the moves and review rows; the command's summary |
| `scripts/quick_hash_oracle.py` | the golden quick-hash vectors from Python's hashlib (`--check <file>`) |
| `tests/unit/catalog/test_fingerprint.cpp`, `tests/unit/catalog_db/test_reconciliation.cpp`, `test_reconciliation_real.cpp` (hidden), `tests/unit/cli/test_cli_identities.cpp` | the tests (below); `test_domain.cpp`, `test_import_service.cpp`, `test_catalog_store.cpp`, `test_migrator.cpp` follow the new port methods and columns |

Typical use (retroverse-cli does this; the app's catalog worker, step 13, the same):

```cpp
rv::catalog::IdentityPairing pairing;                                   // keep it for many imports
rv::catalog::ImportService service(source, media, *store, &pairing);    // 11a's import, now reconciling
auto result = service.run(profile, options);                            // ChangeSet::moved, ChangeSet::reviews

rv::catalog::QuickHashPassOptions pass;                                 // the progressive pass, between imports
pass.cancel = &stopRequested;
pass.limit = 5000;                                                      // or none: until done
auto hashed = rv::catalog::runQuickHashPass(*store, profile.id, pass);  // resumable: committed batches stay

rv::catalog::resolveReview(*store, profile.id, {reviewId, chosenGameId});   // or {reviewId, std::nullopt}
```

## The quick hash, exactly

Rule 1 (`kQuickHashRule`), `quickHashDigest()`:

- a file of n bytes, n <= 131,072 (2 x 64 KiB): SHA-256 of its whole content, each byte once; the empty file: SHA-256 of
  nothing, `e3b0c442...7852b855`;
- a file of n > 131,072 bytes: SHA-256 of its bytes [0, 65,536) followed by its bytes [n - 65,536, n);
- nothing else enters the digest (neither the size nor the name): the pairing compares sizes before digests. A folder
  has no quick hash.

`readQuickHash()` opens the file for reading only with full sharing (`core::SharedFileReader`: `GENERIC_READ`,
`FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`, through `\\?\` paths; `readQuickHashBelow()` reaches a name a
walk listed with `core::extendedLengthPathBelow()`, trailing dots and spaces kept), reads at most 128 KiB (two positioned
reads, or one), takes the size and last write time from the open handle before the reads and checks them again after
(a file written meanwhile is an `IoError`, never a hash half old, half new), and writes nothing. The hash is stored with
the size and time of the file it was read from (`QuickHash::size`, `lastWriteTime`, the schema's `quick_hash_size` and
`quick_hash_time`); it is **current** while they equal the identity's file facts (`isCurrent()`, `currentQuickHash()`),
**stale** once an import records other facts. Golden vectors: `scripts/quick_hash_oracle.py` generates 11 contents (0, 1,
3, 64 KiB - 1, 64 KiB, 64 KiB + 1, 128 KiB - 1, 128 KiB, 128 KiB + 1, 192 KiB, 1 MiB + 7 bytes; a linear congruential
generator, no short period) and hashes them with hashlib; `tests/unit/catalog/test_fingerprint.cpp` holds the table and
generates the same contents, in memory and as files; `python -I scripts/quick_hash_oracle.py --check
tests/unit/catalog/test_fingerprint.cpp` passes.

## The quick-hash pass

`runQuickHashPass(store, library, options)`, `retroverse-cli identities hash [--system <name>] [--limit <files>]`:

1. Takes the import lock (`lockImport()`) and holds it to the end: no import, resolution or other pass meanwhile (the
   import reads the identities before its write transaction trusting that nothing else writes them). Refused at once
   (`Conflict`) while another holds it.
2. Reads each system's identities (systems in ordinal order, identities in the stored order: key, then path) and the ROM
   folders the last import recorded (`ICatalogStore::systems()`); the candidates are the present files without a current
   quick hash (not the identities whose file is not present, nor folders). A system whose folder was out of reach at the
   last import is not read (its files counted `unavailable`).
3. Reads the candidates one after the other (`QuickHashReader`, `readQuickHashBelow()` by default). A file whose size or
   time on disk are no longer its identity's is not hashed (`changed`: the next import records its facts, the pass after
   hashes it); a file no longer there is `gone`; a file that cannot be read (denied, locked) is an issue
   (`quick-hash-unreadable`, warning) and the pass goes on; a drive that goes (`Unavailable`) leaves the system's other
   files for later (`quick-hash-unavailable`).
4. Commits the hashes in batches (256 files or 2 s, whichever first), each in an import transaction of its own
   (`IImportTransaction::putQuickHash()`, which writes the three hash columns alone: `last_seen` does not move), so that
   other writers (play sessions) wait only for a batch. Cancelled (`options.cancel`), it commits what it read and ends
   (`cancelled`); `--limit` bounds the files read. **Resumable**: a later run starts with what is left.

The figures: identities, not present, folders, current, candidates, then of the candidates hashed, changed, gone,
unreadable, unavailable and remaining; batches, bytes read, time. A first import still reads no ROM byte: the pass is
separate, and the import's pairing reads only new files of a stored hash's size.

## The pairing rules, as implemented

`IdentityPairing::pair()`, per library and system, called by the import with the vanished identities (present at the
last import, their file gone from a folder that was listed, gamelist entry or not; in the stored order) and the new files
(in entry order, which within a system is key order):

1. **The quick hash.** A vanished identity whose stored quick hash is current for its last file facts pairs with a new
   file of the same digest and size. Only the new files whose size equals one of those hashes' sizes are read (folders
   never; each once; the cancel flag is honoured between files: `Cancelled`). The identities and files of one digest
   form a group. **Ties**: the score of a pair is the number of trailing path elements their keys share (folded as the
   file systems compare names): 0 when the file names differ, 1 for the same file name, more when the folders above are
   the same too; a pair whose identity's best file and whose file's best identity are each other alone is paired
   (`QuickHash`), repeatedly. So the equal file name wins, then the longest common path suffix. **Identical copies**:
   what is left with every score equal and as many files as identities moved together and is paired in key order
   (`IdenticalCopies`). Anything else in the group is left to review. A stale hash says nothing of the content that
   vanished: such an identity goes to rule 2.
2. **Size, time and name**, for the vanished identities without a current hash: the new files of the same kind (file or
   folder), size, last write time and file name, not paired by rule 1 and whose hash, if read, matched no stored hash.
   An identity pairs only with a **unique** such file that no other such identity has (`SizeTimeName`); no ties are
   broken here (ADR-0008: "only with a unique new file").
3. **Review rows**, never a silent merge, for the vanished identities left with candidates: the files of their quick
   hash that ties could not tell apart; the new files of their hash's size that could not be read; the files of equal
   size, time and name that were not unique (two of them, or one another vanished identity wants too); the new files of
   the same file name whatever their content, size or time (a re-dumped or patched file moved: the user may know it).
   The candidates are in key order, the reason a sentence per kind. A vanished identity without any candidate stays
   vanished (deleted, or changed beyond recognition), with no review row.

The new files with a holder (`NewFile::heldBy`: a file moved back where a gamelist entry kept another identity) pair like
any other; the import makes the holder an alias of the moved identity (`MoveReason::MovedBack` on the alias; the move
keeps the reason it rests on). The hashes the pass read (`IdentityPairs::hashes`) are stored with the identities the files
end with (moved, held or new), sparing the quick-hash pass those files. The import validates what the pass returns (a pair,
a hash or a review naming what it was not given: `InvalidArgument`, nothing written).

Scale: the new files are indexed by hash, by (kind, size, time, name) and by name, so a mass move (a renamed folder of
thousands of files) pairs in about linear time; a group of identical files is compared within itself.

## History, aliases, review rows

- **History** (`rv_identity_history`, `IdentityEvent`, `ICatalogStore::history()`): written by the import after the
  identities: each move (`moved`, its reason, the old and new key and path, a sentence on what it rested on), each alias
  (`alias`, on the identity it stands for: the alias's GameId and last path, reason `moved-back` or `review-resolved`),
  each re-keyed identity (`rekeyed`: the path, the key computed now, the key rule and casing table before and after; the
  key before is not kept, the store computes keys again from their paths); by a resolution: the move, the alias and
  `review-resolved` (with the review's id). A casing table change re-keys every identity, one row each (informative:
  about 105,000 rows on the reference library; BACKLOG). The history of an identity that becomes an alias moves to the
  identity it stands for.
- **Aliases** (`rv_identity_aliases`, `StoredAlias`, `aliasesOf()`, `alias()`): written by `putAlias()` (11a), now for
  moves back and resolutions; an alias that stood for the holder is moved to the moved identity: aliases never chain.
- **Review rows** (`rv_identity_reviews`, `StoredReview`, `reviews()`, `review()`): written by the import (`putReview()`,
  their ids returned into `ChangeSet::reviews`), once per vanishing (the identity is no longer present at the next
  import, so it is never reviewed twice for one disappearance); resolved by `resolveReview()` (`paired` with the identity
  paired with, `kept-separate`), by the import when the identity's file comes back at its path or key (`returned`), and by
  `putAlias()` when the identity becomes an alias (`aliased`, with the identity it stands for).

## The re-pointing rule (`putAlias()`)

Before it deletes the holder's identity row, `putAlias()`:

1. resolves the holder's open review rows (`aliased`, `resolved_with` the moved identity);
2. moves **every state row that names the holder** to the moved identity, generically: the columns of the state tables
   (`rv_*`, never `idx_*`) that have a single-column foreign key into `rv_game_identities` (its `id`, or its primary key)
   are found in the schema (`pragma_foreign_key_list` over `sqlite_schema`, once per store: the schema does not change
   while the database is open), and for each column it runs `UPDATE OR IGNORE <table> SET <column> = moved WHERE <column>
   = holder`, then `DELETE FROM <table> WHERE <column> = holder`. Every row follows, but where a table holds one row per
   game (a unique or primary key on the column) and the moved identity has its own, the **moved identity's row is kept**
   and the holder's is dropped (its launch choice wins over the holder's). Step 12's tables (sessions, per-game launch
   choices, verifications) are covered without either step knowing the other's; so are the history, the review rows
   and the aliases;
3. removes the holder's index rows (11a), records the alias and deletes the identity row.

A composite foreign key into `rv_game_identities` (none exists) is not moved: the deletion would then fail with a FOREIGN
KEY error, and the import rolls back.

## Resolving a review

`resolveReview(store, library, {review, pairWith})` under the import lock, in one import transaction:

- **paired** (`identities resolve <review> --with <gameId>`): the chosen identity (same library and system, its file
  present, not the vanished one, not an alias) becomes an alias of the vanished one (`putAlias()`: its state rows move,
  its index rows go, its path is freed); the vanished identity takes its path, file facts and quick hash, present; its
  index row is written again under the vanished identity's GameId; a row still at the old path (a gamelist entry left
  there) gets a new identity as the import gives one (`assignGameIds()`: the tuple's counter); the disc groups that named
  either are written again; the history records the move (`review-resolved`), the alias and the resolution; the row is
  resolved `paired` with the chosen GameId. The next import changes nothing (tested).
- **kept separate** (`--keep-separate`): the row is resolved `kept-separate`, the history records it; nothing else
  changes.

Refused: no such row or identity (`NotFound`), resolved already, the vanished identity's file present again, an import
holding the lock (`Conflict`), a row of another library, an identity of another system, an alias, a file not present
(`InvalidArgument`).

## `retroverse-cli identities`

```text
retroverse-cli [global options] identities list --system <name> [--json] [--output <file>]
retroverse-cli [global options] identities show <gameId> [--json] [--output <file>]
retroverse-cli [global options] identities reviews [--system <name>] [--all] [--json] [--output <file>]
retroverse-cli [global options] identities resolve <review> (--with <gameId> | --keep-separate) [--json] [--output <file>]
retroverse-cli [global options] identities hash [--system <name>] [--limit <files>] [--json] [--output <file>]
```

- `list`: the identities of a system of the library library.json records: GameId, state (`present`; `missing`: its file
  gone since an import saw it; `unseen`: no import saw its file), the quick hash (`current`, `stale`, `none`, `folder`),
  size and last path; JSON `library`, `system`, `count`, `states`, `quickHashes`, `identities` (each with its key and key
  version, file facts, quick hash).
- `show`: one identity: state, last path and key, file facts, quick hash, the aliases standing for it, its history and its
  review rows; a GameId that is an alias prints what it stands for, then that identity. JSON `asked`, `alias` (null unless
  an alias was asked), `identity`, `aliases`, `history`, `reviews`.
- `reviews`: the open rows (`--all`: the resolved ones too), each with its vanished identity and path, why, and the
  candidates with the GameId each has now (what `--with` takes).
- `resolve`: as above; prints the move and the old path's new identity, or "kept separate".
- `hash`: the pass, its figures and issues; progress goes to the log every 2 s (stderr at `--log-level info` or below, and
  the log file).
- `list`, `show` and `reviews` open the catalog without writing into it (no WAL switch, never made); `resolve` and `hash`
  write state rows under the import lock.
- Exit codes: 0; 1 `show`: no such identity or alias; `resolve`: no such review row, resolved already, its file present
  again; `hash`: files that could not be read or reached; 2 usage errors, no library.json, a library.json naming another
  library than the catalog's, a system without identities, an identity that cannot be paired with; 3 no catalog, the
  catalog cannot be read or written, another import runs (in this or another process), the results cannot be written.
- `import` now pairs (`IdentityPairing`): its text lists each move (`moved <id> <system>: <from> -> <to> (<reason>)`, the
  alias when there is one) and each review row, the totals line counts `to review`; its JSON's `reviews` carry `review`
  (the row's id) and `game` (11a's `id` was the game), `statistics` gain `historyWritten`, `reviewsWritten`,
  `quickHashesRead`, `quickHashesUnreadable`, `quickHashBytesRead`.

## Decisions taken within the step

1. **The stored identity carries its quick hash** (`StoredIdentity::quickHash`, read by `identities()`, `identity()` and
   written by `putIdentity()`): the import compares and writes identities whole, the pairing reads the vanished
   identities' hashes from `PairingRequest::vanished` and needs no store read of its own (11a's alternative). The quick-hash
   pass writes the three columns alone (`putQuickHash()`), so that `last_seen` keeps its meaning.
2. **Only a current hash pairs** (its size and time those of the identity's last facts): a stale one belonged to content
   that changed before it vanished; such an identity goes to the size-time-name rule.
3. **The pass skips files whose facts are no longer the last import's** (`changed`) rather than hashing them: the hash
   would be stale at once. The next import records the facts, the pass after hashes the file.
4. **The pass holds the import lock for its whole run** (an import, a resolution or another pass is refused meanwhile);
   batches are transactions of their own, so play sessions wait only for a batch. Long runs are bounded with `--limit`
   or cancelled; on the app's catalog worker the pass and the imports take turns on one thread (step 13).
5. **Ties apply to the hash rule only**; the size-time-name rule pairs a unique file or none (ADR-0008's "only with a
   unique new file").
6. **Review candidates include the new files of the same file name** whatever their content: a file moved and re-dumped
   or patched is offered to the user rather than lost; a vanished identity without any candidate gets no review row.
7. **The move keeps the reason it rests on; the alias of a file moved back says `moved-back`** (11a's import gave the
   alias the pair's reason).
8. **The hashes the pairing read are kept** (`IdentityPairs::hashes`) on the identities the files end with.
9. **Generic re-pointing in `putAlias()`** (above), with UPDATE OR IGNORE then DELETE: one row per game keeps the moved
   identity's.
10. **Review rows resolved without the user** when the identity's file comes back (`returned`) or the identity becomes an
    alias (`aliased`), so that `reviews` lists only what still waits.
11. **A resolution applies the move as the import would** in one transaction, the index rows of both identities written
    again from what the index held (the row at the old path under a new GameId), so that the next import has nothing to
    do.
12. **`core::SharedFileReader`** (core's file I/O, beside `readFileShared()`): positioned reads of a file opened with full
    sharing, its facts from the handle.
13. **Schema v1 additions** (open until the M1 gate): `rv_identity_history.alias`, `.review` (a foreign key into
    `rv_identity_reviews`), a CHECK on `event`; `rv_identity_reviews.resolved_with`, a CHECK pairing `resolved_at` and
    `resolution`. A development database made by an earlier build is deleted, never migrated.

## Tests

(Filled with the final figures below.)

## Real library

(Filled after the run.)

## Notes for the steps that build on it

- **Step 13 (the app):** construct one `catalog::IdentityPairing` with the catalog worker's `ImportService` and keep it;
  `ChangeSet::moved` and `ChangeSet::reviews` (with their ids) say what moved and what waits. Run
  `catalog::runQuickHashPass()` on the catalog worker between imports (it holds the import lock for its run: an import
  started meanwhile from another process is refused), with the quit flag as `cancel` (what was read stays committed) and
  a `limit` per idle slice if the worker must stay responsive; its progress callback runs on the worker. Review rows are
  shown and resolved through `ICatalogStore::reviews()` and `catalog::resolveReview()` (a later UI; the CLI has them now).
  After a resolution, run the queries again (`data_version` changes at the commit) and restore the selection by GameId:
  the chosen identity's GameId is an alias from then on (`ICatalogStore::alias()` names the identity it stands for).
- **Step 12 (sessions, choices):** nothing to do for aliases: `putAlias()` finds step 12's tables by their foreign keys
  into `rv_game_identities` and moves their rows (a table unique per game keeps the moved identity's row). Keep the
  foreign keys single-column, into `rv_game_identities (id)`.

## For the merge

- **CLAUDE.md**, the retroverse-cli section: `identities list | show | reviews | resolve | hash` (options, exit codes);
  `import` pairs moved files and prints moves and review rows. "Catalog database": `StoredIdentity::quickHash` (read and
  written with the identity), `putQuickHash()`, the history, aliases and review rows, `putAlias()`'s generic re-pointing
  of state rows (a later step's state table needs a single-column foreign key into `rv_game_identities (id)` to follow
  aliases); core helpers: `core::SharedFileReader` in `file_io.hpp`. Oracles: `scripts/quick_hash_oracle.py --check
  tests/unit/catalog/test_fingerprint.cpp`.
- **docs/architecture.md**: `rv_catalog`'s reconciliation (`IdentityPairing`, the quick-hash pass, review resolution) and
  fingerprints; `tools/cli`'s `identities`.
- **docs/i2/step-4.md**'s table: `rv_identity_history` (alias, review, the CHECK), `rv_identity_reviews` (resolved_with),
  the quick hash read with the identity.
- **docs/i2/step-11a.md**: the seam is filled (`IdentityPairs::hashes`; the alias of a moved-back file says `moved-back`;
  `ChangeSet::reviews` carry their ids; the CLI's JSON `reviews[].review` and `.game`).
- **docs/roadmap.md**: step 11b done, with the figures below.
- **ADR-0008**, suggested amendments: §4 "the quick hash: SHA-256 of a file's whole content up to 128 KiB, else of its
  first and last 64 KiB (rule 1); only a current hash (the size and time it was taken at are the file's last facts)
  pairs"; §4 "ties: the number of trailing path elements shared; identical copies pair in key order when the counts are
  equal; the size-time-name rule breaks no ties"; §4 "review candidates include the new files of the same file name";
  §5 "an identity that becomes an alias moves every state row naming it to the identity it stands for (a table holding one
  row per game keeps the latter's)".
- **BACKLOG** candidates: the critique's "candidates are every missing identity with a stored hash" (a file deleted at one
  import and back elsewhere at a later one keeps no identity under ADR-0008's rule, which pairs only identities that
  vanished at this import); a compact history for re-keying (one row per identity on a casing table change); review rows
  in the product UI (I7); a quick-hash pass scheduled by the app (idle time, I7).
