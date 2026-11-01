# I7 step 2: the wizard and `rv_library`

Roadmap row: I7 step 2. Decision: [ADR-0018](../decisions/ADR-0018-daily-use.md) §4, §12, §13, §15. Plan: [plan.md](plan.md).

## What was built

- **`rv_library`** (`src/library`): the one launch library (`LaunchLibrary`, the CLI's and the app's copies gone), a
  library made (`draftLibrary`, `createLibrary`), where a data directory stands (`libraryStage`), a library read
  (`previewLibrary`, `previewCoverage`), the browser's listings (`listDrives`, `listFolders`, `checkAccess`). `library
  create`, `import --dry-run`, `profiles`, `launch`, `assign` and the catalog worker read through it; the CLI's tests did
  not change.
- **The wizard** (`ui::WizardModel`, `ui::FolderBrowserModel`, `ui::WizardScreen`, `assets/ui/wizard.rml`,
  `app::WizardFlow`): seven pages by the pad alone, clicks pressing what the keys press. Page 5 says that ES-DE's commands
  work as they are and goes on: step 4 fills it.
- **The run's start:** `wizardStart()` decides from the options and `libraryStage()`. While it runs, `PendingAppCatalog`
  stands for the catalog and the test room shows behind it without its notice. Page 4 writes `library.json`, then
  `ProductUi::replaceCatalog()` starts `EsdeAppCatalog` with `createDatabase` and the refresh runs with the trigger
  `wizard`. Page 7 starts the world **in place**: `startWorldInPlace()` writes the settings, drops the test room's session
  and makes a new one, whose first commit replaces the scene. The ADR's fallback was not needed.
- **The notices** (`world_notice.rml`, `quick_catalog.rml`) name the wizard beside the CLI. The other notices that name
  the CLI (`assign`, `db check`) wait for the screens that replace them (steps 4, 8).
- **The report's** `wizard`, and a refresh's trigger `wizard`.

## Decisions and departures

- **No catalog worker before page 4** is kept by a stand-in, `PendingAppCatalog`, since `ProductUi` holds a catalog from
  its creation. `replaceCatalog()` makes the list again over the new source.
- **The wizard's Back is the menu's level:** `ProductUi::menuOpen()` is true while it shows and `menuBack()` is its Back,
  so `BackAction` and its table did not change.
- **A database no import finished in counts as no catalog** (`LibraryStage::NoCatalog`), which ADR-0018 §4 does not say:
  a wizard killed during page 4 leaves an empty database, and without this the next start would show an empty building.
  The check reads the database read-only and never makes it.
- **`--esde-home <dir>` takes the ES-DE home or the folder that holds it**, so a fixture's root (`--home`'s meaning) and
  the real `RV_REAL_ESDE_HOME` both work; `<dir>\ES-DE` decides. A folder chosen in the browser for the home is read the
  same way.
- **`--wizard` without `--esde-home` exits 2 in every run**, interactive too; `--esde-home` needs `--wizard` and
  `--esde-install` needs `--esde-home`. An interactive first run needs neither.
- **The ROM folder out of reach stops page 2** (`rom-root-unavailable`); the media folder does not: it may not exist yet,
  and the preview counts what it finds.
- **`library.json` changed after it was written** (a resumed wizard, or Riprova after a folder chosen) is replaced only once
  `backUpFile` kept a copy in `config/backups`; a copy that fails leaves the file and keeps what was chosen for the next
  try. A library.json another process made meanwhile is kept (`library-exists`). The flag is cleared only after the write.
  This is the one write under `config` besides `library.json` itself.
- **A lockstep run waits for a page's work**, up to 30 minutes rather than the worker's 10 s: the real library's first
  import takes minutes, and a script's presses must find their page ready.
- **A second A on the way in presses nothing** while page 2 or 3 is still looking: Avanti pressed twice would otherwise
  open a browser, or go back and cancel the read. Clicks are aimed and still work.
- **Back on the first page rests on Esci** rather than quitting. The window's close still quits.
- **A job's thread holds only a shared box**; the flow's destructor waits 5 s and leaves one a dead volume holds, as the
  catalog worker does. Each listing has its thread, so a volume that hangs never holds up the next folder.
- **Page 7's tutorial is static text** naming the shipped pad map; step 3's hint bar may take it over.

## What was checked

The figures of the runs are in the step's report; the cases are `tests/unit/library/test_library.cpp`,
`tests/unit/ui/test_wizard_model.cpp`, `test_folder_browser_model.cpp`, `tests/unit/app/test_wizard_flow.cpp`,
`tests/unit/app/test_wizard_real.cpp` (`[real-library][wizard_real]`) and `tests/smoke/test_wizard.cpp` (`[wizard]`; `RV_SHOTS_DIR`
with `[wizard_shots]` writes each page, the browser and an error).
