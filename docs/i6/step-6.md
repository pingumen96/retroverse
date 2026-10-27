# I6 step 6: arcade and handhelds

Roadmap row: I6 step 6. Decision: [ADR-0014](../decisions/ADR-0014-the-styling-pass.md) §2 (the arcade room's
succession) and §6 (amended on 2026-10-27: the cabinets as a family facing front, their capacity, the hero cabinet,
the handhelds' counters as a look). The direction: [look.md](look.md) (the arcade's and the handhelds' rows, concept
panels 5 and 6).

## What was built

- **The `arcade_cabinet` prefab** (`rv/geometry/prefabs.hpp`, `prefab_arcade_cabinet.cpp`): a row of 1 to 4 generic
  upright cabinets of original design: side panels with the stepped profile (the lower body, the control panel
  standing out, the screen leaning back in a dark bezel, the marquee's hood) edged by a lit T-moulding, a recessed toe,
  a coin door, a joystick and three buttons, a lit marquee under a cap; no brand, logo or artwork. Cabinets alternate
  two paints, mouldings and marquees (the `_alt` roles). With `slots` each screen is a face-out slot `Slot_0_<i>` (the
  collection draws the game's screen there) behind which a dim idle screen asks for a coin; without, the screen is a
  `ScreenSurface` (the hero cabinet). Its manifest `furniture.arcade_cabinet.generic` is a pair 0.88 x 1.7 x 0.4 m
  (0.42 m cabinets 4 cm apart, 0.30 x 0.40 m portrait screens): a pair stands in any of the template's 0.9 x 0.4 m
  sockets, so the template does not change. Every pack dresses the kind (the arcade's plum and oxblood paints,
  magenta and cyan mouldings and marquees; the others' walnut and amber); a marquee is the new `marquee` pattern (a
  generic arcade word, PLAY, 1 UP, HI SCORE ..., in dark lettering on a backlit panel, sparkles by the fixture's
  seed) times its colour: never a title (the game on a screen changes, its marquee stays) or a trade name.
- **The family `arcade_cabinet`** (`assets/world/media_families.json`, `rv/world/media_family.hpp`): the arcade room's
  (`kArcadeRoomFamily`, the arcade systems' rows of `systems.json`), succeeding `generic_case` in the arcade room only.
  Its item is the cabinet's screen (`media.screen.arcade.generic`, a thin black shell whose 298 x 398 mm label is the
  cover, no spine label). New in the family format: `"spineFacing": "front"` (its spine-out slot is its face-out one;
  `SlotLevelInfo::facing(family, role)` stands a hero-wall or side-wall module's levels as the spine section's, the
  display fixtures' as the featured titles', no hero band), fixtures of kind `arcade_cabinet` under any key, and
  `"labelMaterial"` (`MAT_Screen_Arcade`: the collection's front layer through the CRT's picture tube, unlit; the unlit
  shader now samples a layered base colour as the lit one does). Its rooms take no shelf dressing
  (`withFamilyFixtures()` empties the list). The collection draws every item of the family face-out
  (`ItemStance::FaceOut`: no spine label, the face-out focus tilt); the cabinets' occluders are a shelf's around the
  box their screens span.
- **The migration** is step 4's rule, unchanged: a stored arcade room of `generic_case` is regenerated once with the
  cabinets, keeping its id, floor, x, sections and seed, its featured titles chosen again by priority, reported
  (`migratedRooms`, "Room migrated", the notice). On generator 1.3's building it is migrated and re-placed onto the hub
  in the same update.
- **The hero cabinet** (`assets/world/themes/arcade_after_dark.json`, ADR-0014 §1's furniture): the hero corner's
  `crt` drawn as a deluxe cabinet (0.7 x 1.85 x 0.85 m, a 4:3 screen of 0.50 x 0.375 m leaning 12 degrees) standing on
  the floor where the TV's cabinet stood; the `tv-cabinet` part left out. The furniture reader now takes a hero
  piece's `yMm`, an empty list (the part not drawn; never the `crt`'s), and the `crt` only as `arcade_cabinet` pieces
  (the first must hold a ScreenSurface, else the CRT is drawn as planned); both apply in the arcade room only (a
  console room drawn with the pack keeps its TV, cabinet and console). The screen service finds the hero cabinet's
  screen as the CRT's (`findHeroScreen()`); the room's video, its fallbacks and its run report are as before. Two red
  bar stools stand by its side, clear of its player's place (the validation's interaction point and passages as
  before), the dark red runner leading to it.
- **The handhelds.** The handheld families' display cases (`furniture.display_case.handheld_cart.generic`,
  `.handheld_case.generic`) are drawn as low counter showcases (`DisplayCaseParams::counter`: the base a glass front
  over five generic handhelds on stands, ivory and teal in turn) and the display case's vitrine lit by a strip under
  its rails where the pack names `board_light` (the handheld archive): neither moves a slot nor the bounds, so no plan
  changes and nothing migrates (the plan's prefab catalogue measures bounds and slots only). Stations (a handheld in a
  small lit vitrine on a 0.85 m pedestal, the showcase prefab) stand in most decoration plants' place (the handheld
  archive's variants of `plant`, weights 0.6 and 0.4).

## Capacity

`test_world_families.cpp`'s table, the reserve kept (ADR-0010 §5):

| Family | D | B | 2 Sh | S | 9 x 10 m | 16 m |
|---|--:|--:|--:|--:|--:|--:|
| arcade_cabinet | 6 | 0 | 4 | 2 | 33 | 53 |
| generic_case (the arcade room's until now) | 38 | 10 | 410 | 328 | 4,391 | 7,813 |

The real library's arcade room places 36 titles: every one in a cabinet, the room grown one step (40 at 12 m); the
unit test's room of 38 titles, a little above it, fits the same way. A larger arcade places the rest as unplaced for
capacity, launchable from the quick catalog (ADR-0014 §6 amended; BACKLOG): the review's test of 60 titles places the
53 first by priority and reports the 7 last.

## Proof

- **Unit** (`rv_tests`): the prefab (`test_prefabs.cpp`: its roles, bounds, sockets, slots at each cabinet's screen
  leaning back, the hero cabinet's ScreenSurface, the refusals; the counter showcase's slots and bounds those of the
  display case), the `marquee` pattern, the family (`test_world_families.cpp`: its succession in the arcade room only,
  `facing(family, role)`, no dressing, the capacity row, a room of 38 titles: all placed, the first six by priority
  featured, grown a step), the migration (`test_world_migration.cpp`: the arcade room migrated once beside psx, amiga
  and the gen6 shared room, the cartridges' rooms byte for byte, a second update writing nothing; generator 1.3's
  golden building: the arcade room migrated and re-placed in one update, everything else as it stood), the hero
  substitution (`test_era_furniture.cpp`: the reader's empty list, `yMm` and `crt` rules; the golden plans drawn with
  every pack, the arcade room's hero cabinet inside the hero corner with the same soft findings, a console room drawn
  with the arcade pack keeping its TV), the CLI's report of the migration, the shared rooms. Plan goldens: the two
  building plans changed in the arcade room's elements and the input digest alone (checked element by element); the
  SNES plan goldens, the v1 and legacy oracles untouched; the floors' scene digests rewritten for the ground and
  handheld floors (gen4, gen5, computer unchanged). Debug: 1,910 tests, all passed (in four shards).
- **GPU** (Release): G15 (`g15_arcade_room_golden.png`: the arcade room from its doorway, its video bound to the hero
  cabinet `area:arcade/fx:hero.crt` and held at 1.0 s) and G16 (`g16_handheld_room_golden.png`: the gb room) added,
  G13 regenerated on purpose (its lit cases and stations), made in Debug and 0 pixels over the tolerance in Release;
  G01-G12 and G14 unchanged (G11 and G14 show the arcade's portal: within the tolerance). `gpu.world families: the
  arcade room's cabinets ...`: every item a screen facing out, six featured, no dressing, a screen up close lit (mean
  luma 146), a flyer's card at the screen's 298 x 398 shape; `... a game launched from its cabinet's screen ...`:
  inspect, Avvia into the fake emulator (RV_LAUNCH_ALLOWLIST), returned, the pose restored; generator 1.3's stored
  building migrated once with the arcade room. `./scripts/test.ps1 -Preset msvc-x64-release-gpu`: 2,053 tests (1,936
  unit, 117 GPU), all passed but the overlay's pick (skipped as always) and the floors' digests, rewritten after the
  marquee change and then passing.
- **Benchmark path** (Release, 1920 x 1080, `--no-vsync`, 1,230 frames): p50 3.30 ms, p95 3.55 ms, p99 3.77 ms, GPU
  memory peak 265.5 MiB (step 5: 3.32, 3.77, 265.5). The walk from the hub into the arcade room
  (`hub_walk_into_arcade.json`, 870 frames): p50 3.20 ms, p99 3.75 ms, one 186 ms frame (the hero cabinet's first
  video bind, 262 ms in step 5), 237.6 MiB at its peak (252 before).
- **The real library** (`./scripts/real-library.ps1 -Filter '[real-library][building]'`, ES-DE closed,
  `RV_LEGACY_CLI` of generator 1.3; 0 differences among the snapshots): all passed. The arcade room's 36 placed titles
  (36 before) in cabinets, 6 featured and 30 in the modules, the room grown one step (on the hub at x 9000, z -14150:
  its z follows its depth); generator 1.3's building updated once in 3.2 s: 25 rooms migrated (the arcade room
  among them, re-placed onto the hub in the same update), the other 19 byte for byte, none over capacity; planned twice
  to one digest and SHA-256 (19,985 placements); the hub shown 2.60 s and 2.84 s after the process started (NFR-01:
  5 s); the busiest floor (computer) 471.8 MiB cold and warm, unchanged (512 acceptable), the ground floor 212.2 MiB;
  the warm ride's view back in 1.50 s (3 s), the cold one timed out at 2.5 s with its covers pending as before (the
  room whole at 13.2 s, 30 s).

## Screenshots and judgement

`build/i6/step6` (Release, 1920 x 1080, the building fixture; `shots.sh` in `build/i6/step6-work`; `compare_*.png`
beside concept panels 5 and 6): the arcade from its doorway (`arcade_doorway`, G15's view), from its middle
(`arcade_inside`), the hero cabinet (`arcade_hero`), a west row (`arcade_west_row`), the aisle's pairs
(`arcade_aisle`), looking back (`arcade_back`); the gb room (`handheld_gb`, G16's view), a counter up close
(`handheld_gb_case`), stations (`handheld_gb_station`), the shared room (`handheld_shared`, G13's), its counter
(`handheld_shared_case`). Measured at 768 x 432 like the concept (the panels without their captions):

| image | median luma | calm / colour / accent % | luma < 12 % | hue % warm / red / magenta / blue / cyan / green / grey |
|---|---|---|---|---|
| concept panel 5 | 27.4 | 57.1 / 30.7 / 12.1 | 27.1 | 14 / 15 / 30 / 11 / 1 / 1 / 28 |
| concept panel 6 | 54.8 | 85.6 / 8.8 / 5.6 | 6.9 | 58 / 5 / 6 / 7 / 0 / 4 / 20 |
| arcade_doorway | 25.0 | 85.8 / 12.5 / 1.7 | 10.8 | 18 / 19 / 17 / 1 / 2 / 0 / 43 |
| arcade_inside | 27.0 | 75.3 / 21.4 / 3.3 | 8.6 | 20 / 10 / 25 / 0 / 1 / 0 / 43 |
| arcade_hero | 61.1 | 37.5 / 51.0 / 11.5 | 4.3 | 10 / 23 / 49 / 1 / 2 / 0 / 15 |
| arcade_west_row | 18.0 | 87.0 / 8.1 / 4.9 | 11.7 | 10 / 2 / 15 / 1 / 3 / 0 / 68 |
| arcade_aisle | 19.3 | 83.9 / 9.3 / 6.8 | 5.9 | 18 / 1 / 10 / 1 / 6 / 0 / 64 |
| arcade_back | 17.7 | 95.0 / 4.5 / 0.4 | 10.3 | 23 / 12 / 7 / 1 / 0 / 0 / 57 |
| g15_arcade | 25.0 | 85.8 / 12.5 / 1.7 | 10.8 | 18 / 19 / 17 / 1 / 2 / 0 / 43 |
| handheld_gb | 45.3 | 99.0 / 0.4 / 0.6 | 0.9 | 87 / 0 / 0 / 0 / 0 / 4 / 9 |
| handheld_shared | 42.7 | 98.2 / 0.6 / 1.1 | 0.9 | 87 / 0 / 0 / 0 / 0 / 2 / 10 |
| handheld_gb_case | 45.6 | 98.3 / 0.4 / 1.2 | 3.6 | 81 / 0 / 0 / 0 / 0 / 13 / 6 |
| handheld_gb_station | 24.7 | 100.0 / 0.0 / 0.0 | 14.9 | 68 / 0 / 0 / 0 / 0 / 0 / 32 |
| g16_handheld | 45.3 | 99.0 / 0.4 / 0.6 | 0.9 | 87 / 0 / 0 / 0 / 0 / 4 / 9 |
| g13_shared | 42.7 | 98.2 / 0.6 / 1.1 | 0.9 | 87 / 0 / 0 / 0 / 0 / 2 / 10 |

Judgement beside panel 5: the arcade now reads as one: rows of upright cabinets along both walls and a pair either
side of the aisle facing it, lit marquees and magenta and cyan mouldings, each game's flyer glowing on its screen
through a picture tube, the hero cabinet playing the room's clip under the ARCADE neon, red stools by it and the runner
leading to it; its brightness is the panel's (median 25 to 27 against 27). It is sparser and calmer than the panel
(colour 12 to 21 % against 31, accent 2 to 3 % against 12): the panel's cabinets carry full-colour side art and
action on their screens, ours plain paints and a cover; its far wall is a wall of screens and signs, ours one neon.
The slim cabinets (0.4 m, to fit the template's sockets) read as uprights from the front and the doorway; side-on they
are shallow. Beside panel 6: the counters are the panel's low glass counters, lit inside, generic handhelds on stands
behind the glass of their bases, and the stations stand along the walls; the brightness is close (45 against 55). The
room stays far calmer (calm 98 to 99 % against 86): the panel's colour is its shelves of coloured boxes, its posters
and its brand neon, and from its doorway our counters show their ends, their handhelds facing the aisle.

## Deviations and follow-ups

- **Capacity.** The arcade holds 53 titles at most (16 m); beyond, unplaced for capacity (ADR-0014 §6 amended). The
  real library's 36 fit. More arcade rooms or a back room are in the BACKLOG.
- **Slim cabinets** (0.4 m deep) so that a pair fits the template's sockets with no new template version; a deeper
  arcade template with a stool before each cabinet is in the BACKLOG. Stools stand in the hero corner only.
- **Marquees** show generic words, not the game's title or art (no per-item marquee layer; BACKLOG). The screens show
  the cover; the hero cabinet alone plays a clip (one decoder, ADR-0004; I5).
- **The run report's re-placement** of a room migrated in the same update named the z of its old depth (the real
  arcade room: -12150 reported, -14150 stored): fixed by the review, the report names where the plan stores it.
- **Handheld colour** stays below panel 6 (BACKLOG); the counters' handhelds face the aisle, unseen from the doorway.

## Review

The step's review found two broken behaviours, three smaller faults and a look short of panels 5 and 6. Fixed:

- **A floating CRT.** A pack leaving a hero part out (the arcade's TV cabinet) left it out whatever became of the CRT:
  a cabinet without a screen surface, one that does not resolve or one the footprint guard refuses drew the plan's
  CRT in the air. A part is now left out only while its socket's CRT is drawn as the pack's cabinet, and the reader
  refuses a pack that leaves a part out without pieces for the CRT (`test_era_furniture.cpp`: a cabinet with slots, a
  manifest of another kind, a cabinet moved out of the corner each draw the TV cabinet under the CRT).
- **A stale re-placement record.** The update recorded placeOnHub()'s placement before the migration grew the room;
  each record now names the placement the plan stores, and the tests (the real library's included) compare whole
  placements again. buildFloor()'s preview of a stored plan draws the stored room at the z of its stored depth, where
  it stands until the update: nothing to fix there.
- **The migration notice** now names both rearrangements (cases standing with their spine out, the arcade's games on
  its cabinets' screens), the log line what each room became.
- **A counter's strip** under its base's top is drawn only where the pack lights the case (`boardLight`).
- **Tests:** an arcade of 60 titles (the 53 first by priority placed, the 7 last unplaced for capacity naming the room,
  the same twice); a family facing front without every fixture refused. The capacity text above said 38 where the
  real library has 36: corrected.

The look, everything generic (invented words, procedural patterns, no brand or real title, no console's outline):

- The handhelds' counters showed a cover stretched 16:1 across an empty level: a pack may now ask for a riser card per
  slot at the item's shape (`roomDressing.cardPerSlot`, the handheld archive's; a counter always), so both handheld
  rooms show rows of small cards. Other packs keep one card per span (G02 to G12 unchanged; BACKLOG).
- The hero cabinet's hood printed a jagged shadow across its bezel: the hero cabinet casts no shadow (the ambient
  occlusion grounds it).
- The marquees' sparkles read as dead pixels: chaser bulbs along their bands instead, every other one lit.
- The showcases' three boxes were one picture: each box takes a cover of its own (`coverPerBox`).
- Side art (`side_art` pattern: stripes, chevrons or a starburst in the arcade's colours, by seed) on the outer sides of
  every pair and of the hero cabinet, a dim glow as under black light.
- The arcade's upper walls and ceiling: a magenta neon cove along the west wall and a cyan one along the east at 2.6 m,
  each washing its wall up to the ceiling and below; backlit flyers over the rows and showcases at 2.2 m; a can over
  each key and washer spot; the aisle's pairs lit cyan and red in turn (a pack accent, 3 of the 4 free lights: 15 of
  16). Saturated tints (washers `#50D0FF`) — the pendant keeps its violet and the key turns a little lighter, and the
  flyers glow dimmer, so that the arcade seen from the hub keeps the light probe of I6 step 1 (`roomSeen` 21.2 against
  21.0 required: a thin margin, the coves' glow counting in the probe's cut view too; BACKLOG).
- The arcade's dressing: a 3.5 m runner from the hero cabinet out into the room (the hero corner's rug left out), two
  light columns (a pink tube) in the decoration's floor lamps' place, their light the lamps'.
- The handhelds: counters and stations lined in ivory under a brighter glass (alpha 0.13), every handheld's display a
  lit pale green, the stations' strip brighter, a warm cove washing the upper walls, posters over the shelves and
  showcases at 2.0 to 2.4 m, a generic handheld pictogram in the neon beside the sign.
- Parked: taller cabinets (item 9: they change the arcade room's plan, its fixtures' heights, for a look), brushed
  metal on the counters' glass edges, a per-part shadow flag (BACKLOG).

Measured as above (768 x 432, `build/i6/step6-fix`, Release, the building fixture; before: the step's table):

| image | median luma | calm / colour / accent % | luma < 12 % |
|---|---|---|---|
| concept panel 5 | 27.4 | 57.1 / 30.7 / 12.1 | 27.1 |
| arcade_doorway (G15) before | 25.0 | 85.8 / 12.5 / 1.7 | 10.8 |
| arcade_doorway (G15) after | 30.3 | 74.6 / 18.6 / 6.8 | 4.4 |
| arcade_inside before / after | 27.0 / 27.7 | 75.3 / 21.4 / 3.3 → 79.0 / 17.4 / 3.6 | 8.6 / 8.1 |
| arcade_west_row before / after | 18.0 / 18.6 | 87.0 / 8.1 / 4.9 → 74.3 / 19.5 / 6.2 | 11.7 / 13.7 |
| arcade_aisle before / after | 19.3 / 28.8 | 83.9 / 9.3 / 6.8 → 61.3 / 24.8 / 13.9 | 5.9 / 5.5 |
| arcade_back before / after | 17.7 / 19.0 | 95.0 / 4.5 / 0.4 → 88.7 / 8.2 / 3.1 | 10.3 / 9.8 |
| concept panel 6 | 54.8 | 85.6 / 8.8 / 5.6 | 6.9 |
| handheld_gb (G16) before / after | 45.3 / 52.0 | 99.0 / 0.4 / 0.6 → 96.5 / 0.5 / 3.0 | 0.9 / 0.9 |
| handheld_shared (G13) before / after | 42.7 / 49.4 | 98.2 / 0.6 / 1.1 → 96.7 / 0.7 / 2.6 | 0.9 / 0.7 |
| handheld_gb_case before / after | 45.6 / 52.7 | 98.3 / 0.4 / 1.2 → 88.0 / 0.5 / 11.5 | 3.6 / 2.5 |
| handheld_gb_station before / after | 24.7 / 25.7 | 100.0 / 0.0 / 0.0 → 100.0 / 0.0 / 0.0 | 14.9 / 12.0 |

Beside panel 5 the arcade now has its lit upper walls (magenta and cyan coves, flyers over the rows), side art on
every pair and spots under its ceiling: accent from 1.7 to 6.8 % (panel 12.1), colour from 12.5 to 18.6 % (panel
30.7: its screens show full-colour action and its walls are covered in art; our floor and wainscot stay dark wood),
its median 30 (panel 27). Beside panel 6 the handheld rooms reach its brightness (52 against 55) with an accent of 3 %
(5.6): ivory counters of lit handhelds, a warm cove, posters high on the walls, the pictogram by the neon; they stay
calmer (its colour is its shelves of coloured boxes). The station view looks into the room's darkest corner.

Goldens: G13, G15 and G16 regenerated (Debug, within the tolerance in Release); G11 too, on purpose: the hub's view
from the elevator sees the arcade through its doorway (its cyan cove, a flyer, a light column; 1.07 % of the pixels).
G01 to G10, G12 and G14 unchanged. The floors' scene digests rewritten for the ground and handheld floors only; no
plan golden changed (the room dressing is a look: nothing collides, nothing moves).

The real library (`./scripts/real-library.ps1 -Filter '[real-library][building]'`, ES-DE closed, `RV_LEGACY_CLI` of
generator 1.3; 0 differences among 542,120 entries): all passed; the arcade room migrated and re-placed in one update,
reported where it is stored (x 9000, z -14150; it was reported at -12150), its 36 titles in cabinets (6 featured, 30
in the rows), grown one step; planned twice to one digest and SHA-256 (19,985 placements); the lobby shown 2.74 s
after the process started (NFR-01: 5 s); the busiest floor (computer) 471.8 MiB cold and warm, unchanged (512
acceptable), the ground floor 239.4 MiB (212.2 before: the arcade's flyers, side art and coves); the warm ride's view
back in 1.52 s (3 s), the cold one timed out at 2.5 s with its covers pending as before (the room whole at 13.4 s,
30 s). The benchmark path (Release, 1920 x 1080, `--no-vsync`, 1,230 frames): p50 3.31 ms, p95 3.58 ms, p99 3.86 ms,
GPU memory peak 265.5 MiB (before: 3.30, 3.55, 3.77, 265.5).
Tests after the review: Debug 1,942 unit tests, all passed; `./scripts/test.ps1 -Preset msvc-x64-release-gpu` 2,059
(1,942 unit, 117 GPU), all passed but the overlay's pick (skipped as always).
