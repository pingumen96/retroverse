# I4 step 7b: the elevator, the directory and the ride

Roadmap row: I4 step 7 (its second half: [step-7a.md](step-7a.md) made the panel). Decisions:
[ADR-0013](../decisions/ADR-0013-the-building.md) §9, §10, §13, [ADR-0012](../decisions/ADR-0012-interaction-in-the-world.md)
§8; step 7a's contract.

## What was built

- **Opening the directory.** In front of a floor's elevator (`Hall::elevatorZone`, in the scene's frame:
  `WorldBuild::elevatorZone`) the prompt shows (`assets/ui/elevator_prompt.rml`: "Ascensore", the interact key of the
  device used last, "Scegli il piano"; `rv/app/elevator_prompt_ui.hpp`) and interact opens the directory on its floors;
  Map (M, the pad's Back/Select) opens it anywhere in free roam and closes it. It is the product UI's (`ProductUi`):
  the UI's input context (the Ui context reads Map too), the Back level `DirectoryBack` after the quick catalog's.
- **The directory's data** (`rv/app/directory_data.hpp`, GPU-free): `directoryBuildingOf()` (the plan's floors by
  `Floor::order`, each floor's rooms along its hall by the middle of their footprint, the north row first on a tie, the
  sections' full names from the catalog, the placed games), refreshed after every commit with the walker's floor and the
  room that holds the eye; the favourites and recent games asked of the catalog worker when the model asks
  (`CatalogWorker::recentGames`, `AppCatalog::requestRecentGames`: `favoritesAndRecent` with the machine's zone made on
  the worker), each played date the machine's local date; the card the quick catalog's (`cardContentOf()` of the
  catalog's row, the room key of `roomPlacementOf()`), Avvia on it the quick catalog's launch by GameId (its outcome
  back on the card).
- **Where a command leads** (`routeDirectoryCommand()`, `routeShowInRoom()`): another floor's hall a ride, the
  walker's own floor's hall its elevator (a teleport), a room of the walker's floor its doorway (a teleport), of another
  floor a ride first; "Mostra nella stanza" (both cards) the Focus pose in the interaction's room, a teleport to the
  room's doorway then the ease on the same floor, a ride then the ease to another. Refused while the persisted start's
  reconcile is pending or a result is rebuilt for the floor shown, a ride runs, a launch is active, or in Focus (but
  "Mostra nella stanza") and Inspect: the directory reopens with `ride-unavailable`; no plan: `building-pending`. The
  quick catalog's card now gives a game placed in another room `placed` (its button shows), `other-room` no more.
- **The ride** (`rv/app/elevator_ride.hpp`, the machine; `src/app/src/world_ride.cpp`, the session's side): the
  view's exposure faded out in 250 ms (instant under `reduceMotion`), the destination built by a floor-build job from
  the plan shown and the rooms' items files (`WorldBuild::requestedFloor`; the latest asked wins), the resident floor
  dropped only once it is built and swapped at a frame boundary (the scene's camera the arrival or the room's doorway
  camera, `world::doorwayCamera()`), the hero CRT unbound without a join and bound again by the eye's room tracker, the
  view faded in once the room that holds the eye has every item done (a hall at once) or after 10 s; input suspended
  throughout. The phase `elevator`; `world.floors`, `world.rides[]`, `world.teleports[]`, `world.elevatorPromptFrames`
  and the top-level `directory` record in the run report.
- **Results during a ride** wait for its end; one built for another floor than the one shown (a ride since its
  floor was asked) is built again for the floor shown and applied as a live update (`retargetResult()`). The flows ask
  for the floor shown (`WorldFlow::Options::floor`).
- **Tests.** Unit: the ride's machine (`[elevator_ride]`), the directory's data and routing on the golden building
  (`[directory_data]`), a ride's floor build (`app/world_build: a ride's floor ...`), the recent games on the worker,
  the run report's records, the Back level, the prompt's document. GPU (`gpu.world elevator: ...`, six cases on
  `esde/building`, every room, floor and pose read from the plan and the floors' layout): the ride to gen4 and back
  through the prompt and the panel, a room of another floor chosen with Map and the floor's other room teleported to,
  the quick catalog's game on another floor ending in its Focus pose, the favourites against the fixture's gamelists,
  Avvia on the directory's card into the fake and back, a ride refused in Inspect. The real library's building test
  rides from the lobby to the computer floor and back, cold then warm.

## Decisions

- **The eye's area resident** is every item of the room that holds the eye done in the cover loader (a hall has none,
  so a ride to a floor's elevator fades in once the floor is built); the items seen through doorways come after, as on
  any floor.
- **Lockstep stays frame-exact.** An idle wake only takes the destination and swaps; the fades and the residency check
  move with frames, the check from the frame after the swap whether it came in a frame or a wait: 15 frames out, the
  swap, one frame, 15 frames in at `--fixed-dt 0.016667`, on every run.
- **The fade is the view's exposure** (down to 1e-5, black once tonemapped), the UI above it: no new render pass.
- **After a ride the scene's frame is the destination floor's**, the start room's again on its own floor; `--spawn`
  applies to the start only (`SceneShow::atCamera`).
- **The refusal in Inspect** is reached as a user can: interact on a face-out item and Map in one frame (the frame's
  context is the one it began in), the floor chosen while the item is in the hand.
- **The directory's Launch is the product UI's**; the world sees ChooseFloor, ChooseRoom and ShowInRoom.

## Measurements (Release, reference machine)

- **The building fixture** (`gpu.world elevator: ...`, lockstep): a ride from the request to the view back 0.21 to
  0.42 s (the destination's floor job 16 to 31 ms; 15 frames out, the swap, 15 frames in); the collection arrays
  after each ride a direct run's of that floor (2.4 MiB for the ground floor and gen4, 4.8 MiB for the handheld
  floor).
- **The real library** (`./scripts/real-library.ps1 -Filter '[real-library][building]'`, ES-DE closed, 0 differences
  among the snapshots' 542,120 entries; 44 rooms on 9 floors, 19,985 placements), as the user rides (no lockstep):
  - lobby to the computer floor (10 rooms, 5,314 items): the view back 1.23 s cold, 1.19 s warm (its floor job 0.65
    to 0.68 s; about 0.25 s more for the swap's first frame behind the fade); back to the lobby 1.21 s and 0.76 s:
    within the acceptable row (3 s warm, 10 s cold);
  - the GPU memory at the view back: 401.0 MiB on the computer floor (its arrays 210.9 MiB, a start there's), 147.0
    MiB back in the lobby (2.4 MiB).
- The whole Release suite (1,959 tests, unit and GPU) passes, the overlay's case skipped as in every Release run,
  under the focus watcher with no finding; the Debug unit suite (1,860) and its 53 `gpu.world*`, `gpu.golden` and
  `gpu.directory` cases pass; G01 to G10 0 pixels apart in both.

## Deviations and limits

- A ride asked by the quick catalog's "Mostra nella stanza" that fails is only logged (no panel to reopen).
- Step 7a's contract had the app give `other-room` for a game placed elsewhere; the app gives `placed` instead, so that
  both cards offer the button there (the documents keep the `other-room` sentence, which nothing gives now).
- A ride to a floor's elevator waits for no cover (the hall holds none): on the real library "cold" is the floor's
  build on a data directory that never showed it, not its covers.
- A building's result built again for the floor shown after a ride (`retargetResult()`) has no GPU case: the
  fixture's live updates do not overlap a ride.
- On the building fixture the ground floor and gen4 each hold one collection room of two arrays of one size: the GPU
  case compares the arrays after each ride with a run started on that floor, and the handheld floor's case (four
  arrays) shows a destination's arrays that are not the lobby's.

## Follow-ups

- Step 8: Gate C (the gate's script, the goldens, the real-library rows).
