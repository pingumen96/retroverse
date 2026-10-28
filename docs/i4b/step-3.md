# I4b step 3: the editor in the app, games

Roadmap row: I4b step 3. Decision: [ADR-0015](../decisions/ADR-0015-customization.md) §4 (amending ADR-0012 §1), §6,
§9. Plan: [plan.md](plan.md).

## What was built

- **The state `Edit`** (`WorldState::Edit`, `WorldContext::Edit`, `WorldInteraction::enterEdit()` / `leaveEdit()`):
  the interaction lets go of everything (nothing hovered, focused, pulled or lifted) and reads nothing; its context
  reads the walk, the look, Sprint, interact, Back, confirm, the new `edit_pin` (P, the pad's west button), the arrows
  and ui_activate; never the quick catalog nor Map. The walker walks and looks as in free roam; a held arrow steps
  instead of walking (the arrow keys are bound to both).
- **The editor's interaction** (`rv/app/world_edit_interaction.hpp`, GPU-free): Preparing, Browse, Holding, Busy,
  Menu, Saving. interact picks up the game under the reticle; on an empty slot it asks a move, on another game a swap,
  on its own slot it puts it back; Back puts it back; `edit_pin` pins or unpins in place; the D-pad steps over every
  slot, empty ones included, the walker eased in front of it (Focus's ease) until the player looks or walks; confirm
  (Enter, Start) opens the menu: Salva, Annulla ultima azione, Esci senza salvare, Continua; Back with pending changes
  opens it too, without any it leaves. The shell's Back levels come first; the editor's own always before Quit.
- **The edited room's targets** (`rv/app/world_edit_room.hpp`): the layout's items and its **free slots**
  (`world::CollectionLayout::freeSlots`, new: every slot of a navigation fixture that holds the family's item in its
  level's stance and no placement, bare or dressed, with the pick id an item there would have; `NavSlot::slot`, the
  slot's own pick id) in one pick index with the layout's occluders, and Focus's steps over both.
- **The draft** (`rv/app/world_edit_draft.hpp`, `rv_app_catalog`): each operation applied by the frozen `applyEdit()`
  to a copy of the stored plan and overrides, kept with what it made (a refusal or a no-op is not), undone back to the
  base byte for byte; `saveDraft()` takes the world lock, reads the plan (lifted as the app reads it) and the overrides,
  commits the draft as it is when they are the session's base, else replays the operations with `applyEdits()` (a
  refusal writes nothing and names the operation and the rule), through `commitWorld()` (action `edit`, scope the room).
  `prepareEditData()` reads the configuration, the building and every room of the floor.
- **In the app** (`src/app/src/world_edit.cpp`, `world_edit.hpp`): "Modifica stanza" above the directory's floors
  (`DirectoryBuilding::editableRoom`, `DirectoryCommandKind::EditRoom`: the room that holds the eye, with its
  collection, while no floor is on its way, no ride, no launch) begins a session; one job at a time on the pool
  prepares it (the data, `overrides.json`, the floor of the plan shown), applies each operation and lays out what to
  show, undoes, discards and saves. What shows goes through the live update's room-by-room path
  (`planLiveUpdate(..., movesInPlace)`): a move or swap onto bare slots lays the edited room out again on the floor shown
  (`editedRoomCollections()`) and applies its collection in place (`applyLayout()` keeps every game's pictures by its
  layer keys); an edit whose dressing gives way builds the floor again from the draft and commits it with the walker
  kept. The held game leaves its slot (its static records hidden) and is drawn in the hand, smaller, turned a little;
  over an empty slot a copy of it stands there outlined in warm white; a game targeted is outlined. Salva ends the
  session (a replay over a changed plan shows the plan saved); Esci senza salvare shows the stored plan again. A
  refreshed catalog's live update waits for the session's end. `overrides.json` that cannot be read (a newer schema)
  edits nothing: the world's notice `edit-unavailable`. Lockstep runs wait between frames for the session's job.
- **The bar** (`assets/ui/edit_bar.rml`, `.rcss`, `EditBarPanel`, `ProductUi::setEditBar()`): a band at the bottom in
  the quick catalog's look (the room, the target or the game in the hand, "Fissato", "In vetrina", the pending count,
  the last refusal's sentence, the keys of the device used last), the menu above its right end. It takes no input.
- **The run report's `world.edits`**: each session's room, entry, operations (their refusal, rebuild, path, job and
  display times), undos, menus, end, save (written, replayed, refusal, snapshot, digest, times) and the live updates it
  deferred.

## Decisions beyond ADR-0015

- **"Sposta" on the card is wired but not shown.** `CardAction::Move` and the session's entry with the item held are
  in place and unit-tested, but a fifth button on the card changes golden G07 (the inspected item with its card), which
  this step must not move. Adding `CardButton::Move` and its button in `game_card.rml` is one small change once the user
  agrees to regenerate G07; until then "Modifica stanza" is the way in (then interact picks the game up).
- **"Modifica stanza" stands above the directory's floors**, reached with Up from the first floor (then the tab bar),
  not as a floor row, so the floors' indices, ids and the I4 step 7a probe model are unchanged.
- **One job at a time; input waits while it runs** (Busy, Saving): the draft is never ahead of what shows, and a
  scripted run shows each edit on the frame after it was asked.
- **Free slots join the layout** (rv_world, outside the plan's "rv_app, rv_ui, rv_input" for app steps): computed by
  `collectionLayout()` with the items, never in its canonical document, so no layout or scene golden changes.
- **The shown draft has a synthetic digest** (`edit:<session>:<n>`; the stored one again after an undo to the base or
  a discard, the saved one after Salva), so nothing mistakes a draft for a file.
- **The walker gets the raw actions in the world's context** (one line in `retroverse_app.cpp`): equivalent for every
  other context, it lets the editor's walker see a held arrow.

## Measurements

Release, reference machine, lockstep runs (each time from the request, on the main thread's frame, to the frame that
shows it; the job is the pool's share):

| Edit | Room | Path | Job | Shown | Acceptable (ADR-0015 §9) |
|---|---|---|---|---|---|
| A swap | overflow's amiga room, 1,561 big boxes (computer floor) | in place | 22 to 25 ms | 106 to 112 ms | 0.25 s |
| An unpin (the digest alone) | the same | in place | 5 ms | 42 ms | 0.25 s |
| A move into a dressed slot | building's room:snes, 40 titles (gen4) | the floor built again | 23 ms | 85 to 88 ms | 0.25 s |
| A swap | the same | in place | 5 ms | 16 ms | 0.25 s |

The session's start (the floor's data and its floor read on the pool) took 211 ms in the amiga room, 31 to 36 ms in
room:snes; Salva 186 ms and 64 ms, the room back 195 ms and 65 ms after it; Esci senza salvare's room back 77 ms
(acceptable: 3 s warm). Debug is about ten times slower (the swap in the amiga room 956 ms). The in-place apply runs on
the main thread: in the amiga room it is about 80 ms, one long frame per edit (the bench run's p99 73 ms counts lockstep
waits; an interactive session over 50 ms frames is the open issue below).

## Proofs

- Unit (`tests/unit/app/test_world_edit.cpp`, 9 cases): the targets (every item and free slot once, the steps from an
  item to the free slot beside it, a moved game taking the slot's pick id), dressed free slots equal to the plan's
  dressing (golden plans); the Edit context's actions and the walker's arrows; the interaction's pick up, move, swap,
  refusal kept in the hand, put back (Back, its own slot), pin and unpin, the D-pad steps and the reticle taking back,
  the menu, Back's levels (a popup first, never Quit), "Sposta" holding the item; the draft's history (refusals and
  no-ops not kept, undo to the base byte for byte, adopt); a save behind a snapshot undone by `undoLastAction()` to the
  plan's SHA-256, a second save writing nothing; a save replayed over another writer's swap (both stand) and over a
  move into the session's slot (`slot-taken`, nothing written). `test_edit_bar_document.cpp`: the bar's ids, model,
  every mode, refusal and menu button, the directory's "Modifica stanza". `test_directory.cpp`: its focus, Activate and
  click. `test_run_report.cpp`: `world.edits` round trip. `test_action_map`, `test_input_system`: `edit_pin`.
- GPU (`tests/smoke/test_world_edit.cpp`): `gpu.world_edit: a scripted session ...` on the building fixture's
  room:snes (`world_edit_session.json`): a display case's game moved into the free (dressed) slot beside it (the floor
  built again), two games swapped (in place), Salva; the stored plan holds them there with three editor pins, the
  snapshot is there, a restart writes nothing and shows the same digest; runs stopped at frames 105 and 280 leave the
  plan as it was. `gpu.world_edit: a session left with "Esci senza salvare" ...`: the plan's SHA-256 unchanged, no
  `overrides.json`. `[.bench]`: the overflow fixture's amiga room (below).
- No golden changed.

## Screenshots

`build/i4b/step3/`: `holding.png` (a game in the hand over the free slot, where it would stand outlined, the bar
"Posalo qui: la decorazione fa spazio."), `menu.png` (the menu over the bar, "2 modifiche"), `session.png` (after Salva,
the swapped games in the case), `directory.png` ("Modifica stanza" focused above the floors), `bench.png`.

## Open issues

- "Sposta" on the card (above): needs G07 regenerated on purpose.
- An edit that makes dressing give way builds the whole floor again (85 ms on a small floor; the reference library's
  computer floor took 0.72 s for a ride's floor job): a room-only rebuild of the static part would need the scene per
  room (step 4's fixture moves have the same cost).
- The in-place apply (the room's collection, the cover loader adopting the floor's pictures, the plan's copies) takes
  about 80 ms on the main thread in a room of 1,561 items: one frame over the 50 ms row per edit. Narrowing it (only
  the edited room's loader items, the plan state shared) is a follow-up if the user feels it.
- The bar's actions are not clickable (pad and keyboard only); the hover's title label does not show in the editor
  (the bar names the target).
- The overflow fixture stands for the reference library's largest room, not its plan (20,000 placements: the draft's
  copies and the floor's validation grow with it); the real library's measure is Gate M3's.
