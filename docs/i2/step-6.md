# I2 step 6: UI actions in the action map and a virtualized quick catalog

Outcome: the product UI reads its input through the action map, like the 3D world: sixteen `ui_*` actions with their
defaults in `assets/config/default_bindings.json`, overridden by `<user data>/config/bindings.json`, resolved by
`input::InputSystem`, overridable by `--input-script`, and turned into `ui::UiCommand`s by `ui::NavInput`, which keeps
the I1 spike's stick hysteresis (0.55 / 0.35 of the stick's travel, as ADR-0005 records it) and repeat timing on top of
them, and gives the triggers that page a hysteresis of their own. The quick catalog is virtualized: `ui::CatalogSource`
(in `rv_ui_model`) pages a materialized list the way rv_catalog's `ICatalogQuery` does (ADR-0008 §5), its queries
answered at once or later from the catalog worker; `ui::QuickCatalogModel` is a window over it that reads only the rows
entering the window as it scrolls; `ui::QuickCatalogScreen` binds a pool of as many rows as fit the list area, in which
a game keeps its row while the window holds it, so a scroll step binds one row. `spike_ui` runs the screen over 100,000
generated games: its 153 scenario checks pass in Debug and Release (imports under the screen and a list emptied under
the card's actions among them), an input script and a user's bindings file drive it in two more GPU checks. Quiet
frames cost less than in I1 (UI pass CPU p50 0.13 to 0.15 ms in the driver's fast mode; GPU 0.135 ms); a single tap
costs what it cost (3.0 to 3.4 ms); a held direction that scrolls the list costs 2.8 to 3.0 ms per step, 0.5 to 0.7 ms
less than the step's first version (3.3 to 3.6 ms in the same interleaved runs) but still about a tap's cost: in RmlUi
6.3 any text change lays the whole document out again and generates every text's mesh again (informative figures).

## What was built

- `rv_input` (`action.hpp`, `action.cpp`, `action_map.cpp`): sixteen UI actions after the existing ones (whose names
  and indices are unchanged), `input::isUiAction()`, their defaults in `ActionMap::defaults()` and in
  `assets/config/default_bindings.json` (the test that the two agree covers them).
- `rv_ui_model`:
  - `ui_command.hpp`: six more commands (PageUp, PageDown, Home, End, FilterPrevious, FilterNext).
  - `nav_input.hpp`: `NavInput::update(const input::ActionState&, ...)` replaces the raw-input version and
    `NavBindings` is gone; `ui::actionFor(UiCommand)` names each command's action, `ui::commandKeys(ActionMap)` the
    keys bound to UI actions (what `UiView` keeps away from RmlUi). `DirectionRepeater` and `StickDirection` are
    unchanged; a second repeater runs the page commands, whose actions engage with a hysteresis
    (`NavSettings::pageRelease`).
  - `catalog_source.hpp` (new): `CatalogEntry` (moved here, with a `state` key), `isValidEntryId`, `PlatformFilter`,
    `CatalogSourceQuery`, `QueryState`, the `CatalogSource` port (`query()` answers Ready or Pending, `poll()`
    collects a pending list).
  - `in_memory_catalog_source.hpp` (new): `InMemoryCatalogSource`, for tests and mock-ups; `setPendingPolls()` makes
    it answer like a source on the catalog worker, `replaceEntries()` like an import.
  - `quick_catalog_model.hpp`: the model rewritten as a window over a `CatalogSource`, with `poll()` and `pending()`.
  - `ui_assets.hpp`: the pool (`kPoolRows` 28 at most, `kRowPrefix` "row-", `poolSlotOf()`), the list, the selected
    class, the scroll thumb, the state keys; `kGamePrefix` is gone (no element is named after a game any more).
- `rv_ui`: `UiView::setCommandKeys(bitset)` (from `commandKeys()`; the defaults' until set) and `UiView::caretKey()`
  (RmlUi's Home and End for a focused text field); `QuickCatalogScreen` with the pool of rows, `windowBinds()`,
  `rowBinds()`.
- `assets/ui/quick_catalog.rml` and `.rcss`: the pool, the scroll indicator, the state labels, one-line rows placed
  by the screen, an empty list that says why; every string the screen shows is in the document ("Tutte", "n.d." and
  the state labels moved out of C++).
- `spikes/ui`: the 100,000-game mock, the scenarios (`--scenario quick-catalog | user-bindings`), `--input-script`
  and `--expect-selection`, the new checks, three ctest cases, the fixture `spikes/ui/fixtures/user_bindings.json`; the
  virtual pad's triggers rest at SDL's minimum (they read half pulled before).
- `tests/input_scripts/quick_catalog_select.json`; unit tests in `tests/unit/input` and `tests/unit/ui`.

## The UI actions and their default bindings

| Action | Keyboard | Pad | Command |
|---|---|---|---|
| `ui_up`, `ui_down`, `ui_left`, `ui_right` | arrows | D-pad | Up, Down, Left, Right (repeat while held) |
| `ui_nav_x`, `ui_nav_y` | | left stick X, Y (read whole: no dead zone) | the same directions, through `StickDirection` |
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
hysteresis. The default bindings read the stick whole (`"deadzone": 0`), so the hysteresis thresholds (engage 0.55,
release 0.35, axis switch 1.25) are fractions of the stick's travel, as in I1 and ADR-0005: the hysteresis is the
stick's dead zone (a user binding with a dead zone shapes the values first). The page commands have a repeater of
their own (the last engaged page wins); a page action engages when held (a key, or a trigger past the input system's
press threshold: 0.575 of its travel with the default 0.15 dead zone) and stays engaged until its value falls below
`NavSettings::pageRelease` (0.35: 0.45 of the travel), so a trigger resting near the press threshold pages at the held
cadence instead of on every crossing; the press is the engagement's rising edge. The other commands fire on the press
of their action, after the steps, in a fixed order. `reset()` suppresses held directions and pages alike (a trigger
still pulled through it pages only once released and pulled again). One behavior is lost on the way: a button released
and pressed again between two frames reads as held at the action level (as for every action), so it no longer steps
again at once.

Back keeps its levels: `ui_back` gives the screen's Back (the focus history, then `QuickCatalogEvents::close`); in
`spike_ui` Back with the catalog closed quits, as before.

## The catalog source contract

`ui::CatalogSource` (`rv/ui/catalog_source.hpp`), called from the UI's thread only and never blocking it:

| Call | Does | ICatalogQuery's counterpart |
|---|---|---|
| `platforms()` | the platforms, one chip each, in the chips' order, with their entry counts, at once | the systems list, kept by the adapter |
| `query(CatalogSourceQuery{text, platform})` | materializes the ordered list of the entries the query matches: at once (`Ready`), or on the catalog worker (`Pending`) | `run(CatalogQuery)`, on the worker |
| `poll()` | collects a pending query: `Ready` once its list has replaced the previous one (within this call), `Pending` while it runs | the worker's result, handed over |
| `count()` | the list's size | `list.size()` |
| `rows(first, n, out)` | the display rows of positions [first, first + n), cut at the end, one per position, reusing `out`'s storage | `fetch(pageOf(list, first, n))` |
| `position(id)` | an id's position in the list, if it holds it | `positionOf(list, id)` |

- The list the UI reads changes only within `query()` and `poll()`: while a query is pending, `count()`, `rows()` and
  `position()` read the previous list. A query asked while another is pending replaces it: only the latest query's list
  is ever switched to (typing coalesces).
- Ids are the GameId text form ('g' and 16 hexadecimal digits), which `isValidEntryId` accepts.
- `CatalogEntry::state` is the availability or readiness slot: a key the document labels (`unavailable`: the file
  cannot be reached; `needs-configuration`: the command needs a profile; `emulator-missing`: emulator or core not
  found; empty: ready). The text stays in the RML (Italian strings never in C++).
- A position whose entry vanished after the list was materialized (an import ran) gives a row with its id only, so
  the window's rows stay aligned with positions; the next query materializes the list again.
- Errors (`IoError`, `Unavailable`, `InvalidArgument` for an unknown platform) leave the list empty (a query, also one
  collected by `poll()`) or the rows empty (a fetch).
- The adapter of step 13 keeps the materialized `std::vector<GameId>` and an index of it for `position()` (query.hpp:
  "a view that asks often keeps its own index of the list"), maps a chip to `CatalogQuery::systems`, formats
  `CatalogRow` into `CatalogEntry` (year from the release date, players and the readiness key), keeps the platforms it
  read with the last import, hands `query()` to the catalog worker (`ICatalogQuery::run` runs there, ADR-0008 §5) and
  answers `Pending`, and switches to the worker's list in `poll()`. `rows()` fetches a page on the UI thread within
  ADR-0008's 5 ms.

`ui::InMemoryCatalogSource` keeps the entries' order, rejects invalid and repeated ids (the first entry of an id
stays; a sort, not the I1 model's quadratic scan), lists the platforms in the order the entries first name them, and
matches like the I1 model (every word, split at ASCII white space, must occur in the title, platform, developer,
publisher, genre or year, ASCII case folded; the catalog's FTS5 index also folds diacritics). A query that only adds
to the previous words (typing on) searches the previous list instead of the catalog. Positions are kept per entry
and ids are looked up in an index sorted by id: `position()` never scans. `replaceEntries()` stands for an import: the
list stays the one materialized until the next query, its rows read from the new entries by id (the id alone for a
game that is gone). `setPendingPolls(n)` makes queries answer `Pending` and switch at the n-th `poll()`.

## The model: a window over the source

`QuickCatalogModel(source, windowRows = 10, scrollMargin = 2)` keeps the filters, the search, the list's count, the
selected position and id, the window's first position and its rows (`window()`), never state per entry of the
catalog.

- Window: `windowRows()` positions from `windowFirst()`; the selection is always inside. A move scrolls the window
  only when the selection would come closer than the margin (2 rows, at most half the window) to the edge it moves
  towards; at the list's ends the margins give way. A page (the window's rows) moves the selection and the window
  together, so the selection keeps its row; Home and End jump to the first and last game. A click (`selectPosition`)
  on a shown row never scrolls.
- Reads: a scroll keeps the rows the window keeps (their strings swapped into place, not copied) and reads only the
  rows that enter it, one per step of a held direction, three per notch of the wheel; a page, a jump, a new list or
  another window size reads the whole window. A read that failed leaves the window without rows, and the next move
  reads it whole again, also within the window.
- Re-queries: a new list keeps the selected game when it still holds it (by id, `position()`), on the row of the
  window it had (within the margins). When the game is gone, a new search or filter starts at the first entry (as the
  I1 model did), and `refresh()` (the same query again: an import changed the catalog, ADR-0008 §5) takes the nearest
  position: the one the game had, or the last. `refresh()` also reads the platforms again; a filter whose platform
  disappeared falls back to every game, and platforms that cannot be read keep the chips and the active filter.
- Pending lists: when the source answers `Pending`, the model keeps the list it shows (moves, pages and the window work
  on it, `pending()` says so) until `poll()` collects the new one, then places the selection as the operation that
  asked for it says: when a search or a filter asked since the last list, a game the new list lacks restarts at its
  first entry; after refreshes alone, it takes the nearest position. The screen polls once per frame.
- `setWindowRows()` (the screen's measurement) keeps the selection's row where the margins allow, so a size or text
  scale change and its reverse give back the same window from the margin rows and from the list's start.
- `cycleFilter(step)` wraps around the chips (the shoulders).
- Revisions: filters, list (each list applied), window (each read of its rows), selection (the selected game's id).
  `error()` is the first error of the latest operation that read the source (a query, the platforms, the window's
  rows), until an operation reads it without one.

## The screen: a pool of rows

- `quick_catalog.rml` binds `rows`, a `std::vector<RowData>` as long as the rows that fit the list area (at most 28;
  its capacity reserved): element ids `row-0`, `row-1`, ..., fixed (focus targets and the history never name a game).
  `present` marks the rows with a game (blank rows past the end carry the class `vacant`: hidden, never focused);
  title, platform, year, state. Rows that do not fit do not exist, so a change of the rows never evaluates their views
  (the first version kept 28 rows and hid 21 of them at 1080p).
- A game keeps its row while the window holds it: the slot of position p is p mod the rows shown
  (`quick_catalog::poolSlotOf`). A scroll by one row binds the one slot whose game left the window to the game that
  entered it; the screen then moves every row to its place in the window with its style `top` (relative to its slot in
  the list's flow, in whole pixels so that the rows' rounding does not change: moved, a row's geometry is not built
  again). A new list or another number of rows binds every row. The focus moves from row to row with the selected
  game, as in I1's held move.
- How many rows fit is measured after each layout: the pitch between the first two slots of the list's flow (the
  data-for elements, which the offsets never move), a row's height and the list's content height (`measureRows`,
  three box reads per frame, no allocation). When the number of rows or the pitch changes (a resize, the text scale,
  the theme) the screen gives the model the new window, places the rows and lays the context out again at once, so
  the frame shows them in place. Titles stay on one line (`white-space: nowrap`, `text-overflow: ellipsis`) so that
  every row has one height at any text scale.
- Rows that fit, measured on the reference machine (display at 125%, regular theme unless noted): 1920x1080 7 rows;
  1920x1080 at text scale 1.2, 5; 1280x720 (compact), 5; and with the dp ratio at 1.0 (`--ui-scale 0.8`): 1920x1080
  10, 2560x1440 15, 3840x2160 24. The pool covers the largest (a 2160-pixel-tall view at 100%) with a margin; taller
  views in dp (a text scale below 1) show 28 rows.
- The selected game's row carries the class `selected`, set on the two rows concerned when the selection's row
  changes (BACKLOG "Per-change cost under 1 ms": no re-evaluation of a bound array for a move within the window).
- The model may change outside the screen's commands: `QuickCatalogModel::refresh()` called by the application after
  an import, a list a pending query brings. The screen compares the model's revisions before every command and after
  every update (`afterUpdate`, which also calls `QuickCatalogModel::poll()`) and shows what changed (rows, card,
  chips, the focus on the selected game's row), so a command never acts on a game the screen does not show and a click
  selects the game the row showed (by id when the list changed under it).
- Focus: in the list, Up and Down move the selection through the model and the focus follows it. Up at the first game
  and Right leave by the region links, as before; landing in the list (a link, Back, show, a reload) always focuses
  the selected game's row. A list that empties sends the focus to the active filter's chip, also from the card's
  actions (the card hides with no game selected; the focus would stay on a button nobody sees); a focused element that
  is no longer visible gives way to the list, else the filters. A click on a row selects its game; the wheel over the
  list moves the selection 3 games per notch (the list does not scroll in RmlUi: `overflow: hidden`). Page moves, Home
  and End act in the list; Home and End move the search field's caret (the screen sends RmlUi's keys, since they are
  command keys now) and jump to the first and last chip in the chips row. The shoulders change the filter from any
  region; the active chip scrolls into view, and the focus follows it when it is in the chips row.
- An empty list says why: "Catalogo non disponibile: non è stato possibile leggerlo." while the source fails (the
  model's error with no rows: `unavailable` in the data model), "Nessun gioco corrisponde alla ricerca." for a search
  without results, "Nessun gioco nel catalogo." for an empty catalog.
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
- `--scenario quick-catalog` (default, 2,271 frames, 153 checks): the I1 sequence with its checks adapted from
  per-game element ids to positions and rows (`expect-selected`, `expect-row`: the focus on the selected game's row,
  which shows its title and carries `selected`), I1's stick values (resting at 0.47 and 0.52 of its travel, crossing
  at 0.58 and 0.53, near the diagonal), and the new ones: the list restored around the marker in the middle after a
  search (the search "mezzanotte", then erased: position 50,000), a word of its title narrowing the list with the
  marker kept at its new place and back, a chip filter (A on the second platform: its first game; back to "Tutte":
  that game at its place in the whole list), Home, the resize with the window shrinking to 5 rows and back to 7, the
  search field's caret (the marker's word typed in two halves around the Home key, End before erasing it: the keys
  reach the field through the screen now that they are command keys), the text scale (7 rows, 5, 7), the shoulders
  (the next filter keeps the marker, the previous brings it back to the middle; twice the next selects the second
  platform's first game), End (the last game, the window at the end), a trigger's page up from the end and down again,
  Home and a trigger held for 0.55 s (3 pages), `expect-window` (twelve times: a row per window row and none more, the
  row of each position showing its game, the rows in the window's order a pitch apart from the list area's top and
  filling it, blank rows past the end, the scroll thumb where the window is); after the measurement phases, two imports
  under the screen (`import`: the spike replaces the source's entries and calls the model's `refresh()` itself, never
  the screen, the source answering three polls later: the previous list until then, then the marked game five places
  up on its row with its card and window, then back) and a list emptied under the card's actions (the marker alone,
  Avvia focused, the next filter twice: no game, the focus on that filter's chip; the previous twice brings the marker
  back). `expect-steady` counts the moves of the selection and of the focus (in the list the focus follows the
  selected game's row); `expect-row` follows the resting and the diagonal stick.
- `--scenario user-bindings` (80 frames, 7 checks) with `spikes/ui/fixtures/user_bindings.json` as the user's
  `bindings.json`: the D-pad down no longer moves the selection, X moves it down and shows no details, the D-pad up
  still moves it up. Without the file the run fails (3 checks).
- `--input-script <json>` drives the screen with UI actions alone (no pad heard) and `--expect-selection <position>`
  fails the run unless the selection ends there; `tests/input_scripts/quick_catalog_select.json` (a tap down, the
  stick held for 1 s, End, a tap up) ends on 99,998.
- The virtual pad's triggers span SDL's whole joystick range, at rest `SDL_JOYSTICK_AXIS_MIN`
  (`SDL_SetJoystickVirtualAxis`); the first version sent 0 at rest, which SDL reads as a trigger half pulled (0.41 past
  the dead zone: under the press threshold, so it went unnoticed until the page hysteresis held the page engaged).
- The report adds the catalog (games, generation time, pool and window rows, the final selection, the user's bindings
  file), the selection's moves, the rows shown per layout, frames that bound rows and how many rows per frame
  (`rowsBound`), and a `virtualizedList` criterion.
- ctest (label `gpu`): `gpu.spike_ui.scenario` (screenshots `down10`, `actions`, `middle` at frame 571: the list
  restored around the marker, `resized_1280x720`, `last_chip`, `middle_list_focused` at 1022, `text_scale_1.2`, `end`
  at 1100), `gpu.spike_ui.input_script`, `gpu.spike_ui.user_bindings`.

```powershell
build/msvc-x64-debug/bin/spike_ui.exe --smoke --frames 2271 --hidden --no-vsync --fixed-dt 0.016667 `
  --data-dir build/spikes/ui/data --report build/spikes/ui/report.json `
  --screenshot-at "571:build/screenshots/middle.png" --screenshot-at "1022:build/screenshots/middle_focused.png"
build/msvc-x64-debug/bin/spike_ui.exe --smoke --frames 120 --hidden --fixed-dt 0.016667 `
  --input-script tests/input_scripts/quick_catalog_select.json --expect-selection 99998 --data-dir build/rvdata
./scripts/test.ps1 -Preset msvc-x64-debug-gpu -Filter spike_ui
```

## Tests

- `tests/unit/input`: the UI actions' names and `isUiAction`; the defaults table (specification §12.1 and the UI's),
  equal to `default_bindings.json`; every default button and key with the world's and the UI's actions it presses; the
  stick read whole by `ui_nav_*` (the walker's dead zone on `move_*`) and the triggers' dead zone on `ui_page_*`; a
  user file over the shipped defaults rebinding UI actions apart from the world's, and refusing an unknown one.
- `tests/unit/ui/test_nav_input.cpp`: `NavInput` on `ActionState`: every command from its action, the order within a
  frame, held D-pad and stick repeating alike, digital over stick and the last pressed winning, a fresh press, the
  suppression after `reset()` (directions and pages), a resting stick, page repeats, a trigger chattering around the
  press threshold through the default bindings (the input system presses it every other frame; one page, then the held
  cadence, a new page only after it falls under the release threshold), custom settings, `actionFor`, `commandKeys`
  (defaults and a user's map).
- `tests/unit/ui/test_catalog_source.cpp`: ids, rejected entries, paging and positions, platforms, search, narrowing,
  replaced entries (the list kept until the next query, its rows read anew by id, the id alone of a game gone), queries
  answered on a worker (the previous list readable, the latest query only, a failing one), 100,000 entries.
- `tests/unit/ui/test_quick_catalog_model.cpp`: the window over 100,000 games (moves and margins, pages, Home, End, a
  click, the most negative step), a new search or filter (kept by id on its row, else the first entry), `refresh()`
  (kept by id; the nearest position when gone; the last; empty and back), the platforms read again, the shoulders,
  window sizes, a short list, revisions, a failing source, a move reading the rows again after a failed read, platforms
  that cannot be read (filters kept, the error reported), a scroll reading only the rows that enter, lists answered on
  the worker (kept until `poll()`, the restore rule of the operations that asked), a model created over a pending
  source.
- `tests/unit/ui/test_quick_catalog_input.cpp`: a user's `bindings.json` (written to a temporary directory, applied
  over the shipped defaults as `app::loadBindings` does) reaching the commands and the model; the input script played
  GPU-free over 100,000 games (positions 1, 8, 99,999, then 99,998).
- `tests/unit/ui/test_ui_documents.cpp`: the pool's bindings and ids, the strings in the document (the three empty-list
  messages among them), the state keys, the list's overflow, one-line and relatively positioned rows, the pool's size,
  `poolSlotOf` (consecutive positions take every row once, the row that leaves takes the game that enters).

## Measurements (Release)

Method as in `docs/spikes/i1-ui.md`: `msvc-x64-release`, hidden 1920x1080 window on the reference machine (RTX 2080
SUPER, driver 617.42, display at 125%: dp ratio 1.25, 7 rows shown), `--no-vsync --fixed-dt 0.016667 --fps-cap 60`, the
quick-catalog scenario (its idle, taps and navigation phases: 300, 360 and 300 frames). Six runs, alternating the
step's first version (commit 482cfed, "first") and this one ("final"), both built in the worktree, the driver in its
fast mode (BeginFrame p50 0.002 ms) in all of them; the machine's CPU load sampled before each run was 17 to 46% (other
worktrees of the wave built at the same time): the figures are informative, the gate measures on a quiet machine
(ADR-0008 §10).

UI pass at p50, milliseconds, minimum to maximum over three runs each (I1: ADR-0005 and `docs/spikes/i1-ui.md`):

| Frames | I1, fast mode | First version | Final |
|---|---|---|---|
| Quiet, CPU | 0.206 to 0.243 | 0.174 to 0.203 | 0.132 to 0.145 |
| Quiet, GPU | 0.090 to 0.113 | 0.119 to 0.135 | 0.135 |
| Single tap (the card laid out again), CPU | 3.01 to 4.20 | 3.14 to 3.40 | 3.01 to 3.40 |
| D-pad held, CPU | 1.23 to 1.48 (a focus move) | 3.31 to 3.55 (a move that scrolls) | 2.80 to 2.98 (a move that scrolls) |
| D-pad held, update / render CPU | | 0.58 to 0.72 / 2.64 to 2.85 | 0.47 to 0.49 / 2.30 to 2.42 |

- **Quiet frames**: within the 1 ms budget of ADR-0005, cheaper than I1's and than the first version's: the rows that
  do not fit no longer exist (update 0.036 to 0.038 ms against 0.063 to 0.072); 90 draw calls (87 in I1: the scroll
  indicator). The CPU p99 of quiet frames was 0.21 to 0.43 ms.
- **A single tap**, which moves the selection within the window and lays the card out again, costs what it cost in I1.
- **A held direction** scrolls the window on most steps. Each step now binds one row (`rowsBound` p50 1: the first
  version bound seven), reads one row from the source and moves the seven rows by their offsets, and the focus moves to
  the next row as in I1's held move. It costs 0.5 to 0.7 ms less than the first version, and still about a tap: in
  RmlUi 6.3 a text change dirties the document's layout, `ElementDocument::UpdateLayout` formats the whole document
  from its root, and every text element laid out again generates its mesh again on the CPU (`ElementText::AddLine`
  dirties it; only meshes that come out identical keep their compiled geometry). The entering row's new text therefore
  costs the layout and the text meshes of the whole screen (catalog, chips, card, hints), as the card's text does for a
  tap; the renderer compiled 23 geometries per step (32 in the first version). I1's held move changed no text: its
  focus move restyled two rows, without a layout, and so do the held steps that stay inside the window here (3
  geometries, as in I1). What would bring a scroll step under 1 ms is a smaller layout and
  fewer meshes per change: the list (and the card, as ADR-0005 already lists) in documents of their own, so that the
  entering row lays out the list's document alone, and new geometry into a pooled vertex buffer (ADR-0005). Both are
  BACKLOG candidates (below). ADR-0005's accepted deviation ("a change costs at most a quarter of a 60 Hz frame, comes
  only with input") holds: 2.8 to 3.0 ms at p50, input only.
- **Allocations** (per frame, main thread, Retroverse's code + RmlUi, update + render; the same in every run): quiet
  frames 0 + 0 and RmlUi 0 + 3 (as in I1); a tap 0 + 27 and RmlUi 1,369 + 217; a held step 0 + 23 and RmlUi 1,406 +
  219 (the layout's and the text meshes'; the first version: 0 + 32 and 1,386 + 209). Outside the vendored renderer's
  one object per new geometry, Retroverse's code allocates nothing in quiet frames and taps, and in held steps 0 at p50
  and up to 4 (mean 0.5): the rows' strings circulate between the window and the rows read for it, and grow when a
  longer title or description enters. The selected id and the pool's strings reuse their storage, the scroll
  indicator's percentages fit the strings' own buffers.
- **The catalog**: 100,000 games generated, sorted and indexed in 0.33 to 1.25 s (load-dependent; 3.6 s in Debug).
  The in-memory search of the scenario's typed words is not timed (the catalog's FTS5 search is step 9's, on the
  worker).

## Decisions taken within the step

1. **`ui_activate`, not `ui_accept`**: the brief allowed ADR-0005's names, which match `UiCommand::Activate`.
2. **Analog actions for the stick** (`ui_nav_x`, `ui_nav_y`) beside the digital directions: see "How input reaches
   the screen". The brief's "left stick with its dead zone" is NavInput's hysteresis: the bindings read the stick whole
   so that its thresholds stay the stick's travel, as ADR-0005 records them (the first version kept the 0.15 dead zone
   of the walker's binding, which moved them to 0.62 and 0.45).
3. **Defaults beyond ADR-0005's eight actions**: pages on the triggers and Page Up/Down, Home/End on the stick clicks
   and the keys, filters on the shoulders. A 100,000-game list needs jumps on the pad (held pages run at 20 pages per
   second at most); the shoulders give the pad the filters without leaving the list (the chips row is reached from the
   list's first game only). The triggers page with a hysteresis of their own (`pageRelease`), in `NavInput` like the
   stick's: the input system's held flag has a single threshold for every action.
4. **Restore rules**: kept by id; when the game is gone, `refresh()` takes the nearest position (the brief) and a new
   search or filter its first entry (the I1 behavior; the nearest position of another list would be an arbitrary
   game). Both keep the selection's row in the window. A pending list follows the rule of the operations that asked
   for it (a search or a filter among them: the first entry).
5. **Scroll margin of 2 rows, pages keep the row**: the next games stay visible while moving; pages and Home/End do not
   pull the selection off its row.
6. **The list measures its rows** instead of a fixed count per theme: text scale, display scale and theme all change
   it, and the model's window follows. Rows are one line high (ellipsis), else long titles at text scale 1.2 wrapped
   and the window fell to 3 rows.
7. **Strings**: "Tutte", "n.d.", the state labels and the empty list's reasons are in the RML (the screen binds empty
   values, keys and flags). The thousands of the count are not grouped ("100000 giochi"): number formatting comes with
   localization.
8. **`NavBindings` removed**: the action map is the only binding table (ADR-0005 planned to build `NavBindings` from
   it; reading `ActionState` makes the table unnecessary).
9. **A source that may answer later**: `query()` answers `Ready` or `Pending` and `poll()` collects, because
   `ICatalogQuery` runs queries on the catalog worker only (ADR-0008 §5, catalog_query.hpp): a synchronous `query()`
   would have made the adapter block the UI thread on every keystroke of a 100,000-game catalog, or return the previous
   list with the wrong restore rule. The list the UI reads never changes between its calls, also when an import
   replaces the entries.
10. **A pool as long as the rows that fit, a game keeping its row**: the rows past the window cost nothing per change,
    a scroll binds the entering row and reads one row. Rows are moved with their style `top` in whole pixels (a
    relative offset changes no layout and, in whole pixels, no geometry), set by the screen like the `selected` class.
11. **Errors per operation**: `error()` holds the first error of the latest operation that read the source, so a
    failed read of the platforms during `refresh()` is reported although the query succeeds, and keeps the chips and
    the user's filter instead of falling back to every game.
12. **An emptied list sends the focus to the active filter's chip** (from the list and from the card's actions): the
    filter that emptied it, where the next filter is a shoulder or a step away.

## Review findings (I2 step 6 review)

| Finding | Outcome |
|---|---|
| R6C-1: the screen missed model changes made outside its commands (`refresh()`) | Fixed: revisions compared before every command and after every update (`catchUp`); `afterUpdate` polls pending lists. Scenario: two imports under the screen, the list answered three polls later. |
| R6C-2: triggers paged on every crossing of the press threshold | Fixed in `NavInput` (`pageRelease` hysteresis, the press as the engagement's edge); unit test with a chattering trigger. The scenario then exposed the virtual pad's triggers resting half pulled: fixed. |
| R6C-3: focus left on a hidden Avvia when the list empties | Fixed: `followSelection` sends it to the active chip; `afterUpdate` refocuses an invisible focused element. Scenario step. |
| R6C-4: a failed `platforms()` swallowed, the filter lost | Fixed: filters and active filter kept, the error kept for the whole operation; unit test. |
| R6C-5: no retry after a failed `rows()` until the window moved | Fixed: a move reads a window without rows again; unit test. |
| R6C-6, adr5-stick: stick thresholds moved off ADR-0005 by the 0.15 dead zone | Fixed: `ui_nav_x`/`ui_nav_y` bound without a dead zone, I1's stick values back in the scenario; no ADR amendment for the thresholds. |
| perf-1: a held scroll step at 2.2 to 2.6 times I1's held move | Partly fixed: rows recycled (one row bound per step), the pool as long as the rows that fit (no hidden rows' views); 0.5 to 0.7 ms less per step, quiet frames 25% cheaper. The rest is RmlUi's whole-document layout on a text change (measured above); the documents of their own and the pooled vertex buffer stay BACKLOG candidates; ADR-0005's figures need a note at the merge (below). |
| perf-2: no asynchronous path for queries | Fixed: `QueryState`, `poll()`, pending lists in the model; unit tests and the scenario's imports. |
| perf-3: every scroll step fetched the whole window | Fixed: the rows the window keeps are kept, only the entering ones read (one per step); unit test counting reads. List-only fields for the window were not added: one row per step is within ADR-0008's page budget. |
| spike-focus: focus stability no longer checked directly | Fixed: `expect-row` at frames 364 and 398, `expect-steady` counts the focus's moves too. |
| ux-error-state: an unreadable catalog read as "no match" | Fixed: "Catalogo non disponibile" while the source fails, "Nessun gioco nel catalogo." for an empty catalog; document test. |

## For the merge

- **ADR-0005**, an amendment to its Input paragraph: "`NavInput` reads the UI actions of the action map (`ui_up` ...
  `ui_filter_next`, `assets/config/default_bindings.json`, the user's `bindings.json` over them) from
  `input::ActionState`, keeping the stick hysteresis (the stick read whole by `ui_nav_x`/`ui_nav_y`: 0.55 / 0.35 of its
  travel, unchanged) and the repeat, with a release threshold for the triggers that page; `NavBindings` is gone; the
  user's bindings and `--input-script` reach the product UI (I2 step 6)." The Consequences' "A catalog of 40,000 games
  needs a virtualized list" is done; a note beside the held-move figure: "a held direction that scrolls the virtualized
  list costs about a tap, 2.8 to 3.0 ms at p50 (I2 step 6): RmlUi 6.3 formats the whole document and generates every
  text's mesh again for the entering row's text; documents of their own for the list and the card and the pooled vertex
  buffer are the levers". The accepted deviation itself does not change (a change under a quarter of a 60 Hz frame,
  with input only); if the merge judges the note a change of the recorded decision, it goes into a new ADR (CLAUDE.md).
- **ADR-0008** §5 is met as written: the UI's port lets `query()` run on the worker (`Pending`, `poll()`).
- **CLAUDE.md**: the I1 spikes paragraph (spike_ui: 100,000 games, `--scenario`, `--input-script`,
  `--expect-selection`, 2,271 frames, 153 checks, the three GPU checks); "Controls go through the action map" may name
  the UI actions.
- **docs/architecture.md**: `rv_ui_model` (the catalog source and its pending queries, the window model), the input
  flow of the product UI.
- **BACKLOG.md**: close "UI actions in the action map" and "Large collections: a virtualized game list" (the settle
  delay's tuning on a TV remains); "Per-change cost under 1 ms": the selected row is marked by class and the list's
  rows are recycled now; the pooled vertex buffer, the card's own document and Tracy remain, and a new item: the list
  in a document of its own (a scroll step lays out the whole screen today: 2.8 to 3.0 ms). New items: number
  formatting with localization ("100000 giochi"); keyboard defaults for Vedi media, Informazioni and the filters, if
  any key can be spared from text entry.
- **docs/roadmap.md**: step 6 closed (the GPU checks, the input script, the user's bindings).
