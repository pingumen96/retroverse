# I7 plan: M5 part 1, daily use

This file holds the I7 section, already pasted into `docs/roadmap.md`, the row for the iteration table, the order of the
steps, and the decisions made on the user's behalf. The decisions themselves are
[ADR-0018](../decisions/ADR-0018-daily-use.md).

## Iteration table row (replaces the I7 row)

| I7 | M5 part 1, daily use (ADR-0018): one menu, a settings screen over Retroverse's own `settings.json`, the first-run wizard from the pad, emulator management with launch tests, remapping and a hint bar from the active bindings, the on-screen keyboard, statistics and the game's status, the file watcher (games and media added in ES-DE appear without a restart), the media coverage report per system, the library screen | Gate Pad: `gate.ps1 -Gate Pad`, then a 30-minute controller-only session |

## Section for the roadmap

```markdown
## I7: M5 part 1, daily use

[ADR-0018](decisions/ADR-0018-daily-use.md), planned on November 1, 2026 after the user's "procedi con M5" (the
decisions taken on the user's behalf: [i7/plan.md](i7/plan.md)):
- **One menu**, on Start and Back's last level, in place of quitting.
- **Settings** in Retroverse's own `<user data>/settings.json` over the hand-written `config` files, which stay untouched.
- **The first-run wizard**, from the pad, ES-DE only, replacing `library create` and `import` by hand (they keep working).
- **Emulators:** every system's readiness, the choice for a system or a title, a program for a command that finds none,
  a launch test before a new choice counts.
- **Controls:** the hint bar from the active bindings, remapping, hotplug proven, the on-screen keyboard.
- **Statistics** from Retroverse's own sessions, estimated hours, the game's status.
- **The file watcher:** games and media added in ES-DE appear without a restart, read-only, debounced, idle during a game.
- **Coverage and the library:** the media coverage per system, sources, review rows, the mass-disappearance guard.

Every step delivers a screen or a behaviour to try, works from the pad alone, and appears on VR's panel. Automated runs
never discover the real ES-DE (the wizard needs `--wizard --esde-home`), and a launch test fails closed without
`RV_LAUNCH_ALLOWLIST`. Step notes in `docs/i7/`.

| # | Step | Needs | Proof | Status |
|---|---|---|---|---|
| 0 | ADR-0018, this plan | | The files exist | done |
| 1 | The menu and settings: `Action::Menu` in free roam, Back's last level the menu (`BackAction::MenuBack`, `ui::SystemMenuModel`, `system_menu.rml`, Esci with a confirmation), `core::SettingsFile` and `Paths::settingsFile()`, the overlay in `loadVideoSettings`, `loadWorldConfig`, the XR settings and `loadBindings`, `player.json` (`app::PlayerSettings`), the settings screen and its table of rows (`setting_rows.hpp`, `settings_model`, `settings.rml`) without the bindings, applied live and written when it closes | 0 | Unit: the layers' order, only the changed keys written, unknown keys kept, comments of `config` files untouched (their bytes equal), a broken file left out and backed up, a stray key never reaching a strict loader, Ripristina; `test_back_action`'s table. GPU (pad actions only): Start opens the menu, a volume and `reduceMotion` change at once and survive a restart, FOV changes the view, a lighting change commits the floor within 3 s, Back out to the world, Esci ends the run; the panel on `--xr-runtime fake`, its FOV and window rows hidden. Every golden 0 pixels apart with no `settings.json` | done |
| 2 | The wizard and `rv_library`: the module (the one launch library, `createLibrary`, `previewLibrary`; the CLI's `library create`, `import --dry-run` and `launch` and `CatalogLaunchLibrary` moved onto it), `wizard_model`, `folder_browser_model`, `wizard.rml`, the pages but Emulatori, `--wizard`, `--esde-home`, `--esde-install`, the database made and the first import through the refresh's progress, the world started in place, the notices no longer naming the CLI alone | 1 (beside 3) | Unit: the pages' states, resuming with `library.json` and no database, the data-directory guard, a folder browser over a vanished folder; the CLI's tests unchanged. GPU (`esde/building`, pad actions only): the seven pages to the lobby, the report's `wizard` counts equal the CLI's dry run; `--wizard` without `--esde-home` exits 2; a frame-limited run without `library.json` keeps today's test room and report. Real library (`real-library.ps1`, data under `build/`): the preview and the first import from `--esde-home`, 0 differences | done |
| 3 | Controls: `input::bindingLabel`, `ui::HintBar`, `assets/ui/key_labels.json`, every document's hints from the active map and the last device; remapping in the settings screen (capture, a clash judged per context, the swap, no lock-out, `--safe-bindings`); the notice "Controller scollegato"; FR-029 proven | 1 (beside 2) | Unit: labels for every key and button of `codes.hpp`, clashes per context, the actions that may never be left unbound, `applyOverrides` over a hand-written file. Platform: a virtual pad unplugged and back during a UI screen. GPU: a rebound activate works and the hints name it, on the keyboard and the pad; a report after `--safe-bindings`; the goldens 0 pixels apart, or regenerated on purpose where a hint's text changed | done |
| 4 | Emulators: `launchTable()`, the worker's jobs (the table, a pending choice, a test with it, stored choices), `rv_launch_program_choices` (launch area step 2), `EsdeCommandTranslator` taking a program first, `launchTestAllowed()`, `emulators_model`, `emulators.rml`, the wizard's page 5, SC-10's suggestion; `assign --emulator --program`; `gate.ps1` and `test.ps1` under the automation guard | 2 (beside 5) | Unit: the table against `retroverse-cli profiles`'s output on the fixtures, a pending choice stored only after a clean return, a stale label, a program refused by name and by the file it opens, a verification not counting for another program, the migration; `launchTestAllowed()`'s table. GPU with the fake emulator: a system requiring configuration repaired from the pad and launched (SC-10), its session `test`, a crash leaving the choice pending, an unattended run without the allow-list refused before anything starts. Real library: the 48 systems' readiness as in `profiles`, nothing launched, 0 differences; a real launch test is the user's | |
| 5 | The on-screen keyboard: `keyboard_model`, `keyboard.rml`, opened on the search field by the pad, typing through `typeText`, accents, delete, confirm, clicks for the ray | 1 (beside 4) | Unit: the grid's moves, wrapping, delete, confirm, UTF-8 of the accented letters. GPU: a search typed with pad actions alone narrows the list, the keyboard not cut at 1280 x 720 and at text scale 125%; the same by a scripted ray with `--xr-runtime fake`. M1's one keyboard step of the manual session closes | done |
| 6 | Statistics, the status, Informazioni: `rv_game_status` (launch area step 2), `rv/catalog/statistics.hpp`, `playStatistics()`, `retroverse-cli stats`, `statistics_model`, `statistics.rml`; Informazioni's rows: another version launched, the title's emulator, the status | 4 (beside 7) | Unit: hours from sessions by outcome (tests, crashes and left-running out), per system and status, the derived status, an alias keeping its status, `db rebuild-index` leaving it; p95 within 100 ms on a generated 10,000 sessions. GPU: after a fake emulator's launch the screen counts it and the word *stimate* shows; another version launched from Informazioni, its session under its own GameId; a title's emulator stored | |
| 7 | The file watcher: `core::IChangeWatcher`, `platform_win::DirectoryWatcher`, `esde::changeScope()`, `app::LibraryWatch`, `RefreshRequest` with `watcher`, partial refreshes, the existence poll, safe removal, the 30 s fallback, another process's import, `--watch-library` and `--no-watch-library`, the report's `watcher` | 1, step 13r's refresh (beside 6) | Unit: the policy's table on a fake clock (quiet, burst, a game, the editor, overflow, a volume lost and back), scope on hostile paths and an emulator's save, real notifications on a `TempDir`, a partial refresh leaving the other systems' rows byte-identical; a mock device notification closing a handle. GPU: a ROM copied into a materialized `esde/building` appears in the quick catalog within 10 s and in a new slot with 0 moves (SC-05, NFR-08), a cover added shows, nothing starts during the fake's game; the folder renamed away and back (SC-06: same ids). Real library: armed on the real roots for a minute, 0 differences; the biggest system's partial refresh timed; delivery on exFAT is the user's | |
| 8 | Coverage and the library: `mediaCoverage()`, `coverage_model`, `coverage.rml`, the verdicts' failures; `library_model`, `library.rml`, the roots' state and watch mode, the last import's issues, "Applica le rimozioni", review rows through `resolveReview()`, "Calcola impronte" on request | 4, 7 | Unit: `mediaCoverage()` equal to `measureCoverage()`'s two populations on the fixtures, a review resolved only to its candidates, the removals held until confirmed. GPU: both screens by pad actions, a review resolved, a hash pass cancelled and resumed. Real library: the figures within 0.1 point of `media --coverage`, the screen within 1 s, 0 differences | |
| 9 | Gate Pad: `scripts/gate.ps1 -Gate Pad` (and `-RealLibrary`), `docs/testing/gate-pad.md`, documentation | 1-8 | The gate passes; the user plays the 30-minute controller-only session, tries a ROM added and the drive unplugged by hand, and declares I7 done | |

Order: 1, then 2 beside 3, then 4 beside 5, then 6 beside 7, then 8, then 9.

### Gate Pad

`scripts/gate.ps1 -Gate Pad` runs what Gate M4 runs (the build matrix, the unit tests and the GPU smoke tests in Debug and
Release, every golden, the benchmark path with two clips), and golden G21, under `scripts/focus_watch.py` with
`RV_LAUNCH_ALLOWLIST` naming the build's fake emulator, and adds a row per I7 area counted from those runs: the menu and
settings, the wizard, emulators, controls, the keyboard, statistics, the watcher, coverage and the library; the
watcher's 10 s row; one pad-only script that crosses the menu, a setting, a search, the emulators, the statistics and a
launch. It prints the summary table (logs in `build/gate-pad`) and exits 1 when a step failed.

`-RealLibrary` adds, with ES-DE closed and `RV_AUDIO=null`, one `scripts/real-library.ps1` run with snapshots before and
after and 0 differences: the wizard's preview and first import under `build/`, the emulators' table of the 48 systems,
the coverage against `media --coverage`, the watcher armed, a partial refresh timed, and the rows Gate M4 runs. Nothing
launches.

`docs/testing/gate-pad.md` holds the figures, where ADR-0018 and I7's requirements are proven (FR-014, FR-020, FR-026,
FR-027, FR-029, FR-030, FR-033, NFR-05, NFR-08, SC-05, SC-06, SC-10), the follow-ups, and what to try.

**The user's session, 30 minutes with the pad alone, the keyboard and mouse put away:** first start on a fresh data
directory through the wizard; a real launch test; walking, the elevator, the directory, a search typed on the on-screen
keyboard, several games launched and returned from; settings changed and kept after a restart; the pad unplugged and
back; statistics; then, by hand and outside the session, a ROM added to the real folder, a scrape in ES-DE, and the
external drive ejected and returned.
```

## Acceptable performance rows to add

Added to the roadmap's table with the section (ADR-0018 §16):

| Measure | Acceptable |
|---|---|
| A game or medium added in ES-DE (NFR-05, the watcher) | in the quick catalog within 10 s of the file's arrival, local volume, nothing else running; in its room informative |
| A menu section's data (emulators, statistics, coverage) on the reference library | shown within 1 s of opening |

## Side by side: who owns what

At most two steps at once, each in its own worktree. Steps 1 and 9 run alone.

| Pair | Step | Owns |
|---|---|---|
| 2 + 3 | 2 | `src/library` (new), `tools/cli`'s `cmd_library`, `cmd_import`, `launch_text.hpp`, `catalog_launch_library`, `wizard_*`, `folder_browser_*`, `wizard.rml`, `world_notice.rml`'s no-library lines |
| | 3 | `rv_input`, `hint_bar`, `key_labels.json`, the bindings part of `settings_*`, every document's hint lines, `gamepad_manager`, `default_bindings.json` left unchanged |
| 4 + 5 | 4 | `emulators_*`, `launch_guard`, `launch_translation`, `schema_launch.cpp`, `session_store`, the worker's emulator jobs, `scripts/test.ps1` |
| | 5 | `keyboard_*`, `keyboard.rml`, the search field in `quick_catalog_screen.cpp`, `quick_catalog.rml` |
| 6 + 7 | 6 | `rv/catalog/statistics.hpp`, `game_status.hpp`, `stats_query.cpp`, `statistics_*`, `game_info_*`, `cmd_stats.cpp` |
| | 7 | `change_watcher.hpp`, `directory_watcher.*`, `change_scope.*`, `library_watch.*`, `library_refresh.*`, `EsdeLibrarySourceOptions`'s users |

**Files several steps touch,** merged by hand, the second to merge rebasing: `src/app/src/product_ui.{hpp,cpp}`,
`retroverse_app.cpp`, `catalog_worker.{hpp,cpp}`, `run_report.{hpp,cpp}`, `command_line.{hpp,cpp}`, `system_menu_*` (each
section adds its entry), `tools/cli/src/commands.hpp` and `command.cpp`, the CMake source lists, `scripts/gate.ps1`, and
the docs (`docs/roadmap.md`, `docs/reference/*`, `docs/architecture.md`, `BACKLOG.md`).

Steps 4 and 6 each add an `AreaStep` to the launch area's schema, step 6's after step 4's.

## Decisions made on the user's behalf

The scope below was set for I7 on the user's behalf after "procedi con M5"; the choices made while designing it follow.

**Scope as given**
- The wizard imports from ES-DE only; the legacy and manual-folder modes stay out, the provider and scraper step is
  dropped.
- The wizard starts when a data directory has no library, works from the pad alone, and `library create` and `import`
  keep working.
- Emulator management shows whether ES-DE's command resolves, "Richiede configurazione launcher" when not, keeps the
  choice per system or title, offers a launch test; argv through `rv_launcher` only; choices in Retroverse's data.
- The settings screen covers `reduceMotion`, FOV, sensitivity, lighting, volume, mute, display mode and remapping.
- Statistics: sessions, recent games, per system and status, estimated hours; personal data apart from ES-DE's.
- The watcher reads only, debounces, does nothing during a game, and never moves a placement.
- The coverage report sits in the product UI over what `media --coverage` computes.
- Multiple profiles, kiosk, autostart and gamelist write-back are out of I7.

**Choices made while designing**
- **`<user data>/settings.json`, a separate file over `config`,** rather than editing commented files: no comment-keeping
  writer exists, and hand-written files stay byte-identical.
- **`settings.json` wins over a hand-written key,** or the screen would seem to do nothing; "Ripristina" gives the
  hand-written value back and the start logs the shadowing.
- **Only display keys are on the screen:** nothing the generator reads can be changed there.
- **A new `assets/config/player.json`** for FOV, sensitivities, inversion, display mode and text scale, which had no file.
- **FOV stays the scene's until the user sets it,** so no golden moves.
- **Back's last level opens the menu instead of quitting:** with a pad, B in free roam ended the program at once, which a
  30-minute pad session cannot live with. Fourteen input scripts name Back and `test_back_action.cpp` asserts Quit, so
  step 1 reviews them.
- **One menu with a document per section,** not more tabs in the directory.
- **Exclusive fullscreen is not offered:** the return from an emulator was proven on borderless only.
- **The wizard imports before the emulators page,** the reverse of specification §5.1, because a launch test needs a game
  in the catalog.
- **The wizard has no world presets, seed or density:** one preset exists; the page offers lighting and `reduceMotion`.
- **Frame-limited runs never start the wizard or discover ES-DE by themselves.** Without this, every smoke case with an
  empty data directory would have read the machine's real library.
- **Paths are browsed, not typed.**
- **ES-DE's own commands stay Ready without a test** (ADR-0008 §8), `Verificato` shown once one returns. **Only a choice
  made in the new screen waits for a test,** with "Salva senza provare" as the way out. This reads
  specification §11.3's "test di lancio" narrowly: requiring it everywhere would mark 41 working systems as not ready.
- **A program can be chosen for an ES-DE command** in a new state table, as FR-020's recovery for "emulator not found"; it
  is the first thing to drop if step 4 runs long, since hand-written `emulators.json` profiles cover it.
- **No profile editor:** a user's own profile stays a hand-written `emulators.json` entry.
- **A launch test fails closed** in unattended runs, beyond what the guard does.
- **Key names live in `assets/ui/key_labels.json`,** the one string table outside RML.
- **XR controllers are not remapped in the app.**
- **The game's status is derived until set,** so nobody must tag a library of 90,000 games; ES-DE's `completed` counts.
- **Hours add `Returned` sessions only.**
- **ES-DE's `playcount` and `playtime` stay out of the statistics.**
- **Informazioni, not the card, gets the versions, the title's emulator and the status:** the card is at its height limit.
- **Refreshes triggered by the watcher are partial,** by system: a whole-library refresh costs 5.6 s warm, which leaves
  too little of NFR-05's 10 s.
- **Debounce: 2 s quiet, at most 30 s, then one refresh a minute** during a burst.
- **The watcher filters by extension,** so an emulator's saves next to a ROM wake nothing.
- **The watcher closes its handles on a query to remove the device,** so the external drive stays ejectable.
- **The fallback polls at the system folders' level every 30 s,** never a subtree, and only where notifications fail.
- **The watcher is off in automated runs** unless asked, so existing runs stay deterministic.
- **"Calcola impronte" is on request:** 45 minutes of reads on an external drive should not start by themselves.
- **Coverage reads the catalog's rows,** not the folders, so the screen opens within 1 s; two populations of the CLI's
  four.
- **No new golden:** these are panels.
- **The gate is named Pad** and reruns Gate M4's checks and G21.

## Risks

- **Delivery on exFAT is unproven.** No automated test may write into the real ROM root, and no scratch exFAT volume
  exists here, so `ReadDirectoryChangesW` on the real drive is proven only by arming it. If the user's trial shows no
  notifications, the 30 s fallback catches the system folders and the 10 s row holds only on NTFS; the row would then
  be recorded so.
- **The partial refresh is unmeasured.** `EsdeLibrarySourceOptions::systems`' comment forbids a narrowed snapshot for a
  whole import; narrowing both together follows `ImportOptions::systems`' comment but no code does it yet. Step 7 proves
  the other systems' rows byte-identical. A large system (psx: about 12,800 disc members) may still take seconds.
- **The building job after a refresh** reads every system; the room's update may take much longer than the catalog's,
  hence the room's half of the row is informative until step 7 measures it.
- **Moving from the test room into the world in place** (wizard page 7) touches `retroverse_app.cpp`'s start, written for
  a decision made once. The fallback is a page asking for a restart.
- **Back no longer quitting** touches scripted runs (fourteen input scripts name Back) and may change how some runs end.
- **Parallel steps share `product_ui.cpp` and `catalog_worker.cpp`,** both large. Each step adds its section beside the
  others; the second to merge rebases.
- **A launch test is a real launch** with real emulators: the user's first test may stall on an emulator's dialog.
  ADR-0006's confirmation applies; nothing is ever killed.
- **Device notifications on a message-only window** are new here and unchecked against an external drive.
  If they fail, the drive cannot be ejected while Retroverse runs, and `--no-watch-library` is the workaround.
- **Text scale above 100%** may cut panels written at 100%. Step 1 dropped 150% for this reason (ADR-0018 §3, amended
  2026-11-01): the screen offers 100 and 125%.
- **The pad session's 30 minutes** may expose gaps no script does (the settle delay on a TV, BACKLOG). Those become
  follow-ups, not new I7 steps, unless they block the session.
- **Pending trials:** none: the user declared M3, M4 and I10 done on November 1, 2026.