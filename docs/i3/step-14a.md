# I3 step 14a: the card beside the inspected item

Roadmap row: I3 step 14a (wave 4, on 7, 10b and 12; 13b and 15b run beside it in their own worktrees). Decisions:
[ADR-0012](../decisions/ADR-0012-interaction-in-the-world.md) §1 (Inspect's input context: the arrows move between the
card's buttons), §5 (the card: title, cover, metadata, readiness and first blocker with the quick catalog's sentences,
other versions, the buttons Avvia, Vedi media, Informazioni, Indietro by pad, keyboard and mouse; pictures through
`rv-texture:`). While the interaction is in Inspect a card shows on the right of the view with the inspected game: its
cover (the item's hero texture), title, platform and year, readiness and first blocker, developer, genre, players and
other versions, and its four buttons in a 2 x 2 grid, Avvia focused. The arrows and the D-pad move the focus, Enter and
the pad's south button press the focused button, a click presses the one clicked; the pad's Y, X and Start stay
shortcuts of Vedi media, Informazioni and Avvia. Avvia still ends at once (launching from the room is 15a's); Vedi
media and Informazioni are recorded and open nothing yet (14b).

## What was built

| File | What it holds |
|---|---|
| `rv/ui/game_card_model.hpp`, `src/game_card_model.cpp` (`rv_ui_model`, step 0a's stubs) | `ui::game_card` (document, ids, data model, `rv-texture:game-card-cover`, the outcome codes it words), `CardButton`, `stepFocus()` (the 2 x 2 grid, no wrap), `GameCardContent`, `GameCardModel` (focus; Activate presses the focused button, the shortcuts theirs, a click focuses and presses; Avvia for every new game) |
| `rv/ui/game_card_screen.hpp`, `src/game_card_screen.cpp` (`rv_ui`, step 0a's stubs) | `GameCardScreen`: the data model `game_card` ("card"), the document shown without taking the world's input, the model's button focused (`:focus`), clicks through the `press('<button>')` data event, `handlePointer()` (true only for a button event over the card's own document), `squeezed()`, `coverBox()` |
| `assets/ui/game_card.rml`, `.rcss` (new) | the card: the readiness and blocker lines copied from `quick_catalog.rml` unchanged, "Pronto" for a ready game not yet verified, a sentence per launch outcome code, "n.d." for empty fields, the hints; the document is the card itself (right 4%, top 7%, 31% wide), a compact theme |
| `third_party/rmlui_backends/RmlUi_Renderer_GL3.*`, `README.md` | local change 4: `SetExternalTexture(key, texture, size)`, `ExternalTexture()`; `rv-texture:<key>` loads as a handle of the key's slot (handles from 2^40), bound when drawn (nothing drawn while empty), never deleted by RmlUi |
| `rv/ui/ui_system.hpp`, `src/ui_system.cpp` | `UiSystem::setTexture()`, `texture()` |
| `rv/app/world_card_ui.hpp` (`rv_app_cli`, new, header only) | `otherVersionCounts(plan)` (duplicate-title entries by representative), `cardContentOf()` (the catalog's row, else the room's title with the details to come; the outcome of a launch that did not return), `cardCommandOf()` (arrows, ui_activate), `cardActionOf()` |
| `src/world_card.hpp`, `src/world_card.cpp` (`rv_app`, step 0a's stubs) | `WorldCard`: `press()` before the interaction (the frame's arrows and ui_activate in the Inspect context, the clicks; presses wait for the item in the hand), `update()` after it (the content, the catalog's row read once per game and again when `ProductUi::catalogRevision()` moves, the cover: an RGBA8 `glTextureView` of the sRGB hero texture registered as `game-card-cover`), the run report's `world.card` |
| `rv/app/world_interaction.hpp`, `src/world_interaction.cpp` (step 7's) | `CardAction`, `InteractionFrame::card` (the entry for a card button); `readInspect()` no longer maps ui_activate to Avvia: Avvia is the card's press or confirm without ui_activate (the pad's Start; Enter is both and presses the focused button); Indietro is ui_back |
| `rv/app/app_catalog.hpp`, `catalog_database_source.hpp`, `src/catalog_database_source.cpp` | `AppCatalog::gameEntry(GameId)` (default nothing; the ES-DE catalog's `CatalogDatabaseSource::entryOf()`: a page of one on the main thread's connection, its readiness asked of the worker like a row's) |
| `rv/app/run_report.hpp`, `src/run_report.cpp` | `CardRecord`, `CardEventRecord` under `WorldRecord::card`; `FieldReader::numberArray()` |
| tests | `tests/unit/ui/test_game_card.cpp`, `tests/unit/app/test_world_card_ui.cpp` (new); `test_world_interaction.cpp` (card cases, Enter no longer launches by itself), `world_interaction_harness.hpp` (`press()`); `tests/smoke/test_world_card.cpp`, `tests/input_scripts/world_card.json` (new) |

### Hooks in shared files (13b and 15b touch the app's world wiring too)

- `src/app/src/world_hover.hpp`, `.cpp`: `Frame::card` and its one line into `InteractionFrame`; `item(PickId)` (inline).
- `src/app/src/world_session.hpp`, `.cpp`: members `m_card` (`WorldCard`) and `m_otherVersions`; `cardPress()`,
  `showCard()`; in `showRoom()` one line (`m_card.bind()`), in 15b's `keepShown()` (`world_live_update.cpp`) one line
  (`m_otherVersions = otherVersionCounts(plan)`, so a live update's plan counts too); in `record()`
  one line (`world.card`).
- `src/app/src/retroverse_app.cpp`: `interactWorld()` sets `frame.card = world->cardPress(...)`; `onUpdate` calls
  `world->showCard(worldResult, *product)` after the label; `onEvent` gives the card the mouse events while the mouse is
  free (`ProductUi::handleWorldCardEvent`) before the product UI's own block.
- `src/app/src/product_ui.hpp`, `.cpp`: the card's screen next to the label's panel (`syncCard()`, hidden under the
  catalog and the game-session panel, counted in `visible()`), `setWorldCard()`, `handleWorldCardEvent()`,
  `takeWorldCardClick()`, `worldCardSqueezed()`, `worldCardCoverBox()`, `setUiTexture()`, `catalogRevision()` (one line
  in `syncCatalog()`).

## One frame

```text
onUpdate (room mode, Inspect)
  interactWorld: WorldCard::press (the arrows move the model's focus; ui_activate or a click presses, once the item is
                 in the hand) -> WorldHover::Frame::card -> WorldInteraction::update (Launch: Avvia; Media, Info: the
                 result's showMedia / showInfo; Back: ui_back's levels)
  the label; WorldSession::showCard -> WorldCard::update: the content (catalog row, other versions, hero view) ->
                 ProductUi::setWorldCard (shown at ProductUi::update, with the model's button focused)
onEvent: a mouse event while the mouse is free -> the card (a click on a button: press('<button>') -> the next press())
```

## Decisions taken within the step

1. **The card's focus is its model's, not RmlUi's spatial navigation**: a 2 x 2 grid (Avvia, Vedi media / Informazioni,
   Indietro), no wrap, every button within two steps; the screen focuses the model's button for the `:focus` style. The
   world keeps the input context (the card takes no keys or pad buttons from RmlUi), so the look actions still turn the
   item and the context rules of step 7 hold.
2. **Enter presses the focused button; the pad's Start is Avvia**: Enter is bound to confirm and ui_activate, so
   `readInspect` launches on confirm only without ui_activate. The pad's Y and X stay shortcuts.
3. **The card shows from the first frame of Inspect; its presses wait for the item in the hand** (the press that skips
   the lift presses nothing), but Indietro, like ui_back, is taken at any time.
4. **The readiness and blocker sentences are copied into `game_card.rml` and kept equal by a test** that reads both
   documents (RmlUi has no include for fragments); the card adds "Pronto" for a ready game not yet verified, which the
   quick catalog's card leaves unsaid.
5. **The cover is an RGBA8 view of the hero texture**: the UI blends sRGB-encoded values with `GL_FRAMEBUFFER_SRGB`
   off, so sampling the sRGB storage would darken it; the view shares the storage and its mips. The renderer's slot is
   an indirection, so the document's `<img>` survives a change of game or texture.
6. **The catalog's row is read on the main thread, once per game** (and when the catalog's rows changed): a page of
   one, as the quick catalog's pages are.
7. **The card is not used to frame Focus** (step 13a's note): Focus aims the reticle at the item so that the hover stays
   on it; framing it off-centre in the area the card leaves would move the reticle off the item. Inspect, where the card
   shows, holds the item at the view's centre (13b), clear of the card.

## Tests

- `tests/unit/ui/test_game_card.cpp` (`[ui][game_card]`): the grid's steps and reachability, the model's presses and
  clicks, Avvia for a new or reshown game, names and ids; the document's ids, data bindings, cover source (no file),
  buttons in focus order with their `press()` events and words, every readiness, blocker and outcome key, no
  `transition` and no `px` in the style sheet; **the readiness and blocker sentences equal to `quick_catalog.rml`'s,
  condition by condition**.
- `tests/unit/app/test_world_card_ui.cpp` (`[app][world_card]`): the content from a catalog row and before it, the
  outcome of a launch that did not return, the other versions from a plan, the commands of a frame, the entries.
- `tests/unit/app/test_world_interaction.cpp` (`[card]`): ui_activate and Enter launch nothing alone, Start does; the
  card's Launch, Media, Info and Back; nothing outside Inspect or without the frame's actions; presses wait for the
  lift, Indietro does not.
- `tests/smoke/test_world_card.cpp` (`gpu.world card: ...`, hidden, lockstep, snes-room imported; `world_card.json`):
  at frame 145 Inspect with the card on Avvia, the catalog's title, "Lumen 16 Home System", a year in 1990-1998, a
  readiness key; **the cover's pixels equal to the fixture's cover made into the hero picture** (box-filtered to the
  footprint, the box's border left out: mean difference 8.3 of 255, required under 12; luma spread 203); over the whole
  script the focus events 70 media, 82 back, 88 info, 100 play, 150 media, 156 back; the presses 76 media, 94 info,
  130 play, 170 back (all `activate`); media at 76 and 106 (Y), info at 94 and 112 (X), launch at 118 (Start) and 130;
  shown at 40, 119, 131, 200 and hidden at 118, 130, 170 (Indietro), 230 (Back); the interaction in Focus at 170 and
  230; `squeezedFrames` 0.

## Results

Debug only, on the reference machine (RTX 2080 SUPER), `RV_LAUNCH_BACKGROUND` set, other worktrees building meanwhile:

- Build `msvc-x64-debug` with 0 warnings.
- `rv_tests "[app],[ui]"`: 300 passed, 1 skipped (the hidden bench).
- `rv_smoke_tests "[world],golden*"` (the tag also selects the hidden benches): the step's case, the 3 `world focus`
  cases, golden G01 (0 pixels apart) and the other `[world]` cases passed; under the machine's load
  `world: without a lockstep ...` and `world: a ROM added ...` failed on timing (the room still generating, the first
  frame) and passed when run again alone; the hidden snes-stress bench (`covers: the figures ...`) did not get every
  item resident within its wait under the load (a bench for a quiet machine, not this step's).
- Screenshots (not committed, the fixture's invented titles): `build/msvc-x64-debug/smoke/world_card/buttons/card.png`
  (the card beside "Trellis Tales": cover, "Lumen 16 Home System, 1995", "Pronto", Paper Owl, Sport, 1-4, Avvia
  focused in tungsten, the hints), `end.png` (Focus again, no card, the hover label back). The item stays at its
  pulled pose in Inspect until 13b lifts it.
- The product UI's CPU section in the Debug smoke run: p50 1.1 ms with the card shown (the Release budget of 1 ms on
  quiet frames is the gate's to measure; no Release build in this step).

## Open issues

- **Clicks need a free cursor**: in Inspect the mouse stays captured (13b's mouse rotation reads the captured look), so
  a click on the card works only while the mouse is free (the developer overlay open, the window refocused). ADR-0012
  §1 also speaks of "mouse drag" rotation, which would free the cursor in Inspect: decide at the 13b merge. No input
  script can click, so the click path is covered by the model's and the document's tests and the screen's code, not by
  a smoke case.
- **"Other versions"** counts the plan's duplicate-title entries; the snes-room item checked has none, so the row is
  not seen in a screenshot.
- The card's cover shows nothing until the hero texture is resident (lockstep runs wait for it; interactive runs show
  the empty frame for a few frames).

## For the merge

- **Order**: the hooks above are small additions next to 13a's; 13b's lift and dimming and 15b's live updates touch
  `world_hover`, `world_session` and `retroverse_app.cpp` around them. 15a replaces the "ended at once" branch of
  `WorldHover::update`; `WorldCard` records a launch when Inspect gives way to Launching or Returning, so it keeps
  working, and the card shows the outcome code of a launch that did not return (`InteractionNotice::LaunchOutcome`).
- **13b**: the inspected item is at the view's centre; the card covers the right 35% of the view from 7% to at most
  93% of its height. If Inspect frees the mouse (drag rotation), the card's clicks start working without change.
- **14b**: Vedi media and Informazioni arrive as `InteractionResult::showMedia` / `showInfo` (the card's presses and
  the pad's Y and X alike); the media viewer can show pictures the same way (`UiSystem::setTexture`).
- **tests/smoke/test_world_card.cpp** repeats helpers of `test_world_focus.cpp`; fold them into a shared header.
- **CLAUDE.md** (not edited by the step):
  - "Running the app": in Inspect a card beside the item shows its game (cover, title, platform, year, readiness and
    first blocker, developer, genre, players, other versions); the arrows and the D-pad move between Avvia, Vedi media,
    Informazioni and Indietro, Enter or A presses the focused one, a click (mouse free) the one clicked; Y, X and Start
    are Vedi media, Informazioni and Avvia; Esc / B is Indietro.
  - `--run-report`: `world.card` (`shownFrames`, `coverFrames`, `squeezedFrames`, what it showed, `focused`,
    `coverBox`, `events`: shown, hidden, focus, press, launch, media, info).
  - GPU smoke tests: `gpu.world card: ...` (`tests/smoke/test_world_card.cpp`); input script `world_card.json`.
  - "Layout and layering": `rv_ui_model`'s `game_card_model`, `rv_ui`'s `game_card_screen`, `rv_app_cli`'s
    `world_card_ui.hpp`, `rv_app`'s `world_card`; the vendored renderer's `rv-texture:` images
    (`UiSystem::setTexture`).
- **docs/roadmap.md**: row 14a done.
