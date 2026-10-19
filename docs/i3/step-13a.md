# I3 step 13a: hover, pull-out and Focus

Roadmap row: I3 step 13a (wave 3, on 7, 8 and 10b; 10c and 12 run beside it in their own worktrees). Decisions:
[ADR-0012](../decisions/ADR-0012-interaction-in-the-world.md) §1 (the states, their input contexts and Back levels), §2
(the pick ray, the click rule), §3 (the hover with its outline and title label, the pull-out after 150 ms, Focus). The
step wires step 7's GPU-free interaction (`app::WorldInteraction`, `world::PickIndex`, `world::SlotNavigation`,
`scene::PoseAnimator`) into the generated room of step 10b, on step 8's outline and hidden flag: the item the reticle
rests on is outlined and labelled with its title; after 150 ms it leaves its slot on the dynamic path (a face-out
cartridge tips towards the viewer, a spine-out one slides out of its row and turns its cover to the viewer), its static
records hidden meanwhile; interact focuses a spine-out item (the walker eased in 350 ms to 0.9 m in front of it) or
inspects a face-out one, and in Focus the D-pad and the arrow keys step from cartridge to cartridge (repeating while
held); Back leaves one level at a time; the world's `reduceMotion` (world.json) makes every transition instant. It fills
step 0a's stub `world_hover` (`rv_app`) and adds new files; the hooks it adds to shared files are listed under "For the
merge".

## What was built

| File | What it holds |
|---|---|
| `rv/app/world_focus.hpp`, `src/world_focus.cpp` (`rv_app_cli`, new, GPU-free) | `pulledItemTransform()`, `rowReach()`, `itemBounds()`, `ItemFace` and `itemFace()`, `FocusViewer`, `focusPoseFor()`, `CollectionRoom` (the layout as `InteractionRoom`), `FocusStepRepeat`, `pickRay()`, `labelAnchor()`; the constants `kPullOutDepthShare` 0.7, `kFaceOutTiltDeg` 10, `kPullClearance` 2 mm, `kMaxRowReach` 3 cm, `kPullSlideShare` 0.4, `kFocusDistance` 0.9 m |
| `rv/app/world_label_ui.hpp` (`rv_app_cli`, new, header only) | the label document's paths, ids and data model; `WorldLabelContent` |
| `src/world_hover.hpp`, `src/world_hover.cpp` (`rv_app`, step 0a's stub) | `WorldHover`: the interaction driven every frame over the room's collection (the pick sample, the D-pad's repeat, the collection's highlight and hidden flags, the pulled items' render items, the label, the "animation" phase, the run report's record) |
| `src/world_label_panel.hpp`, `src/world_label_panel.cpp` (`rv_app`, new) | `WorldLabelPanel`: the label's RmlUi document, shown without focus or input |
| `assets/ui/world_label.rml`, `.rcss` (new) | the label: the catalog's title in a small band, "Gioco senza titolo" and "(file non trovato)" around it |
| `src/world_session.hpp`, `.cpp` (step 10b's) | the session owns the `WorldHover` (reduceMotion read from world.json), gives it the committed layout and collection, forwards the frame (`interact`, `viewItems`, `label`, `context`), records `world.interaction` |
| `src/collection_instance.hpp`, `.cpp` (step 10b's) | `setHidden()`, `hiddenItems()`, `staticDrawn()`, `appendRenderItems()` (an item's parts as render items at another pose), `writeFlag()` |
| `src/retroverse_app.cpp` | in room mode, the world's interaction before the walker moves: the pick ray, Back's and the quick catalog's results, the walker placed by an ease or given its context's actions, the pulled items in the view, the label |
| `src/product_ui.hpp`, `.cpp` | `setWorldLabel()` and the label's panel (shown like the notice, never over the catalog or the panel) |
| `rv/app/run_report.hpp`, `src/run_report.cpp` | `InteractionRecord`, `InteractionEventRecord`, `kMaxInteractionEvents`; `world.interaction` written and read |
| `src/app/CMakeLists.txt` | `world_focus.cpp` (`rv_app_cli`), `world_label_panel.cpp` (`rv_app`) |
| tests | `tests/unit/app/test_world_focus.cpp`, `test_world_label_document.cpp`; `tests/platform/test_click_interact.cpp`; `tests/smoke/test_world_focus.cpp`; `tests/input_scripts/world_hover_pull.json`, `world_focus_steps.json`, `world_focus_enter.json` |

## One frame

```text
onUpdate (room mode)
  input -> actions; the frame's InputContext (World, or Ui while the quick catalog or the game-session panel is open)
  WorldHover::update (before the walker moves; the camera as the last frame left it):
    the pick ray: the reticle while the mouse is captured or the pad was used last, else the cursor (SDL's position;
      frame-limited runs, which never read the user's mouse, rest it at the window's centre); none under the product UI
    PickIndex::sample(ray, hovered)                     (the room's targets and occluders, step 10a)
    in Focus: FocusStepRepeat over the arrows           (a held direction repeats)
    WorldInteraction::update                            (step 7: Back, the state's actions, the hover, the animations)
    Avvia (Inspect): ended at once as returned          (launching from the room is step 15a's)
    the collection: kHidden on the pulled items' records, kHighlighted on the highlighted item's (16-byte writes)
    the record: events, counts, the "animation" phase opened or closed
  result.shellBack -> the shell's Back level; result.openCatalog -> the quick catalog
  the walker: placed at result.walker (an ease, a skip), else FirstPersonController with walkerActionsFor(context)
  the label: labelAnchor() of the highlighted item as this frame's camera shows it -> ProductUi::setWorldLabel
  ProductUi::update; WorldSession::update (a commit replaces the room: WorldHover::setRoom)
onRender
  the view's items: the scene's, then each pulled item's parts at pulledItemTransform() (its layers and highlight)
  the static group draws every other record; the hidden ones draw nothing (step 8's vertex stage); the outline pass
  marks the highlighted records wherever they are drawn, static or dynamic
```

## The room's side (`world_focus.hpp`)

- **The pull-out** (`pulledItemTransform(item, progress, reach)`), a function of the eased progress alone, so an
  interrupted pull goes back the way it came (step 7's animator reverses it from where it is):
  - **spine-out**: the item slides straight out along its slot's +Z until its back is `kPullClearance` in front of its
    row (the first 40% of the progress), then turns a quarter about the slot's vertical through its centre, its front
    (its cover, which faces the slot's +X while it stands on its end) coming round to the viewer, while it settles to
    70% of its depth out of its slot; during the turn its centre never comes closer to the row than its turned
    footprint needs (|sin| half its width + |cos| half its depth, plus the clearance), so it never passes through its
    neighbours. `rowReach()` (computed once per item by `CollectionRoom`) is how far the row stands out past the item's
    face within the span its turn sweeps and its height: its neighbours' fronts (their insets differ by up to 4 mm) and,
    at the end of a row, the module's side panel (up to 1.4 cm proud of the slots); an item at the end of a row settles
    that much farther out (at most 80.6% of its depth: 30 of the 834 spine-out items of the 890-title golden plan, 68
    of 1,944 of the 2,000-title one), every other at 70%.
  - **face-out**: it tips its top 10 degrees towards the viewer about its slot box's bottom front edge (the band's
    upright items, the display cases' and the wall shelf's leaning ones alike).
- **Focus** (`focusPoseFor()`): the feet on the walker's floor, the eye 0.9 m in front of the item's visible face along
  its slot's outward direction projected on the floor, looking at the middle of the face (`scene::aimAt` in the
  viewer's area: the whole view in 13a; 14a's card gives the area it leaves free). The reticle then rests on the item's
  pick target, so the hover stays on the focused item while the stick is still. Every item's pose re-aims (a D-pad
  step eases the walker to the next item's pose).
- **The room** (`CollectionRoom`): the layout's items by pick id; face-out from the stance; the section the pick id's
  socket (`id >> 24`: a module, a display case, the wall shelf), so looking in Focus moves the focus within that
  fixture only; the D-pad steps from `world::SlotNavigation` over the layout's navigation runs.
- **The D-pad's repeat** (`FocusStepRepeat`, step 7 left it to this step): in Focus a held arrow or D-pad direction
  steps at once, again after 0.4 s, then every 0.1 s (0.05 s once held 1.5 s), at most one step a frame, no backlog: the
  quick catalog's cadence (`ui::DirectionRepeater` with `ui::NavSettings`' defaults); the direction pressed last among
  those held wins; a tap within one frame steps once. Entering Focus with an arrow held from walking does not step until
  it is pressed again.
- **The pick ray** (`pickRay()`): the view's centre, or through the cursor with the camera's aspect set to the window's.
- **The label's anchor** (`labelAnchor()`): the middle of the top edge of the item's box (at its pull) as the view shows
  it, at least 64 px from the view's top; none behind the eye or off screen.

## The app's side (`WorldHover`, the label, the hooks)

- **The frame order**: the interaction runs before the walker moves, with the camera the last frame left (step 7's
  harness order), then the walker is placed or driven; the label is projected with this frame's camera; the room's
  commit comes after the product UI's update as before (a commit drops the interaction's room: `WorldHover::clear()`).
- **The World context**: Back and QuickCatalog go through the interaction (`InteractionResult::shellBack` replaces the
  World context's own Back, `openCatalog` replaces `catalogToggleRequested` there); without a room's collection (the
  shell, the test room, a room still being made) the interaction has no room and passes every Back level and the quick
  catalog to the shell exactly as before. Runs with `--scene`, `--model` or `--mock-catalog` have no world and keep the
  previous code path (golden G01's runs).
- **The dynamic path**: each pulled item's three parts (shell, front, spine) become render items at its pulled pose with
  its records' attributes (its layers as uploaded, its highlight) without the hidden flag, appended to the scene's items
  only while something is pulled; its static records carry `kHidden` meanwhile, so no pass draws them (shadow map,
  depth pre-pass, lit pass, the outline's mark). The outline follows the dynamic copy (its records carry
  `kHighlighted`). Back in its slot (progress 0, the interaction drops it), its records lose `kHidden`.
- **The label** (`assets/ui/world_label.rml`): the highlighted item's title (the catalog's, data) in a dark band centred
  12 px above the item's projected top, moved by whole pixels; "Gioco senza titolo" for a game the snapshot lacks,
  "(file non trovato)" after the title of an unavailable one. Like the world's notice it is an RmlUi document of the
  product UI that takes neither focus nor input, hidden while the quick catalog or the game-session panel shows; it is
  drawn into screenshots with the product UI.
- **Inspect in 13a**: entered as the model says (interact on a face-out item, interact or ui_activate in Focus), its
  input context and Back levels live (Indietro returns to Focus); 13b draws the lift, the rotation and the dimming, so
  until then the item stays at its pulled pose and the outline and label go (the model highlights nothing in
  Inspect). Avvia ends at once as if the game had returned (logged), so the state stays usable: launching from the room
  is 15a's.
- **reduceMotion**: `WorldSession` reads world.json (`world::loadWorldConfig`) at its start; every animation is
  instant (the walker placed in the frame of the step, the pull-out whole on the frame it starts, the lift and the
  lowering at once), the hover's dwell kept (step 7's decision 8).
- **The click rule** (ADR-0012 §2): unchanged code, now proved through the pipeline: `platform::MouseCapture` consumes
  the click that captures (it never reaches the action map), the next left click is `interact`
  (`tests/platform/test_click_interact.cpp`).

## The run report

`world.interaction` (`{}` without a room's collection): `bound`, `reduceMotion`, `state`, `hoveredPickId`,
`focusedPickId`, `highlightedPickId`, `focusedGame`, `pulled` (pick ids in hexadecimal), `hoverChanges`, `focusChanges`,
`steps`, `pullOuts`, `pulledFrames`, `maxPulled`, `staticDrawnWhilePulledFrames` (frames in which a pulled item still
had a static record drawn, or a hidden record had no dynamic copy: always 0), `animationFrames`, `walkerEaseFrames`,
`cursorFrames`, `labelFrames`, `label` (the title shown last), `events` (`{frame, event, pickId?, state?}`: "hover",
"pull-out", "pulled", "in", "state", "focus", "step"; at most 512, `eventsDropped` past them). `world.hoverChanges`
counts the interaction's hover changes. The phase `animation` (`kPhaseAnimation`) is open while the walker eases, a
pull moves or the lift moves.

## Tests

- `tests/unit/app/test_world_focus.cpp` (`[app][world_focus]`), on the golden plans' layouts (50, 890 and 2,000
  titles): **a pulled item ends as ADR-0012 says and never passes through its neighbours** (every item: the static pose
  at 0; spine-out 70% of its depth out with its cover to the viewer, or as far as its row reach needs; face-out tipped
  10 degrees, top forward; at 50 points along the pull its oriented box never overlaps another item's and never sinks
  into an occluder deeper than standing on its board; no jump); **the reticle at every item's Focus pose rests on that
  item** (the ray from the pose's camera picks it through the room's pick index, the eye 0.9 m from its face) **and the
  walker there is clear of every collider** (the room's collision world from the real prefabs); the layout as the
  interaction's room (items, sections, the steps equal to `SlotNavigation`'s, the viewer); the D-pad's repeat (frames 0,
  24, 30, 36, ...; the fast cadence; a tap; the last pressed of two; no step for an arrow held into Focus); the pick ray
  from the reticle and the cursor; the label's anchor; and **the wiring as the app drives it**, frame by frame on the
  50-title room (snes-room's size): the hover within 6 frames, the pull-out 8 to 10 frames later and out 14 to 16
  frames after, a face-out item inspected and Indietro to Focus, a still stick held 60 frames, steps along the band,
  across into the wall shelf, between its ledges and back, a held step repeating after 0.4 s, Back to FreeRoam with
  every item back in its slot; the same under reduceMotion, every transition instant and the dwell kept.
- `tests/unit/app/test_world_label_document.cpp`: the document's ids, data model and bindings, no focusable element, no
  transition.
- `tests/platform/test_click_interact.cpp`: a click while the mouse is free only captures it; once captured, a left
  click is interact; freed again, the next click only captures it.
- `tests/smoke/test_world_focus.cpp` (`gpu.world focus: ...`, label `gpu`, every run hidden, lockstep), on the snes-room
  fixture imported, walker poses computed from the plan the run wrote:
  - **hover and pull-out** (`world_hover_pull.json`: the walker looks down from the dressed level above onto the band's
    item 5.4, later up again): nothing hovered before frame 40, the hover by frame 55 (the reticle on the middle of the
    face from frame 50 at the latest: within 6 frames), the pull-out 8 to 10 frames later, fully out 14 to 16 frames
    after; at frame 99 the item hovered, highlighted, pulled, labelled, the outline's pixels around it; the hover gone
    by frame 135 once the walker looks up, no other item hovered, the item in 11 to 13 frames later, no outline, no
    label; the longer run's events equal to the shorter's; the "animation" phase recorded;
  - **Focus and the D-pad** (`world_focus_steps.json`): Inspect at frame 40, Focus at 70, FreeRoam at 440; every step on
    its frame to the item `world::SlotNavigation` names over the layout (band 5.4 -> 5.5 -> the wall shelf's upper
    ledge, across fixtures -> nothing up -> its lower ledge -> back into the band at 5.5 -> 5.4, 5.3, 5.2, 5.1 held, at
    frames 360, 384, 390, 396); no other focus change (the still stick of frames 100-159); at frame 229 Focus on the
    wall shelf's item, pulled, outlined, labelled; at the end every item in its slot, the last one hovered;
  - **reduceMotion** (world.json `{"reduceMotion": true}`, the same script): the plan unchanged, the same steps on the
    same frames, no walker ease, no animation frame, every pull-out whole on the frame it starts, the dwell kept;
  - **a spine-out cartridge** (one of G10's additions imported after a first run: the plan updated, 1 spine-out):
    `world_focus_enter.json`, pulled out with its cover turned to the viewer, Focus at frame 40, outlined; Back at 100,
    put back while the hover rests on it;
  - **the cursor** (developer builds, `--devui-open`): every frame's ray through the cursor, the item hovered. (A
    frame-limited run never captures the mouse, so every automated run's ray comes from the cursor resting at the
    window's centre: the reticle's ray; the conversion of a cursor elsewhere is the unit test's.)
  - **the frame budget** (hidden, `[.bench]`, Release on a quiet machine): the steps script at 1920 x 1080 without
    vsync, the "animation" phase's p99 within 33.3 ms. The other cases print the phase's times (`WARN`) without checking
    them: they share the machine with whatever else runs.
  Every run: `staticDrawnWhilePulledFrames` 0, no event dropped, 0 high or medium GL messages.

## Results

On the reference machine (RTX 2080 SUPER, driver 617.42), `RV_LAUNCH_BACKGROUND` set and `RV_LAUNCH_ALLOWLIST` unset,
with other worktrees building and testing most of the time (timings informative; the gate measures on a quiet machine):

- **Builds**: `msvc-x64-debug`, `msvc-x64-relwithdebinfo` and `msvc-x64-release` with 0 warnings.
- **Unit tests**: `./scripts/test.ps1` passed 1,642 of 1,642 in Debug (4,017 s under the load), wrapped in `python -I
  scripts/focus_watch.py --watch build --out build/focus/13a-unit.json`: 0 findings.
- **GPU tests**: `./scripts/test.ps1 -Preset msvc-x64-debug-gpu` (wrapped in `python -I scripts/focus_watch.py --watch
  build --out build/focus/13a.json`: 0 findings, no foreground change, no visible window of a test process): 1,696 of
  1,697 passed, the 4 `gpu.world focus` cases, the 7 `gpu.world` cases and golden G01 among them; the one failure,
  `gpu.video: out of the view the decoder pauses, the picture kept` (the clip's first frame not yet shown when the
  walker turned away, under the machine's load), runs on the test room with `--mock-catalog`, which has no world and
  keeps the code path this step does not touch; it passed three times of three when run again alone. After the final
  edits (formatting and comments) the `[world]` cases and G01 passed again in Debug, the `[world_focus]` cases in
  Release (the cursor case skipped: no overlay in Release), the step's unit and platform tests in Debug.
- **Golden G01** (`--scene m0_testroom`): `gpu.golden/G01` passed (0 pixels apart; the run has no world).

## Figures

- The interaction's room at the commit (`WorldHover::setRoom`: the pick index, the navigation, every item's row reach;
  the hidden `rv_tests "[world_focus][bench]"`, Release, medians of 11): 2.5 ms for the 890-title room (98 occluders),
  6.6 ms for the 2,000-title one (145); 0.6 ms for snes-room's 50 items in Debug (the log line).
- The frame budget during the animations (the hidden `rv_smoke_tests "world focus: the frame budget*"`, Release, 1920 x
  1080, no vsync, the steps script): the `animation` phase 185 frames, p50 2.62 ms, p99 3.41 ms, max 3.55 ms (budget:
  p99 33.3 ms). The vsync-bound smoke runs: p99 6.2 to 7.0 ms in Debug and Release.
- The steps script's run (Debug): 8 steps, 9 pull-outs, at most 3 items off their slots at once (two being pushed back
  during the held steps), 441 of 480 frames with an item out, 0 frames with a static record drawn while its item was
  out, 132 frames of walker eases; under reduceMotion 0 eases, 0 animation frames, never more than 1 item out.
- The pull-outs (golden plans): every spine-out item 70% out but those at the ends of their rows (30 of 834 on the
  890-title plan, 68 of 1,944 on the 2,000-title one), at most 80.6%.

## Screenshots

Under the build tree, not committed (the fixture's invented titles and generated covers):

- `build/msvc-x64-debug/smoke/world_focus/hover/pulled.png`: 0.9 m from the hero wall's band, "Trellis Tales" tipped
  forward out of the band, its ivory outline, its title label above it, the magazines and storage boxes on the dressed
  levels around; `back.png`: the walker looking up again (the module's top level and the wall), the band whole at the
  bottom of the view, no outline, no label.
- `build/msvc-x64-debug/smoke/world_focus/steps/focus.png`: Focus on the wall shelf's upper ledge, "Acorn Errands" at
  the view's centre, outlined and labelled; `end.png`: back in FreeRoam at the band, "Saffron Satchel" hovered.
- `build/msvc-x64-debug/smoke/world_focus/spine/spine_focus.png`: the spine-out "Zinnia Zephyr (USA)" slid out of its
  row and turned, its cover (sideways) to the viewer, outlined, its label above; nothing intersects the shelf.
- `build/screenshots/13a/display_case.png`: a display case's item hovered through the glass (the plan kept with the
  fixture's media folder gone: the `media-unknown-kept` notice).
Reviewed: the outline hugs the dynamic copy, the static item is gone from its slot while pulled, the label reads over
the room in the quick catalog's look, the tipped items stay clear of their shelves and of the display case's glass.

## Decisions taken within the step

1. **The spine-out cartridge stays on its end when it turns**: a quarter turn about the vertical shows its cover
   sideways (the title on the cover runs upwards; the label above reads it). Rolling it upright in front of the row
   would sweep its 12.5 cm corners past the boards above and below its slot at 70% of its depth out; the hand holds it
   upright in Inspect (13b).
2. **The turn keeps clear of the row** (`rowReach()`): a cartridge at the end of a row settles farther than 70% out
   (at most 80.6% on the golden plans) rather than through the module's side panel, which stands up to 1.4 cm proud of
   the slots; the slide comes first, so the item turns only once clear of its neighbours.
3. **A face-out item tips its top forward by 10 degrees** about its bottom front edge: a leaning item (the display
   cases, the wall shelf) can only come off what it leans on, and the band's upright items tip alike.
4. **Focus aims at the middle of the item's face**, not at the pulled item: the reticle then rests on the item's pick
   target (the pulled item stands just in front of it), so the hover stays on the focused item and a still stick keeps
   the focus; a pulled spine-out item sits a few centimetres off the view's centre for low shelves.
5. **The D-pad repeats while held in Focus**, at the quick catalog's cadence (step 7 left the choice here): dense
   spine-out rows of 33 slots are crossed without a press per cartridge; a step is still exact (each one is the
   navigation's).
6. **The interaction runs before the walker moves** (step 7's harness order): the pick ray is the camera the last frame
   left, the ease's pose is applied in the same frame and the label is projected with this frame's camera.
7. **Without a room's collection the interaction still takes the World context's Back and quick catalog**, as the shell
   levels it returns; runs without a world keep the previous path, so golden G01 and the catalog cases are untouched.
8. **Frame-limited runs rest the cursor at the window's centre**: they never read the user's mouse (as they never
   capture it), so a hidden automated run's cursor ray is the reticle's; interactive runs read SDL's position.
9. **Inspect is reachable in 13a and Avvia ends at once as returned**, so no state can get stuck before 13b and 15a; the
   item stays at its pulled pose while "in the hand".
10. **The label is an RmlUi document of the product UI** (as the notice), positioned in pixels of the view through data
    bindings, its band as wide as the view and centred on the anchor, so a title of any length stays centred over the
    item without measuring text in C++.
11. **The static records of a pulled item stay in their batch with `kHidden`** (step 8's design): no group is rebuilt,
    16 bytes per record and change.

## Open issues

- The cursor's ray ignores the developer overlay's panels: a cursor over a panel picks the scene behind it (the panels
  keep the click). Harmless for a developer tool; ImGui's "wants the mouse" could gate it.
- The wheel is not passed to the interaction yet (`InteractionFrame::wheelNotches`): 13b's zoom.
- The label's gap and margin are pixels, not dp.

## Notes for later steps

- **13b**: `WorldHover::update` already passes the frame; draw the inspected item from `inspectView()` (its pulled pose
  at lift 0) on the same dynamic path (`CollectionInstance::appendRenderItems` at the held pose), dim through
  `PostSettings`, pass `RawInputFrame::mouseWheel` as `wheelNotches`; the CRT follows `focused()` after its dwell.
- **14a**: the card's `ViewArea` goes into `FocusViewer::area` (Focus frames the item in the part the card leaves).
- **15a**: replace the "ended at once as returned" branch of `WorldHover::update` with `AppCatalog::requestLaunch` by
  GameId and `launchEnded()` from `LaunchFlow`; "Mostra nella stanza" is `WorldInteraction::showInRoom()` (the room's
  pick id: `CollectionRoom::layout().findGame()`).
- **15b**: a live update that keeps items should call `WorldInteraction::roomChanged()` rather than `setRoom()` (which
  drops the hover and Focus); `rowReach()` depends on the neighbours: rebuild `CollectionRoom` for a changed layout.

## For the merge

- **Order** (the roadmap's rule: 10c, then 12, then 13a): the hooks in shared files are
  - `src/app/src/world_session.hpp`, `.cpp`: the `WorldHover` member and its construction (reduceMotion), the commit's
    pick-index lines replaced by `m_interaction->setRoom(...)`, `hover()` replaced by `interact()`, `context()`,
    `viewItems()`, `label()`, `dropCollection()` and `record()` (two lines each);
  - `src/app/src/retroverse_app.cpp`: `State::view()` (one line), `State::interactWorld()` and `placeWalker()` (new),
    `back()` split into `performBack()`, `onUpdate`'s World-context block, the walker's update and the label (the old
    `world->hover(...)` call removed);
  - `src/app/src/collection_instance.hpp`, `.cpp`: four methods, `writeFlags` generalized to `writeFlag`, the records
    kept (`m_sources`);
  - `src/app/src/product_ui.hpp`, `.cpp`: `setWorldLabel()` and the label's panel next to the notice's;
  - `src/app/include/rv/app/run_report.hpp`, `src/run_report.cpp`: `InteractionRecord` under `WorldRecord::interaction`;
  - `src/app/CMakeLists.txt`: two source lines.
- **tests/smoke/test_world_focus.cpp** repeats a few helpers of `test_world_room.cpp` (the imported fixture, the room
  shown) to stay out of 10c's and 12's edits of that file; fold them into a shared header after the wave.
- **CLAUDE.md** (not edited by the step):
  - "Running the app": in the room, the item the reticle rests on within 2.5 m is outlined and labelled with its title;
    after 150 ms it leaves its slot (a face-out cartridge tips forward, a spine-out one slides out and turns its cover
    to the viewer); E, a captured left click or the pad's south button focuses a spine-out cartridge (the walker eased
    to 0.9 m in front of it) or inspects a face-out one; in Focus the arrows and the D-pad step between cartridges
    (repeating while held), Esc / B leaves one level; `reduceMotion` in `<user data>/config/world.json` makes every
    transition instant; with the mouse free (the developer overlay open) the cursor picks.
  - "Running the app", `--run-report`: `world.interaction` (above); the phase `animation`.
  - GPU smoke tests: `gpu.world focus: ...` (`tests/smoke/test_world_focus.cpp`); input scripts `world_hover_pull.json`,
    `world_focus_steps.json`, `world_focus_enter.json`.
  - "Layout and layering": `rv_app_cli`'s `world_focus.hpp` (the room's side of the hover, the pull-out and Focus) and
    `world_label_ui.hpp`; `rv_app`'s `world_hover` (the interaction in the app) and `world_label_panel`.
- **docs/architecture.md**: the app's world gains the interaction (the frame order above).
- **docs/roadmap.md**: row 13a done, with the figures above.
