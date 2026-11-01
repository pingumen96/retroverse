# I7 step 1: the menu and the settings screen

Roadmap row: I7 step 1. Decision: [ADR-0018](../decisions/ADR-0018-daily-use.md) §1, §2, §3, §12, §13, §15, §16. Plan:
[plan.md](plan.md).

## What was built

- **The menu** (`ui::SystemMenuModel`, `ui::SystemMenuScreen`, `assets/ui/system_menu.rml`): Riprendi, Impostazioni,
  Esci, which asks once with Annulla focused. Emulatori, Statistiche, Copertura media and Libreria are absent: each step
  adds its button (`system_menu_*`, as the plan's "files several steps touch" has it). `Action::Menu` opens it in free
  roam, and `BackAction::OpenMenu`, Back's last level where a product UI exists; `BackAction::MenuBack` walks out of it.
  The model viewer and the asset preview still quit. It never opens in the editor, during a launch or a ride, or over
  another panel; the screens keep playing, ducked as under the directory.
- **`settings.json`** (`core::SettingsFile`, `Paths::settingsFile()`): over the shipped defaults and the user's `config`
  files in `loadVideoSettings`, `world::loadWorldConfig`, `xr::loadXrSettings`, `loadBindings` and the new
  `app::loadPlayerSettings` (`assets/config/player.json`). Each loader names its keys
  (`videoSettingKeys()`, `worldSettingKeys()`, `xrSettingKeys()`, `playerSettingKeys()`, the actions' names), and finds
  the file beside the config folder it was handed, so the CLI and every job read it with no change at their call.
- **The settings screen** (`app::settingRows()`, `app::SettingsStore`, `ui::SettingsModel`, `ui::SettingsScreen`,
  `assets/ui/settings.rml`): the rows of ADR-0018 §3 but the bindings, in five groups, each by the pad alone and by
  clicks that mirror it. Applied live, written off the main thread when it closes, before a launch and at the end of the
  run.
- **What a change does** (`RetroverseApp::State::settingChanged`): the lens, the look's scales, the text scale, the
  window, the mixer's master and mute at once; `ScreenService::setVideo()` binds the screens afresh for clips on or off
  and another programme; `WorldSession::setReduceMotion()`; `WorldSession::commitFloorAgain()` for the lighting profile
  and the CRT effect.
- **The report's** `menu` and `settings`, and `world.liveUpdates` with `trigger` `look`.

## Decisions and departures

- **Two Back levels, not one.** ADR-0018 §1 names `BackAction::MenuBack`. Back's last level and the menu's own Back are
  different things to do, so `MenuBack` is the second and `OpenMenu` the first; `backAction()` still answers `Quit`
  without a product UI.
- **A pad's B does not stop at the captured mouse.** Esc still frees it first, as ADR-0018 §1 says of the keyboard; B
  goes on to the menu, or a pad-only session would press it once for nothing.
- **The CRT effect commits the floor too** (ADR-0018 §3 says only the lighting profile does). The looks are chosen when
  a scene's materials are made (`applyScreenLooks()`), so applying one at once would mean a second path through the
  material registry for one row; the floor's commit reaches the same result in the same row's time. A run without a
  floor (`--scene`) says "Al prossimo avvio".
- **The lighting commit reads the file.** The floor's job reads `roomLightingProfile()`, which goes through the loaders,
  so the row writes `settings.json` first (off the main thread) and asks the commit when the write ended; a lockstep run
  waits for that write, so that the commit's frame repeats. The job is the ride's (`requestFloor()` with the plan shown,
  not the building job), and `applyLive()` forces a rebuild, since the items compare equal.
- **Sound turned on in a run that started off** comes at the next start: no engine exists then, and the screens' service
  holds the one it was given. The row says so. Turning it off in a run that has one mutes its mix.
- **Ripristina tutto asks once**, which ADR-0018 §2 does not say, and it removes every row's key, across all groups:
  keys no row names stay. On the keyboard the group and Ripristina have no key until step 3 (`ui_info` and the filters
  are pad-only by default): the tabs and the row's button take clicks, and the hints leave them out.
- **`settings.json` is read by each loader** from beside the folder it is given, rather than passed in, so that no call
  changes: an empty folder gives an empty document. An unreadable file is backed up only by the app, once at its start:
  `load()` writes nothing, because `retroverse-cli` reads through the same loaders.
- **A value equal to the layer below removes its key,** so the file holds only what differs.
- **A hidden window never changes mode:** the row's value is stored and reported, the window stays, so that no automated
  run takes the screen.

## What was checked

The figures of the runs are in the step's report; the cases are `tests/unit/core/test_settings_file.cpp`,
`tests/unit/app/test_settings_store.cpp`, `tests/unit/ui/test_system_menu.cpp`, `test_settings_model.cpp`,
`tests/unit/app/test_back_action.cpp` and `tests/smoke/test_menu_settings.cpp` (`[menu_settings]`).

Fourteen input scripts name Back, as the plan warned. Three press the world's `back` (`world_focus_enter`,
`world_focus_steps`, `world_inspect`), each to leave Focus, so none relied on it quitting; so do the C++ scripts of
`test_cabinets.cpp`, `test_world_launch.cpp` and `test_world_elevator.cpp`. The others press `ui_back` inside a panel.
None changed.

## Text scale

`UiView::setTextScale()` scales every dp, so at 150% a 1280 x 720 window on this machine's 125% display is 683 x 384 dp.
See the step's report for what that does to the panels of earlier steps.
