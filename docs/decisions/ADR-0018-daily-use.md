# ADR-0018: Daily use: the menu, settings, the first run, emulators, statistics, the file watcher

- Status: accepted (the user's "procedi con M5"; the scope was set when I7 was planned, and the choices made on the
  user's behalf are listed in [i7/plan.md](../i7/plan.md), "Decisions made on the user's behalf")
- Date: 2026-11-01
- Amends:
  - ADR-0005: the on-screen keyboard and the hint bar are built; the keys' names live in a table under `assets/ui` (§7);
  - ADR-0008 §6: `library.json` is also written by the wizard (§4); §8: a new choice waits for a launch test, a program
    can be chosen for an ES-DE command (§5); the launch area's schema moves to version 2 (§5, §9);
  - ADR-0012 §3: `reduceMotion` also comes from the settings screen (§3);
  - ADR-0016 §4: `<user data>/settings.json` goes over `video.json`, and the mute kept there is the state a run starts
    with (§2, §3); §9: `toggle_mute` can take a pad button by remapping (§7);
  - ADR-0017 §8: `settings.json` goes over `xr.json` (§2, §12);
  - specification §5.1 (the wizard's order and content, §4), §12.1 (Back's last level, §1), §13.2 (one menu, §1);
  - CLAUDE.md and `docs/reference/` as recorded under "Rules that change".
- Relates to: ADR-0003 (no scraper: no provider step), ADR-0006 (launches), ADR-0009 (quick hashes), ADR-0013 §10 (the
  directory), ADR-0015 §2 (world data beside the plan, `config` read-only); specification §5, §11, §12, §13.4, §18.1,
  §23; roadmap I7 (Gate Pad)

## Context

M5 asks that Retroverse can replace ES-DE in daily use (specification §22). What is left for I7 is what a person meets
outside the building: the first start, a game that does not launch, a setting to change, what they played, a game that
scraping added an hour ago. Requirements: FR-014, FR-020, FR-026, FR-027, FR-029, FR-030, FR-033, NFR-05, NFR-08,
SC-05, SC-06, SC-10.

What the code has and lacks (the survey of 2026-11-01):

- **Configuration.** Five files are read under `<user data>/config`, each with its own loader: `bindings.json`
  (`input::ActionMap::applyOverrides`, per action), `world.json` (`world::loadWorldConfig`, strict), `video.json`
  (`app::loadVideoSettings`, merged key by key, strict), `xr.json`, `emulators.json`, plus `crt_presets.json`. Comments
  are allowed in all of them, and no writer that keeps comments exists (BACKLOG, "Saving the developer overlay's
  edits"). FOV, sensitivity and the display mode are in no file: the camera's lens comes from the scene,
  `input::InputSettings` from the developer overlay, the window from `--fullscreen` and Alt+Enter.
- **Back.** `BackAction::Quit` is Back's last level, so the pad's B in free roam ends the program at once.
  `Action::Menu` (pad Start) is bound and opens nothing.
- **The first run.** `library create` is CLI code (`cmd_library.cpp`) over `esde::discoverEsde`, `esde::profileOf` and
  `esde::writeLibraryFile`. Without `library.json` the app decides at once for the test room and a notice naming the
  CLI (`roomWithoutCatalog()`), and smoke cases assert that. `CatalogWorker` opens an existing database only.
- **Launches.** `catalog::resolveLaunch`, `LaunchReadiness`, `SessionStore`'s choices per game and system and
  `rv_launch_verifications` exist; `retroverse-cli profiles`, `assign` and `launch --test` use them. The launch library
  has two copies (`app::CatalogLaunchLibrary`, the CLI's `LaunchLibrary`, BACKLOG). `RV_LAUNCH_ALLOWLIST` unset means
  the guard does nothing.
- **Sessions.** Every attempt is in `rv_play_sessions`; `favoritesAndRecent()` serves the directory. `idx_games` holds
  ES-DE's `completed`, `play_count` and `play_time`, read-only. No game status of Retroverse's exists.
- **Refresh.** `CatalogWorker::refresh()` runs the import in slices, and a live update reaches the room without moving
  anything. `ImportOptions::systems` makes an import partial, and `EsdeLibrarySourceOptions::systems` narrows the
  snapshot. A whole-library refresh that changes nothing takes 5.6 s warm on the reference library (Gate M1).
- **The ROM root here** is exFAT on another drive, a volume that can go away; the media root and the ES-DE home are on
  NTFS. Retroverse must never write there, so no test can add a file to the real folders.
- **The UI.** Each screen is a GPU-free model in `rv_ui_model`, a `*_screen.cpp` in `rv_ui` and a document under
  `assets/ui`, driven by `ui::UiCommand`s, with clicks mirroring the keys. `ProductUi` holds them. In VR the whole
  canvas is drawn onto `XrUi`'s panel, its ray the UI's mouse. The hints are hard-coded in each RML document.

## Decision

### 1. One menu

- **The way in.** `Action::Menu` (pad Start) in free roam, and Back's last level. Back's last level is now the menu, not
  `Quit`, wherever a product UI exists; the keyboard reaches it with Esc once the mouse is free. Runs without a product
  UI (the model viewer, the asset preview) still quit. Closing the window still quits at once.
- **What it holds.** Riprendi, Impostazioni, Emulatori, Statistiche, Copertura media, Libreria, Esci. Esci asks once;
  a game left running stays running (ADR-0006).
- **Shape.** `ui::SystemMenuModel` and one document per section, each a screen of its own: a section's Back returns to
  the menu, the menu's Back to the world. Each step then owns its document, as each directory tab could not.
- **Input.** `BackAction::MenuBack` joins the Back levels; the menu has the UI's input like the directory. It does not
  open in the editor, during a launch or a ride. Nothing pauses behind it but the walker: the screens keep playing,
  ducked as under the directory.
- **The directory stays the building's panel.** World actions (Modifiche) stay there; the menu holds what is not about
  the building.

### 2. `settings.json`: what the app writes

**`<user data>/config` stays the user's: Retroverse never writes a file there** (the exception, `library.json`, is §4's).
What the settings screen changes goes to one file of Retroverse's own, `<user data>/settings.json`
(`core::Paths::settingsFile()`), never under ES-DE's folders. This was the simplest choice that keeps hand-written
overrides working: nothing edits a commented file, so no comment-preserving writer is needed.

- **Layers,** weakest first: the shipped defaults, the user's `config/<file>.json`, `settings.json`'s section. A key
  the screen never touched is decided by the hand-written file as today.
- **Format.** Plain JSON, no comments: `{"schemaVersion": 1, "video": {...}, "world": {...}, "bindings": {...},
  "player": {...}, "xr": {...}}`, each section shaped like the file it overlays, holding **only the keys the user
  changed**.
- **Reading.** `core::SettingsFile::section(name, allowedKeys)`. Each loader names the keys the screen may set and
  merges those alone, so a stray key can never make a strict loader refuse a file. `loadVideoSettings`,
  `loadWorldConfig`, `xr` settings and `loadBindings` gain this one step; `retroverse-cli` reads through the same
  loaders. An unreadable file is left out whole, logged, kept aside as a backup (`core::backUpFile`) and replaced at the
  next write.
- **Writing.** `core::writeFileAtomic`, off the main thread, when a screen closes and before a launch. Keys, sections
  and values the running build does not know are written back as read. Tests write only under their `--data-dir`.
- **Restoring.** A row's "Ripristina" removes its key, so the layer below shows through; the screen's "Ripristina tutto"
  removes the section's keys. A key shadowing a hand-written value is logged once at the start.
- **No key that shapes a plan is on the screen.** Seeds, density, passages, `sharedRoomMinTitles` stay hand-written, in
  `world.json`: settings change how the world is shown, never what it holds (NFR-08, the building stamp unchanged).
- **The player's file.** FOV, sensitivities, inversion, the display mode and the text scale have no file today:
  `assets/config/player.json` holds them (schema 1), `config/player.json` overrides it, `app::PlayerSettings` reads it.
  Its `horizontalFovDeg` is `null` by default: the scene's lens stays, so every golden is unchanged.

### 3. The settings screen

Rows from a table (`app::settingRows()`): key, kind (toggle, choice, slider), range, whether it applies at once. The
Italian label of each is the document's.

| Group | Rows |
|---|---|
| Schermo | window or borderless fullscreen; text scale (100, 125, 150%); FOV (70 to 110 degrees, or the scene's) |
| Audio | master and video volume, mute, sound off (`audio.mode`) |
| Mondo | lighting profile, `reduceMotion`, clips on or off (`video.enabled`), CRT effect, the TVs' programme (`hero.playlist` without `explicit`) |
| Controlli | mouse and stick sensitivity, invert Y, the bindings (§7) |
| VR | start in the headset, turn, vignette, walking relative to, posture, quality, refresh rate |

- **Applied at once:** everything except the lighting profile, which commits the floor again within the save row's 3 s,
  and the VR session's rows, which apply at the next session and say so. The mute row is what a run starts with; F8
  changes the run only, as ADR-0016 §4 had it.
- **Exclusive fullscreen is not offered:** `WindowHandover` restores borderless only, and borderless is what the return
  from an emulator was proven on (ADR-0006). The effects and ambience volumes wait for I8's sounds.

### 4. The wizard

Specification §5.1, with the mode choice reduced to `Importa da ES-DE`, the provider step gone (ADR-0003) and the world
presets gone (one preset exists; others are beyond v1). The order changes: **the import comes before the emulators**,
because a launch test needs a game in the catalog.

| # | Page | What it does |
|---|---|---|
| 1 | Benvenuto | What Retroverse does and does not touch |
| 2 | Rilevamento | `esde::discoverEsde`, the ES-DE home, the ROM and media roots with an access test; a folder browser changes any of them; the data-directory guard |
| 3 | Anteprima | The snapshot's counts: systems, games, media, unrecognized entries, errors, coverage in brief (`import --dry-run`'s reading) |
| 4 | Importazione | `library.json` written, the database made, the first import through the refresh's progress |
| 5 | Emulatori | The emulators screen (§5), skippable |
| 6 | Aspetto | Lighting profile, `reduceMotion` |
| 7 | Il mondo | The building job, then the controls' tutorial as one page of the pad's buttons |

- **When it starts.** In an interactive run with `roomMode` when the data directory has no `library.json` or no
  database (the latter resumes at page 3). `retroverse-cli library create` and `import` keep working, and a data
  directory they prepared never sees the wizard.
- **Never by itself in an automated run.** **A frame-limited run keeps today's test room and notice**, so that no
  smoke case changes and, above all, no automated run ever discovers the machine's real ES-DE. The wizard runs in such a
  run only with `--wizard` and `--esde-home <dir>` (with `--esde-install <dir>`): discovery and the system probe are then
  skipped and `--wizard` without `--esde-home` exits 2.
- **Paths are browsed, not typed.** `ui::FolderBrowserModel` lists drives and folders on a worker, so a slow or
  vanished volume never blocks a frame; nothing there needs the keyboard.
- **The code.** `library create`'s logic moves to the new module `rv_library` (§13), which the CLI then calls.
- **The app's phases.** Until page 4 no catalog worker exists; the test room shows behind the panel, without its notice.
  Page 4 starts `EsdeAppCatalog` with `createDatabase` set (the migration, then the opening) and runs a refresh with the
  trigger `wizard`. Page 7 starts the world on the same catalog. Should moving from the test room into the world in
  place prove too large, the fallback is a closing page that asks for a restart.
- **An interrupted wizard** needs no record of its own: `library.json` and the database say where it stands. Pages 5
  and 6 are skippable because the menu has them.
- **The notices** (`world_notice.rml`, `quick_catalog.rml`) stop naming the CLI as the only way.

### 5. Emulator management

`Emulatori` lists the systems with games: readiness, what launches them and why (`ChoiceSource`), whether it was
verified, the first blocker. A system opens every ES-DE command and every Retroverse profile that serves it, with
their own readiness (what `retroverse-cli profiles` prints, moved into `rv_library::launchTable()`), first the ready
ones (SC-10's suggestion).

- **Readiness and verification stay ADR-0008 §8's.** ES-DE's own commands stay Ready without a test: they are what the
  user already plays with. The screen shows `Pronto`, or `Verificato` once a launch returned normally.
- **A choice made here waits for its test (narrowing §8's departure from the specification, SC-10).** It is pending
  until a launch test returned normally; the test launches with the pending choice, and only then is it stored
  (`SessionStore::setSystemChoice`, `setGameChoice`). "Salva senza provare" stores it anyway, shown as not verified, for
  a system whose emulator cannot be tried now. The choice is never written to ES-DE's files.
- **A program for a command that does not find one.** `Scegli il programma` browses for a `.exe`, for "emulator not
  found". It is stored per `%EMULATOR_X%` in a new state table `rv_launch_program_choices`, and
  `esde::EsdeCommandTranslator` looks there before the find rules (`FoundLocation::rule` `retroverse`). The path is still
  refused by `isRefusedProgram()` by the name found and the file it opens, and must pass `validateLaunchRequest`. A
  verification counts only for the program it ran (`LaunchVerification::program`). `retroverse-cli assign --emulator
  <name> --program <exe>` does the same.
- **Never a shell string.** Argv through `rv_launcher` and `Win32ProcessRunner`. Batch files stay refused (BACKLOG).
- **Per title.** Informazioni names the title's choice and opens the same list (§11).
- **The launch test** is a real launch of a sample game (the system's most recently played available game, else its
  first by title; any game can be picked), in the normal flow: decoders stopped, session recorded with `test` set,
  return to the screen with its verdict.

### 6. Launch tests and automated runs

A launch test is a real launch, so it **fails closed**: `launcher::launchTestAllowed()` refuses one in an unattended run
(frame-limited, or `RV_LAUNCH_BACKGROUND` set) while `RV_LAUNCH_ALLOWLIST` is unset, with its own outcome, before
anything is resolved. The guard then applies as to every launch: the build's `rv_fake_emulator.exe` alone in tests,
DuckStation never, RetroArch only isolated. The fixtures' find rules name the fake. `[real-library]` runs resolve and
never launch. `scripts/gate.ps1 -Gate Pad` sets the allow-list for its whole run, closing that BACKLOG item (the two
runner tests that start unlisted files clear it for themselves).

### 7. Controls: the hint bar, remapping, hotplug

- **Hints from the active map.** `input::bindingLabel(action, device)` and `ui::HintBar` give each screen's hints from
  the `ActionMap` and the last device used (`ActionState`'s), replacing the letters written in the RML. A hint's verb
  stays in the document; only the key's name is data.
- **The keys' names** are a string table, `assets/ui/key_labels.json` (Italian): one name per key, which `data-if` lines
  cannot hold. Pad buttons show A, B, X, Y and LB, RB as today: text, no logo.
- **Remapping.** Controlli lists the actions a user may bind, world and UI groups apart: a press is captured from the raw
  input. A clash is judged within a context (world and UI share buttons on purpose: `input_context.hpp`) and offers a
  swap. Stored in `settings.json`'s `bindings`, applied by `applyOverrides` after the user's file, so a hand-written
  action the screen did not touch stays.
- **No lock-out.** UiActivate, UiBack and Menu always keep a pad button and a key; "Ripristina predefiniti" is always
  reachable, and starting with `--safe-bindings` ignores the section.
- **`toggle_mute`** can take a pad button here; the shipped map stays without one.
- **Hotplug (FR-029).** `GamepadManager` handles it. I7 adds the world notice "Controller scollegato" and proves a
  reconnection during a UI screen and across a game with a virtual pad (`rv_platform_tests`).
- **VR controllers** keep `xr_bindings.json`: not remapped in the app (§12).

### 8. The on-screen keyboard

`ui::KeyboardModel` and `assets/ui/keyboard.rml`, a panel that opens on a text field when Activate is pressed from the
pad (a physical keyboard types as today). A grid with the Italian accented letters, digits, space, delete and confirm;
the D-pad and stick move, A types, X deletes, B closes, Start confirms. It types through the path `ProductUi::typeText`
already uses, into the quick catalog's search, its only field. It takes clicks, so the VR ray types too.

### 9. Statistics and the game's status

- **The screen.** Statistiche: sessions, estimated hours, the most played, the most recent, per system and per status.
  It says *ore stimate*: from the launch to the detected return (specification §13.4).
- **What counts.** `Returned` sessions, by their own start and end; never `test` sessions; `Crashed` and `LeftRunning`
  listed as attempts and not added to hours, their end not being the game's.
- **Separate from ES-DE.** Figures come from `rv_play_sessions` alone. ES-DE's `playcount` and `playtime` are not added
  in: they are another program's count of other launches. ES-DE's `lastplayed` stays in the recent games as it is.
- **The status (`Da iniziare`, `In corso`, `Completato`, `Abbandonato`).** A new state table `rv_game_status`,
  single-column reference to the identity, so `putAlias()` re-points it and `db rebuild-index` never touches it. A game
  with no row derives its status: ES-DE's `completed` gives `Completato`, a returned session `In corso`, else `Da
  iniziare`. Set in Informazioni (§11).
- **Queries.** `catalog::PlayStatistics` (`rv/catalog/statistics.hpp`), `SqliteCatalogQuery::playStatistics()` on the
  worker, `retroverse-cli stats`.

### 10. The file watcher

**What it watches:** every folder of `esde::readOnlyFolders()`' roots that matter: the ROM root, each system folder
outside it, the media root, and the ES-DE home's `gamelists`, `custom_systems` and `es_settings.xml`. It only reads:
directory handles with `FILE_LIST_DIRECTORY` and full sharing.

- **The port and the thread.** `core::IChangeWatcher` (`change_watcher.hpp`), `platform_win::DirectoryWatcher` one
  thread with overlapped `ReadDirectoryChangesW` per root at below-normal priority, `FakeChangeWatcher` for tests. It
  never calls SDL but the wake.
- **Which changes matter.** `esde::changeScope()` maps a path to a system, or to nothing: a ROM folder's file only with
  one of the system's extensions, a media folder's only with a media extension, `gamelist.xml` by its folder. So an
  emulator's save or state next to a ROM wakes nothing.
- **The policy.** `app::LibraryWatch`, GPU-free, on an injected clock, in `rv_app_cli`:
  - a change opens a window that ends after **2 s quiet**, or **30 s** after its first change while changes keep coming
    (ES-DE scraping thousands of files);
  - then at most one refresh every 60 s while the burst lasts;
  - **while a game runs, in the editor or during a ride nothing starts**: changes are remembered and the window opens
    after the return;
  - a buffer overflow or an unmapped change widens the refresh to every system.
- **The refresh.** `CatalogWorker::refresh()` takes a `RefreshRequest` (trigger, systems, accept removals; new trigger
  `watcher`). A refresh of some systems narrows the source and the import together, `EsdeLibrarySourceOptions::systems`
  with `ImportOptions::systems` (the source's header forbids the first alone). The live update is the one "Aggiorna
  libreria" runs: it moves nothing (NFR-08), and a watcher's refresh never accepts removals (FR-033).
- **A volume that goes.** A root's handle fails or is reported going: the watcher closes it, polls its existence every
  5 s and asks one refresh, which marks the systems source unavailable as the import does (SC-06: nothing deleted, the
  notice `Origine non disponibile`). When it returns, it arms again and asks a refresh.
- **Safe removal.** An open handle would keep Windows from ejecting the drive. The thread owns a message-only window
  and registers each handle (`RegisterDeviceNotificationW`, `DBT_DEVTYP_HANDLE`): on `DBT_DEVICEQUERYREMOVE` it closes
  the handle at once and goes to the existence poll.
- **The fallback.** A root where `ReadDirectoryChangesW` is refused (a network share, a file system that does not
  support it) is polled every 30 s at its system folders' level: their times and the gamelists' size and time, no
  subtree walk. It wakes the disk, so it never runs while notifications work. NFR-05 is not promised there, and the
  screen Libreria says which mode each root is in.
- **Another process's import** (BACKLOG): the worker compares the newest `rv_imports` row every 5 s while idle and, when
  it changed, asks the same lists again and the building job.
- **Off in automated runs** unless `--watch-library` asks; `--no-watch-library` turns it off in an interactive run.

### 11. Informazioni: versions, the title's emulator, the status

Informazioni gains focusable rows below its scrolled text: each other version of the title (`VersionGroups`), whose
Activate launches that version as Avvia launches a game; `Emulatore`, opening §5's list for this title; `Stato`,
stepping the four statuses. The game's card keeps its buttons: Informazioni is one press away (X), and the card is at
its height limit.

### 12. VR

Every new screen is a `ProductUi` document, so it is on `XrUi`'s panel in VR with no new code; a row must take a click
as its key does (`DirectoryClick`'s rule), since the ray is the mouse there. The exceptions, each by design:

- **Hidden in a headset:** the window mode and FOV rows (the runtime decides the field).
- **Remapping** covers the pad and the keyboard. Touch and other controllers keep `xr_bindings.json`, which their
  runtimes take at the session's start; a pad in VR is remapped like any pad.
- **A launch test in VR** keeps the session and shows the game on the CRT (ADR-0017 §7).
- **`reduceMotion`** stays forced in VR.

### 13. Where things live

| Thing | Where |
|---|---|
| `settings.json`, the change-watcher port | `rv_core`: `settings_file.hpp`, `change_watcher.hpp` |
| Binding labels | `rv_input`: `binding_labels.hpp` |
| Launch-test permission | `rv_launcher`: `launch_guard.hpp` |
| Statistics and status domain | `rv_catalog`: `statistics.hpp`, `game_status.hpp` |
| Changed path to system, the program choice | `rv_esde`: `change_scope.hpp`, `launch_translation.hpp` |
| Tables, statistics and coverage queries | `rv_catalog_db`: launch area step 2, `stats_query.cpp`, `coverage_query.cpp` |
| **New module `rv_library`** (`src/library`, `rv::library`): the one launch library, library creation and preview, the launch table, the sample game | above `esde`, `catalog_db` and `platform_win`, below `rv_app_catalog` and `rv_cli_core`; `rv_forbid_link` adds RmlUi and `rv_world` |
| The directory watcher | `rv_platform_win`: `directory_watcher.hpp` |
| Screen models, hint bar | `rv_ui_model`: `system_menu_model`, `settings_model`, `wizard_model`, `folder_browser_model`, `emulators_model`, `statistics_model`, `coverage_model`, `library_model`, `keyboard_model`, `hint_bar` |
| Screens, documents | `rv_ui`, `assets/ui` |
| Rows, player settings, the watch policy | `rv_app_cli`: `setting_rows.hpp`, `player_settings.hpp`, `library_watch.hpp` |
| Worker jobs, the wizard's flow | `rv_app_catalog`: `catalog_worker`, `wizard_flow` |
| Drawing them | `rv_app`: `product_ui`, `retroverse_app` |

### 14. Coverage and the Libreria screen

- **Copertura media.** A table per system and kind, worst first, over the catalog's own rows:
  `SqliteCatalogQuery::mediaCoverage()` groups `idx_media` over `idx_games` into `catalog::MediaCoverage`'s `Games` and
  `ScrapedGames` populations, so opening it scans no folder. It adds the clips' failures from `cache/media/videos.json`
  and says what to do: scrape in ES-DE (ADR-0003).
- **Libreria.** "Aggiorna libreria", each root's state and watch mode, the last import's issues, systems held by the
  mass-disappearance guard with "Applica le rimozioni" after a confirmation, review rows resolved through
  `catalog::resolveReview()` offering their candidates only, and "Calcola impronte": the quick-hash pass on the worker
  on request, cancellable and resumable, its progress shown.
- **The hashing pass is not automatic.** About 45 minutes of reads on the reference library's external drive, so it
  runs when asked; an idle-time pass stays in the BACKLOG.

### 15. Tests

- New GPU cases drive each screen with the pad's actions only, through input scripts, and assert the report's new
  `menu`, `settings`, `wizard`, `emulators`, `statistics`, `watcher` sections; each screen also runs once with `--vr
  --xr-runtime fake` and its ray where it matters.
- Text entry, menus and rows are checked for cut text as the card is (`cardSqueezed`'s rule).
- **No new golden is required:** the screens are panels, not rooms. Existing goldens must stay 0 pixels apart; a hint
  label that changes one regenerates it on purpose.
- The watcher's tests write only under `build/`: real `ReadDirectoryChangesW` on a `TempDir`, the policy on a fake clock,
  a smoke case adding a ROM to a materialized fixture. The real library's case arms the watcher and proves 0 differences;
  it cannot prove delivery on the real exFAT root, which only the user's own trial can.

### 16. Acceptable performance

New rows for the roadmap's table:

| Measure | Acceptable |
|---|---|
| A game or medium added in ES-DE (NFR-05, the watcher) | in the quick catalog within 10 s of the file's arrival, local volume, nothing else running; in its room informative |
| A menu section's data (emulators, statistics, coverage) on the reference library | shown within 1 s of opening |

Frames during a watcher's refresh are the existing row "while loading covers, refreshing or animating" (p99 at most
50 ms). A setting applied is the existing "edit shown" row; the lighting profile's commit is the "save" row.

### 17. Out of I7

| Item | Where it goes |
|---|---|
| Multiple library profiles (§5.2) | Beyond v1: one library per data directory; `--data-dir` is the workaround |
| Autostart with Windows (§5.3) | I9, the installer's option |
| Kiosk mode (§5.3) | I9, if it stays small: the menu without Esci; otherwise beyond v1 |
| Gamelist write-back (§4.6) | Beyond v1, as decided |
| Legacy and manual-folder wizard modes, provider step | Beyond v1; removed from scope (ADR-0003) |
| Other world presets | Beyond v1, as decided |

## Rules that change

- **`<user data>/config` remains read-only to Retroverse, but is no longer the last word:** `<user data>/settings.json`,
  written by the app, goes over it for the keys it holds.
- **`library.json`** is written by `retroverse-cli library create` and by the wizard.
- **A frame-limited run never discovers ES-DE:** the wizard needs `--wizard --esde-home`.
- **A launch test fails closed** in an unattended run without `RV_LAUNCH_ALLOWLIST`.
- **Product UI strings** are in the RML documents, and the keys' names in `assets/ui/key_labels.json`.
- **Back's last level** is the menu where a product UI exists.

## Consequences

**Gains**
- A person can connect ES-DE from the pad on the first start and use the app every day without a terminal.
- A game ES-DE adds shows up by itself; a game that does not launch can be repaired from the sofa.
- `config` stays safe to edit by hand, comments included, and the keys the screen never touched keep working.

**Costs**
- One more configuration layer: a hand-written value can be shadowed by a row changed earlier (logged, and "Ripristina"
  clears it).
- A new module, two state tables (a migration of the launch area), eight new documents.
- A background thread with open handles on the user's volumes, and the device-notification path that keeps them
  ejectable.
- Back no longer quits, so scripts that end a run with Back change at step 1.

**Deferred**
- An idle-time hashing pass, quick-catalog filters by missing media, a video in the quick catalog's card, editing the
  `explicit` playlist, high contrast, a catalog-only mode, localization tables: `BACKLOG.md`, "Parked at I7's planning".
