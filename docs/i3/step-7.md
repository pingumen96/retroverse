# I3 step 7: the interaction model, GPU-free

Step 7 of I3 (docs/roadmap.md, "I3: M2 SNES vertical slice", wave 1) builds the model of ADR-0012's interaction with the
generated room, all of it GPU-free and tested without a window: picking against pick targets that are not the collision
world (`world::PickIndex`), the hover's hysteresis (`world::HoverTracker`), the D-pad steps of Focus
(`world::SlotNavigation`), an eased pose animator (`scene::PoseAnimator`) and the state machine with its input contexts
and Back levels (`app::WorldInteraction`). Nothing in the app uses them yet: steps 13a, 13b, 14a and 15a wire them, on
the collection layout of step 10a. The step fills the stubs step 0a listed for it and adds tests; it touches no other
source, no CMake list and no shared file of the wave.

## What was built

| File | What it holds |
|---|---|
| `rv/world/pick_index.hpp`, `src/pick_index.cpp` | `PickId` (the instance record's pick id, 0 none), `PickTarget`, `faceTargetBox()`, `PickHit`, `HoverSample`, `PickIndex` (`build`, `pick`, `sample`), `HoverTracker`; the constants `kMaxPickDistance` 2.5 m, `kFaceInflation` 1 mm, `kHysteresisMargin` 2 mm, `kHoverChangeFrames` 3 |
| `rv/world/slot_navigation.hpp`, `src/slot_navigation.cpp` | `StepDirection`, `NavSlot`, `NavLevel`, `NavFixture`, `NavRun`, `SlotNavigation` (`build`, `step`) |
| `rv/scene/pose_animator.hpp`, `src/pose_animator.cpp` | `Easing` and `ease()`, `RigidPose`, `interpolatePose()` and `samePose()` for values, positions, walker poses and rigid poses, `EasedProgress`, `PoseAnimator<Pose>` |
| `rv/app/world_interaction.hpp`, `src/world_interaction.cpp` (`rv_app_cli`) | `WorldState`, `WorldContext` with `readsAction` / `actionsFor` / `walkerActionsFor`, `PickRaySource`, `LeftClick`, `WorldBack` and `worldBackAction()`, `InteractionTimings`, `InspectSettings`, `InteractionSettings`, `RoomItem`, the `InteractionRoom` port, `InteractionFrame`, `InteractionResult`, `WorldLaunchRequest`, `PulledItem`, `InspectView`, `InteractionNotice`, `WorldLaunchRecord`, `WorldInteraction` |
| `tests/unit/world/synthetic_shelves.hpp`, `.cpp` | hand-built shelves of step 1b's figures: targets, occluders, navigation runs |
| `tests/unit/world/test_pick_index.cpp`, `test_pick_index_bench.cpp`, `test_slot_navigation.cpp` | picking, the hover, the benchmark (`[.bench]`), the D-pad steps |
| `tests/unit/scene/test_pose_animator.cpp` | the animator |
| `tests/unit/app/world_interaction_harness.hpp`, `test_world_interaction.cpp`, `test_world_interaction_script.cpp` | the app's loop around the interaction without a window, the contexts, Back levels, transitions and failures, scripted sequences |

## Picking (`world::PickIndex`)

- **Targets**: one per placed item, a box in the frame of its slot (any invertible affine transform, local to world;
  the slot's +Z towards the viewer): the item's box with its visible face inflated by 1 mm on each side
  (`faceTargetBox`: grown along X and Y, its depth kept), so a spine's 2.0 cm becomes 2.2 cm in its 2.4 cm pitch. A
  target carries the caller's cluster number (a level of a module, a display case); `build` groups the targets by
  cluster and keeps each cluster's world bounds (its targets grown by the margin), tested before its targets.
- **Occluders**: world boxes that hide what lies behind them (the shelves' boards, sides, base and back, the walls, the
  furniture). The walking colliders do not serve: a module's collider is its whole box, which holds every slot. Glass is
  no occluder.
- **The ray** is tested in each target's frame (an affine map keeps the ray's parameter, so the answer stays meters along
  the world ray), at most 2.5 m from its origin. `pick` answers the nearest target the ray enters unless an occluder
  comes strictly before it: then nothing, since whatever lies behind the occluder is hidden too. Ties: equal distances
  go to the smaller PickId, and an occluder flush with a target's face (a board's front edge) leaves the target in
  front. A zero or non-finite ray, or a range that is not positive and finite, hits nothing.
- **`sample(ray, current)`**, once a frame for the hover: the nearest hit, whether the ray also enters that target's
  core (shrunk by 2 mm on each side, an axis thinner than 4 mm shrinking to its middle plane) in front of every
  occluder, and whether it enters the current hover's target grown by 2 mm.
- `build` refuses (InvalidArgument) an id 0, an id twice, an empty or non-finite box, a frame that is not a finite,
  invertible affine transform, an empty or non-finite occluder.

### The hover (`world::HoverTracker`)

Per frame, from the sample, in this order:

1. the ray hits the hovered item: keep it;
2. the ray is within the hovered item's grown target (between targets, or slanting past its edge into a neighbour's
   side): keep it;
3. the ray is on another item's core: that item is the candidate;
4. the ray is on another item's rim (its target but not its core): keep the hover, forget the candidate;
5. the ray hits nothing: nothing is the candidate.

A candidate becomes the hover after 3 consecutive frames; anything kept in between starts the count again. So the hover
needs 3 frames on a new item's core to change, and 3 frames off every target and off the hovered item's margin to go;
a still ray never changes it, and a ray jittered by 1 mm around a hovered item keeps it (the margins of two neighbours
overlap in the 2 mm between their targets, and a neighbour's core starts 4 mm past the hovered target's edge).

## The D-pad steps (`world::SlotNavigation`)

The room as the walker faces it: runs (a wall's fixtures, left to right), fixtures (levels, bottom to top, heights in a
frame the run shares), levels (slots, left to right, `along` strictly increasing), each slot holding an item or nothing
(empty or dressed). From an item:

- **Left, Right**: the nearest item that way on its level; past the level's end, the next fixture that way holding an
  item, on its level nearest in height among those holding one (the lower on a tie), at its near end (stepping right
  enters at the left end). A fixture without items is crossed whole.
- **Up, Down**: the nearest level that way in the same fixture holding an item, on it the item nearest along (the left
  one on a tie). Up and Down never leave the fixture.
- Nothing that way: no step (the focus stays). `build` refuses an item in two slots, slots not strictly to the right,
  levels not strictly rising, positions or heights that are not finite.

## The pose animator (`scene::PoseAnimator<Pose>`)

A run between two poses on an `EasedProgress` (a time fraction from 0 to 1 at the rate of the run's duration),
interpolated on an easing curve (smoothstep for every animation of the interaction: gentle at both ends, alike both
ways). `animateTo(target, duration)` reverses the run from where it is when `target` is the run's start (the way back
takes the fraction travelled of `duration`: a 250 ms pull-out interrupted at 60% slides back in 60% of its 200 ms), goes
on when `target` is the run's end, settles at once when `target` is where it stands, and otherwise starts a new run
from the current pose. `finish()` skips; `setReduceMotion(true)` finishes and makes every later run instant; a duration
that is not positive and finite arrives at once. Nothing reads a clock: `advance(dt)` moves it by the frame time the
caller passes (the fixed one under `--fixed-dt`), so equal frame times give bit-identical poses. Poses: plain values,
positions, `WalkerPose` (the feet linearly, the yaw along the shorter arc, wrapped, the pitch linearly), `RigidPose`
(slerp along the shorter arc). `samePose` compares within 0.01 mm and 1e-5 radians (a quaternion and its negation are
one orientation).

## The interaction (`app::WorldInteraction`)

| State | Enter | Leave |
|---|---|---|
| FreeRoam | the start; Back from Focus | interact on the hovered item: Focus (spine-out) or Inspect (face-out) |
| Focus | interact on a spine-out item; Back from Inspect; `showInRoom()` | interact or ui_activate: Inspect; Back: FreeRoam, the item put back |
| Inspect | interact or ui_activate in Focus; interact on a face-out item in FreeRoam; the return of a launch | Avvia: Launching; Back: Focus, the item lowered to its pulled-out pose |
| Launching | Avvia (`InteractionResult::launch`) | `launchEnded(outcome)`: Returning |
| Returning | `launchEnded()` | the next update: Inspect of the same item, the walker's pose and the inspection restored (FreeRoam when the item left the room) |

Per frame, `update(InteractionFrame)` reads the frame's actions through its state's context (none while the product UI
has the input: `actions` null), Back first (a Back the world takes ends the frame's reading), then the state's actions,
then the hover from the frame's pick sample (FreeRoam and Focus), the return (Returning), and advances the animations by
the frame time. Its result asks the app for a shell Back level, the quick catalog, a launch, Vedi media or Informazioni,
and gives the walker's pose while an ease or a restore drives it (the app places the walker there and gives it no
actions that frame). Between frames the app reads `state()`, `hovered()`, `focused()`, `highlighted()` (the outline
and the label: the hovered item in FreeRoam, the focused one in Focus), `pulledItems()` (the items off their static
pose, by id, with their eased progress: drawn on the dynamic path), `inspectView()` (the lift, which dims the scene,
yaw, pitch, distance), `notice()` and `launchRecord()`.

- **Hover and pull-out** (FreeRoam): the dwell counts from the frame the hover comes; after 150 ms (9 frames at 60 fps)
  the item slides out (or tilts) in 250 ms, and slides back in 200 ms from where it is when the hover leaves it. Put
  back by Back from Focus while the hover rests on it, it stays in until the hover leaves and comes back.
- **Focus**: the walker eases in 350 ms from where it stands to the room's `focusPose(item)`, the item pulled out; the
  D-pad and the arrows step (each step pushes the old item back, pulls the new one out and eases the walker to its Focus
  pose); looking moves the focus to the item the hover settles on, within the item's section only, and only while the
  player looks (a look in the last 3 frames) and no ease runs: a step is exact even though the ease sweeps the ray over
  other items, and a still stick never changes the focus. Walking is off.
- **Inspect**: the lift takes 300 ms, the lowering 200 ms; the look actions turn the item about its center (180 degrees
  a second at full stick, 5 mrad a mouse pixel; yaw unlimited and wrapped, positive turning the front to the viewer's
  right; pitch within 80 degrees, positive tilting it up), the triggers (and Page Up / Down) zoom at 0.5 m/s and the
  wheel 5 cm a notch between 0.4 and 1.0 m (from 0.6 m at each entry); the walker gets nothing. Once the item is in the
  hand, ui_activate and confirm are Avvia, ui_media Vedi media, ui_info Informazioni; during the lift they wait.
- **Skips**: interact or ui_activate during the ease into Focus (or "Mostra nella stanza") and during the lift ends
  them at once; Back during the lowering ends it (WorldBack::SkipLowering). After a D-pad step interact inspects at once
  (the step started that ease) and the walker arrives at its pose that frame.
- **"Mostra nella stanza"**: `showInRoom(id, walker)` from FreeRoam or Focus, a 600 ms ease (36 frames), skippable with
  interact.
- **Launching**: Avvia records the item, its GameId, the walker's pose and the inspection (`WorldLaunchRecord`) and
  returns `WorldLaunchRequest{item, game}`; nothing is read until `launchEnded(outcome)`; the next update restores the
  walker (`InteractionResult::walker`) and the inspection, in hand.
- **Failure paths**: an item the room no longer holds (`roomChanged()` after a new plan, or found missing on interact,
  Avvia or the return) falls back to FreeRoam with `InteractionNotice::ItemUnavailable`, the walker where it stands (or
  restored, at a return); a launch outcome other than `returned` (refused, not ready, failed, crashed) returns to
  Inspect with `LaunchOutcome` and A1's message; `showInRoom` on an item the room lacks answers false with the notice; a
  hover on an item the room lacks is dropped. The next transition clears the notice.
- **reduceMotion** (`InteractionSettings::reduceMotion`, `setReduceMotion()`): every animation instant, those running
  finished (the walker placed once); the hover's dwell still applies.

### Input contexts and bindings

`readsAction(WorldContext, Action)` inside A1's World context (the Ui context still takes everything while the quick
catalog or the game-session panel is open). The defaults (`assets/config/default_bindings.json`):

| Action | Keyboard / mouse | Pad | FreeRoam | Focus | Inspect | Launching, Returning |
|---|---|---|---|---|---|---|
| move, sprint | WASD, arrows, Left Shift | left stick, its click | walk | - | - | - |
| look | mouse (captured) | right stick | look; moves the reticle's pick | moves the pick within the section | turns the item | - |
| interact | E, left click (captured) | south | Focus / Inspect | Inspect (skips the ease into Focus) | skips the lift | - |
| ui_activate | Enter | south | - | Inspect (skips the ease into Focus) | Avvia (skips the lift) | - |
| confirm | Enter | Start | read, unused | - | Avvia | - |
| ui_up / down / left / right | arrows | D-pad | - | steps | the card's buttons (14a) | - |
| ui_page_up / ui_page_down | Page Up / Down | triggers | - | - | zoom out / in | - |
| wheel (`InteractionFrame::wheelNotches`) | wheel | - | - | - | zoom | - |
| ui_media | - | north | - | - | Vedi media | - |
| ui_info | - | west | - | - | Informazioni | - |
| back | Esc | east | Back levels | Back levels | - | - |
| ui_back | Esc | east | - | - | Back levels (Indietro) | - |
| quick_catalog | Tab | north | opens the catalog | opens the catalog | - | - |
| toggle_dev_ui, reload_shaders, screenshot | F1, F5, F12 | - | read | read | read | read |

The pad's north button is thus the quick catalog in FreeRoam and Focus and Vedi media in Inspect only; the triggers zoom
in Inspect only. `walkerActionsFor()` gives the walker FreeRoam's actions, the look actions alone in Focus, nothing in
Inspect or while suspended. The pick ray (`pickRaySource`) comes from the reticle while the mouse is captured or the pad
was used last, from the cursor while the mouse is free; a left click while the mouse is free only captures it
(`leftClickMeaning`; `platform::MouseCapture` already consumes that click, so it never reaches the action map), and once
captured it is interact.

### Back levels

`worldBackAction(BackState, WorldBack)`: the shell's Ignore (an overlay field), ClosePopup, PanelBack and CatalogBack
first; then the world's level (`backLevel()`): LeaveInspect in Inspect, SkipLowering in Focus while the item is still
being lowered, LeaveFocus in Focus; then the shell's ReleaseMouse, CloseOverlay or Quit. In FreeRoam, Launching and
Returning the world has no level. Inside `update` Back is the `back` action in FreeRoam and Focus and `ui_back` in
Inspect; a shell level comes back as `InteractionResult::shellBack` for the app to perform.

## Tests

- `test_pick_index.cpp` (`[world][pick_index]`): the face inflation; malformed targets and occluders; the index by id
  and cluster; **at 1.5 m the ray picks the spine it crosses at a 2.4 cm pitch** (25 points over each of a module's 231
  spine faces, with the distance; a scan across a level in 1 mm steps: over a face its spine, in a gap a neighbour or
  nothing); **an item behind a board is never picked** (41,580 rays from 60 eyes above, level with and below the levels,
  each ray blocked by a board, side or base before the face it aims at never picking that item; straight up and down
  through boards); the 2.5 m range, ranges and rays that cannot hit; ties to the smaller id and a flush occluder; a
  tilted target tested in its own frame (a ray through its world box's empty corner picks nothing) and a module run on a
  west wall; the sample's core and margin flags; the hover's 3-frame changes, rims, gaps and leaving; **a ray jittered
  by 1 mm never changes the hover** (seeded jitter around points across faces and on the inflated edges, from 0.9 and
  1.5 m, on three levels).
- `test_slot_navigation.cpp` (`[world][slot_navigation]`): steps along a level skipping empty slots; across modules
  (the level nearest in height, dressed levels skipped, the face-out band entered at its near end); a module without
  items crossed whole and a walk along a wall's level visiting every item; up and down over dressed levels, into and out
  of the face-out band, never out of the module; ties; unknown items and malformed runs.
- `test_pose_animator.cpp` (`[scene][pose_animator]`): the curves; interpolation's exact ends, the yaw's short way and
  slerp; a run's frames at 60 fps; **identical frame times give bit-identical animations** (and another frame time
  another sequence); **interrupted, it reverses from where it is** (no jump, the travelled fraction of the way back);
  skip, settle, zero and non-finite durations; reduceMotion.
- `test_world_interaction.cpp` (`[app][world_interaction]`): **each state's context table** for every action; the
  north button (Vedi media in Inspect only, the catalog in FreeRoam and Focus, Tab nothing in Inspect); the triggers and
  the wheel (zoom in Inspect only, within 0.4 to 1.0 m); keyboard, mouse and pad equivalents (Enter, E, south, Start,
  west, Esc, arrows); the walker's actions; the ray's source and the click rule; **the Back levels** for every world
  level against every shell state, and through the interaction (Inspect, the lowering skipped, Focus, the shell's mouse
  and quit; an overlay popup and field first); **the transitions**: the dwell and the pull-out to the frame, the
  push-back, an interrupted pull-out reversed, the ease into Focus (21 frames), walking off, the skips, a D-pad step
  followed by interact, the lift (18 frames), rotation, pitch limit, zoom, a face-out item inspected from FreeRoam,
  exact D-pad steps across modules while the ease sweeps the ray, looking within a section and not across, Avvia and
  the return with the walker and the inspection restored, "Mostra nella stanza" (36 frames, skippable); **the failure
  paths**: a launch refused (the notice, the state usable), items removed while focused, inspected, hovered and during a
  launch, no room; the product UI holding the input; reduceMotion.
- `test_world_interaction_script.cpp` (`[app][world_interaction][script]`): input scripts in `--input-script`'s format
  (`InputScript`, `InputScriptPlayer`) drive the harness for 270 frames at 1/60 s: hover, Focus, two steps right, one
  up, Inspect, a turn, a zoom, Indietro, Back, a walk, each state and item on its frame; two runs identical frame by
  frame; a still stick held 60 frames changes neither the hover nor the focus.
- The harness (`world_interaction_harness.hpp`) is the app's loop of steps 13a and 13b without a window: the reticle's
  ray from `scene::Camera`, `PickIndex::sample` with the current hover, `WorldInteraction::update`, the walker
  (`scene::FirstPersonController`) placed at the result's pose or given `walkerActionsFor`.

Results on the reference machine, with other worktrees building at the same time: `msvc-x64-debug` and
`msvc-x64-release` built with 0 warnings; `./scripts/test.ps1` passed 1,441 of 1,441 unit tests in Debug (860 s) and in
Release (777 s), the step's 52 test cases among them (`rv_tests "[pick_index],[slot_navigation],[pose_animator],
[world_interaction]"`, which also runs the hidden benchmark). `RV_LAUNCH_ALLOWLIST` not set.

## Benchmark

`tests/unit/world/test_pick_index_bench.cpp` (`[.bench]`, Release: `build/msvc-x64-release/bin/rv_tests.exe
"[pick_index][bench]"`): hand-built rooms of the 9 x 10 m template's 16 modules and of the 16 m room's 28 (two on the
north wall, the rest on the side walls, turned 90 degrees), two sets of 20,000 seeded rays each sampled as the hover
samples them (`sample` with a current hover, so up to three passes over the occluders): free rays (anywhere in the
room, any direction; most reach nothing within 2.5 m) and aimed rays (from 0.6 to 2 m in front of a random item, at its
face). The roadmap's budgets name none for picking; the case checks the step's own, a median of 20 us a sample. On the
reference machine, Release, with other worktrees building (informative):

| Room | Targets, clusters, occluders | Free rays: median, p99 (us) | Aimed rays: median, p99 (us) |
|---|---|---|---|
| 9 x 10 m, 16 modules | 3,696, 112, 192 | 1.23, 1.73 | 5.23, 37.42 |
| 16 m, 28 modules | 6,468, 196, 336 | 1.86, 2.70 | 8.42, 9.76 |

A second run gave medians within 10% of these (1.18, 5.74, 2.00 and 8.99 us); its p99s of 19.5 and 53.7 us on the aimed
rays, like the first run's 37.4 us, come from batches the concurrent builds interrupted. The aimed rays' cost is mostly the linear passes over the occluders (one for the hit, one for the core, one for the
current hover); a grid of the occluders would cut it if a later step samples more than one ray a frame. One sample a
frame is about 0.05% of a 16.7 ms frame. (Debug: 15 to 44 us.)

## Decisions taken within the step

1. **Oriented targets.** A target is a box in its slot's frame rather than a world AABB: the display cases' slots lean
   30 degrees and the wall shelf's 12, and an AABB round a leaning cartridge would take rays that pass beside it; the
   hysteresis's 2 mm only means something on the item's own faces. Spines in axis-aligned modules cost the same.
2. **Occluders are the caller's solid pieces**, not the walking colliders, which hold every slot; an occluder in front
   of the nearest target hides everything (`pick` answers nothing rather than the next target behind it).
3. **The hover's rules, in full.** ADR-0012 states the switch (3 frames on another item's shrunk target); the step adds
   the symmetric keep (the hovered target grown by the same 2 mm keeps the hover, before any neighbour's core, so a ray
   slanting past an edge into a neighbour's side does not take it) and the leave (3 frames on nothing). A rim keeps the
   hover without counting. Acquiring from nothing takes 3 frames too (50 ms at 60 fps, inside style §16's 100 ms).
4. **Look-driven focus only while looking.** In Focus the focus follows the hover only within the item's section, only
   with a look action in the last 3 frames and never during the walker's ease: otherwise a D-pad step's ease, sweeping
   the ray over the items between, would move the focus, and a reticle left on the previous item would pull it back.
5. **Skips belong to the button that started the transition.** Interact skips the ease into Focus and the lift; after a
   D-pad step it inspects at once (finishing the walker's ease, which never runs in Inspect). Back skips the lowering
   (one more Back level) but not the push-back after leaving Focus, which would swallow a shell level.
6. **The walker stays in Inspect and after it.** Back from Inspect returns to Focus without moving the walker, also for
   a face-out item inspected straight from FreeRoam; Back from Focus stops a running ease where it is instead of
   walking back.
7. **One easing for every animation** (smoothstep): symmetric, so a reversed run ends as gently as a forward one; an
   ease-out pull-out would slam back into its slot when reversed.
8. **The dwell survives reduceMotion**: the pull-out is instant, but only after 150 ms on one item, so scanning a shelf
   does not flash every spine.
9. **Inspect reads neither QuickCatalog nor Back**: the north button is Vedi media there (ADR-0012 §1), and Tab, which
   only QuickCatalog uses, opens nothing either; Esc and B arrive as ui_back, the card's Indietro, and go through the
   same Back levels. Confirm (Enter, Start) is Avvia in Inspect and unread in Focus, where Enter is ui_activate.
10. **The wheel is a frame field**, not an action: no action is bound to the wheel, and binding one would change the
    shared bindings and the quick catalog; the app passes `RawInputFrame::mouseWheel` (13b).
11. **Returning lasts one update**: `launchEnded()` is called when A1's flow ends; the next update restores, so the
    frame that resumes after the emulator reads nothing (A1 releases what the wait dropped).
12. **Names**: `WorldLaunchRequest` and `WorldLaunchRecord`, since `app::LaunchRecord` (the run report) and
    `launcher::LaunchRequest` exist.
13. **No D-pad auto-repeat** in the model: a step per press, exact; a held direction repeating is 13a's choice (the
    product UI's `NavInput` has the repeat rules).

## Notes for later steps

- **8** (static instance groups, the outline): the outline goes on `highlighted()`; the items of `pulledItems()` (and
  the inspected one) are drawn on the dynamic path with their static instances hidden (`instance_flags::kHidden`); a
  pick id in the instance record is the `world::PickId` of the item's target (0 for everything else).
- **10a** (`collectionLayout`): give each placement a `PickTarget` (its slot's frame, `faceTargetBox` of the item's box
  in that frame, its cluster: a level of a module, a display case), the occluders (boards, sides, base, back panels,
  walls, furniture boxes; not the walking colliders, not glass) and the navigation runs (per wall, the fixtures left to
  right as one faces it, their levels bottom to top with world heights, the slots left to right with positions along a
  frame the fixture shares; dressed and empty slots as `kNoPick`). PickIds must be stable for a plan and nonzero; the
  synthetic shelves of `tests/unit/world/synthetic_shelves.hpp` show the shapes.
- **10b**: implement `InteractionRoom` over the room (the plan's placements: GameId, face-out from the slot's section,
  a section per module or display case; `focusPose` and `step` below), build `PickIndex` and `SlotNavigation` with the
  plan, call `setRoom()` / `roomChanged()` when a plan is committed (at a frame boundary), `setRoom(nullptr)` without a
  room.
- **13a**: each frame, before `update`: the ray from `pickRaySource` (the reticle `Camera::rayFromNdc({0, 0})`, the
  cursor converted to NDC while the overlay frees the mouse), `index.sample(ray, interaction.hovered())`, the actions
  (null in the Ui context), the walker's pose, `BackState`; after it: `shellBack` replaces the World context's own
  `back()` call (the app must not also act on the Back action there), `openCatalog` replaces
  `catalogToggleRequested` in World, a result's `walker` teleports `FreeRoam`'s controller (no update that frame), else
  the controller gets `walkerActionsFor`. `focusPose` is `scene::frontViewpoint` of the item's section at 0.9 m in the
  card's `ViewArea`; it may depend on the item (each step re-aims) or only on its section. The pull-out's poses come
  from `pullOf`: spine-out 70% of the item's depth along the slot's +Z and a quarter turn showing the front (stood
  spine-out the front faces the slot's +X, step 1b), face-out a tilt; `reduceMotion` from `WorldConfig`.
- **13b**: `inspectView()` drives the held item (in front of the camera at `distance`, turned by yaw about the camera's
  up and pitch about its right, from the pulled-out pose at lift 0) and the dimming (`lift`); pass the wheel in
  `wheelNotches`.
- **14a**: the card reads Avvia, Vedi media, Informazioni from `update`'s result and mapping today (ui_activate and
  confirm, ui_media, ui_info); once its buttons take the focus (the arrows, Enter pressing the focused one), add an entry
  for a button press (Avvia, Vedi media, Informazioni, Indietro) and stop mapping ui_activate in `readInspect`; the
  notice (`notice()`, `noticeMessage()`) is the card's failure sentence, by code.
- **15a**: `InteractionResult::launch` goes to `AppCatalog::requestLaunch` by GameId; `launchEnded(LaunchMessage)` when
  `LaunchFlow` ends (refused or after the return); `launchRecord()` holds the pose to restore and the item; the
  decoder and the loader are the app's. "Mostra nella stanza": `showInRoom(pickIdOf(gameId), walker)` after closing the
  catalog.

## For the merge

- The step touches only the stubs step 0a listed for it (`pick_index`, `slot_navigation` in `rv_world`,
  `pose_animator` in `rv_scene`, `world_interaction` in `rv_app_cli`) and new test files; no source list, CMake file,
  data file or shared header changed, so it merges in any order within wave 1.
- **CLAUDE.md** (not edited by the step): "Layout and layering": `rv_world` holds picking (`pick_index.hpp`: targets in
  slot frames, occluders, 2.5 m, the 3-frame hover) and the D-pad steps (`slot_navigation.hpp`) since I3 step 7;
  `rv_scene` the eased pose animator; `rv_app_cli` `WorldInteraction` (the world's states, input contexts and Back
  levels; unused by the app until step 13a).
- **docs/architecture.md**: the `world` row (PickIndex, HoverTracker, SlotNavigation), the `scene` row (PoseAnimator),
  `rv_app_cli` (WorldInteraction, its contexts inside A1's World context and its Back level between the shell's).
- **docs/roadmap.md**: step 7 done.
