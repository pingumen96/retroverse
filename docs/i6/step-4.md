# I6 step 4: side-face spines

Roadmap row: I6 step 4. Decision: [ADR-0014](../decisions/ADR-0014-the-styling-pass.md) §2 (amending ADR-0013 §4, §5
and ADR-0010 §10). The direction: [look.md](look.md) ("Shelves").

## What was built

- **`SlotFacing::SideOut`** (`rv/geometry/prefabs.hpp`): the item turned a quarter about +Y, its -X face out, upright;
  its slot is (d, h, w), its front towards the slot's +X. The cartridge prefab's `spineFace: "side"` lays the spine
  label upright on the -X face (the Spine socket's +X along the item's +Z, +Y up, the shell 0.5 mm narrower on that
  side, no top label); since the review it covers that face's flat part top to bottom (below).
- **Five successor families** (`assets/world/media_families.json`, new ids, ADR-0013 §4): `disc_case_cd_v2`,
  `disc_case_dvd_v2`, `case_handheld_v2`, `big_box_v2`, `generic_case_v2`, each with `"succeeds"`, `"spineFacing":
  "side"`, an item manifest with the side spine (`media.case.*_side`, `media.box.big_side`) and hero and side modules
  as deep as the item is wide (`furniture.shelf.{hero,side}_module.*_side`: 15 to 22 cm, the most levels that hold the
  item upright with its 2 cm headroom, the band on the level nearest 1.3 m); face-out nothing changed, so they keep
  their predecessors' display cases and wall shelves. `systems.json`, the fallback and `kSharedRoomFamily` name them;
  a new room asked for a succeeded family takes its last successor in rooms of its kind (a platform file naming an old
  id included). The registry checks the successions (no self, no unknown, one successor per kind of room, no cycle)
  and the spine-out slot against the facing; `checkItemManifest()` checks the spine label's face and size.
- **The one-time migration** (`rv/world/incremental.hpp`): a stored room whose recorded family has a successor in
  rooms of its kind, given
  every section's selection, kept by no rule and named by no pin, is regenerated with its successor's data
  (`WorldData::successor`, which `loadRoomData()` reads): its id, kind, floor, x, yaw, sections and seed kept, its
  face-out titles chosen again by priority, its z following its new depth either way. `ChangeReport::migratedRooms`
  ({room, from, to, unplacedForCapacity}), the rebuild level `full`; the CLI prints "Room migrated" (JSON
  `changes.migratedRooms`); the app logs each one, records them in the run report (`world.migratedRooms`) and shows the
  notice `rooms-migrated` once ("Alcune stanze sono state riordinate"). A pinned room is updated as it stands, the
  refusal in `needsConfirmation` (logged, and in the run report's `world.needsConfirmation` since the review). Each
  room migrates at most once per succession: its new family has none in rooms of its kind.
- **Generator 1.4**, a minor version (`generation.hpp`): a major one would hold every room of every stored plan until
  an explicit regeneration (ADR-0010 §10); the migration is ADR-0014 §2's one announced exception, scoped to the rooms
  of a succeeded family, and an update by 1.4 of a plan it wrote moves nothing.
- **No spine label on face-out items**: `instance_flags::kNoSpineLabel` (`RV_INSTANCE_NO_SPINE_LABEL`) in the record of
  every face-out item (`CollectionItem::attributes()`); `materialInstanceTest()` discards a spine-layer material's
  fragments for it in the lit, depth, shadow, outline and inspect programs. Spine materials are alpha-masked with a
  cutoff of 0 (never cut by alpha) so that the depth passes, which bind no material for opaque batches, run the test.
- **Focus and pull-out** need no new code: the pull slides the case out along its slot (+Z) and turns it a quarter
  about +Y, its cover (the slot's +X) to the viewer, upright all the way; hover and pick boxes are the slot's.

## The review (2026-10-24)

The reviewers' mock-ups (`build/i6/review4`: the spine stretched to full height; then dark shells too) and the
orchestrator's decisions:

- **The arcade migrates once** (ADR-0014 §2, amended on 2026-10-24). A succession names the kinds of room it applies
  in (`"succeedsIn"`, every kind by default): `generic_case_v2` succeeds `generic_case` in console and shared rooms
  only. The arcade room, the arcade systems' rows of `systems.json` and `kArcadeRoomFamily` keep `generic_case` until
  step 6's `arcade_cabinet` succeeds it there (its own migration, once). `successorOf()` and `latestSuccessor()` take
  the kind of room; the registry refuses two successors of a family in one kind and any cycle (each kind's chains
  walked from every family).
- **Printed spines.** The side spine label covers the face's flat part: jewel case 7.1 x 123.4 mm, keep case 9.7 x
  186.9, handheld case 9.7 x 137, generic case 10.7 x 177, big box 40.7 x 236.9 (the keep case's and the big box's 0.1
  mm short of the flat part, which the prefab's float limit refuses exactly). The family names it
  (`spineLabelWidthMm`, `spineLabelHeightMm`, to a tenth of a millimetre, checked against the manifest's Spine socket);
  `MediaFamily::spineAspect()` goes through `CoverItem::spine` into `SpineRequest::spine`. `drawSpine()` lays the title
  out on a canvas of that shape (`labelCanvas()` of the 40 x 256 layer: 40 x 696 for the jewel case; the sizes shares
  of its thickness, the line as long as its length less the band), draws a darker band of the spine's colour across
  its top 12% (45% towards black), and resamples it once into the unchanged layer, which the quad stretches back. Its
  cache key gains `spine=<shape>;style=1` only for such a shape (`kPrintedSpineStyleVersion`), so the cartridges' keys
  and `kCoverPipelineVersion` stay.
- **Dark shells.** `"shellMaterial"` per family, read by `CollectionInstance::create` (the pending picture follows the
  shell): `MAT_Case_Jewel_Smoke` (smoky dark, glossy), `MAT_Case_Keep_Black`, `MAT_Case_Plastic_Dark` (handheld and
  generic cases), `MAT_Cardboard_BigBox`; original presets in `materials.json` (its registry entry notes them). The
  cartridges and the predecessors keep the theme's `cartridge` role. The big box's shell tinted from its spine is in
  the BACKLOG.
- **The hero-wall modules in turns** (generator 1.4's rule for a family standing on its side, ADR-0014 §2 amending
  ADR-0010 §5; `allocation.hpp`'s `spineFillRounds()`, which the update's fill order shares): a round is the next
  level in band order of each hero-wall module, the titles a round cannot all fill shared out among its levels one at
  a time; the side-wall modules follow one after the other. The case-shelves psx room's 112 spine-out cases now stand
  56 and 56 (48 at the eye on each module, 8 above) rather than filling one module's levels; the cartridges' rooms and
  their goldens do not move.
- **Tests.** `tests/fixtures/world/legacy/building-esde-seed-102938-generator-1.3.json` is main's generator-1.3 golden
  building: its unit update migrates `room:psx` and the two shared rooms, every other element of the plan byte for
  byte but the generator's stamps and the input digest, then writes nothing; the CLI test checks "Room migrated" and
  `changes.migratedRooms`; the run report's round trip `world.migratedRooms` and `world.needsConfirmation`; the GPU
  case `gpu.world families: a stored building of generator 1.3 is migrated once` puts it in a data directory of the
  building fixture and starts twice (the notice and the three rooms, then neither). A migration past the new capacity
  (amiga's 1,700 big boxes: 139 unplaced for capacity, counted in the report and the log) and one that shrinks the
  room (psx's 5,600 jewel cases: fewer growth steps, z back towards the hall) are unit cases; the fill rule has one,
  the cycles and kinds of the registry, the printed spines' layout, band and contrast after BC1 theirs. The
  `case-shelves` fixture gains 100 keep cases (`ps2`).
- **Not done:** face-out items still get a spine layer made and uploaded (the residency counts two layers per item;
  BACKLOG); a missing `*_side` item manifest still falls back to `media.case.plain.generic` at run time (a manifest's
  `fallbackPrefab` must name its prefab family's generic; BACKLOG).

## Capacity per family

`test_world_families.cpp` (ADR-0010 §5's way, the reserve kept):

| Family | Side module | Hero module (band) | D / B | 2 Sh / S | 9 x 10 m | 16 m | predecessor's 9 x 10 m / 16 m |
|---|---|---|---|---|--:|--:|---|
| disc_case_cd_v2 | 8 x 56 | 8 (5) | 38 / 10 | 784 / 448 | 6,177 | 10,852 | 5,398 / 9,488 |
| disc_case_dvd_v2 | 5 x 44 | 5 (3) | 38 / 10 | 352 / 220 | 3,026 | 5,321 | 4,097 / 7,311 |
| case_handheld_v2 | 7 x 44 | 7 (4) | 44 / 12 | 528 / 308 | 4,257 | 7,471 | 4,793 / 8,466 |
| big_box_v2 | 4 x 16 | 4 (2) | 28 / 8 | 96 / 64 | 893 | 1,561 | 1,088 / 1,923 |
| generic_case_v2 | 6 x 41 | 6 (4) | 38 / 10 | 410 / 246 | 3,393 | 5,960 | 4,391 / 7,813 |

Standing upright, a case is as tall as face-out: the CD gains a level, the taller DVD and the boxes lose some. The
reference library's largest rooms of these families still fit (ps2's 2,134 titles in 3,026; amiga's 1,055 grow the
room within 1,561): see the real library below.

## Proof

- **Unit** (`rv_tests`): the facing and the side spine (`test_collection_prefabs.cpp`), the families, their successions,
  items and fixtures facing them exactly, a room of each generating and validating, the capacity table
  (`test_world_families.cpp`), the migration (`test_world_migration.cpp`: an old building of psx, amiga, the gen6 shared
  room and the arcade room beside snes and gb, generated with the predecessors' data; three migrated, snes's, gb's and
  the arcade room byte for byte, every title placed, the psx face-out set the first 48 by priority, a second update
  writing nothing; a partial update migrating only the rooms given; a pinned room refused; a new room taking the
  successor; since the review the legacy fixture, the capacity and shrink cases), the flag in every face-out record and
  its GLSL mirror, the depth program's test, the pull-out of cases and boxes on their side (upright all the way). The v1
  lift oracles still hold (cartridges only). The SNES plan goldens changed in their generator version and input digest
  alone; the building goldens and the floors' scene digests are rewritten on purpose (again at the review: the arcade
  room back to `generic_case`, as on `main` but for its generator stamp).
- **GPU** (`gpu.world families: jewel cases stand on their side`, the new `esde/case-shelves` fixture): 160 jewel cases
  (38 featured, 10 in the band, 112 spine-out) and 120 big boxes (28, 8, 84); from 0.6 m every one of the 59 spines in
  view upright, a picture on each. Since the review also `gpu.world families: a stored building of generator 1.3 is
  migrated once` (above).
- **The real library at the review** (`./scripts/real-library.ps1 -Filter '[real-library][building]'`, ES-DE closed,
  `RV_LEGACY_CLI` as below, three runs, each with 0 differences among 542,120 entries): the migration case passed every
  time: **24 rooms migrated** (the arcade room no longer among them), the other **20 byte for byte** in their
  elements, **no title over capacity**, the update 3.7 s on a quiet machine, the notice, then a second start and the
  CLI's update writing nothing. The counts per room are the ones below (amiga still grown one step, z -6.65 to -8.65
  m). The building case's timing rows hold on a quiet machine but not under the other worktrees' test runs, which
  shared it all night: run 2 (another worktree's unit tests starting midway) met the warm ride (its view back 1.67 s,
  the room whole at 1.42 s) and missed the third start's lobby (6.75 s for 5 s); run 3 met the lobbies (2.65 and 2.81
  s) and missed the warm ride by 66 ms, its floors built in 2.3 s and 2.0 s against 0.66 s and 0.47 s in run 2 (the
  floor's build reads no spine). The busiest floor (computer) peaks at 420.3 MiB, within 512.
- **The real library before the review**: the building planned twice, one digest and one file SHA-256 (19,985
  placements); planned by generator 1.3 (`RV_LEGACY_CLI`, the CLI built from I6 step 2's commit) then started in this
  build: **25 rooms migrated** in one update of 4.7 s, the other **19 byte for byte** in their elements, **no title over
  capacity**, the notice shown, a second start and the CLI's update writing nothing. The large ones: psx 1,400 titles
  (48 face-out), ps2 2,134, gc 450, amiga 1,055 (the room grown one step, its z -6.65 to -8.65 m: big boxes take 4
  levels of 16 rather than 5), amstradcpc 1,646, atari800 1,207, apple2 494, c64 408, atarist 295, switch 229; the seven
  shared rooms and the arcade room unchanged in their counts. The busiest floor (computer) peaks at 421.0 MiB (407.3 at
  Gate C), within 512; NFR-01 to the lobby 2.3 s; the warm ride's view back 1.81 s (a first run under another worktree's
  GPU tests timed out at 2.5 s with 596 covers pending; alone it arrives).
- **Suites at the review**: 1,892 unit tests in Debug and in Release; the 108 GPU cases in Release, G12 then
  regenerated on purpose (the jewel cases' dark shells; matching again in Release and Debug, G13 within the tolerance
  in both); the benchmark path (Release, `--no-vsync`, 1,230 frames, a quiet machine) p50 3.32 ms, p95 3.80 ms, p99
  6.88 ms, within the acceptable 16.7 and 33.3 ms (34.2 ms at p99 while another worktree's GPU tests shared the GPU).
- **Suites before the review**: 1,887 unit tests in Debug and in Release; the 102 GPU cases in Release (the eight
  goldens regenerated on purpose, then all eleven room goldens matching in Debug and Release); the benchmark path p50
  3.31 ms, p99 4.55 ms.

## Screenshots and judgement

`build/i6/step4` (the review's; the first ones in `build/i6/step4-before-review`), the `case-shelves` fixture:
`psx_doorway.png`, `psx_module_2m.png`, `psx_module_1m5.png`, `psx_module_1m.png` and `psx_spines_0m6.png` (160 jewel
cases), `ps2_doorway.png` and `ps2_module_1m5.png` (100 keep cases), `amiga_doorway.png` and `amiga_module_1m5.png`
(120 big boxes); `g12_psx_doorway.png` (the building's 40 jewel cases, all face-out, G12).

Against `example.png` (panels 2 and 4): the shelves now read as racks of coloured spines, each case a strip of its
cover's colour top to bottom with its title along it and a darker cap, the shells dark between them, as on the
concept's CD and DVD shelves; the big boxes' spines are printed boxes with large titles, the strongest of the three;
face-out, the jewel and keep cases frame their covers in black, which makes the covers read as pictures (G12's display
cases). Both hero-wall modules now hold spines at the eye in a room of a few dozen to a hundred cases. Still plainer
than the concept: from the doorway the modules are small in the frame and their upper and lower levels are dressing
(storage boxes, magazines) in a room of 160 titles; the spines' text is legible from about a metre (it is squeezed
2.7 times along a jewel case's length in the 40 x 256 layer); no spine carries a logo or a strip of colour of its
own beyond the band.

## Deviations and follow-ups

- **The display fixtures are shared** with the predecessors (face-out nothing changed): ten manifests, not twenty.
- **Generator 1.4 is a minor version** although the migration places games differently (ADR-0010 §10's test): it is
  ADR-0014 §2's announced exception, and a major one would freeze every stored room until a regeneration.
- **Spine materials are alpha-masked** (cutoff 0) so that the depth passes run the face-out test; pixel for pixel the
  lit result is the same.
- A room kept by another rule (an older template) or named by a pin is not migrated (BACKLOG). The spine layer stays
  40 x 256 (no new pipeline version): a printed spine is resampled into it, its glyphs squeezed along the length
  (BACKLOG: a taller spine layer).
- **The arcade room keeps `generic_case`** (ADR-0014 §2, amended): its cases still stand on their end, pale, until
  step 6's cabinets; its golden content is `main`'s.
- The real-library case needs the old generator's CLI (`RV_LEGACY_CLI`); without it it is skipped. Made with `git
  worktree add --detach build/legacy-1.3 7fae18c`, then `./scripts/build.ps1 -Preset msvc-x64-release -Target
  retroverse-cli` in it (`build/legacy-1.3/build/msvc-x64-release/bin/retroverse-cli.exe`).
