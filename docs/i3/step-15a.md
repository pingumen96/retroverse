# I3 step 15a: launching from the world, "Mostra nella stanza"

Roadmap row: I3 step 15a (wave 5, on 13b and 14a; 13b's inspection, 14b's media viewer and 16's look run beside it in
their own worktrees: this step keeps to new files and the hooks listed under "For the merge"). Decisions:
[ADR-0012](../decisions/ADR-0012-interaction-in-the-world.md) §6 (Avvia by GameId through A1's `LaunchFlow`, the world's
state kept across the launch, the decoder stopped and joined before and reopened after, the loader paused, a refused
launch back in Inspect with its outcome on the card) and §8 ("Mostra nella stanza" on the quick catalog's card, the
unplaced reasons in the RML document). Avvia on the inspected item's card (or the pad's Start) now launches the game as
the quick catalog's Avvia does: resolved on the catalog worker right before the launch, the game-session panel, the
play session recorded; after the return the walker stands where it stood and the same item is in the hand again, its
card on Avvia. The quick catalog's card offers "Mostra nella stanza" for a game the room places: the catalog closes and
the walker eases in 600 ms to the item's Focus pose, the item pulled out; for a game the room leaves out the card says
why. Golden G01 (`--scene m0_testroom`) is unchanged.

## What was built

| File | What it holds |
|---|---|
| `rv/app/world_launch_ui.hpp` (`rv_app_cli`, new, header only) | `roomPlacementOf()` (the card's room key from the plan's `world::PlanIndex`), `busyLaunchRefusal()` / `kReasonLaunchBusy`, `spawnOf()`, `RoomLaunchLog` (the run report's `world.launch`) |
| `rv/ui/quick_catalog_room.hpp` (`rv_ui_model`, new, header only) | the button's id `act-room` and data event `show_in_room`, the room keys `placed`, `duplicate-title`, `no-cover`, `unavailable`, `capacity`, `not-in-room` |
| `src/world_launch.hpp`, `.cpp` (`rv_app`, step 0a's stubs) | `WorldLaunch`: the plan's index, the card's placement given to the product UI, "Mostra nella stanza" taken to its item, Avvia asked of the product UI and its end given back to the interaction, the record |
| `assets/ui/quick_catalog.rml`, `.rcss` | the button "Mostra nella stanza" on a row of its own in the card's actions (shown for `card.room == 'placed'`), a sentence per unplaced key ("Non esposto: un'altra versione è sullo scaffale", "Non esposto: senza copertina", "... il file del gioco mancava quando la stanza è stata allestita", "... la stanza è piena", "... questa stanza raccoglie i giochi di un altro sistema") |
| `rv/ui/quick_catalog_screen.hpp`, `src/quick_catalog_screen.cpp` | `QuickCatalogEvents::showInRoom`, `setRoomPlacement()`, the card's `room` member, the `show_in_room` event |
| `src/product_ui.hpp`, `.cpp` | `launchWorldGame()`, `takeWorldLaunchEnded()`, `setRoomPlacement()`, `takeShowInRoom()`; `Impl::launch()` returns the token; the world's token followed in `applyDecision()` (a refusal) and `afterReturn()` (the end) |
| `src/world_hover.hpp`, `.cpp` | the "ended at once" branch removed (Avvia passes out as `result.launch`); `launchEnded()`, `showInRoom()`, `pickOf()` |
| `src/world_session.hpp`, `.cpp`, `src/world_live_update.cpp` | the `WorldLaunch` member; `beforeInteraction()`, `afterInteraction()`; `keepShown()` gives it the plan, `dropCollection()` takes it; `record()` writes `world.launch` |
| `src/retroverse_app.cpp` | `onUpdate`: `beforeInteraction` and `afterInteraction` around `interactWorld` |
| `rv/app/run_report.hpp`, `src/run_report.cpp` | `RoomLaunchRecord`, `RoomLaunchEventRecord` under `WorldRecord::launch` (`world.launch`, optional when read) |
| tests | `tests/unit/app/test_world_launch.cpp` (new); `tests/smoke/test_world_launch.cpp` (new), `test_world_card.cpp` (Avvia now launches: the fake's profile assigned, the frames after the returns); `tests/input_scripts/world_launch.json`, `world_show_in_room.json`, `world_show_unplaced.json` (new), `world_card.json` (comment) |

## One launch from the room

```text
frame N (Inspect, World context)
  WorldSession::beforeInteraction        nothing pending
  WorldHover::update                     Avvia (the card's press, the pad's Start): Launching, the launch record
                                         (item, GameId, walker pose, inspect view) kept by WorldInteraction
  WorldSession::afterInteraction         ProductUi::launchWorldGame(GameId, title): Impl::launch() as the quick
                                         catalog's Avvia (AppCatalog::requestLaunch: the ES-DE catalog resolves the game
                                         on its worker; lockstep runs wait for it); refused at once (busy): launchEnded
                                         with not-ready/busy, back to Inspect next frame
frame N+1                                ProductUi::update: the decision -> LaunchFlow::request ("Gioco in corso"), or
                                         refuse (the Outcome panel; the world's end = the refusal)
host idle: onIdle                        LaunchHooks::starting: ScreenService::launchStarting (the decoder stopped, its
                                         thread waited for within 1 s); the hand-over; the process (the guard applies);
                                         the cover loader's rounds paused (launchActive()); nothing rendered
the return                               LaunchHooks::ended: ScreenService::launchEnded (reopened at the position);
                                         the world's end = LaunchFlow::message()
frame M                                  beforeInteraction: takeWorldLaunchEnded -> WorldInteraction::launchEnded
                                         WorldHover::update: Returning -> Inspect of the same item, the walker placed at
                                         the recorded pose (result.walker); afterInteraction records "restored"
                                         ProductUi::update notes LaunchRecord::after (the walker equal to before)
```

## Decisions taken within the step

1. **The world's launch is the quick catalog's**: `ProductUi::launchWorldGame` builds a `ui::CatalogEntry` of the GameId
   and the room's title and calls the same `Impl::launch()`, so the resolution right before the launch, `LaunchFlow`,
   the play session, the launch records (`before`, `after`), the hooks (decoder, loader) and the guard apply unchanged.
   No GameId overload was added to `AppCatalog` (ADR-0012 §6 names one): both catalogs read only the entry's id and
   title, and the product UI is the one caller.
2. **The end comes back through the product UI by token**: the token of the world's launch is kept until the flow's
   return (`afterReturn`) or the decision's refusal (`applyDecision`); the interaction is told on the next frame, before
   its update, so the restore (pose and Inspect) happens in the frame that follows the return, as the quick catalog's
   `LaunchRecord::after` is noted.
3. **An Avvia the product UI cannot take** (a launch decided, waiting or running) ends at once as `not-ready` with the
   reason `busy`: the interaction never stays Launching without a launch.
4. **The card's room key is asked by the screen when the card fills** (`QuickCatalogScreen::setRoomPlacement`, a
   function of the entry): the screen knows nothing of the room; the product UI keeps the function until the screen
   exists and the world gives a new one whenever the plan shown changes (the card shown fills again). A game the plan
   places but the room shows no item of (a skipped placement) counts as not in the room.
5. **"Mostra nella stanza" is pressed in the catalog and taken on the next frame**: the screen's event closes the catalog
   at the end of the frame's commands (as Back at its root does), and the next frame, back in the World context, calls
   `WorldInteraction::showInRoom` before the interaction's update, whose result carries the 600 ms ease.
6. **The button stands on a row of its own** below Avvia, Vedi media and Informazioni (in the actions region: Down from
   Avvia reaches it), so that the three actions keep their row at every layout.

## For the merge

- **Hooks in shared files** (13b, 14b and 16 touch the app's world wiring and the quick catalog too):
  - `src/app/src/world_hover.hpp`, `.cpp`: the "ended at once" branch of `update()` removed (two comment lines in its
    place) and three small methods (`launchEnded`, `showInRoom`, `pickOf`);
  - `src/app/src/world_session.hpp`, `.cpp`: the include, the `m_launch` member, `beforeInteraction()` and
    `afterInteraction()` (one line each), one line in `dropCollection()` and in `record()`;
    `src/app/src/world_live_update.cpp`: one line in `keepShown()`;
  - `src/app/src/retroverse_app.cpp`: the two calls around `interactWorld()` in `onUpdate`;
  - `src/app/src/product_ui.hpp`, `.cpp`: four public methods, `Impl::launch()` returning the token, two lines in
    `applyDecision()` and `afterReturn()`, the `showInRoom` event where the catalog's screen is made;
  - `src/ui/include/rv/ui/quick_catalog_screen.hpp`, `src/ui/src/quick_catalog_screen.cpp`: the event, the card's
    `room` member, `setRoomPlacement()`; `assets/ui/quick_catalog.rml`, `.rcss`: the button and the sentences after
    the actions (14b's Vedi media and Informazioni of the quick catalog sit next to them);
  - `src/app/include/rv/app/run_report.hpp`, `src/run_report.cpp`: `RoomLaunchRecord` under `WorldRecord::launch`.
- **13b**: Inspect is restored by the model (lift 1, the recorded yaw, pitch and distance): its drawing of the held item
  shows it at once after the return. The CRT following the inspected item is 13b's; the launch's stop and reopening
  apply to whatever clip it plays.
- **14a's `test_world_card.cpp`** now assigns the fake's profile to snes (Avvia launches it twice) and expects the card
  shown again at frames 120 and 132 (the decision the frame after Avvia, the return the frame after that).
- **tests/smoke/test_world_launch.cpp** repeats helpers of `test_world_card.cpp`; fold them into a shared header.
- **CLAUDE.md** (not edited by the step):
  - "Running the app": Avvia on the world's card (or Start) launches the game as the quick catalog's Avvia does; after
    the return the walker stands where it stood and the item is in the hand again; the hero CRT's decoder stops before
    the launch and reopens at the clip's position after it; the covers wait meanwhile. The quick catalog's card offers
    "Mostra nella stanza" for a game the room places (the walker eased to its item's Focus pose) and says why for one
    it does not.
  - `--run-report`: `world.launch` (`launches`, `returned`, `notReturned`, `showInRoom`, `showInRoomFailed`,
    `walkerBefore`, `walkerAfter`, `poseRestored`, `placementQueries`, `placementGame`, `placement`, `events`: launch,
    refused, ended, restored, show-in-room, show-in-room-failed).
  - GPU smoke tests: `gpu.world launch: ...` (`tests/smoke/test_world_launch.cpp`); input scripts `world_launch.json`,
    `world_show_in_room.json`, `world_show_unplaced.json`.
  - "Layout and layering": `rv_app_cli`'s `world_launch_ui.hpp`, `rv_ui_model`'s `quick_catalog_room.hpp`, `rv_app`'s
    `world_launch`.
- **docs/architecture.md**: the world's launch goes through the product UI's (one LaunchFlow, one token).
- **docs/roadmap.md**: row 15a done, with the results below.

## Tests

- `tests/unit/app/test_world_launch.cpp` (`[app][world_launch]`): the room key of every placement and unplaced reason
  (a hand-built plan with all four reasons, a game it does not hold, a placement the room shows no item of), the world's
  reason names equal to the document's keys; the quick catalog's document (the button in the actions region after
  Informazioni, its data event, a "Non esposto: ..." sentence for every key, no transition); the busy refusal; the
  record as the app's loop drives the interaction (step 7's harness) through a launch, its return (the walker moved
  meanwhile and restored exactly) and a refusal, and through "Mostra nella stanza" (the ease to the item's Focus pose,
  an item the room lost refused); the record read back from the run report.
- `tests/smoke/test_world_launch.cpp` (`gpu.world launch: ...`, hidden, lockstep, snes-room imported, the fake's
  `fake-return` profile assigned to snes, `RV_LAUNCH_ALLOWLIST` the fake):
  - **Avvia** (`world_launch.json`, 120 frames): the launch asked at frame 80, requested at 81, `Returned` with exit code
    0 and the fake's 2.5 s, no frame rendered while it ran; `LaunchRecord::before.camera == after.camera`; the play
    session in the catalog database, returned and verified; `world.launch`: launch, ended (returned) and restored
    (Inspect of the same item) on one frame, `poseRestored`, the final camera the pose at Avvia; the card shown at 40 and
    again on the restore frame, hidden at 80, on the same game; the hero CRT's decoder ran at the launch, its thread
    ended before the hand-over (`threadsRunning` 0, stopped in about 1 ms) and reopened after the return (at the
    clip's position when a frame of it had shown).
  - **Mostra nella stanza** (`world_show_in_room.json`, 80 frames): Acorn Errands searched, Down from Avvia reaches the
    button, pressed at 23; at 24 `show-in-room` on its GameId, the catalog closed, no launch, the interaction in Focus of
    it, at least 30 eased frames, the final camera at the item's Focus pose (1 mm, 0.05 degrees). **An unplaced game**
    (`world_show_unplaced.json`, Thistle Trellis): the card's key `no-cover` for it, no event, the catalog still open, no
    card squeezed.
- `tests/smoke/test_world_card.cpp` (14a's) with real launches: shown at 40, 120, 132, 200; two launches returned.

## Results

Debug only, on the reference machine (RTX 2080 SUPER), `RV_LAUNCH_BACKGROUND` set, `RV_LAUNCH_ALLOWLIST` the fake,
other worktrees building meanwhile (the compiler ran out of heap at `-j 6`; built at `-j 3`):

- Build `msvc-x64-debug` with 0 warnings.
- `rv_tests "[app],[ui]"`: 312 passed, 1 skipped (the hidden bench).
- `rv_smoke_tests "[world_launch]"`: both cases passed; `"[world_card],[world_focus],golden*"`: 7 cases passed, golden
  G01 0 pixels apart (the run has no world).
- The cover loader's `pausedRounds` was 0 in the Avvia case: lockstep runs have every cover resident before Avvia, so the
  paused rounds had nothing to hold back (they count rounds with work held); the pause itself is step 12's (tested on
  `CoverSchedule`), and no frame is rendered while the game runs.
- Screenshots (not committed, the fixture's invented titles): `build/msvc-x64-debug/smoke/world_launch/avvia/
  returned.png` (Trellis Tales in the hand again, its card on Avvia, "Pronto: già avviato e tornato senza errori": the
  session verified), `show_in_room/show_in_room.png` (Focus on Acorn Errands on the wall shelf, its label),
  `show_in_room/unplaced.png` (Thistle Trellis's card: "Non esposto: senza copertina", no button).
