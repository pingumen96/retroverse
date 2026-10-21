# I4 step 7a: the directory panel

Roadmap row: I4 step 7 (its first half; 7b wires the panel into the app: the elevator's prompt, M and Select, the ride,
the teleport). Decisions: [ADR-0013](../decisions/ADR-0013-the-building.md) §10,
[ADR-0012](../decisions/ADR-0012-interaction-in-the-world.md) §8, [ADR-0005](../decisions/ADR-0005-product-ui.md).

## What was built

- **The catalog query** (`rv/catalog/recent_games.hpp`, `ICatalogQuery::favoritesAndRecent`, answered by
  `SqliteCatalogQuery` in `src/catalog_db/src/recent_query.cpp`): one read of the games that are ES-DE favourites or
  were played, across systems (entries of state `game` only, `<hidden>` and unavailable ones left out), ranked
  favourites first, then by when they were last played (the never played last), then sort key and GameId. "Last
  played" is the later of Retroverse's last session that started the game (UTC, launch tests and failures to start
  left out, the room snapshot's rule) and ES-DE's `<lastplayed>`.
- **The model** (`rv/ui/directory_model.hpp`, `rv_ui_model`, GPU-free): two tabs, "Piani" (floors, then a floor's hall
  and rooms) and "Preferiti e recenti" (games, then a game's card); the walker's floor and room marked; `ui_*` actions
  through `NavInput` and clicks; Back's levels (card -> games, rooms -> floors, floors or games -> closed); the
  commands `ChooseFloor`, `ChooseRoom`, `Launch`, `ShowInRoom`, `Close`; the requests it makes of the app
  (`gamesRequest()`, `cardRequest()`) and `reopen()` for a refused command. 17 unit cases in
  `tests/unit/ui/test_directory.cpp`, the document's contract among them.
- **The screen and its document** (`rv/ui/directory_screen.hpp`, `assets/ui/directory.rml`, `.rcss`): the quick
  catalog's look, dp sizes, a compact theme, no animated colour. Floors are named by their `building.json` id (Piano
  terra, Prime console, Console a 8 bit, Console a 16 bit, Console a 32 e 64 bit, Sesta generazione, Console moderne,
  Console portatili, Computer e altro; another id reads "Piano <id>"), rooms by kind and systems (a console room by its
  system's full name, "Sala condivisa" and "Sala giochi arcade" with their sections' systems). A row's second line puts
  the short facts first: a floor's rooms and games, then what it holds; a room's games, then (shared, arcade) every
  section's system, wrapped onto as many lines as they take; a game's "Giocato il <date>", then its system. The card is
  the quick catalog's: its readiness, blocker and room sentences (the test compares them line for line; since the
  merge with step 5a both say `not-in-room` and `other-room` of the building alike).
- **`rv_ui_probe`** (`tests/ui_probe`): the panel over the test room from a fixed model (`directory_fixture.hpp`: six
  floors, gen4's shared room of five long system names, twelve games with a long title and a favourite on a long
  system name, invented names), driven by `tests/input_scripts/directory_panel.json` (the long title selected for
  `games.png`), its checkpoints reported and screenshotted. `gpu.directory: ...` checks each checkpoint's view and
  RmlUi focus, the focus's edge in the screenshots and the one command (Mostra nella stanza); the unit suite plays the
  same script GPU-free. How to look at it: [visual-verification.md](../reference/visual-verification.md).

## Decisions

- **ES-DE's `<lastplayed>` is local wall time.** ES-DE writes the machine's local time without a zone, Retroverse's
  sessions are UTC. `<lastplayed>` is read in the machine's current zone with that zone's rules at that date
  (`machineLocalTimeToUtc()`): a time the clocks showed twice is its earlier instant, a skipped one the instant of the
  change (`std::chrono::choose::earliest`; checked against `to_sys` across DST changes). The converter caches the
  zone's intervals, so a conversion is a search; a time more than a century from them, or one that would keep more
  than 4,096 intervals, starts the cache again at its own (a hand-edited `<lastplayed>` of 9999 must not lock the
  others out). Without a time zone database (`machineLocalTimeToUtc()` failing) the app reads them as UTC
  (`fixedOffsetLocalTimeToUtc(0)`), with a warning.
- **One-line texts sit straight in their clipping paragraph.** RmlUi 6.3 draws `text-overflow: ellipsis` only for a
  text whose parent clips it, measured from the parent's left edge (`ElementText::GenerateGeometry`): a text inside a
  `data-if` span, or after another element, is cut through a glyph. Variants are sibling paragraphs (one text each);
  a floor's counts are a flex lead before the description that ends in the ellipsis. The unit test checks the
  document for it.
- **Ties by sort key, then GameId.** Favourites never played list alphabetically; the order is total and the same on
  every run.
- **The query scans.** `idx_games` has no index on the favourite flag or `<lastplayed>`: the read visits every row once
  and ranks in memory. 100,000 games (about 5% favourites, `<lastplayed>` on 1 in 20, sessions on 1 in 50), hidden
  `[recent_query][bench]`: a median of 140 to 190 ms in Release on a machine loaded by other work (slowest 215 to
  305 ms; Debug about 870 ms). It runs on the catalog worker when the tab opens, the tab saying it is reading
  meanwhile; no index was added (no migration for an informative cost).
- **The hall is a row.** A floor's list starts with "Corridoio e ascensore" (its badge the elevator's doors), which
  rides to the floor (`ChooseFloor`); a floor opens on the walker's room, else its first room.
- **The model closes itself** on `ChooseFloor`, `ChooseRoom`, `ShowInRoom` and `Close`; `Launch` keeps the card open,
  which then tells the outcome.

## The contract for step 7b

- **Building.** The app fills `DirectoryBuilding` from the plan: the floors that exist in `building.json`'s order,
  `order` their index there (0 the ground floor, the badge "T"), each floor's rooms in the order along its hall, each
  room's `id`, `kind` (`world::toString(RoomKind)`), its sections' systems (ES-DE name and catalog full name) and its
  placed games; `currentFloor` and `currentRoom` from the area that holds the eye ("" in a hall). `setBuilding()` again
  on a new plan: the selections follow their ids.
- **Games.** When `gamesRequest()` changes (the panel opened on the games, the tab entered by a shoulder, Left or
  Right on the tab bar, a click): `setGamesState(Loading)`, then on the catalog worker
  `favoritesAndRecent({kDefaultRecentGames, machineLocalTimeToUtc()})`, mapped to `DirectoryGame` (the GameId's text,
  title, system, its full name, favourite, `played` the local date of `lastPlayed`), `setGames()`; an error:
  `setGamesState(Failed)`. The stale list shows meanwhile; the panel opens on the new list's first game unless the
  user has moved since.
- **Card.** When `cardRequest()` changes while `view() == Card` (a card opened, the same game's again included), the
  app gives `cardGameId()`'s `GameCardContent` (the quick catalog's readiness, blocker, details) and its standing in
  the plan (`quick_catalog::kRoomPlaced`, an unplaced reason, `not-in-room` for a system with no room, `other-room`
  for a game placed elsewhere) with `setCard()` (an answer for another game is ignored). A launch's outcome comes
  back through `setCard()` too.
- **Commands.** `ChooseFloor(id)`: ride to that floor's elevator; the walker's own floor (the hall row of the floor
  the walker is on): back to its elevator without a ride. `ChooseRoom(id)`: teleport to its doorway, riding first
  when it is on another floor. `ShowInRoom(gameId)`: leave Focus, ride if needed, then the Focus pose (ADR-0012 §8).
  `Launch(gameId)`: the launch flow. `Close`: the panel's input context ends. A ride refused for now: `reopen()` (the
  panel where the command left it: the rooms list with the room selected, the card), then
  `setNotice("ride-unavailable")`; a plan not ready: `"building-pending"`.
- **Each frame** (as `tests/ui_probe/probe_app.cpp` does): the `NavInput` commands to `apply()`, `takeClicked()` to
  `click()`, then `show(model)` or `hide()` by `isOpen()`, `UiView::update()`, `afterUpdate()`; pointer events through
  `handlePointer()` (true: the world must not see the click).
