# I7 step 6: statistics, the status, Informazioni

Roadmap row: I7 step 6. Decision: [ADR-0018](../decisions/ADR-0018-daily-use.md) §9, §11, §5, §12, §13, §15, §16. Plan:
[plan.md](plan.md).

## What was built

- **The game's status** (`catalog::GameStatus`, `rv/catalog/game_status.hpp`): `rv_game_status` (launch area step 3, schema
  version 3), `SessionStore::gameStatus()`, `setGameStatus()`; `gameStatusOf()` is the one rule (set, else ES-DE's
  `completed`, else a `Returned` session, else `Da iniziare`).
- **The statistics** (`catalog::PlayStatistics`, `rv/catalog/statistics.hpp`): `SqliteCatalogQuery::playStatistics()`
  (`stats_query.cpp`) reads every game once in one statement on a reader; `catalog::buildPlayStatistics()` adds the rows up,
  so the rules are tested without a database. `retroverse-cli stats [--limit] [--json]` prints the same.
- **The screen** (`ui::StatisticsModel`, `ui::StatisticsScreen`, `assets/ui/statistics.rml`, `app::statistics_data`): the
  menu's Statistiche, between Emulatori and Esci: Riepilogo, Più giocati, Recenti, Per sistema, Per stato. The worker answers
  (`CatalogWorker::statistics()`), so the panel opens at once on "Lettura del catalogo..." and no frame waits. A catalog
  with nothing played says so, as does no catalog.
- **Informazioni's rows** (`ui::InfoRow`, `ui::GameInfoControls`, `GameScreensPanel`): Stato, Emulatore, then each other version
  listed, in a region below the text that is always in view. Down past the text's end reaches them, Up from the first
  returns to it; each takes a click. The rows come from the worker (`CatalogWorker::gameControls()`), and do not show in a
  catalog that has none (the mock's), where only the versions are rows.
- **A title's emulators** (`EmulatorsModel::openForGame()`, `ChoiceChange::Kind::SaveGame`, `ClearGame`): Emulatore opens step
  4's screen on the title's system. Prova runs that title, what is chosen is its alone (`LaunchTestChoice::forGame`, built at
  step 4, reached here), "Torna alla scelta del sistema" takes it away, Back returns to Informazioni.
- **A version's launch:** Activate on its row is `launch()`, what Avvia is, so its session is under its own GameId, with the
  game-session panel over Informazioni.
- **The report's** `statistics`; `screens` gains `infoTextCut` and the events `row`, `controls`, `status`, `emulators`,
  `version-launch`, `version-busy`.

## Decisions and departures

- **The branch carries step 4.** The plan's row needs it and it was not on `main`, so `i7/step-4` was merged in at cb400b66
  (with step 5's conflicts resolved on both sides). Its `rv_launch_program_choices` is schema 2; this step's table is 3.
- **No third-party notices in Informazioni.** The step's brief asked for them "as the ADR says", but ADR-0018 §11 does not:
  Informazioni is the game's panel there, and the roadmap gives "third-party notices shipped" to a later iteration. Nothing
  was added; a screen of licences needs its own decision.
- **The rows stand in a region of their own**, not at the foot of the scrolled text as before: a long description would
  put Stato out of reach of every press but a page. The versions moved there with them, so `game-info-rows` scrolls to
  the focused row. Up, Down and the pages on the text work as before while no row has the focus.
- **`Attempts` are `Crashed` and `LeftRunning` only.** `Interrupted`, `Blocked` and `FailedToStart` are neither: the first's
  end is unknown and the others never ran a game.
- **Hours are by the session's start and end**, as §9 says, not `run_ms`: "from the launch to the detected return".
- **A clock moved back counts for nothing:** a session's time is never below zero, so it cannot subtract from the others.
- **A game the library lost keeps its hours**, under its system, with its path for a title; it leaves the statuses and the
  recent games, which are about the library. Hidden games do too, and their hours stay.
- **Availability is not looked at:** an unplugged drive must not change what was played. The recent games therefore differ
  from the directory's, which lists only what can launch, and favourites do not come first: this is a history.
- **Stato writes a row at every step,** so a game that was stepped never derives again. §11 says only "stepping the four
  statuses"; going back to derived would need a fifth state on the row.
- **Stato shows at once, the worker's answer has the last word:** two quick presses would otherwise step from the same value.
  A failed write says "non salvato".
- **Emulatore replaces Informazioni while it shows** rather than standing over it: `backAction()` would otherwise close
  Informazioni under the list (`PanelBack` comes before `MenuBack`).
- **`p95` within 100 ms holds for Release;** a Debug build's SQLite takes about 300 ms for it, and the case only reports that.
- **Sections have no focused row:** nothing here opens anything, so the screen is read, not walked.

## What was checked

The figures of the runs are in the step's report; the cases are `tests/unit/catalog_db/test_statistics.cpp`,
`tests/unit/ui/test_statistics_model.cpp`, the title's cases of `tests/unit/ui/test_emulators_model.cpp`,
`tests/unit/ui/test_system_menu.cpp` and `tests/smoke/test_statistics.cpp` (`[statistics]`; pictures: `[statistics_shots]` with
`RV_SHOTS_DIR`). By hand, in Release: `rv_tests.exe "[statistics_bench]"`, 40,000 games and 10,000 sessions.
