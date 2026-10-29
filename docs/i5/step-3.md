# I5 step 3: secondary TVs

Roadmap row: I5 step 3. Decision: [ADR-0016](../decisions/ADR-0016-screens-and-media.md) §5 (amended on 2026-10-29
for this step), §2's `ambient` class, amending ADR-0014 §1's furniture rule. Beside I4b: the room editor (steps 3 and
4) and expansion rooms (step 7) are merged.

## What was built

- **The piece** (`rv_geometry`, `prefab_secondary_tv.cpp`): `secondary_tv`, a low sideboard (tv_cabinet's body)
  carrying a small consumer CRT (the crt's model at 0.46 x 0.4 x 0.32 m, style §15.1's "one model in two sizes"),
  0.8 x 0.36 m on the floor (the narrow sideboard's footprint), 0.9 m tall; its screen is the CRT's `ScreenSurface`.
  Manifest `furniture.secondary_tv.generic`, family `furniture.secondary_tv`, the asset preview and the edit bar
  ("Televisore").
- **The packs' furniture** (`room_look.hpp`): a pack lists one `secondary_tv` piece, without a weight, in a decoration
  kind's list (`ThemeFurniture::secondaryTv`). Every pack but the arcade's and the hub's lists it under `tv_cabinet`:
  the classic pack's walnut sideboard and ivory set (its first furniture: the built-in pack too), the 90s collector's
  low oak sideboard without drawers and grey set, the optical-era lounge's black set on tall legs, the handheld
  archive's oak one, the computer studio's filing sideboard. Its materials are the pack's `tv_cabinet` and `crt`
  kinds'. The reader refuses a second one, a weight, one in the hero corner, and a decoration piece of kind `crt`.
- **Where it stands** (`scene_builder.cpp`, `secondaryTvFixtures()`): among the room's decorations of that kind whose
  own prefab's footprint it fits, ordered by `deriveSeed(room seed, "secondary_tv", fixture id)`, the first; in a room
  deeper than 12 m the next one on another wall too (the review ranks them away from the doorway instead: "Review").
  None in the arcade (its cabinets are its screens), the hub and the halls (no pack of theirs lists one). It stands
  where its fixture stands, judged within that fixture's own footprint, so a sideboard the editor moves carries it. Its
  screen shows the `standby` pattern until the app plays on it. The plan, its slots, lights and validation are
  untouched; the other decorations draw as before.
- **The screens** (`screen_service.cpp`): `findScreens()` takes the hero by its role (`...fx:hero.crt`, the arcade's
  hero cabinet keeping that id), then every `secondary_tv` of its room as a secondary (`ambient`, its own shuffle,
  within `proximityMeters`; step 2's scheduler unchanged). `ScreenService::rebind()` moves the bound screens onto the
  new objects when the editor rebuilds a room's static part in place: a moved TV keeps its object id and, its
  screen's picture unchanged, its material (the registry keys a material by object, role and picture; the review
  corrects this note's first "a new object with a new material").

## Decisions beyond ADR-0016

- **One prefab, not two pieces.** The sideboard and its set are one object of kind `secondary_tv`, so the decoration's
  machinery (one piece per fixture, its footprint, the edit bar's name, `drawnFixtures()`) holds it unchanged and the
  app finds it by its kind.
- **Placed by rule, not by weight.** The count is the room's (one, two past 12 m), so the piece is no weighted
  variant; the sockets' order is the room's seed. "Past 12 m" is the room's depth: 14 and 16 m rooms hold two.
- **Under `tv_cabinet`.** A TV replaces a sideboard (same footprint); a room whose decoration drew no sideboard has
  none (a room whose side walls are all modules, plan-2000-seed-7; `BACKLOG.md`).
- **Its own footprint, not its socket's.** The guard of other variants is the socket's footprint at home, so a moved
  variant falls back to the plan's prefab; the TV is judged within its fixture's own prefab where it stands, which the
  editor already validated: the TV follows the sideboard and never leaves its box.
- **Expansions** are rooms of their own: each holds its TV by the same rule and its own seed.
- **The distance** an ambient TV counts by is the eye's to its object's bounds' middle, as every screen's (step 2).
- **Hand-made scenes** keep binding further `crt`s as secondaries (`screens_four`).
- **The hero's own smoke cases** (`world_start`, `world_inspect`, `world_launch`: one decoder thread, one start)
  turn the secondary TVs' autoplay off in their data directories' `video.json`: they follow the hero alone, and the
  snes room's TV opened a second decoder. The live floor case's bound room holds both screens: its threads stay within
  the budget of 2.

## Proofs

- Unit (`tests/unit/world/test_secondary_tv.cpp`, 6 cases and 1 hidden): the reader; every pack's TV resolving with
  a `ScreenSurface`, the built-in pack's equal to the classic file's; every golden plan's room with three packs (one TV
  when the room has a sideboard, the same choice twice, drawn with its fixture's id, standby screen and the pack's
  materials, every other object as without it, none in plan-2000-seed-7); the depth rule (12 m one, 14 and 16 m two on
  two walls) and none in the arcade or with the hub's pack; a sideboard moved 0.3 m and turned 15 degrees keeping its
  TV within its box, the drawn room's validation as the plan's; a room the generator grew to 14 m (4,300 titles, world
  seed 1) holding two, `fx:w11` and `fx:e11`, its validation unchanged (since the review: one, at `w11`; "Review").
  `test_era_furniture.cpp` (the classic pack's
  furniture is the TV alone), `test_prefabs.cpp` (the prefab's roles, bounds, sockets, refusals), the manifest and
  family counts.
- GPU (`tests/smoke/test_secondary_tv.cpp`, `[secondary_tv]`, 2 cases on `esde/snes-room`, whose TV stands at
  `fx:w02`): walking from the doorway towards it with `proximityMeters` 1.2 m short of it, the hero opens on slot 0 on
  the room's first update and keeps it, the secondary (`ambient`, in view) opens on slot 1 on the frame the walk comes
  within its proximity (frame 33 in Debug), each its own texture; with `--max-decoders 1` the secondary never opens
  and shows its still (its game's cover), the hero on slot 0.
- Goldens: new G19 (`g19_secondary_tv_golden.png`): the snes-room's hero CRT and its TV, both clips held at 1.0 s on
  two slots, the TV's playlist one sound clip through `video.json`'s per-screen keys (its shuffle may start on one of
  the fixture's damaged clips). Regenerated on purpose where the TV stands in view: G02, G06, G10, G12, G17, G18 (the
  commit names each). G08b forgets the first run's verdicts (the TV may have played the corrupt clip there, which is
  then never opened again); G10's comparison with G02 admits the TV, whose playlist follows the room's games. G01 and
  every other golden 0 pixels apart.
- Scene digests rewritten on purpose (`RV_UPDATE_GOLDEN=1`): `digests.json`'s `scene` of every golden plan whose room
  has a sideboard (not plan-2000-seed-7 nor plan-8000), `plan-57-seed-102938.scene.json`, the building's gen4 floor
  scene and floors digest. Every plan golden and collection digest byte-identical.

## Results

- Debug unit tests (`./scripts/test.ps1`): 2152 of 2152 passed (the build clean, no warning).
- Release unit and GPU (`./scripts/test.ps1 -Preset msvc-x64-release-gpu`): 2301 tests (2152 unit, 149 GPU), all
  passed but one skipped by design (the overlay's pick, as always). The first Release run caught six hero-CRT cases
  counting one decoder beside the snes room's new TV (`world_start`, `world_inspect`, `world_launch`, the live floor
  case): fixed as "Decisions beyond ADR-0016" says, then passed.
- Goldens: G01 to G20 0 pixels apart in Debug and Release after the regenerations (largest difference 0/255).

## Screenshots

Hidden, frame-limited Debug runs (`build/i5/step3/`; `rv_smoke_tests "[.secondary_tv_shots]"` and `rv_tests
"[.secondary_tv_look]"` remake them):
- `console_room_hero_and_secondary.png`: the snes room from in front of the two screens, the hero CRT on its cabinet
  and the small ivory set on its walnut sideboard by the west wall, both playing (frame counters "O 0074" and
  "R 0074");
- `secondary_still_one_decoder.png`: the same view with `--max-decoders 1`: the hero plays, the secondary shows its
  game's cover;
- `grown_room_two_secondaries.png`: the room of 14 m the generator grew from 4,300 titles, drawn from its scene file
  (`--scene`: no collection, no games, so dim and on standby), its two TVs at `w11` and `e11` facing each other across
  the south end;
- `g19_both_frozen.png` (G19's run) and `walk_secondary_in_proximity.png` (the walk's last frame, the TV playing in
  view).

Beside style §15.1 and §15.3 and `example.png`: one model in two sizes, the secondary a lesser screen in the room's
warm light, never competing with the hero; its sideboard the room's walnut (or its pack's wood); the 90s and
optical-era rooms' sets grey and black (G12, G17).

## Real library

`./scripts/real-library.ps1 -Filter '[real-library][secondary_tv]'` (Release, ES-DE closed, the folders passed as
parameters), **0 differences**: the library imported in 15.9 s; the busiest floor `computer` (10 rooms, 5,314
items), 9 of its rooms with a secondary TV (`room:amiga`, 12 m and its walls all modules, has none); its largest
such room `room:amstradcpc` (1,646 items, its TV at `fx:e07`) started with both screens in view: the hero and the
secondary playing on slots 0 and 1, at most 2 decoders, **465.5 MiB** of GPU memory at its peak cold and warm
(acceptable 512; video 5.9 MiB; collection arrays 210.9, procedural surfaces 84.9, scene textures 106.0, targets
48.2), frame p50 6.1 ms in these lockstep runs. Without clips the same view holds 459.7 MiB.

Two findings on the way:
- **`--screenshot` adds its own render targets** at the screenshot's size (about 72 MiB at 1920 x 1080: 120.7 against
  48.2 MiB of targets): the first runs took a screenshot and read 538.0 MiB. The measuring runs take none (as Gate D's
  did); the screenshot is a run of its own. (Corrected by the review: these runs used the default 1280 x 720 window,
  and a 1920 x 1080 window carries a comparable target set of its own: see "Review", "Measurement".)
- **The TVs lower this floor's memory**: with the TVs left out of the deployed packs (a measurement by hand, not
  committed) the same view held 467.2 MiB without clips, against 459.7 with them: on the computer floor the TV takes a
  sideboard's socket whose variant was a showcase of boxes with covers of their own (scene textures 115.2 against
  106.0 MiB).

## Open issues

- A room whose decoration drew no sideboard has no TV; a secondary whose clip fails shows its still until its preview
  ends; the secondary is silent while the hero is in view (`BACKLOG.md`).
- A moved TV's bounds update only when the editor rebuilds its room's static part (the held fixture's frames keep its
  bounds of before): settled by the review, the secondaries playing nothing while the editor runs.

## Review

The step's review (2026-10-29) found bugs, tests that hid regressions, a measurement taken in the wrong window and a
placement that competed with the hero. ADR-0016 §5 is amended again ("I5 step 3's review").

**Bugs and robustness**
- **The reader threw** on a piece whose prefab `kind` is no string (`value("kind", ...)`): it now tests `find()` and
  `is_string()`, and readPiece() refuses the piece (a reader case with `"kind": 5`). It admits the TV only under a
  floor-standing decoration kind (`kFloorStandingDecorationKinds`: `tv_cabinet`, `floor_lamp`, `plant`): under a
  poster's kind `secondaryTvFits()`, which checks x and z alone, would have let it float.
- **`rebind()`** matches the bound screens to those found again by object id, whatever their order
  (`app::rebindOrder()`, unit-tested with a grown room's hero and two TVs reordered), and refuses another kind;
  `findScreens()` takes each screen's material from its object's own render items, never the first registry entry
  under its key's prefix (the registry is append-only: a rebuilt object with another picture leaves its old material
  there). When `rebind()` fails the app binds the room's screens again with the same games (`ScreenService::games()`)
  instead of only warning. This note's first version said "a moved TV is a new object with a new material": a moved
  TV keeps its object id and, its picture unchanged, its material (the registry keys a material by object, role and
  picture); the new editor case logs no material change. The report gains `video.rebinds` and each screen's `bounds`.
- **In view as well as within proximity**: the scheduler refuses an out-of-view `ambient` demand a free slot; an
  ambient screen already playing keeps its slot out of view for the usual 10 s (a scheduler case). After a launch's
  return a TV out of view therefore waits until it is in view to open again, where it stopped.
- **The editor**: while its session runs the secondaries ask for no clip (their slots stop, `unwanted`, as the
  cabinets' do), which also settles a held TV's stale bounds; they ask again after the session, on the objects
  `rebind()` gave them. Checked by hand in the editor, the CRT's upper half (above the planned sideboard's 0.72 m box)
  picked nothing: a fixture drawn as the secondary TV now has its pick box up to the set's top (0.9 m).
- **The hero search** tests the kind and the `.crt` id before binding, once (an arcade's cabinets are no longer
  bound to be refused).

**Tests that hid regressions**
- The live floor case (`test_world_floor_app.cpp`) writes `{"secondary": {"enabled": false}}` and keeps its
  one-thread guard (`<= 1`); G08a turns the secondary's autoplay off too (`maxDecoderThreads <= 1`, one decoder open),
  its metadata naming that step; its picture is unchanged.
- The world_launch Avvia case keeps the secondary off; a new case runs on the product's settings: the TV opens in view
  at the start, the walker turns onto Amber Lantern's item (the TV paused), inspects it and launches: both slots stop
  ("launch") before the hand-over with their threads ended; after the return, Indietro, back to free roam and a turn
  towards the TV, its clip opens again where it stopped (`fromSeconds`).
- G08b's metadata now names its whole command, the deletion of `videos.json` included (its PNG byte-identical). The
  runs are deterministic, so no "may": with the TV at `fx:w02` (before the review's placement) the first run (300
  frames from the doorway) opened the TV's playlist's first game, Lilac Loom, whose clip is the fixture's corrupt one,
  on frame 0 and gave it the `corrupt-data` verdict on frame 103, and without the deletion the golden run would never
  have opened the clip it shows failing. With the TV at `fx:e01` (its playlist seeded by its new object id) the first
  run plays two other games' clips and gives no verdict; the deletion stays, named, so that the golden never depends
  on what the TV plays.

**New proofs**
- GPU (`tests/smoke/test_secondary_tv.cpp`, now 4 cases and 1 hidden): on the product defaults (`proximityMeters`
  12) the TV behind the walker at the doorway opens nothing (one decoder) until a half turn at frame 90 opens it on
  the second slot; the editor case picks the TV by its CRT, nudges it a few grid steps and turns it 15 degrees, drops it
  and saves: one stop of its slot when the session starts, no event on it while the session runs, no bind (one
  rebind), no event on the hero's slot, its bounds where it now stands, `video` again after Salva.
- Unit: the scheduler's out-of-view ambient rule; `rebindOrder()`; the reader's refusals; the placement's rule on every
  golden plan with three packs and on the grown room.

**The look**
- **Placement** (style §15.3): the candidates are ranked by where their sockets stand at home, in integers from the
  plan and the room's seed: never the sideboard nearest the doorway's camera; the room's back half or the hero
  corner's reach (3 m) first, the farthest from the doorway's camera first among those, then the seed (between
  sockets mirrored across the room). In a room deeper than
  12 m the second TV stands in another third of the room's depth, else there is none. The room the generator grew to
  14 m from 4,300 titles has its only sideboards at `w11` and `e11`, flanking the doorway (a grown room's free
  side-wall sockets are its last growth steps'): it holds one TV, at `w11`, beside the doorway and out of its view,
  rather than the mirrored pair. Plans byte-identical.
- **The 90s collector pack's set** is `MAT_Plastic_Charcoal` (#4E5054), its own `plastic_body` (the hero keeps
  `MAT_Plastic_Gray`, which read ivory on the small set under the warm light).

**Goldens and digests** (rewritten on purpose, `RV_UPDATE_GOLDEN=1`, Debug, G01 untouched):
- images: G02, G06, G10 and G19 (the snes room's TV moves from the west wall's `w02` to the east corner by the hero
  wall), G12 (the jewel-case room's from mid west wall, `w03`, to `e02` by the hero wall, its set charcoal), G16 and
  G18 (the handheld room's TV elsewhere; G18 sees the room through its doorway);
- metadata only, their PNGs byte-identical: G02s, G07, G08a, G08b, G11, G13, G14, G15, G17, G20;
- scene digests (`digests.json`'s `scene` of the golden plans whose TV moved, `plan-57-seed-102938.scene.json`, the
  gen4 floor's scene and the floors' digests); every plan and collection digest byte-identical.

**Plan digests.** G02s, G07, G08a, G08b, G11, G13 to G16 and G20 recorded plan digests of main's generator before
1.5 (the plans their runs show come from generator 1.5) and warned on every run. Their metadata is refreshed in the
same rewrite: their PNGs stayed byte-identical but G16's, whose TV moved; nothing is left for Gate M4.

**Measurement.** `./scripts/real-library.ps1 -Filter '[real-library][secondary_tv]'` again (Release, ES-DE closed,
the folders passed as parameters, **0 differences** among 542,120 entries), every measuring run now in a 1920 x 1080
window (`--width 1920 --height 1080`): the busiest floor `computer` (10 rooms, 5,314 items, 9 rooms with a TV, the
largest `room:amstradcpc`, its TV at `fx:e07`), both screens playing on slots 0 and 1: **505.8 MiB** at its peak,
cold and warm, within the acceptable 512 MiB by 6.2 MiB (targets 88.5 MiB against 48.2 at 1280 x 720, so +40.3 MiB;
collection arrays 210.9, scene textures 106.0, procedural surfaces 84.9, video 5.9). Without clips 499.9 MiB; with
`--screenshot` at 1080p 578.3 (its own targets on top, 161.0 MiB in all). The step's first 465.5 MiB was the 720p
window's. Not over the limit, but close: Gate D's 471.8 MiB for the same floor was measured at 720p as well, and
holds about 512 MiB at 1080p (471.8 + 40.3); Gate M4 measures it again at 1080p with every screen playing
(`BACKLOG.md`). Should it pass 512, the proposed remedy is in the targets, the one part that grows with the window
(88.5 MiB at 1080p): itemise them and run the post-processing intermediates at reduced resolution or share buffers
between passes, before any cut of the collection arrays.

Frame times in these lockstep Release runs (wall clock per frame, the first 30 left out, 270 frames): with both
clips playing p50 6.06 ms, p99 16.01 ms cold and 18.47 ms warm; without clips p50 6.06 ms, p99 17.84 ms. The second
decoder costs no measurable frame time at the median; the p99 moves with the runs' cover uploads and the machine's
load rather than with the clips (the clips-off run's p99 lies between the two clips-on runs').

**Small fixes**: the `ImportedRoom` doc comments put back above their struct (`test_world_inspect.cpp`,
`test_world_launch.cpp`); `chosenSecondaryTvs()` once per room; `world::kSecondaryTvKind` in place of the app's copy;
`materials("secondary_tv")`'s precedence documented; the amendment says secondaries follow the world selection as
every screen does; this note's unit count corrected (6 cases and 1 hidden, not "6 and 2 hidden"); lines over 120
characters wrapped; BACKLOG's "second decoration kind" remedy names the reader's one-TV rule.

**Results.** Debug (`./scripts/build.ps1`, clean, no warning; `./scripts/test.ps1`): 2154 of 2154 unit tests passed.
Release (`./scripts/build.ps1 -Preset msvc-x64-release`; `./scripts/test.ps1 -Preset msvc-x64-release-gpu`): 2306
tests (2154 unit, 152 GPU), all passed but one skipped by design (the overlay's pick, as always); G02 to G20 0 pixels
apart from the goldens rewritten in Debug (largest difference 0/255), G01 untouched and matching.

**Screenshots** (retaken, `build/i5/step3/`): `console_room_hero_and_secondary.png` and
`secondary_still_one_decoder.png` (`rv_smoke_tests "[.secondary_tv_shots]"`: the snes room's TV now in the east
corner by the hero wall, a lesser screen beside the hero; with one decoder its game's cover), `g12_jewel_case_room.png`
(G12's run: the charcoal set at `e02` by the hero wall, smaller and dimmer than the hero from the doorway),
`g19_both_frozen.png`, `walk_secondary_in_proximity.png`, `turned_towards_secondary.png` (the half turn of the "behind"
case), `editor_tv_moved_saved.png` (the TV nudged, turned 15 degrees and saved, playing again) and
`grown_room_secondary.png` (`rv_tests "[.secondary_tv_look]"`, then `--scene`: the 14 m room from its doorway camera,
its one TV at `w11` beside the doorway, out of that view).
