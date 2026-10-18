# I3 step 13r: the catalog's refresh on its worker

Step 13r of I3 (docs/roadmap.md, "I3: M2 SNES vertical slice", row 13r; wave 1, beside 1c, 2, 3, 4, 5, 6 and 7), carried
from I2 (docs/i2/step-13.md, "Notes for step 13r"; ADR-0008 §5; ADR-0012's refresh that goes on during a game) fills
the stub step 0a pre-listed in `rv_app_catalog` (`library_refresh`) and takes `--refresh-library` out of the options
refused until their step. The ES-DE catalog of `retroverse.exe` refreshes itself from ES-DE: "Aggiorna libreria" in the
quick catalog and `--refresh-library` run the import of `retroverse-cli import` (`catalog::ImportService` with
`catalog::IdentityPairing`, its import lock) as a job of the catalog worker, in slices, so that the quick catalog's
lists, the rows' readiness, Avvia's resolution and the play sessions are answered while it runs; quitting cancels it
within the bound. After its commit the systems' readiness is resolved again, the readiness cache is cleared and the list
runs again with the selection restored by GameId; the line under the header tells the progress and the result (Italian,
in the RML document). The overlay's log copy no longer shows a line twice while workers log (BACKLOG "Log copy with
worker threads"). Nothing changes what a frame of the room draws (golden G01 unchanged).

## What was built

| File | What |
|---|---|
| `src/app/include/rv/app/library_refresh.hpp`, `src/library_refresh.cpp` (`rv_app_catalog`, was a stub) | `runSlicedImport()` (ImportService over its ports, wrapped so that every port call is a slice boundary), `RefreshHost` (what the slices ask of the worker), `RefreshPorts`, `RefreshPhase`, `RefreshOutcome`, `RefreshTrigger`, `RefreshProgress`, `RefreshSlicing`, `RefreshSummary`, `summarizeImport()`, `kRefreshSlice` (4 ms), `kRefreshProgressInterval` (100 ms) |
| `src/app/include/rv/app/catalog_worker.hpp`, `src/catalog_worker.cpp` | `CatalogWorker::refresh()`, the refresh job (`Runner` is the `RefreshHost`: `serve()`, `serveWhile()` with its helper thread, the progress posts, the import's writer interruptible), the systems resolved again after the commit (`resolveSystems()`, shared with the opening), `CatalogRefreshed`, `CatalogResults::refreshProgress` and `::refreshed`, `GameReadiness::generation`, `CatalogList::waitedMs`, `Options::makeSource` (tests); `stop()` sets the import's cancel flag and interrupts the writer while the import writes |
| `src/app/include/rv/app/catalog_database_source.hpp`, `src/catalog_database_source.cpp` | the systems a refresh resolved again taken (chips, counts), the readiness cache cleared, answers of an older generation dropped; `Collected::refreshed` |
| `src/app/include/rv/app/app_catalog.hpp`, `src/app_catalog.cpp` | `AppCatalog::supportsRefresh()`, `requestRefresh()`, `refreshState()` (`RefreshState`), `stopMilliseconds()`; `CatalogChanges::refreshEnded`, `::refreshed`; `EsdeAppCatalog`'s refresh (the lists' waits while it runs, the stop's time); `recordRefresh()` |
| `src/app/include/rv/app/run_report.hpp`, `src/run_report.cpp` | `RefreshRecord` and `CatalogRecord::refreshes` (keys only added, optional when read) |
| `src/app/src/product_ui.cpp` | "Aggiorna libreria" (applied after the frame's commands), `--refresh-library` once the catalog opened with its first list, the `refresh` phase opened and closed (`FrameTiming`), the records (before, after: the list run again), the screen's refresh line |
| `src/app/src/command_line.cpp`, `include/rv/app/command_line.hpp` | `--refresh-library` taken (its refusal block and the "not implemented yet" of its help removed) |
| `src/ui/include/rv/ui/quick_catalog_screen.hpp`, `src/quick_catalog_screen.cpp` | `QuickCatalogEvents::refresh`, `QuickCatalogRefresh`, `setRefresh()`/`refresh()`, the data model's `refresh` and event `refresh()`, the `library` region (search Up, back Down; focused only while the button shows) |
| `src/ui/include/rv/ui/ui_assets.hpp` | `kRegionLibrary`, `kRefreshId`, `kRefreshStatusId`, `kRefreshRunning`, `kRefreshStates`, `kRefreshPhases` |
| `assets/ui/quick_catalog.rml`, `.rcss` | the button in the header (with the count, at its right end), the line under the header: a text per phase while it runs and per outcome |
| `src/core/include/rv/core/log.hpp`, `src/log.cpp` | `logLinesSince()` / `LogLinesSince`: the in-memory lines and the count under one lock (one sink, `MemorySink`, replaces spdlog's ring buffer sink and the counting sink), each configuration a generation |
| `src/devui/include/rv/devui/log_view.hpp`, `src/log_view.cpp` | `LogView::refresh()` on `logLinesSince()` |
| `tests/unit/app/test_library_refresh.cpp` (new), `catalog_test_support.hpp` (new: the imported fixture of `test_catalog_worker.cpp`, shared) | the unit tests (below) |
| `tests/unit/devui/test_log_view.cpp`, `tests/unit/ui/test_ui_documents.cpp`, `tests/unit/app/test_command_line.cpp` | the log race, the document's texts, the option taken |
| `tests/smoke/test_library_refresh.cpp`, `tests/input_scripts/catalog_refresh.json` (new) | the GPU smoke cases (below) |

## Threading: the import in slices

`ImportService::run()` is one call (the load, the media, the comparison and the pairing, the write transaction), so the
worker cannot take a job between two of its pieces by itself. `runSlicedImport()` gives it ports that wrap the real ones
(`SlicedSource`, `SlicedMedia`, `SlicedStore` with its `SlicedTransaction`, `SlicedPairing`):

```text
catalog worker (one thread: the writer, its reader)                  helper thread (no connection)
---------------------------------------------------                  -----------------------------
refresh job: library.json's library, EsdeLibrarySource, EsdeMediaResolver, SqliteCatalogStore on the writer,
IdentityPairing; ImportService::run with the worker's cancel flag
  lockImport()                         (boundary)
  load()             serveWhile -------------------------------->   EsdeLibrarySource::load (ES-DE's files, ROM folders)
    answers every job that comes, until the load returned  <-------- done (wakes the worker)
  presence(first of a system) serveWhile ---------------------->    the system's media folders listed
  presence(others)     boundary: when 4 ms passed, serve(false)
  identities()/rows    boundary (comparing: one system after another)
  pair()             serveWhile -------------------------------->   quick hashes of new files
  beginImport()        writing(true): stop() may now interrupt the writer
    put*/remove*       boundary: serve(true) answers lists and readiness only
    commit()           writing(false)
then: the systems resolved again (ES-DE's configuration, the profiles, the choices, systems()), a new readiness
generation; CatalogRefreshed posted (summary, systems)
```

- **What a boundary answers.** Lists (on the worker's reader: a WAL reader sees the last commit, never the import's
  open transaction) and readiness always; launches and play sessions only outside the write transaction (they use the
  writer; inside it a session would join the import's transaction). Other refreshes never (one at a time:
  `refresh()` refuses while one is queued or runs). Jobs keep their order among those answered.
- **One writer on one thread** (CLAUDE.md, ADR-0008 §5): the helper thread never touches a connection; the writer and
  every port that uses it stay on the worker's thread. The load's source and the media resolver are used by one thread
  at a time (the worker waits while the helper runs them).
- **Quitting** (`CatalogWorker::stop`, via `ProductUi::finish`): the queued jobs but session writes dropped (a queued
  refresh with them), the cancel flag set (the load looks at it between systems and within folder walks, the import
  after the load, every 256 media questions and writes, between systems), the reader interrupted, and the writer too
  while the import's write transaction is open (its commit included: SQLite rolls the transaction back whole). A
  helper still reading is waited for (it sees the flag). The import is thus rolled back or committed whole; the stop's
  bound is the worker's (5 s), the budget's 2 s is measured (below).
- **After the commit**: readiness resolved again as at the opening (the configuration and the emulators read again,
  `assign`'s choices, the systems' games), `GameReadiness::generation` incremented: the source takes the new systems
  (chips and their counts), clears its readiness cache and drops the answers of an older generation (their rows ask
  again as they are read: the list runs again). ProductUi calls `QuickCatalogModel::refresh()` (platforms read again,
  the same query materialized again, the selection restored by GameId).
- **A refresh during a game** (ADR-0012 §7's note): nothing pauses it; the host idles while the game runs and the
  worker goes on, its end collected by the idle wakes. Avvia during a refresh is resolved at the next boundary outside
  the write transaction.
- **Lockstep** (`--fixed-dt`): a refresh is not something a frame waits for (`busy()` is unchanged): its frames are
  real frames, measured in the `refresh` phase. The list it makes the model ask for after its commit is waited for, as
  any list.

## The quick catalog

- **"Aggiorna libreria"**: a button in the header at its right end, after the count, shown while the catalog can refresh
  (the ES-DE catalog, open; never the mock catalog or a notice). With the pad: Up from the list's first game reaches the
  chips, the search field, then the button (its own region, `library`; Down goes back to the search field). A press
  while one runs does nothing (logged).
- **The line under the header** (`refresh-status`), from the first refresh on, until the next:
  - while it runs, by phase: "Aggiornamento della libreria: lettura delle cartelle di ES-DE...", "... controllo dei media
    (N di M)...", "... confronto con il catalogo (N di M sistemi)...", "... scrittura del catalogo...";
  - its end: "Libreria aggiornata: nessuna modifica." or "Libreria aggiornata. Aggiunti: N, rimossi: N, modificati: N.",
    with "Alcuni giochi spariti sono rimasti nel catalogo: "retroverse-cli import --accept-removals" li rimuove." when the
    mass-disappearance guard kept a system; "Aggiornamento della libreria interrotto: il catalogo è rimasto com'era.";
    "Aggiornamento non riuscito: è in corso un'altra importazione. Riprova tra poco." (the import lock, `busy`);
    "Aggiornamento non riuscito: la libreria ES-DE non si legge. "retroverse-cli import" dice perché." (`failed`).
- **`--refresh-library`**: one refresh once the catalog opened, with its first list applied (the record's `before`). A
  catalog that did not open, or the mock catalog, records it as `unavailable` and runs nothing.
- The library imported is library.json's as the catalog read it when it opened (a re-association needs a restart, as
  before); the import is the CLI's: no `--accept-removals`, no partial import.

## The run report

`catalog.refreshes`, one object per refresh: `trigger` (`command-line`, `user`), `outcome` (`done`, `cancelled`, `busy`,
`failed`, `running` when the worker never answered, `unavailable`), `error`, `frames` {`request`, `end`}, `times`
{`ms` on the worker, `longestSliceMs`, `stopMs` when the run ended during it}, `entries`, `added`, `removed`,
`changed`, `moved`, `reviews`, `guarded`, `slices`, `served` (requests answered at the boundaries), `lists` (the lists
asked while it ran, from the request to the answer: p50, p95, p99, max), `before` and `after` (the catalog's selection,
search and filter and the walker's pose when it was asked for, and once its list ran again). The `refresh` phase
(step 0d's `kPhaseRefresh`) is opened when a refresh is accepted and closed when its end is collected (main thread).

## The log race (BACKLOG "Log copy with worker threads")

`LogView::refresh()` read `core::loggedMessageCount()` and then the newest lines: a line logged by another thread in
between took the place of the batch's oldest line and came again with the next batch. Now one spdlog sink
(`MemorySink`, `base_sink<std::mutex>`) keeps the formatted lines and the count, and `core::logLinesSince(generation,
seen, maxLines)` returns the lines logged after `seen` with the count they bring the reader to, under the sink's own lock
(the one the logger takes for each message); each `initLogging()` starts a generation, so a view never mixes two
configurations' lines. A unit test logs 9,000 lines from three threads while a view refreshes: every line once, each
thread's in order.

## Tests

- **Unit** (`rv_tests "[library_refresh]"`, GPU-free, the standard fixture imported in-process as `retroverse-cli
  import` does):
  - a ROM added to snes and one removed from gb after the import: nothing changes until `requestRefresh()`; a second
    one is refused while it runs; then Done (1 added, 1 removed), the list ran again (15 games) with Tiny Tennis still
    selected, the new game listed with snes's readiness, the removed one gone, the chips' counts, the database; psx's
    games resolved per game again (Solo Disc `requires-configuration`/`inject-file`); a refresh that finds nothing
    changes nothing;
  - with a load held 1.5 s: a filter's list (8 games) and Avvia's resolution answered while it runs, the lists' waits
    (`lastLists`) under 100 ms, the filter kept after the commit;
  - with a load held 60 s: `shutdown()` ends in under 2 s, Cancelled, the catalog unchanged (15 games) and its integrity
    check `ok`;
  - another instance holding the import lock: Busy, nothing changed; once released, Done;
  - `runSlicedImport()` over a recording host: the load and the first media question of each system offloaded, the
    write transaction told once (open, close), the phases in order; a cancel flag set before: Cancelled, no
    transaction;
  - `summarizeImport()`'s outcomes (Cancelled; Conflict of the lock as Busy, of library.json as Failed; others Failed)
    and counts (the guard's systems); the names of the states and phases equal the document's keys
    (`kRefreshStates`, `kRefreshPhases`); `RefreshRecord` read back from the report.
- **The log** (`[devui][log_view]`): the concurrent case above; the earlier cases unchanged.
- **Documents** (`[ui][documents]`): the button, its region, the line, a text for every state and phase.
- **GPU smoke** (`gpu.refresh: ...`, hidden, `--fixed-dt`, no launch): the standard fixture imported with
  `retroverse-cli` into the case's data directory, then a ROM added:
  - `--refresh-library` and `catalog_refresh.json` (600 frames): the first refresh (`command-line`) Done with 1 added,
    the selection the same game one place down (the new game sorts first), 16 games listed and shown; "Aggiorna
    libreria" reached with Home and Up and pressed on frame 90: a second refresh (`user`), nothing new, the selection
    kept; the `refresh` phase opened twice and closed, its frame times logged (informative); `retroverse-cli db check`
    clean;
  - 10,000 more files in gb and `--refresh-library` for 40 frames: the run ends during the refresh, `cancelled`, the
    worker stopped within 2 s (`stopMs`), the phase left open, the catalog unchanged (15 games), `db check` clean.

## Measured

FIGURES

## Decisions taken within the step

1. **Slices through the ports, not a new ImportService API.** The import stays one call of `rv_catalog` (no change to
   the domain or its tests); the app wraps its ports, and every port call is a boundary. The long reads that touch no
   database (the load, the first media question of a system, the pairing's quick hashes) run on a helper thread while
   the worker answers jobs as they come; the writer never leaves the worker's thread.
2. **A helper thread per offloaded call, not the WorkerPool**: the app has no pool yet (step 10b makes the app's one);
   the offloaded calls are few (the load, one per system, the pairing per system) and must run while the worker waits,
   which the pool's priorities and queue bound do not promise. 10b may route them through the pool.
3. **Launches and sessions wait for the write transaction alone**, as other writers do (step 11a: 0.05 s for a refresh
   that changes little); lists and readiness never wait for more than a slice.
4. **Readiness generations** instead of tagging every request: an answer resolved before the systems were resolved again
   is dropped, and the row asks again.
5. **The refresh is no lockstep wait**: scripted runs measure its frames; the lists it causes are waited for.
6. **The command-line refresh starts with the first list applied**, so that its `before` names the selection the list
   keeps afterwards.
7. **The import's library is the opening's library.json** (no re-read at refresh): a re-association means a restart, as
   for the CLI's `library create --force` followed by a new start.
8. **The log's count lives in the lines' sink** (one lock for both), with a generation per configuration.
9. **The shared test fixture moved to `tests/unit/app/catalog_test_support.hpp`** (it was private to
   `test_catalog_worker.cpp`).
10. **CLAUDE.md is not edited here** (see "For the merge").

## Open issues

- The roadmap's acceptance on the **real library** (frame p99 over the `refresh` phase in Release through
  `./scripts/real-library.ps1`, lists within 100 ms during an import there) is not measured by this step: it needs a
  quiet machine and ES-DE closed; the step's figures are the fixture's (above). A `[real-library][refresh]` test would
  run the app on the imported real library with `--refresh-library`.
- `app/catalog_worker: a session write that meets a busy database stays pending on the worker, then is written` fails
  intermittently on this machine under load (its `CHECK(worker.idle())` after 600 ms), on main's build as well: not
  caused by this step.
- The quick-hash pass (`runQuickHashPass`) is not run by the app; step 11b's note has it take turns on the same worker
  (BACKLOG).

## For the merge

- **CLAUDE.md** (not edited by the step):
  - "Running the app": `--refresh-library` refreshes the catalog from ES-DE once it opened ("Aggiorna libreria" in the
    quick catalog does the same: the import of `retroverse-cli import` on the catalog worker, in slices, the list usable
    meanwhile; quitting cancels it); the option's "refused until 13r" goes; the run report's `catalog.refreshes`.
  - "Running the app", the quick catalog: the button (search Up) and the line under the header.
  - "Layout and layering": `rv_app_catalog`'s `library_refresh` (the sliced import, GPU-free).
  - "Conventions", core helpers: `log.hpp`'s `logLinesSince` (lines and count under one lock).
  - GPU smoke tests: `tests/smoke/test_library_refresh.cpp` (`gpu.refresh: ...`).
- **docs/roadmap.md**: row 13r done (edited here).
- **docs/architecture.md**: the catalog worker's refresh (the threading diagram above).
- **BACKLOG.md**: "Log copy with worker threads" done (remove it); "Step 13r, the refresh on a worker, moved to I3" done;
  "A refresh when another process imports" stays (I7's watcher); a `[real-library][refresh]` measurement for the gate.
- **ADR-0008 §5** is met: one writer on the worker, the import on it in slices, `sqlite3_interrupt` at shutdown also of
  the import's write transaction.
