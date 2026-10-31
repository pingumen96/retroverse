# I5 step 9: the insertion animation

Roadmap row: I5 step 9. Decision: [ADR-0016](../decisions/ADR-0016-screens-and-media.md) §11 (the insertion), with §2
(the decoders' stop before a launch, at most 1 s) and ADR-0012 §6 (the launch from the world and its return). Amended
on 2026-10-31 by this step (§11: what it left open; see "Decisions beyond ADR-0016").

## What was built

- **The state** (`rv_app_cli`, `world_interaction.hpp`): `WorldState::AnimatingToConsole` between Inspect and
  Launching (specification §8.3's name). Avvia in a room whose `InteractionRoom::consolePose()` answers (a console in
  its hero corner) starts it: `InteractionTimings::insertion` (450 ms) eases the item from the hand into the console's
  media socket (`insertion()`, 0 to 1, eased) and the walker's look to the socket, its feet kept. Avvia's
  `WorldLaunchRequest` comes out at once with `held`; the insertion's end (`InteractionResult::insertionEnded`:
  `Arrived`, or `Skipped` by `anyButtonPressed()`) moves to Launching. Its context reads nothing but the skip; Back,
  the editor and "Mostra nella stanza" are refused meanwhile. `launchEnded()` during it (a refusal) is Returning, then
  Inspect with the item in the hand, the walker as Avvia found it. No console (the arcade, a shared room, the test
  room), `reduceMotion` or an insertion of 0 s: Launching at once, as before.
- **The socket and the item's pose** (`world_inspect.hpp`): `consoleSocketOf()` finds the room's console in the scene
  (`<room>/fx:<socket>.console`), builds its prefab again (no GPU) and takes its `CartridgeSlot` through the object's
  instance, with the opening's size. `insertedItemTransform()` goes from the held pose to the socket: the centre on the
  straight line, the orientation along the shorter arc to the socket's (upright, the cover to the console's front), the
  scale down to `insertedScale()` (90% of the opening's width and depth, at most 11 cm tall). `insertionDimming()`
  fades Inspect's dimming out with it. `CollectionRoom` keeps the socket (`setConsoleSocket()`, the world session's
  per room: `FloorRoom::console`, found before the floor's scene goes to the host, kept through live updates) and
  answers `consolePose()` from its viewer's eye height.
- **The hand-over after both** (`ProductUi`, `world_launch.cpp`): `launchWorldGame(..., held)` asks the catalog worker
  at once and calls the new `LaunchHooks::preparing`; a ready decision is parked until `releaseWorldLaunch()`, which
  `WorldLaunch` calls on `insertionEnded`; a refusal is told at once (and `ended` reopens the screens). The parked
  request never shows the game-session panel, so the World context keeps the input and a button can skip.
- **The stop during it** (`screen_service.cpp`, `screen_policy.cpp`, `retroverse_app.cpp`): `preparing` begins the
  mix's fade and `ScreenService::prepareLaunch()`: every slot stops (each screen keeping its clip's position), never
  waited for; the hero shows the game's title card (`ScreenPolicy::launchStarting(position, true)`: the chain's card
  at once) when that game is its target. `launchStarting()` at the hand-over waits only for what remains of the 1 s
  from the stop's start; `launchEnded()` of a prepared launch that never started records it `cancelled`.
- **The report**: `world.interaction.insertion` (`started`, `arrived`, `skipped`, `refused`, `movingFrames`,
  `socketFrames`, the last one's `pickId`, `startFrame`, `endFrame`, `seconds`, `end`, `socket`, `center`, `scale`,
  `distance` and its `frames` with progress and distance); `world.launch` events `inserted` and `insertion-skipped`;
  `video.launches` `stopFrame`, `waitMs`, `handOverFrame`, `cancelled`.

## Decisions beyond ADR-0016

Recorded in ADR-0016 §11's amendment of 2026-10-31:
- **The eye turns to the socket.** From most Focus poses the console is out of view; the item flying off screen
  would hide nothing. The look eases with the item, the feet stay; the return restores Avvia's pose.
- **"Instant under `reduceMotion`" is no frame of it**: the launch at once, the item in the hand, the eye where it is
  (snapping the view to the console would be a jump `reduceMotion` users asked not to see).
- **"Any button"**: every button action but the look and walking axes, the UI's analog navigation and the global
  actions (F12, F1, F5, F8: host functions, so that a screenshot of the insertion does not end it).
- **The resolution runs during it.** The catalog worker answers within a frame or two; parking its decision keeps the
  panel and the hand-over after the insertion, and a refusal comes back at once instead of after 450 ms.
- **"The item back on its shelf"** (the roadmap's row): after the return the item is in the hand again, as ADR-0012 §6
  restores Inspect; its slot in the plan never moved. The GPU case checks both.

## Proofs

- Unit (`rv_tests`, `[insertion]`, 10 cases):
  - `[world_interaction]`: the transition table on the hand-built shelves: Avvia with a console is AnimatingToConsole,
    `held`, the context suspended, the editor and "Mostra nella stanza" refused, 27 frames to `Arrived` at 60 Hz, the
    eye on the socket, 1 while the game runs, Inspect as it was after the return; the skip by interact, confirm, Back,
    ui_back and the quick catalog's button (Launching within the frame), none by a look or by F12 and F8, nothing read
    without the frame's actions; `reduceMotion` (and an insertion of 0 s) launches at once, turned on mid-way it
    arrives on the next update; a refusal during it back to Inspect with the item in the hand, the walker and the
    inspection as Avvia found them, the notice set, Avvia again inserting again; no console, no insertion.
  - `[world_inspect]`: the golden building `building-esde-seed-102938`: a socket in `room:gb`, `room:psx` and
    `room:snes` (on the console's top, upright, the opening at most 15 x 4.5 cm), none in `area:arcade` or the shared
    rooms; `insertedScale()`'s bounds and the golden plan's 50 cartridges; the pose at 0 the held one, at 1 the
    socket's (centre within 10 um, the axes the socket's, the scale shrunk), on the straight line between; the
    dimming fading; `consolePose()` looking at the socket; the report's records read back.
- GPU (`tests/smoke/test_world_launch.cpp`, the `esde/snes-room` fixture, the fake emulator under
  `RV_LAUNCH_ALLOWLIST`, hidden, lockstep):
  - Avvia on Amber Lantern's card at 80 (`world_insertion.json`): the insertion from 80 to 106, 27 frames, 0.4500 s of
    the frames' time; the item 1.45 m from the socket on its first frame, 0.0053 m on 105, 0.0 m on 106 (within 1 cm),
    scale 0.992; the hero's slot stopped `launch` on 80 and its thread ended 0.7 ms later, the hero showing the
    game's title card from 80; the launch's request on 106, its start on 107 (`handOverFrame` 106, the hand-over
    waiting 0.003 ms), Returned, the play session verified; Inspect of the same item restored on 107 with the walker
    as Avvia found it, the item in the hand, its slot and transform in the plan unchanged; the clip reopened at its
    position.
  - A skip (`world_insertion_skip.json`, interact at 86): `insertion-skipped` on 86, the item in the socket on that
    frame (0.0 m), the launch's request on 86 and its start on 87; the stop from 80, 0.7 ms, nothing left at the
    hand-over.
  - The arcade (`test_world_families.cpp`, a cabinet launched): no insertion, the launch at once.
  - `test_world_card.cpp`: the card's script waits for its two insertions (its frames after 118 shifted, 335 frames).
- Focus: the Debug GPU cases above ran under `scripts/focus_watch.py`: 0 foreground changes, 0 windows shown.

## Results

- Debug build without a warning; Debug unit tests (`./scripts/test.ps1`): 2247 of 2247 passed. The Debug GPU cases of
  the launches, the card and the arcade's launch (`-Filter "world launch|world card|returns to the arcade"`): 7 of 7
  passed under the focus watcher.
- Release build without a warning; Release unit and GPU (`./scripts/test.ps1 -Preset msvc-x64-release-gpu`, under
  `scripts/focus_watch.py`): 2424 tests (2247 unit, 177 GPU), all passed but the one skipped as always (the overlay's
  pick, no overlay in Release); G01 to G20 compared and passed, none skipped; 0 foreground changes, no window visible.
- No golden was rewritten: no golden scene launches a game.

## Screenshots

Hidden, frame-limited Debug runs of the GPU cases (`build/i5/step9/`, 1920 x 1080, F12 at the given frames), looked at
against `style.md` and `example.png`:
- `insertion_start_f080.png`: Avvia's frame: Amber Lantern still in the hand left of the featured shelf, the card
  gone, the scene behind about as dimmed as in Inspect.
- `insertion_middle_f093.png`: half-way: the eye turned down towards the hero corner, the cartridge at the console's
  side, near its socket and almost upright; the CRT and the walnut sideboard at the left, the shelf at the right.
- `insertion_socket_f106.png`: the cartridge standing in the console's slot, half of it in, its cover to the front;
  "Gioco in corso" over the dimmed view (the game-session panel of the hand-over).
- `skipped_f086.png`: the skip's frame, the same view at once.
- `returned_inspect_f119.png`: after the return, the item in the hand again with its card on Avvia ("Pronto: già
  avviato e tornato senza errori").

The look holds: warm wood and plastics, no new materials, the console's plastics as before. The hero CRT's title card
is not in these frames: from Amber Lantern's pose the eye on the socket leaves the CRT's screen above the view.

## Open issues

- The user's trial: the insertion's feel (its 450 ms, the eye's turn, the cartridge half in the slot) with a pad and
  a keyboard, and a real launch afterwards; no automated run starts a real emulator.
- The hero CRT's title card during the insertion is proven by the report (`video.shown`), not seen in the screenshots:
  the eye on the socket leaves the CRT's screen above the view from the poses tried. Aiming the eye between the socket
  and the screen is a possible refinement for the review.
- The real library was not run: the step touches no catalog, plan or media path (the socket is the prefab's, every
  console room has one).
- A cartridge console stands in every console room (the systems' defaults), so disc systems insert their case into a
  cartridge slot, shrunk to fit; a disc console prefab with its own socket would be a look change for a later pack.
