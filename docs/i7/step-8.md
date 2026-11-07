# I7 step 8: coverage and the library

Roadmap row: I7 step 8. Built on 2026-11-02, rebased onto main (I11 steps 0-2 and 11-13, the branding) and finished on
2026-11-07. Decision: [ADR-0018](../decisions/ADR-0018-daily-use.md) §1, §10, §14, §15, §16. Plan:
[plan.md](plan.md).

## What was built

- **The coverage of the catalog's rows** (`SqliteCatalogQuery::mediaCoverage()`, `coverage_query.cpp`): one statement
  groups idx_media's kinds per game (a bit per kind) over idx_games' Game rows, counted into `catalog::MediaCoverage`'s
  `Games` and `ScrapedGames` populations; no folder is read. Beside it `importIssues()` (idx_import_issues) and
  `quickHashCounts()` (the present files, and those with a current quick hash).
- **Copertura media** (`ui::CoverageModel`, `ui::CoverageScreen`, `assets/ui/coverage.rml`, `app::coverageContentOf()`): the
  menu's fifth entry. Per system the games and the shares of covers, 3D boxes, screenshots, marquees and videos, worst
  first; Left, Right and the shoulders switch between every game and the games a gamelist names; Activate opens a system's
  twelve kinds in both populations (with what is missing), and its clips known to fail. The note says to scrape in ES-DE,
  then refresh the library (ADR-0003).
- **The clips' failures**: the failure verdicts of `cache/media/videos.json` (I5 step 7), read on the worker
  (`media::readVideoVerdicts`), counted per system and listed with their verdict in a system's details.
- **Libreria** (`ui::LibraryModel`, `ui::LibraryScreen`, `assets/ui/library.rml`, `app::libraryContentOf()`,
  `app::libraryLiveOf()`): the menu's sixth entry, five sections. Origini: the ES-DE home, the ROM root, each system folder
  outside it and the media root, each reachable or not (one look on the worker) and how the watcher follows it (step 7's
  modes; "Non sorvegliata" when it is off), the last import, Aggiorna libreria with its progress. Problemi: the last
  import's issues grouped by kind (thirteen groups, the worst severity and the systems named). Rimozioni: the systems the
  mass-disappearance guard holds, with their figures, and "Applica le rimozioni". Da verificare: the open review rows.
  Impronte: the quick hashes the catalog has, and "Calcola impronte".
- **The worker's jobs** (`CatalogWorker::coverage()`, `libraryStatus()`, `resolveReview()`, `quickHash()`,
  `cancelQuickHash()`): the screens' reads; a review resolved through `catalog::resolveReview()`, never forced; the
  quick-hash pass through `catalog::runQuickHashPass()` on the worker's writer, the main thread's requests answered between
  two files (no batch is open there).
- **The report's** `coverage` and `library` (`run_report.hpp`); the menu has seven entries.

## Decisions and departures

- **Worst first is the mean of the five kinds the list shows** (covers, 3D boxes, screenshots, marquees, videos), compared
  as fractions of integers, then the full name: the kinds the building and its screens use. The twelve kinds are a
  system's details.
- **The clips' failures are the verdicts as written**: nothing on the media root is looked at, so a clip replaced since its
  verdict still shows until the screens or `media --check-videos` judge it again. Reading its file is what the screens'
  service does.
- **The import's issues are grouped by kind,** not listed one by one: on the reference library they are thousands, mostly
  ES-DE quirks followed. Their reasons are English sentences for the log; the screen names each group in Italian and the
  systems it concerns.
- **The systems held are those of the last import's guard issue** (idx_import_issues, which a partial import keeps for the
  other systems); their figures come from the last import record when it covered the system, else they are not shown.
- **"Applica le rimozioni" asks, Annulla focused,** then asks a refresh of the held systems alone that accepts the removals
  (trigger `user`); an A pressed twice by mistake cancels. The watcher's refreshes still never accept them (FR-033).
- **Pairing a review asks first** (Annulla focused), since an alias cannot be undone; "Tieni separati" does not. ADR-0018
  §14 asks for the candidates only; the confirmation is an addition.
- **Only candidates with an identity whose file is present are offered**: `resolveReview()` refuses the others. They show,
  marked "Non più presente". The worker clears `force` whatever it is asked: the CLI's `--force` stays the only way past
  the candidates.
- **A pairing moves an identity, so the lists and the building follow** as after another process's import
  (`CatalogResults::reviewed`, `RefreshState::reviewsPaired`, which the world session adds to step 7's count).
- **A refresh asked during the quick-hash pass runs first**: the pass stops after its file, commits what it hashed, lets
  the refresh run and goes on after it from what is left, so the watcher's 10 s never waits for a 45-minute pass.
  Quitting stops it the same way.
- **A lockstep run hashes one file per frame** (`EsdeAppCatalog::Options::hashStepped`, `AppCatalog::frameStarted()`): the
  worker waits for the next frame between two files and the frame waits for that file, so a scripted stop lands on the
  same file every time; an interactive run never waits.
- **The last import shows its day only**, through the directory's `machineLocalDate()`.
- **Menu scripts move**: Copertura media and Libreria stand between Statistiche and Esci, so "Esci asks once" and its
  picture press Down six times (four before). The VR case that points at Impostazioni is unchanged (the panel's top is).
- **No new golden:** these are panels (ADR-0018 §15).

## What was checked

Figures (this machine):

| What | Figure |
|---|---|
| Real library (Release, 2026-11-07): coverage on the worker | 536 ms (549 ms from the request), 48 systems, 0 clips known to fail |
| Real library: the library screen on the worker | 206 ms (213 ms from the request): 3 folders, 13 issue groups, 0 systems held, 0 reviews, 0 of 103,937 files with a quick hash |
| Real library: shares compared with `media --coverage` | 1,140 shares, the largest difference 0.0000 points |
| Real library: the snapshots before and after | 0 differences among 542,121 entries |
| Debug, for comparison | coverage 2,268 ms, library 1,167 ms (the 1 s limit is the product's, measured in Release) |

The cases are `tests/unit/catalog_db/test_coverage_query.cpp` (`mediaCoverage()` equal to `measureCoverage()`'s two
populations on the standard, snes-room, building and video-verdicts fixtures; the import issues; the quick hashes'
counts), `tests/unit/ui/test_coverage_model.cpp`, `tests/unit/ui/test_library_model.cpp` (the removals held until
Applica is confirmed, a review resolved only to its candidates after a confirmation, the pass started, stopped and
resumed), `tests/unit/app/test_library_jobs.cpp` (the worker: the coverage with the clips' failures, the folders and the
quick hashes, a review refused for another game and paired with its candidate, the guard's removals applied only by a
refresh that accepts them, a pass stopped after its third file and resumed), `tests/unit/ui/test_system_menu.cpp` and
`tests/smoke/test_library_screens.cpp` (`[library_screens]`; pictures: `[library_shots]` with `RV_SHOTS_DIR`). Real
library: `real-library.ps1 -Filter '[real-library][coverage_real]'` (`tests/unit/app/test_coverage_real.cpp`).

## Not proven

- **"Applica le rimozioni" and a review on the real library:** it holds no system and no review row today, so both are
  proven on the fixtures alone.
- **The whole-library "Calcola impronte":** started and stopped on the fixtures; the real pass (about 45 minutes, I2
  step 11b's estimate) is the user's to run from the screen.
- **A pad-only session of both screens** by the user.
