# I6 step 4: side-face spines

Roadmap row: I6 step 4. Decision: [ADR-0014](../decisions/ADR-0014-the-styling-pass.md) §2 (amending ADR-0013 §4, §5
and ADR-0010 §10). The direction: [look.md](look.md) ("Shelves").

## What was built

- **`SlotFacing::SideOut`** (`rv/geometry/prefabs.hpp`): the item turned a quarter about +Y, its -X face out, upright;
  its slot is (d, h, w), its front towards the slot's +X. The cartridge prefab's `spineFace: "side"` lays the 40 x 256
  spine label upright on the -X face (the Spine socket's +X along the item's +Z, +Y up, the shell 0.5 mm narrower on
  that side, no top label).
- **Five successor families** (`assets/world/media_families.json`, new ids, ADR-0013 §4): `disc_case_cd_v2`,
  `disc_case_dvd_v2`, `case_handheld_v2`, `big_box_v2`, `generic_case_v2`, each with `"succeeds"`, `"spineFacing":
  "side"`, an item manifest with the side spine (`media.case.*_side`, `media.box.big_side`) and hero and side modules
  as deep as the item is wide (`furniture.shelf.{hero,side}_module.*_side`: 15 to 22 cm, the most levels that hold the
  item upright with its 2 cm headroom, the band on the level nearest 1.3 m); face-out nothing changed, so they keep
  their predecessors' display cases and wall shelves. `systems.json`, the fallback and `kSharedRoomFamily` name them;
  a new room asked for a succeeded family takes its last successor (a platform file naming an old id included). The
  registry checks the successions (no self, no unknown, one successor each) and the spine-out slot against the facing;
  `checkItemManifest()` checks the spine label's face.
- **The one-time migration** (`rv/world/incremental.hpp`): a stored room whose recorded family has a successor, given
  every section's selection, kept by no rule and named by no pin, is regenerated with its successor's data
  (`WorldData::successor`, which `loadRoomData()` reads): its id, kind, floor, x, yaw, sections and seed kept, its
  face-out titles chosen again by priority, its z following its new depth. `ChangeReport::migratedRooms` ({room, from,
  to}), the rebuild level `full`; the CLI prints "Room migrated" (JSON `changes.migratedRooms`); the app logs each one,
  records them in the run report (`world.migratedRooms`) and shows the notice `rooms-migrated` once ("Alcune stanze
  sono state riordinate"). A pinned room is updated as it stands, the refusal in `needsConfirmation`.
- **Generator 1.4**, a minor version (`generation.hpp`): a major one would hold every room of every stored plan until
  an explicit regeneration (ADR-0010 §10); the migration is ADR-0014 §2's one announced exception, scoped to the rooms
  of a succeeded family, and an update by 1.4 of a plan it wrote moves nothing.
- **No spine label on face-out items**: `instance_flags::kNoSpineLabel` (`RV_INSTANCE_NO_SPINE_LABEL`) in the record of
  every face-out item (`CollectionItem::attributes()`); `materialInstanceTest()` discards a spine-layer material's
  fragments for it in the lit, depth, shadow, outline and inspect programs. Spine materials are alpha-masked with a
  cutoff of 0 (never cut by alpha) so that the depth passes, which bind no material for opaque batches, run the test.
- **Focus and pull-out** need no new code: the pull slides the case out along its slot (+Z) and turns it a quarter
  about +Y, its cover (the slot's +X) to the viewer, upright all the way; hover and pick boxes are the slot's.

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

- **Unit** (`rv_tests`): the facing and the side spine (`test_collection_prefabs.cpp`), the families, their
  successions, items and fixtures facing them exactly, a room of each generating and validating, the capacity table
  (`test_world_families.cpp`), the migration (`test_world_migration.cpp`: an old building of psx, amiga, the gen6
  shared room and the arcade room beside snes and gb, generated with the predecessors' data; the four migrated, snes's
  and gb's rooms byte for byte, every title placed, the psx face-out set the first 48 by priority, a second update
  writing nothing; a partial update migrating only the rooms given; a pinned room refused; a new room taking the
  successor), the flag in every face-out record and its GLSL mirror, the depth program's test, the pull-out of cases
  and boxes on their side (upright all the way). The v1 lift oracles still hold (cartridges only). The SNES plan
  goldens changed in their generator version and input digest alone; the building goldens and the floors' scene
  digests are rewritten on purpose.
- **GPU** (`gpu.world families: jewel cases stand on their side`, the new `esde/case-shelves` fixture): 160 jewel cases
  (38 featured, 10 in the band, 112 spine-out) and 120 big boxes (28, 8, 84); from 0.6 m every one of the 59 spines in
  view upright, a picture on each.
- **The real library** (`./scripts/real-library.ps1 -Filter '[real-library][building]'`, ES-DE closed, 0 differences
  among 542,120 entries): the building planned twice, one digest and one file SHA-256 (19,985 placements); planned by
  generator 1.3 (`RV_LEGACY_CLI`, the CLI built from I6 step 2's commit) then started in this build: **25 rooms
  migrated** in one update of 4.7 s, the other **19 byte for byte** in their elements, **no title over capacity**,
  the notice shown, a second start and the CLI's update writing nothing. The large ones: psx 1,400 titles (48 face-out),
  ps2 2,134, gc 450, amiga 1,055 (the room grown one step, its z -6.65 to -8.65 m: big boxes take 4 levels of 16 rather
  than 5), amstradcpc 1,646, atari800 1,207, apple2 494, c64 408, atarist 295, switch 229; the seven shared rooms and the
  arcade room unchanged in their counts. The busiest floor (computer) peaks at 421.0 MiB (407.3 at Gate C), within
  512.

## Screenshots and judgement

`build/i6/step4`: `psx_doorway.png` and `psx_close_to_shelf.png` (160 jewel cases), `amiga_big_boxes_doorway.png`
(120 big boxes), `g12_psx_doorway.png` (the building's 40, all face-out), `g13_shared_handheld_doorway.png`.

Against `example.png` (panels 2 and 4): the cases now stand as on the concept's shelves, upright in rows, each spine a
title strip, the boxes' tall spines filling the hero modules, the cover of a hovered case sliding out like a book. Still
plainer: the spines are the shell's pale plastic with a short coloured label (the concept's spines are coloured end
to end), and from the doorway the modules read small and are mostly dressing in a room of a few dozen titles.

## Deviations and follow-ups

- **The display fixtures are shared** with the predecessors (face-out nothing changed): ten manifests, not twenty.
- **Generator 1.4 is a minor version** although the migration places games differently (ADR-0010 §10's test): it is
  ADR-0014 §2's announced exception, and a major one would freeze every stored room until a regeneration.
- **Spine materials are alpha-masked** (cutoff 0) so that the depth passes run the face-out test; pixel for pixel the
  lit result is the same.
- A room kept by another rule (an older template) or named by a pin is not migrated; the spine label keeps the
  texture's 40 x 256 aspect (7 x 45 mm on a jewel case's 125 mm spine). Both in the BACKLOG.
- The real-library case needs the old generator's CLI (`RV_LEGACY_CLI`); without it it is skipped. Made with `git
  worktree add --detach build/legacy-1.3 7fae18c`, then `./scripts/build.ps1 -Preset msvc-x64-release -Target
  retroverse-cli` in it (`build/legacy-1.3/build/msvc-x64-release/bin/retroverse-cli.exe`).
