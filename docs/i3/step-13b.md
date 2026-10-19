# I3 step 13b: inspection

Roadmap row: I3 step 13b (wave 4, on 13a and 10c; 14a and 15b run beside it in their own worktrees). Decisions:
[ADR-0012](../decisions/ADR-0012-interaction-in-the-world.md) §1 (the Inspect input context and its Back levels), §4
(the item lifted into the hand, turned with the look actions, zoomed with the triggers and the wheel, the scene behind
dimmed by one stop and vignetted, Back lowering it) and §7 (the hero CRT following the item focused or inspected after
its 300 ms dwell). Step 7's `WorldInteraction` already holds the state, the context, the lift, the turn, the zoom and
the Back levels; 13a enters Inspect and leaves the item at its pulled-out pose. This step draws it: the held pose, the
dimming, the wheel, the CRT's selection and the run report's record. It fills step 0a's stub `world_inspection`
(`rv_app`), adds new files and keeps to short hooks in shared files, listed under "For the merge".

## What was built

| File | What it holds |
|---|---|
| `rv/app/world_inspect.hpp`, `src/world_inspect.cpp` (`rv_app_cli`, new, GPU-free) | `heldItemTransform()`, `heldRadius()`, `heldDistanceLimit()`, `heldDistance()`, `InspectDimming` and `inspectDimming()`, `screenSelection()`; the constants `kInspectDimShare` 0.5, `kInspectVignette` 0.25, `kHeldClearance` 6 cm, `kHeldMinDistance` 0.2 m |
| `src/world_inspection.hpp`, `.cpp` (`rv_app`, step 0a's stub) | `WorldInspection`: the held pose of the focused item on the dynamic path, the outline's mark of the held item, the view's dimming, the run report's `inspect` record |
| `assets/shaders/outline/outline_dim.frag` (new) | the dimming: black blended outside the marked silhouette at the share's opacity, in linear light |
| `rv/render/outline_pass.hpp`, `src/outline_pass.cpp`, `src/scene_renderer.cpp` | `OutlineShaders::dim`, `OutlineSettings::edge` and `dimOutside`; `drawEdge()` draws the dimming before the edge |
| `src/scene_graphics.cpp` | the `outline_dim` program (one table line, one shader-set line) |
| `src/world_hover.hpp`, `.cpp` (13a's) | the `WorldInspection` member: `observe()` after the update, `markedItem()` for the outline's mark, `itemModel()` in `viewItems(items, camera)`, `applyInspection()`, `focusedGame()`, the record |
| `src/world_session.hpp`, `.cpp` | `viewItems(items, camera)`, `applyInspection()`, `screenSelection()`, the hero front noted for the record |
| `src/retroverse_app.cpp` | `State::view()` (the camera passed, the dimming applied), the wheel in `interactWorld()`, the CRT's selection |
| `src/app_input.hpp`, `.cpp` | `mouseWheel()`: the frame's wheel notches |
| `rv/app/run_report.hpp`, `src/run_report.cpp` | `InspectRecord` under `InteractionRecord::inspect` (`world.interaction.inspect`) |
| tests | `tests/unit/app/test_world_inspect.cpp`; `tests/smoke/test_world_inspect.cpp`; `tests/input_scripts/world_inspect.json`, `world_inspect_crt.json`; `tests/unit/integration/test_shader_sources.cpp` (the new stage) |

## The held item (`world_inspect.hpp`)

- **The pose** (`heldItemTransform(pulled, view, camera, distance)`): at lift 0 the pulled-out pose exactly (13a's
  `pulledItemTransform()`); at lift 1 its centre `distance` along the view, its prefab axes the camera's (+X right, +Y
  up, +Z the cover towards the eye: the cartridge upright and facing the viewer, a spine-out one rolled off its end),
  turned by the inspection's yaw about the camera's up (positive: the cover to the viewer's right) and then by its pitch
  about the camera's right (positive: the cover tilted up); in between the centre on the straight line and the
  orientation along the shorter arc (`scene::interpolatePose` on the interaction's eased lift), the prefab's scale kept.
  The walker does not move in Inspect, so the pose follows the camera of the frame being drawn.
- **Out of its fixture** (`heldDistanceLimit()`, `heldDistance()`): the zoom's distance (0.4 to 1.0 m, 0.6 m at each
  entry: the interaction's) is drawn at most where the view ray enters the item's cluster box (its shelf level, its
  display case's items) grown by 6 cm, less the radius of the sphere that holds the item whatever its turn (half its
  box's diagonal, 7.6 cm for a cartridge); never nearer than 0.2 m. At a Focus pose (0.9 m from the face) the default
  0.6 m is never cut short and zooming out stops about 0.8 m from the eye; the shelf's boards and the display case's
  glass stay behind the item.
- **The dimming** (`inspectDimming(lift, vignette)`): `dimOutside` 0.5 x lift (one stop at full lift), the view's
  vignette raised by 0.25 x lift (at most 1), so the scene fades in with the lift and back with the lowering.
- **The CRT's selection** (`screenSelection()`): the quick catalog's selected game while it is open, else the item in
  Focus, Inspect, Launching or Returning; nothing in FreeRoam (the CRT keeps the clip it plays). `ScreenService`'s dwell
  (300 ms, step 11) does the rest: stepping faster than the dwell never restarts the decoder.

## The app's side

- **The frame**: `WorldHover::update` runs the interaction (13a) and `WorldInspection::observe` counts the inspections
  and the requests; the collection's highlight goes to `markedItem()` (the highlighted item, else the item in the hand
  while lifted); `viewItems(items, camera)` draws every pulled item at its pulled pose and the focused one, while lifted,
  at `itemModel()`'s held pose (its hero texture front with it, step 12's dynamic copy); the view takes
  `applyInspection()`: `outline.dimOutside`, `outline.edge` (only around a highlighted item: none in Inspect, the
  outline back in Focus while the item is lowered) and `post.vignette`.
- **The dimming in the renderer**: the outline pass (step 8) already marks the highlighted records into the stencil
  after the lit pass; with `dimOutside` above 0 its second step first draws `outline_dim.frag` over the whole output,
  blended with `GL_FRAMEBUFFER_SRGB` on (so the blend scales linear light), discarding the marked pixels; the edge
  follows when `edge` is set. One fullscreen triangle a frame while an item is lifted; nothing otherwise (G01 is
  unchanged: `edge` true and `dimOutside` 0 are the defaults).
- **The wheel**: `AppInput::mouseWheel()` (the raw frame's notches) goes into `InteractionFrame::wheelNotches` while the
  world has the input; no action is bound to it (step 7's decision 10).
- **The north button**: Inspect's context reads `ui_media` and not `quick_catalog` (step 7); the record counts Vedi
  media (`mediaRequests`) and any quick catalog opened from Inspect (`catalogOpenedInInspect`, always 0). Vedi media and
  Informazioni open nothing until 14b.

## The run report

`world.interaction.inspect`: `inspections`, `liftFrames` (frames drawn with 0 < lift < 1), `heldFrames` (lift 1),
`dimmedFrames`, `mediaRequests`, `infoRequests`, `catalogOpenedInInspect`, `minDistance` / `maxDistance` (the zoom's,
over the held frames), `minDrawnDistance` / `maxDrawnDistance` (as drawn), and the last frame drawn off the pulled pose:
`pickId`, `lift`, `yawDeg`, `pitchDeg`, `distance`, `drawnDistance`, `heldCenter`, `heldFront`, `heldUp` (in the
camera's frame: x right, y up, z towards the viewer) and `heroFront` (the hero texture on its cover).

## Tests

- `tests/unit/app/test_world_inspect.cpp` (`[app][world_inspect]`): the held pose for twelve camera poses (centre on
  the view, cover to the eye, upright; yaw and pitch each way; the zoom), lift 0 and a non-finite lift giving the pulled
  pose exactly, the scale kept, the lift's path continuous; the distance limit (looking at the box, away, from inside,
  degenerate input) and the drawn distance; **on the golden plans (50, 890 and 2,000 titles), at every item's Focus
  pose and at 0.4, 0.6 and 1.0 m, the held item's sphere clear of every occluder and every item's box**, the default
  hold never shortened; the dimming; the CRT's selection table; the inspect record read back; and the inspection as the
  app drives it on the 50-title room: the lift in 18 frames, 90 degrees in 30 frames at full stick, the walker still,
  the triggers to 0.4 and 1.0 m (drawn short of the band), 4 wheel notches 20 cm, Indietro lowering it in 12 frames to
  its pulled pose exactly, no frame's centre more than 5 cm from the last, Back to FreeRoam.
- `tests/smoke/test_world_inspect.cpp` (`gpu.world inspect: ...`, label `gpu`, every run hidden, lockstep), on the
  snes-room fixture imported, from the Focus pose of the band's item 5.4:
  - **held, turned, zoomed, lowered** (`world_inspect.json`): at frame 99 Inspect on the item, no outline or label,
    lifted in 17 to 19 frames, turned 45 degrees and tilted 15, 0.6 m on the view axis, its front and up as
    `heldItemTransform()` says, the hero texture on its cover, the walker where it stood; the scene behind it at 25 to
    55% of the same view's linear luminance without the inspection (three probes away from the item); at frame 130 at
    0.4 m; the whole run: Inspect at 40, Focus at 260, FreeRoam at 300, the zoom from 0.4 to 1.0 m (drawn less than
    1.0 m), one Vedi media and no quick catalog from the north button, 27 to 31 frames of lift and lowering, every item
    back in its slot;
  - **the hero CRT** (`world_inspect_crt.json`): Inspect at 40 (selected after its dwell), Indietro at 70, four D-pad
    steps 10 frames apart: 14 frames after the last step the CRT still shows the inspected item's game, at frame 170 the
    last focused item's, exactly one selection more; never more than one decoder.
  Every run: `staticDrawnWhilePulledFrames` 0, no event dropped, 0 high or medium GL messages.

## Results

On the reference machine (RTX 2080 SUPER), `RV_LAUNCH_BACKGROUND` set and `RV_LAUNCH_ALLOWLIST` unset, with other
worktrees building and testing (timings informative). Debug only, as the step's brief asked:

- **Build**: `msvc-x64-debug` with 0 warnings.
- **Unit tests**: `rv_tests "[world_inspect],[world_focus],[run_report],[world_interaction]"` (59 cases) and
  `"integration/shaders*"` (20) passed.
- **GPU tests**: `rv_smoke_tests "[world_inspect]"` (2 cases), `"[world]"` (the `world`, `world focus`, `world
  inspect` and `world start` cases; 21 of 23 in the second run: the two failures are the snes-stress cover cases, its
  2,000-title room still "generating" or 1,533 of 2,000 layers resident when the run ended under the machine's load:
  they never reach the interaction) and golden G01 (`golden*`: 0 pixels apart; the run has no world, and `edge` true,
  `dimOutside` 0 draw what step 8 drew). Repeated after merging main (step 15b), see the commit.
- **Figures** (the smoke runs): the lift 17 frames drawn between the pulled pose and the hand, the lowering 11; the
  scene behind at 0.47 to 0.50 of its linear light at the probes (one stop, the vignette's r^4 barely reaching them);
  zoomed out to 1.0 m the item is drawn at 0.79 m (the band's box); the hero texture on the held cover in every held
  frame; one decoder.

## Screenshots

Under the build tree, not committed (the fixture's invented titles and generated covers); G07's candidates (the card
comes with 14a):

- `build/msvc-x64-debug/smoke/world_inspect/held/held.png`: "Trellis Tales" in the hand 0.6 m in front of the eye,
  turned 45 degrees to the right and tilted up 15, its slot in the band empty behind it, the shelf, the magazines and
  the floor lamp a stop darker; `plain.png` the same view before the inspection (the item hovered, outlined, labelled);
  `zoom_in.png` at 0.4 m; `end.png` back in FreeRoam, the item in its slot, hovered.
- Reviewed: the cartridge upright with its cover (the hero texture) facing the eye, no outline or label while held, the
  item undimmed against the dimmed room, nothing of the shelf in front of it. Turned towards the floor lamp, the
  label's sheen lightens the cover's darks (the black stripes read brown at 45 degrees; facing the eye they stay
  black): the label material's response, left to look development (16).

## Decisions taken within the step

1. **The scene is dimmed after the anti-aliasing through the outline's stencil**, not by the exposure: ADR-0012 §4 says
   "through `PostSettings`", but an exposure would dim the held item with the room. The outline pass already marks the
   item's silhouette in the stencil; one blended triangle outside it takes half the linear light (one stop). The
   vignette is `PostSettings`' own, raised.
2. **The held item is upright in the camera's frame** (its up the camera's up, its cover facing the eye): when the
   walker looks down at a low shelf the cartridge tilts with the view, as a hand holds it to the eyes. A spine-out
   cartridge rolls off its end on the way (the quarter turn 13a's decision 1 left to the hand).
3. **Pitch after yaw**: the yaw turns the item about the camera's up, the pitch then tilts it about the camera's right,
   so the stick's vertical always tilts it towards or away from the viewer whatever its turn.
4. **The zoom is drawn short of the item's fixture** (its cluster's box grown by 6 cm, less its sphere): standing 0.9
   m from a shelf, a held item at 1.0 m would stand inside it. The interaction's distance still runs from 0.4 to 1.0 m
   (the record keeps both); the drawn one stops where the fixture begins.
5. **The CRT's selection is the quick catalog's while it is open, else the world's**: one rule for both screens; FreeRoam
   passes nothing, so a hover never restarts the decoder (ADR-0012 §7: focused or inspected).

## Open issues

- Turned towards a lamp, the held cover's label catches its sheen and its darks wash out (`held.png`); the label
  material's roughness is look development's (16).
- The held item casts its shadow like any shell (onto the shelf below the light); a lamp close behind it could light
  it from the back. Look development (16) may give the held item a light of its own.
- The display case's glass is not an occluder; the 6 cm clearance keeps the held item in front of its panes for the
  generated cases (checked on the golden plans), not for any case geometry.
- The wheel is read only in Inspect (the model's rule); the asset preview's wheel is its own.

## For the merge

- **Hooks in shared files** (the roadmap's rule for wave 4):
  - `src/app/src/world_hover.hpp`, `.cpp`: `#include "world_inspection.hpp"`, the `m_inspection` member, `observe()` in
    `update()` (two lines), `markedItem()` in `syncCollection()` (one line), `viewItems()` gains the camera and calls
    `itemModel()`, `applyInspection()`, `inspection()`, `focusedGame()`, `record()` one line;
  - `src/app/src/world_session.hpp`, `.cpp`: `viewItems(items, camera)` (the hero front noted, two lines),
    `applyInspection()`, `screenSelection()`, `#include <rv/app/world_inspect.hpp>`;
  - `src/app/src/retroverse_app.cpp`: `State::view()` (`viewItems(view.items, camera)`, `applyInspection(view)`),
    `interactWorld()` (`frame.wheelNotches`), `onUpdate`'s CRT selection (`world->screenSelection(selected)`);
  - `src/app/src/app_input.hpp`, `.cpp`: `mouseWheel()`;
  - `src/app/include/rv/app/run_report.hpp`, `src/run_report.cpp`: `InspectRecord`, `InteractionRecord::inspect`, its
    JSON;
  - `src/render/include/rv/render/outline_pass.hpp`, `src/render/src/outline_pass.cpp`, `scene_renderer.cpp`: the dim
    program and settings; `src/app/src/scene_graphics.cpp`: two lines;
  - `src/app/CMakeLists.txt`: `src/world_inspect.cpp` in `rv_app_cli`.
  14a's card goes beside the held item (`FocusViewer::area` for Focus; the held item stays on the view's axis, so the
  card's area may want `heldItemTransform()` to take an offset: 14a's call); 15a's launch keeps `WorldLaunchRecord`'s
  inspection, which this step draws as it is restored.
- **tests/smoke/test_world_inspect.cpp** repeats a few helpers of `test_world_focus.cpp` (as that file did of
  `test_world_room.cpp`); fold them into a shared header after the wave.
- **CLAUDE.md** (not edited by the step):
  - "Running the app": in Inspect the item is held upright 0.6 m in front of the eye, the scene behind dimmed by one
    stop; the right stick or the captured mouse turns it, the triggers, Page Up / Down and the wheel zoom it (0.4 to
    1.0 m, drawn short of its shelf), Esc / B (Indietro) lowers it back to Focus; the pad's north button is Vedi media
    there; the hero CRT plays the clip of the item focused or inspected for 300 ms.
  - `--run-report`: `world.interaction.inspect` (above).
  - GPU smoke tests: `gpu.world inspect: ...` (`tests/smoke/test_world_inspect.cpp`); input scripts
    `world_inspect.json`, `world_inspect_crt.json`.
  - "Layout and layering": `rv_app_cli`'s `world_inspect.hpp` (the held pose, the dimming, the CRT's selection);
    `rv_app`'s `world_inspection`; the shader `outline/outline_dim.frag`.
- **docs/architecture.md**: the outline pass also dims the scene outside a marked item.
- **docs/roadmap.md**: row 13b done, with the figures above.
