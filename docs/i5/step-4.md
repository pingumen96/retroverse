# I5 step 4: arcade cabinets playing clips

Roadmap row: I5 step 4. Decision: [ADR-0016](../decisions/ADR-0016-screens-and-media.md) §6 and §7 (amended on
2026-10-28 for this step and on 2026-10-29 by its review: "Review" below), amending ADR-0014 §6. Beside I4b: the room
editor (I4b steps 3 and 4) is merged, so the cabinets are movable fixtures.

## What was built

- **The picture's aspect** (`rv_render`, `rv_app_cli`'s `screen_aspect.hpp`): `MaterialUniforms` gains
  `pictureRect` (a `vec4` after the CRT fields: 112 bytes), read by `unlit/unlit_emissive.frag` only under the new flag
  `kPictureRect`; `gl::MaterialRegistry::setPictureRect` sets or clears it, rewriting the block only when it changes.
  `app::pictureRect()` letterboxes a wider clip and pillarboxes a narrower one, centred; within 2% of the screen's
  aspect it is the whole screen and the flag stays clear, so a 4:3 clip on a 4:3 tube draws exactly as before. The
  service sets it for every screen showing a clip (its `ScreenSurface`'s size: `SceneInstance::screenSurfaceSize()`),
  the whole screen for a still or its pattern; the bars are the tube's black (since the review its lit off-black), its
  scanlines, mask, vignette and edge the screen's.
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
- **The world's side** (`world_cabinets.cpp`, new; `world_session.cpp` touched in `viewItems()` and `dropCollection()`
  alone): `cabinetScreens()` from the interaction's room when it is the bound room and its items are screens
  (`CollectionInstance::screenItems()`: since the review the family `world::kArcadeRoomFamily`); `showCabinets()` hides
  a playing cabinet's static front (`CollectionInstance::setVideoFront()`) and draws it with its pooled material on the
  dynamic path (`frontRenderItem()`), a pulled or inspected one's copy carrying it (`appendRenderItems()`), the hero
  texture's front left out over it. The pool (`CollectionInstance::videoMaterials()`): as many materials as the decoder
  slots, the label's `MAT_Screen_Arcade` with a base colour texture of its own, made the first time a cabinet plays and
  appended to the registry (no handle moves).
- **The hero cabinet never follows the world's selection** (`ScreenBinding::followsWorldSelection`, false for the
  arcade's hero cabinet): the focused cabinet plays itself. Since the review it follows the quick catalog's selection,
  as every hero screen does (the step had dropped it: a regression against main).
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
- **The aspect tolerance is 2%**: the real SNES clips include 584 x 446. Since the review a console's low-resolution
  frame near 4:3 (at most 240 rows, within 15%: 256 x 224) is shown at 4:3, as its television showed it.
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
  hero cabinet never took a selection. Two clips playing (the hero and an attract cabinet): at most 2 decoders, 311 to
  317 MiB of GPU memory on the floor these runs showed (the ground floor, which holds the arcade room: not the
  building's busiest floor; acceptable 512); frame p50 6.1 ms in these lockstep runs (their p99 holds the covers' cold
  waits, not the clips).

## Screenshots

Hidden, frame-limited Release runs on the building fixture with generated clips (`build/i5/step4/`):
`cabinet_inspected_in_hand.png` (the portrait clip in the hand beside its card), `cabinet_focus_playing.png` (the same
cabinet in Focus after Indietro, its clip on the pulled-out screen, the neighbours' covers),
`cabinet_cover_after_back.png` (its cover again after Back), `g20_cabinet_focus_frozen.png` (G20: a 4:3 clip letterboxed
on the portrait screen, held at 1.0 s, "G 0030"), `cabinet_attract_close.png` and `cabinet_attract_row.png` (attract on
the cabinet in front of the walker, then a step back: it plays on among the covers of its row),
`crt_letterbox_pillarbox.png` (`screens_four`: the 16:9 clip letterboxed on crt_a and crt_c, the portrait one
pillarboxed on crt_b and crt_d, held at 1.0 s). Beside style §15.2 and §15.3 and `example.png`: the clips glow through
the picture tube's scanlines and grille, the bars are the tube's black within its rounded edge, the cabinet's moulding
and marquee stay its own; in the hand the clip is the preview linked to its object. The real room's screenshots stay
under `build/msvc-x64-release/real-library/arcade` (the user's covers and clips: never committed).

## Open issues

- A horizontal arcade clip fills only about 56% of a cabinet's portrait screen (a 4:3 picture letterboxed on the
  298 x 398 mm label: 0.562 of its height), the rest the tube's off-black. Candidate remedies, an ADR-0014 follow-up
  (BACKLOG): a landscape screen for the horizontal games, or a deeper item.
- The kept frame is a copy per screen (at most 640 x 480 x 4 bytes, counted with the stills); with one decoder a
  hero cabinet in view keeps its frame for as long as a cabinet is inspected.
- Cabinets' screens show the cover, never the game's screenshot (unchanged since I6 step 6).

## Open questions

- An attract cabinet stays silent while the hero cabinet is in view, even 10 m away: the audible screen is the best
  class first (`room` over `ambient`), then the nearest. Style §15.4 asks for one dominant TV in a room and a diegetic
  volume by distance; whether a cabinet playing beside the walker should be heard over a far hero cabinet is open.

## Review

The step's review (2026-10-29) found five bugs, a race in two test files, four tests that could pass while the feature
was broken, and doc and code nits; every one is fixed here.

### Bugs

- **A verdict change dropped every cabinet mid-frame** (`ScreenService::update()`): the clip restarted from 0, the
  cover flashed for a frame and its "cover" event came a frame late. The cabinets are screened again instead
  (`rescreenCabinets()`, which `setGames()` uses too): only a cabinet whose game left the room's games, or whose clip
  changed (left out, back, another file), lets its clip go; the others play on untouched.
- **An inspected cabinet launching fell back to its slot's box** for its view test and distance (`inHand` held for
  Inspect only). It holds whenever the interaction holds the item: Inspect, Launching, Returning
  (`app::cabinetInHand()`).
- **A kept frame was counted before it was applied** (`heldFrameShows`, the screen record's `heldFrame`):
  `applyHeldFrame()` tells whether the override was set, and only then does the screen count as showing it.
- **The pooled materials' overrides were never cleared**: a pooled material no cabinet shows on now draws its own white
  again (texture override 0, picture rectangle cleared), and every one does when the cabinets go and with the service;
  not when the registry is about to be cleared (`unbind(false)`), whose handles go with it.
- **The quick catalog previewed nothing in the arcade** (a regression against main): the world's selection and the
  quick catalog's reach the service apart (`ScreenSelection`, `WorldSession::screenSelections()`, `RetroverseApp::
  updateScreens()`); a screen follows the catalog's, else the world's, and the arcade's hero cabinet ignores only the
  world's (`ScreenBinding::followsWorldSelection`). ADR-0016 §6's amendment is narrowed to match.

### Determinism

`gpu.video: out of the view the decoder pauses, the picture kept` was racy by design: a lockstep run presents frames on
simulated time while its decoders run on the wall clock, so the frame a clip's first picture showed on depended on the
decoder thread's pace; `test_cabinets.cpp`'s frame windows (`<= 70`, `<= kDwellFrames + 30`) hid the same race. A
lockstep run now waits, as for its stills (`kStillWait`), its frozen frame (`kFreezeWait`) and its sound
(`kAudioWait`), for the first frame of a clip a slot opened for a screen in view: at most `kFirstFrameWait` (NFR-06's
2 s), once per opening. The report records the waits (`video.firstFrameWaits`, `firstFrameWaitMaxMs`,
`firstFrameWaitTimeouts`) and an open's position (`slotEvents[].fromSeconds`). The tests are exact where that made
them deterministic:

- `gpu.video: out of the view ...`: the clip opens on frame 0 with its first frame (one wait, no timeout), pauses on
  frame 2 when the turn takes the screen out of view, one frame uploaded;
- the inspected cabinet's clip shows on frame 40 (the interact), its cover on frame 160 (Back); attract shows on frame
  60 (the dwell's end); after the editor attract shows on the session's end frame + 60; a launch's cabinet on frame 40.
  What still waits on the wall clock stays a window: a slot opens only once its previous decoder's thread has ended
  (one decoder: the cabinet opens on frame 71 to 75, the hero again on 171 to 175).

`gpu.video: out of the view ...` alone, Release: **10 passes in 10 runs** (it failed once in step 4's full Release run).
The full suite's waits for a first frame each took a few milliseconds and none ran out.

### Tests that could pass while the feature was broken

- **Attract** required only a lit screen (a cover is lit too): it requires the clip to move in the screen's window
  across two screenshots 4 frames apart, as the Inspect case does.
- **One decoder** required only `heldFrameShows`: the walker stands where, inspected, the cabinet beside the hero
  cabinet leaves the hero cabinet's screen in view at the left edge (chosen from the plan: within the reticle's reach,
  at most 25 degrees off the cabinet's screen); two screenshots while its slot is taken are identical, lit (not
  black), and apart from the hero cabinet's still, which the same run without clips shows there.
- **New: a cabinet moved by the editor and saved** (`[cabinets][edit]`): from the card's Sposta the game is moved to
  the next slot (the room's collection laid out again in place), Salva; after the session attract plays it at its new
  slot (its screen id the new slot's), the clip moving there on two screenshots, and its static front never drawn
  beneath it (`world.cabinetCoverBeneathFrames` 0, counted on every frame a cabinet's clip draws).
- **New: a launch from the inspected cabinet** (`[cabinets][launch]`): Avvia with the fake emulator; the cabinet's
  clip stops for the launch at about 0.67 s and opens again after the return there (`fromSeconds`), in the hand
  (`direct`, in view, playing), its cover never shown (its slot's last frame stays up while the game runs).
- **New: the quick catalog's selection** of an arcade game in the arcade plays on the hero cabinet (`origin`
  "selection", its clip opened on the hero cabinet's screen).

### Docs and nits

- `docs/architecture.md`'s `render` row is one line again.
- The kept frame's rule is consistent in ADR-0016: §2's Visibility bullet points to it, §10 reads "its kept frame while
  its clip waits for a slot", the §6 amendment quotes §2 exactly, `docs/i5/step-2.md` notes it.
- A console's low-resolution frame near 4:3 is shown at 4:3 (`app::displayAspect()`: at most 240 rows within 15% of
  4:3; unit rows 256 x 224, 320 x 224 and 320 x 240 fill a 4:3 tube, 256 x 240 and 512 x 448 keep their own, 640 x 480
  unchanged, 256 x 224 letterboxed as 4:3 on a cabinet); ADR-0016 §7 amended, the BACKLOG entry done.
- The letterbox bars are the lit tube's off-black (`RV_CRT_BLACK_LEVEL`, linear 0.01) through its scanlines, mask and
  vignette, not 0; `sampleUv` is clamped to [0, 1]. Only G20 changed (regenerated on purpose); every 4:3 screen stays 0
  pixels apart.
- A cabinet's screen size is the item's "Label" socket (the family's 298 x 398 mm label), not the item's 300 x 400 mm.
- The hero cabinet is found by its prefab kind (`kCabinetPrefabKind`), the playing items by their family
  (`world::kArcadeRoomFamily`), not by "any unlit label".
- `VideoScreenRecord`'s comment says what it lists (the cabinets asking at the end, "cover" with texture 0 when their
  clip does not show); `cabinetEvents` past 512 are counted (`cabinetEventsDropped`).
- `allScreens()` returns a list kept up to date on each change, not a vector allocated per call.
- "Registered after everything else" reads "appended: no handle moves"; the lines over 120 characters named by the
  review are wrapped; the 311 to 317 MiB figure names its floor (the ground floor, which holds the arcade).

### ADR amendments (2026-10-29)

- ADR-0016 §2, Visibility: "A slot taken from a clip still wanted (`needed`, `budget`) leaves its screen its kept frame
  instead (§6 amended)."
- ADR-0016 §3: the hero cabinet's "never follows the selection" points to §6 (the quick catalog's plays on it).
- ADR-0016 §6's amendment: Inspect and its launch hold the cabinet in the hand; the pool is appended (no handle
  moves), its materials cleared once unused; the cabinets named by their family and sized by its label; a change of
  the games or verdicts lets go only the cabinets whose clip changed; "never follows the selection" is the world's
  selection, the quick catalog's plays on the hero cabinet; §2 quoted exactly.
- ADR-0016 §7: a console's low-resolution frame near 4:3 is shown at 4:3; the bars are the lit tube's off-black.
- ADR-0016 §10: "its kept frame while its clip waits for a slot".

### Results

- Debug unit tests (`./scripts/test.ps1`): 2127 of 2127 passed (the build clean, no warning).
- Release unit and GPU (`./scripts/test.ps1 -Preset msvc-x64-release-gpu`): 2273 tests (2127 unit, 146 GPU), all
  passed but one skipped by design (the overlay's pick, as always) and `gpu.world: the stored room shows at the first
  frame before the snapshot ...`, whose one-frame run now shows the start choice's clip on frame 0 (the first-frame
  wait) where it expected the screenshot that raced it: its expectation is "video" now, and it passed alone. The seven
  `gpu.cabinets` cases, `[aspect]` and G20 passed in Debug and Release.
- Goldens: G01 to G18 0 pixels apart in Debug and Release (largest difference 0/255), G08a and every 4:3 screen among
  them. G20 regenerated on purpose (the bars' off-black, the label's size); 0 pixels apart in Release after.
- Screenshots (Release, `build/i5/step4/`): retaken, `cabinet_focus_playing.png`, `g20_cabinet_focus_frozen.png` and
  `crt_letterbox_pillarbox.png` among them (the bars a dark lit tube within its rounded edge, never a hole); new
  `cabinet_hero_kept_frame.png` (the hero cabinet at the left edge on its kept frame, "K 0034", beside the inspected
  cabinet) and `cabinet_hero_still_without_clips.png` (the same view without clips: its still),
  `cabinet_moved_attract.png` (attract at the moved cabinet's new slot), `cabinet_after_launch.png` (the clip in the
  hand after the return).

### Merge order

`i4b/step-7` (the generator 1.5) changes `tests/fixtures/world/scene/digests.json` and may merge first; this branch was
not rebased onto it. Whichever merges second takes the other's digests (this step changes no plan or scene digest).
