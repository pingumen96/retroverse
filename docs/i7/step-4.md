# I7 step 4: the emulators screen and launch tests

Roadmap row: I7 step 4. Decision: [ADR-0018](../decisions/ADR-0018-daily-use.md) §5, §6, §4, §13, §15, §16. Plan:
[plan.md](plan.md).

## What was built

- **`library::launchTable()`** (`rv/library/launch_table.hpp`): `retroverse-cli profiles` reads through it, the screen through
  `systemLaunchRow()` on the worker (the catalog's `<alternativeEmulator>`, no gamelist read). `suggestedOptions()` is
  SC-10's suggestion, the ready first; `sampleGames()` the sample, the most recently played, then by title.
- **The programs chosen** (`rv_launch_program_choices`, launch area step 2, schema version 2): `SessionStore::programChoices()`,
  `setProgramChoice()`, `clearProgramChoice()`; `esde::findEmulator()` looks there first (`FoundLocation::rule`
  `retroverse`); `esde::checkProgramChoice()`; `retroverse-cli assign --emulator <name> [--program <exe> | --clear]`.
- **The screen** (`ui::EmulatorsModel`, `ui::EmulatorsScreen`, `assets/ui/emulators.rml`, `app::emulators_data`,
  `app::FolderLister`): the systems, a system's options, a choice (Prova, Scegli il programma, Salva senza provare,
  Annulla, the game of the test), the folder browser choosing a `.exe` (`FolderBrowserModel::openForProgram()`,
  `library::listPrograms()`). It opens from the menu (Emulatori, between Impostazioni and Esci) and over the wizard's page
  5, which shows its counts and stays skippable. Hints through `hint-action`, clicks pressing what the keys press.
- **The worker's jobs** (`emulatorTable()`, `sampleGames()`, `changeChoice()`, `resolve()` with a `LaunchTestChoice`): a test
  launches with the pending choice, its session records `test`, and `finishTest()` stores the choice only after
  `catalog::returnedNormally()`. What is stored changes the readiness: the systems are resolved again without reading ES-DE's
  configuration (`CatalogResults::resolved`, a new generation).
- **`launcher::launchTestAllowed()`**: asked by the app before anything is resolved, and by `launch --test`.
- **A verification counts for its program:** `VerifiedPrograms` holds the program each ran, `verifiedWith()` compares it
  with the one found now.
- **The report's** `emulators`. The notices that named `assign` (card, quick catalog, directory, the game-session panel)
  name Emulatori.

## Decisions and departures

- **`scripts/test.ps1` and `gate.ps1` are unchanged,** though the plan's row names them: the unit runs must not set
  `RV_LAUNCH_ALLOWLIST` (the process-runner tests start renamed fakes, BACKLOG). What guards a launch test is
  `launchTestAllowed()`, and every GPU case already has the list. ADR-0018 §6's allow-list for Gate Pad's whole run is step
  9's, with those runner tests clearing it for themselves.
- **A launch test is offered only for a choice made in the screen** (the orchestrator's decision), so an ES-DE command that
  stands as it is shows `Pronto` until an ordinary launch verifies it. Picking the command already in use is a choice like
  any other: its test stores it.
- **A program is stored at once,** not after a test: it answers "emulator not found" for every system that uses that
  emulator, and the same screen then offers the test of the choice. `ADR-0018 §5` says only the choice waits. A program
  gone later blocks with "emulator not found" and never falls back to the find rules: the emulator must not change behind
  the user's back, as with a stored choice. "Togli il programma scelto" removes it.
- **A program also repairs `find-rules-missing`:** the choice is looked at before the rules exist, so a `%EMULATOR_X%` no
  rule defines takes one. The screen offers the button for "emulator not found" alone, the ADR's case; `assign --emulator` for
  any.
- **"Torna alla scelta di ES-DE"** is added to the ADR: without it a stored choice could only be replaced.
- **An unlabelled command** has no label to store (`SessionStore` refuses an empty value): choosing it removes the stored
  choice, which is what makes it stand.
- **A test forces its choice over the game's own,** by the game's slot of the resolution, with the session's source set
  back to the system's: what it verifies must be what ran, whatever the sample's `<altemulator>`.
- **The game of a test:** Gioco di prova steps through at most 200 games (`emulators::kMaxSampleGames`), not the whole
  system ("any game can be picked"): the model holds rows, not a catalog. Enough for a machine's sake; a search is parked.
- **Per title** is built in the worker (`LaunchTestChoice::forGame`, the stored game choice), not reachable yet:
  Informazioni opens it at step 6, as the plan has it.
- **The wizard's page 5 is a button, not the screen in the page:** the screen is one document, and the wizard's Avanti must
  stay one press away. The table is asked when the page shows, with its catalog open; a screen over the wizard hides it.
- **A system's rows name no program** (a choice does): with paths the list was three lines a row, and a real system has
  twenty commands. A list that runs past the panel counts as cut (`EmulatorsScreen::textCut()`): at 125% one did, unseen by
  the text check, until the views were placed by the panel's edges; the scale case holds 16 extra commands for it.
- **A choice stored from the screen is `Non verificato`** in the list; `Pronto` stays ES-DE's own commands (§5).
- **`retroverse-cli launch --test`** fails closed too (exit 1, nothing recorded): ADR-0018 §6 speaks of a launch test, not
  of the app's.
- **Schema version 2** is the first migration of a real database: a catalog of an older build migrates when opened for
  writing; a read-only open says `db migrate`, as designed. Step 6's table adds its own `AreaStep`.

## What was checked

The figures of the runs are in the step's report; the cases are `tests/unit/ui/test_emulators_model.cpp`,
`tests/unit/app/test_emulator_jobs.cpp`, `tests/unit/library/test_launch_table.cpp`, the program cases of
`tests/unit/esde/test_launch_translation.cpp`, `tests/unit/catalog_db/test_session_store.cpp`,
`tests/unit/cli/test_cli_launch.cpp`, and `tests/smoke/test_emulators.cpp` (`[emulators]`; pictures: `[emulators_shots]` with
`RV_SHOTS_DIR`). Real library: `real-library.ps1 -Filter '[real-library][emulators_real]'`
(`tests/unit/library/test_launch_table_real.cpp`). A real launch test is the user's, at Gate Pad.
