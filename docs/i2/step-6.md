# I2 step 6: UI actions in the action map and a virtualized quick catalog

Outcome: the product UI reads its input through the action map, like the 3D world: sixteen `ui_*` actions with their
defaults in `assets/config/default_bindings.json`, overridden by `<user data>/config/bindings.json`, resolved by
`input::InputSystem`, overridable by `--input-script`, and turned into `ui::UiCommand`s by `ui::NavInput`, which keeps
the I1 spike's stick hysteresis and repeat timing on top of them. The quick catalog is virtualized: `ui::CatalogSource`
(in `rv_ui_model`) pages a materialized list the way rv_catalog's `ICatalogQuery` does (ADR-0008 §5),
`ui::QuickCatalogModel` is a window over it, and `ui::QuickCatalogScreen` binds a fixed pool of 28 rows re-bound as the
window scrolls. `spike_ui` runs the screen over 100,000 generated games: its 128 scenario checks pass in Debug and
Release, an input script and a user's bindings file drive it in two more GPU checks. Quiet frames cost what they cost
in I1 (UI pass CPU p50 0.19 to 0.21 ms in the driver's fast mode, 0.50 to 0.53 ms in its slow one; GPU 0.135 ms), a
single tap too (3.3 to 3.5 ms); a held direction that scrolls the list costs 3.3 to 3.4 ms per step (I1's held focus
move: 1.2 to 1.5 ms), since every scroll step re-binds the rows and re-meshes their text (informative figures, taken
while other worktrees built).

## What was built

- `rv_input` (`action.hpp`, `action.cpp`, `action_map.cpp`): sixteen UI actions after the existing ones (whose names
  and indices are unchanged), `input::isUiAction()`, their defaults in `ActionMap::defaults()` and in
  `assets/config/default_bindings.json` (the test that the two agree covers them).
- `rv_ui_model`:
  - `ui_command.hpp`: six more commands (PageUp, PageDown, Home, End, FilterPrevious, FilterNext).
  - `nav_input.hpp`: `NavInput::update(const input::ActionState&, ...)` replaces the raw-input version and
    `NavBindings` is gone; `ui::actionFor(UiCommand)` names each command's action, `ui::commandKeys(ActionMap)` the
    keys bound to UI actions (what `UiView` keeps away from RmlUi). `DirectionRepeater` and `StickDirection` are
    unchanged; a second repeater runs the page commands.
  - `catalog_source.hpp` (new): `CatalogEntry` (moved here, with a `state` key), `isValidEntryId`, `PlatformFilter`,
    `CatalogSourceQuery`, the `CatalogSource` port.
  - `in_memory_catalog_source.hpp` (new): `InMemoryCatalogSource`, for tests and mock-ups.
  - `quick_catalog_model.hpp`: the model rewritten as a window over a `CatalogSource`.
  - `ui_assets.hpp`: the pool (`kPoolRows` 28, `kRowPrefix` "row-"), the list, the selected class, the scroll thumb,
    the state keys; `kGamePrefix` is gone (no element is named after a game any more).
- `rv_ui`: `UiView::setCommandKeys(bitset)` (from `commandKeys()`; the defaults' until set) and `UiView::caretKey()`
  (RmlUi's Home and End for a focused text field); `QuickCatalogScreen` with the pool of rows, `windowBinds()`.
- `assets/ui/quick_catalog.rml` and `.rcss`: the pool, the scroll indicator, the state labels, one-line titles; every
  string the screen shows is in the document now ("Tutte", "n.d." and the state labels moved out of C++).
- `spikes/ui`: the 100,000-game mock, the scenarios (`--scenario quick-catalog | user-bindings`), `--input-script`
  and `--expect-selection`, the new checks, three ctest cases, the fixture `spikes/ui/fixtures/user_bindings.json`.
- `tests/input_scripts/quick_catalog_select.json`; unit tests in `tests/unit/input` and `tests/unit/ui`.

## The UI actions and their default bindings

| Action | Keyboard | Pad | Command |
|---|---|---|---|
| `ui_up`, `ui_down`, `ui_left`, `ui_right` | arrows | D-pad | Up, Down, Left, Right (repeat while held) |
| `ui_nav_x`, `ui_nav_y` | | left stick X, Y (dead zone 0.15) | the same directions, through `StickDirection` |
| `ui_page_up`, `ui_page_down` | Page Up, Page Down | left, right trigger (dead zone 0.15) | PageUp, PageDown (repeat while held) |
| `ui_home`, `ui_end` | Home, End | left, right stick click | Home, End |
| `ui_activate` | Enter | A (south) | Activate |
| `ui_back` | Esc | B (east) | Back |
| `ui_media` | | Y (north) | Media ("Vedi media") |
| `ui_info` | | X (west) | Info ("Informazioni") |
| `ui_filter_previous`, `ui_filter_next` | | left, right shoulder | FilterPrevious, FilterNext |

- The pad's buttons, the D-pad and the stick are those of the I1 spike; the keys too (arrows, Enter, Esc). Letters,
  digits, the space bar, Backspace, Delete and Tab stay with RmlUi (text fields, its own tab navigation), so the
  keyboard has no default for Vedi media, Informazioni and the filters (the chips row reaches them).
- They share devices with the 3D world's actions (Enter is `confirm` and `ui_activate`, B is `back` and `ui_back`, the
  left stick `move_*` and `ui_nav_*`, its click `sprint` and `ui_home`): `InputSystem` resolves both, the application
  reads the ones of the context that has the input (app shell, step A1).
- A user's `bindings.json` overrides them per action like any other (`ActionMap::applyOverrides`): an action it lists
  takes its bindings (an empty list unbinds it), the others keep the defaults. `spike_ui` loads them with
  `app::loadBindings()`, as `retroverse.exe` does, and gives `UiView` the resulting command keys.
- `--input-script` sets them by name (`{"ui_down": 1}`, `{"ui_nav_y": -0.9}`), after the bindings, like every action:
  values in [-1, 1], a value at or past the press threshold holds a button action.

### How input reaches the screen

```text
SDL event --> UiView::handleEvent: mouse, wheel, text, IME, keys not bound to a UI action --> RmlUi context
          \-> platform::SdlInputCollector --> input::RawInputFrame
              --> input::InputSystem (default_bindings.json, the user's bindings.json over it) --> input::ActionState
              --> input::InputScriptPlayer (--input-script) --> ui::NavInput (stick hysteresis, repeats)
              --> UiCommandList --> QuickCatalogScreen::apply
```

`NavInput` reads the digital directions from `ui_up` ... `ui_right` (held, pressed, a tap within the frame: pressed
and released while not held) and the stick from the axes of `ui_nav_x` and `ui_nav_y`. The stick needs analog actions
of its own: an axis bound to `ui_down` with scale -1 would make `ui_down` held when the stick is pushed up
(`InputSystem` holds an action at |value| >= 0.5), and an analog sum mixed with the D-pad's would bypass the
hysteresis. The hysteresis thresholds (engage 0.55, release 0.35, axis switch 1.25) now apply to the actions' values,
which the bindings' dead zone shapes: with the default 0.15, the stick engages at 0.62 and releases at 0.45 of its
travel along an axis (0.55 and 0.35 in I1, without a dead zone). The page commands have a repeater of their own
(the last pulled trigger wins); the other commands fire on the press of their action, after the steps, in a fixed
order. `reset()` suppresses held directions and pages alike. One behavior is lost on the way: a button released and
pressed again between two frames reads as held at the action level (as for every action), so it no longer steps
again at once.

Back keeps its levels: `ui_back` gives the screen's Back (the focus history, then `QuickCatalogEvents::close`); in
`spike_ui` Back with the catalog closed quits, as before.

## The catalog source contract

`ui::CatalogSource` (`rv/ui/catalog_source.hpp`), main thread:

| Call | Does | ICatalogQuery's counterpart |
|---|---|---|
| `platforms()` | the platforms, one chip each, in the chips' order, with their entry counts | the systems list |
| `query(CatalogSourceQuery{text, platform})` | materializes the ordered list of the entries the query matches | `run(CatalogQuery)` |
| `count()` | the list's size | `list.size()` |
| `rows(first, n, out)` | the display rows of positions [first, first + n), cut at the end, one per position, reusing `out`'s storage | `fetch(pageOf(list, first, n))` |
| `position(id)` | an id's position in the list, if it holds it | `positionOf(list, id)` |

- Ids are the GameId text form ('g' and 16 hexadecimal digits), which `isValidEntryId` accepts.
- `CatalogEntry::state` is the availability or readiness slot: a key the document labels (`unavailable`: the file
  cannot be reached; `needs-configuration`: the command needs a profile; `emulator-missing`: emulator or core not
  found; empty: ready). The text stays in the RML (Italian strings never in C++).
- A position whose entry vanished after the list was materialized (an import ran) gives a row with its id only, so
  the window's rows stay aligned with positions; the next query materializes the list again.
- Errors (`IoError`, `Unavailable`, `InvalidArgument` for an unknown platform) leave the list empty (a query) or the
  rows empty (a fetch).
- The adapter of step 13 keeps the materialized `std::vector<GameId>` and an index of it for `position()` (query.hpp:
  "a view that asks often keeps its own index of the list"), maps a chip to `CatalogQuery::systems`, formats
  `CatalogRow` into `CatalogEntry` (year from the release date, players and the readiness key) and runs `query()` on
  the catalog worker when it may exceed ADR-0008's 5 ms.

`ui::InMemoryCatalogSource` keeps the entries' order, rejects invalid and repeated ids (the first entry of an id
stays; a sort, not the I1 model's quadratic scan), lists the platforms in the order the entries first name them, and
matches like the I1 model (every word, split at ASCII white space, must occur in the title, platform, developer,
publisher, genre or year, ASCII case folded; the catalog's FTS5 index also folds diacritics). A query that only adds
to the previous words (typing on) searches the previous list instead of the catalog. Positions are kept per entry
and ids are looked up in an index sorted by id: `position()` never scans. `replaceEntries()` stands for an import.

## The model: a window over the source

`QuickCatalogModel(source, windowRows = 10, scrollMargin = 2)` keeps the filters, the search, the list's count, the
selected position and id, the window's first position and its rows (`window()`), never state per entry of the
catalog.

- Window: `windowRows()` positions from `windowFirst()`; the selection is always inside. A move scrolls the window
  only when the selection would come closer than the margin (2 rows, at most half the window) to the edge it moves
  towards; at the list's ends the margins give way. A page (the window's rows) moves the selection and the window
  together, so the selection keeps its row; Home and End jump to the first and last game. A click (`selectPosition`)
  on a shown row never scrolls.
- Re-queries: a new list keeps the selected game when it still holds it (by id, `position()`), on the row of the
  window it had (within the margins). When the game is gone, a new search or filter starts at the first entry (as the
  I1 model did), and `refresh()` (the same query again: an import changed the catalog, ADR-0008 §5) takes the nearest
  position: the one the game had, or the last. `refresh()` also reads the platforms again; a filter whose platform
  disappeared falls back to every game.
- `setWindowRows()` (the screen's measurement) keeps the selection's row where the margins allow, so a size or text
  scale change and its reverse give back the same window from the margin rows and from the list's start.
- `cycleFilter(step)` wraps around the chips (the shoulders).
- Revisions: filters, list (each materialization), window (each fetch of its rows), selection (the selected game's
  id). `error()` keeps the last error of a query or fetch until one succeeds.

## The screen: a fixed pool of rows

- `quick_catalog.rml` binds `rows`, a `std::array` of 28 `RowData` (element ids `row-0` to `row-27`, fixed: focus
  targets and the history never name a game). The screen fills them from the model's window: `shown` for the slots
  that fit the list area (`data-if`), `present` for those with a game (blank rows past the end carry the class
  `vacant`: hidden, never focused), title, platform, year, state.
- How many rows fit is measured after each layout: the pitch between the first two rows, the first row's height and
  the list's content height (`measureRows`, three box reads per frame, no allocation). The list's height does not
  depend on its rows, so the measurement is stable. The model gets that window size (`setWindowRows`). Titles stay on
  one line (`white-space: nowrap`, `text-overflow: ellipsis`) so that every row has one height at any text scale.
- Rows that fit, measured on the reference machine (display at 125%, regular theme unless noted): 1920x1080 7 rows;
  1920x1080 at text scale 1.2, 5; 1280x720 (compact), 5; and with the dp ratio at 1.0 (`--ui-scale 0.8`): 1920x1080
  10, 2560x1440 15, 3840x2160 24. The pool covers the largest (a 2160-pixel-tall view at 100%) with a margin; taller
  views in dp (a text scale below 1) show 28 rows.
- The selected game's row carries the class `selected`, set on the two rows concerned when the selection's row
  changes (BACKLOG "Per-change cost under 1 ms": no re-evaluation of a bound array for a move within the window).
- Focus: in the list, Up and Down move the selection through the model and the focus follows it from row to row; when
  the window scrolls under the focused row (the selection on the margin row), the focus stays where it is. Up at the
  first game and Right leave by the region links, as before; landing in the list (a link, Back, show, a reload) always
  focuses the selected game's row. A click on a row selects its game; the wheel over the list moves the selection 3
  games per notch (the list no longer scrolls in RmlUi: `overflow: hidden`). Page moves, Home and End act in the list;
  Home and End move the search field's caret (the screen sends RmlUi's keys, since they are command keys now) and jump
  to the first and last chip in the chips row. The shoulders change the filter from any region; the active chip
  scrolls into view, and the focus follows it when it is in the chips row.
- The scroll indicator: a track down the list's right edge, its thumb as tall as the window's share of the list (at
  least 4%) and as far down as the window is down the list (percentages bound with `data-style-top` and
  `data-style-height`).
- Unchanged: the card and its settle delay (0.15 s), the chips row and its fades, the regions, the history, high
  contrast, the compact theme, hot reload.

## spike_ui

- `mockCatalog()`: 100,000 games generated from a fixed seed (SplitMix64): titles from Italian word lists (48 nouns, 40
  complements, one of 10 suffixes on 10 titles in 16; titles repeat across platforms, as in a real collection), twelve
  generic platform families, invented studios, genres and two-sentence descriptions, years 1977 to 2006, about 2%
  `unavailable` and 1.6% `needs-configuration` games, ids in the GameId text form (a hash of the index). Sorted as the
  catalog sorts (folded title, then id). The marker, the game in the middle (position 50,000), is the only one by
  "Studio Mezzanotte": the search "mezzanotte" finds it alone; it sits on the first platform chip's platform. Generated
  and indexed in 0.3 to 1.3 s in Release (3.6 s in Debug).
- `--scenario quick-catalog` (default, 2,165 frames, 128 checks): the I1 sequence with its checks adapted from
  per-game element ids to positions and rows (`expect-selected`, `expect-row`: the focus on the selected game's row,
  which shows its title and carries `selected`), and the new ones: the list restored around the marker in the middle
  after a search (the search "mezzanotte", then erased: position 50,000), a word of its title narrowing the list with
  the marker kept at its new place and back, a chip filter (A on the second platform: its first game; back to "Tutte":
  that game at its place in the whole list), Home, the resize with the window shrinking to 5 rows and back to 7, the
  search field's caret (the marker's word typed in two halves around the Home key, End before erasing it: the keys
  reach the field through the screen now that they are command keys), the text scale (7 rows, 5, 7), the shoulders
  (the next filter keeps the marker, the previous brings it back to the middle; twice the next selects the second
  platform's first game), End (the last game, the window at the end), a trigger's page up from the end and down again,
  Home and a trigger held for 0.55 s (3 pages), and `expect-window` (ten times: every shown row shows the window's
  game, the others are not displayed, blank rows past the end, the rows fill the list area and one more would not fit,
  the scroll thumb where the window is). `expect-steady` counts moves of the selection now (a scroll keeps the focus
  on its row).
- `--scenario user-bindings` (80 frames, 7 checks) with `spikes/ui/fixtures/user_bindings.json` as the user's
  `bindings.json`: the D-pad down no longer moves the selection, X moves it down and shows no details, the D-pad up
  still moves it up. Without the file the run fails (3 checks).
- `--input-script <json>` drives the screen with UI actions alone (no pad heard) and `--expect-selection <position>`
  fails the run unless the selection ends there; `tests/input_scripts/quick_catalog_select.json` (a tap down, the
  stick held for 1 s, End, a tap up) ends on 99,998.
- The report adds the catalog (games, generation time, pool and window rows, the final selection, the user's bindings
  file), the selection's moves, the rows shown per layout, frames that re-bound the rows, and a `virtualizedList`
  criterion.
- ctest (label `gpu`): `gpu.spike_ui.scenario` (screenshots `down10`, `actions`, `middle` at frame 571: the list
  restored around the marker, `resized_1280x720`, `last_chip`, `middle_list_focused` at 1022, `text_scale_1.2`, `end`
  at 1100), `gpu.spike_ui.input_script`, `gpu.spike_ui.user_bindings`.

```powershell
build/msvc-x64-debug/bin/spike_ui.exe --smoke --frames 2165 --hidden --no-vsync --fixed-dt 0.016667 `
  --data-dir build/spikes/ui/data --report build/spikes/ui/report.json `
  --screenshot-at "571:build/screenshots/middle.png" --screenshot-at "1022:build/screenshots/middle_focused.png"
build/msvc-x64-debug/bin/spike_ui.exe --smoke --frames 120 --hidden --fixed-dt 0.016667 `
  --input-script tests/input_scripts/quick_catalog_select.json --expect-selection 99998 --data-dir build/rvdata
./scripts/test.ps1 -Preset msvc-x64-debug-gpu -Filter spike_ui
```

## Tests

- `tests/unit/input`: the UI actions' names and `isUiAction`; the defaults table (specification §12.1 and the UI's),
  equal to `default_bindings.json`; every default button and key with the world's and the UI's actions it presses; the
  stick's and the triggers' dead zone on `ui_nav_*` and `ui_page_*`; a user file over the shipped defaults rebinding
  UI actions apart from the world's, and refusing an unknown one.
- `tests/unit/ui/test_nav_input.cpp`: `NavInput` on `ActionState`: every command from its action, the order within a
  frame, held D-pad and stick repeating alike, digital over stick and the last pressed winning, a fresh press, the
  suppression after `reset()` (directions and pages), a resting stick, page repeats, custom settings, `actionFor`,
  `commandKeys` (defaults and a user's map).
- `tests/unit/ui/test_catalog_source.cpp`: ids, rejected entries, paging and positions, platforms, search, narrowing,
  replaced entries, 100,000 entries.
- `tests/unit/ui/test_quick_catalog_model.cpp`: the window over 100,000 games (moves and margins, pages, Home, End, a
  click, the most negative step), a new search or filter (kept by id on its row, else the first entry), `refresh()`
  (kept by id; the nearest position when gone; the last; empty and back), the platforms read again, the shoulders,
  window sizes, a short list, revisions, a failing source.
- `tests/unit/ui/test_quick_catalog_input.cpp`: a user's `bindings.json` (written to a temporary directory, applied
  over the shipped defaults as `app::loadBindings` does) reaching the commands and the model; the input script played
  GPU-free over 100,000 games (positions 1, 8, 99,999, then 99,998).
- `tests/unit/ui/test_ui_documents.cpp`: the pool's bindings and ids, the strings in the document, the state keys,
  the list's overflow, one-line titles, the pool's size.

## Measurements (Release)

Method as in `docs/spikes/i1-ui.md`: `msvc-x64-release`, hidden 1920x1080 window on the reference machine (RTX 2080
SUPER, driver 617.42, display at 125%: dp ratio 1.25, 7 rows shown), `--no-vsync --fixed-dt 0.016667 --fps-cap 60`, the
quick-catalog scenario (its idle, taps and navigation phases: 300, 360 and 300 frames), 8 runs; the machine's CPU load
was sampled every 0.7 s in 5 of them (`Win32_PerfFormattedData_PerfOS_Processor`). Other worktrees of the wave built
and tested at the same time: mean loads of 54 to 100%, peaks of 98 to 100%, and the frame pacing itself suffered (frame
p50 18 to 37 ms in three runs where 16.7 ms is due, frame p99 over 100 ms in five): the figures are informative, the
gate measures on a quiet machine (ADR-0008 §10). The driver ran 3 runs in its fast mode (BeginFrame p50 about 0.002
ms) and 4 in its slow one (about 0.02 ms; I1 met it in 1 run of 16); one more fast-mode run at 74% load with a frame
p99 of 205 ms is left out of the table (quiet 0.211 ms, tap 5.8 ms, held 10.9 ms: the load's).

UI pass at p50, milliseconds, minimum to maximum over the runs of each mode (I1: ADR-0005 and `docs/spikes/i1-ui.md`):

| Frames | I1, fast mode | Step 6, fast mode (3 runs) | I1, slow mode (1 run) | Step 6, slow mode (4 runs) |
|---|---|---|---|---|
| Quiet, CPU | 0.206 to 0.243 | 0.193 to 0.211 | 0.493 | 0.501 to 0.528 |
| Quiet, GPU | 0.090 to 0.113 | 0.135 | 0.090 | 0.135 to 0.136 |
| Single tap (the card laid out again), CPU | 3.01 to 4.20 | 3.29 to 3.48 | 1.76 | 1.79 to 2.02 |
| D-pad held, CPU | 1.23 to 1.48 (a focus move) | 3.33 to 3.40 (a move that scrolls the window) | 0.78 | 1.97 to 2.27 |

- **Quiet frames**: as in I1, within the 1 ms budget of ADR-0005 in both driver modes; the pool of 28 rows (7 shown)
  and the scroll indicator draw 90 calls (87 in I1), the GPU time rises by 0.02 to 0.04 ms. Per part in the fast mode:
  update 0.069 to 0.077 ms (with the screen's after-update work: the rows' measurement reads three boxes per frame),
  render 0.121 to 0.131 ms. The CPU p99 of quiet frames was 0.45 to 0.60 ms in the fast-mode runs (up to 13 ms in the
  slow-mode runs at full load).
- **A single tap**, which moves the selection within the window and lays the card out again, costs what it cost in I1:
  the selected row's class replaced the re-evaluation of the bound games array, and RmlUi allocates 1,398 + 217 times
  in such a frame (update + render) where I1's tap allocated 2,625 + 354.
- **A held direction** now scrolls the window on most steps (the selection runs on the margin row): each step re-binds
  the 7 rows, whose titles, platforms and years change, so RmlUi lays them out and the vendored renderer builds new
  geometry for about 30 text elements (32 `CompiledGeometryData` per frame, against 3 in I1's held move), most of the
  cost in the fast driver mode (render 2.6 ms of the 3.4). Still well inside a 60 Hz frame (the test room takes about
  2.9 ms at 1080p), and it comes with input only. The fixes are the ones ADR-0005 lists (new geometry into a pooled
  vertex buffer), and one this step makes possible: recycled rows (a scroll step moves the rows by a transform and
  re-binds only the row that enters, so 1 row's text is re-meshed instead of 7). Both are BACKLOG candidates (below).
- **Allocations** (per frame, main thread, Retroverse's code + RmlUi, update + render; the same in every run): quiet
  frames 0 + 0 and RmlUi 0 + 3 (as in I1); a tap 0 + 27 and RmlUi 1,398 + 217; a held step 0 + 32 and RmlUi 1,386 +
  209. Retroverse's code allocates nothing outside the vendored renderer's one object per new geometry: the window's
  rows, the model's selected id and the pool's strings reuse their storage, the scroll indicator's percentages fit
  the strings' own buffers.
- **The catalog**: 100,000 games generated, sorted and indexed in 0.33 to 1.25 s (load-dependent; 3.6 s in Debug).
  The in-memory search of the scenario's typed words is not timed (the catalog's FTS5 search is step 9's).

## Decisions taken within the step

1. **`ui_activate`, not `ui_accept`**: the brief allowed ADR-0005's names, which match `UiCommand::Activate`.
2. **Analog actions for the stick** (`ui_nav_x`, `ui_nav_y`) beside the digital directions: see "How input reaches
   the screen". The brief's "left stick with its dead zone" is their binding; the hysteresis stays `NavInput`'s.
3. **Defaults beyond ADR-0005's eight actions**: pages on the triggers and Page Up/Down, Home/End on the stick clicks
   and the keys, filters on the shoulders. A 100,000-game list needs jumps on the pad (held pages run at 20 pages per
   second at most); the shoulders give the pad the filters without leaving the list (the chips row is reached from the
   list's first game only).
4. **Restore rules**: kept by id; when the game is gone, `refresh()` takes the nearest position (the brief) and a new
   search or filter its first entry (the I1 behavior; the nearest position of another list would be an arbitrary
   game). Both keep the selection's row in the window.
5. **Scroll margin of 2 rows, pages keep the row**: the next games stay visible while moving; pages and Home/End do not
   pull the selection off its row.
6. **The list measures its rows** instead of a fixed count per theme: text scale, display scale and theme all change
   it, and the model's window follows. Rows are one line high (ellipsis), else long titles at text scale 1.2 wrapped
   and the window fell to 3 rows.
7. **Strings**: "Tutte", "n.d." and the state labels moved into the RML (the screen binds empty values and keys).
   The thousands of the count are not grouped ("100000 giochi"): number formatting comes with localization.
8. **`NavBindings` removed**: the action map is the only binding table (ADR-0005 planned to build `NavBindings` from
   it; reading `ActionState` makes the table unnecessary).

## For the merge

- **ADR-0005**, an amendment to its Input paragraph: "`NavInput` reads the UI actions of the action map (`ui_up` ...
  `ui_filter_next`, `assets/config/default_bindings.json`, the user's `bindings.json` over them) from
  `input::ActionState`, keeping the stick hysteresis (on the dead-zone-shaped values of `ui_nav_x`/`ui_nav_y`: 0.62 and
  0.45 of the stick's travel with the default 0.15) and the repeat; `NavBindings` is gone; the user's bindings and
  `--input-script` reach the product UI (I2 step 6)." The Consequences' "A catalog of 40,000 games needs a virtualized
  list" is done.
- **CLAUDE.md**: the I1 spikes paragraph (spike_ui: 100,000 games, `--scenario`, `--input-script`,
  `--expect-selection`, 2,165 frames, 128 checks, the three GPU checks); "Controls go through the action map" may name
  the UI actions.
- **docs/architecture.md**: `rv_ui_model` (the catalog source, the window model), the input flow of the product UI.
- **BACKLOG.md**: close "UI actions in the action map" and "Large collections: a virtualized game list" (the settle
  delay's tuning on a TV remains); "Per-change cost under 1 ms": the selected row is marked by class now; the pooled
  vertex buffer, the card's own document and Tracy remain, and a scroll step of the list now costs 3.3 to 3.4 ms (fast
  driver mode) for its re-bound rows. New items: recycled rows for the virtualized list (the pool's rows moved by a
  transform as the window scrolls, only the entering row re-bound: about a seventh of the text re-meshed per step);
  number formatting with localization ("100000 giochi"); keyboard defaults for Vedi media, Informazioni and the
  filters, if any key can be spared from text entry.
- **docs/roadmap.md**: step 6 closed (the GPU checks, the input script, the user's bindings).
