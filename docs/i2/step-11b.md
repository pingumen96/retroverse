# I2 step 11b: fingerprints and reconciliation

Roadmap row: I2 step 11b (wave 3, after 11a, parallel with 12). Decisions:
[ADR-0008](../decisions/ADR-0008-esde-catalog.md) §4 (fingerprints, reconciliation, the moved-back rule, review rows),
§5 (state tables: history, aliases, fingerprints, review rows). The import of step 11a hands its pairing seam
(`IIdentityPairing`) the identities whose file vanished and the new files; this step fills it: files moved or renamed
keep their GameId, their history and their state rows, by their quick hash or by a unique size, time and name, and what
no rule pairs safely becomes a review row for the user. The quick hash is computed by a progressive pass of its own.
`retroverse-cli identities` shows identities, their history and the review rows, resolves reviews and runs the pass.
This page is folded into the shared documentation at the wave merge.

## What was built

| File | What it does |
|---|---|
| `rv/catalog/fingerprint.hpp`, `src/fingerprint.cpp` (`rv_catalog`, step 1's stubs) | `QuickHash` (digest, size, time), `isCurrent()`, `quickHashDigest()` (the definition, for golden vectors), `quickHashBytes()`, `readQuickHash()`, `readQuickHashBelow()`, `QuickHashReader`, `kQuickHashRule` |
| `rv/catalog/reconciliation.hpp`, `src/reconciliation.cpp` (`rv_catalog`, step 1's stubs) | `IdentityPairing` (the import's `IIdentityPairing`, with `Statistics`), `currentQuickHash()`, `runQuickHashPass()` (`QuickHashPassOptions`, `QuickHashProgress`, `QuickHashPassResult`), `resolveReview()` (`ReviewDecision`, `ReviewResolutionResult`); issue codes `quick-hash-unreadable`, `quick-hash-unavailable` |
| `rv/catalog/catalog_store.hpp` (extended) | `StoredIdentity::quickHash`; `IdentityEventKind`, `IdentityEvent`, `StoredAlias`, `ReviewResolution`, `StoredReview`; `ICatalogStore::history`, `aliasesOf`, `alias`, `reviews`, `review`, `reviewsOf`; `IImportTransaction::putQuickHash`, `putHistory`, `putReview`, `resolveReview`, `resolveReviewsOf`; `putAlias()`'s contract (state rows moved) |
| `rv/catalog/import_service.hpp`, `src/import_service.cpp` (11a's, extended) | `NewFileHash`, `IdentityPairs::hashes`; `ImportStatistics::historyWritten`, `reviewsWritten`; the import writes the history (moves, aliases, re-keyed identities), the review rows (their ids into `ChangeSet::reviews`), resolves the reviews of identities whose file came back, stores the quick hashes the pairing read; the holder of a file moved back becomes an alias with `MoveReason::MovedBack` |
| `rv/catalog_db/catalog_store.hpp`, `src/catalog_store.cpp` | the store's side: identities read and written with their quick hash, 12 new statements, the history, review and alias reads, `putAlias()` resolving the holder's reviews and moving every state row that names it (found in the schema) |
| `src/catalog_db/src/schema_identity.cpp` | `rv_identity_history` gains `alias`, `review` and a CHECK on `event`; `rv_identity_reviews` gains `resolved_with` and a CHECK pairing `resolved_at` with `resolution` (schema v1 is open) |
| `rv/core/file_io.hpp`, `src/file_io.cpp` | `core::SharedFileReader`: a file opened for reading only with full sharing, read at offsets, its size and time from the handle; `core::listedFileFacts()`: a file's size and time as a listing gives them |
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
walk listed with `core::extendedLengthPathBelow()`, trailing dots and spaces kept), reads at most 128 KiB (two
positioned reads, or one), checks that the handle's size and time are the same after the reads as before (a file written
meanwhile is an `IoError`, never a hash half old, half new), and, when the handle's facts differ from the listing's,
lists the file again after the reads: a listing that changed is an `IoError` too (a file replaced between the listing
and the open, by an atomic replace or an extraction, never a hash of the new content stored with the old file's facts),
a listing that stayed as it was is a directory entry lagging behind (below); it writes nothing.
`detail::readQuickHashObserved()` calls an observer after the listing, the open and the reads, so that tests change the
file at those moments. The hash is stored with the size and time **a listing gives** (`core::listedFileFacts()`: the file's directory entry, or for a symbolic link the file it
leads to, exactly as `walkTree()` and so the import see it; on NTFS a directory entry's copy can lag behind the file
record the handle reads, for a hard link written through another name), in `QuickHash::size`, `lastWriteTime` and the
schema's `quick_hash_size`, `quick_hash_time`; it is **current** while they equal the identity's file facts
(`isCurrent()`, `currentQuickHash()`), **stale** once an import records other facts. Golden vectors:
`scripts/quick_hash_oracle.py` generates 11 contents (0, 1, 3, 64 KiB - 1, 64 KiB, 64 KiB + 1, 128 KiB - 1, 128 KiB, 128
KiB + 1, 192 KiB, 1 MiB + 7 bytes; a linear congruential generator, no short period) and hashes them with hashlib;
`tests/unit/catalog/test_fingerprint.cpp` holds the table and generates the same contents, in memory and as files;
`python -I scripts/quick_hash_oracle.py --check tests/unit/catalog/test_fingerprint.cpp` passes.

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
4. Commits the hashes in batches (1,000 files or 10 s, whichever first), each in an import transaction of its own
   (`IImportTransaction::putQuickHash()`, which writes the three hash columns alone: `last_seen` does not move), so that
   other writers (play sessions) wait only for a batch. A batch's commit is **light**: catalog_db's store commits a
   transaction that wrote only reconciliation's state rows (quick hashes, history, review rows) without recording the
   key versions in `rv_meta` (which scans every identity: 0.1 s on the reference catalog) and without the TRUNCATE
   checkpoint, which SQLite's automatic checkpoint (1,000 pages) and the next import's commit do. Each commit changes
   `data_version`, which readers refreshing their queries see: 10 s between batches keeps that rare. Cancelled
   (`options.cancel`), it commits what it read and ends (`cancelled`); `--limit` bounds the files read. **Resumable**: a
   later run starts with what is left.

The figures: identities, not present, folders, current, candidates, then of the candidates hashed, changed, gone,
unreadable, unavailable and remaining; batches, bytes read, time. A first import still reads no ROM byte: the pass is
separate, and the import's pairing reads only new files of a stored hash's size.

## The pairing rules, as implemented

`IdentityPairing::pair()`, per library and system, called by the import with the vanished identities (present at the
last import, their file gone from a folder that was listed, gamelist entry or not; in the stored order) and the new
files (in entry order, which within a system is key order):

1. **The quick hash.** A vanished identity whose stored quick hash is current for its last file facts pairs with a new
   file of the same digest and size. Only the new files whose size equals one of those hashes' sizes are read (folders
   never; each once; the cancel flag is honoured between files: `Cancelled`). The identities and files of one digest
   form a group. **Ties**: the score of a pair is the number of trailing path elements their keys share (folded as the
   file systems compare names): 0 when the file names differ, 1 for the same file name, more when the folders above are
   the same too; a pair whose identity's best file and whose file's best identity are each other alone is paired
   (`QuickHash`), repeatedly, **when the pair is confirmed**: the same file name (a score of 1 or more) or the same last
   write time. So the equal file name wins, then the longest common path suffix. **Identical copies**: what is left
   with every score equal and as many files as identities moved together and is paired in key order
   (`IdenticalCopies`), when each pair in key order is confirmed. Anything else in the group is left to review. A stale
   hash says nothing of the content that vanished: such an identity goes to rule 2. **Why the confirmation**: the quick
   hash reads a file's two ends only, and on the reference library files over 128 KiB that share them are often
   different ROMs: the review found 5,140 groups of one quick hash among 23,164 hashed identities (gb, gba, nds, snes),
   and files of different full contents in 13 of 120 sampled groups (gb 6 of 40, gba 2, snes 5: `[a]` dumps, hacks,
   translations, debug builds); one gba hash covers 16 distinct contents (Final Fantasy Tactics Advance (USA), its
   Virtual Console release and 14 hacks; checked again for this page with full SHA-256). A rename or a move keeps the
   name or the time; another hack of the same base usually has neither. Equal times alone still leave a risk (five of
   those hacks share their modification second), hence the ADR amendment proposed below.
2. **Size, time and name**, for the vanished identities without a current hash: the new files of the same kind (file or
   folder), size, last write time and file name, not paired by rule 1 and whose hash, if read, matched no stored hash.
   An identity pairs only with a **unique** such file that no other such identity has (`SizeTimeName`); no ties are
   broken here (ADR-0008: "only with a unique new file").
3. **Review rows**, never a silent merge, for the vanished identities left with candidates: the files of their quick
   hash that ties could not tell apart; the new files of their hash's size that could not be read; the files of equal
   size, time and name that were not unique (two of them, or one another vanished identity wants too); the new files of
   the same file name whatever their content, size or time (a re-dumped or patched file moved: the user may know it).
   The candidates are in key order (at most 100: `kMaxReviewCandidates`), the reason a sentence per kind. A vanished
   identity without any candidate stays vanished (deleted, or changed beyond recognition), with no review row.

**Files with a holder** (`NewFile::heldBy`: the path or key of the file has an identity that was not present at the
last import). The import also gives the pass the holder's last file facts (`NewFile::holderFile`):

- **a holder never seen with a file** (no file facts: the identity a gamelist entry left at an old path got, SC-07):
  the file pairs like any new one, and the import makes the holder an alias of the moved identity (the moved-back rule;
  `MoveReason::MovedBack` on the alias, the move keeps the reason it rests on);
- **a holder that had a file of its own** (it was seen present once): the file is the holder's, whether its own file
  came back unchanged (SC-05: it gets its GameId back) or the user put another file where it was. No rule gives it to a
  vanished identity, whatever its hash or its size, time and name say; a vanished identity left unpaired whose quick
  hash (or, without one, size, time and name) the file has lists it among its review candidates ("... but is at the
  path of another identity, which keeps it"), and resolving that review with the holder makes the holder the alias, as
  the user decides. The import refuses a pairing that gives such a file to a vanished identity (`InvalidArgument`,
  nothing written), so that another pairing pass cannot merge a returning game either.

The hashes the pass read (`IdentityPairs::hashes`) are stored with the identities the files end with (moved, held or
new), sparing the quick-hash pass those files. The import validates what the pass returns (a pair, a hash or a review
naming what it was not given, a held file given away: `InvalidArgument`, nothing written).

Scale: the new files are indexed by hash, by (kind, size, time, name) and by name, and the import finds what the pass
returns by GameId and key, so a mass move (a renamed folder of thousands of files) pairs in about n log n; a group of
one quick hash is compared within itself, its scores computed once (a group of more than 4,194,304 pairs goes to review
whole).

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
- **Review rows** (`rv_identity_reviews`, `StoredReview`, `reviews()`, `review()`): written by the import
  (`putReview()`, their ids returned into `ChangeSet::reviews`), once per vanishing (the identity is no longer present
  at the next import, so it is never reviewed twice for one disappearance); resolved by `resolveReview()` (`paired` with
  the identity paired with, `kept-separate`), by the import when the identity's file comes back at its path or key
  (`returned`), and by `putAlias()` when the identity becomes an alias (`aliased`, with the identity it stands for).

## The re-pointing rule (`putAlias()`)

Before it deletes the holder's identity row, `putAlias()`:

1. resolves the holder's open review rows (`aliased`, `resolved_with` the moved identity);
2. moves **every state row that names the holder** to the moved identity, generically: the columns of the state tables
   (`rv_*`, never `idx_*`) that have a single-column foreign key into `rv_game_identities` (its `id`, or its primary
   key) are found in the schema (`pragma_foreign_key_list` over `sqlite_schema`, once per store: the schema does not
   change while the database is open), and for each column it runs `UPDATE OR IGNORE <table> SET <column> = moved WHERE
   <column> = holder`, then `DELETE FROM <table> WHERE <column> = holder`. Every row follows, but where a table holds
   one row per game (a unique or primary key on the column) and the moved identity has its own, the **moved identity's
   row is kept** and the holder's is dropped (its launch choice wins over the holder's). Step 12's tables (sessions,
   per-game launch choices, verifications) are covered without either step knowing the other's; so are the history, the
   review rows and the aliases;
3. removes the holder's index rows (11a), records the alias and deletes the identity row.

A composite foreign key into `rv_game_identities` (none exists) is not moved: the deletion would then fail with a
FOREIGN KEY error, and the import rolls back.

## Resolving a review

`resolveReview(store, library, {review, pairWith})` under the import lock, in one import transaction:

- **paired** (`identities resolve <review> --with <gameId>`): the chosen identity (same library and system, its file
  present, not the vanished one, not an alias, **at one of the review's candidate paths**: its relative path or key is
  a candidate's, unless `ReviewDecision::force`, `--force`, for a candidate beyond the 100 listed or one that moved since)
  becomes an alias of the vanished one (`putAlias()`: its state rows move,
  its index rows go, its path is freed); the vanished identity takes its path, file facts and quick hash, present; its
  index row is written again under the vanished identity's GameId; a row still at the old path (a gamelist entry left
  there) gets a new identity as the import gives one (`assignGameIds()`: the tuple's counter); the disc groups that
  named either are written again; the history records the move (`review-resolved`), the alias and the resolution; the
  row is resolved `paired` with the chosen GameId. The next import changes nothing (tested).
- **kept separate** (`--keep-separate`): the row is resolved `kept-separate`, the history records it; nothing else
  changes.

Refused: no such row or identity (`NotFound`), resolved already, the vanished identity's file present again, an import
holding the lock (`Conflict`), a row of another library, an identity of another system, an alias, a file not present,
an identity that is none of the candidates without `force` (`InvalidArgument`): a mistyped GameId would merge two games
for good.

## `retroverse-cli identities`

```text
retroverse-cli [global options] identities list --system <name> [--json] [--output <file>]
retroverse-cli [global options] identities show <gameId> [--json] [--output <file>]
retroverse-cli [global options] identities reviews [--system <name>] [--all] [--json] [--output <file>]
retroverse-cli [global options] identities resolve <review> (--with <gameId> [--force] | --keep-separate) [--json] [--output <file>]
retroverse-cli [global options] identities hash [--system <name>] [--limit <files>] [--json] [--output <file>]
```

- `list`: the identities of a system of the library library.json records: GameId, state (`present`; `missing`: its file
  gone since an import saw it; `unseen`: no import saw its file), the quick hash (`current`, `stale`, `none`, `folder`),
  size and last path; JSON `library`, `system`, `count`, `states`, `quickHashes`, `identities` (each with its key and
  key version, file facts, quick hash).
- `show`: one identity: state, last path and key, file facts, quick hash, the aliases standing for it, its history and
  its review rows; a GameId that is an alias prints what it stands for, then that identity. JSON `asked`, `alias` (null
  unless an alias was asked), `identity`, `aliases`, `history`, `reviews`.
- `reviews`: the open rows (`--all`: the resolved ones too), each with its vanished identity and path, why, and the
  candidates with the GameId each has now (what `--with` takes).
- `resolve`: as above (`--with` takes a candidate's GameId, `--force` any identity of the system; `--force` without
  `--with` is a usage error); prints the move and the old path's new identity (and, forced with an identity that was
  none of the candidates, a line saying so), or "kept separate".
- `hash`: the pass, its figures and issues; a progress line (`Quick hashes: <done> of <total> file(s) looked at, <n>
  hashed, <MiB> MiB read`) every 2 s on stderr (stdout keeps the results; not at `--log-level warn` or `error`; at
  `debug` and `trace` the log's own line shows it) and in the log file.
- `list`, `show` and `reviews` open the catalog without writing into it (no WAL switch, never made); `resolve` and
  `hash` write state rows under the import lock.
- Exit codes: 0; 1 `show`: no such identity or alias; `resolve`: no such review row, resolved already, its file present
  again; `hash`: files that could not be read or reached; 2 usage errors, no library.json, a library.json naming another
  library than the catalog's, a system without identities, an identity that cannot be paired with (none of the
  review's candidates without `--force` included); 3 no catalog, the
  catalog cannot be read or written, another import runs (in this or another process), the results cannot be written.
- `import` now pairs (`IdentityPairing`): its text lists each move (`moved <id> <system>: <from> -> <to> (<reason>)`,
  the alias when there is one) and each review row, the totals line counts `to review`; its JSON's `reviews` carry
  `review` (the row's id) and `game` (11a's `id` was the game), `statistics` gain `historyWritten`, `reviewsWritten`,
  `quickHashesRead`, `quickHashesUnreadable`, `quickHashBytesRead`.

## Decisions taken within the step

1. **The stored identity carries its quick hash** (`StoredIdentity::quickHash`, read by `identities()`, `identity()` and
   written by `putIdentity()`): the import compares and writes identities whole, the pairing reads the vanished
   identities' hashes from `PairingRequest::vanished` and needs no store read of its own (11a's alternative). The
   quick-hash pass writes the three columns alone (`putQuickHash()`), so that `last_seen` keeps its meaning.
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
10. **Review rows resolved without the user** when the identity's file comes back (`returned`) or the identity becomes
    an alias (`aliased`), so that `reviews` lists only what still waits.
11. **A resolution applies the move as the import would** in one transaction, the index rows of both identities written
    again from what the index held (the row at the old path under a new GameId), so that the next import has nothing to
    do.
12. **`core::SharedFileReader`** (core's file I/O, beside `readFileShared()`): positioned reads of a file opened with
    full sharing, its facts from the handle.
13. **Schema v1 additions** (open until the M1 gate): `rv_identity_history.alias`, `.review` (a foreign key into
    `rv_identity_reviews`), a CHECK on `event`; `rv_identity_reviews.resolved_with`, a CHECK pairing `resolved_at` and
    `resolution`. A development database made by an earlier build is deleted, never migrated.
14. **A hash is stored with the listed facts** (`core::listedFileFacts()`), not the handle's: what the import records
    and `isCurrent()` compares come from one source, so a directory entry lagging behind its file record (NTFS hard
    links) never keeps a hash stale for good.
15. **Bounds for the pathological**: a review row lists at most 100 candidates (`kMaxReviewCandidates`, its reason says
    how many there were); a group of one quick hash with more than 4,194,304 (identity, file) pairs is not compared and
    goes to review whole.
16. **`identities show` needs no library.json** (a GameId names its library), as `retroverse-cli show`; the other
    subcommands work on the library library.json records and refuse another one.
17. **A file whose holder had a file of its own is the holder's** (the second review): only a holder never seen with a
    file is aliased by the moved-back rule; the others keep their file, and the vanished identity it could be gets a
    review row. The import enforces it at the seam (`NewFile::holderFile`, `InvalidArgument`).
18. **A quick-hash pair needs the same file name or the same time** (the second review's real-data finding): the quick
    hash alone is not enough evidence on a library of hacks and dumps; unconfirmed matches are review rows. This
    narrows ADR-0008 §4's "pairs with a new file whose quick hash equals its stored one": an amendment is proposed.
19. **A resolution pairs only with a candidate** unless forced (`--force`): the merge cannot be undone.
20. **Light commits for reconciliation's state rows** and batches of 1,000 files or 10 s (the second review): a batch
    no longer scans the identities for `rv_meta` nor checkpoints; the WAL is SQLite's automatic checkpoint's and the
    next import's.
21. **The listing is checked again after the reads when the handle disagrees with it**: a hash is stored only with
    the facts of the file it was computed from. When they agree, the handle read the file listed and a second listing
    is spared (on the external exFAT drive the warm gb pass took 12.77 s listing every file twice, 8.49 s without). The
    listed size is not required to equal the handle's (a directory entry lagging behind on NTFS would then never be
    hashed: decision 14).

## The first review's findings

One review of the committed step (ten findings). Fixed:

- **The hash's facts came from the open handle, the import's from the listing** (on NTFS a directory entry may lag
  behind the file record, so a hard-linked file could be `changed` at every pass and a hash stale at once):
  `core::listedFileFacts()`, decision 14; tested against `listTree()`'s facts.
- **Linear searches when the import applies the pairing** (per pair, per hash, per review row over every new file or
  vanished identity: quadratic on a mass move): maps built once (11a's per-pair search included).
- **The ties recomputed every score for every candidate** (cubic on a big group of identical files): the scores are a
  matrix computed once per group, each round finds the mutual best pairs in one sweep; the bound of decision 15.
- **The re-keyed event named the path before a case rename of the same import**: it names the identity's path as
  written (tested: re-keyed and renamed in case at once).
- **Bytes read for files found changed were not counted**: counted.
- **`identities show` read every review row of the library**: `ICatalogStore::reviewsOf(GameId)`, through the index on
  the game.
- **No cancel check while review rows are written**: added.

Kept, with the reason:

- **Identical copies by quick hash need not be byte-identical** (two images over 128 KiB of one size sharing their first
  and last 64 KiB): the quick hash is ADR-0008's evidence for every hash pairing, not only copies; the limit is stated
  in `reconciliation.hpp` and here.
- **`identities show` does not check library.json**: decision 16.
- **The pass holds the import lock for its whole run**: `lockImport()` never waits, so a lock taken per batch and taken
  again at once would still refuse almost every import that tries meanwhile, and the pass would have to re-read each
  identity before writing its hash; on the app's catalog worker the pass and the imports take turns on one thread, and
  the CLI bounds a run with `--limit` (decision 4).

## The second review's findings

Two reviewers, six findings on the committed step, each verified (four reproduced by new tests that failed before the
fix, the real-data one also on the library itself):

- **Fixed: a file back at its own path was given to another vanished identity** (high). `pair()` ignored the holder:
  with identical copies A and B, A removed for one import, then restored while B was deleted, B's identity took A's file
  (by quick hash, or by size, time and name without one) and A's identity became its alias, its state rows and history
  merged for good. Now decision 17; tested with and without stored hashes, and with a holder that had another file.
- **Fixed: an equal quick hash paired different games** (medium, real data). Decision 18; tested (another name and time:
  a review row; a rename keeping the time, a move keeping the name: paired; identical copies of other names and times:
  reviewed).
- **Fixed: a resolution accepted any identity of the system** (medium). Decision 19; tested in the store and the CLI
  (exit 2, the row still open; forced: paired).
- **Fixed: a file replaced between the listing and the open was hashed under the old listing's facts** (low). Decision
  21; tested through `detail::readQuickHashObserved()` (replaced before the open, written before the open, before and
  after the reads).
- **Fixed: no progress on the terminal** (low): the progress line goes to stderr.
- **Fixed: every batch committed like an import** (low): decision 20.
- **Not taken, with the reason**: requiring the listed size to equal the handle's size (F2's optional part): on NTFS a
  hard-linked file's directory entry can lag behind its file record for good, and the pass would then report it
  unreadable at every run instead of hashing it (decision 14); the digest is the content's either way, and both sides
  of a pairing label it with the listed size.

## Tests

`./scripts/test.ps1` passes every unit test in Debug and in Release (1,216), both built without a warning; the step's
(`rv_tests "[fingerprint],[reconciliation],[identities]"`: 26 new test cases, one more in `test_import_service.cpp`,
and the hidden real-library one):

- `tests/unit/catalog/test_fingerprint.cpp`: the 11 golden vectors equal the oracle's, on generated contents and on
  files (with the size and time a listing gives: those of `listTree()`); beyond 128 KiB only the two ends count (another
  middle, the same hash; another byte at either end, another one); names ending with a dot or a space and a path past
  MAX_PATH read through `\\?\` (the sibling a plain path would reach left alone); nothing written (content and time as
  they were); a file another program holds open for writing is read; a missing file (`NotFound`), a folder and a
  relative path leaving the folder (`InvalidArgument`), a file whose data is denied are errors; a file replaced or
  written while it is read (after the listing, after the open, after the reads) is an `IoError`, never a hash; a hash
  current or stale for file facts.
- `tests/unit/catalog_db/test_reconciliation.cpp` (scripted snapshots on the SQLite store, a scripted file reader that
  says what each file holds, which files are denied and which were read): a renamed file paired by its quick hash, only
  the new file of the stored hash's size read, the hash it read stored with the moved identity, the move in the history,
  a second import that writes and reads nothing; **SC-07** (a file renamed, one moved to a sub-folder, one moved with
  its gamelist entry left at the old path: GameIds, history and the rows of a state table kept, the old path's entry a
  new GameId, the counter's); **without a stored hash** a unique file of equal size, time and name pairs (no file read),
  two such files make a review row with both candidates and nothing merges (the identity vanished, the files new
  identities, the row stored as the change set says, not repeated by the next import), one file two vanished identities
  could be makes two rows, the same name at another time a row, 102 files of its name a row of the first 100 in key
  order (its reason says 102), nothing of its name no row; **ties** (the longest common path suffix, then the equal file
  name, among copies of one hash); **identical copies** paired in key order, more copies than identities reviewed, an
  unreadable copy reported and offered for review, copies of other names and another time reviewed; **an equal quick
  hash confirmed** (another name and another time: a review row, nothing merged; a rename keeping the time and a move
  keeping the name: paired); **a file back at its own path** while an identical copy vanishes, with and without stored
  hashes (its own GameId, no alias, history and state rows untouched, the copy's identity a review row offering it),
  and **a vanished identity's file at the path of an identity that had another file** (a review row, nothing merged;
  resolved with the holder, the holder the alias); **a review resolved by pairing** (the move, the alias, the old path's
  new identity as the import gives it, the history's three events, resolved once only) and **kept separate** (only the
  row and the history change), each followed by an import that changes nothing; the refusals (no such row or identity,
  itself, another system, a file not present, an alias, resolved already, another holder's import lock, an identity
  that is none of the candidates; forced, it pairs); **a file moved
  back**: the holder an alias, its sessions moved, a table unique per game keeping the moved identity's choice, the
  alias that stood for the holder standing for the moved identity, the holder's history moved, its open review row
  resolved `aliased` and moved, no foreign key broken, the next import steady; **re-keyed identities** in the history
  (one renamed in case in the same import: the event names its path now), a file back at its path resolving its review
  (`returned`); **the pass** committing in batches, cancelled after 5 files (5 committed in 2 batches, 9 left), resumed
  with a limit, then to the end, in the stored order, every hash the file's; files changed or gone since the import
  skipped, a denied file an issue, a hash gone stale after a change and the stale identity then paired by size, time and
  name; **no overlap**: the pass refused while another holder imports, and an import of another holder refused while the
  pass reads.
- `tests/unit/cli/test_cli_identities.cpp` (the standard fixture through the dispatcher): usage errors and exit codes
  (no catalog: exit 3 and nothing made; a system without identities: 2; an unknown GameId: 1); the pass's figures (22
  identities: 18 hashed, 3 folders, 1 not present; again: nothing left); **SC-07 on disk** (Tiny Tennis renamed, its
  gamelist entry left behind, LOUD CART moved to a new folder: both paired by quick hash with 2 files read, the old
  path's entry a new GameId, `list` and `show` with the history as text and JSON, the next import steady); **review rows
  on disk** (Collection A's Pocket Puzzle copied into two folders and deleted: a review with both candidates; resolved
  with `--with` (the alias shown by `show`, the next import steady, resolving again exit 1, an unknown row exit 1,
  another system's identity exit 2, an identity of the system that is none of the candidates exit 2 with the row still
  open; forced with it, paired with the warning line) or `--keep-separate`; `--force` without `--with` exit 2); a
  denied ROM (exit 1, the issue, 17 hashed; the next pass
  hashes it); **the import lock across processes** (the pass and a resolution exit 3 while another holder imports, also
  as a second `retroverse-cli` process; an import of a second process exits 3 while this process's pass reads).
- `scripts/quick_hash_oracle.py --check tests/unit/catalog/test_fingerprint.cpp`: 11 of 11 found.
- `tests/unit/catalog_db/test_import_service.cpp`: the seam gives the pass a held file's holder file facts, and refuses
  a pairing that gives a vanished identity the file of a holder that had a file of its own (nothing written; without
  that pairing the file returns to its identity).
- Followed: `test_catalog_store.cpp` (the quick hash written alone, read with the identity, kept by a write that carries
  it, cleared by one that does not), `test_migrator.cpp` (an orphan history row with a valid event), the port stand-ins
  of `test_domain.cpp` and `test_import_service.cpp`.

## Real library

`./scripts/real-library.ps1 -EsdeHome <ES-DE home> -EsdeInstall <ES-DE install> -RomRoot <ROM root>
-MediaRoot <media root>` (Release, ES-DE closed), the default filter: 10 test cases, 8
passed (the import's and this step's among them), the two launch tests skipped without `-RetroArchExe`; **0 differences
among 542,082 entries**. Then `-Filter '[real-library][import]'` again on a quiet machine: 0 differences.

The pass on gb (`test_reconciliation_real.cpp`: a partial import of gb into a new database under the build tree, then
the pass): 8,621 identities (1 not present, 9 folders), **8,611 files hashed, 0 changed, 0 gone, 0 unreadable, 980.9 MiB
read in 268.76 s** (119 batches): 32 files/s, 3.6 MiB/s, the files read cold from the external exFAT drive (an open and
two positioned reads each; gb's files average 114 KiB read). **A second pass hashed 0 files in 0.08 s**; the import
after it paired nothing, wrote nothing and read no ROM byte. **The whole library, estimated** from the sample: 103,937
present files, 9,857.9 MiB to read: about **45 minutes** at gb's rate per byte (2,701 s), 54 minutes at its rate per
file (3,244 s), cold; informative (the gate measures on a quiet machine). Run again after the review's fixes, the files
still in the system's cache: 8,611 hashed, 0 changed, **10.60 s** (812 files/s, 92.5 MiB/s; 34 batches), the second pass
0.02 s, the whole library estimated at 107 to 128 s warm: the cold drive's seeks are the cost, not the hashing. This is
why the pass is progressive and resumable rather than part of the import: an import reads no ROM byte, and only the new
files of a vanished hash's size when files move. 0 differences again.

After the second review's fixes (light commits, batches of 1,000 files or 10 s, the listing checked again only when the
handle disagrees with it), Release, the default filter: 8 passed, the two launch tests skipped, **0 differences among
542,082 entries**; then `-Filter '[real-library][reconciliation]'`: the gb pass, warm, **8,611 files hashed in 8.49 s**
(9 batches; 1,014 files/s, 115.5 MiB/s), the second pass 0 files in 0.02 s, the import after it pairing nothing; 0
differences. The whole library estimated at 85 to 102 s warm. (An intermediate build that listed every file a second
time took 12.77 s, warm too: decision 21.)

The import with this step's changes (identities written with their quick hash, the pairing pass, the history), on a
quiet machine: the first import 13.06 s (writes 9.36 s), the second 7.17 s (writes 0.15 s), `db rebuild-index` 18.94 s,
the ROM root unplugged 11.86 s and back 11.81 s: step 11a's figures within the variance of the machine (a first run
taken while a Debug build ran beside it took 40.71 s, the writes 30.12 s: disk contention, not the change).

## Notes for the steps that build on it

- **Step 13 (the app):** construct one `catalog::IdentityPairing` with the catalog worker's `ImportService` and keep it;
  a returning file is never merged into another identity (decision 17), so the selection by GameId survives an import;
  `ChangeSet::moved` and `ChangeSet::reviews` (with their ids) say what moved and what waits. Run
  `catalog::runQuickHashPass()` on the catalog worker between imports (it holds the import lock for its run: an import
  started meanwhile from another process is refused), with the quit flag as `cancel` (what was read stays committed) and
  a `limit` per idle slice if the worker must stay responsive; its progress callback runs on the worker; each batch
  (at most every 10 s) changes `data_version`, though it changes nothing a query shows (quick hashes only): a refresh
  may skip it. Review rows are shown and resolved through `ICatalogStore::reviews()` and `catalog::resolveReview()` (a
  later UI; the CLI has them now): offer the review's candidates only (`ReviewDecision::force` exists for the CLI's
  `--force`). After a resolution, run the queries again (`data_version` changes at the commit) and restore the selection by
  GameId: the chosen identity's GameId is an alias from then on (`ICatalogStore::alias()` names the identity it stands
  for).
- **Step 12 (sessions, choices):** nothing to do for aliases: `putAlias()` finds step 12's tables by their foreign keys
  into `rv_game_identities` and moves their rows (a table unique per game keeps the moved identity's row). Keep the
  foreign keys single-column, into `rv_game_identities (id)`. Confirmed on the real tables by the integration of steps
  11b and 12 ([step-11b-12i.md](step-11b-12i.md)): `putAlias()` re-points `rv_identity_aliases.game`,
  `rv_identity_history.game`, `rv_identity_reviews.game`, `rv_launch_game_choices.game`,
  `rv_launch_verifications.game` and `rv_play_sessions.game`, a list the moved-back test asserts.

## For the merge

- **CLAUDE.md**, the retroverse-cli section: `identities list | show | reviews | resolve [--force] | hash` (options, exit
  codes, the pass's progress on stderr); `import` pairs moved files and prints moves and review rows. "Catalog database": `StoredIdentity::quickHash` (read and
  written with the identity), `putQuickHash()`, the history, aliases and review rows, `putAlias()`'s generic re-pointing
  of state rows (a later step's state table needs a single-column foreign key into `rv_game_identities (id)` to follow
  aliases); core helpers: `core::SharedFileReader` and `core::listedFileFacts()` in `file_io.hpp`. Oracles:
  `scripts/quick_hash_oracle.py --check tests/unit/catalog/test_fingerprint.cpp`.
- **docs/architecture.md**: `rv_catalog`'s reconciliation (`IdentityPairing`, the quick-hash pass, review resolution)
  and fingerprints; `tools/cli`'s `identities`.
- **docs/i2/step-4.md**'s table: `rv_identity_history` (alias, review, the CHECK), `rv_identity_reviews`
  (resolved_with), the quick hash read with the identity.
- **docs/i2/step-11a.md**: the seam is filled (`IdentityPairs::hashes`; the alias of a moved-back file says
  `moved-back`; `ChangeSet::reviews` carry their ids; the CLI's JSON `reviews[].review` and `.game`).
- **docs/roadmap.md**: step 11b done, with the figures below.
- **ADR-0008**, suggested amendments: §4 "the quick hash: SHA-256 of a file's whole content up to 128 KiB, else of its
  first and last 64 KiB (rule 1); only a current hash (the size and time it was taken at are the file's last facts)
  pairs, and only when the file name or the last write time is equal too (decision 18): the quick hash is weak evidence
  on a library of hacks and dumps, which share their two ends; a stronger fingerprint (a full hash of files up to some
  size, or of a middle part) would be a decision of its own"; §4 "a file at the path of an identity that had a file of
  its own is that identity's: the moved-back rule aliases only an identity never seen with a file"; §4 "a review is
  resolved with one of its candidates"; §4 "ties: the number of trailing path elements shared; identical copies pair in key order when the counts are
  equal; the size-time-name rule breaks no ties"; §4 "review candidates include the new files of the same file name"; §5
  "an identity that becomes an alias moves every state row naming it to the identity it stands for (a table holding one
  row per game keeps the latter's)".
- **BACKLOG** candidates: the critique's "candidates are every missing identity with a stored hash" (a file deleted at
  one import and back elsewhere at a later one keeps no identity under ADR-0008's rule, which pairs only identities that
  vanished at this import); a compact history for re-keying (one row per identity on a casing table change); review rows
  in the product UI (I7); a quick-hash pass scheduled by the app (idle time, I7); the pass reading with several readers
  (step 7 reads cue sheets with 8; measured cold on the external drive it reads 32 files/s one by one).
