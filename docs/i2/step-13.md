# I2 step 13: the real catalog in the app

Roadmap row: I2 step 13 (wave 4, needs A1, 9, 10, 11a, 11b, 12). Decisions: [ADR-0008](../decisions/ADR-0008-esde-catalog.md)
§5 (one writer on the catalog worker, the main thread reads through its own read-only connection and never waits,
paging, `sqlite3_interrupt` at shutdown), §8 (readiness, the choice of command), §9 (play sessions); ADR-0005 (product
UI), ADR-0006 (launch and return); specification NFR-01. It builds on step 6 (the paged source contract), 9 (queries),
10 (launch resolution), 11a/11b (the import, identities), 12 (sessions, choices) and A1 (ProductUi, LaunchFlow). This
page is folded into the shared documentation at the wave merge.

Outcome: `retroverse.exe` shows the ES-DE catalog of its user data directory in the quick catalog. A catalog worker
thread owns the database's writer and runs the queries; the main thread fetches pages on its own read-only connection
and never waits on the database. The card shows each game's launch readiness (each system's resolved once when the
catalog opens, a game's own only where it can differ) with an Italian sentence for every readiness and blocker code.
Avvia resolves the game again on the worker right before the launch (its file on disk now, the database's choices) and
hands the request to A1's LaunchFlow; a game that is not ready shows why instead. Every launch is recorded as a play
session on the worker. After the return the catalog's selection, search, filter and the walker's camera are what they
were; the run report records them before and after. Without `library.json` or a database the quick catalog says how to
make them. `--mock-catalog` stays for the tests and takes precedence. This step does not import from the app: the
catalog is what `retroverse-cli import` made (step 13r may add a refresh on the worker).

## What was built

| File | What |
|---|---|
| `src/app/include/rv/app/catalog_worker.hpp`, `src/catalog_worker.cpp` | `CatalogWorker` (the worker thread), `CatalogState`, `noticeOf()`, `CatalogSystem`, `GameLaunchState`, `GameReadiness`, `CatalogOpened`, `CatalogList`, `ResolvedLaunch`, `StoredSession`, `CatalogResults`, `kLibraryUnreadableCode` |
| `src/app/include/rv/app/catalog_database_source.hpp`, `src/catalog_database_source.cpp` | `CatalogDatabaseSource`: step 6's `ui::CatalogSource` over the worker; `stateKeyOf()` |
| `src/app/include/rv/app/catalog_launch_library.hpp`, `src/catalog_launch_library.cpp` | `CatalogLaunchLibrary` (ES-DE's configuration, the emulator profiles, the probe), `loadCatalogLaunchLibrary()`, `catalogLaunchChoices()`, `resolveCatalogLaunch()` (`GameFileCheck`: the catalog's availability or the disk now), `resolveSystemLaunch()`, `resolvedPerGame()` |
| `src/app/include/rv/app/app_catalog.hpp`, `src/app_catalog.cpp` | `AppCatalog` (what ProductUi drives), `MockAppCatalog` (A1's mock, moved out of ProductUi), `EsdeAppCatalog` (the worker, its source, the launches and their sessions), `LaunchDecision`, `CatalogChanges`; `startedPlaySession()`, `finishedPlaySession()`, `blockedPlaySession()`, `refusalOf()`, `fileTimeTicksOf()` |
| `src/app/CMakeLists.txt` | `rv_app_catalog` (new, GPU-free: the four sources above; links `rv_app_cli`, `rv_catalog`, `rv_esde`, `rv_catalog_db`, `rv_platform_win`, `rv_ui_model`), linked by `rv_app` and `rv_tests` |
| `src/app/src/product_ui.hpp`, `.cpp` | ProductUi over an `AppCatalog`: decisions applied on the frame after they arrive, the worker's answers collected every frame and every idle wake, lockstep runs, the before/after views, the frame times, the catalog's notice, `typeText()` |
| `src/app/src/retroverse_app.cpp` | the catalog made first (the worker opens the database while the scene loads): `--mock-catalog`'s, else the ES-DE catalog (not in the model viewer); script text typed into the product UI |
| `src/app/include/rv/app/launch_flow.hpp`, `src/launch_flow.cpp` | `LaunchFlowEvents` (`started`, `ended` with `LaunchEndCause` and whether the confirmation was awaited), `noteBefore()`, `noteAfter()` |
| `src/app/include/rv/app/launch_outcome.hpp` | `launch_outcome::kNotReady` |
| `src/app/include/rv/app/run_report.hpp`, `src/run_report.cpp` | `CatalogView`, `TimeSummary`, `summarizeTimes()`; `CatalogRecord` gains `notice`, `library`, `systems`, `search`, `filter`, `times` {`openedMs`, `firstListMs`, `firstPageMs`}, `pages`, `frames`, `interruptedSessions`; `LaunchRecord` gains `before`, `after`, `session` {`id`, `error`, `verified`} (schema version 1: keys only added, all optional when read) |
| `src/ui/include/rv/ui/catalog_source.hpp` | `CatalogEntry::readiness`, `blocker`, `blockerDetail`, `verified` |
| `src/ui/include/rv/ui/quick_catalog_model.hpp`, `src/quick_catalog_model.cpp` | `QuickCatalogModel::reread()`, `contentRevision()`: rows changed in place (a readiness learned later) |
| `src/ui/include/rv/ui/quick_catalog_screen.hpp`, `src/quick_catalog_screen.cpp` | the card's `readiness`, `blocker`, `detail`, `verified`; `setNotice()`/`notice()`; rows and card bound again on a content revision |
| `src/ui/include/rv/ui/ui_assets.hpp` | `kReadinessKeys`, `kBlockerCodes` (32), `kNotice*`, `kNotices` |
| `src/input/include/rv/input/input_script.hpp`, `src/input_script.cpp` | input script steps with `"text"` (typed once on `fromFrame`), `InputScript::textAt()`, `kMaxTextBytes` |
| `assets/ui/quick_catalog.rml`, `.rcss` | the notices, a sentence per readiness and per blocker code, the "Dettagli" line, "Pronto: già avviato e tornato senza errori" |
| `assets/ui/game_session.rml` | the outcome `not-ready` with a text per readiness |
| `tests/unit/app/test_catalog_worker.cpp`, `test_catalog_sessions.cpp`, `test_catalog_real.cpp` (hidden) | the tests (below) |
| `tests/unit/ui/test_ui_documents.cpp`, `tests/unit/app/test_game_session_document.cpp` | the new texts checked |
| `tests/smoke/test_catalog.cpp`, `tests/smoke/CMakeLists.txt` | the GPU smoke cases (below); `rv_smoke_tests` depends on `retroverse-cli` |
| `tests/smoke/test_launch.cpp` | the closed-catalog case: a run without `--mock-catalog` has the ES-DE catalog (notice `no-library`) |
| `tests/input_scripts/catalog_search_launch.json`, `catalog_blocked_launch.json`, `catalog_open.json` | the scripts |

## Threading

```text
main thread                                          catalog worker (CatalogWorker)
-----------                                          ------------------------------
EsdeAppCatalog made at onInit's start  ------------> open: library.json, the data-directory guard, the database
  (before the scene loads)                                (openCatalogDatabase, never created), SessionStore on the
                                                          writer, closeInterrupted(processLiveness), a
                                                          SqliteCatalogQuery on its reader, another reader for the
                                                          main thread, systems() for the chips, library mismatch,
                                                          the library's configuration, each system's readiness
ProductUi::update / onIdle: AppCatalog::poll() <---- CatalogOpened (the main thread's SqliteCatalogQuery moved over)
  model.refresh() on opened                    
CatalogDatabaseSource::query() -> Pending  --------> run() on the worker's query (its kept list narrows typing)
  model.poll() switches lists                  <---- CatalogList {serial, ids, (id, position) sorted}
rows(): fetch(page) on the main thread's reader
  (timed); unknown games' readiness asked  --------> details(), gameChoice(): the system's state or the game's own
  model.reread() when a row's state changed    <---- GameReadiness {state, ownState}
Avvia: requestLaunch(token) ---------------------> resolve right before the launch: details (an alias resolved),
                                                      gameChoice, systemChoices (read again), resolveLaunch with the
                                                      file on disk now, the guard's verdict
  next frame: LaunchFlow::request / refuse     <---- ResolvedLaunch {resolution, session base}
LaunchFlowEvents::started -> beginSession  -------> SessionRecorder::begin (one attempt, 1 s busy timeout)
LaunchFlowEvents::ended   -> finishSession -------> update + flush; pending writes tried every 2 s on the worker;
                                                      a normal return records its verification
                                               <---- StoredSession {id, error, verified}, verified choices
ProductUi::finish: flow.quit, then stop()  -------> interrupt the running query (sqlite3_interrupt through
  (bounded: 5 s; the worker detached if not)          Connection::interrupt), queued lists dropped, queued and
                                                      pending session writes tried for 3 s, then the thread ends
```

- **One writer, on the worker.** The worker's `Database` holds the writer (session writes, `closeInterrupted`) and its
  own `SqliteCatalogQuery` on a reader (lists, `systems()`, `details()`); the main thread holds the other
  `SqliteCatalogQuery` (pages only, `fetch()`), created by the worker and handed over in `CatalogOpened`. No GL or SDL
  call happens on the worker; it wakes the host with `AppContext::wakeCallback()` after it posts results.
- **The main thread never waits.** Requests are queued; results are collected by `AppCatalog::poll()` every frame and
  every idle wake. A query queued behind another is replaced (typing coalesces); `CatalogDatabaseSource` switches only
  to the list of the latest serial. Session writes that meet a busy database stay pending on the worker
  (`SessionRecorder`), tried again every `sessionRetryInterval` (2 s), never on the main thread.
- **Lockstep runs.** With `--fixed-dt` (scripted, reproducible runs) the host idles after each frame while something
  the screen waits for is with the worker (`AppCatalog::busy()`: the opening, a list, readiness, a decision), woken by
  the worker's results: the frames of an input script see the same lists and launches on every run. Other runs never
  wait for the worker; the screen shows the previous list until the new one arrives. A decision that arrives during an
  idle wait is applied by the next frame's update, so that "Gioco in corso" is presented before the emulator starts
  (A1's rule).
- **Quitting** (`ProductUi::finish`, the quit hook): `LaunchFlow::quit` ends a running session (its `ended` event queues
  the session's end), then `EsdeAppCatalog::shutdown()` stops the worker: queued lists and readiness are dropped, the
  query connection is interrupted (`sqlite3_interrupt`), the session writes queued and pending are tried for
  `stopPatience` (3 s), and the thread is joined within `kStopBound` (5 s); past it, it is detached with its wake
  callback dropped and a warning logged. A unit test stops a worker in the middle of a query that never ends: it ends
  in well under 2 s with the query `Cancelled`.
- **Determinism**: positions are found in a vector sorted by GameId (no unordered container), readiness is cached in an
  ordered map (bounded to 4,096 games).

## The catalog in the app

- `retroverse.exe` without `--mock-catalog` (and not in the model viewer) always has the product UI over the ES-DE
  catalog of its user data directory. The worker opens it as `retroverse-cli`'s launch commands do: `library.json` read
  (`esde::readLibraryFile`), then the data-directory guard (`core::checkUserDataDirectory` with
  `esde::readOnlyFolders(profile)`, as the CLI's dispatcher has it) before anything is written into the database, then
  `openCatalogDatabase` with `create = false` (migrated when older; a newer one is refused).
- What the list says instead of games (`CatalogState`, `ui::quick_catalog::kNotice*`, Italian in `quick_catalog.rml`):
  `opening` ("Apertura del catalogo..."), `no-library` (how to run `retroverse-cli library create` then `import`),
  `no-database` (`retroverse-cli import`), `data-directory` (choose another `--data-dir`), `library-mismatch`
  (re-associate with `library create --id ... --force`, then import), `unavailable` (`retroverse-cli db check` says
  why). Nothing is written in the data directory then (tested). The run's log still goes to `<user data>/logs` before the
  guard runs: the app sets up its log first (unlike the CLI, which logs to stderr until the guard passed); the guard
  keeps the database out of the library's folders (BACKLOG candidate below).
- ES-DE's configuration that cannot be read (its `es_systems.xml` gone) leaves the catalog browsable: every game
  "requires launcher configuration" with the blocker `library-unreadable`.
- **Chips**: one per system with available games, in SortOrder::System's order, keyed by the ES-DE system name,
  labelled with its full name, counting its games. **Rows**: title, the system's full name as the platform, the year of
  the release date, players (`formatPlayers`), developer, publisher, genre, description.
- **NFR-01 times** in the run report (`CatalogRecord::times`), from the process's creation
  (`platform_win::currentProcessIdentity().creationTime`): `openedMs` and `firstListMs` when the worker finished them,
  `firstPageMs` when a frame first showed rows of the catalog (the quick catalog open). The worker starts at the top of
  `RetroverseApp::onInit`, before the scene's procedural surfaces are generated.

## Readiness on the card

Each system with games is resolved once when the catalog opens (`resolveSystemLaunch`: `catalog::resolveLaunch` without
a game, Retroverse's choice for the system from the database before emulators.json's, the gamelist's
`<alternativeEmulator>` as the last import read it). A game's own readiness is resolved on the worker, when its row is
first read, only where it can differ (`docs/i2/step-10.md`): its availability (not Available: source unavailable or ROM
missing, from the catalog, not the disk), its `<altemulator>` (with `AlternativeEmulatorPerGame`), Retroverse's choice for
it (`assign --game`), a folder game, or a system whose resolution leaves something to each game (its notes: an `%INJECT%`
file, a `%STARTDIR%=%GAMEDIR%` folder); otherwise it is its system's. A row shows its system's until the worker answered;
a game whose own state differs makes the screen read its rows again (`QuickCatalogModel::reread()`), which binds the rows
and the card again. "Verified" (ADR-0008 §8) is a verification of the choice in effect for the system (the game's own
choice for a game resolved on its own); a session that returns normally verifies its choice, and the rows show it.

The card (`quick_catalog.rml`; keys in `ui::quick_catalog::kReadinessKeys`, `kBlockerCodes`):

| Readiness | Card |
|---|---|
| `source-unavailable` | Non disponibile: la cartella dei giochi non è raggiungibile |
| `rom-missing` | Non disponibile: il file del gioco non si trova |
| `requires-configuration` | Da configurare: il comando di avvio non si può eseguire così com'è |
| `emulator-not-found` | Emulatore non trovato |
| `core-not-found` | Core dell'emulatore non trovato |
| `ready` (verified) | Pronto: già avviato e tornato senza errori |

then the first blocker's code as a sentence (one per code: `source-unavailable`, `rom-missing`, `rom-folder-unresolved`,
`rom-root-unknown`, `system-unknown`/`system-not-configured`, `library-unreadable`, `no-command`, `choice-stale`,
`profile-unknown`, `profile-template`, `profile-variable-missing`, `profile-invalid-request`/`invalid-request`,
`program-not-allowed`, `program-refused`, `program-repeated`, `program-missing`, `install-unknown`, `token-unsupported`,
`token-malformed`, `command-placeholder`, `inject-file`, `start-folder-missing`, `start-folder-repeated`,
`start-folder-glued`, `start-folder-relative`, `find-rules-missing`, `find-rule-undecided`, `emulator-not-found`,
`core-not-found`, `core-malformed`), then the blocker's own English sentence on a smaller "Dettagli:" line, as the
game-session panel shows the runner's words. The mock catalog's games have no readiness: their card shows A1's state
texts. The rows keep A1's state keys (`stateKeyOf()`: `unavailable`, `needs-configuration`, `emulator-missing`).

Avvia on a game that is not ready right before the launch: the game-session panel's outcome `not-ready`, its reason the
readiness (a text each in `game_session.rml`, a fallback for others), its detail the first blocker's words; the attempt
is recorded as a blocked session. A game the worker cannot resolve at all (the catalog no longer has it) gets
`not-ready`/`unresolved` and records nothing.

## Launches and sessions

- **Resolution right before the launch** (`CatalogWorker::resolve`, as `retroverse-cli launch --game`): the game's
  details (an alias resolved through `SessionStore::resolveGame`), a game or a missing file of library.json's library,
  Retroverse's choice for the game, the systems' choices read again (another process's `assign` counts), the system as
  the library configures it now, the file looked for on disk now (`esde::listedPath`), its `<altemulator>` and its
  system's `<alternativeEmulator>` as the last import read them; the guard's verdict for `refusedByGuard`.
- **LaunchFlow** is unchanged in its states; `LaunchFlowEvents` tell its owner when a session began and how it ended
  (`LaunchEndCause`: `session` on its own, `user` "Torna a Retroverse"/"Sono tornato", `quit`, `wait-limit`, and whether
  a UserConfirmReturn confirmation was awaited when quitting or the wait limit stopped it).
- **The play session** (step 12's mapping, `app_catalog.cpp`): `startedPlaySession` (Running, the wall-clock start taken
  back to `SessionTimes::begun`, the started process), `finishedPlaySession` (`catalog::recordOutcome` with the owner's
  reason: none for the session's own end and the user's return, `Quit`, `WaitLimit`; the end at the authoritative exit
  when the game ended by itself, else when the session ended; the run time; a session stopped while it awaited the
  confirmation left running; a refusal by the guard), `blockedPlaySession` (the readiness, the blockers' codes and
  messages). Owner: `currentProcessIdentity()`. At the catalog's opening, `closeInterrupted()` with `processLiveness()`
  (the count in `CatalogRecord::interruptedSessions`).
- The run report's `LaunchRecord::session` gives the stored session's id, an error, and whether a verification was
  recorded; `before` and `after` the catalog's selection (id, position, readiness, blocker), search, filter and the
  walker's pose when Avvia asked and on the first frame after the end.

## Input scripts: typed text

A step may carry `"text"` (UTF-8, 1 to 256 bytes, no control character), typed once on `fromFrame` into the product UI's
focused text field (`ProductUi::typeText` → RmlUi's `ProcessTextInput`), with or without `"actions"`:
`{"fromFrame": 11, "toFrame": 11, "text": "nintendo"}`. The quick catalog's search field is reached with `ui_up` from the
list's first game (the chips), then `ui_up` again.

## Tests

- **Unit** (`rv_tests "[catalog_worker],[catalog_sessions]"`, GPU-free): the standard fixture imported in-process
  (`catalog::ImportService`, as `retroverse-cli import`), psx assigned "Fake Emulator (Inject)":
  - the catalog opened on the worker: four chips (Tutte 15, gb 8, snes 5, psx 2, in the System order), the whole list
    (15), pages cut at the end, every row's position, a row's text, readiness and players, the page times;
  - search and filters on the worker: `nintendo` (13, the systems' full names), `gb` keeping the selected game, a word,
    another system's filter emptying the list, diacritics (`cafe`), typing coalesced (only the last query's list), an
    unknown system refused;
  - readiness: psx resolved per game (its notes), Solo Disc `requires-configuration`/`inject-file` with its detail, Twin
    Tracks ready, the rows read again (`contentRevision`); every readiness has a card key;
  - an import under the screen (a file deleted, imported again): the vanished game's position gives an id-only row until
    the refresh, then the selection restored by GameId one place up;
  - Avvia resolved on the worker (the profile's request, the ROM path), one decision at a time; Solo Disc refused
    (`not-ready`, `requires-configuration`), its blocked session written by the worker before it stops;
  - a session write while another connection holds the write lock: pending on the worker, nothing returned, written
    after the release, with its verification;
  - the sessions of owners that are gone closed as interrupted at the opening (and only those);
  - no library.json (nothing written), no database, a data directory inside the ROM root (the guard), another library;
    ES-DE's systems unreadable (browsable, `library-unreadable`);
  - stopping during an endless query: interrupted (`Cancelled`) and joined in under 2 s;
  - the flow's events and the session mapping on a scripted runner: a return (Running then Returned, its run time and
    end), the user's return, quitting and the wait limit (left running with their reasons), UserConfirmReturn confirmed
    (a verified return) and stopped while awaited (left running), a failed start (the guard's refusal told apart), a
    blocked session and the panel's message;
  - the documents: a text for every notice, readiness and blocker code (`[documents]`), the panel's `not-ready`.
- **GPU smoke** (`gpu.catalog: ...`, hidden, `--fixed-dt`, `RV_LAUNCH_ALLOWLIST` = the fake): the standard fixture
  imported with `retroverse-cli` into the case's data directory, an emulators.json whose `fake-return` profile runs the
  build's fake (2.5 s), `assign gb --profile fake-return`, `assign psx --command "Fake Emulator (Inject)"`:
  - search and launch (`catalog_search_launch.json`, 40 frames): `nintendo` typed, the gb chip, End (Tiny Tennis),
    Avvia on frame 23, the request on 24, started and ended between frames 24 and 25; Returned, exit 0, 2.5 s, no frame
    while it ran; the session recorded (returned, `profile fake-return`, `retroverse-system`, the owner, verified);
    `before == after` (selection, search `nintendo`, filter `gb`, readiness `ready`, camera); the catalog record (15
    games, 3 systems, the times, the pages);
  - a blocked game (`catalog_blocked_launch.json`, 30 frames): `solo` typed, Avvia: `Blocked`, `not-ready`,
    `requires-configuration`, the `%INJECT%` detail, nothing started, the outcome panel; its card said
    `requires-configuration`/`inject-file` before; the blocked session in the database; a screenshot of the card;
  - no library.json (`catalog_open.json`): the notice `no-library`, a screenshot, nothing written but the log and the
    report;
  - A1's closed-catalog case now compares the mock catalog's frames with the ES-DE catalog's (`no-library`): 0 pixels
    apart. Golden G01's runs have the ES-DE catalog of an empty data directory, closed: unchanged.

FIGURES_TESTS

## NFR-01 (Release)

FIGURES_NFR

## Draft: the manual pad-only session of Gate M1

With a real gamepad and nothing else (no keyboard, no mouse), ES-DE closed, RetroArch installed as ES-DE uses it (the
user's own; not an automated run, so no allow-list), the library created and imported (`retroverse-cli library create`,
`retroverse-cli import`), the Release `retroverse.exe` started by hand:

1. Within 5 s of the start, Y opens the quick catalog on its first page (no "Apertura del catalogo..." left showing).
2. The D-pad and the stick move through the list (held: it scrolls, the card follows when the selection rests); the
   triggers page, the stick clicks jump to the first and last game.
3. Search: **not possible with the pad alone yet** (the search field takes text from a keyboard; there is no on-screen
   keyboard): type a word with the keyboard for this step, or skip it (a BACKLOG candidate; the gate decides).
4. The shoulders change the system filter; the chip row shows the active system; the list and the count follow.
5. A game whose emulator is missing or needs configuration shows it on its card; A on it shows why and launches nothing;
   B dismisses the panel.
6. A on a ready game (RetroArch): "Gioco in corso", RetroArch in front, playable with the same pad; Retroverse renders
   nothing meanwhile.
7. Quit RetroArch from its menu with the pad: Retroverse is back in front, in fullscreen, with the same game selected,
   the same filter and search, the walker where it was; `retroverse-cli sessions` lists the session as returned.
8. Launch again; while the game runs, Alt+Tab or the Windows key back to Retroverse: after 1.5 s "Gioco in corso" asks;
   "Torna al gioco" goes back to it.
9. Quit Retroverse while the game runs (its window closed, or Back to the end): the game keeps running; the next start
   of Retroverse (or `retroverse-cli sessions`) shows the session left running (quit), never interrupted while the app
   runs.
10. B walks back out of the catalog to the room; the walker moves with the stick.

## Decisions taken within the step

1. **The catalog worker is a job queue on one thread** owning the writer and a query object; the main thread's query
   object is created by the worker and handed over. A job per request kind (list, readiness, resolution, session);
   only the latest list matters (a queued query is replaced, older answers dropped by serial).
2. **Readiness per game is lazy**: resolved on the worker when its row is first read, only where it can differ, rather
   than for every game when the catalog opens (90,246 games, with disk reads for `%INJECT%` files and folder games); a
   row shows its system's meanwhile, and a game whose own differs makes the rows read again (`reread()`, a new model
   operation: the same list, other data).
3. **The card's readiness comes with the row** (`CatalogEntry::readiness`, `blocker`, `blockerDetail`, `verified`),
   beside A1's state keys, which the rows and the mock keep; the blocker's own words show as data on a "Dettagli" line.
4. **Avvia's decision comes from the worker** and is applied on the next frame: the request frame is the frame after
   Avvia, and "Gioco in corso" is still presented before the emulator starts.
5. **Lockstep only with `--fixed-dt`**: scripted runs wait between frames for the worker so that their frames are
   reproducible; interactive runs never wait.
6. **`AppCatalog`** abstracts the mock and the ES-DE catalog for ProductUi; A1's mock planning moved into
   `MockAppCatalog` unchanged.
7. **The opening's errors name user folders**: logged at debug level, the state at info.
8. **The app keeps its own copy of the CLI's launch library** (`CatalogLaunchLibrary` mirrors `launch_text.hpp`'s
   `LaunchLibrary` and `cmd_launch.cpp`'s `resolveCatalogGame`, which are private to `rv_cli_core`): moving them to a
   shared library is a candidate for the merge.
9. **Input scripts can type text**, so that the smoke tests search through the real search field.
10. **The frame and page times are in the run report** (`TimeSummary` with p50, p95, p99, max), for NFR-01's
    measurement without scraping the log.

## Notes for step 13r (the refresh on a worker)

- The worker is a job loop (`catalog_worker.cpp`): an import is another job (`ImportService` with a cancel flag that
  `stop()` sets, beside the query interrupt), run with the same writer and import lock; after its commit the systems'
  readiness must be resolved again (the opening's part after `systems()`), the readiness cache cleared
  (`CatalogDatabaseSource`), and the main thread told (a `CatalogChanges` flag) to `QuickCatalogModel::refresh()`: the
  list is materialized again (the kept list is dropped by `data_version`) and the selection restored by GameId (tested
  here with an import made outside the app).
- A long import blocks the queue: queries and session writes would wait behind it; run it in slices or let lists jump
  the queue (they only read).
- `IdentityPairing` and `runQuickHashPass()` (step 11b's notes) take turns on the same worker.

## Notes for step 14 (Gate M1)

- The GPU cases `gpu.catalog: ...` run with the gpu presets; NFR-01 runs through `./scripts/real-library.ps1 -Filter
  '[real-library][app]'` (Release by default; build Release first), on a quiet machine for the gate's figures.
- The manual pad-only checklist above, with RetroArch; decide on the pad-only search (an on-screen keyboard is not
  in M1's plan).

## For the merge

- **CLAUDE.md**, "Running the app": without `--mock-catalog` the quick catalog shows the ES-DE catalog of the user data
  directory (`library.json` and the database made by `retroverse-cli`; the notices otherwise); runs with `--fixed-dt` wait
  between frames for the catalog worker (lockstep); input scripts' `"text"`; the run report's catalog (`notice`,
  `library`, `systems`, `search`, `filter`, `times`, `pages`, `frames`, `interruptedSessions`) and launch records
  (`before`, `after`, `session`). GPU smoke tests: the catalog cases (`tests/smoke/test_catalog.cpp`, the fixture
  imported with `retroverse-cli`). Layout: `rv_app_catalog` (GPU-free, `tools`-free) between `rv_app_cli` and `rv_app`;
  `rv_app` uses `rv_catalog`, `rv_esde`, `rv_catalog_db`. Real-library runs: `-Filter '[real-library][app]'` (NFR-01).
- **docs/architecture.md**: the app layer's catalog worker and `AppCatalog`; the threading diagram above.
- **docs/roadmap.md**: step 13 done, with NFR-01's figures.
- **ADR-0008 §5** is met as written (the worker owns the writer, the main thread's reader fetches pages, `sqlite3_interrupt`
  at shutdown); a sentence may record the lockstep of `--fixed-dt` runs.
- **ADR-0005**: the card shows the launch readiness and the blocker in its own words on a "Dettagli" line.
- **BACKLOG** candidates: an on-screen keyboard (pad-only search); the app's log before the data-directory guard (the CLI
  logs to stderr until it passes); one shared launch library for the CLI and the app (`CatalogLaunchLibrary`); a refresh
  when another process imports (13r, a file watcher in I7); the card's "Vedi media" (I3/I5); readiness for every game in
  bulk if a filter "ready only" is wanted.
