# I5 step 4: arcade cabinets playing clips

Roadmap row: I5 step 4. Decision: [ADR-0016](../decisions/ADR-0016-screens-and-media.md) §6 and §7 (amended on
2026-10-28 for this step), amending ADR-0014 §6. Beside I4b: the room editor (I4b steps 3 and 4) is merged, so the
cabinets are movable fixtures.

## What was built

- **The picture's aspect** (`rv_render`, `rv_app_cli`'s `screen_aspect.hpp`): `MaterialUniforms` gains
  `pictureRect` (a `vec4` after the CRT fields: 112 bytes), read by `unlit/unlit_emissive.frag` only under the new flag
  `kPictureRect`; `gl::MaterialRegistry::setPictureRect` sets or clears it, rewriting the block only when it changes.
  `app::pictureRect()` letterboxes a wider clip and pillarboxes a narrower one, centred; within 2% of the screen's
  aspect it is the whole screen and the flag stays clear, so a 4:3 clip on a 4:3 tube draws exactly as before. The
  service sets it for every screen showing a clip (its `ScreenSurface`'s size: `SceneInstance::screenSurfaceSize()`),
  the whole screen for a still or its pattern; the bars are the tube's black, its scanlines, mask, vignette and edge
  the screen's.
- **Which cabinets play** (`cabinet_screens.hpp`, GPU-free): `CabinetAttract` (the nearest cabinet in view within 3 m
  whose screen faces the eye within 60 degrees, kept to 3.5 m and 70 degrees, after a 1 s dwell; at once off when it
  no longer qualifies) and `chooseCabinets()` (Focus `focus`; Inspect and its launch `direct`; attract `ambient` in
  free roam; none while the editor runs). `video.json`'s `cabinet.enabled` is attract, `cabinet.showOnFocus` the
  focused cabinet.
- **The cabinets in the screen service** (`screen_service.cpp`): `setCabinets()` each frame with the cabinets asking
  for their clips and their collection's pool; each is a screen of the scheduler (the bound room's game, its own
  looping clip, no stills: its cover is its item's front), sharing the slots with the hero and the secondaries;
  `cabinetsShown()` says which show their clips, each on its slot's pooled material (its texture override the slot's,
  its picture rectangle the clip's on the cabinet's portrait screen). A cabinet asked for again keeps its clip (a class
  change from attract to Focus to Inspect keeps it playing); one no longer asked for lets it go ("unwanted"). The
  cabinets are audible candidates like the other screens (the inspected one, `direct`, is heard over the hero).
- **The world's side** (`world_cabinets.cpp`, new; `world_session.cpp` touched in `viewItems()` and
  `dropCollection()` alone): `cabinetScreens()` from the interaction's room when it is the bound room and its items are
  screens (`CollectionInstance::screenItems()`: a family whose label material is unlit); `showCabinets()` hides a
  playing cabinet's static front (`CollectionInstance::setVideoFront()`) and draws it with its pooled material on the
  dynamic path (`frontRenderItem()`), a pulled or inspected one's copy carrying it (`appendRenderItems()`), the hero
  texture's front left out over it. The pool (`CollectionInstance::videoMaterials()`): as many materials as the decoder
  slots, the label's `MAT_Screen_Arcade` with a base colour texture of its own, made the first time a cabinet plays and
  registered after everything else (no handle moves).
- **The hero cabinet never follows the selection** (`ScreenBinding::followsSelection`, false for the arcade's hero
  cabinet): neither the focused cabinet nor the quick catalog's selection.
- **The paused frame** (`gl::Texture2D::copyOf`): a bound screen whose slot is taken from a clip it still wants
  (`needed`, `budget`) keeps a GPU copy of its last frame until that clip has a slot again, in place of its still; with
  one decoder the hero cabinet keeps its frame while an inspected cabinet plays (ADR-0016 §6).
- **The report**: `video.screens` lists the cabinets playing at the end (`kind` `cabinet`), each screen's
  `pictureRect` and `heldFrame`; `video.cabinetEvents` (each cabinet's clip shown and its cover again, by frame) and
  `heldFrameShows`.

## The editor (I4b)

**No cabinet plays while the room editor runs** (`chooseCabinets()`'s `editing`, ADR-0016 §6 amended): from the frame
a session begins every cabinet shows its cover, the hero cabinet keeps its playlist (the hero corner is no movable
fixture); after the session attract starts again from its dwell. A session holds, moves and nudges cabinets, probes
their places and rebuilds the room's static part; a clip on a cabinet the hand holds, or on one whose collection is laid
out again under it, would follow a pose that changes every frame for no gain. Outside the editor a cabinet's clip
follows its item by pick id: a collection laid out again in place (a live update, the editor's save) keeps its playing
cabinet's front (`setItems()` carries it over as it carries the hero front), and the next frame's request takes the
item's new transform; a collection dropped (a floor committed again, a ride, the editor's floor fallback) goes with
`dropCollection()` and the service's `unbind()`, which let every cabinet go. Proven by `[cabinets][edit]` (unit) and
`gpu.cabinets: no cabinet plays while the room editor runs ...`.

## Decisions beyond ADR-0016

- **Attract's hysteresis** (3.5 m, 70 degrees) and its **reset** outside free roam: after Back the cabinet shows its
  cover until a new dwell, so Back reads as "put back".
- **Inspect is `direct` through its launch** (the states Launching and Returning), so the inspected cabinet's clip
  position is kept for the return, as the hero's is.
- **The pool is the collection's**, one material per slot (slot i on material i): two cabinets of one clip share it.
- **The aspect tolerance is 2%**: the real SNES clips include 584 x 446; 256 x 224 (8:7) is pillarboxed, as the
  ADR's "never stretched" asks, although a console's pixels were not square (open question below).
- **The kept frame** is every bound screen's, not only the hero cabinet's, and only for a slot taken from a clip still
  wanted; a stop out of view still ends on the still (§2).

## Proofs

- Unit: `test_cabinet_screens.cpp` (5 cases: the attract choice along a row, its distances, angles and hysteresis; the
  dwell and the hand-over along the row, walking away, turning away, `reset()`; the classes; the editor; the screen
  id), `test_screen_aspect.cpp` (2: the rectangle's table for 4:3, 16:9 and portrait clips on 4:3 tubes, the hero
  cabinet and the cabinets' 3:4 screens; the whole screen within the tolerance and without sizes),
  `test_shader_sources.cpp` (the block's new member and flag), `test_run_report.cpp` (the new fields' round trip).
- GPU (`tests/smoke/test_cabinets.cpp`, `[cabinets]`, 4 cases on the building fixture with a generated clip for each
  placed mame title: 4:3, one portrait, one 16:9):
  - the inspected cabinet's clip shows in the hand (`direct`) within half a second of interact, plays on in Focus after
    Indietro, and its cover is back on the frame of Back; the screenshots' middle changes in 4 frames while it plays
    (12,512 and 10,414 pixels) and not at all after Back (0); the hero cabinet never took a selection;
  - attract opens the cabinet in front of the walker at frame 60 (the 1 s dwell), shows it, plays on while the walker
    steps back half a second, and stops on the frame a half turn takes it out of view;
  - `--max-decoders 1`: the inspected cabinet takes the hero cabinet's slot (`needed`), the hero shows its kept frame
    meanwhile, and its clip reopens once the cabinet is put back;
  - no cabinet plays while the editor runs (its cover from the session's first frame), attract again 1 s after it.
- GPU (`test_screens.cpp`, `[aspect]`): a 16:9 clip on two 4:3 tubes of `screens_four` letterboxed (`pictureRect`
  (0, 0.125, 1, 0.75)), a portrait one pillarboxed (0.219, 0, 0.5625, 1); probes on the screenshot: the bars at most
  8 luma, the picture between them 118 to 125.
- Goldens: G01 to G18 0 pixels apart in Debug (largest difference 0/255), G08a and every 4:3 screen among them; new
  G20 (`g20_cabinet_focus_golden.png`: the cabinet of the building fixture's one mame clip in Focus, its clip frozen
  at 1.0 s, frame 30's counter "G 0030" letterboxed on the portrait screen), which proves the counter: the frozen
  frame's pixels are the golden's.
- Real library: `[real-library][arcade]` (`tests/unit/app/test_arcade_real.cpp`): below.

## Results

- Debug unit tests (`./scripts/test.ps1`): 2125 of 2125 passed.
- Release unit and GPU (`./scripts/test.ps1 -Preset msvc-x64-release-gpu`): 2268 tests (2125 unit, 143 GPU), all
  passed but one skipped by design (the overlay's pick, as always) and `gpu.video: out of the view the decoder pauses,
  the picture kept`, whose clip's first frame had not come before the walker turned away (no frame uploaded: a race of
  a lockstep run with the decoder's thread on a loaded machine, another agent building beside it; no cabinet or
  rectangle is involved in the test room); it passed 5 runs of 5 alone. The four `gpu.cabinets` cases, the `[aspect]`
  case and G20 passed in Debug and Release.
- Goldens: G01 to G18 0 pixels apart in Debug and Release (largest difference 0/255), G08a and every 4:3 screen among
  them: the flag keeps the 4:3 tubes' pixels. G20 added (rendered in Debug, 0 pixels apart in Release). No other golden
  changed.
- Real library (`./scripts/real-library.ps1 -Filter '[real-library][arcade]'`, Release, ES-DE closed): passed, **0
  differences among 542,120 entries**. The whole library imported in 20.8 s; the real arcade room places 36 cabinet
  screens. Attract played on **6 of the 6 featured cabinets**, each from frame 60 or 62 (its 1 s dwell, the first frame
  within two frames); their pictures: two letterboxed 4:3 clips (0.562 of the screen's height), three filling the
  portrait screen (vertical games within 2%), one pillarboxed (0.909 of its width). The first, inspected from its Focus
  pose, played in the hand from the frame of interact (its clip the hero cabinet's start choice: one shared slot); the
  hero cabinet never took a selection. Two clips playing (the hero and an attract cabinet): at most 2 decoders, the
  ground floor at 311 to 317 MiB of GPU memory (acceptable 512); frame p50 6.1 ms in these lockstep runs (their p99 holds
  the covers' cold waits, not the clips).

## Screenshots

Hidden, frame-limited Release runs on the building fixture with generated clips (`build/i5/step4/`):
`cabinet_inspected_in_hand.png` (the portrait clip in the hand beside its card), `cabinet_focus_playing.png` (the same
cabinet in Focus after Indietro, its clip on the pulled-out screen, the neighbours' covers), `cabinet_cover_after_back.png`
(its cover again after Back), `g20_cabinet_focus_frozen.png` (G20: a 4:3 clip letterboxed on the portrait screen, held
at 1.0 s, "G 0030"), `cabinet_attract_close.png` and `cabinet_attract_row.png` (attract on the cabinet in front of the
walker, then a step back: it plays on among the covers of its row), `crt_letterbox_pillarbox.png` (`screens_four`: the
16:9 clip letterboxed on crt_a and crt_c, the portrait one pillarboxed on crt_b and crt_d, held at 1.0 s). Beside style
§15.2 and §15.3 and `example.png`: the clips glow through the picture tube's scanlines and grille, the bars are the
tube's black within its rounded edge, the cabinet's moulding and marquee stay its own; in the hand the clip is the
preview linked to its object. The real room's screenshots stay under `build/msvc-x64-release/real-library/arcade` (the
user's covers and clips: never committed).

## Open issues

- A 256 x 224 clip (8:7, a console's frame stored with square pixels) is pillarboxed on a 4:3 tube; such clips were
  shown stretched to 4:3 on a television. A per-system pixel aspect would need data we do not have.
- The quick catalog's selection of an arcade game plays nothing (the hero cabinet never follows): its cabinet could play
  it (`focus`) when it stands in the eye's room.
- The kept frame is a copy per screen (at most 640 x 480 x 4 bytes, counted with the stills); with one decoder a
  hero cabinet in view keeps its frame for as long as a cabinet is inspected.
- Cabinets' screens show the cover, never the game's screenshot (unchanged since I6 step 6).
