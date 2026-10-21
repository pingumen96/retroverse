# I4 step 7b: the elevator, the directory and the ride

Roadmap row: I4 step 7 (its second half: [step-7a.md](step-7a.md) made the panel). Decisions:
[ADR-0013](../decisions/ADR-0013-the-building.md) §9, §10, §13, [ADR-0012](../decisions/ADR-0012-interaction-in-the-world.md)
§8; step 7a's contract (amended here: a game placed elsewhere is `placed`).

## What was built

- **Opening the directory.** In a floor's elevator zone (`Hall::elevatorZone`, in the scene's frame:
  `WorldBuild::elevatorZone`), facing its doors (`WorldBuild::elevatorDoors`, `facesElevator()`: their middle less than
  80 degrees off the view), the prompt shows (`assets/ui/elevator_prompt.rml`: "Ascensore", the interact key of the
  device used last, "Scegli il piano") and interact opens the directory on its floors; Map (M, the pad's Back/Select)
  opens it anywhere in free roam and closes it. Free roam is the frame's: it began there and its interaction left it
  there, no ease driving the walker (interact on an item and Map in one frame open nothing). The directory is the
  product UI's: the UI's input context, the Back level `DirectoryBack` after the quick catalog's.
- **The directory's data** (`rv/app/directory_data.hpp`, GPU-free): `directoryBuildingOf()` (the plan's floors by
  `Floor::order`, each floor's rooms along its hall by the middle of their footprint, the north row first on a tie, the
  sections' full names from the catalog, the placed games), refreshed after every commit with the walker's floor and the
  room that holds the eye; the favourites and recent games on the catalog worker when the model asks
  (`CatalogWorker::recentGames`: `favoritesAndRecent` with the machine's zone), each played date the machine's local
  date; the card the quick catalog's (`cardContentOf()`, the room key of `roomPlacementOf()`), Avvia on it the quick
  catalog's launch by GameId, its outcome back on the card.
- **Where a command leads** (`routeDirectoryCommand()`, `routeShowInRoom()`): another floor's hall a ride, the
  walker's own floor's hall its elevator (a teleport), a room of the walker's floor its doorway (a teleport), of another
  floor a ride first; "Mostra nella stanza" (both cards) the Focus pose in the interaction's room, a teleport to the
  room's doorway then the ease on the same floor, a ride then the ease to another. Refused while a floor applies (the
  persisted start's reconcile, a result built again for the floor shown) but for the Focus pose where the walker is,
  while a ride runs, a launch is active, or in Inspect (and Focus but for "Mostra nella stanza"): the directory reopens
  with `ride-unavailable`, the quick catalog's refusal or failed ride is the world's notice `room-unreachable`; no
  plan: `building-pending`. The quick catalog's card gives a game placed in another room `placed` (its button shows).
- **The ride** (`rv/app/elevator_ride.hpp`, the machine; `src/app/src/world_ride.cpp`, the session's side): the fade
  out in 250 ms (instant under `reduceMotion`), the destination built by a floor-build job from the plan shown, the
  rooms' items files and the catalog's sessions now (`catalogLastPlayed()`: a game played since the items were written
  starts its room's CRT, as the preview does), the resident floor dropped only once it is built and swapped at a frame
  boundary in the frame after the fade ended (the scene's camera the arrival or the room's doorway camera), the hero
  CRT unbound without a join and bound again by the eye's room tracker, the view faded in once the room that holds the
  eye has every item done (a hall at once) or by 10 s after the request (`viewDeadlineMs()`: the wait ends at 9.25 s;
  a fade-in begun late by a long frame is shortened to end by 10 s); the fades by the real clock (by the frames' times
  under lockstep); input suspended throughout. The fade is the post pass's (`PostSettings::fade`: the tonemapped
  view's sRGB values times 1 - fade before dithering, the UI above it). The phase `elevator`; `world.floors`,
  `world.rides[]` (with `gpuKinds`, every kind's bytes at the view back), `world.teleports[]`,
  `world.elevatorPromptFrames` and the top-level `directory` record in the run report.
- **Results during a ride** wait for its end; one built for another floor than the one shown (a ride since its
  floor was asked) is built again for the floor shown and applied as a live update (`retargetResult()`), once no
  launched game runs (ADR-0012 §6). The flows ask for the floor shown (`WorldFlow::Options::floor`).
- **Tests.** Unit: the ride's machine (`[elevator_ride]`: the swap in the frame after the fade, the deadline), the
  directory's data and routing on the golden building (`[directory_data]`), the prompt's facing (`[elevator_prompt]`),
  a ride's floor build with the elevator's doors (`app/world_build: a ride's floor ...`), the recent games on the
  worker, the run report's records, the post block's fade, the Back level, the documents, `--help`. GPU (`gpu.world
  elevator: ...`, nine cases on `esde/building`, every room, floor and pose read from the plan and the floors' layout):
  the ride to gen4 and back through the prompt (turned to the doors) and the panel, each floor's GPU memory after a
  ride a direct run's; a room of another floor with Map, the floor's other room and its hall (teleports); the quick
  catalog's game on another floor ending in its Focus pose; the favourites against the fixture's gamelists; Avvia on
  the directory's card, returned and refused (its outcome on the card); the directory never opened over an item taken
  in the same frame; the fade's view each value times the fade's share; a game launched in the lobby starting its
  room's CRT after a ride. The real library's building test: the computer floor's largest room started cold (covers
  derived) then warm, then, the derived covers emptied, the ride from the lobby to that room and back, cold then warm.

## Decisions

- **The eye's area resident** is every item of the room that holds the eye done in the cover loader (a hall has none,
  so a ride to a floor's elevator fades in once the floor is built); the items seen through doorways come after, as on
  any floor.
- **Lockstep is frame-exact.** The machine moves only in frames: the swap is always in the frame after the fade ended
  (a job that answers later is waited for between the two), the residency checked from the next frame; an idle wake
  only hands the job's answer over. At `--fixed-dt 0.016667`: 15 frames out, the swap, one frame, 15 frames in, on every
  run.
- **The fade is after the tonemap**, the sRGB values scaled (a fade of the exposure kept the lights bright until its
  last frame): no new pass, the post block's padding is `fade`.
- **The view back by 10 s of the request**, not 10 s after the swap: the acceptable row's cold limit is the timeout's.
  The wait ends 0.75 s before it (the fade-in and half a second: the swap's frame and the first uploads into a floor's
  new arrays take up to 0.48 s on the real library), and outside lockstep the fades count the real clock, not the
  frames' times (clamped to 0.1 s in a long frame: a cold ride first measured 10.39 s so).
- **The prompt faces the doors.** The arrival point faces the hall with the elevator behind: a prompt there sat over
  the lobby's entrance doors.
- **After a ride the scene's frame is the destination floor's**, the start room's again on its own floor; `--spawn`
  applies to the start only (`SceneShow::atCamera`).
- **The directory's Launch is the product UI's**; the world sees ChooseFloor, ChooseRoom and ShowInRoom.

## Measurements (Release, reference machine)

- **The building fixture** (`gpu.world elevator: ...`, lockstep): a ride from the request to the view back 0.22 to
  0.39 s (the destination's floor job 17 to 33 ms); the GPU memory a floor holds after each ride a direct run's of that
  floor, kind by kind; the fade's frame 7 of 15 at 0.534 of the view before it (the share 0.533), its brightest value
  255 to 137.
- **The real library** (`./scripts/real-library.ps1 -Filter '[real-library][building]'`, ES-DE closed, 0 differences
  among the snapshots' 542,120 entries; 44 rooms on 9 floors), as the user rides (no lockstep):
  - the computer floor started cold (10 rooms, 5,314 items): 10,228 cache misses, 10,227 stored, its floor build
    11 s; warm: 10,230 hits, 1 miss; 407.3 MiB at its peak;
  - the lobby to the computer floor's largest room (1,646 items) through the elevator: cold (the room's covers made
    from the media) `timed-out`, the view back at 9.51 s (its floor job 0.78 s); warm 1.87 s (the room resident at
    1.61 s, its floor job 0.72 s); back to the lobby's elevator 0.77 s and 0.72 s: within the acceptable row (3 s
    warm, 10 s cold);
  - the GPU memory at the view back: 407.3 MiB in the room (its arrays 210.9 MiB, a start there's), 151.7 MiB back in
    the lobby (2.4 MiB).
- With the fades counting the frames' times and a 0.1 s margin, the same cold ride measured 10.39 s (the elevator
  phase's frames: p99 248 ms, max 479 ms).
- The whole Release suite (1,964 tests, unit and GPU) passes, the overlay's case skipped as in every Release run, under
  the focus watcher with no finding; the Debug unit suite (1,862) and its 56 `gpu.world*`, `gpu.golden` and
  `gpu.directory` cases pass; G01 to G10 0 pixels apart (largest difference 0/255) in both.

## Deviations and limits

- The refusals a user can meet (a floor applying in a start that does not wait, a launch being decided), and so the
  world's `room-unreachable` notice of the quick catalog's, have no GPU case: a lockstep run reconciles at its second
  frame and waits for the catalog's decision. The routing's unit tests cover them; the case in Inspect is gone with
  the race that reached it.
- A building's result built again for the floor shown after a ride (`retargetResult()`), and its wait for a launch's
  end, have no GPU case: under lockstep the frames wait for both jobs, so no live update overlaps a ride.
- A ride to a large room cold can time out (the view back by 10 s with covers still coming, `timed-out`); when the
  room is resident after it (ADR-0013 §13's own row, 30 s cold) is not measured here.

## Follow-ups

- Step 8: Gate C (the gate's script, the goldens, the real-library rows).
