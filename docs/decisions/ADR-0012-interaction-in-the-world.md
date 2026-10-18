# ADR-0012: Interaction in the world: picking, focus, inspection, the card, launching and the hero CRT

- Status: accepted
- Date: 2026-10-18
- Relates to: ADR-0004 (brings forward from I5 the screen material binding, `waitForStoppedDecoders()` at shutdown and
  the filter of NVIDIA's message 131154; the scheduler, the priority boost, upload staggering and audio stay I5's),
  ADR-0005 (Vedi media, Informazioni and pictures in the UI), ADR-0006 (the launch and the return, unchanged)

## Context

M2 adds "picking, inspect, search/teleport" (specification §22) and style's phase B a "pickup/inspection of a cartridge"
and a "TV playing a local video". The documents fix the frame: interaction modes FreeRoam, Inspect and FocusScreen with
clear rules for input, camera, exit and restore, no physical sequence mandatory for `Start` (specification §12.2); a ray
from the camera against interaction shapes, a maximum distance, a discreet indicator, targets that stay usable "even
when covers are close together" (§12.3); the states `OnShelf`, `Focused`, `HeldForInspection`, `Launching`, `Returning`,
animations "not mandatory and interruptible" (§8.3); three visual states (on the shelf, focus with a light highlight of
the silhouette and a small contextual label, inspection with the object near the camera and a discreet card) and target
timings (feedback under 100 ms, pickup 200-400 ms, exit 150-250 ms, "stable focus without jitter") (style §16);
`HoverOutline` "without flicker" among the P0 shaders (style §25); SC-02 (start a SNES cartridge from the room, return
with camera and selection preserved) and SC-03 / SC-08 (the TV shows the focused title's video or a fallback, never
crashing on a bad file).

The user decided (2026-10-18) that most cartridges stand **spine-out**, dense, with the title on the spine, a few
dozen **face-out** in display cases, on the featured shelf and in the hero band, chosen by priority (ADR-0010 §3: on
the reference library 56 of 890), in a compact room whose shelving is sized to the collection, and that **pointing at
a spine-out cartridge pulls it out and shows its cover**; that one video decoder serves the hero CRT in I3
(SelectedGame with the fallback chain to the cover and a test pattern; the scheduler, audio and playlists stay in I5,
ADR-0004); that teleport and the map stay in I4 while the quick catalog's card may offer "show in the room" if cheap;
and that automated tests never take the foreground.

What exists: `scene::Camera::rayFromNdc`, `core::Aabb::intersectRay`, `scene::frontViewpoint`, `aimAt` and `ViewArea`;
the `interact` action (E, the left mouse button, the pad's south button) bound and unused, while a click into the window
also captures the mouse; the action map's world and UI input contexts, Back levels (`backAction`); A1's `LaunchFlow`
with the keep policy, the idle mode and the game-session panel; `AppCatalog::requestLaunch(token, ui::CatalogEntry)`;
the quick catalog's card with readiness and blockers (`kBlockerCodes`); `rv_media` with `gl::VideoTexture` (ADR-0004);
`MAT_Screen_CRT`; a stencil in every render target. A spine is 2.0 cm wide with a 2.4 cm pitch (the shelf's 4 mm gap):
from 1.5 m it covers about 17 px at 1920 x 1080.

## Decision

### 1. States

`app::WorldInteraction`, a GPU-free state machine in `rv_app_cli` (tested without a window), owns the world's input
context and the transitions:

| State | Enter | What the player does | Leave |
|---|---|---|---|
| FreeRoam | start, Back from Focus | walks and looks; the reticle's target is hovered | `interact` on a hovered item: Focus |
| Focus | `interact` on an item, "Mostra nella stanza" | the camera stands in front of the item's shelf section (eased), the item pulled out; the D-pad and the arrows step to the neighbouring item (left, right, the level above or below, across modules; empty slots skipped); looking moves the pick | `interact` (or `ui_activate`): Inspect; Back: FreeRoam, the item put back |
| Inspect | `interact` in Focus, or directly from FreeRoam on a face-out item | the item held in front of the camera, rotated with the look actions (right stick, mouse drag), zoomed with the triggers or the wheel; the card beside it | Avvia: Launching; Back: Focus, the item at its pulled-out pose |
| Launching / Returning | Avvia | A1's flow (the game-session panel, the idle mode) | the return: Inspect of the same item |

Back leaves one level, after the levels `backAction` already has (popups, the overlay, the quick catalog). The quick
catalog (`quick_catalog`: Tab, the pad's north button) opens from FreeRoam and Focus as today. A failed transition (the
item vanished from the plan, a launch refused) leaves the state usable and says why on the card.

Each state has its input context (specification §12.2: clear rules for input focus), built on the action map's world
and UI contexts, so that no button means two things at once:

- **FreeRoam**: the world's actions as today, `interact` added.
- **Focus**: walking off; `ui_up`, `ui_down`, `ui_left`, `ui_right` (the arrows, the D-pad) step between items; the
  look actions move the pick within the shelf section; `interact` or `ui_activate` inspects; `back` returns to
  FreeRoam; `quick_catalog` opens the catalog.
- **Inspect**: a context of its own: the look actions rotate the item (the right stick; the mouse while captured), the
  triggers (`ui_page_up`, `ui_page_down`, read as zoom here) and the wheel zoom; the card reads `ui_activate`,
  `ui_media`, `ui_info` and `ui_back`, and the arrows and the D-pad move between its buttons. The pad's north button is
  thus Vedi media in Inspect and the quick catalog elsewhere (specification §12.1's Y, ADR-0005's `ui_media`).

### 2. Picking

- **The ray**: from the reticle (the view's center) while the mouse is captured or the pad was used last; from the
  cursor while the mouse is free (the developer overlay open). At most **2.5 m** from the eye.
- **Targets**: one box per placement, its visible face inflated by 1 mm on each side (a spine's target is 2.2 cm wide in
  its 2.4 cm pitch), kept per cluster (a shelf level of a module, a display case) for a coarse test; the room's
  colliders (shelves' boards and sides, walls, furniture) occlude, so an item behind a board is never picked. A
  `world::PickIndex` in `rv_world` (GPU-free) answers in microseconds.
- **Stability** (style §16.2): the hovered item changes only when the ray has been on another item's target, shrunk by 2
  mm on each side, for 3 consecutive frames; a held still stick or mouse never changes the focus. In Focus the D-pad
  steps are exact.
- **The click**: a left click while the mouse is free only captures it (as today) and is not an `interact`; once
  captured, a left click is `interact`. E and the pad's south button are always `interact`.

### 3. Hover, focus and the pull-out

- **Hover** (FreeRoam): within the frame, the item's silhouette gets the **outline** (`HoverOutline`: the item drawn
  once more into the stencil after the lit pass, a pass after FXAA marking the pixels within 2 px of the stencil's edge
  at 1080p, scaled with the output's height, in the theme's warm white; after FXAA so that it neither softens nor
  flickers; the stencil is read through a stencil-index texture view of the scene's depth-stencil texture, whose depth
  view the ambient occlusion keeps reading), and a small label in the product UI near the item's projected position
  shows the title (RmlUi, Italian context, no colour transition).
- **Pull-out**: after **150 ms** on the same item, a spine-out cartridge slides forward out of its row by 70% of its
  depth and turns to show its front (the cover, with its hero texture as soon as it is loaded, ADR-0011 §8) in **250
  ms**, and slides back in **200 ms** when the hover leaves it; a face-out item tilts towards the viewer instead. Each
  animation is interruptible (a new target reverses it from where it is) and skipped entirely when the world's
  `reduceMotion` setting is on (`<user data>/config/world.json` until the settings screen of I7). Frame-limited runs
  with a fixed frame time animate on the fixed time, so scripted runs are reproducible.
- **Focus** eases the walker in **350 ms** to `scene::frontViewpoint` of the item's shelf section at 0.9 m, framed in
  the part of the view the card leaves free (`ViewArea`), the same pull-out kept.

### 4. Inspection

The item is drawn by the dynamic path (its static instance hidden by its flag) and lifted in **300 ms** to 0.6 m in
front of the camera (style §6.2: 0.6-1.2 m), its front facing the eye; the look actions rotate it about its center (yaw
unlimited, pitch +-80 degrees), the triggers or the wheel zoom between 0.4 and 1.0 m; the walker does not move. The
scene behind is dimmed by one stop and vignetted through `PostSettings` (no depth of field in I3). Back returns it in
**200 ms**. Every timing is skippable with the same button that started it and instant under `reduceMotion`.

### 5. The card and the media viewer

- `assets/ui/game_card.rml` (Italian strings, dp sizes, bundled fonts, no colour transitions), placed beside the
  inspected item without covering it (style §16.3): title, platform, year, developer, genre, players, the launch
  readiness and its first blocker with the quick catalog's sentences (`kBlockerCodes`, one shared test table), the
  number of other versions in the catalog (`duplicate-title` entries of ADR-0010), the cover as a picture, and the
  actions **Avvia** (`ui_activate`; the pad's Start button too), **Vedi media** (`ui_media`), **Informazioni**
  (`ui_info`), **Indietro** (`ui_back`). Every action is also a button of the card: `ui_media` and `ui_info` have no key
  by default (letters stay with text fields), so with a keyboard the arrows reach the buttons and Enter presses one,
  and a click presses one too (specification §23.1: keyboard and mouse as well as the pad). The card's sentences, the
  reasons of section 8 included, live in its RML document like every product string; the code passes codes.
- **Pictures in the UI**: an image source scheme `rv-texture:<key>` in the vendored RmlUi GL3 renderer maps keys to
  textures the app registers (the hero textures, the video's texture in I5), so RmlUi never loads the user's files
  itself.
- **Vedi media** opens a viewer of the game's images that exist: cover, back cover, physical media, 3D box, screenshot,
  title screen, marquee, fan art (left and right page through them, Back closes); videos and manuals in it are I5's.
  **Informazioni** shows the description and the other versions' titles and regions.

### 6. Launching from the world and the return (SC-02)

Avvia calls `AppCatalog::requestLaunch(token, catalog::GameId, title)` (a GameId overload; the path is resolved on the
worker right before the launch, as for the quick catalog) and A1's `LaunchFlow` unchanged: the readiness checked again,
the session recorded. Before the hand-over the app records the world's state (the state, the item's GameId, the walker's
pose, the camera, the clip's position), pauses the cover loader (ADR-0011 §6) and stops the hero CRT's decoder, waiting
at most 1 s for its thread to end (specification §11.4: decoders stopped before the emulator starts; a decoder that
does not end in time is logged and the launch goes on, the thread ending on its own); the `VideoTexture` is kept. After
the return it restores the pose and Inspect of the same item, resumes the loader and opens the same clip again at the
recorded position. A refused or failed launch restarts the decoder at once, shows A1's outcome text on the card and
stays in Inspect. Quitting while a game runs leaves the emulator running (A1) and finds the decoder already stopped. A
refresh (step 13r) started before the launch goes on in its slices; the world applies its result after the return.
Automated launch tests run hidden with `RV_LAUNCH_ALLOWLIST` naming the fake emulator and `RV_LAUNCH_BACKGROUND` set,
so that neither program takes the foreground.

### 7. The hero CRT and its one decoder

- `app::ScreenService` owns **one** `media::VideoDecoder` (a runtime assertion: never two; NFR-07's limit of 2 is I5's
  scheduler) and the hero CRT's screen through a texture override on its `MAT_Screen_CRT` material
  (`MaterialRegistry::setTextureOverride`, set again when a `VideoTexture` reallocates). `rv_app` links `rv_media`.
- **What it plays** (`SelectedGame`, no playlist): the video of the item focused or inspected for at least **300 ms** (a
  dwell, so scanning a shelf does not restart the decoder at every spine); without a selection since the start, the most
  recently played placed game with a video, else the first featured title with one, else the idle picture; after a
  selection, its video keeps looping until another item is selected. The CRT shows the video of the inspected item
  during the inspection (style §15.3).
- **Fallback chain** (style §21, SC-08), shown at once while the decoder opens and whenever it fails (`DecodeFailure`):
  screenshot, title screen, cover, then the original idle pattern (colour bars) - still pictures through the hero
  texture path; a file ES-DE would not read (an `.flv`) is never opened.
- The decoder is paused while the CRT is outside the view and **stopped before a launch**; `waitForStoppedDecoders()`
  runs at shutdown. Clips are silent (audio is I5's; a third of the SNES clips carry 96 kHz audio to resample then). The
  process priority boost (ADR-0004 point 9) and the upload staggering of its budgets are I5's: one 480p decoder is
  inside the measured envelope (upload p99 about 0.4 ms). The GL debug filter drops NVIDIA's expected message 131154
  (ADR-0004's consequence), so frame-limited runs stay clean.
- `--video-freeze-pts <seconds>` (frame-limited runs only) seeks the hero CRT's video to that time and holds the frame,
  for goldens (style §26.1: "freeze a test frame at a fixed timestamp").

### 8. "Mostra nella stanza"

The quick catalog's card offers **Mostra nella stanza** for a game the plan places (the where-is index of ADR-0010 §9):
the catalog closes, the walker eases in **600 ms** (skippable, instant under `reduceMotion`) to the item's Focus pose
and the item is pulled out. For an unplaced game the action is absent and the card says why, one sentence per
`unplaced` reason in the RML document ("Non esposto: un'altra versione è sullo scaffale", "Non esposto: senza
copertina", "Non esposto: la stanza è piena"). Within the one room only; the map and the teleport between rooms are
I4's. The quick catalog's own Vedi media and Informazioni open the same viewer and details as the world's card (ADR-0005
left them doing nothing until this iteration).

## Consequences

- Dense spine-out shelves stay usable with the pad: a coarse aim then exact D-pad steps, a hover that never jitters, the
  cover shown by pulling the item out instead of asking the player to read 2 cm spines from afar.
- Everything is reachable without an animation: `reduceMotion` makes every transition instant, and the quick catalog's
  Avvia and "Mostra nella stanza" reach any game without walking.
- The world and the quick catalog share the readiness texts and the launch flow, so a launch behaves the same from
  either.
- The hero CRT shows real ES-DE clips from I3 on with the decoder of I1; I5 replaces the single-decoder rule with the
  scheduler (two decoders, priorities, playlists, audio) without changing the screen binding or the fallback chain.
- Left click no longer captures and interacts at once; scripts that click to capture keep working.
